/* Live interop test: drive a real PowerShell RunspacePool over WinRM.
 *
 * This is the acceptance test for the whole library. It is opt-in because it
 * needs a WinRM service and credentials, neither of which a unit test should
 * assume. Set PSRP_INTEROP=1 to enable it; PSRP_USER / PSRP_PASS supply
 * credentials, and PSRP_CONNECTION overrides the endpoint.
 *
 * When disabled it reports success without connecting, so the default `ctest`
 * run stays green on a machine with no WinRM.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"
#include "psrp/psrp_records.h"

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

/* Pumps the transport into the session until `want` is seen or time runs out.
 * Returns the state reported by the matching event, or -2 on timeout. */
static int pump_until(psrp_session_t *s, psrp_transport_t *t,
                      psrp_event_kind_t want, int timeout_ms,
                      psrp_buffer_t *output_text)
{
    int waited = 0;
    const int slice = 250;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        psrp_result_t rc;

        /* Drain everything already decoded before waiting on the wire. */
        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            int state = e.state;
            psrp_event_kind_t kind = e.kind;

            if (kind == PSRP_EVENT_PIPELINE_OUTPUT && output_text)
                (void)psrp_value_to_text(&e.value, output_text);
            if (kind == PSRP_EVENT_ERROR_RECORD && e.text)
                printf("    [error record] %s\n", e.text);
            if (kind == PSRP_EVENT_POOL_STATE)
                printf("    pool -> %s\n", psrp_runspace_pool_state_name(state));
            if (kind == PSRP_EVENT_PIPELINE_STATE)
                printf("    pipeline -> %s\n", psrp_invocation_state_name(state));

            psrp_event_free(&e);
            if (kind == want) return state;
        }

        if (waited >= timeout_ms) return -2;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, (uint32_t)slice);
        if (rc == PSRP_OK && chunk.len) {
            psrp_result_t frc = psrp_session_receive(s, chunk.data, chunk.len);
            if (frc != PSRP_OK) {
                printf("    session_receive failed: %s\n", psrp_strerror(frc));
                psrp_buffer_free(&chunk);
                return -2;
            }
        } else if (rc == PSRP_ERR_TRANSPORT) {
            printf("    transport error: %s\n", psrp_transport_last_error(t));
            psrp_buffer_free(&chunk);
            return -2;
        }
        psrp_buffer_free(&chunk);
        waited += slice;
    }
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    const char *user = getenv("PSRP_USER");
    const char *pass = getenv("PSRP_PASS");
    const char *conn = getenv("PSRP_CONNECTION");
    psrp_wsman_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload, out_text;
    psrp_guid_t pipeline_id;
    wchar_t *wuser = NULL, *wpass = NULL, *wconn = NULL;
    int status = 1;
    int state;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the live test\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    wuser = widen(user);
    wpass = widen(pass);
    wconn = widen(conn);
    cfg.username = wuser;
    cfg.password = wpass;
    cfg.connection = wconn;
    cfg.operation_timeout_ms = 60000;

    psrp_buffer_init(&payload);
    psrp_buffer_init(&out_text);

    printf("connecting as %s\n", user ? user : "<current user>");
    if (psrp_wsman_transport_create(&cfg, &t) != PSRP_OK) {
        printf("FAIL: transport create: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    s = psrp_session_new();
    if (!s) { printf("FAIL: session_new\n"); goto done; }

    /* 1. Open the RunspacePool: SESSION_CAPABILITY + INIT_RUNSPACEPOOL ride
     *    along in the shell Create's creationXml. */
    if (psrp_session_open_payload(s, &payload) != PSRP_OK) {
        printf("FAIL: open_payload\n"); goto done;
    }
    printf("opening shell (%zu bytes of creationXml payload)\n", payload.len);
    if (psrp_transport_open(t, psrp_session_pool_id(s), payload.data, payload.len) != PSRP_OK) {
        printf("FAIL: transport open: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* 2. Wait for the pool to report Opened. */
    state = pump_until(s, t, PSRP_EVENT_POOL_STATE, 30000, NULL);
    while (state != PSRP_RUNSPACE_OPENED && state >= 0)
        state = pump_until(s, t, PSRP_EVENT_POOL_STATE, 30000, NULL);
    if (state != PSRP_RUNSPACE_OPENED) {
        printf("FAIL: pool never opened (last state %d)\n", state);
        goto done;
    }
    if (psrp_session_server_capability(s))
        printf("server protocolversion %s\n",
               psrp_session_server_capability(s)->protocol_version);

    /* 3. Run a command. */
    cmd = psrp_command_new("$env:COMPUTERNAME", true);
    if (!cmd) { printf("FAIL: command_new\n"); goto done; }
    psrp_buffer_reset(&payload);
    if (psrp_session_pipeline_payload(s, &cmd, 1, &pipeline_id, &payload)
        != PSRP_OK) {
        printf("FAIL: pipeline_payload\n"); goto done;
    }
    printf("running $env:COMPUTERNAME\n");
    if (psrp_transport_run_command(t, &pipeline_id, payload.data, payload.len) != PSRP_OK) {
        printf("FAIL: run_command: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* 4. Collect output until the pipeline reaches a terminal state. */
    state = pump_until(s, t, PSRP_EVENT_PIPELINE_STATE, 60000, &out_text);
    while (state >= 0 && !psrp_invocation_state_is_terminal(state))
        state = pump_until(s, t, PSRP_EVENT_PIPELINE_STATE, 60000, &out_text);

    if (out_text.len) {
        if (psrp_buffer_append_u8(&out_text, 0) == PSRP_OK)
            printf("output: \"%s\"\n", (const char *)out_text.data);
    } else {
        printf("output: <none>\n");
    }

    /* 5. Close the pool explicitly (wxf:Delete, 3.1.4.2) so the server tears
     *    the shell down now rather than waiting for an idle timeout. */
    if (psrp_transport_close_shell(t) != PSRP_OK) {
        printf("FAIL: close shell: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    printf("shell closed\n");

    if (state == PSRP_INVOCATION_COMPLETED && out_text.len > 1) {
        printf("PASS\n");
        status = 0;
    } else {
        printf("FAIL: pipeline state %d, %zu bytes of output\n",
               state, out_text.len);
    }

done:
    psrp_command_free(cmd);
    psrp_session_free(s);
    psrp_transport_free(t);
    psrp_buffer_free(&payload);
    psrp_buffer_free(&out_text);
    free(wuser); free(wpass); free(wconn);
    return status;
}
