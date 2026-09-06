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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"
#include "psrp/psrp_records.h"

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

/* Pops whatever the session has queued. The disconnect and reconnect paths
 * raise a pool state event each; nothing here needs to inspect them. */
static void drain_events(psrp_session_t *s)
{
    psrp_event_t e;
    while (psrp_session_next_event(s, &e) == PSRP_OK) psrp_event_free(&e);
}

/* WS-Management reports a ShellId as a string; PSRP's pool id is a GUID that
 * it puts there. Comparing them means formatting one and matching without
 * regard to case, which is how WinRM returns it. */
static bool shell_is(const char *shell_id, const psrp_guid_t *pool)
{
    char want[PSRP_GUID_BUF_SIZE];
    if (!shell_id || psrp_guid_format(pool, want, sizeof want) != PSRP_OK)
        return false;
    return _stricmp(shell_id, want) == 0;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    const char *user = getenv("PSRP_USER");
    const char *pass = getenv("PSRP_PASS");
    const char *conn = getenv("PSRP_CONNECTION");
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload, out_text;
    psrp_guid_t pipeline_id;
    int status = 1;
    int state;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the live test\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.username = user;
    cfg.password = pass;
    cfg.connection = conn;
    cfg.operation_timeout_ms = 60000;

    psrp_buffer_init(&payload);
    psrp_buffer_init(&out_text);

    printf("connecting as %s\n", user ? user : "<current user>");
    if (psrp_transport_over_winrm(&cfg, &t) != PSRP_OK) {
        printf("FAIL: transport create: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    s = psrp_session_new();
    if (!s) { printf("FAIL: session_new\n"); goto done; }

    /* 1. Open the RunspacePool: SESSION_CAPABILITY + INIT_RUNSPACEPOOL ride
     *    along in the shell Create's creationXml. */
    /* Report the machine's time zone, so a real server gets to validate the
     * MS-NRBF blob rather than only our own parser doing so. */
    if (psrp_session_send_timezone(s) != PSRP_OK) {
        printf("FAIL: could not read the local time zone\n");
        goto done;
    }

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
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pipeline_id, &payload)
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
    if (state != PSRP_INVOCATION_COMPLETED || out_text.len <= 1) {
        printf("FAIL: first pipeline state %d, %zu bytes of output\n",
               state, out_text.len);
        goto done;
    }

    /* 5. Enumerate the server's shells (3.1.4.10.1). Our own pool is open, so
     *    it must be in the list, and its ShellId must be our pool id: that is
     *    the identifier another client would use to connect to it. */
    {
        winrm_shell_info_t *shells = NULL;
        size_t shell_count = 0, k;
        bool found = false;

        if (winrm_enumerate_shells(&cfg, &shells, &shell_count)
            != PSRP_OK) {
            printf("FAIL: enumerate shells\n");
            goto done;
        }
        for (k = 0; k < shell_count; k++) {
            if (shell_is(shells[k].shell_id, psrp_session_pool_id(s))) {
                found = true;
                printf("enumerated our pool: state %s\n",
                       shells[k].state ? shells[k].state : "<none>");
            }
        }
        printf("server reports %zu shell(s)\n", shell_count);
        winrm_shell_info_free_all(shells, shell_count);
        if (!found) {
            printf("FAIL: our own pool was not in the enumeration\n");
            goto done;
        }
    }

    /* 6. Disconnect and come back (3.1.4.9, 3.1.4.10.2). The shell keeps
     *    running on the server in between, so this is the one part of the
     *    protocol that cannot be checked without a real server. */
    printf("disconnecting\n");
    if (winrm_disconnect(psrp_transport_session(t), 60000) != PSRP_OK) {
        printf("FAIL: disconnect: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    if (psrp_session_notify_disconnected(s) != PSRP_OK) {
        printf("FAIL: session did not accept the disconnect\n");
        goto done;
    }
    drain_events(s);
    if (psrp_session_pool_state(s) != PSRP_RUNSPACE_DISCONNECTED) {
        printf("FAIL: pool state %d after disconnect\n",
               psrp_session_pool_state(s));
        goto done;
    }

    printf("reconnecting\n");
    if (winrm_reconnect(psrp_transport_session(t)) != PSRP_OK) {
        printf("FAIL: reconnect: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    if (psrp_session_notify_reconnected(s) != PSRP_OK) {
        printf("FAIL: session did not accept the reconnect\n");
        goto done;
    }
    drain_events(s);
    if (psrp_session_pool_state(s) != PSRP_RUNSPACE_OPENED) {
        printf("FAIL: pool state %d after reconnect\n",
               psrp_session_pool_state(s));
        goto done;
    }

    /* 7. The pool still works: run something else through it. */
    psrp_command_free(cmd);
    cmd = psrp_command_new("2 + 2", true);
    if (!cmd) { printf("FAIL: command_new\n"); goto done; }
    psrp_buffer_reset(&payload);
    psrp_buffer_reset(&out_text);
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pipeline_id, &payload)
        != PSRP_OK) {
        printf("FAIL: pipeline_payload after reconnect\n"); goto done;
    }
    printf("running 2 + 2 on the reconnected pool\n");
    if (psrp_transport_run_command(t, &pipeline_id, payload.data, payload.len)
        != PSRP_OK) {
        printf("FAIL: run_command after reconnect: %s\n",
               psrp_transport_last_error(t));
        goto done;
    }
    state = pump_until(s, t, PSRP_EVENT_PIPELINE_STATE, 60000, &out_text);
    while (state >= 0 && !psrp_invocation_state_is_terminal(state))
        state = pump_until(s, t, PSRP_EVENT_PIPELINE_STATE, 60000, &out_text);
    if (out_text.len) {
        if (psrp_buffer_append_u8(&out_text, 0) == PSRP_OK)
            printf("output: \"%s\"\n", (const char *)out_text.data);
    }

    /* 8. Close the pool explicitly (wxf:Delete, 3.1.4.2) so the server tears
     *    the shell down now rather than waiting for an idle timeout. */
    if (psrp_transport_close_shell(t) != PSRP_OK) {
        printf("FAIL: close shell: %s\n", psrp_transport_last_error(t));
        goto done;
    }
    printf("shell closed\n");

    /* 9. Reopen on the same transport. Transport reuse is the documented way to
     *    avoid holding a WinHTTP Event per session (TODO PSRP-14), and it also
     *    exercises the teardown ordering: the command has to be released
     *    before its shell, which used not to happen and made repeated cycles
     *    fail after about forty. */
    {
        int reuse;
        for (reuse = 0; reuse < 3; reuse++) {
            psrp_session_t *s2 = psrp_session_new();
            psrp_buffer_t p2;
            int st2;

            if (!s2) { printf("FAIL: session_new on reuse\n"); goto done; }
            psrp_buffer_init(&p2);
            if (psrp_session_open_payload(s2, &p2) != PSRP_OK ||
                psrp_transport_open(t, psrp_session_pool_id(s2), p2.data,
                                    p2.len) != PSRP_OK) {
                printf("FAIL: reopen %d: %s\n", reuse,
                       psrp_transport_last_error(t));
                psrp_buffer_free(&p2);
                psrp_session_free(s2);
                goto done;
            }
            psrp_buffer_free(&p2);

            st2 = pump_until(s2, t, PSRP_EVENT_POOL_STATE, 30000, NULL);
            while (st2 != PSRP_RUNSPACE_OPENED && st2 >= 0)
                st2 = pump_until(s2, t, PSRP_EVENT_POOL_STATE, 30000, NULL);
            if (st2 != PSRP_RUNSPACE_OPENED) {
                printf("FAIL: reused pool %d never opened\n", reuse);
                psrp_session_free(s2);
                goto done;
            }
            if (psrp_transport_close_shell(t) != PSRP_OK) {
                printf("FAIL: close on reuse %d: %s\n", reuse,
                       psrp_transport_last_error(t));
                psrp_session_free(s2);
                goto done;
            }
            psrp_session_free(s2);
        }
        printf("reopened the transport 3 more times\n");
    }

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
    return status;
}
