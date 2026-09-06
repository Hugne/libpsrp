/* WSMan transport for PSRP ([MS-PSRP] 3.1.5.3) on libcurl and GSS-API.
 *
 * The counterpart to transport_wsman.c. Windows has a WSMan client in the OS
 * and that file is a thin shell over it; here every layer has to be built:
 * HTTP, authentication, message encryption, and the WS-Management envelopes.
 *
 * Four things about this are not obvious and were each established the hard
 * way against a live server. They are the reason this file looks the way it
 * does, so they are recorded here rather than in a commit nobody will read:
 *
 *  1. Authentication belongs to the CONNECTION, not the request. NTLM's
 *     challenge and the response answering it must travel on one TCP
 *     connection, so a single CURL handle serves the whole session. Using a
 *     handle per request makes the server answer 401 forever while the
 *     client's own security context reports success.
 *
 *  2. The token exchange must post an EMPTY body. A real SOAP body during the
 *     handshake is refused with 500 for being unencrypted; WinRM closes the
 *     connection when it faults; and closing the connection destroys the NTLM
 *     context, so the next request is unauthenticated again. That loop looks
 *     exactly like bad credentials and is not.
 *
 *  3. curl's own CURLAUTH_NEGOTIATE cannot be used, for two independent
 *     reasons: it never passes the password to GSS-API, relying on an ambient
 *     Kerberos ticket that a workgroup target does not have, and it keeps the
 *     security context to itself. We need that context to encrypt bodies.
 *
 *  4. gss_wrap_iov, which would hand back the signature and ciphertext
 *     already separated, is not implemented by gss-ntlmssp -- it answers
 *     GSS_S_UNAVAILABLE. Plain gss_wrap works, and an NTLM signature is a
 *     fixed 16 bytes, so the split is done by hand. A Kerberos context has a
 *     variable-length header and would need the IOV path, which MIT's
 *     Kerberos mechanism does implement.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>

#include "psrp/psrp_transport.h"
#include "psrp/psrp_fragment.h"
#include "internal/psrp_codec.h"
#include "internal/psrp_xml.h"

/* 3.1.5.3.1. Note the capitalisation: the resource URI spells it PowerShell
 * while the creationXml namespace spells it powershell. They differ. */
#define RESOURCE_URI \
    "http://schemas.microsoft.com/powershell/Microsoft.PowerShell"

#define WS_XFER   "http://schemas.xmlsoap.org/ws/2004/09/transfer"
#define WS_SHELL  "http://schemas.microsoft.com/wbem/wsman/1/windows/shell"

/* MS-WSMV 2.2.9.1. Both strings are fixed by the protocol, not chosen. */
#define ENC_BOUNDARY "Encrypted Boundary"
#define ENC_PROTOCOL "application/HTTP-SPNEGO-session-encrypted"

/* NTLM's signature is this size; see note 4 above. */
#define NTLM_SIGNATURE_BYTES 16

#define DEFAULT_TIMEOUT_MS 240000u
#define MAX_ENVELOPE_SIZE  512000

/* NTLMSSP, 1.3.6.1.4.1.311.2.2.10. Windows accepts a bare NTLM token under the
 * "Negotiate" scheme, and doing so avoids depending on the SPNEGO mechanism
 * being able to negotiate NTLM on the client's behalf. */
static gss_OID_desc ntlm_oid = { 10,
    (void *)"\x2b\x06\x01\x04\x01\x82\x37\x02\x02\x0a" };

struct psrp_transport {
    CURL *curl;                 /* one for the session; see note 1 */
    bool curl_global;           /* we called curl_global_init */
    char *url;
    char *user;
    char *pass;
    uint32_t timeout_ms;

    gss_ctx_id_t gss;
    gss_cred_id_t cred;
    gss_name_t target;
    bool authenticated;

    psrp_guid_t shell_id;
    /* What the server calls this shell, read back from the Create response
     * rather than assumed to be the id we asked for. WinRM is free to assign
     * its own, and every later selector has to match it exactly or the
     * request is refused with "the shell was not found on the server" --
     * which reads as the shell having died rather than as a naming
     * difference. */
    char shell_sel[64];
    bool have_shell;
    psrp_guid_t command_id;
    char cmd_sel[64];   /* the CommandId the server reports back */
    bool have_command;
    bool command_done;
    bool disconnected;

    /* Bytes decoded from Receive responses that the caller has not drained. */
    psrp_buffer_t rx;

    /* Per-request scratch, refilled by the curl callbacks. */
    psrp_buffer_t resp;
    char challenge[16384];

    char last_error[640];
};

/* Formats a GUID the way WinRM wants it in a selector: upper case.
 *
 * The server matches ShellId case-sensitively. Sending the lower-case form
 * that psrp_guid_format produces creates the shell happily and then reports
 * "the shell was not found on the server" on the very next request, which
 * reads as the shell having died rather than as a spelling difference. The
 * Windows transport uppercases for the same reason (TODO PSRP-24). */
static psrp_result_t guid_upper(const psrp_guid_t *g, char *out, size_t cap)
{
    psrp_result_t rc = psrp_guid_format(g, out, cap);
    size_t i;
    if (rc != PSRP_OK) return rc;
    for (i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'f') out[i] = (char)(out[i] - 'a' + 'A');
    return PSRP_OK;
}

static void set_error(psrp_transport_t *t, const char *what, const char *detail)
{
    if (!t) return;
    if (detail && *detail)
        snprintf(t->last_error, sizeof t->last_error, "%s: %s", what, detail);
    else
        snprintf(t->last_error, sizeof t->last_error, "%s", what);
}

static void set_gss_error(psrp_transport_t *t, const char *what,
                          OM_uint32 major, OM_uint32 minor)
{
    OM_uint32 m, ctx = 0;
    gss_buffer_desc msg = GSS_C_EMPTY_BUFFER;
    char detail[512];

    detail[0] = '\0';
    gss_display_status(&m, major, GSS_C_GSS_CODE, GSS_C_NO_OID, &ctx, &msg);
    if (msg.length)
        snprintf(detail, sizeof detail, "%.*s", (int)msg.length,
                 (char *)msg.value);
    gss_release_buffer(&m, &msg);

    ctx = 0;
    msg.length = 0;
    gss_display_status(&m, minor, GSS_C_MECH_CODE, GSS_C_NO_OID, &ctx, &msg);
    if (msg.length) {
        size_t have = strlen(detail);
        snprintf(detail + have, sizeof detail - have, " (%.*s)",
                 (int)msg.length, (char *)msg.value);
    }
    gss_release_buffer(&m, &msg);

    set_error(t, what, detail);
}

/* ------------------------------------------------------------ curl I/O -- */

static size_t on_header(char *b, size_t sz, size_t n, void *user)
{
    psrp_transport_t *t = (psrp_transport_t *)user;
    size_t len = sz * n;

    if (len > 18 && strncasecmp(b, "WWW-Authenticate:", 17) == 0) {
        char *p = b + 17;
        size_t tl = 0;
        while (*p == ' ') p++;
        if (strncasecmp(p, "Negotiate", 9) == 0) {
            p += 9;
            while (*p == ' ') p++;
            while (p + tl < b + len && p[tl] != '\r' && p[tl] != '\n') tl++;
            if (tl && tl < sizeof t->challenge) {
                memcpy(t->challenge, p, tl);
                t->challenge[tl] = '\0';
            }
        }
    }
    return len;
}

/* An encrypted response is binary, so it is accumulated by length; recovering
 * it with strlen would truncate at the first NUL in the ciphertext. */
static size_t on_body(char *b, size_t sz, size_t n, void *user)
{
    psrp_transport_t *t = (psrp_transport_t *)user;
    size_t len = sz * n;
    if (psrp_buffer_append(&t->resp, b, len) != PSRP_OK) return 0;
    return len;
}

/* One POST. `ctype` and `body` may be NULL/0 for the empty handshake posts. */
static psrp_result_t http_post(psrp_transport_t *t, const char *ctype,
                               const void *body, size_t len,
                               const char *auth_header, long *status)
{
    struct curl_slist *hl = NULL;
    CURLcode cc;

    psrp_buffer_reset(&t->resp);
    t->challenge[0] = '\0';

    if (ctype) hl = curl_slist_append(hl, ctype);
    if (auth_header) hl = curl_slist_append(hl, auth_header);
    /* Suppress curl's own Expect: 100-continue; WinRM does not answer it and
     * every request would stall for curl's one-second timeout. */
    hl = curl_slist_append(hl, "Expect:");

    curl_easy_setopt(t->curl, CURLOPT_URL, t->url);
    curl_easy_setopt(t->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(t->curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(t->curl, CURLOPT_POSTFIELDSIZE, (long)len);
    curl_easy_setopt(t->curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(t->curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(t->curl, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(t->curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(t->curl, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(t->curl, CURLOPT_TIMEOUT_MS, (long)t->timeout_ms);

    cc = curl_easy_perform(t->curl);
    curl_slist_free_all(hl);

    if (cc != CURLE_OK) {
        set_error(t, "http", curl_easy_strerror(cc));
        return PSRP_ERR_TRANSPORT;
    }
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, status);
    return PSRP_OK;
}

/* ------------------------------------------------------- authentication -- */

static psrp_result_t authenticate(psrp_transport_t *t)
{
    OM_uint32 major, minor;
    gss_buffer_desc in_tok, out_tok;
    psrp_buffer_t b64;
    int round;
    psrp_result_t rc = PSRP_ERR_TRANSPORT;

    in_tok.value = NULL;
    in_tok.length = 0;
    psrp_buffer_init(&b64);

    for (round = 0; round < 8; round++) {
        char *hdr;
        long status = 0;

        out_tok.value = NULL;
        out_tok.length = 0;
        major = gss_init_sec_context(&minor, t->cred, &t->gss, t->target,
                                     &ntlm_oid,
                                     GSS_C_MUTUAL_FLAG | GSS_C_CONF_FLAG |
                                     GSS_C_INTEG_FLAG | GSS_C_SEQUENCE_FLAG,
                                     GSS_C_INDEFINITE,
                                     GSS_C_NO_CHANNEL_BINDINGS,
                                     in_tok.length ? &in_tok : GSS_C_NO_BUFFER,
                                     NULL, &out_tok, NULL, NULL);
        if (GSS_ERROR(major)) {
            set_gss_error(t, "gss_init_sec_context", major, minor);
            goto done;
        }
        if (!out_tok.length) break;

        psrp_buffer_reset(&b64);
        if (psrp_base64_encode_buf(&b64, out_tok.value, out_tok.length)
            != PSRP_OK) {
            gss_release_buffer(&minor, &out_tok);
            rc = PSRP_ERR_NOMEM;
            goto done;
        }
        gss_release_buffer(&minor, &out_tok);

        hdr = (char *)malloc(b64.len + 32);
        if (!hdr) { rc = PSRP_ERR_NOMEM; goto done; }
        snprintf(hdr, b64.len + 32, "Authorization: Negotiate %.*s",
                 (int)b64.len, (const char *)b64.data);

        /* Empty body, deliberately; see note 2 at the top of this file. */
        rc = http_post(t, NULL, NULL, 0, hdr, &status);
        free(hdr);
        if (rc != PSRP_OK) goto done;

        if (major == GSS_S_COMPLETE) {
            t->authenticated = true;
            rc = PSRP_OK;
            goto done;
        }
        if (!t->challenge[0]) {
            set_error(t, "authenticate", "server sent no continuation token");
            rc = PSRP_ERR_TRANSPORT;
            goto done;
        }

        free(in_tok.value);
        in_tok.value = NULL;
        {
            psrp_buffer_t dec;
            psrp_buffer_init(&dec);
            if (psrp_base64_decode(t->challenge, strlen(t->challenge), &dec)
                != PSRP_OK) {
                psrp_buffer_free(&dec);
                set_error(t, "authenticate", "undecodable challenge");
                rc = PSRP_ERR_MALFORMED;
                goto done;
            }
            in_tok.value = psrp_buffer_detach(&dec, &in_tok.length);
        }
    }

    if (!t->authenticated) {
        set_error(t, "authenticate", "handshake did not complete");
        rc = PSRP_ERR_TRANSPORT;
    }

done:
    free(in_tok.value);
    psrp_buffer_free(&b64);
    return rc;
}

/* -------------------------------------------------- message encryption -- */

/* Wraps `soap` into the multipart/encrypted body WinRM expects. */
static psrp_result_t encrypt_body(psrp_transport_t *t, const char *soap,
                                  size_t slen, psrp_buffer_t *out)
{
    OM_uint32 major, minor;
    gss_buffer_desc in, wrapped;
    int conf = 0;
    char head[256];
    uint8_t siglen_le[4];
    psrp_result_t rc;

    in.value = (void *)soap;
    in.length = slen;
    wrapped.value = NULL;
    wrapped.length = 0;

    major = gss_wrap(&minor, t->gss, 1, GSS_C_QOP_DEFAULT, &in, &conf, &wrapped);
    if (major != GSS_S_COMPLETE) {
        set_gss_error(t, "gss_wrap", major, minor);
        return PSRP_ERR_CRYPTO;
    }
    /* Never send a body the context declined to encrypt. */
    if (!conf || wrapped.length <= NTLM_SIGNATURE_BYTES) {
        gss_release_buffer(&minor, &wrapped);
        set_error(t, "encrypt_body", "context did not provide confidentiality");
        return PSRP_ERR_CRYPTO;
    }

    /* The leading tab on each part header is not decoration: WinRM's parser
     * requires it. */
    snprintf(head, sizeof head,
             "--" ENC_BOUNDARY "\r\n"
             "\tContent-Type: " ENC_PROTOCOL "\r\n"
             "\tOriginalContent: type=application/soap+xml;charset=UTF-8;"
             "Length=%u\r\n"
             "--" ENC_BOUNDARY "\r\n"
             "\tContent-Type: application/octet-stream\r\n",
             (unsigned)slen);

    siglen_le[0] = (uint8_t)(NTLM_SIGNATURE_BYTES & 0xFF);
    siglen_le[1] = siglen_le[2] = siglen_le[3] = 0;

    rc = psrp_buffer_append_str(out, head);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, siglen_le, 4);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, wrapped.value,
                                               wrapped.length);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out,
                                                   "--" ENC_BOUNDARY "--\r\n");
    gss_release_buffer(&minor, &wrapped);
    return rc;
}

/* Recovers the SOAP from a multipart/encrypted response. */
static psrp_result_t decrypt_body(psrp_transport_t *t, const void *resp,
                                  size_t rlen, psrp_buffer_t *out)
{
    static const char marker[] = "application/octet-stream\r\n";
    static const char tail[] = "--" ENC_BOUNDARY "--";
    const char *p = (const char *)resp;
    const char *start = NULL, *end = NULL;
    size_t i;
    OM_uint32 major, minor;
    gss_buffer_desc in, plain;
    int conf = 0;
    psrp_result_t rc;

    for (i = 0; i + sizeof marker - 1 <= rlen; i++) {
        if (memcmp(p + i, marker, sizeof marker - 1) == 0) {
            start = p + i + sizeof marker - 1;
            break;
        }
    }
    if (!start) {
        set_error(t, "decrypt_body", "no encrypted part in the response");
        return PSRP_ERR_MALFORMED;
    }
    for (i = (size_t)(start - p); i + sizeof tail - 1 <= rlen; i++) {
        if (memcmp(p + i, tail, sizeof tail - 1) == 0) { end = p + i; break; }
    }
    if (!end) end = p + rlen;
    if ((size_t)(end - start) <= 4) {
        set_error(t, "decrypt_body", "encrypted part truncated");
        return PSRP_ERR_TRUNCATED;
    }

    /* Four bytes of signature length, then the token gss_unwrap wants. */
    in.value = (void *)(start + 4);
    in.length = (size_t)(end - start) - 4;
    plain.value = NULL;
    plain.length = 0;

    major = gss_unwrap(&minor, t->gss, &in, &plain, &conf, NULL);
    if (major != GSS_S_COMPLETE) {
        set_gss_error(t, "gss_unwrap", major, minor);
        return PSRP_ERR_CRYPTO;
    }
    rc = psrp_buffer_append(out, plain.value, plain.length);
    gss_release_buffer(&minor, &plain);
    return rc;
}

/* Sends one SOAP request encrypted and returns the decrypted reply. */
static psrp_result_t soap_call(psrp_transport_t *t, const char *soap,
                               psrp_buffer_t *reply)
{
    psrp_buffer_t enc;
    long status = 0;
    psrp_result_t rc;

    if (!t->authenticated) {
        rc = authenticate(t);
        if (rc != PSRP_OK) return rc;
    }

    psrp_buffer_init(&enc);
    rc = encrypt_body(t, soap, strlen(soap), &enc);
    if (rc != PSRP_OK) { psrp_buffer_free(&enc); return rc; }

    rc = http_post(t,
                   "Content-Type: multipart/encrypted;protocol=\""
                   ENC_PROTOCOL "\";boundary=\"" ENC_BOUNDARY "\"",
                   enc.data, enc.len, NULL, &status);
    psrp_buffer_free(&enc);
    if (rc != PSRP_OK) return rc;

    if (status != 200) {
        /* A fault body is encrypted too, so decode it before reporting: the
         * status alone says almost nothing about what went wrong. */
        psrp_buffer_t fault;
        char detail[256];
        psrp_buffer_init(&fault);
        snprintf(detail, sizeof detail, "HTTP %ld", status);
        if (decrypt_body(t, t->resp.data, t->resp.len, &fault) == PSRP_OK &&
            fault.len) {
            const char *m = strstr((const char *)fault.data, "<f:Message");
            if (m) snprintf(detail, sizeof detail, "HTTP %ld: %.180s",
                            status, m);
        }
        psrp_buffer_free(&fault);
        set_error(t, "soap", detail);
        return PSRP_ERR_TRANSPORT;
    }

    return decrypt_body(t, t->resp.data, t->resp.len, reply);
}

/* ----------------------------------------------------------- envelopes -- */

/* The header every WS-Man request carries. `action` selects the operation and
 * `selector` carries the ShellId once one exists. */
static psrp_result_t envelope_head(psrp_transport_t *t, psrp_buffer_t *b,
                                   const char *action, bool with_shell)
{
    char msgid[PSRP_GUID_BUF_SIZE];
    char shell[64];   /* the server's ShellId, which need not be a GUID */
    psrp_guid_t id;
    char *head;
    psrp_result_t rc;
    size_t cap = 4096;

    if (psrp_guid_generate(&id) != PSRP_OK) return PSRP_ERR_INTERNAL;
    if (psrp_guid_format(&id, msgid, sizeof msgid) != PSRP_OK)
        return PSRP_ERR_INTERNAL;
    shell[0] = '\0';
    if (with_shell) snprintf(shell, sizeof shell, "%s", t->shell_sel);

    head = (char *)malloc(cap);
    if (!head) return PSRP_ERR_NOMEM;

    snprintf(head, cap,
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsman=\"http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd\""
        " xmlns:rsp=\"" WS_SHELL "\""
        " xmlns:p=\"http://schemas.microsoft.com/wbem/wsman/1/wsman.xsd\">"
        "<s:Header>"
        "<wsa:To>%s</wsa:To>"
        "<wsman:ResourceURI s:mustUnderstand=\"true\">" RESOURCE_URI
            "</wsman:ResourceURI>"
        "<wsa:ReplyTo><wsa:Address s:mustUnderstand=\"true\">"
            "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
            "</wsa:Address></wsa:ReplyTo>"
        "<wsa:Action s:mustUnderstand=\"true\">%s</wsa:Action>"
        "<wsman:MaxEnvelopeSize s:mustUnderstand=\"true\">%d"
            "</wsman:MaxEnvelopeSize>"
        "<wsa:MessageID>uuid:%s</wsa:MessageID>"
        "<wsman:Locale xml:lang=\"en-US\" s:mustUnderstand=\"false\"/>"
        "<wsman:OperationTimeout>PT%u.000S</wsman:OperationTimeout>"
        "%s%s%s",
        t->url, action, MAX_ENVELOPE_SIZE, msgid, t->timeout_ms / 1000u,
        with_shell ? "<wsman:SelectorSet><wsman:Selector Name=\"ShellId\">" : "",
        with_shell ? shell : "",
        with_shell ? "</wsman:Selector></wsman:SelectorSet>" : "");

    rc = psrp_buffer_append_str(b, head);
    free(head);
    return rc;
}

/* ------------------------------------------------------------ lifetime -- */

psrp_result_t psrp_wsman_transport_create(const psrp_wsman_config_t *cfg,
                                          psrp_transport_t **out)
{
    psrp_transport_t *t;
    OM_uint32 major, minor;
    gss_buffer_desc ubuf, pbuf, tbuf;
    gss_name_t user_name = GSS_C_NO_NAME;
    const char *conn;
    char spn[300];
    const char *hoststart, *hostend;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    t = (psrp_transport_t *)calloc(1, sizeof *t);
    if (!t) return PSRP_ERR_NOMEM;

    psrp_buffer_init(&t->rx);
    psrp_buffer_init(&t->resp);
    t->gss = GSS_C_NO_CONTEXT;
    t->cred = GSS_C_NO_CREDENTIAL;
    t->target = GSS_C_NO_NAME;
    t->timeout_ms = (cfg && cfg->operation_timeout_ms)
                        ? cfg->operation_timeout_ms : DEFAULT_TIMEOUT_MS;

    conn = (cfg && cfg->connection) ? cfg->connection
                                    : "http://localhost:5985/wsman";
    t->url = strdup(conn);
    if (!t->url) { psrp_transport_free(t); return PSRP_ERR_NOMEM; }

    /* The service principal is HTTP@<host>, so the host has to be lifted out
     * of the URL. */
    hoststart = strstr(conn, "://");
    hoststart = hoststart ? hoststart + 3 : conn;
    hostend = hoststart;
    while (*hostend && *hostend != ':' && *hostend != '/') hostend++;
    snprintf(spn, sizeof spn, "HTTP@%.*s", (int)(hostend - hoststart),
             hoststart);

    if (cfg && cfg->username && *cfg->username) {
        t->user = strdup(cfg->username);
        t->pass = strdup(cfg->password ? cfg->password : "");
        if (!t->user || !t->pass) { psrp_transport_free(t); return PSRP_ERR_NOMEM; }

        ubuf.value = t->user;
        ubuf.length = strlen(t->user);
        major = gss_import_name(&minor, &ubuf, GSS_C_NT_USER_NAME, &user_name);
        if (major != GSS_S_COMPLETE) {
            set_gss_error(t, "gss_import_name", major, minor);
            psrp_transport_free(t);
            return PSRP_ERR_TRANSPORT;
        }

        /* See note 3: curl will not do this, so we must. */
        pbuf.value = t->pass;
        pbuf.length = strlen(t->pass);
        {
            gss_OID_set_desc mechs = { 1, &ntlm_oid };
            major = gss_acquire_cred_with_password(&minor, user_name, &pbuf,
                                                   GSS_C_INDEFINITE, &mechs,
                                                   GSS_C_INITIATE, &t->cred,
                                                   NULL, NULL);
        }
        gss_release_name(&minor, &user_name);
        if (major != GSS_S_COMPLETE) {
            set_gss_error(t, "gss_acquire_cred_with_password", major, minor);
            psrp_transport_free(t);
            return PSRP_ERR_TRANSPORT;
        }
    }

    tbuf.value = spn;
    tbuf.length = strlen(spn);
    major = gss_import_name(&minor, &tbuf, GSS_C_NT_HOSTBASED_SERVICE,
                            &t->target);
    if (major != GSS_S_COMPLETE) {
        set_gss_error(t, "gss_import_name(spn)", major, minor);
        psrp_transport_free(t);
        return PSRP_ERR_TRANSPORT;
    }

    /* Paired with curl_global_cleanup in psrp_transport_free. libcurl
     * refcounts these, so nesting is correct when a caller holds several
     * transports. Without the pair libcurl still initialises itself on the
     * first easy handle but never releases that state, which valgrind reports
     * as a definite leak with no frame of ours in it. libcurl documents this
     * call as not thread-safe, so a caller creating transports from several
     * threads at once should call curl_global_init itself first. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        psrp_transport_free(t);
        return PSRP_ERR_INTERNAL;
    }
    t->curl_global = true;

    t->curl = curl_easy_init();
    if (!t->curl) { psrp_transport_free(t); return PSRP_ERR_INTERNAL; }

    *out = t;
    return PSRP_OK;
}

void psrp_transport_free(psrp_transport_t *t)
{
    OM_uint32 minor;

    if (!t) return;
    if (t->have_shell) (void)psrp_transport_close_shell(t);
    if (t->curl) curl_easy_cleanup(t->curl);
    if (t->curl_global) curl_global_cleanup();
    if (t->gss != GSS_C_NO_CONTEXT)
        gss_delete_sec_context(&minor, &t->gss, GSS_C_NO_BUFFER);
    if (t->cred != GSS_C_NO_CREDENTIAL) gss_release_cred(&minor, &t->cred);
    if (t->target != GSS_C_NO_NAME) gss_release_name(&minor, &t->target);
    psrp_buffer_free(&t->rx);
    psrp_buffer_free(&t->resp);
    free(t->url);
    if (t->pass) memset(t->pass, 0, strlen(t->pass));
    free(t->user);
    free(t->pass);
    free(t);
}

const char *psrp_transport_last_error(const psrp_transport_t *t)
{
    if (!t) return "no transport";
    return t->last_error[0] ? t->last_error : "no error";
}

bool psrp_transport_is_disconnected(const psrp_transport_t *t)
{
    return t && t->disconnected;
}

bool psrp_transport_command_done(const psrp_transport_t *t)
{
    return t && t->command_done;
}

/* --------------------------------------------------------- operations -- */

/* Pulls every rsp:Stream payload out of a Receive response, base64-decoding
 * each into `out`, and notices the terminal CommandState. Parsing goes
 * through the same pull-parser seam the protocol code uses, so there is no
 * second XML implementation in this file. */
static psrp_result_t collect_streams(psrp_transport_t *t, const char *xml,
                                     size_t n, psrp_buffer_t *out)
{
    psrp_xml_reader_t *r = NULL;
    psrp_xml_node_t node;
    psrp_result_t rc;
    bool in_stream = false;

    rc = psrp_xml_reader_create(xml, n, &r);
    if (rc != PSRP_OK) return rc;

    while ((rc = psrp_xml_read(r, &node)) == PSRP_OK && node != PSRP_XML_EOF) {
        const char *name = psrp_xml_local_name(r);

        if (node == PSRP_XML_ELEMENT) {
            if (strcmp(name, "Stream") == 0) {
                in_stream = true;
            } else if (strcmp(name, "CommandState") == 0) {
                const char *st = psrp_xml_attr(r, "State");
                if (st && strstr(st, "Done")) t->command_done = true;
            }
        } else if (node == PSRP_XML_END_ELEMENT) {
            if (strcmp(name, "Stream") == 0) in_stream = false;
        } else if (node == PSRP_XML_TEXT && in_stream) {
            size_t len = 0;
            const char *b64 = psrp_xml_value(r, &len);
            if (len) {
                rc = psrp_base64_decode(b64, len, out);
                if (rc != PSRP_OK) break;
            }
        }
    }

    psrp_xml_reader_free(r);
    return rc;
}

psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *shell_id,
                                  const void *payload, size_t len)
{
    psrp_buffer_t soap, b64, reply;
    char shell[PSRP_GUID_BUF_SIZE];
    psrp_result_t rc;

    if (!t || !shell_id || (len && !payload)) return PSRP_ERR_INVALID_ARG;

    t->shell_id = *shell_id;
    if (guid_upper(shell_id, shell, sizeof shell) != PSRP_OK)
        return PSRP_ERR_INTERNAL;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&b64);
    psrp_buffer_init(&reply);

    rc = psrp_base64_encode_buf(&b64, payload, len);
    if (rc != PSRP_OK) goto done;

    rc = envelope_head(t, &soap, WS_XFER "/Create", false);
    if (rc != PSRP_OK) goto done;

    /* 3.1.5.3.1 requires the protocolversion option and the server faults
     * without it. The name is lower-case; PowerShell sends it that way. */
    rc = psrp_buffer_append_str(&soap,
        "<wsman:OptionSet s:mustUnderstand=\"true\">"
        "<wsman:Option Name=\"protocolversion\" MustComply=\"true\">2.2"
        "</wsman:Option></wsman:OptionSet>"
        "</s:Header><s:Body><rsp:Shell ShellId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, shell);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "\"><rsp:InputStreams>stdin pr</rsp:InputStreams>"
        "<rsp:OutputStreams>stdout</rsp:OutputStreams>"
        "<creationXml xmlns=\"http://schemas.microsoft.com/powershell\">");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&soap, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</creationXml></rsp:Shell></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply);
    if (rc != PSRP_OK) goto done;

    /* Take the ShellId the server reports rather than the one we asked for.
     * It is usually the same, but "usually" is not a basis for every later
     * selector, and a mismatch surfaces as the shell having vanished. */
    {
        psrp_xml_reader_t *r = NULL;
        psrp_xml_node_t node;
        bool want_text = false;

        snprintf(t->shell_sel, sizeof t->shell_sel, "%s", shell);
        if (psrp_xml_reader_create(reply.data, reply.len, &r) == PSRP_OK) {
            while (psrp_xml_read(r, &node) == PSRP_OK && node != PSRP_XML_EOF) {
                if (node == PSRP_XML_ELEMENT &&
                    strcmp(psrp_xml_local_name(r), "ShellId") == 0) {
                    want_text = true;
                } else if (node == PSRP_XML_TEXT && want_text) {
                    size_t vlen = 0;
                    const char *v = psrp_xml_value(r, &vlen);
                    if (vlen && vlen < sizeof t->shell_sel)
                        snprintf(t->shell_sel, sizeof t->shell_sel, "%.*s",
                                 (int)vlen, v);
                    break;
                } else {
                    want_text = false;
                }
            }
            psrp_xml_reader_free(r);
        }
    }
    t->have_shell = true;

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms)
{
    psrp_buffer_t soap, reply;
    char cmd[64];   /* the server's CommandId, which need not be a GUID */
    psrp_result_t rc;

    (void)timeout_ms;   /* the operation timeout rides in the envelope */
    if (!t || !out) return PSRP_ERR_INVALID_ARG;

    /* Anything left over from a previous response goes first. */
    if (t->rx.len) {
        rc = psrp_buffer_append(out, t->rx.data, t->rx.len);
        psrp_buffer_reset(&t->rx);
        return rc;
    }
    if (!t->have_shell) return PSRP_ERR_STATE;

    cmd[0] = '\0';
    if (t->have_command) snprintf(cmd, sizeof cmd, "%s", t->cmd_sel);

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_SHELL "/Receive", true);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Receive><rsp:DesiredStream");
    if (rc == PSRP_OK && t->have_command) {
        rc = psrp_buffer_append_str(&soap, " CommandId=\"");
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, cmd);
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"");
    }
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        ">stdout</rsp:DesiredStream></rsp:Receive></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply);
    if (rc != PSRP_OK) goto done;

    rc = collect_streams(t, (const char *)reply.data, reply.len, out);
    /* Nothing decoded is not a failure: it is the ordinary "keep waiting"
     * answer a caller's pump loop expects. */
    if (rc == PSRP_OK && out->len == 0) rc = PSRP_ERR_TRUNCATED;

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t psrp_transport_close_shell(psrp_transport_t *t)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_XFER "/Delete", true);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body/></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply);

    /* Closed exactly once, whatever the server answered. */
    t->have_shell = false;
    t->have_command = false;

    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

/* 3.1.5.3.3 puts only the FIRST fragment in Arguments; the rest follow by
 * Send. Decoding one fragment header tells us where that boundary is. */
static size_t first_fragment_len(const void *payload, size_t len)
{
    psrp_reader_t r;
    psrp_fragment_t f;
    psrp_reader_init(&r, payload, len);
    if (psrp_fragment_decode(&r, &f) != PSRP_OK) return 0;
    return r.pos;
}

/* Reads one named element's text out of a response, for the ids the server
 * assigns. */
static bool response_text(const psrp_buffer_t *reply, const char *element,
                          char *out, size_t cap)
{
    psrp_xml_reader_t *r = NULL;
    psrp_xml_node_t node;
    bool want = false, found = false;

    if (psrp_xml_reader_create(reply->data, reply->len, &r) != PSRP_OK)
        return false;

    while (psrp_xml_read(r, &node) == PSRP_OK && node != PSRP_XML_EOF) {
        if (node == PSRP_XML_ELEMENT) {
            want = strcmp(psrp_xml_local_name(r), element) == 0;
        } else if (node == PSRP_XML_TEXT && want) {
            size_t vlen = 0;
            const char *v = psrp_xml_value(r, &vlen);
            if (vlen && vlen < cap) {
                snprintf(out, cap, "%.*s", (int)vlen, v);
                found = true;
            }
            break;
        } else {
            want = false;
        }
    }
    psrp_xml_reader_free(r);
    return found;
}

/* Shared by both streams: PSRP carries data on "stdin" and host responses on
 * "pr" (3.1.5.3.5). */
static psrp_result_t send_on_stream(psrp_transport_t *t, const char *stream,
                                    const void *data, size_t len)
{
    psrp_buffer_t soap, b64, reply;
    psrp_result_t rc;

    if (!t || (len && !data)) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;
    if (len == 0) return PSRP_OK;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&b64);
    psrp_buffer_init(&reply);

    rc = psrp_base64_encode_buf(&b64, data, len);
    if (rc == PSRP_OK) rc = envelope_head(t, &soap, WS_SHELL "/Send", true);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Send><rsp:Stream Name=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, stream);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"");
    if (rc == PSRP_OK && t->have_command) {
        rc = psrp_buffer_append_str(&soap, " CommandId=\"");
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, t->cmd_sel);
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"");
    }
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, ">");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&soap, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Stream></rsp:Send></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply);

    psrp_buffer_free(&soap);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *command_id,
                                         const void *payload, size_t len)
{
    psrp_buffer_t soap, b64, reply;
    char cmd[PSRP_GUID_BUF_SIZE];
    size_t first;
    psrp_result_t rc;

    if (!t || !command_id || !payload || len == 0) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;

    if (guid_upper(command_id, cmd, sizeof cmd) != PSRP_OK)
        return PSRP_ERR_INTERNAL;

    first = first_fragment_len(payload, len);
    if (first == 0) return PSRP_ERR_MALFORMED;

    t->command_id = *command_id;
    t->command_done = false;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&b64);
    psrp_buffer_init(&reply);

    rc = psrp_base64_encode_buf(&b64, payload, first);
    if (rc == PSRP_OK) rc = envelope_head(t, &soap, WS_SHELL "/Command", true);

    /* 3.1.5.3.3 says the Command element MUST be empty, and here it can be.
     * The Windows transport cannot manage that -- the Win32 client rejects an
     * empty command line client-side with 0x80338180 and has to send a single
     * space instead (TODO PSRP-08). Writing the envelope ourselves, the spec's
     * actual requirement is expressible. */
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:CommandLine CommandId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, cmd);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "\"><rsp:Command></rsp:Command><rsp:Arguments>");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&soap, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Arguments></rsp:CommandLine></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply);
    if (rc != PSRP_OK) goto done;

    /* As with the ShellId, use the identifier the server reports. */
    snprintf(t->cmd_sel, sizeof t->cmd_sel, "%s", cmd);
    (void)response_text(&reply, "CommandId", t->cmd_sel, sizeof t->cmd_sel);
    t->have_command = true;

    /* Fragments past the first travel by Send, per 3.1.5.3.3. */
    if (len > first)
        rc = send_on_stream(t, "stdin", (const uint8_t *)payload + first,
                            len - first);

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t psrp_transport_send(psrp_transport_t *t, const void *data,
                                  size_t len)
{
    return send_on_stream(t, "stdin", data, len);
}

psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len)
{
    return send_on_stream(t, "pr", data, len);
}

psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell || !t->have_command) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    /* Targeted at the command, so it stops that pipeline rather than the pool
     * (3.1.4.4, 3.1.5.3.9). */
    rc = envelope_head(t, &soap, WS_SHELL "/Signal", true);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Signal CommandId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, t->cmd_sel);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "\"><rsp:Code>" WS_SHELL "/signal/terminate</rsp:Code>"
        "</rsp:Signal></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply);

    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

/* ---------------------------------------------------- not ported yet --- */
/*
 * These answer PSRP_ERR_UNSUPPORTED rather than pretending. The Windows
 * transport implements all of them; TODO PSRP-35 records the order this is
 * being built in. An absent operation that says so is recoverable; one that
 * silently does nothing is not.
 */

psrp_result_t psrp_transport_disconnect(psrp_transport_t *t,
                                        uint32_t idle_timeout_ms)
{
    (void)idle_timeout_ms;
    set_error(t, "disconnect", "not in the curl transport yet");
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_transport_reconnect(psrp_transport_t *t)
{
    set_error(t, "reconnect", "not in the curl transport yet");
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_transport_connect(psrp_transport_t *t,
                                     const psrp_guid_t *shell_id,
                                     const void *payload, size_t len,
                                     psrp_buffer_t *response_payload)
{
    (void)shell_id; (void)payload; (void)len; (void)response_payload;
    set_error(t, "connect", "not in the curl transport yet");
    return PSRP_ERR_UNSUPPORTED;
}
