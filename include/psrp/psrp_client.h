/** @file
 * psrp_client.h - the convenience layer: connect, run commands, read output.
 *
 * The rest of this library is a protocol implementation. It is deliberately
 * explicit: the session performs no I/O, so a caller moves bytes between it
 * and a transport and interprets an event queue. That is the right shape for
 * implementing PSRP and the wrong shape for running `Get-Date` on a server.
 *
 * This header is a layer on top of that, not a replacement for it. It is
 * written entirely against the public API below it and adds no new entry
 * points into the state machine, so anything it does can also be done without
 * it. What it buys you:
 *
 *   psrp_client_t *c;
 *   psrp_run_result_t r;
 *   psrp_client_connect(&cfg, &c);
 *   psrp_client_run(c, "Get-Process | Select-Object -First 3", &r);
 *   ... r.output holds three objects, r.errors holds any error text ...
 *   psrp_run_result_free(&r);
 *   psrp_client_free(c);
 *
 * One client holds one RunspacePool open, so several commands reuse a single
 * shell rather than paying for a new one each time. That, rather than the
 * line count, is the main reason to prefer this layer.
 *
 * Output stays as objects. PSRP goes to considerable trouble to preserve the
 * type of what a command emitted, and flattening that to a string at the
 * boundary would throw away most of what CLIXML is for; psrp_run_result_text
 * is there for when a string really is all you want. The error, warning,
 * verbose, debug and information streams are kept apart for the same reason.
 *
 * What this layer does NOT cover, by design: host callbacks, disconnect and
 * reconnect, pipeline input, secure strings, and command metadata. Those are
 * reachable through psrp_client_session and psrp_client_transport, which
 * expose the objects underneath so you can drop a level without rewriting
 * what you already have.
 */
#ifndef PSRP_CLIENT_H
#define PSRP_CLIENT_H

#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_client psrp_client_t;

typedef struct psrp_client_config {
    /** e.g. L"http://localhost:5985/wsman". NULL uses that default. */
    const wchar_t *connection;
    /** NULL authenticates as the current user via Negotiate. */
    const wchar_t *username;
    const wchar_t *password;
    /** WSMan operation timeout. 0 selects the transport's own default. */
    uint32_t operation_timeout_ms;
    /** How long psrp_client_connect waits for the pool. 0 means 30000. */
    uint32_t open_timeout_ms;
    /** How long psrp_client_run waits for a pipeline. 0 means 60000. */
    uint32_t run_timeout_ms;
} psrp_client_config_t;

/** One of the non-output streams, as text in arrival order. */
typedef struct psrp_stream {
    char **items;   /**< owned; `count` NUL-terminated strings */
    size_t count;
} psrp_stream_t;

/** What one command produced. Zeroed by psrp_client_run before it starts, so
 * it need not be initialised, and released with psrp_run_result_free. */
typedef struct psrp_run_result {
    /** A psrp_invocation_state_t. COMPLETED is success; anything else, and
     * for a failure the reason is usually the first entry in `errors`. */
    int32_t state;
    /** True when the command wrote to the error stream. Independent of
     * `state`: a command can emit errors and still complete. */
    bool had_errors;

    /** Objects the pipeline emitted, in order. Owned. */
    psrp_value_t *output;
    size_t output_count;

    /** Error text in arrival order. A terminating error -- `throw`, or an
     * uncaught exception -- appears here too, although PowerShell delivers it
     * as part of the message ending the pipeline rather than on the error
     * stream. */
    psrp_stream_t errors;
    psrp_stream_t warnings;
    psrp_stream_t verbose;
    psrp_stream_t debug;
    psrp_stream_t information;
} psrp_run_result_t;

/** Connects, and opens a RunspacePool. On failure `*out` is left NULL and the
 * reason is available from psrp_client_last_error only if a client was
 * created; a NULL `*out` means the transport itself could not be built. */
psrp_result_t psrp_client_connect(const psrp_client_config_t *cfg,
                                  psrp_client_t **out);

/** Closes the shell and releases everything. Safe on NULL. */
void psrp_client_free(psrp_client_t *c);

/** Runs `script` and waits for it to finish.
 *
 * `script` is treated as PowerShell script text rather than a bare command
 * name, so an expression like "2 + 2" works. Returns PSRP_OK when the
 * pipeline reached a terminal state, which includes finishing with errors --
 * check `out->state` and `out->had_errors` for what actually happened.
 * PSRP_ERR_TRUNCATED means the run timeout elapsed first. */
psrp_result_t psrp_client_run(psrp_client_t *c, const char *script,
                              psrp_run_result_t *out);

/** As psrp_client_run, for a command built with psrp_command_new so that
 * parameters can be passed as parameters rather than interpolated into
 * script text. The caller keeps ownership of `cmd`. */
psrp_result_t psrp_client_run_command(psrp_client_t *c, psrp_command_t *cmd,
                                      psrp_run_result_t *out);

/** Releases what psrp_client_run filled in and zeroes it. Safe on NULL and
 * safe to call twice. */
void psrp_run_result_free(psrp_run_result_t *r);

/** Appends every output object's text form to `out`, one per line. The
 * convenience path for when the object model is not what you wanted. */
psrp_result_t psrp_run_result_text(const psrp_run_result_t *r,
                                   psrp_buffer_t *out);

/** The last failure on this client, for logging. Never NULL. */
const char *psrp_client_last_error(const psrp_client_t *c);

/** The objects underneath, for anything this layer does not cover. Both
 * remain owned by the client. Mixing calls at the two levels is supported --
 * that is the point of exposing them -- but a caller that drives the
 * transport directly is responsible for keeping the session fed. */
psrp_session_t *psrp_client_session(psrp_client_t *c);
psrp_transport_t *psrp_client_transport(psrp_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_CLIENT_H */
