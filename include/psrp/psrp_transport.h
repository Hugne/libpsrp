/** @file
 * psrp_transport.h - carrying PSRP over WSMan ([MS-PSRP] 3.1.5.3).
 *
 * The protocol core does no I/O; this is the piece that does. It is kept
 * behind a narrow interface so the session can be driven by a real WinRM
 * endpoint or by a test double.
 *
 * The mapping of PSRP onto WSMan requests, from 3.1.5.3:
 *
 *   open()        -> wxf:Create. The payload is base64'd into a `<creationXml>`
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
    /** e.g. L"http://localhost:5985/wsman". NULL uses that default. */
    const wchar_t *connection;
    /** NULL authenticates as the current user via Negotiate. */
    const wchar_t *username;
    const wchar_t *password;
    /* 0 selects the 240000 ms the spec's appendix cites as typical. */
    uint32_t operation_timeout_ms;
} psrp_wsman_config_t;

psrp_result_t psrp_wsman_transport_create(const psrp_wsman_config_t *cfg,
                                          psrp_transport_t **out);
void psrp_transport_free(psrp_transport_t *t);

/* Creates the remote shell, carrying psrp_session_open_payload's output in
 * `<creationXml>`. `shell_id` becomes the WSMan ShellId; pass the RunspacePool
 * GUID so the WSMan and PSRP id spaces line up, as PowerShell does. */
/** Opens a shell, carrying `payload` as the creationXml.
 *
 * A transport may be reused: after psrp_transport_close_shell, another shell
 * can be opened on the same transport. Prefer that: each discarded session
 * leaves a WinHTTP connection Event behind until WinHTTP scavenges it about a
 * minute later, so a transport per shell holds handles it does not need (TODO
 * PSRP-14). They are reclaimed either way; 80 shells on one transport simply
 * cost nothing in the first place. */
psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *shell_id,
                                  const void *payload, size_t len);

/** Starts a pipeline from psrp_session_pipeline_payload's output.
 * `command_id` becomes the WSMan CommandId; pass the pipeline GUID.
 * Only the first fragment may go in Arguments (3.1.5.3.3); this splits the
 * payload and sends any remainder itself. */
psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *command_id,
                                         const void *payload, size_t len);

/** Sends bytes on the "stdin" stream. */
psrp_result_t psrp_transport_send(psrp_transport_t *t,
                                  const void *data, size_t len);

/** Sends bytes on the "pr" stream, which 3.1.5.3.5 reserves for host
 * responses. Pair it with psrp_session_take_priority_output. */
psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len);

/** Appends whatever has arrived on "stdout", waiting up to `timeout_ms` for at
 * least one byte. Returns PSRP_ERR_TRUNCATED if nothing arrived in time,
 * which is a normal "keep waiting" answer rather than a failure. */
psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms);

/** Stops the running pipeline with a wxf:Signal (3.1.4.4, 3.1.5.3.9). The
 * signal is targeted at the command, so it stops that pipeline rather than
 * the whole pool. */
psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t);

/** Closes the remote shell with a wxf:Delete (3.1.4.2). psrp_transport_free
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

/** wxf:Disconnect (3.1.5.3.16). `idle_timeout_ms` is how long the server keeps
 * the shell alive with nobody attached; 0 asks for the server's default. */
psrp_result_t psrp_transport_disconnect(psrp_transport_t *t,
                                        uint32_t idle_timeout_ms);

/** wxf:Reconnect (3.1.5.3.18), for the client that disconnected. */
psrp_result_t psrp_transport_reconnect(psrp_transport_t *t);

/** wxf:Connect (3.1.5.3.14), for a client adopting someone else's shell.
 * `payload` is the SESSION_CAPABILITY and CONNECT_RUNSPACEPOOL pair from
 * psrp_session_connect_payload, which rides in the message's open content.
 *
 * The server answers in kind: 3.1.5.3.15 puts its own SESSION_CAPABILITY in
 * the ConnectResponse's open content, not on the stream. On success that
 * decoded content is written to `response_payload` (which may be NULL to
 * discard it), and the caller feeds it to psrp_session_receive exactly as it
 * would bytes from psrp_transport_receive. A response without it fails the
 * connect, which is what the spec requires. */
psrp_result_t psrp_transport_connect(psrp_transport_t *t,
                                     const psrp_guid_t *shell_id,
                                     const void *payload, size_t len,
                                     psrp_buffer_t *response_payload);

/** True while the shell is disconnected. */
bool psrp_transport_is_disconnected(const psrp_transport_t *t);

/* ---------------- discovering RunspacePools (3.1.4.10.1) --------------- */
/**
 * Connecting to a pool this client did not create needs that pool's
 * identifier first. Each RunspacePool is a WSMan shell, so enumerating the
 * server's shells lists them; the ShellId is the pool id, and it is what
 * psrp_session_adopt_pool takes.
 *
 * This goes through the WSMan automation interface rather than the flat C API
 * the rest of the transport uses, because that API covers shells and commands
 * but not WS-Enumerate. It is the same interface `winrm enumerate` uses.
 */
typedef struct psrp_shell_info {
    psrp_guid_t shell_id;   /**< the RunspacePool id */
    char *name;             /**< owned; NULL when the server did not report it */
    char *owner;
    char *state;            /**< e.g. "Connected" or "Disconnected" */
    char *resource_uri;
} psrp_shell_info_t;

/** A discovery session. Hold one of these when listing shells more than once.
 *
 * Each WSMan session that does work leaves a WinHTTP connection Event behind
 * for about a minute before WinHTTP's scavenger reclaims it, so building and
 * tearing one down per call holds roughly one handle per call for that long.
 * Nothing is leaked -- the handles come back -- but a caller listing in a
 * loop pays for it needlessly, and reuse costs nothing measurable. See TODO
 * PSRP-14.
 *
 * COM is initialised for the calling thread when the handle is opened and
 * uninitialised when it is freed, so a handle must be opened, used and freed
 * on the same thread. */
typedef struct psrp_discovery psrp_discovery_t;

/** Opens a discovery session. `cfg` supplies the endpoint and credentials; a
 * NULL connection means the local machine. */
psrp_result_t psrp_wsman_discovery_open(const psrp_wsman_config_t *cfg,
                                        psrp_discovery_t **out);
void psrp_wsman_discovery_free(psrp_discovery_t *d);

/** Lists the shells. Safe to call repeatedly on one handle. The caller frees
 * the result with psrp_shell_info_free_all; an empty list is PSRP_OK with
 * count 0. */
psrp_result_t psrp_wsman_discovery_shells(psrp_discovery_t *d,
                                          psrp_shell_info_t **out,
                                          size_t *count);

/** One-shot convenience: opens a session, lists the shells, closes it. Fine for
 * a single lookup. Use a psrp_discovery_t when listing more than once, for the
 * reason described above. */
psrp_result_t psrp_wsman_enumerate_shells(const psrp_wsman_config_t *cfg,
                                          psrp_shell_info_t **out,
                                          size_t *count);

void psrp_shell_info_free(psrp_shell_info_t *s);
void psrp_shell_info_free_all(psrp_shell_info_t *list, size_t count);

/** Parses one shell element from an enumeration result. Exposed so the parsing
 * can be tested without a server; a shell carrying no ShellId is rejected,
 * since it could not be connected to anyway. */
psrp_result_t psrp_wsman_parse_shell(const void *xml, size_t n,
                                     psrp_shell_info_t *out);

/** True once the remote command has reported it is done. */
bool psrp_transport_command_done(const psrp_transport_t *t);

/** Human-readable detail for the last failure; never NULL. */
const char *psrp_transport_last_error(const psrp_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TRANSPORT_H */
