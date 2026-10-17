/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include "wf_channels.h"

#include "wf_rail.h"
#include "wf_cliprdr.h"
#include "wf_event.h"
#include "wf_frames.h"

#include <freerdp/gdi/gfx.h>
#include <freerdp/gdi/video.h>

#include <freerdp/log.h>
#define TAG CLIENT_TAG("windows")

/* The server takes monitor layouts once it has sent its capabilities: from then on, the window size
 * is sent if it already differs from the remote desktop (window resized while connecting). */
static UINT wf_disp_caps(DispClientContext* disp, UINT32 maxNumMonitors,
                         WINPR_ATTR_UNUSED UINT32 maxMonitorAreaFactorA,
                         WINPR_ATTR_UNUSED UINT32 maxMonitorAreaFactorB)
{
	WINPR_ASSERT(disp);
	wfContext* wfc = (wfContext*)disp->custom;
	WINPR_ASSERT(wfc);

	WLog_DBG(TAG, "display control ready: %" PRIu32 " monitor(s) at most", maxNumMonitors);
	wfc->disp = disp;
	if (wfc->hwnd &&
	    freerdp_settings_get_bool(wfc->common.context.settings, FreeRDP_DynamicResolutionUpdate))
		PostMessage(wfc->hwnd, WM_TIMER, WF_TIMER_DYNAMIC_RESOLUTION, 0);
	return CHANNEL_RC_OK;
}

/* Copy within a surface shown 1:1 on the desktop: a copy on the desktop once the surface is
 * output, passed on to the frame export. */
static UINT wf_gfx_surface_to_surface(RdpgfxClientContext* gfx,
                                      const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu)
{
	WINPR_ASSERT(gfx);
	rdpGdi* gdi = (rdpGdi*)gfx->custom;
	wfContext* wfc = gdi ? (wfContext*)gdi->context : nullptr;

	if (!wfc || !wfc->surfaceToSurface)
		return ERROR_INTERNAL_ERROR;

	const UINT rc = wfc->surfaceToSurface(gfx, pdu);

	if ((rc != CHANNEL_RC_OK) || !wfc->frames || !pdu || !gfx->GetSurfaceData ||
	    (pdu->surfaceIdSrc != pdu->surfaceIdDest))
		return rc;

	EnterCriticalSection(&gfx->mux);
	const gdiGfxSurface* surface =
	    (const gdiGfxSurface*)gfx->GetSurfaceData(gfx, pdu->surfaceIdSrc);

	if (surface && surface->outputMapped && (surface->outputTargetWidth == surface->mappedWidth) &&
	    (surface->outputTargetHeight == surface->mappedHeight))
	{
		const RECTANGLE_16* source = &pdu->rectSrc;
		const INT32 x = (INT32)surface->outputOriginX;
		const INT32 y = (INT32)surface->outputOriginY;

		for (UINT16 i = 0; i < pdu->destPtsCount; i++)
			wf_frames_copy(wfc->frames, x + source->left, y + source->top,
			               source->right - source->left, source->bottom - source->top,
			               x + pdu->destPts[i].x, y + pdu->destPts[i].y);
	}

	LeaveCriticalSection(&gfx->mux);
	return rc;
}

void wf_OnChannelConnectedEventHandler(void* context, const ChannelConnectedEventArgs* e)
{
	wfContext* wfc = (wfContext*)context;
	rdpSettings* settings;

	WINPR_ASSERT(wfc);
	WINPR_ASSERT(e);

	settings = wfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
		wf_rail_init(wfc, (RailClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		wf_cliprdr_init(wfc, (CliprdrClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		DispClientContext* disp = (DispClientContext*)e->pInterface;
		WINPR_ASSERT(disp);
		disp->custom = wfc;
		disp->DisplayControlCaps = wf_disp_caps;
	}
	else
	{
		freerdp_client_OnChannelConnectedEventHandler(context, e);

		if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
		{
			RdpgfxClientContext* gfx = (RdpgfxClientContext*)e->pInterface;

			if (gfx && gfx->SurfaceToSurface &&
			    (gfx->SurfaceToSurface != wf_gfx_surface_to_surface))
			{
				wfc->surfaceToSurface = gfx->SurfaceToSurface;
				gfx->SurfaceToSurface = wf_gfx_surface_to_surface;
			}
		}
	}
}

void wf_OnChannelDisconnectedEventHandler(void* context, const ChannelDisconnectedEventArgs* e)
{
	wfContext* wfc = (wfContext*)context;
	rdpSettings* settings;

	WINPR_ASSERT(wfc);
	WINPR_ASSERT(e);

	settings = wfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
		wf_rail_uninit(wfc, (RailClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		wf_cliprdr_uninit(wfc, (CliprdrClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		wfc->disp = nullptr;
	}
	else
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
}
