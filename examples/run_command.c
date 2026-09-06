/* A worked example: run one PowerShell command on a remote machine.
 *
 *   run_command <connection> <user> <password> <command>
 *   run_command http://localhost:5985/wsman Administrator secret "Get-Date"
 *
 * This is the protocol, explicitly. If you only want to run a command, read
 * quick_run.c instead: psrp_client.h does all of the below for you. Read this
 * one to understand what it is doing, or when you need something the
 * convenience layer does not cover.
 *
 * The two halves are worth noticing:
 *
 *   - psrp_session_* is the protocol. It performs no I/O at all. You hand it
 *     bytes that arrived and it hands you events and bytes to send.
 *   - psrp_transport_* moves those bytes over WinRM.
 *
 * Keeping them apart is what makes the protocol testable without a server, and
 * it is why a different transport could be dropped in without touching any of
 * the code above this line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"

/* Moves bytes from the transport into the session and drains what comes out.
 * Returns the state of the first event of `want`, or -1 if it never arrives.
 *
 * A real application would do this on its own schedule rather than blocking;
 * nothing in the session requires a loop shaped like this one. */
static int pump(psrp_session_t *s, psrp_transport_t *t, psrp_event_kind_t want,
                int timeout_ms)
{
    int waited = 0;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            psrp_event_kind_t kind = e.kind;
            int state = e.state;

            if (kind == PSRP_EVENT_PIPELINE_OUTPUT) {
                psrp_buffer_t text;
                psrp_buffer_init(&text);
                if (psrp_value_to_text(&e.value, &text) == PSRP_OK &&
                    psrp_buffer_append_u8(&text, 0) == PSRP_OK)
                    printf("%s\n", (const char *)text.data);
                psrp_buffer_free(&text);
            } else if (kind == PSRP_EVENT_ERROR_RECORD && e.text) {
                fprintf(stderr, "error: %s\n", e.text);
            }

            psrp_event_free(&e);
            if (kind == want) return state;
        }

        if (waited >= timeout_ms) return -1;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += 250;
    }
}

int main(int argc, char **argv)
{
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t payload;
    psrp_guid_t pipeline_id;
    const char *conn = NULL, *user = NULL, *pass = NULL;
    int status = 1;
    int state;

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <connection> <user> <password> <command>\n"
                "  e.g. %s http://localhost:5985/wsman Administrator pw "
                "\"Get-Date\"\n",
                argv[0], argv[0]);
        return 2;
    }

    conn = argv[1];
    user = argv[2];
    pass = argv[3];

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = conn;
    cfg.username = user;
    cfg.password = pass;
    cfg.operation_timeout_ms = 60000;

    psrp_buffer_init(&payload);

    if (psrp_transport_over_winrm(&cfg, &t) != PSRP_OK) {
        fprintf(stderr, "connect: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    s = psrp_session_new();
    if (!s) { fprintf(stderr, "out of memory\n"); goto done; }

    /* Opening a pool sends SESSION_CAPABILITY and INIT_RUNSPACEPOOL. Both ride
     * inside the shell-create request rather than as separate round trips. */
    if (psrp_session_open_payload(s, &payload) != PSRP_OK) goto done;
    if (psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                            payload.len) != PSRP_OK) {
        fprintf(stderr, "open: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* The server reports the pool's state when it is ready. */
    do {
        state = pump(s, t, PSRP_EVENT_POOL_STATE, 30000);
    } while (state >= 0 && state != PSRP_RUNSPACE_OPENED);
    if (state != PSRP_RUNSPACE_OPENED) {
        fprintf(stderr, "pool did not open\n");
        goto done;
    }

    /* `true` marks this as a script rather than a bare command name, which is
     * what lets an expression like "2 + 2" work. */
    cmd = psrp_command_new(argv[4], true);
    if (!cmd) goto done;

    psrp_buffer_reset(&payload);
    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                      &pipeline_id, &payload)
        != PSRP_OK) goto done;
    if (psrp_transport_run_command(t, &pipeline_id, payload.data, payload.len)
        != PSRP_OK) {
        fprintf(stderr, "run: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* Output arrives as events while the pipeline runs; pump prints it. */
    do {
        state = pump(s, t, PSRP_EVENT_PIPELINE_STATE, 60000);
    } while (state >= 0 && !psrp_invocation_state_is_terminal(state));

    status = state == PSRP_INVOCATION_COMPLETED ? 0 : 1;
    (void)psrp_transport_close_shell(t);

done:
    psrp_command_free(cmd);
    psrp_session_free(s);
    psrp_transport_free(t);
    psrp_buffer_free(&payload);
    return status;
}
