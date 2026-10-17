/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Client: changed regions of the session written to a pipe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>

#include <freerdp/log.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>

#include "wf_frames.h"

#define TAG CLIENT_TAG("windows.frames")

/* Beyond this many rectangles in a message, their bounding rectangle is sent instead. */
#define WF_FRAMES_MAX_RECTS 256

/* Copies kept at most between two messages: beyond, they are all dropped (their destinations are
 * invalidated anyway, the pixels are sent instead). */
#define WF_FRAMES_MAX_COPIES 64

/* Side of the cells compared with the reader's desktop. */
#define WF_FRAMES_CELL 32

/* Bytes per WriteFile call. */
#define WF_FRAMES_WRITE_CHUNK (1u << 20)

typedef struct
{
	INT32 sx;
	INT32 sy;
	INT32 width;
	INT32 height;
	INT32 dx;
	INT32 dy;
	BOOL shown;
} wfFrameCopy;

struct wf_frames
{
	HANDLE pipe;
	UINT32 interval;
	HANDLE thread;
	HANDLE changed;
	HANDLE stop;
	volatile LONG failed;

	/* Held by the drawing thread between wf_frames_begin and wf_frames_end, and by the writer
	 * while it copies the changed regions: it never reads a half drawn or reallocated desktop. */
	CRITICAL_SECTION lock;
	BOOL owned;

	/* Under the lock. */
	rdpGdi* gdi;
	REGION16 damage;
	BOOL whole;

	/* Copies, under their own lock: graphics pipeline copies come from the channel thread, which
	 * holds the channel lock and must not wait for the drawing thread. Shown once a paint ends. */
	CRITICAL_SECTION copiesLock;
	wfFrameCopy copies[WF_FRAMES_MAX_COPIES];
	UINT32 ncopies;
	BOOL copiesLost;

	/* Writer thread only: the desktop as the reader has it (BGRX32, 4 * width bytes per row), and
	 * the message being built. */
	BYTE* shadow;
	UINT32 shadowWidth;
	UINT32 shadowHeight;
	BYTE* cells;
	size_t cellsCapacity;
	RECTANGLE_16* rects;
	BYTE* buffer;
	size_t capacity;
	UINT64 messages;
	UINT64 copiesSent;
};

static BYTE* wf_frames_put32(BYTE* p, UINT32 value)
{
	p[0] = (BYTE)(value & 0xFF);
	p[1] = (BYTE)((value >> 8) & 0xFF);
	p[2] = (BYTE)((value >> 16) & 0xFF);
	p[3] = (BYTE)((value >> 24) & 0xFF);
	return p + 4;
}

static BYTE* wf_frames_put16(BYTE* p, UINT16 value)
{
	p[0] = (BYTE)(value & 0xFF);
	p[1] = (BYTE)((value >> 8) & 0xFF);
	return p + 2;
}

static BOOL wf_frames_write(wfFrames* frames, const BYTE* data, size_t length)
{
	while (length > 0)
	{
		DWORD written = 0;
		const DWORD chunk =
		    (DWORD)((length > WF_FRAMES_WRITE_CHUNK) ? WF_FRAMES_WRITE_CHUNK : length);

		if (!WriteFile(frames->pipe, data, chunk, &written, nullptr) || (written == 0))
			return FALSE;

		data += written;
		length -= written;
	}

	return TRUE;
}

/* A copy kept within the desktop (source and destination); FALSE if nothing is left of it. */
static BOOL wf_frames_clip(wfFrameCopy* c, INT32 width, INT32 height)
{
	if (c->sx < 0)
	{
		c->width += c->sx;
		c->dx -= c->sx;
		c->sx = 0;
	}

	if (c->dx < 0)
	{
		c->width += c->dx;
		c->sx -= c->dx;
		c->dx = 0;
	}

	if (c->sy < 0)
	{
		c->height += c->sy;
		c->dy -= c->sy;
		c->sy = 0;
	}

	if (c->dy < 0)
	{
		c->height += c->dy;
		c->sy -= c->dy;
		c->dy = 0;
	}

	if (c->sx + c->width > width)
		c->width = width - c->sx;

	if (c->dx + c->width > width)
		c->width = width - c->dx;

	if (c->sy + c->height > height)
		c->height = height - c->sy;

	if (c->dy + c->height > height)
		c->height = height - c->dy;

	return (c->width > 0) && (c->height > 0) && ((c->sx != c->dx) || (c->sy != c->dy));
}

/* A copy applied to the reader's desktop, as the reader does: source read before destination. */
static void wf_frames_apply(wfFrames* frames, const wfFrameCopy* c)
{
	const size_t stride = 4ull * frames->shadowWidth;
	const size_t bytes = 4ull * (size_t)c->width;

	if (c->dy > c->sy)
	{
		for (INT32 row = c->height - 1; row >= 0; row--)
			memmove(frames->shadow + (size_t)(c->dy + row) * stride + 4ull * (size_t)c->dx,
			        frames->shadow + (size_t)(c->sy + row) * stride + 4ull * (size_t)c->sx, bytes);
	}
	else
	{
		for (INT32 row = 0; row < c->height; row++)
			memmove(frames->shadow + (size_t)(c->dy + row) * stride + 4ull * (size_t)c->dx,
			        frames->shadow + (size_t)(c->sy + row) * stride + 4ull * (size_t)c->sx, bytes);
	}
}

/* Pixels of the primary surface into the reader's desktop. */
static BOOL wf_frames_refresh(wfFrames* frames, rdpGdi* gdi, UINT32 x, UINT32 y, UINT32 width,
                              UINT32 height)
{
	return freerdp_image_copy_no_overlap(
	    frames->shadow, PIXEL_FORMAT_BGRX32, 4 * frames->shadowWidth, x, y, width, height,
	    gdi->primary_buffer, gdi->dstFormat, gdi->stride, x, y, &gdi->palette, FREERDP_FLIP_NONE);
}

/* Cells of the region that differ from the reader's desktop, brought up to date there, as
 * rectangles (runs of cells, merged with the same run of the row above). */
static BOOL wf_frames_compare(wfFrames* frames, rdpGdi* gdi, const REGION16* region, UINT32* count)
{
	const UINT32 width = frames->shadowWidth;
	const UINT32 height = frames->shadowHeight;
	const UINT32 columns = (width + WF_FRAMES_CELL - 1) / WF_FRAMES_CELL;
	const UINT32 rows = (height + WF_FRAMES_CELL - 1) / WF_FRAMES_CELL;
	const size_t stride = 4ull * width;
	BYTE cell[4 * WF_FRAMES_CELL * WF_FRAMES_CELL];

	*count = 0;

	if (frames->cellsCapacity < (size_t)columns * rows)
	{
		BYTE* cells = realloc(frames->cells, (size_t)columns * rows);

		if (!cells)
			return FALSE;

		frames->cells = cells;
		frames->cellsCapacity = (size_t)columns * rows;
	}

	memset(frames->cells, 0, (size_t)columns * rows);

	UINT32 nrects = 0;
	const RECTANGLE_16* rects = region16_rects(region, &nrects);

	for (UINT32 i = 0; i < nrects; i++)
	{
		for (UINT32 cy = rects[i].top / WF_FRAMES_CELL;
		     cy <= (rects[i].bottom - 1u) / WF_FRAMES_CELL && cy < rows; cy++)
		{
			for (UINT32 cx = rects[i].left / WF_FRAMES_CELL;
			     cx <= (rects[i].right - 1u) / WF_FRAMES_CELL && cx < columns; cx++)
				frames->cells[(size_t)cy * columns + cx] = 1;
		}
	}

	for (UINT32 cy = 0; cy < rows; cy++)
	{
		for (UINT32 cx = 0; cx < columns; cx++)
		{
			BYTE* marked = &frames->cells[(size_t)cy * columns + cx];

			if (!*marked)
				continue;

			const UINT32 x = cx * WF_FRAMES_CELL;
			const UINT32 y = cy * WF_FRAMES_CELL;
			const UINT32 w = MIN(WF_FRAMES_CELL, width - x);
			const UINT32 h = MIN(WF_FRAMES_CELL, height - y);

			if (!freerdp_image_copy_no_overlap(cell, PIXEL_FORMAT_BGRX32, 4 * WF_FRAMES_CELL, 0, 0,
			                                   w, h, gdi->primary_buffer, gdi->dstFormat,
			                                   gdi->stride, x, y, &gdi->palette, FREERDP_FLIP_NONE))
				return FALSE;

			BOOL same = TRUE;

			for (UINT32 r = 0; r < h; r++)
			{
				BYTE* target = frames->shadow + (size_t)(y + r) * stride + 4ull * x;
				const BYTE* source = cell + 4ull * WF_FRAMES_CELL * r;

				if (memcmp(target, source, 4ull * w) != 0)
				{
					memcpy(target, source, 4ull * w);
					same = FALSE;
				}
			}

			*marked = same ? 0 : 1;
		}
	}

	UINT32 n = 0;
	BOOL many = FALSE;
	RECTANGLE_16 extents = { UINT16_MAX, UINT16_MAX, 0, 0 };

	for (UINT32 cy = 0; cy < rows; cy++)
	{
		for (UINT32 cx = 0; cx < columns;)
		{
			if (!frames->cells[(size_t)cy * columns + cx])
			{
				cx++;
				continue;
			}

			UINT32 end = cx;

			while (end < columns && frames->cells[(size_t)cy * columns + end])
				end++;

			const RECTANGLE_16 run = { (UINT16)(cx * WF_FRAMES_CELL), (UINT16)(cy * WF_FRAMES_CELL),
				                       (UINT16)MIN(end * WF_FRAMES_CELL, width),
				                       (UINT16)MIN((cy + 1) * WF_FRAMES_CELL, height) };
			extents.left = MIN(extents.left, run.left);
			extents.top = MIN(extents.top, run.top);
			extents.right = MAX(extents.right, run.right);
			extents.bottom = MAX(extents.bottom, run.bottom);
			cx = end;

			if (many)
				continue;

			BOOL merged = FALSE;

			for (UINT32 i = 0; i < n; i++)
			{
				RECTANGLE_16* r = &frames->rects[i];

				if ((r->bottom == run.top) && (r->left == run.left) && (r->right == run.right))
				{
					r->bottom = run.bottom;
					merged = TRUE;
					break;
				}
			}

			if (merged)
				continue;

			if (n == WF_FRAMES_MAX_RECTS)
				many = TRUE;
			else
				frames->rects[n++] = run;
		}
	}

	if (many)
	{
		/* Too many rectangles: their bounding rectangle, brought up to date in full. */
		if (!wf_frames_refresh(frames, gdi, extents.left, extents.top,
		                       (UINT32)(extents.right - extents.left),
		                       (UINT32)(extents.bottom - extents.top)))
			return FALSE;

		frames->rects[0] = extents;
		n = 1;
	}

	*count = n;
	return TRUE;
}

/* Under the lock: the next message in frames->buffer, damage and copies cleared. *length stays 0
 * when the reader's desktop is already up to date. */
static BOOL wf_frames_take(wfFrames* frames, size_t* length)
{
	BOOL rc = FALSE;
	REGION16 region = WINPR_C_ARRAY_INIT;
	rdpGdi* gdi = frames->gdi;
	wfFrameCopy copies[WF_FRAMES_MAX_COPIES];
	UINT32 ncopies = 0;
	UINT32 count = 0;

	*length = 0;

	/* Copies shown since the previous message, in order; those still waiting for their paint
	 * stay. */
	EnterCriticalSection(&frames->copiesLock);
	if (frames->copiesLost)
	{
		frames->ncopies = 0;
		frames->copiesLost = FALSE;
	}

	UINT32 kept = 0;

	for (UINT32 i = 0; i < frames->ncopies; i++)
	{
		if (frames->copies[i].shown)
			copies[ncopies++] = frames->copies[i];
		else
			frames->copies[kept++] = frames->copies[i];
	}

	frames->ncopies = kept;
	LeaveCriticalSection(&frames->copiesLock);

	if (!gdi || !gdi->primary_buffer || (gdi->width <= 0) || (gdi->height <= 0) ||
	    (gdi->width > UINT16_MAX) || (gdi->height > UINT16_MAX))
		return TRUE;

	const UINT32 width = (UINT32)gdi->width;
	const UINT32 height = (UINT32)gdi->height;
	const RECTANGLE_16 desktop = { 0, 0, (UINT16)width, (UINT16)height };

	region16_init(&region);

	if (!region16_intersect_rect(&region, &frames->damage, &desktop))
		goto out;

	region16_clear(&frames->damage);

	const BOOL whole = frames->whole || (frames->shadowWidth != width) ||
	                   (frames->shadowHeight != height) || !frames->shadow;

	if (!frames->rects)
	{
		frames->rects = calloc(WF_FRAMES_MAX_RECTS, sizeof(RECTANGLE_16));

		if (!frames->rects)
			goto out;
	}

	if (whole)
	{
		/* New desktop: sent in full, and kept as the reader has it from now on. */
		frames->whole = FALSE;
		ncopies = 0;

		if ((frames->shadowWidth != width) || (frames->shadowHeight != height) || !frames->shadow)
		{
			free(frames->shadow);
			frames->shadow = calloc((size_t)width * height, 4);
			frames->shadowWidth = frames->shadow ? width : 0;
			frames->shadowHeight = frames->shadow ? height : 0;

			if (!frames->shadow)
				WLog_WARN(TAG, "no copy of the desktop: changed regions sent as drawn");
		}

		if (frames->shadow && !wf_frames_refresh(frames, gdi, 0, 0, width, height))
			goto out;

		frames->rects[0] = desktop;
		count = 1;
	}
	else if (frames->shadow)
	{
		for (UINT32 i = 0; i < ncopies;)
		{
			if (!wf_frames_clip(&copies[i], (INT32)width, (INT32)height))
			{
				copies[i] = copies[--ncopies];
				continue;
			}

			const RECTANGLE_16 target = { (UINT16)copies[i].dx, (UINT16)copies[i].dy,
				                          (UINT16)(copies[i].dx + copies[i].width),
				                          (UINT16)(copies[i].dy + copies[i].height) };
			wf_frames_apply(frames, &copies[i]);

			if (!region16_union_rect(&region, &region, &target))
				goto out;

			i++;
		}

		if (region16_is_empty(&region) && (ncopies == 0))
		{
			rc = TRUE;
			goto out;
		}

		if (!wf_frames_compare(frames, gdi, &region, &count))
			goto out;

		if ((count == 0) && (ncopies == 0))
		{
			rc = TRUE;
			goto out;
		}
	}
	else
	{
		/* No copy of the desktop to compare with: what was drawn, without the copies. */
		ncopies = 0;
		const RECTANGLE_16* rects = region16_rects(&region, &count);

		if (count > WF_FRAMES_MAX_RECTS)
		{
			rects = region16_extents(&region);
			count = 1;
		}

		for (UINT32 i = 0; i < count; i++)
			frames->rects[i] = rects[i];

		if (count == 0)
		{
			rc = TRUE;
			goto out;
		}
	}

	size_t size = 20 + 12ull * ncopies + 8ull * count;

	for (UINT32 i = 0; i < count; i++)
	{
		const RECTANGLE_16* r = &frames->rects[i];
		size += 4ull * (r->right - r->left) * (r->bottom - r->top);
	}

	if (size - 4 > UINT32_MAX)
		goto out;

	if (size > frames->capacity)
	{
		BYTE* buffer = realloc(frames->buffer, size);

		if (!buffer)
			goto out;

		frames->buffer = buffer;
		frames->capacity = size;
	}

	BYTE* p = frames->buffer;
	p = wf_frames_put32(p, (UINT32)(size - 4));
	p = wf_frames_put32(p, width);
	p = wf_frames_put32(p, height);
	p = wf_frames_put32(p, ncopies);
	p = wf_frames_put32(p, count);

	for (UINT32 i = 0; i < ncopies; i++)
	{
		p = wf_frames_put16(p, (UINT16)copies[i].sx);
		p = wf_frames_put16(p, (UINT16)copies[i].sy);
		p = wf_frames_put16(p, (UINT16)copies[i].width);
		p = wf_frames_put16(p, (UINT16)copies[i].height);
		p = wf_frames_put16(p, (UINT16)copies[i].dx);
		p = wf_frames_put16(p, (UINT16)copies[i].dy);
	}

	for (UINT32 i = 0; i < count; i++)
	{
		const RECTANGLE_16* r = &frames->rects[i];
		p = wf_frames_put16(p, r->left);
		p = wf_frames_put16(p, r->top);
		p = wf_frames_put16(p, (UINT16)(r->right - r->left));
		p = wf_frames_put16(p, (UINT16)(r->bottom - r->top));
	}

	for (UINT32 i = 0; i < count; i++)
	{
		const RECTANGLE_16* r = &frames->rects[i];
		const UINT32 w = r->right - r->left;
		const UINT32 h = r->bottom - r->top;

		if (frames->shadow)
		{
			for (UINT32 row = 0; row < h; row++)
				memcpy(p + 4ull * w * row,
				       frames->shadow + 4ull * ((size_t)(r->top + row) * width + r->left),
				       4ull * w);
		}
		else if (!freerdp_image_copy_no_overlap(p, PIXEL_FORMAT_BGRX32, w * 4, 0, 0, w, h,
		                                        gdi->primary_buffer, gdi->dstFormat, gdi->stride,
		                                        r->left, r->top, &gdi->palette, FREERDP_FLIP_NONE))
			goto out;

		p += 4ull * w * h;
	}

	frames->messages++;
	frames->copiesSent += ncopies;
	*length = size;
	rc = TRUE;
out:
	region16_uninit(&region);
	return rc;
}

static DWORD WINAPI wf_frames_thread(LPVOID arg)
{
	wfFrames* frames = (wfFrames*)arg;
	WINPR_ASSERT(frames);

	static const BYTE magic[4] = { 'W', 'F', 'R', '2' };
	HANDLE events[] = { frames->stop, frames->changed };
	UINT64 last = 0;

	if (!wf_frames_write(frames, magic, sizeof(magic)))
		goto out;

	for (;;)
	{
		if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
			break;

		/* At most one message per interval: the updates in between are merged. */
		const UINT64 now = GetTickCount64();

		if ((last != 0) && (now - last < frames->interval))
		{
			const DWORD wait = (DWORD)(frames->interval - (now - last));

			if (WaitForSingleObject(frames->stop, wait) != WAIT_TIMEOUT)
				break;
		}

		size_t length = 0;
		EnterCriticalSection(&frames->lock);
		const BOOL taken = wf_frames_take(frames, &length);
		LeaveCriticalSection(&frames->lock);

		if (!taken)
		{
			WLog_ERR(TAG, "frame export stopped: changed regions could not be copied");
			break;
		}

		last = GetTickCount64();

		if ((length > 0) && !wf_frames_write(frames, frames->buffer, length))
		{
			WLog_INFO(TAG, "frame export stopped: pipe closed [%" PRIu32 "]", GetLastError());
			break;
		}
	}

out:
	WLog_INFO(TAG, "frame export: %" PRIu64 " message(s), %" PRIu64 " copies", frames->messages,
	          frames->copiesSent);
	InterlockedExchange(&frames->failed, 1);
	return 0;
}

wfFrames* wf_frames_new(HANDLE pipe, UINT32 interval)
{
	wfFrames* frames = calloc(1, sizeof(wfFrames));

	if (!frames)
		return nullptr;

	frames->pipe = pipe;
	frames->interval = interval;
	region16_init(&frames->damage);

	if (!InitializeCriticalSectionAndSpinCount(&frames->lock, 4000))
	{
		region16_uninit(&frames->damage);
		free(frames);
		return nullptr;
	}

	if (!InitializeCriticalSectionAndSpinCount(&frames->copiesLock, 4000))
	{
		DeleteCriticalSection(&frames->lock);
		region16_uninit(&frames->damage);
		free(frames);
		return nullptr;
	}

	frames->changed = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	frames->stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	if (!frames->changed || !frames->stop)
		goto fail;

	frames->thread = CreateThread(nullptr, 0, wf_frames_thread, frames, 0, nullptr);

	if (!frames->thread)
		goto fail;

	WLog_INFO(TAG, "frame export to pipe, at most one message every %" PRIu32 " ms", interval);
	return frames;

fail:
	wf_frames_free(frames);
	return nullptr;
}

void wf_frames_free(wfFrames* frames)
{
	if (!frames)
		return;

	if (frames->thread)
	{
		(void)SetEvent(frames->stop);

		/* A write blocked by a reader that no longer reads: cancelled, again if it started after
		 * the previous attempt. */
		while (WaitForSingleObject(frames->thread, 100) == WAIT_TIMEOUT)
			(void)CancelSynchronousIo(frames->thread);

		(void)CloseHandle(frames->thread);
	}

	if (frames->changed)
		(void)CloseHandle(frames->changed);

	if (frames->stop)
		(void)CloseHandle(frames->stop);

	if (frames->pipe && (frames->pipe != INVALID_HANDLE_VALUE))
		(void)CloseHandle(frames->pipe);

	DeleteCriticalSection(&frames->copiesLock);
	DeleteCriticalSection(&frames->lock);
	region16_uninit(&frames->damage);
	free(frames->shadow);
	free(frames->cells);
	free(frames->rects);
	free(frames->buffer);
	free(frames);
}

void wf_frames_begin(wfFrames* frames)
{
	if (!frames || frames->owned || InterlockedCompareExchange(&frames->failed, 0, 0))
		return;

	EnterCriticalSection(&frames->lock);
	frames->owned = TRUE;
}

void wf_frames_end(wfFrames* frames, rdpGdi* gdi, BOOL whole)
{
	BOOL changed = FALSE;

	if (!frames)
		return;

	if (InterlockedCompareExchange(&frames->failed, 0, 0))
	{
		if (frames->owned)
		{
			frames->owned = FALSE;
			LeaveCriticalSection(&frames->lock);
		}

		return;
	}

	if (!frames->owned)
		EnterCriticalSection(&frames->lock);

	frames->gdi = gdi;

	if (gdi && whole && (gdi->width > 0) && (gdi->height > 0) && (gdi->width <= UINT16_MAX) &&
	    (gdi->height <= UINT16_MAX))
	{
		/* The whole desktop goes in the next message, whatever was drawn. */
		frames->whole = TRUE;
		changed = TRUE;
	}
	else if (gdi && gdi->primary && gdi->primary->hdc && gdi->primary->hdc->hwnd)
	{
		const HGDI_WND hwnd = gdi->primary->hdc->hwnd;

		if (hwnd->invalid && !hwnd->invalid->null && hwnd->cinvalid)
		{
			for (INT32 i = 0; i < hwnd->ninvalid; i++)
			{
				const GDI_RGN* r = &hwnd->cinvalid[i];

				if ((r->w <= 0) || (r->h <= 0) || (r->x < 0) || (r->y < 0) ||
				    (r->x + r->w > UINT16_MAX) || (r->y + r->h > UINT16_MAX))
					continue;

				const RECTANGLE_16 rect = { (UINT16)r->x, (UINT16)r->y, (UINT16)(r->x + r->w),
					                        (UINT16)(r->y + r->h) };

				/* A region lost would leave the reader behind: the whole desktop goes instead. */
				if (!region16_union_rect(&frames->damage, &frames->damage, &rect))
				{
					frames->whole = TRUE;
					changed = TRUE;
				}
			}
		}
	}

	/* The copies made so far are on the desktop now. */
	EnterCriticalSection(&frames->copiesLock);
	for (UINT32 i = 0; i < frames->ncopies; i++)
	{
		changed |= !frames->copies[i].shown;
		frames->copies[i].shown = TRUE;
	}
	LeaveCriticalSection(&frames->copiesLock);

	/* Drawing done with Windows GDI (hardware GDI mode) is in the bitmap before another thread
	 * reads it. */
	(void)GdiFlush();
	changed |= !region16_is_empty(&frames->damage);
	frames->owned = FALSE;
	LeaveCriticalSection(&frames->lock);

	if (changed)
		(void)SetEvent(frames->changed);
}

void wf_frames_copy(wfFrames* frames, INT32 sx, INT32 sy, INT32 width, INT32 height, INT32 dx,
                    INT32 dy)
{
	if (!frames || (width <= 0) || (height <= 0) || ((sx == dx) && (sy == dy)) ||
	    InterlockedCompareExchange(&frames->failed, 0, 0))
		return;

	EnterCriticalSection(&frames->copiesLock);
	if (frames->ncopies < WF_FRAMES_MAX_COPIES)
	{
		const wfFrameCopy copy = { sx, sy, width, height, dx, dy, FALSE };
		frames->copies[frames->ncopies++] = copy;
	}
	else
		frames->copiesLost = TRUE;
	LeaveCriticalSection(&frames->copiesLock);
}

void wf_frames_detach(wfFrames* frames)
{
	if (!frames)
		return;

	if (!frames->owned)
		EnterCriticalSection(&frames->lock);

	frames->gdi = nullptr;
	region16_clear(&frames->damage);
	EnterCriticalSection(&frames->copiesLock);
	frames->ncopies = 0;
	frames->copiesLost = FALSE;
	LeaveCriticalSection(&frames->copiesLock);
	frames->owned = FALSE;
	LeaveCriticalSection(&frames->lock);
}
