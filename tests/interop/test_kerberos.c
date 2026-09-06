/* Live test: authenticate to WinRM from Linux with Kerberos.
 *
 * The point of this test is not that a pipeline runs -- test_curl_open already
 * proves that over NTLM. The point is WHICH mechanism carried it. A client
 * that quietly fell back to NTLM would pass every functional assertion while
 * testing nothing, so the mechanism is forced and then verified:
 *
 *   - WINRM_AUTH_KERBEROS is requested, so no other mechanism is on offer;
 *   - winrm_negotiated_auth is asserted to report Kerberos;
 *   - no password is supplied, so the credential can only come from the
 *     ticket cache. If kinit has not run, this test fails rather than
 *     authenticating some other way.
 *
 * That last point is the one that matters. Passing a password would let
 * gss_acquire_cred_with_password succeed against a mechanism that had nothing
 * to do with the KDC.
 *
 * Opt-in: PSRP_INTEROP=1, PSRP_CONNECTION pointing at the domain endpoint by
 * its FQDN. The name matters -- a Kerberos service ticket is issued for
 * HTTP/<fqdn>, so connecting by IP asks the KDC for a principal it has never
 * heard of.
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

static const char *auth_name(winrm_auth_t a)
{
    switch (a) {
    case WINRM_AUTH_KERBEROS:  return "Kerberos";
    case WINRM_AUTH_NTLM:      return "NTLM";
    case WINRM_AUTH_NEGOTIATE: return "SPNEGO";
    default:                   return "default";
    }
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    const char *conn = getenv("PSRP_CONNECTION");
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_buffer_t payload;
    psrp_result_t rc;
    int i, opened = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the Kerberos test\n");
        return 0;
    }
    if (!conn) {
        printf("skipped: PSRP_CONNECTION must name the endpoint by FQDN "
               "for Kerberos\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = conn;
    cfg.auth = WINRM_AUTH_KERBEROS;
    cfg.operation_timeout_ms = 60000;
    /* No username and no password on purpose: the credential must come from
     * the ticket cache or not at all. */

    printf("Kerberos against %s\n", conn);
    printf("  (no password supplied; the credential must come from kinit)\n");

    rc = psrp_transport_over_winrm(&cfg, &t);
    if (rc != PSRP_OK) {
        printf("  FAIL: transport: %s\n", psrp_transport_last_error(t));
        printf("  If this says no credentials were found, run kinit first.\n");
        return 1;
    }
    check(1, "transport built from the ticket cache");

    s = psrp_session_new();
    if (!s) return 1;

    psrp_buffer_init(&payload);
    rc = psrp_session_open_payload(s, &payload);
    check(rc == PSRP_OK, "session produced its open payload");

    rc = psrp_transport_open(t, psrp_session_pool_id(s), payload.data,
                             payload.len);
    if (rc != PSRP_OK)
        printf("    open failed: %s\n", psrp_transport_last_error(t));
    check(rc == PSRP_OK, "shell created over Kerberos");

    /* The assertion this test exists for. */
    {
        winrm_auth_t used = winrm_negotiated_auth(psrp_transport_session(t));
        printf("    mechanism actually used: %s\n", auth_name(used));
        check(used == WINRM_AUTH_KERBEROS,
              "the context really used Kerberos, not a fallback");
    }

    for (i = 0; rc == PSRP_OK && i < 20 && !opened; i++) {
        psrp_buffer_t chunk;
        psrp_event_t e;

        psrp_buffer_init(&chunk);
        rc = psrp_transport_receive(t, &chunk, 5000);
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

    /* A pipeline, so this is an end-to-end proof and not just a handshake.
     * $env:USERDNSDOMAIN comes back only from a domain-joined host, which
     * also confirms we reached the machine we meant to. */
    if (opened) {
        psrp_command_t *cmd = psrp_command_new(
            "$env:USERDNSDOMAIN + '|' + [Security.Principal.WindowsIdentity]"
            "::GetCurrent().AuthenticationType", true);
        psrp_buffer_t cmdpay, text;
        psrp_guid_t pid;
        int terminal = 0, state = -1;

        psrp_buffer_init(&cmdpay);
        psrp_buffer_init(&text);

        rc = psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                           &pid, &cmdpay);
        if (rc == PSRP_OK)
            rc = psrp_transport_run_command(t, &pid, cmdpay.data, cmdpay.len);
        check(rc == PSRP_OK, "pipeline started");

        for (i = 0; rc == PSRP_OK && i < 40 && !terminal; i++) {
            psrp_buffer_t chunk;
            psrp_event_t e;

            psrp_buffer_init(&chunk);
            rc = psrp_transport_receive(t, &chunk, 5000);
            if (rc == PSRP_OK && chunk.len)
                (void)psrp_session_receive(s, chunk.data, chunk.len);
            psrp_buffer_free(&chunk);
            if (rc == PSRP_ERR_TRUNCATED) rc = PSRP_OK;
            if (rc != PSRP_OK) break;

            while (psrp_session_next_event(s, &e) == PSRP_OK) {
                if (e.kind == PSRP_EVENT_PIPELINE_OUTPUT)
                    (void)psrp_value_to_text(&e.value, &text);
                if (e.kind == PSRP_EVENT_PIPELINE_STATE) {
                    state = e.state;
                    if (psrp_invocation_state_is_terminal(state)) terminal = 1;
                }
                psrp_event_free(&e);
            }
        }
        (void)psrp_buffer_append_u8(&text, 0);
        check(state == PSRP_INVOCATION_COMPLETED, "pipeline completed");
        printf("    remote reports: %s\n", (const char *)text.data);

        /* The server's own view of how we authenticated. It should agree with
         * ours; if Windows says NTLM while we say Kerberos, one of the two is
         * wrong and that is worth knowing loudly. */
        check(strstr((const char *)text.data, "Kerberos") != NULL,
              "and the server agrees the logon was Kerberos");

        psrp_command_free(cmd);
        psrp_buffer_free(&cmdpay);
        psrp_buffer_free(&text);
    }

    (void)psrp_transport_close_shell(t);
    psrp_buffer_free(&payload);
    psrp_session_free(s);
    psrp_transport_free(t);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
