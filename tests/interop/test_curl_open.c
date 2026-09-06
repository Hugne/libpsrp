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

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    psrp_wsman_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_buffer_t payload;
    psrp_result_t rc;
    int i, opened = 0, saw_capability = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the curl transport test\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = getenv("PSRP_CONNECTION");
    cfg.username = getenv("PSRP_USER");
    cfg.password = getenv("PSRP_PASS");
    cfg.operation_timeout_ms = 60000;

    printf("curl transport against %s\n",
           cfg.connection ? cfg.connection : "http://localhost:5985/wsman");

    rc = psrp_wsman_transport_create(&cfg, &t);
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

    /* Everything past this point is deliberately absent from this transport;
     * it must say so rather than appear to work. */
    rc = psrp_transport_run_command(t, psrp_session_pool_id(s), "", 0);
    check(rc == PSRP_ERR_UNSUPPORTED,
          "run_command reports UNSUPPORTED rather than failing silently");

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
