/** @file
 * psrp_transport.h - PSRP's use of a WinRM session.
 *
 * The protocol core does no I/O. This is the contract it needs from whatever
 * does: open a session, start a pipeline, push bytes, pull bytes, shut down.
 *
 * PSRP is a construct layered on WS-Management, and the two layers are kept
 * apart. `winrm.h` is a WS-Management client and knows nothing about
 * PowerShell -- no runspace pools, no pipelines, no fragments. Everything on
 * this side of the line is PSRP's: which stream host responses travel on,
 * that a payload is a sequence of fragments and only the first may ride in
 * the Command request, that a pipeline identifier is a GUID.
 *
 * That division is why this header exists at all rather than the core calling
 * winrm.h directly. The mapping between the two is small but it is real, and
 * it belongs in one place: src/transport/psrp_over_winrm.c.
 */
#ifndef PSRP_TRANSPORT_H
#define PSRP_TRANSPORT_H

#include "psrp/winrm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_transport psrp_transport_t;

/** Builds a transport over a WinRM session.
 *
 * WS-Management is the only carrier this library speaks, so there is one
 * constructor and it names it. Everything after construction goes through the
 * calls below, which is the surface the protocol core actually consumes. */
psrp_result_t psrp_transport_over_winrm(const winrm_config_t *cfg,
                                        psrp_transport_t **out);

/** The session underneath, for the WinRM-only operations -- disconnect,
 * reconnect, connect, and asking which authentication mechanism was
 * negotiated. Remains owned by the transport. */
winrm_session_t *psrp_transport_session(psrp_transport_t *t);

/** Releases the transport, closing the shell first if one is open.
 * Safe on NULL. */
void psrp_transport_free(psrp_transport_t *t);

/** Opens a RunspacePool, carrying psrp_session_open_payload's output as the
 * shell's open content.
 *
 * `pool_id` becomes the WinRM ShellId, which is PowerShell's convention: it
 * lines the two identifier spaces up. WinRM itself does not require it.
 *
 * A transport may be reused: after psrp_transport_close_shell another pool
 * can be opened on the same transport, and that is the cheaper way to run
 * several. See TODO PSRP-14 for what a transport per pool costs. */
psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *pool_id,
                                  const void *payload, size_t len);

/** Starts a pipeline from psrp_session_pipeline_payload's output.
 *
 * 3.1.5.3.3 allows only the first fragment to ride in the Command request;
 * this splits the payload on its first fragment boundary and sends the
 * remainder on the input stream, so a caller passes the whole thing. Knowing
 * where that boundary is requires reading a PSRP fragment header, which is
 * why the split lives on this side of the layer and not in the WinRM
 * client. */
psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *pipeline_id,
                                         const void *payload, size_t len);

/** Sends bytes as pipeline input. */
psrp_result_t psrp_transport_send(psrp_transport_t *t,
                                  const void *data, size_t len);

/** Sends bytes as a host response. 3.1.5.3.5 reserves a separate stream for
 * these so that a host reply cannot queue behind pipeline input. Pair it with
 * psrp_session_take_priority_output. */
psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len);

/** Appends whatever has arrived, waiting up to `timeout_ms` for at least one
 * byte. PSRP_ERR_TRUNCATED means nothing arrived in time, which is a normal
 * "keep waiting" answer rather than a failure. */
psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms);

/** Stops the running pipeline without taking the pool with it (3.1.4.4). */
psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t);

/** Closes the pool (3.1.4.2). psrp_transport_free does this anyway; call it
 * directly when you want to observe the result. */
psrp_result_t psrp_transport_close_shell(psrp_transport_t *t);

/** True once the remote pipeline has reported it is done. */
bool psrp_transport_command_done(const psrp_transport_t *t);

/** Human-readable detail for the last failure; never NULL. */
const char *psrp_transport_last_error(const psrp_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TRANSPORT_H */
