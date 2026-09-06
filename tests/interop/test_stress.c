/* Lifecycle stress test.
 *
 * Some defects only appear across repetition, and this suite had no way to see
 * them. Three real bugs were found by running the ordinary lifecycle in a loop
 * and watching process handle counts: a command handle overwritten without
 * being released, a receive context not reset when a transport was reused, and
 * a race where a Receive we cancelled ourselves reported its cancellation as a
 * server fault after the replacement had already been posted. The last of
 * those failed about one run in a hundred cycles, so a single pass could never
 * have caught it.
 *
 * Two things are checked:
 *
 *   1. Every cycle succeeds. The race showed up here as a hard failure.
 *   2. Reusing one transport for many shells does not grow the handle count.
 *      A fresh transport per shell does grow it transiently -- WinHTTP holds a
 *      connection Event for about a minute per discarded session (TODO
 *      PSRP-14) -- which is why only the reuse case is asserted here.
 *
 * Opt-in like the other interop tests: PSRP_INTEROP=1, with PSRP_USER and
 * PSRP_PASS for credentials. PSRP_STRESS_CYCLES raises the count for a soak.
 */
#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_winrm.h"

static DWORD handle_count(void)
{
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    return h;
}

/* Pumps until `want` is seen, or gives up. Returns the event's state, or -1. */
static int pump(psrp_session_t *s, psrp_transport_t *t, psrp_event_kind_t want,
                int timeout_ms)
{
    int waited = 0;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        int seen = -1;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == want) seen = e.state;
            psrp_event_free(&e);
        }
        if (seen >= 0) return seen;
        if (waited >= timeout_ms) return -1;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
}

/* Opens a pool on `t`, runs one pipeline, closes the shell. */
static int one_cycle(psrp_transport_t *t, int run_pipeline)
{
    psrp_session_t *s = psrp_session_new();
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload;
    psrp_guid_t pid;
    int rc = 1;
    int state;

    if (!s) return 1;
    psrp_buffer_init(&payload);

    if (psrp_session_open_payload(s, &payload) != PSRP_OK) goto done;
    if (psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                            payload.len) != PSRP_OK) {
        printf("    open: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    do {
        state = pump(s, t, PSRP_EVENT_POOL_STATE, 30000);
    } while (state >= 0 && state != PSRP_RUNSPACE_OPENED);
    if (state != PSRP_RUNSPACE_OPENED) {
        printf("    pool never opened: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    if (run_pipeline) {
        cmd = psrp_command_new("1", true);
        if (!cmd) goto done;
        psrp_buffer_reset(&payload);
        if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                          &pid, &payload) != PSRP_OK)
            goto done;
        if (psrp_transport_run_command(t, &pid, payload.data, payload.len)
            != PSRP_OK) {
            printf("    run: %s\n", psrp_transport_last_error(t));
            goto done;
        }
        do {
            state = pump(s, t, PSRP_EVENT_PIPELINE_STATE, 60000);
        } while (state >= 0 && !psrp_invocation_state_is_terminal(state));
        if (state != PSRP_INVOCATION_COMPLETED) {
            printf("    pipeline state %d: %s\n", state,
                   psrp_transport_last_error(t));
            goto done;
        }
    }

    if (psrp_transport_close_shell(t) != PSRP_OK) {
        printf("    close: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    rc = 0;

done:
    psrp_command_free(cmd);
    psrp_session_free(s);
    psrp_buffer_free(&payload);
    return rc;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    const char *cycles_env = getenv("PSRP_STRESS_CYCLES");
    psrp_wsman_config_t cfg;
    psrp_transport_t *t = NULL;
    int cycles = cycles_env ? atoi(cycles_env) : 25;
    int status = 1;
    int i;
    DWORD before, after;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the stress test\n");
        return 0;
    }
    if (cycles < 2) cycles = 2;

    memset(&cfg, 0, sizeof cfg);
    cfg.username = getenv("PSRP_USER");
    cfg.password = getenv("PSRP_PASS");
    cfg.operation_timeout_ms = 60000;

    if (psrp_wsman_transport_create(&cfg, &t) != PSRP_OK) {
        printf("FAIL: transport create: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* One cycle before measuring, so the WSMan client's one-time setup is not
     * counted as growth. */
    if (one_cycle(t, 1) != 0) {
        printf("FAIL: warm-up cycle\n");
        goto done;
    }

    before = handle_count();
    for (i = 0; i < cycles; i++) {
        if (one_cycle(t, 1) != 0) {
            printf("FAIL: cycle %d of %d\n", i, cycles);
            goto done;
        }
    }
    after = handle_count();

    printf("%d cycles on one transport: handles %lu -> %lu\n", cycles,
           (unsigned long)before, (unsigned long)after);

    /* Reuse must not accumulate. A small allowance covers lazy per-thread
     * setup inside WSMan; the bugs this has caught grew by one handle per
     * cycle, so anything proportional to the count is caught even at the
     * default size. */
    if ((long)after - (long)before > 16) {
        printf("FAIL: handle count grew by %ld over %d reused cycles\n",
               (long)after - (long)before, cycles);
        goto done;
    }

    printf("PASS\n");
    status = 0;

done:
    psrp_transport_free(t);
    return status;
}
