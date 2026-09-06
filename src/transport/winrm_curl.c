/* A WS-Management client on libcurl and GSS-API ([MS-WSMV]).
 *
 * The counterpart to winrm_wsman.c. Windows has a WSMan client in the OS and
 * that file is a thin shim over it; here every layer has to be built: HTTP,
 * authentication, message encryption, and the SOAP envelopes.
 *
 * Nothing here knows what travels on the streams it moves. PSRP's use of this
 * client lives in psrp_over_winrm.c.
 *
 * Eight things about this are not obvious and were each established the hard
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
 *  4. Message encryption needs the signature and the ciphertext separately.
 *     gss_wrap_iov produces exactly that and Kerberos requires it, its
 *     signature being variable-length. gss-ntlmssp does not implement IOV at
 *     all -- it answers GSS_S_UNAVAILABLE -- but an NTLM signature is a fixed
 *     16 bytes, so plain gss_wrap output can be split by hand. Both paths are
 *     here, chosen by what the mechanism supports rather than by preference.
 *     See note 7 for the shape the IOV call has to ask for.
 *
 *  5. A finished context does not have to hand back a final token. Kerberos
 *     with mutual authentication ends by CONSUMING the server's AP-REP: that
 *     call returns GSS_S_COMPLETE and an empty output token, and there is
 *     nothing left to send. NTLM never does this -- its last leg is always
 *     the AUTHENTICATE message -- so treating "no token" as the end of the
 *     exchange works for NTLM and silently loses Kerberos, which then fails
 *     as if the handshake had never completed. Completion is checked on its
 *     own, and the token is sent only if there is one.
 *
 *  6. Forcing Kerberos is done to the CREDENTIAL, not to the wire. The
 *     obvious reading -- force Kerberos by initialising the context with the
 *     Kerberos OID -- puts a bare AP-REQ on the wire, and Windows will
 *     happily authenticate it and then refuse everything that follows,
 *     because the context is filed under a different package than the one it
 *     looks in later. It fails differently depending on how the mismatch is
 *     dressed up: 401 under the Negotiate scheme, an empty 500 straight from
 *     http.sys if the body claims Kerberos encryption, 400 under the Kerberos
 *     scheme. None of them mentions a package. The wire mechanism is always
 *     SPNEGO; a mechanism is forced by acquiring a credential for that
 *     mechanism alone, leaving SPNEGO nothing else it could offer, and the
 *     result is confirmed by asking the context what it settled on.
 *
 *  7. Do NOT ask gss_wrap_iov for a trailer. Windows builds this blob with
 *     SSPI's EncryptMessage, whose output is one token followed by the
 *     ciphertext -- there is no room in that layout for a checksum at the
 *     end. Request HEADER/DATA/PADDING/TRAILER and MIT krb5 obliges, putting
 *     the checksum after the data: a well-formed GSS token that Windows
 *     cannot read. Omit the trailer and MIT emits the SSPI-compatible
 *     rotated form instead, with the whole token in the header buffer, which
 *     is what the four-byte length in front of it is describing. The symptom
 *     of getting this wrong is an empty HTTP 400 and "couldn't decrypt the
 *     packet" in the server's event log.
 *
 *  8. A shell is disconnectable only if the client identified itself. WinRM
 *     refuses a Disconnect on a shell created without the wsmv:SessionId
 *     header, and says so as "this WinRS shell instance does not support
 *     disconnect and reconnect operations because it was created by an older
 *     WinRS client" -- which sounds like a version negotiation and is not:
 *     nothing about the protocolversion option changes it. Disconnecting is
 *     defined as leaving a shell for the same client to come back to, so the
 *     server needs a client identity to attach it to, and with no SessionId
 *     there is none. One GUID generated per session and sent on every request
 *     is the whole fix.
 *
 * Two environment variables make the above debuggable when it goes wrong
 * again: PSRP_GSS_TRACE reports each authentication round and the size of
 * every wrap, and PSRP_HTTP_TRACE turns on curl's own verbose output. Both
 * write to stderr and are silent unless set.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>

#include "psrp/winrm.h"
#include "internal/psrp_codec.h"
#include "internal/psrp_xml.h"

/* 3.1.5.3.1. Note the capitalisation: the resource URI spells it PowerShell
 * while the creationXml namespace spells it powershell. They differ. */
#define RESOURCE_URI \
    "http://schemas.microsoft.com/powershell/Microsoft.PowerShell"

#define WS_XFER   "http://schemas.xmlsoap.org/ws/2004/09/transfer"
#define WS_SHELL  "http://schemas.microsoft.com/wbem/wsman/1/windows/shell"

/* MS-WSMV 2.2.9.1. The boundary is fixed by the protocol, not chosen. */
#define ENC_BOUNDARY "Encrypted Boundary"

/* This names the authentication package that produced the security context,
 * not the cipher, and WinRM checks it: the wrong name is refused with a bare
 * HTTP 400 and "couldn't decrypt the packet" in the server's event log, which
 * reads as a corrupt payload rather than as a label it disagreed with. It is
 * SPNEGO for every context this client establishes, Kerberos included, since
 * SPNEGO is what carried them -- MS-WSMV also defines
 * application/HTTP-Kerberos-session-encrypted, but that belongs to a bare
 * AP-REQ sent under the Kerberos HTTP scheme, which note 6 explains we do
 * not do. */
#define ENC_PROTOCOL "application/HTTP-SPNEGO-session-encrypted"

/* NTLM's signature is this size; see note 4 above. */
#define NTLM_SIGNATURE_BYTES 16

/* Below this a Receive is more round trip than wait; WinRM is also entitled
 * to reject an operation timeout it considers too small. */
#define MIN_RECEIVE_TIMEOUT_MS 200u

#define DEFAULT_TIMEOUT_MS 240000u
#define MAX_ENVELOPE_SIZE  512000

/* The three mechanisms, by OID.
 *
 * SPNEGO negotiates; naming one of the others narrows what it may negotiate,
 * which is what a test needs when the point is to prove which one was used.
 *
 * Only SPNEGO and NTLM are ever put on the wire; the Kerberos OID appears
 * here as something to constrain a credential to. See note 6. */
static gss_OID_desc spnego_oid = { 6, (void *)"\x2b\x06\x01\x05\x05\x02" };

/* Kerberos 5, 1.2.840.113554.1.2.2 */
static gss_OID_desc krb5_oid = { 9,
    (void *)"\x2a\x86\x48\x86\xf7\x12\x01\x02\x02" };

/* NTLMSSP, 1.3.6.1.4.1.311.2.2.10 */
static gss_OID_desc ntlm_oid = { 10,
    (void *)"\x2b\x06\x01\x04\x01\x82\x37\x02\x02\x0a" };

/* What goes on the wire. Never a bare Kerberos AP-REQ, however emphatically
 * the caller asked for Kerberos -- see note 6. */
static gss_OID wire_mech_for(winrm_auth_t a)
{
    switch (a) {
    case WINRM_AUTH_KERBEROS:
    case WINRM_AUTH_NEGOTIATE: return &spnego_oid;
    /* DEFAULT and NTLM alike: see the note in winrm.h for why the
     * default is not SPNEGO yet. */
    default:                   return &ntlm_oid;
    }
}

/* The mechanism SPNEGO must end up using, or NULL to let it choose. */
static gss_OID required_mech_for(winrm_auth_t a)
{
    return a == WINRM_AUTH_KERBEROS ? &krb5_oid : NULL;
}

struct winrm_session {
    CURL *curl;                 /* one for the session; see note 1 */
    bool curl_global;           /* we called curl_global_init */
    char *url;
    char *user;
    char *pass;
    uint32_t timeout_ms;

    gss_ctx_id_t gss;
    gss_cred_id_t cred;
    gss_name_t target;
    gss_OID mech;       /* what goes on the wire */
    gss_OID require;    /* mechanism SPNEGO must settle on, or NULL */
    winrm_auth_t negotiated; /* what the context actually used */
    bool authenticated;

    /* What the server calls this shell, read back from the Create response
     * rather than assumed to be the id we asked for. WinRM is free to assign
     * its own, and every later selector has to match it exactly or the
     * request is refused with "the shell was not found on the server" --
     * which reads as the shell having died rather than as a naming
     * difference. */
    /* MS-WSMV 2.2.4.10, and the reason a shell is disconnectable; see the
     * comment in envelope_head. Generated once per session. */
    char session_id[PSRP_GUID_BUF_SIZE + 8];

    char shell_sel[64];
    bool have_shell;
    char cmd_sel[64];   /* the CommandId the server reports back */
    bool have_command;
    bool command_done;

    /* Which stream the next Receive asks for; see the note in winrm_receive.
     * Only meaningful while a command is running. */
    bool rx_shell_turn;
    bool disconnected;

    /* Bytes decoded from Receive responses that the caller has not drained. */
    psrp_buffer_t rx;

    /* Per-request scratch, refilled by the curl callbacks. */
    psrp_buffer_t resp;
    char challenge[16384];

    char last_error[640];
};


static void set_error(winrm_session_t *t, const char *what, const char *detail)
{
    if (!t) return;
    if (detail && *detail)
        snprintf(t->last_error, sizeof t->last_error, "%s: %s", what, detail);
    else
        snprintf(t->last_error, sizeof t->last_error, "%s", what);
}

static void set_gss_error(winrm_session_t *t, const char *what,
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
    winrm_session_t *t = (winrm_session_t *)user;
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
    winrm_session_t *t = (winrm_session_t *)user;
    size_t len = sz * n;
    if (psrp_buffer_append(&t->resp, b, len) != PSRP_OK) return 0;
    return len;
}

/* One POST. `ctype` and `body` may be NULL/0 for the empty handshake posts. */
static psrp_result_t http_post(winrm_session_t *t, const char *ctype,
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
    if (getenv("PSRP_HTTP_TRACE"))
        curl_easy_setopt(t->curl, CURLOPT_VERBOSE, 1L);

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

/* Records which mechanism the established context actually used.
 *
 * Asked of the context rather than assumed from what we requested. Under
 * SPNEGO that is the whole question, and a test meaning to exercise Kerberos
 * has to be able to tell that it did not quietly succeed over NTLM instead.
 *
 * This also decides how the session authenticates from here on, which is not
 * a matter of taste: see note 6. */
static void record_negotiated(winrm_session_t *t)
{
    gss_OID actual = GSS_C_NO_OID;
    OM_uint32 m;

    if (t->mech == &krb5_oid)      t->negotiated = WINRM_AUTH_KERBEROS;
    else if (t->mech == &ntlm_oid) t->negotiated = WINRM_AUTH_NTLM;

    if (gss_inquire_context(&m, t->gss, NULL, NULL, NULL, &actual,
                            NULL, NULL, NULL) == GSS_S_COMPLETE &&
        actual != GSS_C_NO_OID) {
        if (actual->length == krb5_oid.length &&
            memcmp(actual->elements, krb5_oid.elements,
                   krb5_oid.length) == 0)
            t->negotiated = WINRM_AUTH_KERBEROS;
        else if (actual->length == ntlm_oid.length &&
                 memcmp(actual->elements, ntlm_oid.elements,
                        ntlm_oid.length) == 0)
            t->negotiated = WINRM_AUTH_NTLM;
    }
}

static psrp_result_t authenticate(winrm_session_t *t)
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
        size_t produced;

        out_tok.value = NULL;
        out_tok.length = 0;
        major = gss_init_sec_context(&minor, t->cred, &t->gss, t->target,
                                     t->mech,
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
        if (getenv("PSRP_GSS_TRACE"))
            fprintf(stderr, "[gss] round %d in=%zu major=0x%08x out=%zu\n",
                    round, (size_t)in_tok.length, (unsigned)major,
                    (size_t)out_tok.length);
        /* Send only if the mechanism actually produced something. Whether
         * the context is complete is a separate question from whether there
         * is a last token to deliver, and conflating the two is note 5 at the
         * top of this file: Kerberos finishes on an empty token. */
        /* gss_release_buffer zeroes the descriptor, so anything we want to
         * know about the token has to be remembered before it is freed. */
        produced = (size_t)out_tok.length;

        if (produced) {
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
            if (getenv("PSRP_GSS_TRACE"))
                fprintf(stderr, "[gss]   http %ld, challenge %s\n", status,
                        t->challenge[0] ? "yes" : "no");
            if (rc != PSRP_OK) goto done;
        }

        if (major == GSS_S_COMPLETE) {
            t->authenticated = true;
            record_negotiated(t);
            rc = PSRP_OK;
            goto done;
        }
        if (!produced) {
            set_error(t, "authenticate",
                      "mechanism wants another round but produced no token");
            rc = PSRP_ERR_TRANSPORT;
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
static psrp_result_t encrypt_body(winrm_session_t *t, const char *soap,
                                  size_t slen, psrp_buffer_t *out)
{
    OM_uint32 major, minor;
    gss_iov_buffer_desc iov[3];
    gss_buffer_desc in, wrapped;
    unsigned char *scratch = NULL;
    const unsigned char *sig = NULL, *enc = NULL;
    size_t siglen = 0, enclen = 0;
    int conf = 0;
    char head[256];
    uint8_t siglen_le[4];
    psrp_result_t rc;
    bool used_iov = false;

    wrapped.value = NULL;
    wrapped.length = 0;

    /* Two ways to produce what WinRM wants, and which one applies depends on
     * the mechanism rather than on preference.
     *
     * gss_wrap_iov hands back the signature and the ciphertext already
     * separated, which is exactly the shape needed, and Kerberos requires it:
     * its header is variable-length, so there is no fixed offset to split at.
     * gss-ntlmssp does not implement IOV at all -- it answers
     * GSS_S_UNAVAILABLE -- but NTLM's signature is always 16 bytes, so plain
     * gss_wrap output can be split by hand. Try the general path, fall back
     * to the special case. */
    scratch = (unsigned char *)malloc(slen ? slen : 1);
    if (!scratch) return PSRP_ERR_NOMEM;
    memcpy(scratch, soap, slen);

    /* HEADER, DATA, PADDING and deliberately no TRAILER.
     *
     * Windows produces this blob with SSPI's EncryptMessage, which returns
     * one token buffer and the ciphertext -- there is nowhere in its layout
     * for a trailing checksum. Ask MIT krb5 for a trailer and it obliges,
     * putting the checksum AFTER the data; the result is a perfectly valid
     * GSS token that Windows cannot read, and http.sys rejects it with an
     * empty HTTP 400 and "couldn't decrypt the packet" in the event log.
     * Omit the trailer and MIT emits the SSPI-compatible rotated form
     * instead, with the whole token in the header buffer -- which is what
     * the four-byte length prefix in front of it is describing. */
    memset(iov, 0, sizeof iov);
    iov[0].type = GSS_IOV_BUFFER_TYPE_HEADER | GSS_IOV_BUFFER_FLAG_ALLOCATE;
    iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;
    iov[1].buffer.value = scratch;
    iov[1].buffer.length = slen;
    iov[2].type = GSS_IOV_BUFFER_TYPE_PADDING | GSS_IOV_BUFFER_FLAG_ALLOCATE;

    major = gss_wrap_iov(&minor, t->gss, 1, GSS_C_QOP_DEFAULT, &conf, iov, 3);
    if (major == GSS_S_COMPLETE) {
        used_iov = true;
        sig = (const unsigned char *)iov[0].buffer.value;
        siglen = iov[0].buffer.length;
        /* Padding rides with the ciphertext; for AES it is empty. */
        enc = (const unsigned char *)iov[1].buffer.value;
        enclen = iov[1].buffer.length + iov[2].buffer.length;
    } else {
        in.value = (void *)soap;
        in.length = slen;
        major = gss_wrap(&minor, t->gss, 1, GSS_C_QOP_DEFAULT, &in, &conf,
                         &wrapped);
        if (major != GSS_S_COMPLETE) {
            free(scratch);
            set_gss_error(t, "gss_wrap", major, minor);
            return PSRP_ERR_CRYPTO;
        }
        if (wrapped.length <= NTLM_SIGNATURE_BYTES) {
            gss_release_buffer(&minor, &wrapped);
            free(scratch);
            set_error(t, "encrypt_body", "wrapped token too short to split");
            return PSRP_ERR_CRYPTO;
        }
        sig = (const unsigned char *)wrapped.value;
        siglen = NTLM_SIGNATURE_BYTES;
        enc = sig + NTLM_SIGNATURE_BYTES;
        enclen = wrapped.length - NTLM_SIGNATURE_BYTES;
    }

    if (getenv("PSRP_GSS_TRACE"))
        fprintf(stderr, "[enc] iov=%d plain=%zu sig=%zu ct=%zu conf=%d\n",
                (int)used_iov, slen, siglen, enclen, conf);

    /* Never send a body the context declined to encrypt. */
    if (!conf) {
        set_error(t, "encrypt_body", "context did not provide confidentiality");
        rc = PSRP_ERR_CRYPTO;
        goto done;
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

    siglen_le[0] = (uint8_t)(siglen & 0xFF);
    siglen_le[1] = (uint8_t)((siglen >> 8) & 0xFF);
    siglen_le[2] = (uint8_t)((siglen >> 16) & 0xFF);
    siglen_le[3] = (uint8_t)((siglen >> 24) & 0xFF);

    rc = psrp_buffer_append_str(out, head);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, siglen_le, 4);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, sig, siglen);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, enc, enclen);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out,
                                                   "--" ENC_BOUNDARY "--\r\n");

done:
    if (used_iov) gss_release_iov_buffer(&minor, iov, 3);
    else gss_release_buffer(&minor, &wrapped);
    free(scratch);
    return rc;
}

/* Recovers the SOAP from a multipart/encrypted response. */
static psrp_result_t decrypt_body(winrm_session_t *t, const void *resp,
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
static psrp_result_t soap_call(winrm_session_t *t, const char *soap,
                               psrp_buffer_t *reply, bool is_receive)
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
        const char *body = NULL;
        const char *m;

        psrp_buffer_init(&fault);
        snprintf(detail, sizeof detail, "HTTP %ld", status);

        /* A fault is usually encrypted like anything else, but not always:
         * WinRM answers a request it could not even parse in the clear,
         * and those are exactly the faults worth reading. Try the encrypted
         * form, then fall back to reading the response as it arrived. */
        if (decrypt_body(t, t->resp.data, t->resp.len, &fault) == PSRP_OK &&
            fault.len)
            body = (const char *)fault.data;
        else if (t->resp.len &&
                 psrp_buffer_append(&t->resp, "", 1) == PSRP_OK)
            body = (const char *)t->resp.data;

        /* WinRM answers a Receive that waited out its OperationTimeout with a
         * fault, not an empty body: wsman:TimedOut, code 2150858793. That is
         * the ordinary "nothing arrived yet" reply and the caller asked for
         * it by naming a timeout, so it is PSRP_ERR_TRUNCATED rather than a
         * failure. Nothing hit this while every operation timeout was the
         * session's sixty seconds, because data always arrived first.
         *
         * Only for Receive: a Create or a Send that times out really has
         * failed. */
        if (body && is_receive &&
            (strstr(body, "2150858793") || strstr(body, "TimedOut"))) {
            psrp_buffer_free(&fault);
            return PSRP_ERR_TRUNCATED;
        }

        if (body) {
            m = strstr(body, "<f:Message");
            if (!m) m = strstr(body, "<s:Text");
            if (!m) m = strstr(body, "<f:Detail");
            snprintf(detail, sizeof detail, "HTTP %ld: %.180s", status,
                     m ? m : body);
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
/* `op_timeout_ms` is how long the SERVER may hold this request open before
 * answering; 0 takes the session's own timeout. It matters for Receive, where
 * a caller says how long it is prepared to wait and WinRM decides whether to
 * answer early or hold on -- see the note on winrm_receive. It is clamped to
 * the session timeout because that is what bounds curl's own wait: an
 * operation the server may hold longer than curl will wait produces a
 * client-side abort rather than an empty answer. */
static psrp_result_t envelope_head(winrm_session_t *t, psrp_buffer_t *b,
                                   const char *action, bool with_shell,
                                   uint32_t op_timeout_ms)
{
    char msgid[PSRP_GUID_BUF_SIZE];
    char shell[64];   /* the server's ShellId, which need not be a GUID */
    psrp_guid_t id;
    char *head;
    psrp_result_t rc;
    size_t cap = 4096;

    if (!op_timeout_ms || op_timeout_ms > t->timeout_ms)
        op_timeout_ms = t->timeout_ms;

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
        "<p:SessionId s:mustUnderstand=\"false\">uuid:%s</p:SessionId>"
        "<wsman:OperationTimeout>PT%u.%03uS</wsman:OperationTimeout>"
        "%s%s%s",
        t->url, action, MAX_ENVELOPE_SIZE, msgid, t->session_id,
        op_timeout_ms / 1000u, op_timeout_ms % 1000u,
        with_shell ? "<wsman:SelectorSet><wsman:Selector Name=\"ShellId\">" : "",
        with_shell ? shell : "",
        with_shell ? "</wsman:Selector></wsman:SelectorSet>" : "");

    rc = psrp_buffer_append_str(b, head);
    free(head);
    return rc;
}

/* ------------------------------------------------------------ lifetime -- */

psrp_result_t winrm_session_open(const winrm_config_t *cfg,
                                          winrm_session_t **out)
{
    winrm_session_t *t;
    OM_uint32 major, minor;
    gss_buffer_desc ubuf, pbuf, tbuf;
    gss_name_t user_name = GSS_C_NO_NAME;
    const char *conn;
    char spn[300];
    const char *hoststart, *hostend;
    winrm_auth_t auth;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    t = (winrm_session_t *)calloc(1, sizeof *t);
    if (!t) return PSRP_ERR_NOMEM;

    psrp_buffer_init(&t->rx);
    psrp_buffer_init(&t->resp);
    t->gss = GSS_C_NO_CONTEXT;

    /* One identity for the life of this client, sent on every request. */
    {
        psrp_guid_t sid;
        if (psrp_guid_generate(&sid) != PSRP_OK ||
            psrp_guid_format(&sid, t->session_id, sizeof t->session_id)
                != PSRP_OK) {
            free(t);
            return PSRP_ERR_INTERNAL;
        }
    }
    t->cred = GSS_C_NO_CREDENTIAL;
    t->target = GSS_C_NO_NAME;
    t->timeout_ms = (cfg && cfg->operation_timeout_ms)
                        ? cfg->operation_timeout_ms : DEFAULT_TIMEOUT_MS;
    auth = cfg ? cfg->auth : WINRM_AUTH_DEFAULT;
    if (auth == WINRM_AUTH_DEFAULT) {
        /* Resolve the default against the credential we were actually given;
         * see the discussion on winrm_auth_t. A password means NTLM, because
         * SPNEGO cannot encrypt what it authenticates that way; no password
         * means SPNEGO, which is the only one of the two that can use a
         * ticket cache. */
        auth = (cfg && cfg->password && *cfg->password)
             ? WINRM_AUTH_NTLM : WINRM_AUTH_NEGOTIATE;
    }
    t->mech = wire_mech_for(auth);
    t->require = required_mech_for(auth);
    t->negotiated = WINRM_AUTH_DEFAULT;

    /* With no username, GSS_C_NO_CREDENTIAL means "whatever the environment
     * already holds" -- for Kerberos, the ticket cache kinit filled in. That
     * is the ordinary way a Kerberos client authenticates, and it is why a
     * password is optional rather than required here. */

    conn = (cfg && cfg->connection) ? cfg->connection
                                    : "http://localhost:5985/wsman";
    t->url = strdup(conn);
    if (!t->url) { winrm_session_free(t); return PSRP_ERR_NOMEM; }

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
        if (!t->user || !t->pass) { winrm_session_free(t); return PSRP_ERR_NOMEM; }

        ubuf.value = t->user;
        ubuf.length = strlen(t->user);
        major = gss_import_name(&minor, &ubuf, GSS_C_NT_USER_NAME, &user_name);
        if (major != GSS_S_COMPLETE) {
            set_gss_error(t, "gss_import_name", major, minor);
            winrm_session_free(t);
            return PSRP_ERR_TRANSPORT;
        }

        /* See note 3: curl will not do this, so we must. */
        pbuf.value = t->pass;
        pbuf.length = strlen(t->pass);
        {
            /* A credential for the mechanism that has to be used, which for
             * SPNEGO is what constrains what it may offer. */
            gss_OID_set_desc mechs = { 1, t->require ? t->require : t->mech };
            major = gss_acquire_cred_with_password(&minor, user_name, &pbuf,
                                                   GSS_C_INDEFINITE, &mechs,
                                                   GSS_C_INITIATE, &t->cred,
                                                   NULL, NULL);
        }
        gss_release_name(&minor, &user_name);
        if (major != GSS_S_COMPLETE) {
            set_gss_error(t, "gss_acquire_cred_with_password", major, minor);
            winrm_session_free(t);
            return PSRP_ERR_TRANSPORT;
        }
    } else if (t->require) {
        /* No password: the credential comes from the ticket cache. It still
         * has to be narrowed to the required mechanism, or SPNEGO is free to
         * fall back to something else and "forced Kerberos" would mean
         * nothing. */
        gss_OID_set_desc mechs = { 1, t->require };
        major = gss_acquire_cred(&minor, GSS_C_NO_NAME, GSS_C_INDEFINITE,
                                 &mechs, GSS_C_INITIATE, &t->cred,
                                 NULL, NULL);
        if (major != GSS_S_COMPLETE) {
            set_gss_error(t, "gss_acquire_cred", major, minor);
            winrm_session_free(t);
            return PSRP_ERR_TRANSPORT;
        }
    }

    tbuf.value = spn;
    tbuf.length = strlen(spn);
    major = gss_import_name(&minor, &tbuf, GSS_C_NT_HOSTBASED_SERVICE,
                            &t->target);
    if (major != GSS_S_COMPLETE) {
        set_gss_error(t, "gss_import_name(spn)", major, minor);
        winrm_session_free(t);
        return PSRP_ERR_TRANSPORT;
    }

    /* Paired with curl_global_cleanup in winrm_session_free. libcurl
     * refcounts these, so nesting is correct when a caller holds several
     * transports. Without the pair libcurl still initialises itself on the
     * first easy handle but never releases that state, which valgrind reports
     * as a definite leak with no frame of ours in it. libcurl documents this
     * call as not thread-safe, so a caller creating transports from several
     * threads at once should call curl_global_init itself first. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        winrm_session_free(t);
        return PSRP_ERR_INTERNAL;
    }
    t->curl_global = true;

    t->curl = curl_easy_init();
    if (!t->curl) { winrm_session_free(t); return PSRP_ERR_INTERNAL; }

    *out = t;
    return PSRP_OK;
}

void winrm_session_free(winrm_session_t *t)
{
    OM_uint32 minor;

    if (!t) return;
    if (t->have_shell) (void)winrm_shell_delete(t);
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

const char *winrm_last_error(const winrm_session_t *t)
{
    if (!t) return "no transport";
    return t->last_error[0] ? t->last_error : "no error";
}

bool winrm_is_disconnected(const winrm_session_t *t)
{
    return t && t->disconnected;
}

winrm_auth_t winrm_negotiated_auth(const winrm_session_t *t)
{
    return t ? t->negotiated : WINRM_AUTH_DEFAULT;
}

bool winrm_command_done(const winrm_session_t *t)
{
    return t && t->command_done;
}

/* --------------------------------------------------------- operations -- */

/* Pulls every rsp:Stream payload out of a Receive response, base64-decoding
 * each into `out`, and notices the terminal CommandState. Parsing goes
 * through the same pull-parser seam the protocol code uses, so there is no
 * second XML implementation in this file. */
static psrp_result_t collect_streams(winrm_session_t *t, const char *xml,
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

/* Appends the text of the first element with this local name to `out`.
 *
 * The text is COPIED. psrp_xml_value points into the reader, so returning
 * that pointer to a caller who reads it after the reader is freed is a
 * use-after-free -- one this function existed in exactly that shape long
 * enough to corrupt a ShellId and produce an HTTP 400 that looked like a
 * decryption failure.
 *
 * Namespace-blind on purpose: these responses put the element we want in
 * whichever namespace the operation belongs to, and the local name is
 * unambiguous within one reply. */
static psrp_result_t element_text(const void *xml, size_t n, const char *want,
                                  psrp_buffer_t *out)
{
    psrp_xml_reader_t *r = NULL;
    psrp_xml_node_t node;
    bool in_it = false;
    psrp_result_t rc = PSRP_ERR_NOT_FOUND;

    if (psrp_xml_reader_create(xml, n, &r) != PSRP_OK) return PSRP_ERR_XML;

    while (psrp_xml_read(r, &node) == PSRP_OK && node != PSRP_XML_EOF) {
        if (node == PSRP_XML_ELEMENT) {
            in_it = strcmp(psrp_xml_local_name(r), want) == 0;
        } else if (node == PSRP_XML_TEXT && in_it) {
            size_t vlen = 0;
            const char *v = psrp_xml_value(r, &vlen);
            if (vlen) rc = psrp_buffer_append(out, v, vlen);
            break;
        } else {
            in_it = false;
        }
    }
    psrp_xml_reader_free(r);
    return rc;
}

/* <element xmlns="...powershell">base64</element>, the open content of a
 * wxf:Create or a wxf:Connect.
 *
 * The element name is not decoration: a Create carrying connectXml, or a
 * Connect carrying creationXml, reaches the PowerShell plugin and comes back
 * as a .NET exception, because the payload it went looking for is not
 * there. */
static psrp_result_t append_open_content(psrp_buffer_t *soap,
                                         const char *element,
                                         const void *payload, size_t len)
{
    psrp_buffer_t b64;
    psrp_result_t rc;

    psrp_buffer_init(&b64);
    rc = psrp_base64_encode_buf(&b64, payload, len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap, "<");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap, element);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap,
        " xmlns=\"http://schemas.microsoft.com/powershell\">");
    if (rc == PSRP_OK) rc = psrp_buffer_append(soap, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap, "</");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap, element);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(soap, ">");
    psrp_buffer_free(&b64);
    return rc;
}

psrp_result_t winrm_shell_create(winrm_session_t *t, const char *shell_id,
                                 const void *open_content, size_t len)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t || !shell_id || (len && !open_content)) return PSRP_ERR_INVALID_ARG;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_XFER "/Create", false, 0);
    if (rc != PSRP_OK) goto done;

    /* The protocolversion option is required and the server faults without
     * it. The name is lower-case; PowerShell sends it that way. */
    rc = psrp_buffer_append_str(&soap,
        "<wsman:OptionSet s:mustUnderstand=\"true\">"
        "<wsman:Option Name=\"protocolversion\" MustComply=\"true\">2.2"
        "</wsman:Option></wsman:OptionSet>"
        "</s:Header><s:Body><rsp:Shell ShellId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, shell_id);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "\"><rsp:InputStreams>stdin pr</rsp:InputStreams>"
        "<rsp:OutputStreams>stdout</rsp:OutputStreams>");
    if (rc == PSRP_OK)
        rc = append_open_content(&soap, "creationXml", open_content, len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Shell></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, false);
    if (rc != PSRP_OK) goto done;

    /* Take the ShellId the server reports rather than the one asked for. It
     * is usually the same, but "usually" is not a basis for every later
     * selector, and a mismatch surfaces as the shell having vanished. */
    {
        psrp_buffer_t id;

        psrp_buffer_init(&id);
        snprintf(t->shell_sel, sizeof t->shell_sel, "%s", shell_id);
        if (element_text(reply.data, reply.len, "ShellId", &id) == PSRP_OK &&
            id.len && id.len < sizeof t->shell_sel)
            snprintf(t->shell_sel, sizeof t->shell_sel, "%.*s",
                     (int)id.len, (const char *)id.data);
        psrp_buffer_free(&id);
    }
    t->have_shell = true;

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t winrm_receive(winrm_session_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms)
{
    psrp_buffer_t soap, reply;
    char cmd[64];   /* the server's CommandId, which need not be a GUID */
    psrp_result_t rc;

    if (!t || !out) return PSRP_ERR_INVALID_ARG;

    /* The caller's wait becomes the envelope's OperationTimeout, which is the
     * only thing that bounds it: WinRM holds a Receive open until either data
     * arrives or that timeout expires, so ignoring the parameter -- as this
     * did -- turns "wait up to 250ms" into "wait up to the session timeout",
     * a factor of two hundred. Callers budget in these units. The one that
     * showed it was a pump loop crediting itself 250ms per call while the
     * server held each one for a minute, so a pipeline it meant to stop after
     * a second and a half ran to completion instead.
     *
     * Floored, because a timeout of zero asks the server to answer instantly
     * and turns a pump loop into a spin. */
    if (timeout_ms && timeout_ms < MIN_RECEIVE_TIMEOUT_MS)
        timeout_ms = MIN_RECEIVE_TIMEOUT_MS;

    /* Anything left over from a previous response goes first. */
    if (t->rx.len) {
        rc = psrp_buffer_append(out, t->rx.data, t->rx.len);
        psrp_buffer_reset(&t->rx);
        return rc;
    }
    if (!t->have_shell) return PSRP_ERR_STATE;

    /* A Receive asks for one stream: the shell's, or one command's. Naming
     * the command as soon as one exists -- which is what this did -- means
     * every pool-level message after the first pipeline is never asked for,
     * so RUNSPACE_AVAILABILITY replies, USER_EVENTs and pool-addressed host
     * calls simply stop arriving. That is TODO PSRP-21, found and fixed once
     * already in the Windows client, and reintroduced here because a second
     * transport is a second chance at the same mistake.
     *
     * PowerShell's own client keeps two Receives outstanding at once. This
     * one is synchronous over a single connection, so it alternates instead:
     * each call asks for the other stream, and a caller's pump loop drains
     * both. The cost is asking for each half as often, which is why the
     * caller's timeout had to start working first -- alternating while every
     * Receive blocks for the session timeout would be worse than the bug.
     *
     * Only while a command is running; before and after, the shell stream is
     * the only one there is. */
    cmd[0] = '\0';
    if (t->have_command) {
        if (!t->rx_shell_turn) snprintf(cmd, sizeof cmd, "%s", t->cmd_sel);
        t->rx_shell_turn = !t->rx_shell_turn;
    }

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_SHELL "/Receive", true,
                       timeout_ms);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Receive><rsp:DesiredStream");
    if (rc == PSRP_OK && cmd[0]) {
        rc = psrp_buffer_append_str(&soap, " CommandId=\"");
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, cmd);
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"");
    }
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        ">stdout</rsp:DesiredStream></rsp:Receive></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, true);
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

psrp_result_t winrm_shell_delete(winrm_session_t *t)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_XFER "/Delete", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body/></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply, false);

    /* Closed exactly once, whatever the server answered. */
    t->have_shell = false;
    t->have_command = false;

    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
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
static psrp_result_t send_on_stream(winrm_session_t *t, const char *stream,
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
    if (rc == PSRP_OK) rc = envelope_head(t, &soap, WS_SHELL "/Send", true, 0);
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
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply, false);

    psrp_buffer_free(&soap);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t winrm_command(winrm_session_t *t, const char *command_id,
                            const char *command_line,
                            const void *arguments, size_t len)
{
    psrp_buffer_t soap, b64, reply;
    psrp_result_t rc;

    if (!t || !command_id) return PSRP_ERR_INVALID_ARG;
    if (len && !arguments) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;

    t->command_done = false;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&b64);
    psrp_buffer_init(&reply);

    rc = psrp_base64_encode_buf(&b64, arguments, len);
    if (rc == PSRP_OK) rc = envelope_head(t, &soap, WS_SHELL "/Command", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:CommandLine CommandId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, command_id);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"><rsp:Command>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
                                                   command_line ? command_line : "");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Command><rsp:Arguments>");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&soap, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Arguments></rsp:CommandLine></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, false);
    if (rc != PSRP_OK) goto done;

    /* As with the ShellId, use the identifier the server reports. */
    snprintf(t->cmd_sel, sizeof t->cmd_sel, "%s", command_id);
    (void)response_text(&reply, "CommandId", t->cmd_sel, sizeof t->cmd_sel);
    t->have_command = true;

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t winrm_send(winrm_session_t *t, const char *stream,
                         const void *data, size_t len)
{
    if (!stream) return PSRP_ERR_INVALID_ARG;
    return send_on_stream(t, stream, data, len);
}


psrp_result_t winrm_signal(winrm_session_t *t, const char *code)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t || !code || !*code) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell || !t->have_command) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    /* Targeted at the command, so it stops that command rather than the
     * shell. */
    rc = envelope_head(t, &soap, WS_SHELL "/Signal", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Signal CommandId=\"");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, t->cmd_sel);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, "\"><rsp:Code>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, code);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Code></rsp:Signal></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc == PSRP_OK) rc = soap_call(t, (const char *)soap.data, &reply, false);

    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

/* -------------------------------------------- disconnected shells --- */
/*
 * The Windows client cancels its standing Receive before disconnecting, per
 * 3.1.4.9 step 1. Here there is nothing to cancel: every operation is one
 * synchronous request and response, so no operation is ever outstanding when
 * control reaches these functions.
 */

psrp_result_t winrm_disconnect(winrm_session_t *t, uint32_t idle_timeout_ms)
{
    psrp_buffer_t soap, reply;
    char idle[96];
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;
    if (t->disconnected) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    /* Zero means "whatever the server's default is", which is expressed by
     * leaving the element out rather than by sending PT0S -- that would ask
     * for a shell that expires immediately. */
    idle[0] = '\0';
    if (idle_timeout_ms)
        snprintf(idle, sizeof idle,
                 "<rsp:IdleTimeOut>PT%u.%03uS</rsp:IdleTimeOut>",
                 idle_timeout_ms / 1000u, idle_timeout_ms % 1000u);

    rc = envelope_head(t, &soap, WS_SHELL "/Disconnect", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Disconnect>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap, idle);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Disconnect></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, false);
    if (rc == PSRP_OK) t->disconnected = true;

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t winrm_reconnect(winrm_session_t *t)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->have_shell) return PSRP_ERR_STATE;
    if (!t->disconnected) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    rc = envelope_head(t, &soap, WS_SHELL "/Reconnect", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, false);
    if (rc != PSRP_OK) goto done;
    t->disconnected = false;

    /* The Windows client has to reattach the command handle here, because a
     * handle is a live object there. A CommandId is only a selector on this
     * side, so the next Receive naming it resumes the pipeline by itself and
     * there is nothing to reattach. */

done:
    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}

psrp_result_t winrm_connect(winrm_session_t *t, const char *shell_id,
                            const void *open_content, size_t len,
                            psrp_buffer_t *response_content)
{
    psrp_buffer_t soap, reply;
    psrp_result_t rc;

    if (!t || !shell_id || (len && !open_content)) return PSRP_ERR_INVALID_ARG;
    if (t->have_shell) return PSRP_ERR_STATE;

    psrp_buffer_init(&soap);
    psrp_buffer_init(&reply);

    /* The selector is the shell being adopted, so it has to be in place
     * before the envelope header is built. Kept verbatim: what a ShellId
     * looks like is the caller's business, and the server matches it
     * exactly. */
    snprintf(t->shell_sel, sizeof t->shell_sel, "%s", shell_id);

    rc = envelope_head(t, &soap, WS_SHELL "/Connect", true, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</s:Header><s:Body><rsp:Connect>");
    /* No OptionSet, unlike a Create: see TODO PSRP-25. Any option sent with
     * mustComply on a Connect is rejected, and the version the option would
     * carry was already negotiated when the shell was created. */
    if (rc == PSRP_OK)
        rc = append_open_content(&soap, "connectXml", open_content, len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&soap,
        "</rsp:Connect></s:Body></s:Envelope>");
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&soap, 0);
    if (rc != PSRP_OK) goto done;

    rc = soap_call(t, (const char *)soap.data, &reply, false);
    if (rc != PSRP_OK) goto done;

    if (response_content) {
        /* 3.1.5.3.15 puts the server's answer in the response's open content
         * and nowhere else, so a response without it is a failed connect
         * however cheerful its status was. */
        psrp_buffer_t b64;

        psrp_buffer_init(&b64);
        rc = element_text(reply.data, reply.len, "connectResponseXml", &b64);
        if (rc != PSRP_OK) {
            psrp_buffer_free(&b64);
            set_error(t, "connect", "response carried no connectResponseXml");
            rc = PSRP_ERR_MALFORMED;
            goto done;
        }
        rc = psrp_base64_decode((const char *)b64.data, b64.len,
                                response_content);
        psrp_buffer_free(&b64);
        if (rc != PSRP_OK) goto done;
    }
    t->have_shell = true;
    t->disconnected = false;

done:
    if (rc != PSRP_OK) t->shell_sel[0] = '\0';
    psrp_buffer_free(&soap);
    psrp_buffer_free(&reply);
    return rc;
}
