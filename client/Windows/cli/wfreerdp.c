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

#include <winpr/windows.h>
#include <winpr/winsock.h>

#include <winpr/crt.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>
#include <freerdp/log.h>
#include <freerdp/transport_io.h>

#include <freerdp/client/file.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>

#include "../resource/resource.h"

#include <wf_client.h>
#include <wf_defaults.h>

#include <shellapi.h>

#define TAG CLIENT_TAG("windows")

/* Messages of the frame export: at most one every this many milliseconds by default. */
#define WF_FRAME_INTERVAL_DEFAULT 100

static COMMAND_LINE_ARGUMENT_A wf_args[] = {
	{ "frame-pipe", COMMAND_LINE_VALUE_REQUIRED, "<handle>[,interval:<ms>]", nullptr, nullptr, -1,
	  nullptr,
	  "Write the regions of the remote desktop that change to this pipe, a handle inherited from "
	  "the parent process: the desktop as decoded, whatever the window shows. At most one message "
	  "every interval milliseconds (default 100, from 10 to 60000)." },
	{ "connect-pipe", COMMAND_LINE_VALUE_REQUIRED, "<read-handle>,<write-handle>", nullptr, nullptr,
	  -1, nullptr,
	  "Get each connection to the server, reconnections included, from the parent process instead "
	  "of opening it, through two pipe handles inherited from it: for a server only reachable "
	  "through a tunnel the parent opens (SSH jump host). For each connection, the port (16 bits), "
	  "the length (16 bits) and the UTF-8 host name are written to <write-handle>; <read-handle> "
	  "then gives a 32-bit length and a WSAPROTOCOL_INFOW (WSADuplicateSocket) of a socket already "
	  "connected to that server, or a length of 0 when the parent did not connect. Little-endian. "
	  "/v still names the server, for its certificate." },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

/* A pipe handle of this process, in decimal (as .NET writes it) or hexadecimal with 0x. */
static BOOL wf_parse_pipe_handle(const char* value, char** end, HANDLE* pipe)
{
	const int base = ((value[0] == '0') && ((value[1] == 'x') || (value[1] == 'X'))) ? 16 : 10;
	errno = 0;
	const unsigned long long handle = strtoull(value, end, base);

	if ((errno != 0) || (*end == value) || (handle == 0) || (handle > UINTPTR_MAX))
		return FALSE;

	*pipe = (HANDLE)(uintptr_t)handle;
	return GetFileType(*pipe) == FILE_TYPE_PIPE;
}

/* /connect-pipe:<read-handle>,<write-handle> */
static int wf_parse_connect_pipe(wfContext* wfc, const char* value)
{
	char* end = nullptr;
	HANDLE in = nullptr;
	HANDLE out = nullptr;

	if (!value || wfc->connectPipeIn || !wf_parse_pipe_handle(value, &end, &in) || (*end != ','))
		return COMMAND_LINE_ERROR;

	const char* second = end + 1;

	if (!wf_parse_pipe_handle(second, &end, &out) || (*end != '\0') || (in == out))
		return COMMAND_LINE_ERROR;

	wfc->connectPipeIn = in;
	wfc->connectPipeOut = out;
	return 0;
}

static BOOL wf_connect_pipe_read(HANDLE pipe, void* data, DWORD size)
{
	BYTE* bytes = (BYTE*)data;
	DWORD total = 0;

	while (total < size)
	{
		DWORD read = 0;

		if (!ReadFile(pipe, bytes + total, size - total, &read, nullptr) || (read == 0))
			return FALSE;

		total += read;
	}

	return TRUE;
}

static BOOL wf_connect_pipe_write(HANDLE pipe, const void* data, DWORD size)
{
	const BYTE* bytes = (const BYTE*)data;
	DWORD total = 0;

	while (total < size)
	{
		DWORD written = 0;

		if (!WriteFile(pipe, bytes + total, size - total, &written, nullptr) || (written == 0))
			return FALSE;

		total += written;
	}

	return TRUE;
}

/* TCPConnect of the transport with /connect-pipe: the parent process connects to the server
 * (through its tunnel) and gives the socket; it may refuse a host it did not expect (a server
 * redirection). The timeout is the parent's. */
static int wf_connect_pipe_tcp(rdpContext* context, rdpSettings* settings, const char* hostname,
                               int port, DWORD timeout)
{
	WINPR_UNUSED(settings);
	WINPR_UNUSED(timeout);

	wfContext* wfc = (wfContext*)context;
	WSAPROTOCOL_INFOW info = WINPR_C_ARRAY_INIT;
	UINT16 request[2] = WINPR_C_ARRAY_INIT;
	UINT32 length = 0;
	SOCKET sockfd = INVALID_SOCKET;
	const size_t hostLength = hostname ? strlen(hostname) : 0;

	WINPR_ASSERT(wfc);

	if ((hostLength == 0) || (hostLength > UINT16_MAX) || (port <= 0) || (port > UINT16_MAX))
		goto fail;

	request[0] = (UINT16)port;
	request[1] = (UINT16)hostLength;
	WLog_DBG(TAG, "connection to %s:%d asked from the parent process", hostname, port);

	if (!wf_connect_pipe_write(wfc->connectPipeOut, request, sizeof(request)) ||
	    !wf_connect_pipe_write(wfc->connectPipeOut, hostname, (DWORD)hostLength))
	{
		WLog_ERR(TAG, "the parent process does not answer the connection request");
		goto fail;
	}

	if (!wf_connect_pipe_read(wfc->connectPipeIn, &length, sizeof(length)))
	{
		WLog_ERR(TAG, "no connection from the parent process");
		goto fail;
	}

	if (length != sizeof(info))
	{
		if (length == 0)
			WLog_ERR(TAG, "the parent process did not connect to %s:%d", hostname, port);
		else
			WLog_ERR(TAG, "unexpected answer of %" PRIu32 " bytes from the parent process", length);

		goto fail;
	}

	if (!wf_connect_pipe_read(wfc->connectPipeIn, &info, sizeof(info)))
		goto fail;

	sockfd = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
	                    WSA_FLAG_OVERLAPPED);

	if (sockfd == INVALID_SOCKET)
	{
		WLog_ERR(TAG, "socket from the parent process: WSASocketW failed with %d", WSAGetLastError());
		goto fail;
	}

	return (int)sockfd;

fail:
	freerdp_set_last_error_if_not(context, FREERDP_ERROR_CONNECT_FAILED);
	return -1;
}

/* /frame-pipe:<handle>[,interval:<ms>]: the handle must be a pipe of this process. */
static int wf_parse_frame_pipe(wfContext* wfc, const char* value)
{
	char* end = nullptr;

	if (!value || (*value == '\0'))
		return COMMAND_LINE_ERROR;

	/* Decimal (as .NET writes it), or hexadecimal with 0x. */
	const int base = ((value[0] == '0') && ((value[1] == 'x') || (value[1] == 'X'))) ? 16 : 10;
	errno = 0;
	const unsigned long long handle = strtoull(value, &end, base);

	if ((errno != 0) || (end == value) || (handle == 0) || (handle > UINTPTR_MAX))
		return COMMAND_LINE_ERROR;

	UINT32 interval = WF_FRAME_INTERVAL_DEFAULT;

	if (*end == ',')
	{
		const char* option = end + 1;
		const char prefix[] = "interval:";

		if (strncmp(option, prefix, sizeof(prefix) - 1) != 0)
			return COMMAND_LINE_ERROR;

		const char* number = option + sizeof(prefix) - 1;
		errno = 0;
		const unsigned long ms = strtoul(number, &end, 10);

		if ((errno != 0) || (end == number) || (*end != '\0') || (ms < 10) || (ms > 60000))
			return COMMAND_LINE_ERROR;

		interval = (UINT32)ms;
	}
	else if (*end != '\0')
		return COMMAND_LINE_ERROR;

	const HANDLE pipe = (HANDLE)(uintptr_t)handle;

	if (GetFileType(pipe) != FILE_TYPE_PIPE)
		return COMMAND_LINE_ERROR;

	if (wfc->framePipe)
		(void)CloseHandle(wfc->framePipe);

	wfc->framePipe = pipe;
	wfc->frameInterval = interval;
	return 0;
}

static int wf_handle_option(const COMMAND_LINE_ARGUMENT_A* arg, void* custom)
{
	wfContext* wfc = (wfContext*)custom;

	if (!arg || !arg->Name || !wfc)
		return COMMAND_LINE_ERROR;

	if (strcmp(arg->Name, "frame-pipe") == 0)
		return wf_parse_frame_pipe(wfc, arg->Value);

	if (strcmp(arg->Name, "connect-pipe") == 0)
		return wf_parse_connect_pipe(wfc, arg->Value);

	return 0;
}

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

	WINPR_UNUSED(hInstance);
	WINPR_UNUSED(hPrevInstance);
	WINPR_UNUSED(lpCmdLine);
	WINPR_UNUSED(nCmdShow);

	/* Libraries loaded at run time (channel add-ins, delay-loaded system libraries) are looked
	 * for in the application directory and System32 only, never in the current directory or the
	 * PATH, where anyone may have dropped a library of the same name. */
	if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
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

	freerdp_client_warn_deprecated(argc, argv);

	settings = context->settings;
	wfc = (wfContext*)context;

	if (!settings || !wfc)
		goto out;

	status = freerdp_client_settings_parse_command_line_ex(
	    settings, argc, argv, FALSE, wf_args, ARRAYSIZE(wf_args) - 1, wf_handle_option, wfc);
	if (status)
	{
		ret = freerdp_client_settings_command_line_status_print_ex(settings, status, argc, argv,
		                                                           wf_args);
		goto out;
	}

	AddDefaultSettings(settings);

	if (wfc->connectPipeIn)
	{
		/* Kept by the transport through reconnections (see rdp_new_common). */
		const rdpTransportIo* current = freerdp_get_io_callbacks(context);

		if (!current)
			goto out;

		rdpTransportIo io = *current;
		io.TCPConnect = wf_connect_pipe_tcp;

		if (!freerdp_set_io_callbacks(context, &io))
			goto out;

		/* The parent opens the connection: no proxy from the environment (https_proxy) in
		 * between, each request names the server. */
		if (!freerdp_settings_set_uint32(settings, FreeRDP_ProxyType, PROXY_TYPE_IGNORE))
			goto out;
	}

	if (freerdp_client_start(context) != 0)
		goto out;

	thread = freerdp_client_get_thread(context);

	if (thread)
	{
		if (WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0)
		{
			GetExitCodeThread(thread, &dwExitCode);
			ret = (int)dwExitCode;
		}
	}

	if (freerdp_client_stop(context) != 0)
		goto out;

out:
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
