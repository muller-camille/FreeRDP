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

#ifndef FREERDP_CLIENT_WIN_FRAMES_H
#define FREERDP_CLIENT_WIN_FRAMES_H

#include <winpr/wtypes.h>
#include <freerdp/gdi/gdi.h>

/**
 * Changed regions of the remote desktop written to a pipe (/frame-pipe), for an application that
 * records or shares the session: it gets the desktop as decoded, at its own size, whatever the
 * window shows (scaled, hidden behind another window, full screen), only what changed, and the
 * screen to screen copies (scrolling, moved windows) the server asked for.
 *
 * The stream starts with the 4 bytes "WFR2". Each message then brings the reader's copy of the
 * desktop up to date (the whole desktop in the first one, after a resize and after a
 * reconnection):
 *
 *   UINT32 length of the rest of the message, in bytes
 *   UINT32 desktop width, UINT32 desktop height, in pixels
 *   UINT32 number of copies, UINT32 number of rectangles
 *   for each copy: UINT16 source left, source top, width, height, destination left, destination top
 *   for each rectangle: UINT16 left, top, width, height
 *   for each rectangle, in the same order: width * height pixels, row by row from the top,
 *   4 bytes each: blue, green, red, unused
 *
 * The reader applies the copies first, one after the other, each as if its source were read
 * before its destination is written (they may overlap), then writes the rectangles. Copies and
 * rectangles are within the desktop; the rectangles do not overlap. A message may hold copies and
 * no rectangle, or rectangles and no copy.
 *
 * The exporter keeps the desktop as the reader has it: only the parts that differ from it after
 * the copies are sent, however much the server redrew. Values are little endian. One message is
 * written at most every interval (milliseconds): the screen updates in between are merged.
 * Writing never holds up the session: while the reader is slow, changes accumulate and go in the
 * next message. The first write error (reader gone) stops the export, the session goes on.
 */
typedef struct wf_frames wfFrames;

wfFrames* wf_frames_new(HANDLE pipe, UINT32 interval);
void wf_frames_free(wfFrames* frames);

/* Drawing thread: the desktop is about to be drawn to (or reallocated). */
void wf_frames_begin(wfFrames* frames);

/* Drawing thread: drawing done; the invalid region of the primary surface (or the whole desktop)
 * goes in the next message. */
void wf_frames_end(wfFrames* frames, rdpGdi* gdi, BOOL whole);

/* Any thread: the primary surface got (or, for a graphics pipeline surface, will get with the next
 * paint) a copy of one of its rectangles at another place. The copy is sent once the paint that
 * shows it ends; the drawing itself must still invalidate the destination. */
void wf_frames_copy(wfFrames* frames, INT32 sx, INT32 sy, INT32 width, INT32 height, INT32 dx,
                    INT32 dy);

/* Drawing thread: the GDI is about to be freed; nothing is read from it any more. */
void wf_frames_detach(wfFrames* frames);

#endif /* FREERDP_CLIENT_WIN_FRAMES_H */
