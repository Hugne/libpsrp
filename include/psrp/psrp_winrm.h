/** @file
 * psrp_winrm.h - carrying PSRP over WS-Management ([MS-PSRP] 3.1.5.3).
 *
 * Everything here is WinRM's, not PSRP's. Once a transport exists, the
 * protocol drives it through `psrp_transport.h` and never needs this header
 * again; a caller includes it to construct a transport, and for the few
 * operations that have no meaning on any other carrier.
 *
 * The mapping of PSRP onto WSMan requests, from 3.1.5.3:
 *
 *   open()        -> wxf:Create, payload base64'd into `<creationXml>`
 *   run_command() -> wxf:Command, first fragment only in Arguments
 *   send()        -> wxf:Send on "stdin" (or "pr" for host responses)
 *   receive()     -> wxf:Receive on "stdout"
 *   stop()        -> wxf:Signal, terminate
 *   close()       -> wxf:Delete
 *
 * Two implementations, chosen at build time. Windows has a WSMan client in
 * the OS and the transport there is a shim over it. Elsewhere it is built
 * from libcurl, GSS-API and MS-WSMV message encryption; that build is a
 * subset today, and every gap answers PSRP_ERR_UNSUPPORTED rather than
 * pretending. See TODO PSRP-35.
 */
#ifndef PSRP_WINRM_H
#define PSRP_WINRM_H

#include "psrp/psrp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_wsman_config {
    /** e.g. "http://localhost:5985/wsman". NULL uses that default.
     *
     * Strings here are UTF-8, like every string in this library. The transport
     * converts and keeps its own copies, so the config need not outlive the
     * call that consumes it. */
    const char *connection;
    /** NULL authenticates as the current user. */
    const char *username;
    const char *password;
    /** 0 selects the 240000 ms the spec's appendix cites as typical. */
    uint32_t operation_timeout_ms;
} psrp_wsman_config_t;

/** Builds a WinRM transport. Drive it through psrp_transport.h thereafter. */
psrp_result_t psrp_wsman_transport_create(const psrp_wsman_config_t *cfg,
                                          psrp_transport_t **out);

/* ---------------- disconnect and reconnect (3.1.4.9, 3.1.4.10) ---------- */
/*
 * A disconnected shell keeps running on the server until its idle timeout
 * expires. The same client can reconnect to it, or another client can connect
 * to it after discovering its ShellID.
 *
 * These are PSRP concepts, but only WS-Management provides the operations
 * that implement them, which is why they live here rather than in the
 * transport contract.
 *
 * Each performs the wxf exchange only. Tell the session what happened with
 * psrp_session_notify_disconnected / _reconnected / _fault, so the protocol
 * state and the transport state cannot drift apart.
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
#if defined(_WIN32)
/**
 * Connecting to a pool this client did not create needs that pool's
 * identifier first. Each RunspacePool is a WSMan shell, so enumerating the
 * server's shells lists them; the ShellId is the pool id, and it is what
 * psrp_session_adopt_pool takes.
 *
 * This goes through the WSMan COM automation interface rather than the flat C
 * API the rest of the transport uses, because that API covers shells and
 * commands but not WS-Enumerate.
 *
 * Declared only where it is implemented, deliberately. There is no
 * WS-Enumerate in the curl transport yet, and a declaration without a
 * definition turns a missing feature into an unresolved symbol at link time
 * with nothing to explain it. This way the call site fails to compile and
 * says which header it wanted.
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
 * loop pays for it needlessly. See TODO PSRP-14.
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

/** One-shot convenience: opens a session, lists the shells, closes it. Fine
 * for a single lookup. Use a psrp_discovery_t when listing more than once. */
psrp_result_t psrp_wsman_enumerate_shells(const psrp_wsman_config_t *cfg,
                                          psrp_shell_info_t **out,
                                          size_t *count);

void psrp_shell_info_free(psrp_shell_info_t *s);
void psrp_shell_info_free_all(psrp_shell_info_t *list, size_t count);

/** Parses one shell element from an enumeration result. Exposed so the
 * parsing can be tested without a server; a shell carrying no ShellId is
 * rejected, since it could not be connected to anyway. */
psrp_result_t psrp_wsman_parse_shell(const void *xml, size_t n,
                                     psrp_shell_info_t *out);
#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* PSRP_WINRM_H */
