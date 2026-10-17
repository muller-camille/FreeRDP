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

/* Bytes per WriteFile call. */
#define WF_FRAMES_WRITE_CHUNK (1u << 20)

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

	/* Writer thread only. */
	BYTE* buffer;
	size_t capacity;
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

/* Under the lock: the changed regions of the desktop as a message in frames->buffer, and the
 * damage cleared. *length stays 0 when nothing changed. */
static BOOL wf_frames_take(wfFrames* frames, size_t* length)
{
	BOOL rc = FALSE;
	REGION16 region = WINPR_C_ARRAY_INIT;
	rdpGdi* gdi = frames->gdi;

	*length = 0;

	if (!gdi || !gdi->primary_buffer || region16_is_empty(&frames->damage) || (gdi->width <= 0) ||
	    (gdi->height <= 0) || (gdi->width > UINT16_MAX) || (gdi->height > UINT16_MAX))
		return TRUE;

	region16_init(&region);
	const RECTANGLE_16 desktop = { 0, 0, (UINT16)gdi->width, (UINT16)gdi->height };

	if (!region16_intersect_rect(&region, &frames->damage, &desktop))
		goto out;

	region16_clear(&frames->damage);

	UINT32 count = 0;
	const RECTANGLE_16* rects = region16_rects(&region, &count);

	if (count == 0)
	{
		rc = TRUE;
		goto out;
	}

	if (count > WF_FRAMES_MAX_RECTS)
	{
		rects = region16_extents(&region);
		count = 1;
	}

	size_t size = 16 + 8ull * count;

	for (UINT32 i = 0; i < count; i++)
		size += 4ull * (rects[i].right - rects[i].left) * (rects[i].bottom - rects[i].top);

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
	p = wf_frames_put32(p, (UINT32)gdi->width);
	p = wf_frames_put32(p, (UINT32)gdi->height);
	p = wf_frames_put32(p, count);

	for (UINT32 i = 0; i < count; i++)
	{
		p = wf_frames_put16(p, rects[i].left);
		p = wf_frames_put16(p, rects[i].top);
		p = wf_frames_put16(p, (UINT16)(rects[i].right - rects[i].left));
		p = wf_frames_put16(p, (UINT16)(rects[i].bottom - rects[i].top));
	}

	for (UINT32 i = 0; i < count; i++)
	{
		const UINT32 width = rects[i].right - rects[i].left;
		const UINT32 height = rects[i].bottom - rects[i].top;

		if (!freerdp_image_copy_no_overlap(p, PIXEL_FORMAT_BGRX32, width * 4, 0, 0, width, height,
		                                   gdi->primary_buffer, gdi->dstFormat, gdi->stride,
		                                   rects[i].left, rects[i].top, &gdi->palette,
		                                   FREERDP_FLIP_NONE))
			goto out;

		p += 4ull * width * height;
	}

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

	static const BYTE magic[4] = { 'W', 'F', 'R', '1' };
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

	DeleteCriticalSection(&frames->lock);
	region16_uninit(&frames->damage);
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
		const RECTANGLE_16 desktop = { 0, 0, (UINT16)gdi->width, (UINT16)gdi->height };
		(void)region16_union_rect(&frames->damage, &frames->damage, &desktop);
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
				(void)region16_union_rect(&frames->damage, &frames->damage, &rect);
			}
		}
	}

	/* Drawing done with Windows GDI (hardware GDI mode) is in the bitmap before another thread
	 * reads it. */
	(void)GdiFlush();
	changed = !region16_is_empty(&frames->damage);
	frames->owned = FALSE;
	LeaveCriticalSection(&frames->lock);

	if (changed)
		(void)SetEvent(frames->changed);
}

void wf_frames_detach(wfFrames* frames)
{
	if (!frames)
		return;

	if (!frames->owned)
		EnterCriticalSection(&frames->lock);

	frames->gdi = nullptr;
	region16_clear(&frames->damage);
	frames->owned = FALSE;
	LeaveCriticalSection(&frames->lock);
}
