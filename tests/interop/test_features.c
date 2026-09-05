/* Everything that was implemented and unit-tested but never spoken to a real
 * server.
 *
 * Unit tests prove an encoding matches what the specification describes. They
 * cannot prove a server accepts it, or that the feature is reachable through
 * the public API at all. Pipeline input passed its unit tests for weeks while
 * being impossible to use, because the pipeline was always created with
 * NoInput=true and the server discarded everything sent (TODO PSRP-16). That
 * class of defect is invisible without a live server.
 *
 * Each section here drives one feature end to end and says what it saw. Run a
 * single section by name to isolate a failure:
 *
 *   test_features            all sections
 *   test_features streams    just that one
 *
 * Opt-in: PSRP_INTEROP=1, with PSRP_USER / PSRP_PASS.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"
#include "psrp/psrp_records.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_metadata.h"
#include "psrp/psrp_crypto.h"

static wchar_t *widen(const char *s)
{
    size_t n;
    wchar_t *w;
    if (!s) return NULL;
    n = strlen(s) + 1;
    w = (wchar_t *)calloc(n, sizeof *w);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, (int)n);
    return w;
}

/* What one section observed, so an assertion can name what was missing. */
typedef struct {
    int output;
    int error_record;
    int verbose;
    int warning;
    int debug;
    int progress;
    int information;
    int host_call;
    int user_event;
    int app_private_data;
    int availability;
    int session_key_ready;
    int public_key_requested;
    int64_t last_availability_count;
    int last_availability_known;
    int host_answered;           /* value-returning host calls we replied to */
    int pool_init_data;          /* RUNSPACEPOOL_INIT_DATA replies to a connect */
    char first_error[256];
    char output_text[2048];      /* every PIPELINE_OUTPUT rendered, joined */
    char last_ss_b64[1024];      /* last SecureString output, still encrypted */
} seen_t;

static void flush_out(psrp_session_t *s, psrp_transport_t *t)
{
    psrp_buffer_t wire;
    psrp_buffer_init(&wire);
    if (psrp_session_take_output(s, &wire) == PSRP_OK && wire.len)
        (void)psrp_transport_send(t, wire.data, wire.len);
    psrp_buffer_free(&wire);
    psrp_buffer_init(&wire);
    if (psrp_session_take_priority_output(s, &wire) == PSRP_OK && wire.len)
        (void)psrp_transport_send_priority(t, wire.data, wire.len);
    psrp_buffer_free(&wire);
}

/* Drains events into `seen`, answering host calls as they arrive. Returns the
 * last pipeline state observed, or -1. */
static int drain(psrp_session_t *s, psrp_transport_t *t, seen_t *seen)
{
    psrp_event_t e;
    int pipeline_state = -1;

    while (psrp_session_next_event(s, &e) == PSRP_OK) {
        switch (e.kind) {
        case PSRP_EVENT_PIPELINE_OUTPUT: {
            psrp_buffer_t text;
            size_t used = strlen(seen->output_text);
            seen->output++;
            /* A SecureString is kept as it arrived, still encrypted, so the
             * crypto section can decrypt it and check the plaintext rather
             * than merely noting that something came back. */
            if (e.value.kind == PSRP_VAL_SECURESTRING &&
                e.value.as.text.len < sizeof seen->last_ss_b64) {
                memcpy(seen->last_ss_b64, e.value.as.text.ptr,
                       e.value.as.text.len);
                seen->last_ss_b64[e.value.as.text.len] = '\0';
            }
            psrp_buffer_init(&text);
            if (psrp_value_to_text(&e.value, &text) == PSRP_OK &&
                used + text.len + 2 < sizeof seen->output_text) {
                memcpy(seen->output_text + used, text.data, text.len);
                seen->output_text[used + text.len] = '|';
                seen->output_text[used + text.len + 1] = '\0';
            }
            psrp_buffer_free(&text);
            break;
        }
        case PSRP_EVENT_VERBOSE_RECORD:       seen->verbose++; break;
        case PSRP_EVENT_WARNING_RECORD:       seen->warning++; break;
        case PSRP_EVENT_DEBUG_RECORD:         seen->debug++; break;
        case PSRP_EVENT_PROGRESS_RECORD:      seen->progress++; break;
        case PSRP_EVENT_INFORMATION_RECORD:   seen->information++; break;
        case PSRP_EVENT_USER_EVENT:           seen->user_event++; break;
        case PSRP_EVENT_APPLICATION_PRIVATE_DATA: seen->app_private_data++; break;
        case PSRP_EVENT_POOL_INIT_DATA:       seen->pool_init_data++; break;
        case PSRP_EVENT_SESSION_KEY_READY:    seen->session_key_ready++; break;
        case PSRP_EVENT_PUBLIC_KEY_REQUESTED: seen->public_key_requested++; break;
        case PSRP_EVENT_RUNSPACE_AVAILABILITY:
            seen->availability++;
            seen->last_availability_count = e.count;
            seen->last_availability_known = e.state;
            break;
        case PSRP_EVENT_ERROR_RECORD:
            seen->error_record++;
            if (e.text && seen->first_error[0] == '\0')
                snprintf(seen->first_error, sizeof seen->first_error, "%s",
                         e.text);
            break;
        case PSRP_EVENT_HOST_CALL:
            seen->host_call++;
            /* A method with a return value must be answered or the pipeline
             * waits forever; one without must not be. The answers are chosen
             * so a script can echo them back and prove they arrived. */
            if (psrp_host_method_returns_value(e.state)) {
                psrp_value_t reply;
                psrp_value_init(&reply);
                /* A return value is encoded per 2.2.6, and 2.2.6.1.1 says a
                 * plainly serializable type is not encoded at all. A bare
                 * string is what the server casts to System.String; a
                 * wrapped one fails that cast. */
                if (e.state == PSRP_HOST_READ_LINE)
                    (void)psrp_value_set_string(&reply, "typed-by-client");
                else if (e.state == PSRP_HOST_GET_BUFFER_SIZE)
                    (void)psrp_host_make_size(120, 3000, &reply);
                else
                    psrp_value_set_null(&reply);
                if (psrp_session_respond_to_host_call(
                        s, psrp_guid_is_empty(&e.pipeline_id) ? NULL
                                                              : &e.pipeline_id,
                        e.call_id, e.state, &reply, NULL) == PSRP_OK)
                    seen->host_answered++;
                psrp_value_free(&reply);
                flush_out(s, t);
            }
            break;
        case PSRP_EVENT_PIPELINE_STATE:
            pipeline_state = e.state;
            break;
        default:
            break;
        }
        psrp_event_free(&e);
    }
    return pipeline_state;
}

/* Pumps the wire into the session until `ms` elapse or a terminal pipeline
 * state arrives. Returns that state, or -1. */
static int pump(psrp_session_t *s, psrp_transport_t *t, seen_t *seen, int ms)
{
    int waited = 0;
    int state = -1;

    for (;;) {
        psrp_buffer_t chunk;
        int got = drain(s, t, seen);
        if (got >= 0) state = got;
        if (state >= 0 && psrp_invocation_state_is_terminal(state)) return state;
        if (waited >= ms) return state;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
}

/* Opens a pool. `with_host` declares a client host, which is what makes the
 * server route Write-Host and prompts back to us. The caller frees the
 * session. */
static psrp_session_t *open_pool_ex(psrp_transport_t *t, seen_t *seen,
                                    bool with_host)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t payload;
    int waited = 0;

    if (!s) return NULL;
    if (with_host && psrp_session_provide_host(s, NULL) != PSRP_OK) {
        psrp_session_free(s);
        return NULL;
    }
    psrp_buffer_init(&payload);
    if (psrp_session_open_payload(s, &payload) != PSRP_OK) goto fail;
    if (psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                            payload.len) != PSRP_OK) {
        printf("    open: %s\n", psrp_transport_last_error(t));
        goto fail;
    }
    psrp_buffer_free(&payload);

    while (psrp_session_pool_state(s) != PSRP_RUNSPACE_OPENED && waited < 30000) {
        psrp_buffer_t chunk;
        (void)drain(s, t, seen);
        if (psrp_session_pool_state(s) == PSRP_RUNSPACE_OPENED) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
    if (psrp_session_pool_state(s) != PSRP_RUNSPACE_OPENED) {
        printf("    pool never opened\n");
        goto fail;
    }
    return s;

fail:
    psrp_buffer_free(&payload);
    psrp_session_free(s);
    return NULL;
}

static psrp_session_t *open_pool(psrp_transport_t *t, seen_t *seen)
{
    return open_pool_ex(t, seen, false);
}

/* Prints the printable text of a payload, for eyeballing what went out. */
static void dump_payload(const psrp_buffer_t *b)
{
    size_t i;
    printf("  --- payload ---\n  ");
    for (i = 0; i < b->len; i++) {
        unsigned char c = b->data[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    printf("\n  --- end ---\n");
}

/* Runs one script to completion. Returns the pipeline state. */
static int run_script(psrp_session_t *s, psrp_transport_t *t, const char *script,
                      seen_t *seen)
{
    psrp_command_t *cmd = psrp_command_new(script, true);
    psrp_buffer_t payload;
    psrp_guid_t pid;
    int state = -1;

    if (!cmd) return -1;
    psrp_buffer_init(&payload);
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pid, &payload) == PSRP_OK &&
        (getenv("PSRP_DUMP") == NULL || (dump_payload(&payload), 1)) &&
        psrp_transport_run_command(t, &pid, payload.data, payload.len)
            == PSRP_OK) {
        do {
            state = pump(s, t, seen, 60000);
        } while (state >= 0 && !psrp_invocation_state_is_terminal(state));
    } else {
        printf("    run: %s\n", psrp_transport_last_error(t));
    }
    psrp_buffer_free(&payload);
    psrp_command_free(cmd);
    return state;
}

/* --------------------------------------------------------------- streams -- */

static int section_streams(psrp_transport_t *t)
{
    /* Each stream is written once. Preferences are set inline because the
     * defaults suppress verbose and debug entirely, and a suppressed stream
     * would look exactly like a stream we failed to decode. */
    static const char kScript[] =
        "$VerbosePreference='Continue'; $DebugPreference='Continue'; "
        "$WarningPreference='Continue'; "
        "Write-Output 'out'; "
        "Write-Verbose 'v'; "
        "Write-Warning 'w'; "
        "Write-Debug 'd'; "
        "Write-Progress -Activity 'act' -Status 'st' -PercentComplete 42; "
        "Write-Information 'i' -InformationAction Continue; "
        "Write-Error 'e'";
    psrp_session_t *s;
    seen_t seen;
    int state, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    state = run_script(s, t, kScript, &seen);
    printf("  output=%d error=%d verbose=%d warning=%d debug=%d progress=%d "
           "information=%d\n", seen.output, seen.error_record, seen.verbose,
           seen.warning, seen.debug, seen.progress, seen.information);
    if (seen.first_error[0]) printf("  first error record: %s\n",
                                    seen.first_error);

    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    if (seen.output < 1)      { printf("  FAIL: no output\n"); bad = 1; }
    if (seen.error_record < 1) { printf("  FAIL: no error record\n"); bad = 1; }
    if (seen.verbose < 1)     { printf("  FAIL: no verbose record\n"); bad = 1; }
    if (seen.warning < 1)     { printf("  FAIL: no warning record\n"); bad = 1; }
    if (seen.debug < 1)       { printf("  FAIL: no debug record\n"); bad = 1; }
    if (seen.progress < 1)    { printf("  FAIL: no progress record\n"); bad = 1; }
    if (seen.information < 1) { printf("  FAIL: no information record\n"); bad = 1; }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------------------------- runspaces -- */

static int section_runspaces(psrp_transport_t *t)
{
    psrp_session_t *s;
    seen_t seen;
    int64_t ci = 0;
    int bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    /* Each of these mints a call identifier the server must quote back. */
    if (psrp_session_get_available_runspaces(s, &ci) != PSRP_OK) {
        printf("  FAIL: get_available_runspaces refused\n");
        bad = 1;
    } else {
        flush_out(s, t);
        (void)pump(s, t, &seen, 15000);
        printf("  get_available: replies=%d count=%lld recognised=%d\n",
               seen.availability, (long long)seen.last_availability_count,
               seen.last_availability_known);
        if (seen.availability < 1) {
            printf("  FAIL: no RUNSPACE_AVAILABILITY for get_available\n");
            bad = 1;
        } else if (!seen.last_availability_known) {
            printf("  FAIL: server quoted a call id we never sent\n");
            bad = 1;
        }
    }

    seen.availability = 0;
    if (psrp_session_set_max_runspaces(s, 2, &ci) != PSRP_OK) {
        printf("  FAIL: set_max_runspaces refused\n");
        bad = 1;
    } else {
        flush_out(s, t);
        (void)pump(s, t, &seen, 15000);
        printf("  set_max: replies=%d recognised=%d\n", seen.availability,
               seen.last_availability_known);
        if (seen.availability < 1) {
            printf("  FAIL: no reply to set_max_runspaces\n");
            bad = 1;
        }
    }

    seen.availability = 0;
    if (psrp_session_set_min_runspaces(s, 1, &ci) != PSRP_OK) {
        printf("  FAIL: set_min_runspaces refused\n");
        bad = 1;
    } else {
        flush_out(s, t);
        (void)pump(s, t, &seen, 15000);
        printf("  set_min: replies=%d\n", seen.availability);
        if (seen.availability < 1) {
            printf("  FAIL: no reply to set_min_runspaces\n");
            bad = 1;
        }
    }

    /* Every call identifier must have been retired by now. */
    if (psrp_session_pending_call_count(s) != 0) {
        printf("  FAIL: %zu call identifiers still outstanding\n",
               psrp_session_pending_call_count(s));
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------------------------ host calls -- */

static int section_hostcall(psrp_transport_t *t)
{
    /* Write-Host does not reach the host any more: since PowerShell 5.0 it
     * writes to the information stream instead, which is why it produced no
     * host call at all. $Host.UI.WriteLine goes to the host proper. */
    static const char kScript[] =
        "$Host.UI.WriteLine('from the host'); 'done'";
    psrp_session_t *s;
    seen_t seen;
    int state, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool_ex(t, &seen, true);   /* declare a host to be called back */
    if (!s) return 1;

    state = run_script(s, t, kScript, &seen);
    printf("  host_calls=%d output=%d state=%d\n", seen.host_call, seen.output,
           state);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);

    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    if (seen.host_call < 1) {
        printf("  FAIL: Write-Host produced no host call\n");
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ---------------------------------------------------------------- crypto -- */

static int section_crypto(psrp_transport_t *t)
{
    /* 3.1.4.8: the higher layer may start a key exchange at any time while the
     * pool is Opened. That is the path a client controls, so it is the one
     * tested here.
     *
     * Asking the server to hand back a SecureString does not start an exchange
     * on its own, which is worth knowing: a server with no session key
     * serializes the value as an empty Secure String rather than requesting a
     * key. So a client that wants to read protected values has to ask first. */
    psrp_session_t *s;
    seen_t seen;
    int bad = 0;
    int waited = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    if (psrp_session_start_key_exchange(s) != PSRP_OK) {
        printf("  FAIL: start_key_exchange refused\n");
        psrp_session_free(s);
        return 1;
    }
    flush_out(s, t);

    while (!psrp_session_has_session_key(s) && waited < 30000) {
        psrp_buffer_t chunk;
        (void)drain(s, t, &seen);
        if (psrp_session_has_session_key(s)) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
    /* The loop exits as soon as the key is installed, and that happens inside
     * psrp_session_receive, before the event announcing it has been drained.
     * Draining once more keeps the count honest rather than reporting a zero
     * that looks like a missing event. */
    (void)drain(s, t, &seen);

    printf("  key_ready=%d have_key=%d\n", seen.session_key_ready,
           (int)psrp_session_has_session_key(s));

    if (!psrp_session_has_session_key(s)) {
        printf("  FAIL: server never returned an encrypted session key\n");
        bad = 1;
    } else {
        /* The key is only useful if it decrypts what the server encrypts with
         * it, so round-trip a protected value the whole way. */
        static const char kScript[] =
            "ConvertTo-SecureString 'hunter2' -AsPlainText -Force";
        int state = run_script(s, t, kScript, &seen);
        printf("  secure string: output=%d state=%d\n", seen.output, state);
        if (state != PSRP_INVOCATION_COMPLETED) {
            printf("  FAIL: pipeline state %d\n", state);
            bad = 1;
        }
        if (seen.output < 1) {
            printf("  FAIL: no SecureString came back\n");
            bad = 1;
        } else {
            /* A value arriving is not the same as the key working. Decrypt
             * it: the server encrypted "hunter2" under the key it sent us,
             * so reading that back is the proof both halves agree. */
            psrp_value_t ss;
            psrp_buffer_t plain;
            psrp_value_init(&ss);
            psrp_buffer_init(&plain);
            if (psrp_value_set_text(&ss, PSRP_VAL_SECURESTRING,
                                    seen.last_ss_b64,
                                    strlen(seen.last_ss_b64)) != PSRP_OK ||
                psrp_crypto_unprotect_value(psrp_session_crypto(s), &ss,
                                            &plain) != PSRP_OK) {
                printf("  FAIL: could not decrypt the returned SecureString\n");
                bad = 1;
            } else if (plain.len != 7 || memcmp(plain.data, "hunter2", 7) != 0) {
                printf("  FAIL: decrypted to %zu bytes, not \"hunter2\"\n",
                       plain.len);
                bad = 1;
            } else {
                printf("  decrypted the server's SecureString: hunter2\n");
            }
            psrp_buffer_free(&plain);
            psrp_value_free(&ss);
        }
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* -------------------------------------------------------------- metadata -- */

static int section_metadata(psrp_transport_t *t)
{
    /* 3.1.5.4.14. The reply is a pipeline output stream: one count object,
     * then one CommandMetadata per command.
     *
     * Two rounds, because they exercise different server paths. An exact name
     * must return exactly that command; "*" must return the whole catalogue
     * and its count must agree with how many objects actually arrived.
     *
     * Note what is not asserted: mid-word wildcards. Measured against this
     * server, "*" returns 1166 commands and "Get-ChildItem" returns 1, but
     * "Get-Ch*" and "*ChildItem*" return none. That is the server filtering on
     * the Namespace we send, which 2.2.2.14 defines Null to mean a list
     * holding one empty string. Our encoding is faithful; the library simply
     * offers no way to send a different namespace yet (TODO PSRP-17). */
    const char *pat = getenv("PSRP_META_PATTERN");
    const char *ns_env = getenv("PSRP_META_NAMESPACE");
    const char *patterns[1];
    const char *namespaces[1];
    size_t ns_count = 0;
    psrp_session_t *s;
    seen_t seen;
    psrp_buffer_t payload;
    psrp_guid_t pid;
    int bad = 0, waited = 0;
    int32_t declared = -1;
    int described = 0;
    bool have_count = false;
    char first_name[128];

    if (!pat) pat = "Get-ChildItem";
    patterns[0] = pat;
    if (ns_env && ns_env[0]) { namespaces[0] = ns_env; ns_count = 1; }
    first_name[0] = '\0';
    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    psrp_buffer_init(&payload);
    if (psrp_session_command_metadata_payload(s, patterns, 1,
                                              PSRP_COMMAND_TYPE_ALL,
                                              ns_count ? namespaces : NULL,
                                              ns_count,
                                              &pid, &payload) != PSRP_OK) {
        printf("  FAIL: command_metadata_payload refused\n");
        psrp_buffer_free(&payload);
        psrp_session_free(s);
        return 1;
    }
    if (psrp_transport_run_command(t, &pid, payload.data, payload.len)
        != PSRP_OK) {
        printf("  FAIL: run: %s\n", psrp_transport_last_error(t));
        psrp_buffer_free(&payload);
        psrp_session_free(s);
        return 1;
    }
    psrp_buffer_free(&payload);

    /* Read the output stream directly, since the objects have to be told
     * apart by shape: the first is the count, the rest are the commands. */
    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        bool finished = false;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_PIPELINE_OUTPUT) {
                int32_t n = -1;
                psrp_command_metadata_t m;
                if (!have_count &&
                    psrp_command_metadata_count_from_value(&e.value, &n)
                        == PSRP_OK) {
                    declared = n;
                    have_count = true;
                } else if (psrp_command_metadata_from_value(&e.value, &m)
                               == PSRP_OK) {
                    if (first_name[0] == '\0')
                        snprintf(first_name, sizeof first_name, "%s", m.name);
                    described++;
                    psrp_command_metadata_free(&m);
                }
            } else if (e.kind == PSRP_EVENT_ERROR_RECORD && e.text &&
                       seen.first_error[0] == '\0') {
                snprintf(seen.first_error, sizeof seen.first_error, "%s",
                         e.text);
            } else if (e.kind == PSRP_EVENT_PIPELINE_STATE &&
                       psrp_invocation_state_is_terminal(e.state)) {
                finished = true;
            }
            psrp_event_free(&e);
        }
        if (finished || waited >= 60000) break;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }

    printf("  declared=%ld described=%d first=%s\n", (long)declared, described,
           first_name[0] ? first_name : "<none>");
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);

    if (!have_count) {
        printf("  FAIL: no CommandMetadataCount object\n");
        bad = 1;
    }
    if (described < 1) {
        printf("  FAIL: no CommandMetadata objects\n");
        bad = 1;
    }
    if (have_count && declared != described) {
        printf("  FAIL: count said %ld but %d were described\n",
               (long)declared, described);
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------- host calls with a return ---- */

static int section_hostread(psrp_transport_t *t)
{
    /* WriteLine proved the server calls us. It does not prove we can answer:
     * a method with a return value blocks the pipeline until the response
     * arrives, and psrp_session_respond_to_host_call had never been run
     * against a server. ReadLine returns a string and GetBufferSize returns a
     * Size, so between them a primitive and a complex reply are covered. */
    static const char kScript[] =
        "$line = $Host.UI.ReadLine(); "
        "$size = $Host.UI.RawUI.BufferSize; "
        "\"line:$line\"; \"width:$($size.Width)\"";
    psrp_session_t *s;
    seen_t seen;
    int state, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool_ex(t, &seen, true);
    if (!s) return 1;

    state = run_script(s, t, kScript, &seen);
    printf("  host_calls=%d answered=%d output=%d state=%d\n", seen.host_call,
           seen.host_answered, seen.output, state);
    printf("  output text: %s\n", seen.output_text);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);

    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    /* ReadLine must come to us and be answered; the script echoes the reply,
     * so that is the round trip. BufferSize does not produce a call at all:
     * the server answers it from the _hostDefaultData sent in HostInfo, which
     * is what "width:120" proves. Both paths are checked, and only the one
     * that genuinely calls back is required to have been answered. */
    if (seen.host_answered < 1) {
        printf("  FAIL: ReadLine was never answered\n");
        bad = 1;
    }
    if (!strstr(seen.output_text, "line:typed-by-client")) {
        printf("  FAIL: ReadLine reply did not reach the script\n");
        bad = 1;
    }
    if (!strstr(seen.output_text, "width:120")) {
        printf("  FAIL: the console data sent in HostInfo did not take\n");
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* --------------------------------------------------------- stop pipeline -- */

static int section_stop(psrp_transport_t *t)
{
    /* 3.1.4.4. A pipeline that would run for a minute is stopped after a
     * moment; it must report Stopped, and the marker after the sleep must
     * never appear. */
    static const char kScript[] = "Start-Sleep -Seconds 60; 'never'";
    psrp_session_t *s;
    psrp_command_t *cmd;
    psrp_buffer_t payload;
    psrp_guid_t pid;
    seen_t seen;
    int state = -1, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    cmd = psrp_command_new(kScript, true);
    psrp_buffer_init(&payload);
    if (!cmd ||
        psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pid, &payload) != PSRP_OK ||
        psrp_transport_run_command(t, &pid, payload.data, payload.len)
            != PSRP_OK) {
        printf("  FAIL: could not start the pipeline: %s\n",
               psrp_transport_last_error(t));
        bad = 1;
        goto done;
    }

    /* Let it get going, then pull the plug. */
    (void)pump(s, t, &seen, 1500);
    if (psrp_transport_stop_pipeline(t) != PSRP_OK) {
        printf("  FAIL: stop: %s\n", psrp_transport_last_error(t));
        bad = 1;
        goto done;
    }
    do {
        state = pump(s, t, &seen, 30000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    printf("  state=%d (%s) output=%d\n", state,
           psrp_invocation_state_name(state), seen.output);
    if (state != PSRP_INVOCATION_STOPPED) {
        printf("  FAIL: expected Stopped\n");
        bad = 1;
    }
    if (strstr(seen.output_text, "never")) {
        printf("  FAIL: the pipeline ran to completion anyway\n");
        bad = 1;
    }

done:
    psrp_buffer_free(&payload);
    psrp_command_free(cmd);
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ---------------------------------------- several commands, parameters --- */

static int section_multicommand(psrp_transport_t *t)
{
    /* Two commands in one pipeline, the first with typed parameters: an
     * integer and a string. Everything live so far was a single script with
     * no parameters at all, so neither the Cmds list nor CommandParameter
     * had been seen by a server. */
    psrp_session_t *s;
    psrp_command_t *cmds[2] = { NULL, NULL };
    psrp_buffer_t payload;
    psrp_guid_t pid;
    psrp_value_t v;
    seen_t seen;
    int state = -1, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    psrp_value_init(&v);
    cmds[0] = psrp_command_new("Get-Date", false);
    cmds[1] = psrp_command_new("Out-String", false);
    psrp_buffer_init(&payload);
    if (!cmds[0] || !cmds[1]) { bad = 1; goto done; }

    psrp_value_set_int32(&v, 2000);
    if (psrp_command_add_parameter(cmds[0], "Year", &v) != PSRP_OK) { bad = 1; goto done; }
    psrp_value_set_int32(&v, 1);
    if (psrp_command_add_parameter(cmds[0], "Month", &v) != PSRP_OK) { bad = 1; goto done; }
    psrp_value_set_int32(&v, 1);
    if (psrp_command_add_parameter(cmds[0], "Day", &v) != PSRP_OK) { bad = 1; goto done; }
    if (psrp_command_add_string_parameter(cmds[0], "Format", "yyyy-MM-dd")
        != PSRP_OK) { bad = 1; goto done; }

    if (psrp_session_pipeline_payload(s, cmds, 2, PSRP_PIPELINE_NO_INPUT,
                                      &pid, &payload) != PSRP_OK ||
        psrp_transport_run_command(t, &pid, payload.data, payload.len)
            != PSRP_OK) {
        printf("  FAIL: run: %s\n", psrp_transport_last_error(t));
        bad = 1;
        goto done;
    }
    do {
        state = pump(s, t, &seen, 60000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    printf("  state=%d output=%d text=%s\n", state, seen.output,
           seen.output_text);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);
    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    /* Out-String turns the date into text, so the parameters we sent show up
     * verbatim in what comes back. */
    if (!strstr(seen.output_text, "2000-01-01")) {
        printf("  FAIL: parameters did not reach Get-Date\n");
        bad = 1;
    }

done:
    psrp_value_free(&v);
    psrp_buffer_free(&payload);
    psrp_command_free(cmds[0]);
    psrp_command_free(cmds[1]);
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* --------------------------------------- SecureString to the server ------ */

static int section_secure_to_server(psrp_transport_t *t)
{
    /* The crypto section proved the server can send us a protected value.
     * This is the other direction: encrypt under the negotiated key, send it
     * as a parameter, and have the script reveal it, which proves the server
     * decrypted exactly what we encrypted. */
    static const char kScript[] =
        "param($s) (New-Object System.Net.NetworkCredential('', $s)).Password";
    psrp_session_t *s;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload, cipher;
    psrp_guid_t pid;
    psrp_value_t v;
    seen_t seen;
    int state = -1, bad = 0, waited = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;
    psrp_value_init(&v);
    psrp_buffer_init(&payload);
    psrp_buffer_init(&cipher);

    if (psrp_session_start_key_exchange(s) != PSRP_OK) { bad = 1; goto done; }
    flush_out(s, t);
    while (!psrp_session_has_session_key(s) && waited < 30000) {
        psrp_buffer_t chunk;
        (void)drain(s, t, &seen);
        if (psrp_session_has_session_key(s)) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
    if (!psrp_session_has_session_key(s)) {
        printf("  FAIL: no session key\n");
        bad = 1;
        goto done;
    }

    /* Raw encrypt output is bytes, and an <SS> element carries base64, so a
     * value built from the bytes is not valid XML and never serializes. The
     * protect helper produces the wire form, which is the only shape a caller
     * should need. */
    if (psrp_crypto_protect_string(psrp_session_crypto(s), "hunter2", 7, &v)
        != PSRP_OK) {
        printf("  FAIL: protect_string\n");
        bad = 1;
        goto done;
    }

    cmd = psrp_command_new(kScript, true);
    if (!cmd) { bad = 1; goto done; }
    {
        psrp_result_t rc = psrp_command_add_parameter(cmd, "s", &v);
        if (rc != PSRP_OK) {
            printf("  FAIL: add_parameter: %s\n", psrp_strerror(rc));
            bad = 1;
            goto done;
        }
        rc = psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                           &pid, &payload);
        if (rc != PSRP_OK) {
            printf("  FAIL: pipeline_payload: %s\n", psrp_strerror(rc));
            bad = 1;
            goto done;
        }
    }
    if (psrp_transport_run_command(t, &pid, payload.data, payload.len)
        != PSRP_OK) {
        printf("  FAIL: run: %s\n", psrp_transport_last_error(t));
        bad = 1;
        goto done;
    }
    do {
        state = pump(s, t, &seen, 60000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    printf("  state=%d text=%s\n", state, seen.output_text);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);
    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    if (!strstr(seen.output_text, "hunter2")) {
        printf("  FAIL: the server did not decrypt what we sent\n");
        bad = 1;
    }

done:
    psrp_value_free(&v);
    psrp_buffer_free(&cipher);
    psrp_buffer_free(&payload);
    psrp_command_free(cmd);
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------------------ user events ------ */

static int section_userevent(psrp_transport_t *t)
{
    /* 2.2.2.12. Registering with -Forward asks the server to relay events to
     * the client, which is the only way a USER_EVENT is ever generated. */
    /* Forwarding happens from the runspace's event pump, which only runs when
     * the pipeline yields to it. Start-Sleep does not; Wait-Event does. */
    static const char kScript[] =
        "Register-EngineEvent -SourceIdentifier libpsrp.test -Forward | Out-Null; "
        "New-Event -SourceIdentifier libpsrp.test -MessageData 'hi' | Out-Null; "
        "Wait-Event -SourceIdentifier libpsrp.test -Timeout 3 | Out-Null; "
        "'done'";
    psrp_session_t *s;
    seen_t seen;
    int state, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    state = run_script(s, t, kScript, &seen);
    /* The event can trail the pipeline's own completion; give it a moment. */
    (void)pump(s, t, &seen, 2000);
    printf("  user_events=%d state=%d\n", seen.user_event, state);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);

    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: pipeline state %d\n", state);
        bad = 1;
    }
    if (seen.user_event < 1) {
        printf("  FAIL: no USER_EVENT arrived\n");
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ---------------------------------- application private data, reset ----- */

static int section_appdata(psrp_transport_t *t)
{
    /* 2.2.2.13 arrives unasked during open. Its content is opaque to PSRP; the
     * assertion is only that the message was recognised rather than falling
     * through as unknown. */
    psrp_session_t *s;
    seen_t seen;
    int bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;
    (void)pump(s, t, &seen, 1000);

    printf("  app_private_data=%d\n", seen.app_private_data);
    if (seen.app_private_data < 1) {
        printf("  FAIL: no APPLICATION_PRIVATE_DATA during open\n");
        bad = 1;
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

static int section_reset(psrp_transport_t *t)
{
    /* 2.2.2.31 is protocol 2.3 and later. This server speaks 2.2, so the
     * interesting question is what happens: the message must at least not
     * break the pool. If a reply comes it must quote our identifier. */
    psrp_session_t *s;
    seen_t seen;
    int64_t ci = 0;
    int bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    if (psrp_session_reset_runspace_state(s, &ci) != PSRP_OK) {
        printf("  FAIL: reset_runspace_state refused\n");
        bad = 1;
    } else {
        flush_out(s, t);
        (void)pump(s, t, &seen, 5000);
        printf("  server protocol %s: replies=%d recognised=%d pool=%s\n",
               psrp_session_server_capability(s)
                   ? psrp_session_server_capability(s)->protocol_version
                   : "?",
               seen.availability, seen.last_availability_known,
               psrp_runspace_pool_state_name(psrp_session_pool_state(s)));
        if (seen.availability > 0 && !seen.last_availability_known) {
            printf("  FAIL: reply quoted an identifier we never sent\n");
            bad = 1;
        }
        if (psrp_session_pool_state(s) != PSRP_RUNSPACE_OPENED) {
            printf("  FAIL: pool left Opened\n");
            bad = 1;
        }
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------- pool-level traffic after a pipeline ----- */

static int section_poolafter(psrp_transport_t *t)
{
    /* Every pool-level exchange verified so far happened before any pipeline
     * ran. This asks the same question after one: does a RunspacePool-level
     * reply still arrive once a command has come and gone? If not, the
     * transport is only listening at shell level until the first command,
     * and every USER_EVENT, RUNSPACEPOOL_HOST_CALL and availability reply
     * after that point is lost. */
    psrp_session_t *s;
    seen_t seen;
    int64_t ci = 0;
    int state, bad = 0;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    state = run_script(s, t, "'warm'", &seen);
    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: warm-up pipeline state %d\n", state);
        bad = 1;
        goto done;
    }

    seen.availability = 0;
    if (psrp_session_get_available_runspaces(s, &ci) != PSRP_OK) {
        printf("  FAIL: get_available_runspaces refused\n");
        bad = 1;
        goto done;
    }
    flush_out(s, t);
    (void)pump(s, t, &seen, 10000);
    printf("  after a pipeline: availability replies=%d\n", seen.availability);
    if (seen.availability < 1) {
        printf("  FAIL: pool-level reply lost once a pipeline has run\n");
        bad = 1;
    }

done:
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ---------------------------------- connect from a new client session ---- */

static int section_connect_new(psrp_transport_t *t)
{
    /* 3.1.4.10.3, end to end: one client opens a pool and disconnects; a
     * second, with a fresh transport and session, discovers the pool by
     * enumeration, adopts its ShellId, connects, and runs a command in it.
     * None of adopt_pool, connect_payload, transport_connect or the
     * RUNSPACEPOOL_INIT_DATA handling had been exercised against a server.
     *
     * `t` plays the first client. The second is created here so its wreckage
     * cannot be confused with the first's. */
    psrp_session_t *s1, *s2 = NULL;
    psrp_transport_t *t2 = NULL;
    psrp_wsman_config_t cfg;
    psrp_shell_info_t *shells = NULL;
    size_t shell_count = 0, k;
    psrp_guid_t pool_id;
    psrp_buffer_t payload, resp;
    seen_t seen;
    int state, bad = 1, waited = 0;
    bool found = false;

    memset(&seen, 0, sizeof seen);
    /* Zeroed before the first early exit, since the cleanup frees its
     * strings unconditionally. */
    memset(&cfg, 0, sizeof cfg);
    psrp_buffer_init(&payload);
    psrp_buffer_init(&resp);
    s1 = open_pool(t, &seen);
    if (!s1) return 1;
    pool_id = *psrp_session_pool_id(s1);

    /* First client does some work, then leaves the pool behind. */
    state = run_script(s1, t, "'before'", &seen);
    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("  FAIL: first client's pipeline state %d\n", state);
        goto done;
    }
    if (psrp_transport_disconnect(t, 120000) != PSRP_OK ||
        psrp_session_notify_disconnected(s1) != PSRP_OK) {
        printf("  FAIL: disconnect: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* Discover it the way a stranger would: by enumeration. */
    memset(&cfg, 0, sizeof cfg);
    cfg.username = widen(getenv("PSRP_USER"));
    cfg.password = widen(getenv("PSRP_PASS"));
    cfg.operation_timeout_ms = 60000;
    if (psrp_wsman_enumerate_shells(&cfg, &shells, &shell_count) != PSRP_OK) {
        printf("  FAIL: enumerate\n");
        goto done;
    }
    for (k = 0; k < shell_count; k++) {
        if (psrp_guid_equal(&shells[k].shell_id, &pool_id)) {
            found = true;
            printf("  discovered the pool, state %s\n",
                   shells[k].state ? shells[k].state : "<none>");
        }
    }
    if (!found) {
        printf("  FAIL: the disconnected pool was not enumerated\n");
        goto done;
    }

    /* Second client adopts and connects. */
    if (psrp_wsman_transport_create(&cfg, &t2) != PSRP_OK) {
        printf("  FAIL: second transport: %s\n", psrp_transport_last_error(t2));
        goto done;
    }
    s2 = psrp_session_new();
    if (!s2 || psrp_session_adopt_pool(s2, &pool_id) != PSRP_OK) {
        printf("  FAIL: adopt_pool\n");
        goto done;
    }
    if (psrp_session_connect_payload(s2, &payload) != PSRP_OK) {
        printf("  FAIL: connect_payload\n");
        goto done;
    }
    if (psrp_transport_connect(t2, &pool_id, payload.data, payload.len, &resp)
        != PSRP_OK) {
        printf("  FAIL: transport_connect: %s\n", psrp_transport_last_error(t2));
        goto done;
    }

    /* 3.1.5.3.15: the server's SESSION_CAPABILITY came back inside the
     * ConnectResponse, so it is fed to the session by hand before anything
     * from the stream. If the session cannot read it, show what arrived: the
     * spec is ambiguous about whether this is fragments or bare CLIXML. */
    printf("  connect response: %zu bytes\n", resp.len);
    {
        psrp_result_t rc = psrp_session_receive(s2, resp.data, resp.len);
        if (rc != PSRP_OK) {
            size_t i;
            printf("  session_receive on it: %s; starts: ", psrp_strerror(rc));
            for (i = 0; i < resp.len && i < 96; i++) {
                unsigned char c = resp.data[i];
                putchar((c >= 32 && c < 127) ? c : '.');
            }
            printf("\n");
        }
    }

    /* 3.1.4.10.3 steps 5 and 6: the capability reply takes the pool to
     * Opened; step 8, the private data follows. */
    memset(&seen, 0, sizeof seen);
    while (psrp_session_pool_state(s2) != PSRP_RUNSPACE_OPENED && waited < 30000) {
        psrp_buffer_t chunk;
        (void)drain(s2, t2, &seen);
        if (psrp_session_pool_state(s2) == PSRP_RUNSPACE_OPENED) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t2, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s2, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
    printf("  second client pool state: %s\n",
           psrp_runspace_pool_state_name(psrp_session_pool_state(s2)));
    if (psrp_session_pool_state(s2) != PSRP_RUNSPACE_OPENED) {
        printf("  FAIL: connected pool never reached Opened\n");
        goto done;
    }

    /* The proof: the adopted pool runs a command for its new owner. */
    state = run_script(s2, t2, "'after'", &seen);
    printf("  second client ran a pipeline: state=%d text=%s\n", state,
           seen.output_text);
    /* If that produced nothing, say what did arrive and what the transport
     * thinks, since the pump above swallows transport errors as silence. */
    printf("  init_data=%d app_data=%d transport: %s\n", seen.pool_init_data,
           seen.app_private_data, psrp_transport_last_error(t2));
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);
    if (state != PSRP_INVOCATION_COMPLETED || !strstr(seen.output_text, "after")) {
        printf("  FAIL: adopted pool did not run the command\n");
        goto done;
    }
    bad = 0;

done:
    psrp_shell_info_free_all(shells, shell_count);
    if (t2) (void)psrp_transport_close_shell(t2);
    psrp_session_free(s2);
    psrp_transport_free(t2);
    /* The first client's shell now belongs to the second and is already
     * closed; freeing its session must tolerate that. */
    psrp_session_free(s1);
    psrp_buffer_free(&payload);
    psrp_buffer_free(&resp);
    free((void *)cfg.username);
    free((void *)cfg.password);
    return bad;
}

/* --------------------------------- disconnect with a pipeline running ---- */

static int section_disconnect_running(psrp_transport_t *t)
{
    /* The point of disconnect is that the pipeline keeps running while nobody
     * is attached. So: start something slow, disconnect before it finishes,
     * wait past its finish, reconnect, and collect the output it produced
     * while we were away. The live test only ever disconnected an idle pool. */
    static const char kScript[] = "Start-Sleep -Seconds 3; 'late'";
    psrp_session_t *s;
    psrp_command_t *cmd;
    psrp_buffer_t payload;
    psrp_guid_t pid;
    seen_t seen;
    int state = -1, bad = 1;

    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    cmd = psrp_command_new(kScript, true);
    psrp_buffer_init(&payload);
    if (!cmd ||
        psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pid, &payload) != PSRP_OK ||
        psrp_transport_run_command(t, &pid, payload.data, payload.len)
            != PSRP_OK) {
        printf("  FAIL: start: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    if (psrp_transport_disconnect(t, 120000) != PSRP_OK ||
        psrp_session_notify_disconnected(s) != PSRP_OK) {
        printf("  FAIL: disconnect: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    printf("  disconnected with the pipeline running\n");
    Sleep(4500);                        /* let it finish unobserved */

    if (psrp_transport_reconnect(t) != PSRP_OK ||
        psrp_session_notify_reconnected(s) != PSRP_OK) {
        printf("  FAIL: reconnect: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    do {
        state = pump(s, t, &seen, 30000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    printf("  after reconnect: state=%d text=%s\n", state, seen.output_text);
    if (state != PSRP_INVOCATION_COMPLETED || !strstr(seen.output_text, "late")) {
        printf("  FAIL: output produced while disconnected was lost\n");
        goto done;
    }
    bad = 0;

done:
    psrp_buffer_free(&payload);
    psrp_command_free(cmd);
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------------------- PSCredential ---- */

static int section_credential(psrp_transport_t *t)
{
    /* 2.2.3.25 to the server: a credential whose password rides as a
     * SecureString under the negotiated key. The script unwraps both halves,
     * so what comes back is exactly what we sent. */
    static const char kScript[] =
        "param($c) \"$($c.UserName):$($c.GetNetworkCredential().Password)\"";
    psrp_session_t *s;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload;
    psrp_guid_t pid;
    psrp_value_t ss, cred;
    seen_t seen;
    int state = -1, bad = 1, waited = 0;

    memset(&seen, 0, sizeof seen);
    psrp_value_init(&ss);
    psrp_value_init(&cred);
    psrp_buffer_init(&payload);
    s = open_pool(t, &seen);
    if (!s) return 1;

    if (psrp_session_start_key_exchange(s) != PSRP_OK) goto done;
    flush_out(s, t);
    while (!psrp_session_has_session_key(s) && waited < 30000) {
        psrp_buffer_t chunk;
        (void)drain(s, t, &seen);
        if (psrp_session_has_session_key(s)) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
    if (!psrp_session_has_session_key(s)) {
        printf("  FAIL: no session key\n");
        goto done;
    }

    if (psrp_crypto_protect_string(psrp_session_crypto(s), "hunter2", 7, &ss)
        != PSRP_OK) {
        printf("  FAIL: protect\n");
        goto done;
    }
    if (psrp_host_make_credential("alice", ss.as.text.ptr, &cred) != PSRP_OK) {
        printf("  FAIL: make_credential\n");
        goto done;
    }

    cmd = psrp_command_new(kScript, true);
    if (!cmd || psrp_command_add_parameter(cmd, "c", &cred) != PSRP_OK) goto done;
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pid, &payload) != PSRP_OK ||
        psrp_transport_run_command(t, &pid, payload.data, payload.len)
            != PSRP_OK) {
        printf("  FAIL: run: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    do {
        state = pump(s, t, &seen, 60000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    printf("  state=%d text=%s\n", state, seen.output_text);
    if (seen.first_error[0]) printf("  error record: %s\n", seen.first_error);
    if (state != PSRP_INVOCATION_COMPLETED ||
        !strstr(seen.output_text, "alice:hunter2")) {
        printf("  FAIL: the credential did not survive the trip\n");
        goto done;
    }
    bad = 0;

done:
    psrp_value_free(&ss);
    psrp_value_free(&cred);
    psrp_buffer_free(&payload);
    psrp_command_free(cmd);
    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    return bad;
}

/* ------------------------------------------------------------------ main -- */

typedef int (*section_fn)(psrp_transport_t *);

static const struct { const char *name; section_fn fn; } kSections[] = {
    { "streams",   section_streams },
    { "runspaces", section_runspaces },
    { "hostcall",  section_hostcall },
    { "crypto",    section_crypto },
    { "metadata",  section_metadata },
    { "hostread",  section_hostread },
    { "stop",      section_stop },
    { "multicommand", section_multicommand },
    { "secure_to_server", section_secure_to_server },
    { "userevent", section_userevent },
    { "appdata",   section_appdata },
    { "reset",     section_reset },
    { "poolafter", section_poolafter },
    { "connect_new", section_connect_new },
    { "disconnect_running", section_disconnect_running },
    { "credential", section_credential },
};
#define SECTION_COUNT (sizeof kSections / sizeof kSections[0])

int main(int argc, char **argv)
{
    const char *enabled = getenv("PSRP_INTEROP");
    const char *only = argc > 1 ? argv[1] : NULL;
    psrp_wsman_config_t cfg;
    psrp_transport_t *t = NULL;
    int status = 0;
    size_t i;
    bool ran = false;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the feature tests\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.username = widen(getenv("PSRP_USER"));
    cfg.password = widen(getenv("PSRP_PASS"));
    cfg.operation_timeout_ms = 60000;

    for (i = 0; i < SECTION_COUNT; i++) {
        int rc;
        if (only && strcmp(only, kSections[i].name) != 0) continue;
        ran = true;

        /* A fresh transport per section, so one section's wreckage cannot be
         * mistaken for the next one's failure. */
        if (psrp_wsman_transport_create(&cfg, &t) != PSRP_OK) {
            printf("FAIL: transport create: %s\n",
                   psrp_transport_last_error(t));
            status = 1;
            break;
        }
        printf("[%s]\n", kSections[i].name);
        rc = kSections[i].fn(t);
        printf("[%s] %s\n\n", kSections[i].name, rc ? "FAIL" : "ok");
        if (rc) status = 1;
        psrp_transport_free(t);
        t = NULL;
    }

    if (only && !ran) {
        printf("no such section: %s\n", only);
        status = 2;
    }
    printf("%s\n", status ? "FAIL" : "PASS");

    free((void *)cfg.username);
    free((void *)cfg.password);
    return status;
}
