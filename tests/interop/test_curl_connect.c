/* Live test: adopt a disconnected RunspacePool from Linux.
 *
 * test_curl_open proves a client can create a pool and run in it. This proves
 * the other half of MS-WSMV's disconnected shells: one client leaves a pool
 * running on the server, and a SECOND client -- its own transport, its own
 * session, no shared state beyond the pool's identifier -- picks it up and
 * finds it usable.
 *
 * Three things are being asserted, and only the third is really about connect:
 *
 *   - the disconnect leaves the shell alive rather than tearing it down;
 *   - the ConnectResponse carries open content, which 3.1.5.3.15 says is
 *     where the server's SESSION_CAPABILITY lives and the only place it
 *     appears for a connect;
 *   - the adopting session reaches Opened, which per 3.1.4.10.3 step 6 the
 *     CLIENT decides on seeing that capability -- no RUNSPACE_STATE follows a
 *     connect, so a session waiting for one would hang here forever.
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

/* Reads from the transport into the session until the pool opens or the
 * budget runs out. Returns non-zero once it is open. */
static int pump_until_open(psrp_session_t *s, psrp_transport_t *t, int rounds)
{
    int i;

    for (i = 0; i < rounds; i++) {
        psrp_buffer_t chunk;
        psrp_event_t e;
        psrp_result_t rc;

        if (psrp_session_pool_state(s) == PSRP_RUNSPACE_OPENED) return 1;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, 3000);
        if (rc == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        if (rc != PSRP_OK && rc != PSRP_ERR_TRUNCATED) break;

        while (psrp_session_next_event(s, &e) == PSRP_OK)
            psrp_event_free(&e);
    }
    return psrp_session_pool_state(s) == PSRP_RUNSPACE_OPENED;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    winrm_config_t cfg;
    psrp_transport_t *t1 = NULL, *t2 = NULL;
    psrp_session_t *s1 = NULL, *s2 = NULL;
    psrp_buffer_t payload, resp;
    psrp_guid_t pool_id;
    char pool_sel[PSRP_GUID_BUF_SIZE];
    psrp_result_t rc;
    size_t i;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the connect test\n");
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

    printf("adopting a disconnected pool over %s\n",
           cfg.connection ? cfg.connection : "http://localhost:5985/wsman");

    psrp_buffer_init(&payload);
    psrp_buffer_init(&resp);

    /* ---- first client: create a pool and leave it running ------------- */
    if (psrp_transport_over_winrm(&cfg, &t1) != PSRP_OK) {
        printf("  FAIL: transport: %s\n", psrp_transport_last_error(t1));
        return 1;
    }
    s1 = psrp_session_new();
    if (!s1) return 1;

    rc = psrp_session_open_payload(s1, &payload);
    if (rc == PSRP_OK)
        rc = psrp_transport_open(t1, psrp_session_pool_id(s1), payload.data,
                                 payload.len);
    if (rc != PSRP_OK)
        printf("    open failed: %s\n", psrp_transport_last_error(t1));
    check(rc == PSRP_OK && pump_until_open(s1, t1, 20),
          "first client opened a pool");
    pool_id = *psrp_session_pool_id(s1);

    rc = winrm_disconnect(psrp_transport_session(t1), 120000);
    if (rc != PSRP_OK)
        printf("    disconnect failed: %s\n", psrp_transport_last_error(t1));
    check(rc == PSRP_OK, "and disconnected, leaving it on the server");
    if (rc != PSRP_OK) goto done;

    /* ---- second client: adopt it ------------------------------------- */
    if (psrp_transport_over_winrm(&cfg, &t2) != PSRP_OK) {
        printf("  FAIL: second transport: %s\n",
               psrp_transport_last_error(t2));
        goto done;
    }
    s2 = psrp_session_new();
    if (!s2 || psrp_session_adopt_pool(s2, &pool_id) != PSRP_OK) goto done;

    psrp_buffer_reset(&payload);
    rc = psrp_session_connect_payload(s2, &payload);
    check(rc == PSRP_OK, "second client built its connect payload");
    if (rc != PSRP_OK) goto done;

    if (psrp_guid_format(&pool_id, pool_sel, sizeof pool_sel) != PSRP_OK)
        goto done;
    /* Upper case: the server matches a ShellId byte for byte and stored the
     * one the first client's Create sent (TODO PSRP-24). A second client has
     * to spell the identifier the way the first one did. */
    for (i = 0; pool_sel[i]; i++)
        if (pool_sel[i] >= 'a' && pool_sel[i] <= 'f')
            pool_sel[i] = (char)(pool_sel[i] - 'a' + 'A');

    rc = winrm_connect(psrp_transport_session(t2), pool_sel, payload.data,
                       payload.len, &resp);
    if (rc != PSRP_OK)
        printf("    connect failed: %s\n", psrp_transport_last_error(t2));
    check(rc == PSRP_OK, "adopted the pool it did not create");
    if (rc != PSRP_OK) goto done;

    check(resp.len > 0, "the ConnectResponse carried open content");
    printf("    connect response: %zu bytes\n", resp.len);

    /* 3.1.5.3.15: that content is the server's SESSION_CAPABILITY and it
     * arrives nowhere else, so it is fed in by hand before the stream. */
    rc = psrp_session_receive(s2, resp.data, resp.len);
    check(rc == PSRP_OK, "and the session could read it");

    check(pump_until_open(s2, t2, 20), "the adopted pool reached Opened");

done:
    if (t2) {
        (void)psrp_transport_close_shell(t2);
        psrp_transport_free(t2);
    } else if (t1) {
        /* Nobody adopted it, so the first client has to take it back before
         * closing or the shell sits on the server until its idle timeout. */
        if (winrm_reconnect(psrp_transport_session(t1)) == PSRP_OK)
            (void)psrp_transport_close_shell(t1);
    }
    psrp_session_free(s2);
    psrp_session_free(s1);
    psrp_transport_free(t1);
    psrp_buffer_free(&payload);
    psrp_buffer_free(&resp);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
