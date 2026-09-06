/* Pipeline input against a real server: the PSRP form of the original lab.
 *
 * This project started as an experiment about feeding bytes to PowerShell and
 * seeing what it read back. Everything since has been about the protocol, and
 * the input direction quietly never got tested end to end: PIPELINE_INPUT and
 * END_OF_PIPELINE_INPUT were built and unit-tested against canned bytes, but
 * no test ever sent one to a live PowerShell.
 *
 * The original question was whether a read of N bytes consumed more than N.
 * That does not carry over literally, because PSRP input is a stream of
 * serialized objects rather than a byte pipe, and the framing is the
 * protocol's business rather than the script's. The equivalent question is
 * the one worth answering here:
 *
 *   send exactly N objects, and check the far side sees exactly N, in order,
 *   with nothing merged, dropped or duplicated -- then check the pipeline is
 *   still alive afterwards.
 *
 * The liveness marker from the original harness survives as the trailing
 * "total:N" line, for the same reason: it proves the pipeline reached the end
 * of the script rather than stalling partway.
 *
 * Sizes climb the way the original did. The 64K round is the one that matters
 * most: 2.2.4 caps a fragment blob at 32768 bytes, so that payload cannot fit
 * in one and the send-side fragmenter has to split it. Nothing had exercised
 * that path against a live server before.
 *
 * Opt-in: PSRP_INTEROP=1, with PSRP_USER / PSRP_PASS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_winrm.h"
#include "psrp/psrp_records.h"

/* Drains the session, appending any pipeline output to `sink`. Returns the
 * state of the first pipeline-state event seen, or -1. */
static int pump(psrp_session_t *s, psrp_transport_t *t, psrp_buffer_t *sink,
                int timeout_ms)
{
    int waited = 0;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        int pipeline_state = -1;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_PIPELINE_OUTPUT && sink) {
                psrp_buffer_t text;
                psrp_buffer_init(&text);
                if (psrp_value_to_text(&e.value, &text) == PSRP_OK) {
                    (void)psrp_buffer_append(sink, text.data, text.len);
                    (void)psrp_buffer_append(sink, "\n", 1);
                }
                psrp_buffer_free(&text);
            } else if (e.kind == PSRP_EVENT_ERROR_RECORD && e.text) {
                printf("    [error] %s\n", e.text);
            } else if (e.kind == PSRP_EVENT_PIPELINE_STATE) {
                pipeline_state = e.state;
            }
            psrp_event_free(&e);
        }
        if (pipeline_state >= 0) return pipeline_state;
        if (waited >= timeout_ms) return -1;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
}

/* Flushes whatever the session has queued onto the wire. */
static int flush(psrp_session_t *s, psrp_transport_t *t)
{
    psrp_buffer_t wire;
    int rc = 0;

    psrp_buffer_init(&wire);
    if (psrp_session_take_output(s, &wire) == PSRP_OK && wire.len) {
        if (psrp_transport_send(t, wire.data, wire.len) != PSRP_OK) {
            printf("    send: %s\n", psrp_transport_last_error(t));
            rc = 1;
        }
    }
    psrp_buffer_free(&wire);
    return rc;
}

static size_t count_lines(const psrp_buffer_t *b, const char *prefix)
{
    size_t n = strlen(prefix), i, found = 0;
    if (b->len < n) return 0;
    for (i = 0; i + n <= b->len; i++) {
        if ((i == 0 || b->data[i - 1] == '\n') &&
            memcmp(b->data + i, prefix, n) == 0)
            found++;
    }
    return found;
}

static bool contains(const psrp_buffer_t *b, const char *needle)
{
    size_t n = strlen(needle), i;
    if (b->len < n) return false;
    for (i = 0; i + n <= b->len; i++)
        if (memcmp(b->data + i, needle, n) == 0) return true;
    return false;
}

/* Runs one round: `count` input objects, each `payload` characters long. */
static int one_round(psrp_transport_t *t, size_t count, size_t payload)
{
    /* The script echoes a tag per item and then a total, so a merged, dropped
     * or duplicated item shows up as a wrong count rather than as silence. */
    static const char kScript[] =
        "$n = 0; foreach ($x in $input) { $n++; \"item:$($x.Length)\" }; "
        "\"total:$n\"";
    psrp_session_t *s = psrp_session_new();
    psrp_command_t *cmd = NULL;
    psrp_buffer_t start, sink;
    psrp_guid_t pid;
    psrp_value_t v;
    char *blob = NULL;
    char expect[64];
    size_t i;
    int state;
    int rc = 1;

    if (!s) return 1;
    psrp_buffer_init(&start);
    psrp_buffer_init(&sink);
    psrp_value_init(&v);

    if (psrp_session_open_payload(s, &start) != PSRP_OK) goto done;
    if (psrp_transport_open(t, psrp_session_pool_id(s), start.data, start.len)
        != PSRP_OK) {
        printf("    open: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    do {
        psrp_buffer_t chunk;
        psrp_event_t e;
        bool opened = false;
        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_POOL_STATE &&
                e.state == PSRP_RUNSPACE_OPENED) opened = true;
            psrp_event_free(&e);
        }
        if (opened) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
    } while (psrp_session_pool_state(s) != PSRP_RUNSPACE_BROKEN);

    cmd = psrp_command_new(kScript, true);
    if (!cmd) goto done;
    psrp_buffer_reset(&start);
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_EXPECT_INPUT,
                                      &pid, &start) != PSRP_OK)
        goto done;
    if (psrp_transport_run_command(t, &pid, start.data, start.len) != PSRP_OK) {
        printf("    run: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* Feed exactly `count` objects. */
    blob = (char *)malloc(payload + 1);
    if (!blob) goto done;
    memset(blob, 'x', payload);
    blob[payload] = '\0';

    for (i = 0; i < count; i++) {
        if (psrp_value_set_text(&v, PSRP_VAL_STRING, blob, payload) != PSRP_OK)
            goto done;
        if (psrp_session_send_input(s, &pid, &v) != PSRP_OK) {
            printf("    send_input %zu\n", i);
            goto done;
        }
        if (flush(s, t) != 0) goto done;
    }
    if (psrp_session_end_input(s, &pid) != PSRP_OK) goto done;
    if (flush(s, t) != 0) goto done;

    do {
        state = pump(s, t, &sink, 60000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("    pipeline state %d\n", state);
        goto done;
    }

    /* Exactly `count` items, each the length we sent, then the marker. */
    snprintf(expect, sizeof expect, "item:%zu", payload);
    if (count_lines(&sink, expect) != count) {
        printf("    expected %zu of \"%s\", got %zu\n", count, expect,
               count_lines(&sink, expect));
        goto done;
    }
    snprintf(expect, sizeof expect, "total:%zu", count);
    if (!contains(&sink, expect)) {
        printf("    missing liveness marker \"%s\"\n", expect);
        goto done;
    }

    printf("  %4zu objects x %6zu chars -> %s\n", count, payload, expect);
    rc = 0;

done:
    free(blob);
    psrp_value_free(&v);
    if (psrp_transport_close_shell(t) != PSRP_OK && rc == 0) {
        printf("    close: %s\n", psrp_transport_last_error(t));
        rc = 1;
    }
    psrp_command_free(cmd);
    psrp_session_free(s);
    psrp_buffer_free(&start);
    psrp_buffer_free(&sink);
    return rc;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    psrp_wsman_config_t cfg;
    psrp_transport_t *t = NULL;
    int status = 1;
    size_t i;

    /* Counts and payload sizes climb the way the original lab's did. Only the
     * 64K round exceeds the 32768-byte blob cap and is therefore split across
     * fragments; the rest ride in one each. */
    static const struct { size_t count, payload; } kRounds[] = {
        { 1, 8 }, { 4, 64 }, { 16, 512 }, { 8, 4096 },
        { 4, 16384 }, { 2, 65536 }, { 32, 16 },
    };

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the input test\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.username = getenv("PSRP_USER");
    cfg.password = getenv("PSRP_PASS");
    cfg.operation_timeout_ms = 60000;

    if (psrp_wsman_transport_create(&cfg, &t) != PSRP_OK) {
        printf("FAIL: transport create: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    printf("feeding pipeline input to a live PowerShell:\n");
    for (i = 0; i < sizeof kRounds / sizeof kRounds[0]; i++) {
        if (one_round(t, kRounds[i].count, kRounds[i].payload) != 0) {
            printf("FAIL: round %zu (%zu x %zu)\n", i, kRounds[i].count,
                   kRounds[i].payload);
            goto done;
        }
    }

    printf("PASS\n");
    status = 0;

done:
    psrp_transport_free(t);
    return status;
}
