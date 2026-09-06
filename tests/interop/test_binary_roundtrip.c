/* The original stdin lab's binary test, ported onto PSRP.
 *
 * The lab that started this project (../pslab, ../tests.py) drove a LOCAL
 * PowerShell with `powershell -Command -`, defined a cmdlet by feeding it
 * statements one line at a time down the process's stdin pipe, then wrote
 * exactly N raw bytes into that same pipe and had the cmdlet read them back
 * with [Console]::OpenStandardInput(), reporting the count and a SHA256.
 *
 * That mechanism does not exist over PSRP and cannot be ported literally.
 * There is no process whose stdin you can write to: the remote runspace lives
 * inside wsmprovhost.exe, [Console]::OpenStandardInput() there is not
 * connected to anything a client can reach, and PowerShell's line-at-a-time
 * console reader -- the thing that let the lab define a function across
 * several sends -- has no counterpart either. Each PSRP pipeline carries a
 * complete script.
 *
 * What PSRP has instead is pipeline input (2.2.2.17): typed objects fed to a
 * running pipeline, arriving as $input rather than as a byte stream. So this
 * test keeps everything about the original that was actually being tested --
 * the same size sweep, the same deterministic payload, exact length and
 * SHA256 verification, and a liveness check afterwards -- and changes only
 * the mechanism by which the bytes travel.
 *
 * The interesting question it answers: does arbitrary binary survive the trip?
 * Bytes here are not written to a pipe, they are serialised as a CLIXML `<BA>`
 * element (2.2.5.1.17), fragmented, sent, reassembled and deserialised. A
 * SHA256 that matches at 16 KB says every one of those layers is byte-exact.
 *
 * Opt-in like the other interop tests: PSRP_INTEROP=1, with PSRP_USER /
 * PSRP_PASS / PSRP_CONNECTION.
 */
/* SHA-256 over the bytes that went out, to be compared with what PowerShell
 * computed over what it received. One implementation per platform; a digest
 * is the only thing either library is asked for. */
#ifdef _WIN32
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <openssl/sha.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_transport.h"
#include "psrp/psrp_records.h"

/* The lab's sizes, unchanged. */
static const size_t kSizes[] = {8, 16, 32, 64, 128, 256, 512, 1024,
                                2048, 4096, 8192, 16384};
#define NSIZES (sizeof kSizes / sizeof kSizes[0])

/* The cmdlet, doing what the lab's Invoke-PslabRead did, but reading pipeline
 * input instead of a console handle. PowerShell unrolls an array as it passes
 * through the pipeline, so the bytes may arrive either as one byte[] or as N
 * separate bytes depending on how the server binds them; accept both rather
 * than depending on which. */
static const char *kScript =
    "$acc = New-Object System.Collections.Generic.List[byte]\n"
    "foreach ($x in $input) {\n"
    "  if ($x -is [byte[]]) { $acc.AddRange($x) } else { $acc.Add([byte]$x) }\n"
    "}\n"
    "$b = $acc.ToArray()\n"
    "$sha = [Security.Cryptography.SHA256]::Create().ComputeHash($b)\n"
    "'PSLAB_DONE:' + $b.Length + ':' + "
    "[BitConverter]::ToString($sha).Replace('-','')\n";

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* The lab's payload, byte for byte: (i * 13 + 7) & 0xFF. */
static void fill_payload(unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)((i * 13 + 7) & 0xFF);
}

/* Uppercase hex, matching what the script's BitConverter produces. */
static void hex32(const unsigned char *digest, char out[65])
{
    static const char *hex = "0123456789ABCDEF";
    int i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = 0;
}

#ifndef _WIN32
static int sha256_hex(const void *data, size_t n, char out[65])
{
    unsigned char digest[32];

    if (!SHA256((const unsigned char *)data, n, digest)) return 0;
    hex32(digest, out);
    return 1;
}
#else
static int sha256_hex(const void *data, size_t n, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char digest[32];
    int ok = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return 0;
    /* create/hash/finish rather than BCryptHash: the latter is a Windows 10
     * convenience wrapper that llvm-mingw's headers do not declare, and this
     * test builds under both toolchains. */
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
        hex32(digest, out);
        ok = 1;
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
#endif

/* Pushes whatever the session has queued out over the transport. */
static int flush(psrp_session_t *s, psrp_transport_t *t)
{
    psrp_buffer_t out;
    int rc = 0;

    psrp_buffer_init(&out);
    if (psrp_session_take_output(s, &out) == PSRP_OK && out.len) {
        if (psrp_transport_send(t, out.data, out.len) != PSRP_OK) {
            printf("    send: %s\n", psrp_transport_last_error(t));
            rc = -1;
        }
    }
    psrp_buffer_free(&out);
    return rc;
}

/* Runs until the pipeline reaches a terminal state, accumulating output text.
 * Returns the invocation state, or -2 on timeout. */
static int pump(psrp_session_t *s, psrp_transport_t *t, psrp_buffer_t *text,
                int timeout_ms)
{
    int waited = 0;
    const int slice = 250;

    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;

        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            psrp_event_kind_t kind = e.kind;
            int state = e.state;

            if (kind == PSRP_EVENT_PIPELINE_OUTPUT)
                (void)psrp_value_to_text(&e.value, text);
            if (kind == PSRP_EVENT_ERROR_RECORD && e.text)
                printf("    [error] %s\n", e.text);

            psrp_event_free(&e);
            if (kind == PSRP_EVENT_PIPELINE_STATE &&
                psrp_invocation_state_is_terminal(state))
                return state;
        }

        if (flush(s, t) != 0) return -2;
        if (waited >= timeout_ms) return -2;

        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, (uint32_t)slice) == PSRP_OK &&
            chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        waited += slice;
    }
}

/* One size: feed N bytes as pipeline input, read back count and digest. */
static int one_size(psrp_session_t *s, psrp_transport_t *t, size_t n)
{
    unsigned char *payload = NULL;
    psrp_command_t *cmd = NULL;
    psrp_buffer_t start, text;
    psrp_value_t v;
    psrp_guid_t pid;
    char expect_sha[65], want[160];
    int state, ok = 0;

    psrp_buffer_init(&start);
    psrp_buffer_init(&text);
    psrp_value_init(&v);

    payload = (unsigned char *)malloc(n);
    if (!payload) goto done;
    fill_payload(payload, n);
    if (!sha256_hex(payload, n, expect_sha)) goto done;

    cmd = psrp_command_new(kScript, true);
    if (!cmd) goto done;

    if (psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_EXPECT_INPUT,
                                      &pid, &start) != PSRP_OK) goto done;
    if (psrp_transport_run_command(t, &pid, start.data, start.len) != PSRP_OK) {
        printf("    run: %s\n", psrp_transport_last_error(t));
        goto done;
    }

    /* The whole payload as one `<BA>`. At 16 KB this is comfortably past the
     * fragment size, so the fragmenter is doing real work here. */
    if (psrp_value_set_bytes(&v, payload, n) != PSRP_OK) goto done;
    if (psrp_session_send_input(s, &pid, &v) != PSRP_OK) goto done;
    if (flush(s, t) != 0) goto done;
    if (psrp_session_end_input(s, &pid) != PSRP_OK) goto done;
    if (flush(s, t) != 0) goto done;

    state = pump(s, t, &text, 60000);
    if (state != PSRP_INVOCATION_COMPLETED) {
        printf("    size %zu: pipeline state %d\n", n, state);
        goto done;
    }
    if (psrp_buffer_append_u8(&text, 0) != PSRP_OK) goto done;

    snprintf(want, sizeof want, "PSLAB_DONE:%zu:%s", n, expect_sha);
    ok = strstr((const char *)text.data, want) != NULL;
    if (!ok)
        printf("    size %zu: wanted %s, got %s\n", n, want,
               (const char *)text.data);

done:
    free(payload);
    psrp_command_free(cmd);
    psrp_value_free(&v);
    psrp_buffer_free(&start);
    psrp_buffer_free(&text);
    return ok;
}

int main(void)
{
    const char *enabled = getenv("PSRP_INTEROP");
    winrm_config_t cfg;
    psrp_transport_t *t = NULL;
    psrp_session_t *s = NULL;
    psrp_buffer_t start;
    const char *user = NULL, *pass = NULL, *conn = NULL;
    size_t i;
    int all = 1;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("skipped: set PSRP_INTEROP=1 to run the binary round-trip\n");
        return 0;
    }

    user = getenv("PSRP_USER");
    pass = getenv("PSRP_PASS");
    conn = getenv("PSRP_CONNECTION");

    memset(&cfg, 0, sizeof cfg);
    cfg.username = user;
    cfg.password = pass;
    cfg.connection = conn;
    cfg.operation_timeout_ms = 60000;

    psrp_buffer_init(&start);

    if (psrp_transport_over_winrm(&cfg, &t) != PSRP_OK) {
        printf("FAIL: transport: %s\n", psrp_transport_last_error(t));
        return 1;
    }
    s = psrp_session_new();
    if (!s) return 1;

    if (psrp_session_open_payload(s, &start) != PSRP_OK) return 1;
    if (psrp_transport_open(t, psrp_session_pool_id(s), start.data,
                            start.len) != PSRP_OK) {
        printf("FAIL: open: %s\n", psrp_transport_last_error(t));
        return 1;
    }
    for (;;) {
        psrp_event_t e;
        psrp_buffer_t chunk;
        int opened = 0;
        while (psrp_session_next_event(s, &e) == PSRP_OK) {
            if (e.kind == PSRP_EVENT_POOL_STATE &&
                e.state == PSRP_RUNSPACE_OPENED) opened = 1;
            psrp_event_free(&e);
        }
        if (opened) break;
        psrp_buffer_init(&chunk);
        if (psrp_transport_receive(t, &chunk, 250) == PSRP_OK && chunk.len)
            (void)psrp_session_receive(s, chunk.data, chunk.len);
        psrp_buffer_free(&chunk);
        if (psrp_session_pool_state(s) == PSRP_RUNSPACE_BROKEN) {
            printf("FAIL: pool broke while opening\n");
            return 1;
        }
    }

    printf("binary round-trip, the lab's sweep over PSRP pipeline input\n");
    for (i = 0; i < NSIZES; i++) {
        char label[64];
        int ok = one_size(s, t, kSizes[i]);
        snprintf(label, sizeof label, "%zu bytes: exact length and SHA256",
                 kSizes[i]);
        check(ok, label);
        if (!ok) all = 0;
    }

    /* The lab checked the session still evaluated statements afterwards; so
     * does this, and for the same reason -- a read that silently starves the
     * session is the failure worth catching. */
    {
        psrp_buffer_t text;
        psrp_command_t *cmd = psrp_command_new("'ALIVE:' + $PID", true);
        psrp_guid_t pid;
        int state = -1;

        psrp_buffer_init(&text);
        psrp_buffer_reset(&start);
        if (cmd &&
            psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
                                          &pid, &start) == PSRP_OK &&
            psrp_transport_run_command(t, &pid, start.data,
                                       start.len) == PSRP_OK) {
            state = pump(s, t, &text, 30000);
            (void)psrp_buffer_append_u8(&text, 0);
        }
        check(state == PSRP_INVOCATION_COMPLETED &&
              strstr((const char *)text.data, "ALIVE:") != NULL,
              "session still evaluates statements after the sweep");
        psrp_command_free(cmd);
        psrp_buffer_free(&text);
    }

    (void)psrp_transport_close_shell(t);
    psrp_session_free(s);
    psrp_transport_free(t);
    psrp_buffer_free(&start);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    (void)all;
    return failures ? 1 : 0;
}
