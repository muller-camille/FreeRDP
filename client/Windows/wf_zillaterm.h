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

#ifndef FREERDP_CLIENT_WIN_ZILLATERM_H
#define FREERDP_CLIENT_WIN_ZILLATERM_H

#include <winpr/wtypes.h>
#include <freerdp/freerdp.h>

/*
 * ZillaTerm starts this client with the single argument "/zillaterm" and talks to it over
 * standard input and output, one UTF-8 line per message:
 *
 *  - stdin, first: the connection, one item per line, ended by an empty line. A line starting
 *    with '/', '+' or '-' is a command line option; "FreeRDP_<Name>=<value>" sets that setting
 *    after the options (values that must not go through the option parser: user name,
 *    password, start program, RemoteApp command line). Nothing secret is ever on the command
 *    line or on disk.
 *  - stdout: "zillaterm:certificate <flags> <port> <base64 PEM chain> <host>" for every TLS
 *    certificate (server, redirection or gateway). The connection waits for the answer on
 *    stdin: "accept" or "deny". "zillaterm:connected <window handle>" once connected,
 *    "zillaterm:reconnecting <attempt>" for each automatic reconnection attempt.
 *  - stdin, afterwards: "disconnect" ends the session. End of input (ZillaTerm gone) too.
 *
 * In this mode no dialog is ever shown (credentials, certificates, gateway messages), saved
 * Windows credentials are not read, and log messages go to stderr.
 */

/** TRUE when started by ZillaTerm ("/zillaterm"). */
BOOL wf_zt_enabled(void);

/** Reads the connection from stdin (see above) into a new argv. FALSE on error. */
BOOL wf_zt_read_arguments(const char* program, int* pargc, char*** pargv, char*** psettings);

/** Applies the "FreeRDP_<Name>=<value>" lines, then wipes them. */
BOOL wf_zt_apply_settings(rdpSettings* settings, char** lines);

/** Frees an argv or settings list returned by wf_zt_read_arguments (wiping its content). */
void wf_zt_free_list(char** list);

/** Starts the stdin reader (answers, "disconnect", end of input) for this connection. */
BOOL wf_zt_start(rdpContext* context);

/** Writes "zillaterm:<text>" to stdout. */
void wf_zt_event(const char* format, ...);

/** RetryDialog callback (automatic reconnection): "zillaterm:reconnecting <attempt>", then waits
 * between attempts while answering messages sent to the window; stops when the session is
 * closed. */
SSIZE_T wf_zt_retry_dialog(freerdp* instance, const char* what, size_t current, void* userarg);

/** VerifyX509Certificate callback: asks ZillaTerm. */
int wf_zt_verify_x509(freerdp* instance, const BYTE* data, size_t length, const char* hostname,
                      UINT16 port, DWORD flags);

#endif /* FREERDP_CLIENT_WIN_ZILLATERM_H */
