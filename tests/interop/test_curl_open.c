/* Live test for the curl transport: open a RunspacePool from Linux.
 *
 * The Windows interop suites drive a whole session, which this transport
 * cannot do yet -- run_command and send are still absent. What it can do is
 * the part that was in doubt: authenticate with NTLM, encrypt the request,
 * create the shell, and read the server's SESSION_CAPABILITY back off the
 * stream. That is the milestone this test pins.
 *
 * Opt-in like the other interop tests: PSRP_INTEROP=1, with PSRP_USER,
 * PSRP_PASS and PSRP_CONNECTION. The connection matters here -- the default is
 * localhost, and on Linux the Windows host is somewhere else entirely.
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
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_buffer_t payload;
    psrp_result_t rc;
    int i, opened = 0, saw_capability = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the curl transport test\n");
        return 0;
    }
    /* This one authenticates with a name and password. Without them it would
     * fall back to whatever the ambient credentials are and report a failure
     * that says nothing about the code, so say what is missing instead.
     * (test_kerberos is the opposite case: it must run with NO password.) */
    if (!getenv("PSRP_USER") || !getenv("PSRP_PASS")) {
        printf("skipped: set PSRP_USER and PSRP_PASS as well\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = getenv("PSRP_CONNECTION");
    cfg.username = getenv("PSRP_USER");
    cfg.password = getenv("PSRP_PASS");
    cfg.operation_timeout_ms = 60000;

    printf("curl transport against %s\n",
           cfg.connection ? cfg.connection : "http://localhost:5985/wsman");

    rc = psrp_transport_over_winrm(&cfg, &t);
    if (rc != PSRP_OK) {
        printf("  FAIL: transport create: %s\n", psrp_transport_last_error(t));
        return 1;
    }
    check(1, "transport created");

    s = psrp_session_new();
    if (!s) return 1;

    psrp_buffer_init(&payload);
    rc = psrp_session_open_payload(s, &payload);
    check(rc == PSRP_OK, "session produced its open payload");

    rc = psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                             payload.len);
    if (rc != PSRP_OK)
        printf("    open failed: %s\n", psrp_transport_last_error(t));
    check(rc == PSRP_OK, "shell created (authenticated and encrypted)");

    /* The pool reports Opened once the server's half of the negotiation has
     * arrived, which is the real proof the bytes made the round trip. */
    for (i = 0; rc == PSRP_OK && i < 20 && !opened; i++) {
        psrp_buffer_t chunk;
        psrp_event_t e;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, 5000);
        if (rc == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        /* Nothing yet is an ordinary answer, not a failure. */
        if (rc == PSRP_ERR_TRUNCATED) rc = PSRP_OK;
        if (rc != PSRP_OK) {
            printf("    receive failed: %s\n", psrp_transport_last_error(t));
            break;
        }

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_SESSION_CAPABILITY) saw_capability = 1;
            if (e.kind == PSRP_EVENT_POOL_STATE &&
                e.state == PSRP_RUNSPACE_OPENED) opened = 1;
            psrp_event_free(&e);
        }
    }

    check(saw_capability, "server announced its SESSION_CAPABILITY");
    check(opened, "RunspacePool reached Opened");

    if (saw_capability) {
        const psrp_session_capability_t *c = psrp_session_server_capability(s);
        if (c) printf("    server protocolversion %s\n", c->protocol_version);
    }

    /* ---- run a pipeline and read its output back ---------------------- */
    if (opened) {
        psrp_command_t *cmd = psrp_command_new("'hello from linux'; 6*7", true);
        psrp_buffer_t cmdpay, text;
        psrp_guid_t pid;
        int state = -1, terminal = 0;

        psrp_buffer_init(&cmdpay);
        psrp_buffer_init(&text);

        rc = psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                           &pid, &cmdpay);
        check(rc == PSRP_OK, "pipeline payload built");

        if (rc == PSRP_OK)
            rc = psrp_transport_run_command(t, &pid, cmdpay.data, cmdpay.len);
        if (rc != PSRP_OK)
            printf("    run_command: %s\n", psrp_transport_last_error(t));
        check(rc == PSRP_OK, "command started");

        for (i = 0; rc == PSRP_OK && i < 40 && !terminal; i++) {
            psrp_buffer_t chunk;
            psrp_event_t e;

            psrp_buffer_init(&chunk);
            rc = psrp_transport_receive(t, &chunk, 5000);
            if (rc == PSRP_OK && chunk.len)
                (void)psrp_session_receive(s, chunk.data, chunk.len);
            psrp_buffer_free(&chunk);
            if (rc == PSRP_ERR_TRUNCATED) rc = PSRP_OK;
            if (rc != PSRP_OK) {
                printf("    receive: %s\n", psrp_transport_last_error(t));
                break;
            }

            while (psrp_session_next_event(s, &e) == PSRP_OK) {
                if (e.kind == PSRP_EVENT_PIPELINE_OUTPUT)
                    (void)psrp_value_to_text(&e.value, &text);
                if (e.kind == PSRP_EVENT_ERROR_RECORD && e.text)
                    printf("    [error] %s\n", e.text);
                if (e.kind == PSRP_EVENT_PIPELINE_STATE) {
                    state = e.state;
                    if (psrp_invocation_state_is_terminal(state)) terminal = 1;
                }
                psrp_event_free(&e);
            }
        }

        (void)psrp_buffer_append_u8(&text, 0);
        check(state == PSRP_INVOCATION_COMPLETED, "pipeline completed");
        check(strstr((const char *)text.data, "hello from linux") != NULL,
              "its string output came back");
        check(strstr((const char *)text.data, "42") != NULL,
              "and the expression was evaluated remotely");
        if (text.len > 1) printf("    output: %s\n", (const char *)text.data);

        psrp_command_free(cmd);
        psrp_buffer_free(&cmdpay);
        psrp_buffer_free(&text);
    }

    /* Disconnect and come back to the same shell. The assertion that means
     * anything is the one after the reconnect: a shell that is still there
     * accepts an operation naming it, and one the server tore down answers
     * that it was not found. */
    rc = winrm_disconnect(psrp_transport_session(t), 120000);
    if (rc != PSRP_OK)
        printf("    disconnect failed: %s\n",
               psrp_transport_last_error(t));
    check(rc == PSRP_OK, "shell disconnected");

    if (rc == PSRP_OK) {
        rc = winrm_reconnect(psrp_transport_session(t));
        if (rc != PSRP_OK)
            printf("    reconnect failed: %s\n",
                   psrp_transport_last_error(t));
        check(rc == PSRP_OK, "and reconnected to the same shell");
    }

    rc = psrp_transport_close_shell(t);
    if (rc != PSRP_OK)
        printf("    close failed: %s\n", psrp_transport_last_error(t));
    check(rc == PSRP_OK, "shell closed");

    psrp_buffer_free(&payload);
    psrp_session_free(s);
    psrp_transport_free(t);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
