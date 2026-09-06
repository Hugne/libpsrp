/** @file
 * winrm.h - a WS-Management client ([MS-WSMV]).
 *
 * This layer knows nothing about PowerShell. It creates shells, runs commands
 * in them, moves bytes on named streams, signals, and deletes -- which is the
 * whole of what WS-Management's shell profile offers. What travels on those
 * streams is the caller's business.
 *
 * That is not a stylistic preference. PSRP is a construct layered on top of
 * WinRM, so the dependency runs one way: `psrp_transport.h` is implemented
 * over this header, and nothing here refers to runspace pools, pipelines,
 * fragments or [MS-PSRP] at all. An earlier version of this code decoded a
 * PSRP fragment header down here in order to split a payload, which had the
 * lower layer reading the upper layer's wire format; that split now happens
 * where it belongs, above.
 *
 * Identifiers are strings rather than GUIDs on purpose. WS-Management's
 * ShellId and CommandId are opaque selector strings; that PowerShell chooses
 * to put a GUID in them is PSRP's convention, not this layer's rule.
 *
 * Two implementations, selected at build time, never both:
 *
 *   Windows   winrm_wsman.c   a shim over the Win32 WSMan client in the OS
 *   elsewhere winrm_curl.c    HTTP, GSS-API and MS-WSMV message encryption,
 *                             built from parts, because there is no OS client
 */
#ifndef PSRP_WINRM_CLIENT_H
#define PSRP_WINRM_CLIENT_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A connection to a WS-Management endpoint, and at most one shell on it. */
typedef struct winrm_session winrm_session_t;

/** Which authentication mechanism to use.
 *
 * DEFAULT follows the credential, because measurement against a domain
 * controller says the two cases differ. Given a password it is NTLM; given
 * none it is SPNEGO, which then takes Kerberos from the ticket cache.
 *
 * SPNEGO is not the default in both cases only because it does not work in
 * both cases. With a name and password it still fails at the first wrap with
 * "no context has been established" -- gss-ntlmssp under SPNEGO leaves a
 * context that reports success and cannot encrypt -- and that was measured
 * against a real KDC, not only against the workgroup target that first
 * showed it, so the mechanism is at fault rather than the absence of a
 * realm. With no password and a ticket cache present, the same SPNEGO
 * request negotiates Kerberos and works, which is exactly the case NTLM
 * cannot serve at all.
 *
 * Naming the mechanism explicitly is what a test should do regardless.
 * Proving Kerberos was exercised means refusing to succeed by another route,
 * and winrm_negotiated_auth reports what was actually used.
 *
 * On Windows these select WSMan's own authentication flags and this
 * discussion does not apply. */
typedef enum winrm_auth {
    WINRM_AUTH_DEFAULT = 0,
    WINRM_AUTH_NTLM,
    WINRM_AUTH_KERBEROS,
    WINRM_AUTH_NEGOTIATE
} winrm_auth_t;

typedef struct winrm_config {
    /** e.g. "http://localhost:5985/wsman". NULL uses that default.
     *
     * Strings here are UTF-8. The session converts and keeps its own copies,
     * so the config need not outlive the call that consumes it. */
    const char *connection;
    /** NULL authenticates as the current user, or from the ambient Kerberos
     * ticket cache where one exists. */
    const char *username;
    const char *password;
    /** 0 selects 240000 ms, which MS-WSMV's appendix cites as typical. */
    uint32_t operation_timeout_ms;
    /** Zero is DEFAULT. */
    winrm_auth_t auth;
    /** Accept the server's TLS certificate without verifying it.
     *
     * Off by default, and it should stay off wherever the certificate can be
     * trusted properly, because it removes exactly what TLS was there to
     * provide: with this set the connection is still encrypted but the peer
     * is no longer authenticated, so anything able to intercept the route can
     * present its own certificate and be believed.
     *
     * It exists because the common WinRM deployment is a self-signed
     * certificate, and the alternative for a caller is installing that
     * certificate into the machine's trust store from outside this library.
     * Ignored for http:// endpoints, which have no certificate to check. */
    bool insecure_tls;
} winrm_config_t;

/** The resource this shell speaks. PSRP passes PowerShell's URI; another
 * caller could ask for cmd.exe's. NULL in the config means PowerShell's,
 * since that is what this library exists for. */
#define WINRM_RESOURCE_POWERSHELL \
    "http://schemas.microsoft.com/powershell/Microsoft.PowerShell"

psrp_result_t winrm_session_open(const winrm_config_t *cfg,
                                 winrm_session_t **out);
void winrm_session_free(winrm_session_t *s);

/** Which mechanism the established context actually used. Worth asking,
 * because SPNEGO's whole job is to pick one for you. */
winrm_auth_t winrm_negotiated_auth(const winrm_session_t *s);

/* ------------------------------------------------------ shell profile -- */

/** wxf:Create. `open_content` is carried verbatim as the request's open
 * content -- base64'd into a `<creationXml>` element -- and what it contains
 * is not this layer's concern. `shell_id` is the identifier to ask for; the
 * server may answer with a different one, and later requests use whatever it
 * reported. */
psrp_result_t winrm_shell_create(winrm_session_t *s, const char *shell_id,
                                 const void *open_content, size_t len);

/** wxf:Delete. */
psrp_result_t winrm_shell_delete(winrm_session_t *s);

/** wxf:Command. `command_line` is the Command element's text, which may be
 * empty; `arguments` becomes the Arguments element, base64'd. Anything that
 * does not fit in one request is the caller's to send afterwards with
 * winrm_send. */
psrp_result_t winrm_command(winrm_session_t *s, const char *command_id,
                            const char *command_line,
                            const void *arguments, size_t len);

/** wxf:Send on a named stream. WS-Management names the streams; MS-WSMV
 * defines "stdin" and "stdout", and the WinRM shell profile adds "pr". */
psrp_result_t winrm_send(winrm_session_t *s, const char *stream,
                         const void *data, size_t len);

/** wxf:Receive. Appends whatever has arrived on the output stream, waiting up
 * to `timeout_ms`. PSRP_ERR_TRUNCATED means nothing arrived in time, which is
 * an ordinary answer rather than a failure. */
psrp_result_t winrm_receive(winrm_session_t *s, psrp_buffer_t *out,
                            uint32_t timeout_ms);

/** The two signals WS-Management defines, for callers that want them by
 * name. `winrm_signal` takes any URI, because a plugin is free to define its
 * own and this layer has no way to know which ones mean anything. */
#define WINRM_SIGNAL_TERMINATE     "http://schemas.microsoft.com/wbem/wsman/1/windows/shell/signal/terminate"
#define WINRM_SIGNAL_CTRL_C     "http://schemas.microsoft.com/wbem/wsman/1/windows/shell/signal/ctrl_c"

/** wxf:Signal, targeted at the running command rather than the shell.
 *
 * `code` is the signal's URI and is the CALLER's choice. Which signals a
 * shell understands is a property of the plugin behind it, not of
 * WS-Management, so nothing here interprets it. */
psrp_result_t winrm_signal(winrm_session_t *s, const char *code);

/** True once the server has reported the command finished. */
bool winrm_command_done(const winrm_session_t *s);

/** Human-readable detail for the last failure; never NULL. */
const char *winrm_last_error(const winrm_session_t *s);

/* ------------------------------------- disconnected shells (MS-WSMV) --- */
/*
 * A disconnected shell keeps running on the server until its idle timeout
 * expires. The same client can reconnect to it, or another client can connect
 * to it after discovering its ShellId.
 */

/** wxf:Disconnect. `idle_timeout_ms` is how long the server keeps the shell
 * alive with nobody attached; 0 asks for the server's default. */
psrp_result_t winrm_disconnect(winrm_session_t *s, uint32_t idle_timeout_ms);

/** wxf:Reconnect, for the client that disconnected. */
psrp_result_t winrm_reconnect(winrm_session_t *s);

/** wxf:Connect, for a client adopting a shell it did not create. The response
 * carries open content, which is written to `response_content` when non-NULL;
 * what it means is the caller's business. */
psrp_result_t winrm_connect(winrm_session_t *s, const char *shell_id,
                            const void *open_content, size_t len,
                            psrp_buffer_t *response_content);

/** True while the shell is disconnected. */
bool winrm_is_disconnected(const winrm_session_t *s);

/* ------------------------------------------------ enumerating shells --- */
/**
 * WS-Enumerate over the shell resource, which is how a client finds shells it
 * did not create.
 *
 * Both backends implement it: the WSMan COM automation interface on Windows,
 * because the flat WSMan C API does not cover enumeration, and SOAP over
 * libcurl elsewhere. A record is parsed the same way either way.
 */
typedef struct winrm_shell_info {
    char *shell_id;
    char *name;
    char *owner;
    char *state;            /**< e.g. "Connected" or "Disconnected" */
    char *resource_uri;
} winrm_shell_info_t;

typedef struct winrm_enumerator winrm_enumerator_t;

psrp_result_t winrm_enumerator_open(const winrm_config_t *cfg,
                                    winrm_enumerator_t **out);
void winrm_enumerator_free(winrm_enumerator_t *e);

/** Lists the shells. Safe to call repeatedly on one enumerator. An empty list
 * is PSRP_OK with count 0. */
psrp_result_t winrm_enumerator_shells(winrm_enumerator_t *e,
                                      winrm_shell_info_t **out, size_t *count);

/** One-shot convenience: opens an enumerator, lists, closes. */
psrp_result_t winrm_enumerate_shells(const winrm_config_t *cfg,
                                     winrm_shell_info_t **out, size_t *count);

void winrm_shell_info_free(winrm_shell_info_t *s);
void winrm_shell_info_free_all(winrm_shell_info_t *list, size_t count);

/** Parses one shell element from an enumeration result. Exposed so the
 * parsing can be tested without a server. */
psrp_result_t winrm_parse_shell(const void *xml, size_t n,
                                winrm_shell_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_WINRM_CLIENT_H */
