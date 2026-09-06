/** @file
 * psrp_transport.h - moving PSRP bytes, whatever carries them.
 *
 * The protocol core does no I/O. This is the contract it needs from whatever
 * does: a way to open a session, start a pipeline, push bytes, pull bytes, and
 * shut down. Nothing here knows how those bytes travel.
 *
 * Constructing a transport is deliberately NOT part of this header, because
 * every carrier needs different things to be constructed -- an endpoint and
 * credentials for WinRM, a host and key for SSH. `psrp_winrm.h` has the WinRM
 * constructor and the operations only WinRM can perform. A caller includes
 * the one it is using; everything after construction is the same either way.
 *
 * The vocabulary here is WS-Management's, because that is the only carrier
 * implemented: a "shell" is a session and a "command" is a pipeline. PSRP
 * over SSH, which PowerShell also speaks, has neither -- it is a plain byte
 * stream. Renaming for a transport that does not exist yet would be inventing
 * an abstraction on speculation, so the names stay until there is a second
 * carrier to name them against.
 *
 * The two implementations are selected at build time, never both at once,
 * exactly as the XML and crypto backends are:
 *
 *   Windows   transport_wsman.c   a thin shim over the Win32 WSMan client
 *   elsewhere transport_curl.c    HTTP, GSS-API and MS-WSMV encryption, built
 *                                 from parts, because there is no OS client
 */
#ifndef PSRP_TRANSPORT_H
#define PSRP_TRANSPORT_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_transport psrp_transport_t;

/** Releases the transport, closing the session first if one is open.
 * Safe on NULL. */
void psrp_transport_free(psrp_transport_t *t);

/** Opens a session, carrying psrp_session_open_payload's output with it.
 *
 * Over WinRM this is a wxf:Create whose open content is the payload, base64'd
 * into a `<creationXml>` element, and `shell_id` becomes the WSMan ShellId;
 * pass the RunspacePool GUID so the two id spaces line up, as PowerShell does.
 *
 * A transport may be reused: after psrp_transport_close_shell another session
 * can be opened on the same transport, and that is the cheaper way to run
 * several. See TODO PSRP-14 for what a transport per session costs. */
psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *shell_id,
                                  const void *payload, size_t len);

/** Starts a pipeline from psrp_session_pipeline_payload's output.
 *
 * `command_id` becomes the pipeline's identifier. Over WinRM only the first
 * fragment may ride in the request (3.1.5.3.3); the implementation splits the
 * payload and sends the remainder itself, so a caller passes the whole thing. */
psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *command_id,
                                         const void *payload, size_t len);

/** Sends bytes on the ordinary data stream. */
psrp_result_t psrp_transport_send(psrp_transport_t *t,
                                  const void *data, size_t len);

/** Sends bytes on the priority stream, which 3.1.5.3.5 reserves for host
 * responses. Pair it with psrp_session_take_priority_output. */
psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len);

/** Appends whatever has arrived, waiting up to `timeout_ms` for at least one
 * byte. PSRP_ERR_TRUNCATED means nothing arrived in time, which is a normal
 * "keep waiting" answer rather than a failure. */
psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms);

/** Stops the running pipeline. Targeted at the pipeline, so it does not take
 * the session with it (3.1.4.4). */
psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t);

/** Closes the session (3.1.4.2). psrp_transport_free does this anyway; call
 * it directly when you want to observe the result. */
psrp_result_t psrp_transport_close_shell(psrp_transport_t *t);

/** True once the remote pipeline has reported it is done. */
bool psrp_transport_command_done(const psrp_transport_t *t);

/** Human-readable detail for the last failure; never NULL. */
const char *psrp_transport_last_error(const psrp_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TRANSPORT_H */
