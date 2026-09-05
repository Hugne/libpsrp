/* Live interop test for the convenience layer (psrp_client_*).
 *
 * The point of this layer is that a caller need not understand the sans-IO
 * split to run a command, so the test is written the way a caller would write
 * it: no pump loop, no event queue, no payload buffers.
 *
 * It is opt-in like the other interop tests. Set PSRP_INTEROP=1; PSRP_USER /
 * PSRP_PASS supply credentials and PSRP_CONNECTION overrides the endpoint.
 * With nothing set it reports success without connecting, so a default ctest
 * run stays green on a machine with no WinRM.
 *
 * What is worth proving here, beyond "it works":
 *
 *   - output survives as objects, not as a pre-flattened string;
 *   - the five streams stay apart, which is the thing a single output string
 *     would have destroyed;
 *   - one client runs several commands, because reusing a pool rather than
 *     paying for a shell per command is the real reason this layer exists;
 *   - a command that fails is reported as failing rather than as empty
 *     output, which is the failure mode a convenience wrapper invites.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_client.h"
#include "psrp/psrp_records.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* Flattens a result to a NUL-terminated string for substring assertions. */
static char *result_text(const psrp_run_result_t *r)
{
    psrp_buffer_t b;
    char *out = NULL;

    psrp_buffer_init(&b);
    if (psrp_run_result_text(r, &b) == PSRP_OK &&
        psrp_buffer_append_u8(&b, 0) == PSRP_OK) {
        out = (char *)malloc(b.len);
        if (out) memcpy(out, b.data, b.len);
    }
    psrp_buffer_free(&b);
    return out;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    psrp_client_config_t cfg;
    psrp_client_t *c = NULL;
    psrp_run_result_t r;
    psrp_command_t *cmd = NULL;
    const char *user = NULL, *pass = NULL, *conn = NULL;
    char *text = NULL;
    psrp_result_t rc;
    int i;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the client test\n");
        return 0;
    }

    user = getenv("PSRP_USER");
    pass = getenv("PSRP_PASS");
    conn = getenv("PSRP_CONNECTION");

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = conn;
    cfg.username = user;
    cfg.password = pass;
    cfg.operation_timeout_ms = 60000;

    printf("connect\n");
    rc = psrp_client_connect(&cfg, &c);
    if (rc != PSRP_OK) {
        printf("  FAIL: connect: %s\n", psrp_strerror(rc));
        return 1;
    }
    check(c != NULL, "connect opened a pool");

    /* ---- output stays as objects ------------------------------------- */
    printf("objects\n");
    rc = psrp_client_run(c, "1..3", &r);
    check(rc == PSRP_OK, "run 1..3 returned PSRP_OK");
    check(r.state == PSRP_INVOCATION_COMPLETED, "pipeline completed");
    check(r.output_count == 3, "three separate output objects, not one blob");
    text = result_text(&r);
    check(text && strstr(text, "1") && strstr(text, "3"),
          "text helper flattens them");
    free(text); text = NULL;
    check(!r.had_errors, "no errors reported");
    psrp_run_result_free(&r);

    /* Freeing twice must be safe: the header promises it. */
    psrp_run_result_free(&r);
    check(r.output_count == 0, "double free of a result is safe");

    /* ---- the streams stay apart --------------------------------------- */
    printf("streams\n");
    rc = psrp_client_run(c,
        "Write-Output 'out'; Write-Error 'bad'; Write-Warning 'warn'; "
        "$VerbosePreference='Continue'; Write-Verbose 'chatty'", &r);
    check(rc == PSRP_OK, "mixed-stream command ran");
    check(r.output_count == 1, "only 'out' landed in output");
    check(r.errors.count == 1, "the error went to the error stream");
    check(r.had_errors, "had_errors is set");
    check(r.errors.count && strstr(r.errors.items[0], "bad") != NULL,
          "error text preserved");
    check(r.warnings.count == 1, "the warning went to the warning stream");
    check(r.verbose.count == 1, "the verbose record went to its own stream");
    psrp_run_result_free(&r);

    /* ---- a failing command is reported as failing ---------------------- */
    printf("failure\n");
    rc = psrp_client_run(c, "throw 'deliberate'", &r);
    check(rc == PSRP_OK, "a throwing command still returns PSRP_OK");
    check(r.state == PSRP_INVOCATION_FAILED,
          "and reports FAILED rather than empty output");
    check(r.errors.count >= 1, "with the reason on the error stream");
    psrp_run_result_free(&r);

    /* ---- one pool, many commands -------------------------------------- */
    printf("pool reuse\n");
    for (i = 0; i < 5; i++) {
        rc = psrp_client_run(c, "$PID", &r);
        if (rc != PSRP_OK || r.output_count != 1) {
            psrp_run_result_free(&r);
            break;
        }
        psrp_run_result_free(&r);
    }
    check(i == 5, "five commands over a single connection");

    /* The server-side process must be the same one throughout: a new shell
     * per command would show a different $PID and would mean the pool was
     * being rebuilt behind the caller's back. */
    {
        char first[64] = {0};
        char again[64] = {0};

        rc = psrp_client_run(c, "$PID", &r);
        text = result_text(&r);
        if (text) { strncpy(first, text, sizeof first - 1); free(text); text = NULL; }
        psrp_run_result_free(&r);

        rc = psrp_client_run(c, "$PID", &r);
        text = result_text(&r);
        if (text) { strncpy(again, text, sizeof again - 1); free(text); text = NULL; }
        psrp_run_result_free(&r);

        check(first[0] && strcmp(first, again) == 0,
              "and they share one server-side runspace");
    }

    /* ---- parameters as parameters ------------------------------------- */
    printf("parameterised command\n");
    cmd = psrp_command_new("Write-Output", false);
    if (cmd && psrp_command_add_string_parameter(cmd, "InputObject",
                                                 "parameterised") == PSRP_OK) {
        rc = psrp_client_run_command(c, cmd, &r);
        check(rc == PSRP_OK && r.state == PSRP_INVOCATION_COMPLETED,
              "run_command with a real parameter");
        text = result_text(&r);
        check(text && strstr(text, "parameterised") != NULL,
              "parameter reached the server");
        free(text); text = NULL;
        psrp_run_result_free(&r);
    } else {
        check(0, "building a parameterised command");
    }
    psrp_command_free(cmd);

    /* ---- binary input in the middle of a sequence ---------------------- */
    /*
     * The shape that matters: ordinary commands, then one that consumes
     * binary input, then more ordinary commands -- all on one connection,
     * with the runspace intact throughout. A command that reads input is
     * where a client is most likely to desynchronise the stream and leave
     * everything after it broken, so the commands after `c` are the real
     * assertion here.
     */
    printf("binary input mid-sequence\n");
    {
        unsigned char blob[4096];
        char pid_before[64] = {0}, pid_after[64] = {0};
        size_t k;
        int ok_abc;

        for (k = 0; k < sizeof blob; k++)
            blob[k] = (unsigned char)((k * 13 + 7) & 0xFF);

        /* a, b */
        rc = psrp_client_run(c, "$PID", &r);
        text = result_text(&r);
        if (text) { strncpy(pid_before, text, sizeof pid_before - 1);
                    free(text); text = NULL; }
        psrp_run_result_free(&r);
        rc = psrp_client_run(c, "'b'", &r);
        check(rc == PSRP_OK && r.output_count == 1, "commands before the input one");
        psrp_run_result_free(&r);

        /* c: consumes the bytes and reports what it received */
        rc = psrp_client_run_bytes(c,
            "$acc = New-Object System.Collections.Generic.List[byte]\n"
            "foreach ($x in $input) {\n"
            "  if ($x -is [byte[]]) { $acc.AddRange($x) } else "
            "{ $acc.Add([byte]$x) }\n"
            "}\n"
            "$b = $acc.ToArray()\n"
            "'GOT:' + $b.Length + ':' + $b[0] + ':' + $b[$b.Length-1]\n",
            blob, sizeof blob, &r);
        text = result_text(&r);
        ok_abc = rc == PSRP_OK && r.state == PSRP_INVOCATION_COMPLETED &&
                 text != NULL && strstr(text, "GOT:4096:7:") != NULL;
        check(ok_abc, "the input command received all 4096 bytes intact");
        free(text); text = NULL;
        psrp_run_result_free(&r);

        /* d, e, f */
        rc = psrp_client_run(c, "'d'", &r);
        check(rc == PSRP_OK && r.output_count == 1,
              "the session still works after an input command");
        psrp_run_result_free(&r);

        rc = psrp_client_run(c, "2 + 2", &r);
        text = result_text(&r);
        check(rc == PSRP_OK && text && strstr(text, "4") != NULL,
              "and keeps evaluating correctly");
        free(text); text = NULL;
        psrp_run_result_free(&r);

        rc = psrp_client_run(c, "$PID", &r);
        text = result_text(&r);
        if (text) { strncpy(pid_after, text, sizeof pid_after - 1);
                    free(text); text = NULL; }
        psrp_run_result_free(&r);
        check(pid_before[0] && strcmp(pid_before, pid_after) == 0,
              "in the same runspace it started in");
    }

    /* Several objects rather than one blob, since input is objects and only
     * incidentally bytes. */
    printf("object input\n");
    {
        psrp_value_t vals[3];
        int k;

        for (k = 0; k < 3; k++) {
            psrp_value_init(&vals[k]);
            psrp_value_set_string(&vals[k], k == 0 ? "alpha" :
                                            k == 1 ? "beta" : "gamma");
        }
        rc = psrp_client_run_input(c, "$input | ForEach-Object { $_.ToUpper() }",
                                   vals, 3, &r);
        check(rc == PSRP_OK && r.output_count == 3,
              "three input objects produced three outputs");
        text = result_text(&r);
        check(text && strstr(text, "ALPHA") && strstr(text, "GAMMA"),
              "and each was processed");
        free(text); text = NULL;
        psrp_run_result_free(&r);
        for (k = 0; k < 3; k++) psrp_value_free(&vals[k]);
    }

    /* ---- the escape hatch is real ------------------------------------- */
    printf("escape hatch\n");
    check(psrp_client_session(c) != NULL, "session is reachable");
    check(psrp_client_transport(c) != NULL, "transport is reachable");
    check(psrp_session_pool_state(psrp_client_session(c)) ==
          PSRP_RUNSPACE_OPENED,
          "and the low-level view agrees the pool is open");

    psrp_client_free(c);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
