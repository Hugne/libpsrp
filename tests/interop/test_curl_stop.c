/* Live test: stop a running pipeline from Linux.
 *
 * This path had no live coverage at all until now. The Windows client sends
 * PowerShell's own signal and the Windows suite exercises it; the curl client
 * sent WS-Management's generic terminate URI instead and nothing ever asked a
 * server what it made of that. TODO PSRP-38 recorded the disagreement and
 * said it should not be resolved without a server in front of it. This is
 * that test.
 *
 * What it asserts is not that the Signal request succeeded -- WinRM answers a
 * signal it does nothing with just as cheerfully -- but that the PIPELINE
 * ends up Stopped. A command still running after its stop, or one that ran to
 * completion because the sleep expired first, both fail here.
 *
 * Opt-in: PSRP_INTEROP=1 with PSRP_CONNECTION, PSRP_USER and PSRP_PASS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"
#include "psrp/psrp_records.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload, cmdpay;
    psrp_guid_t pid;
    psrp_result_t rc;
    int i, opened = 0, state = -1, terminal = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the stop test\n");
        return 0;
    }
    if (!getenv("PSRP_USER") || !getenv("PSRP_PASS")) {
        printf("skipped: set PSRP_USER and PSRP_PASS as well\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = getenv("PSRP_CONNECTION");
    cfg.username = getenv("PSRP_USER");
    cfg.password = getenv("PSRP_PASS");
    cfg.operation_timeout_ms = 60000;

    printf("stopping a pipeline over %s\n",
           cfg.connection ? cfg.connection : "http://localhost:5985/wsman");

    psrp_buffer_init(&payload);
    psrp_buffer_init(&cmdpay);

    if (psrp_transport_over_winrm(&cfg, &t) != PSRP_OK) {
        printf("  FAIL: transport: %s\n", psrp_transport_last_error(t));
        return 1;
    }
    s = psrp_session_new();
    if (!s) return 1;

    rc = psrp_session_open_payload(s, &payload);
    if (rc == PSRP_OK)
        rc = psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                                 payload.len);
    for (i = 0; rc == PSRP_OK && i < 20 && !opened; i++) {
        psrp_buffer_t chunk;
        psrp_event_t e;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, 3000);
        if (rc == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        if (rc == PSRP_ERR_TRUNCATED) rc = PSRP_OK;
        if (rc != PSRP_OK) break;
        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_POOL_STATE &&
                e.state == PSRP_RUNSPACE_OPENED) opened = 1;
            psrp_event_free(&e);
        }
    }
    check(opened, "RunspacePool reached Opened");
    if (!opened) goto done;

    /* Long enough that it cannot finish on its own while the stop travels. A
     * pipeline that completed normally would report Completed and pass an
     * assertion that only looked for "terminal". */
    cmd = psrp_command_new("Start-Sleep -Seconds 120", true);
    rc = psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                       &pid, &cmdpay);
    if (rc == PSRP_OK)
        rc = psrp_transport_run_command(t, &pid, cmdpay.data, cmdpay.len);
    if (rc != PSRP_OK)
        printf("    run_command: %s\n", psrp_transport_last_error(t));
    check(rc == PSRP_OK, "a long-running pipeline started");
    if (rc != PSRP_OK) goto done;

    /* One receive first, so the server has certainly begun the command
     * before it is asked to stop it. */
    {
        psrp_buffer_t chunk;
        psrp_event_t e;

        psrp_buffer_init(&chunk);
        (void)psrp_transport_receive(t, &chunk, 3000);
        if (chunk.len) (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_PIPELINE_STATE) state = e.state;
            psrp_event_free(&e);
        }
    }
    check(state != PSRP_INVOCATION_COMPLETED,
          "and had not finished by itself");

    rc = psrp_transport_stop_pipeline(t);
    if (rc != PSRP_OK)
        printf("    stop failed: %s\n", psrp_transport_last_error(t));
    check(rc == PSRP_OK, "the stop was accepted");
    if (rc != PSRP_OK) goto done;

    for (i = 0; i < 20 && !terminal; i++) {
        psrp_buffer_t chunk;
        psrp_event_t e;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, 3000);
        if (rc == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        if (rc != PSRP_OK && rc != PSRP_ERR_TRUNCATED) break;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_PIPELINE_STATE) {
                state = e.state;
                if (psrp_invocation_state_is_terminal(state)) terminal = 1;
            }
            psrp_event_free(&e);
        }
    }

    printf("    pipeline ended in state %d\n", state);
    /* The assertion that matters: Stopped, not merely terminal. Completed
     * here would mean the signal did nothing and the sleep ran out. */
    check(state == PSRP_INVOCATION_STOPPED,
          "and the pipeline really stopped");

done:
    if (t) {
        (void)psrp_transport_close_shell(t);
        psrp_transport_free(t);
    }
    psrp_command_free(cmd);
    psrp_session_free(s);
    psrp_buffer_free(&payload);
    psrp_buffer_free(&cmdpay);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
