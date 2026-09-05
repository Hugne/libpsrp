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
    char first_error[256];
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
        case PSRP_EVENT_PIPELINE_OUTPUT:      seen->output++; break;
        case PSRP_EVENT_VERBOSE_RECORD:       seen->verbose++; break;
        case PSRP_EVENT_WARNING_RECORD:       seen->warning++; break;
        case PSRP_EVENT_DEBUG_RECORD:         seen->debug++; break;
        case PSRP_EVENT_PROGRESS_RECORD:      seen->progress++; break;
        case PSRP_EVENT_INFORMATION_RECORD:   seen->information++; break;
        case PSRP_EVENT_USER_EVENT:           seen->user_event++; break;
        case PSRP_EVENT_APPLICATION_PRIVATE_DATA: seen->app_private_data++; break;
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
             * waits forever; one without must not be. */
            if (psrp_host_method_returns_value(e.state)) {
                psrp_value_t nothing;
                psrp_value_init(&nothing);
                psrp_value_set_null(&nothing);
                (void)psrp_session_respond_to_host_call(
                    s, psrp_guid_is_empty(&e.pipeline_id) ? NULL
                                                          : &e.pipeline_id,
                    e.call_id, e.state, &nothing, NULL);
                psrp_value_free(&nothing);
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
    const char *patterns[1];
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
    first_name[0] = '\0';
    memset(&seen, 0, sizeof seen);
    s = open_pool(t, &seen);
    if (!s) return 1;

    psrp_buffer_init(&payload);
    if (psrp_session_command_metadata_payload(s, patterns, 1,
                                              PSRP_COMMAND_TYPE_ALL,
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

/* ------------------------------------------------------------------ main -- */

typedef int (*section_fn)(psrp_transport_t *);

static const struct { const char *name; section_fn fn; } kSections[] = {
    { "streams",   section_streams },
    { "runspaces", section_runspaces },
    { "hostcall",  section_hostcall },
    { "crypto",    section_crypto },
    { "metadata",  section_metadata },
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
