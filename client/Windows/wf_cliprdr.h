/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Clipboard Redirection
 *
 * Copyright 2012 Jason Champion
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
#ifndef FREERDP_CLIENT_WIN_CLIPRDR_H
#define FREERDP_CLIENT_WIN_CLIPRDR_H

#include "wf_client.h"

/* Posted to the session window once the server has files dropped on it: paste them (Ctrl+V).
 * wParam: number of files, lParam: number of folders. */
#define WM_FREERDP_PASTE_DROPPED (WM_USER + 101)

BOOL wf_cliprdr_init(wfContext* wfc, CliprdrClientContext* cliprdr);
BOOL wf_cliprdr_uninit(wfContext* wfc, CliprdrClientContext* cliprdr);
void wf_cliprdr_drop_files(wfContext* wfc, HANDLE drop);

#endif /* FREERDP_CLIENT_WIN_CLIPRDR_H */
