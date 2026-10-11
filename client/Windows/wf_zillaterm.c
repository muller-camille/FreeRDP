/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Client, ZillaTerm host link
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <winpr/windows.h>
#include <winpr/crt.h>
#include <winpr/string.h>

#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/crypto.h>
#include <freerdp/log.h>
#include <freerdp/client.h>

#include "wf_zillaterm.h"

#define TAG CLIENT_TAG("windows.zillaterm")

/* Longest accepted line (a RemoteApp command line or a certificate chain is far shorter). */
#define ZT_MAX_LINE (64 * 1024)
#define ZT_MAX_ITEMS 256

static BOOL s_enabled = FALSE;
static HANDLE s_in = nullptr;
static HANDLE s_out = nullptr;
static CRITICAL_SECTION s_write_lock;
static char s_buffer[4096];
static size_t s_buffered = 0;

static HANDLE s_answer_event = nullptr;
static volatile LONG s_answer = -1;
static volatile LONG s_closed = FALSE;

static void zt_wipe_free(char* text)
{
	if (text)
	{
		SecureZeroMemory(text, strlen(text));
		free(text);
	}
}

void wf_zt_free_list(char** list)
{
	if (!list)
		return;

	for (size_t i = 0; list[i]; i++)
		zt_wipe_free(list[i]);
	free((void*)list);
}

/* One line from stdin, without its end of line; nullptr at end of input or on error. */
static char* zt_read_line(void)
{
	size_t length = 0;
	size_t capacity = 256;
	char* line = calloc(capacity, sizeof(char));
	if (!line)
		return nullptr;

	for (;;)
	{
		if (s_buffered == 0)
		{
			DWORD got = 0;
			if (!ReadFile(s_in, s_buffer, sizeof(s_buffer), &got, nullptr) || (got == 0))
			{
				zt_wipe_free(line);
				return nullptr;
			}
			s_buffered = got;
		}

		const char* end = memchr(s_buffer, '\n', s_buffered);
		const size_t take = end ? (size_t)(end - s_buffer) : s_buffered;
		if (length + take + 1 > ZT_MAX_LINE)
		{
			WLog_ERR(TAG, "input line too long");
			zt_wipe_free(line);
			return nullptr;
		}

		if (length + take + 1 > capacity)
		{
			size_t larger = capacity;
			while (larger < length + take + 1)
				larger *= 2;
			char* bigger = calloc(larger, sizeof(char));
			if (!bigger)
			{
				zt_wipe_free(line);
				return nullptr;
			}
			memcpy(bigger, line, length);
			SecureZeroMemory(line, capacity);
			free(line);
			line = bigger;
			capacity = larger;
		}

		memcpy(&line[length], s_buffer, take);
		length += take;
		const size_t used = end ? take + 1 : take;
		memmove(s_buffer, &s_buffer[used], s_buffered - used);
		s_buffered -= used;
		SecureZeroMemory(&s_buffer[s_buffered], sizeof(s_buffer) - s_buffered);

		if (end)
		{
			if ((length > 0) && (line[length - 1] == '\r'))
				length--;
			line[length] = '\0';
			return line;
		}
	}
}

BOOL wf_zt_enabled(void)
{
	return s_enabled;
}

static BOOL zt_append(char*** plist, size_t* pcount, char* item)
{
	if (*pcount + 1 >= ZT_MAX_ITEMS)
		return FALSE;

	char** list = *plist;
	if (!list)
	{
		list = calloc(ZT_MAX_ITEMS, sizeof(char*));
		if (!list)
			return FALSE;
		*plist = list;
	}

	list[(*pcount)++] = item;
	return TRUE;
}

BOOL wf_zt_read_arguments(const char* program, int* pargc, char*** pargv, char*** psettings)
{
	char** argv = nullptr;
	char** settings = nullptr;
	size_t argc = 0;
	size_t count = 0;

	WINPR_ASSERT(pargc);
	WINPR_ASSERT(pargv);
	WINPR_ASSERT(psettings);

	s_in = GetStdHandle(STD_INPUT_HANDLE);
	s_out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!s_in || (s_in == INVALID_HANDLE_VALUE) || !s_out || (s_out == INVALID_HANDLE_VALUE))
		return FALSE;

	InitializeCriticalSection(&s_write_lock);
	s_enabled = TRUE;

	char* name = _strdup(program ? program : "wfreerdp");
	if (!name || !zt_append(&argv, &argc, name))
	{
		free(name);
		goto fail;
	}

	for (;;)
	{
		char* line = zt_read_line();
		if (!line)
			goto fail;

		if (line[0] == '\0')
		{
			free(line);
			break;
		}

		BOOL ok = FALSE;
		if ((line[0] == '/') || (line[0] == '+') || (line[0] == '-'))
			ok = zt_append(&argv, &argc, line);
		else if ((strncmp(line, "FreeRDP_", 8) == 0) && strchr(line, '='))
			ok = zt_append(&settings, &count, line);

		if (!ok)
		{
			WLog_ERR(TAG, "unexpected input line");
			zt_wipe_free(line);
			goto fail;
		}
	}

	if (!settings)
	{
		settings = calloc(1, sizeof(char*));
		if (!settings)
			goto fail;
	}

	*pargc = (int)argc;
	*pargv = argv;
	*psettings = settings;
	return TRUE;

fail:
	wf_zt_free_list(argv);
	wf_zt_free_list(settings);
	return FALSE;
}

BOOL wf_zt_apply_settings(rdpSettings* settings, char** lines)
{
	BOOL rc = TRUE;

	for (size_t i = 0; lines && lines[i]; i++)
	{
		char* separator = strchr(lines[i], '=');
		if (!separator)
			continue;

		*separator = '\0';
		if (!freerdp_settings_set_value_for_name(settings, lines[i], separator + 1))
		{
			WLog_ERR(TAG, "setting %s refused", lines[i]);
			rc = FALSE;
		}
		*separator = '=';
	}

	return rc;
}

void wf_zt_event(const char* format, ...)
{
	char text[2048] = WINPR_C_ARRAY_INIT;
	char* line = nullptr;
	va_list args;

	if (!s_enabled)
		return;

	va_start(args, format);
	const int length = vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (length < 0)
		return;

	/* The certificate message carries a whole chain: allocated, not truncated. */
	if ((size_t)length >= sizeof(text))
	{
		line = calloc((size_t)length + 1, sizeof(char));
		if (!line)
			return;
		va_start(args, format);
		(void)vsnprintf(line, (size_t)length + 1, format, args);
		va_end(args);
	}

	const char* body = line ? line : text;
	EnterCriticalSection(&s_write_lock);
	{
		DWORD written = 0;
		(void)WriteFile(s_out, "zillaterm:", 10, &written, nullptr);
		(void)WriteFile(s_out, body, (DWORD)strlen(body), &written, nullptr);
		(void)WriteFile(s_out, "\n", 1, &written, nullptr);
		(void)FlushFileBuffers(s_out);
	}
	LeaveCriticalSection(&s_write_lock);
	free(line);
}

static DWORD WINAPI zt_reader_thread(LPVOID arg)
{
	rdpContext* context = (rdpContext*)arg;

	for (;;)
	{
		char* line = zt_read_line();
		if (!line)
			break;

		if (strcmp(line, "accept") == 0)
		{
			InterlockedExchange(&s_answer, 1);
			(void)SetEvent(s_answer_event);
		}
		else if (strcmp(line, "deny") == 0)
		{
			InterlockedExchange(&s_answer, 0);
			(void)SetEvent(s_answer_event);
		}
		else if (strcmp(line, "disconnect") == 0)
		{
			WLog_INFO(TAG, "disconnect requested");
			(void)freerdp_abort_connect_context(context);
		}
		else
			WLog_WARN(TAG, "unexpected input line");

		zt_wipe_free(line);
	}

	/* ZillaTerm closed its end (tab closed, application gone): the session ends. */
	WLog_INFO(TAG, "end of input, disconnecting");
	InterlockedExchange(&s_closed, TRUE);
	(void)SetEvent(s_answer_event);
	(void)freerdp_abort_connect_context(context);
	return 0;
}

BOOL wf_zt_start(rdpContext* context)
{
	if (!s_enabled)
		return TRUE;

	s_answer_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	if (!s_answer_event)
		return FALSE;

	HANDLE thread = CreateThread(nullptr, 0, zt_reader_thread, context, 0, nullptr);
	if (!thread)
		return FALSE;

	(void)CloseHandle(thread);
	return TRUE;
}

int wf_zt_verify_x509(freerdp* instance, const BYTE* data, size_t length, const char* hostname,
                      UINT16 port, DWORD flags)
{
	if (!s_enabled || !instance || !instance->context || !data || (length == 0) || !hostname)
		return 0;

	char* encoded = crypto_base64_encode(data, length);
	if (!encoded)
		return 0;

	InterlockedExchange(&s_answer, -1);
	(void)ResetEvent(s_answer_event);
	wf_zt_event("certificate %" PRIu32 " %" PRIu16 " %s %s", (UINT32)flags, port, encoded,
	            hostname);
	free(encoded);

	/* Waits for ZillaTerm (its own dialog may be open), the end of input, or an abort. */
	HANDLE handles[] = { s_answer_event, freerdp_abort_event(instance->context) };
	const DWORD count = handles[1] ? 2 : 1;
	for (;;)
	{
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
		if (status != WAIT_OBJECT_0)
			return 0;
		if (InterlockedCompareExchange(&s_closed, FALSE, FALSE))
			return 0;
		const LONG answer = InterlockedCompareExchange(&s_answer, -1, -1);
		if (answer >= 0)
			return (answer == 1) ? 1 : 0;
	}
}

SSIZE_T wf_zt_retry_dialog(freerdp* instance, const char* what, size_t current, void* userarg)
{
	const SSIZE_T delay = client_common_retry_dialog(instance, what, current, userarg);
	if ((delay < 0) || !s_enabled)
		return delay;

	if (freerdp_shall_disconnect_context(instance->context))
		return -1;

	wf_zt_event("reconnecting %" PRIuz, current + 1);
	if (current == 0)
		return 0;

	/* The previous attempt failed: waits here rather than in the caller's Sleep loop, answering
	 * the messages sent to the window (ZillaTerm checks that it responds) and stopping at once
	 * when the session is closed. */
	HANDLE abort = freerdp_abort_event(instance->context);
	const ULONGLONG end = GetTickCount64() + (ULONGLONG)delay;
	for (;;)
	{
		const ULONGLONG now = GetTickCount64();
		if (now >= end)
			break;

		const DWORD count = abort ? 1 : 0;
		const DWORD status =
		    MsgWaitForMultipleObjects(count, &abort, FALSE, (DWORD)(end - now), QS_SENDMESSAGE);
		if ((count == 1) && (status == WAIT_OBJECT_0))
			return -1;
		if (status == WAIT_FAILED)
			return -1;

		MSG msg = WINPR_C_ARRAY_INIT;
		(void)PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
	}
	return 0;
}
