/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Client
 *
 * Copyright 2009-2011 Jay Sorg
 * Copyright 2010-2011 Vic Lee
 * Copyright 2010-2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include <stdio.h>

#include <winpr/windows.h>

#include <winpr/crt.h>
#include <winpr/wlog.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>

#include <freerdp/client/file.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>

#include "../resource/resource.h"

#include <wf_client.h>
#include <wf_defaults.h>
#include <wf_zillaterm.h>

#include <shellapi.h>

INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	int status;
	HANDLE thread;
	wfContext* wfc;
	DWORD dwExitCode;
	rdpContext* context;
	rdpSettings* settings;
	LPWSTR cmd;
	char** argv = nullptr;
	RDP_CLIENT_ENTRY_POINTS clientEntryPoints = WINPR_C_ARRAY_INIT;
	int ret = 1;
	int argc = 0;
	LPWSTR* args = nullptr;
	int ztArgc = 0;
	char** ztArgv = nullptr;
	char** ztSettings = nullptr;

	WINPR_UNUSED(hInstance);
	WINPR_UNUSED(hPrevInstance);
	WINPR_UNUSED(lpCmdLine);
	WINPR_UNUSED(nCmdShow);

	/* ZillaTerm build: libraries are only ever loaded from System32. */
	if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
		return -1;

	RdpClientEntry(&clientEntryPoints);
	context = freerdp_client_context_new(&clientEntryPoints);

	if (!context)
		return -1;

	cmd = GetCommandLineW();

	if (!cmd)
		goto out;

	args = CommandLineToArgvW(cmd, &argc);

	if (!args || (argc <= 0))
		goto out;

	argv = calloc((size_t)argc, sizeof(char*));

	if (!argv)
		goto out;

	for (int i = 0; i < argc; i++)
	{
		int size = WideCharToMultiByte(CP_UTF8, 0, args[i], -1, nullptr, 0, nullptr, nullptr);
		if (size <= 0)
			goto out;
		argv[i] = calloc((size_t)size, sizeof(char));

		if (!argv[i])
			goto out;

		if (WideCharToMultiByte(CP_UTF8, 0, args[i], -1, argv[i], size, nullptr, nullptr) != size)
			goto out;
	}

	settings = context->settings;
	wfc = (wfContext*)context;

	if (!settings || !wfc)
		goto out;

	/* ZillaTerm: the connection comes from stdin, never from the command line. */
	if ((argc == 2) && (strcmp(argv[1], "/zillaterm") == 0))
	{
		/* Log messages on stderr, written at once (a pipe is otherwise fully buffered): stdout
		 * carries the messages for ZillaTerm. */
		(void)setvbuf(stderr, nullptr, _IONBF, 0);
		wLog* root = WLog_GetRoot();
		if (!WLog_SetLogAppenderType(root, WLOG_APPENDER_CONSOLE))
			goto out;
		wLogAppender* appender = WLog_GetLogAppender(root);
		if (!appender || !WLog_ConfigureAppender(appender, "outputstream", (void*)"stderr"))
			goto out;

		if (!wf_zt_read_arguments(argv[0], &ztArgc, &ztArgv, &ztSettings))
			goto out;

		status =
		    freerdp_client_settings_parse_command_line(settings, ztArgc, ztArgv, FALSE);
		if (status)
		{
			ret = freerdp_client_settings_command_line_status_print(settings, status, ztArgc,
			                                                       ztArgv);
			goto out;
		}

		if (!wf_zt_apply_settings(settings, ztSettings))
			goto out;

		wf_zt_free_list(ztArgv);
		ztArgv = nullptr;
		wf_zt_free_list(ztSettings);
		ztSettings = nullptr;
		if (!wf_zt_start(context))
			goto out;
	}
	else
	{
		freerdp_client_warn_deprecated(argc, argv);
		status = freerdp_client_settings_parse_command_line(settings, argc, argv, FALSE);
		if (status)
		{
			ret = freerdp_client_settings_command_line_status_print(settings, status, argc, argv);
			goto out;
		}
	}

	/* ZillaTerm build: saved Windows credentials (TERMSRV/<host>) are never read. */

	if (freerdp_client_start(context) != 0)
		goto out;

	thread = freerdp_client_get_thread(context);

	if (thread)
	{
		if (WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0)
		{
			GetExitCodeThread(thread, &dwExitCode);
			ret = (int)dwExitCode;
			wf_zt_event("ended %lu %s", (unsigned long)dwExitCode,
			            freerdp_get_last_error_name((UINT32)dwExitCode));
		}
	}

	if (freerdp_client_stop(context) != 0)
		goto out;

out:
	wf_zt_free_list(ztArgv);
	wf_zt_free_list(ztSettings);
	freerdp_client_context_free(context);

	if (argv)
	{
		for (int i = 0; i < argc; i++)
			free(argv[i]);

		free(argv);
	}

	LocalFree(args);
	return ret;
}

#ifdef WITH_WIN_CONSOLE
int main()
{
	return WinMain(nullptr, nullptr, nullptr, 0);
}
#endif
