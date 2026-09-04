/* psrp_transport.h - carrying PSRP over WSMan ([MS-PSRP] 3.1.5.3).
 *
 * The protocol core does no I/O; this is the piece that does. It is kept
 * behind a narrow interface so the session can be driven by a real WinRM
 * endpoint or by a test double.
 *
 * The mapping of PSRP onto WSMan requests, from 3.1.5.3:
 *
 *   open()        -> wxf:Create. The payload is base64'd into a <creationXml>
 *                    element carried as the Create open content.
 *   run_command() -> wxf:Command. The Command element MUST be empty and
 *                    Arguments carries only the FIRST fragment; the rest are
 *                    sent afterwards with Send. This function handles that
 *                    split itself.
 *   send()        -> wxf:Send on the "stdin" stream (or "pr" for host
 *                    responses).
 *   receive()     -> wxf:Receive on the "stdout" stream.
 */
#ifndef PSRP_TRANSPORT_H
#define PSRP_TRANSPORT_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_transport psrp_transport_t;

typedef struct psrp_wsman_config {
    /* e.g. L"http://localhost:5985/wsman". NULL uses that default. */
    const wchar_t *connection;
    /* NULL authenticates as the current user via Negotiate. */
    const wchar_t *username;
    const wchar_t *password;
    /* 0 selects the 240000 ms the spec's appendix cites as typical. */
    uint32_t operation_timeout_ms;
} psrp_wsman_config_t;

psrp_result_t psrp_wsman_transport_create(const psrp_wsman_config_t *cfg,
                                          psrp_transport_t **out);
void psrp_transport_free(psrp_transport_t *t);

/* Creates the remote shell, carrying psrp_session_open_payload's output in
 * <creationXml>. `shell_id` becomes the WSMan ShellId; pass the RunspacePool
 * GUID so the WSMan and PSRP id spaces line up, as PowerShell does. */
psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *shell_id,
                                  const void *payload, size_t len);

/* Starts a pipeline from psrp_session_pipeline_payload's output.
 * `command_id` becomes the WSMan CommandId; pass the pipeline GUID.
 * Only the first fragment may go in Arguments (3.1.5.3.3); this splits the
 * payload and sends any remainder itself. */
psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *command_id,
                                         const void *payload, size_t len);

/* Sends bytes on the "stdin" stream. */
psrp_result_t psrp_transport_send(psrp_transport_t *t,
                                  const void *data, size_t len);

/* Sends bytes on the "pr" stream, which 3.1.5.3.5 reserves for host
 * responses. Pair it with psrp_session_take_priority_output. */
psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len);

/* Appends whatever has arrived on "stdout", waiting up to `timeout_ms` for at
 * least one byte. Returns PSRP_ERR_TRUNCATED if nothing arrived in time,
 * which is a normal "keep waiting" answer rather than a failure. */
psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms);

/* Stops the running pipeline with a wxf:Signal (3.1.4.4, 3.1.5.3.9). The
 * signal is targeted at the command, so it stops that pipeline rather than
 * the whole pool. */
psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t);

/* Closes the remote shell with a wxf:Delete (3.1.4.2). psrp_transport_free
 * does this anyway; call it directly when you want to observe the result. */
psrp_result_t psrp_transport_close_shell(psrp_transport_t *t);

/* ---------------- disconnect and reconnect (3.1.4.9, 3.1.4.10) --------- */
/*
 * A disconnected shell keeps running on the server until its idle timeout
 * expires. The same client can reconnect to it, or another client can connect
 * to it after discovering its ShellID.
 *
 * Each of these performs the wxf exchange only. Tell the session what happened
 * with psrp_session_notify_disconnected / _reconnected / _fault, so the
 * protocol state and the transport state cannot drift apart.
 */

/* wxf:Disconnect (3.1.5.3.16). `idle_timeout_ms` is how long the server keeps
 * the shell alive with nobody attached; 0 asks for the server's default. */
psrp_result_t psrp_transport_disconnect(psrp_transport_t *t,
                                        uint32_t idle_timeout_ms);

/* wxf:Reconnect (3.1.5.3.18), for the client that disconnected. */
psrp_result_t psrp_transport_reconnect(psrp_transport_t *t);

/* wxf:Connect (3.1.5.3.14), for a client adopting someone else's shell.
 * `payload` is the SESSION_CAPABILITY and CONNECT_RUNSPACEPOOL pair from
 * psrp_session_connect_payload, which rides in the message's open content. */
psrp_result_t psrp_transport_connect(psrp_transport_t *t,
                                     const psrp_guid_t *shell_id,
                                     const void *payload, size_t len);

/* True while the shell is disconnected. */
bool psrp_transport_is_disconnected(const psrp_transport_t *t);

/* True once the remote command has reported it is done. */
bool psrp_transport_command_done(const psrp_transport_t *t);

/* Human-readable detail for the last failure; never NULL. */
const char *psrp_transport_last_error(const psrp_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TRANSPORT_H */
