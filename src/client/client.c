/* client.c - the convenience layer over session + transport.
 *
 * Everything here is written against the public API. That is a deliberate
 * constraint rather than an accident of layering: if this file ever needed a
 * new entry point into the state machine, it would mean the low-level API had
 * a hole in it, and the right fix would be to close the hole rather than to
 * reach past it from up here.
 *
 * The interesting part is pump(). The session does no I/O and reads no clock,
 * so someone has to carry bytes in both directions, advance the key-exchange
 * timer, and turn the event queue into something a caller wants. Doing that
 * correctly is most of what a first-time caller gets wrong, and it is the
 * thing this layer exists to do once.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "psrp/psrp_client.h"

/* One poll of the transport. Short enough that a caller's timeout is honoured
 * with reasonable granularity, long enough not to spin. */
#define POLL_MS 250

#define DEFAULT_OPEN_MS 30000
#define DEFAULT_RUN_MS  60000

struct psrp_client {
    psrp_transport_t *t;
    psrp_session_t *s;
    uint32_t open_timeout_ms;
    uint32_t run_timeout_ms;
    char err[512];
};

static void set_err(psrp_client_t *c, const char *what, const char *detail)
{
    if (!c) return;
    if (detail && *detail)
        snprintf(c->err, sizeof c->err, "%s: %s", what, detail);
    else
        snprintf(c->err, sizeof c->err, "%s", what);
}

/* ------------------------------------------------------------- streams --- */

static psrp_result_t stream_push(psrp_stream_t *st, const char *text)
{
    char **grown;
    size_t n;
    char *copy;

    if (!text) text = "";
    n = strlen(text) + 1;

    copy = (char *)malloc(n);
    if (!copy) return PSRP_ERR_NOMEM;
    memcpy(copy, text, n);

    grown = (char **)realloc(st->items, (st->count + 1) * sizeof *grown);
    if (!grown) { free(copy); return PSRP_ERR_NOMEM; }

    st->items = grown;
    st->items[st->count++] = copy;
    return PSRP_OK;
}

static void stream_free(psrp_stream_t *st)
{
    size_t i;
    for (i = 0; i < st->count; i++) free(st->items[i]);
    free(st->items);
    st->items = NULL;
    st->count = 0;
}

/* Takes a copy of the event's value: the event is freed as soon as it has been
 * handled, and the result outlives the run that produced it. */
static psrp_result_t output_push(psrp_run_result_t *r, const psrp_value_t *v)
{
    psrp_value_t *grown;
    psrp_result_t rc;

    grown = (psrp_value_t *)realloc(r->output,
                                    (r->output_count + 1) * sizeof *grown);
    if (!grown) return PSRP_ERR_NOMEM;
    r->output = grown;

    memset(&r->output[r->output_count], 0, sizeof r->output[0]);
    rc = psrp_value_clone(v, &r->output[r->output_count]);
    if (rc != PSRP_OK) return rc;

    r->output_count++;
    return PSRP_OK;
}

void psrp_run_result_free(psrp_run_result_t *r)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < r->output_count; i++) psrp_value_free(&r->output[i]);
    free(r->output);
    stream_free(&r->errors);
    stream_free(&r->warnings);
    stream_free(&r->verbose);
    stream_free(&r->debug);
    stream_free(&r->information);
    memset(r, 0, sizeof *r);
}

psrp_result_t psrp_run_result_text(const psrp_run_result_t *r,
                                   psrp_buffer_t *out)
{
    size_t i;
    psrp_result_t rc;

    if (!r || !out) return PSRP_ERR_INVALID_ARG;

    for (i = 0; i < r->output_count; i++) {
        rc = psrp_value_to_text(&r->output[i], out);
        if (rc != PSRP_OK) return rc;
        rc = psrp_buffer_append_u8(out, '\n');
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}

/* ---------------------------------------------------------------- pump --- */

/* Drives the conversation until an event of `want` arrives, collecting
 * anything a run result cares about on the way.
 *
 * `want_state` receives that event's state field. `r` may be NULL, for the
 * open path where there is no result to fill in yet. Returns PSRP_OK when the
 * awaited event arrived and PSRP_ERR_TRUNCATED when `timeout_ms` elapsed
 * first; a transport failure is reported as itself.
 */
static psrp_result_t pump(psrp_client_t *c, psrp_event_kind_t want,
                          psrp_run_result_t *r, uint32_t timeout_ms,
                          int32_t *want_state)
{
    uint32_t waited = 0;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        psrp_result_t rc;
        bool got = false;

        while (psrp_session_next_event(c->s, &e) == PSRP_OK) {
            psrp_event_kind_t kind = e.kind;
            int32_t state = e.state;
            psrp_result_t prc = PSRP_OK;

            if (r) {
                switch (kind) {
                case PSRP_EVENT_PIPELINE_OUTPUT:
                    prc = output_push(r, &e.value);
                    break;
                case PSRP_EVENT_ERROR_RECORD:
                    r->had_errors = true;
                    prc = stream_push(&r->errors, e.text);
                    break;
                case PSRP_EVENT_WARNING_RECORD:
                    prc = stream_push(&r->warnings, e.text);
                    break;
                case PSRP_EVENT_VERBOSE_RECORD:
                    prc = stream_push(&r->verbose, e.text);
                    break;
                case PSRP_EVENT_DEBUG_RECORD:
                    prc = stream_push(&r->debug, e.text);
                    break;
                case PSRP_EVENT_INFORMATION_RECORD:
                    prc = stream_push(&r->information, e.text);
                    break;
                case PSRP_EVENT_PIPELINE_STATE:
                    /* A terminating error -- `throw`, or an exception -- never
                     * reaches the error stream. It arrives as the
                     * ExceptionAsErrorRecord of the state message that ends
                     * the pipeline (2.2.2.21), which the session hands over as
                     * this event's text. Without this the caller would see
                     * FAILED with no reason anywhere. */
                    if (e.text) {
                        r->had_errors = true;
                        prc = stream_push(&r->errors, e.text);
                    }
                    break;
                default:
                    break;
                }
            }

            psrp_event_free(&e);

            if (prc != PSRP_OK) {
                set_err(c, "collecting output", psrp_strerror(prc));
                return prc;
            }

            if (kind == want) {
                if (want_state) *want_state = state;
                got = true;
                break;
            }
        }
        if (got) return PSRP_OK;

        /* Anything the session queued has to go out before waiting for more:
         * a reply the server is blocked on would otherwise deadlock us
         * against our own timeout. */
        psrp_buffer_init(&chunk);
        if (psrp_session_take_output(c->s, &chunk) == PSRP_OK && chunk.len) {
            rc = psrp_transport_send(c->t, chunk.data, chunk.len);
            if (rc != PSRP_OK) {
                psrp_buffer_free(&chunk);
                set_err(c, "send", psrp_transport_last_error(c->t));
                return rc;
            }
        }
        psrp_buffer_reset(&chunk);
        if (psrp_session_take_priority_output(c->s, &chunk) == PSRP_OK &&
            chunk.len) {
            rc = psrp_transport_send_priority(c->t, chunk.data, chunk.len);
            if (rc != PSRP_OK) {
                psrp_buffer_free(&chunk);
                set_err(c, "send priority", psrp_transport_last_error(c->t));
                return rc;
            }
        }
        psrp_buffer_free(&chunk);

        if (waited >= timeout_ms) {
            set_err(c, "timed out waiting for the server", NULL);
            return PSRP_ERR_TRUNCATED;
        }

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(c->t, &chunk, POLL_MS);
        if (rc == PSRP_OK && chunk.len)
            (void)psrp_session_receive(c->s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);

        /* PSRP_ERR_TRUNCATED from the transport means "nothing yet", which is
         * an ordinary answer; anything else is a real failure and waiting
         * longer will not help. */
        if (rc != PSRP_OK && rc != PSRP_ERR_TRUNCATED) {
            set_err(c, "receive", psrp_transport_last_error(c->t));
            return rc;
        }

        /* The session reads no clock of its own (3.1.2). */
        (void)psrp_session_tick(c->s, POLL_MS);
        waited += POLL_MS;
    }
}

/* ------------------------------------------------------------ lifecycle --- */

psrp_result_t psrp_client_connect(const psrp_client_config_t *cfg,
                                  psrp_client_t **out)
{
    psrp_wsman_config_t wcfg;
    psrp_client_t *c;
    psrp_buffer_t payload;
    psrp_result_t rc;
    int32_t state = -1;

    if (!cfg || !out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    c = (psrp_client_t *)calloc(1, sizeof *c);
    if (!c) return PSRP_ERR_NOMEM;

    c->open_timeout_ms = cfg->open_timeout_ms ? cfg->open_timeout_ms
                                              : DEFAULT_OPEN_MS;
    c->run_timeout_ms = cfg->run_timeout_ms ? cfg->run_timeout_ms
                                            : DEFAULT_RUN_MS;

    memset(&wcfg, 0, sizeof wcfg);
    wcfg.connection = cfg->connection;
    wcfg.username = cfg->username;
    wcfg.password = cfg->password;
    wcfg.operation_timeout_ms = cfg->operation_timeout_ms;

    rc = psrp_wsman_transport_create(&wcfg, &c->t);
    if (rc != PSRP_OK) {
        set_err(c, "connect", psrp_transport_last_error(c->t));
        psrp_transport_free(c->t);
        free(c);
        return rc;
    }

    c->s = psrp_session_new();
    if (!c->s) { psrp_transport_free(c->t); free(c); return PSRP_ERR_NOMEM; }

    psrp_buffer_init(&payload);
    rc = psrp_session_open_payload(c->s, &payload);
    if (rc != PSRP_OK) {
        set_err(c, "open payload", psrp_strerror(rc));
        goto fail;
    }

    /* SESSION_CAPABILITY and INIT_RUNSPACEPOOL both ride inside the shell
     * create rather than costing round trips of their own. */
    rc = psrp_transport_open(c->t, psrp_session_pool_id(c->s),
                             payload.data, payload.len);
    if (rc != PSRP_OK) {
        set_err(c, "open shell", psrp_transport_last_error(c->t));
        goto fail;
    }

    /* The pool passes through intermediate states before Opened, so keep
     * waiting until it is either open or provably never going to be. */
    do {
        rc = pump(c, PSRP_EVENT_POOL_STATE, NULL, c->open_timeout_ms, &state);
        if (rc != PSRP_OK) goto fail;
    } while (state != PSRP_RUNSPACE_OPENED &&
             !psrp_runspace_pool_state_is_terminal(state));

    if (state != PSRP_RUNSPACE_OPENED) {
        set_err(c, "pool did not open",
                psrp_runspace_pool_state_name(state));
        rc = PSRP_ERR_STATE;
        goto fail;
    }

    psrp_buffer_free(&payload);
    *out = c;
    return PSRP_OK;

fail:
    psrp_buffer_free(&payload);
    /* Hand the client back even on failure so the caller can read the error,
     * then free it -- but only through *out, which stays NULL here, so free
     * it ourselves and copy the message nowhere. The transport close is what
     * matters: an abandoned shell holds a runspace until its idle timeout. */
    (void)psrp_transport_close_shell(c->t);
    psrp_session_free(c->s);
    psrp_transport_free(c->t);
    free(c);
    return rc;
}

void psrp_client_free(psrp_client_t *c)
{
    if (!c) return;
    (void)psrp_transport_close_shell(c->t);
    psrp_session_free(c->s);
    psrp_transport_free(c->t);
    free(c);
}

/* ----------------------------------------------------------------- run --- */

psrp_result_t psrp_client_run_command(psrp_client_t *c, psrp_command_t *cmd,
                                      psrp_run_result_t *out)
{
    psrp_buffer_t payload;
    psrp_guid_t pipeline_id;
    psrp_result_t rc;
    int32_t state = -1;

    if (!c || !cmd || !out) return PSRP_ERR_INVALID_ARG;

    memset(out, 0, sizeof *out);
    out->state = PSRP_INVOCATION_NOT_STARTED;

    psrp_buffer_init(&payload);
    rc = psrp_session_pipeline_payload(c->s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                       &pipeline_id, &payload);
    if (rc != PSRP_OK) {
        set_err(c, "pipeline payload", psrp_strerror(rc));
        psrp_buffer_free(&payload);
        return rc;
    }

    rc = psrp_transport_run_command(c->t, &pipeline_id,
                                    payload.data, payload.len);
    psrp_buffer_free(&payload);
    if (rc != PSRP_OK) {
        set_err(c, "run command", psrp_transport_last_error(c->t));
        return rc;
    }

    /* Output and records arrive as events while the pipeline runs; pump
     * collects them, so by the time a terminal state shows up the result is
     * already complete. */
    do {
        rc = pump(c, PSRP_EVENT_PIPELINE_STATE, out, c->run_timeout_ms,
                  &state);
        if (rc != PSRP_OK) {
            out->state = state;
            return rc;
        }
    } while (!psrp_invocation_state_is_terminal(state));

    out->state = state;
    return PSRP_OK;
}

psrp_result_t psrp_client_run(psrp_client_t *c, const char *script,
                              psrp_run_result_t *out)
{
    psrp_command_t *cmd;
    psrp_result_t rc;

    if (!c || !script || !out) return PSRP_ERR_INVALID_ARG;

    /* true: script text rather than a bare command name, which is what makes
     * an expression like "2 + 2" work. */
    cmd = psrp_command_new(script, true);
    if (!cmd) return PSRP_ERR_NOMEM;

    rc = psrp_client_run_command(c, cmd, out);
    psrp_command_free(cmd);
    return rc;
}

/* --------------------------------------------------------------- misc --- */

const char *psrp_client_last_error(const psrp_client_t *c)
{
    if (!c) return "no client";
    return c->err[0] ? c->err : "no error";
}

psrp_session_t *psrp_client_session(psrp_client_t *c)
{
    return c ? c->s : NULL;
}

psrp_transport_t *psrp_client_transport(psrp_client_t *c)
{
    return c ? c->t : NULL;
}
