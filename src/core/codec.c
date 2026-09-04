#include <string.h>

#include "internal/psrp_codec.h"

/* ---------------------------------------------------------------- base64 -- */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* -1 invalid, -2 whitespace (skippable), -3 padding '='. */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -3;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return -2;
    return -1;
}

size_t psrp_base64_encoded_len(size_t raw_len)
{
    return ((raw_len + 2) / 3) * 4;
}

size_t psrp_base64_decoded_cap(size_t b64_len)
{
    return (b64_len / 4 + 1) * 3;
}

static void b64_emit(const uint8_t *in, size_t n, char out[4])
{
    uint32_t v = (uint32_t)in[0] << 16;
    if (n > 1) v |= (uint32_t)in[1] << 8;
    if (n > 2) v |= in[2];
    out[0] = b64_alphabet[(v >> 18) & 0x3F];
    out[1] = b64_alphabet[(v >> 12) & 0x3F];
    out[2] = (n > 1) ? b64_alphabet[(v >> 6) & 0x3F] : '=';
    out[3] = (n > 2) ? b64_alphabet[v & 0x3F] : '=';
}

psrp_result_t psrp_base64_encode(const void *src, size_t n, char *out, size_t out_size)
{
    const uint8_t *p = (const uint8_t *)src;
    size_t need = psrp_base64_encoded_len(n);
    size_t i, o = 0;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !src) return PSRP_ERR_INVALID_ARG;
    if (out_size < need + 1) return PSRP_ERR_TOO_SMALL;

    for (i = 0; i < n; i += 3) {
        size_t chunk = (n - i < 3) ? (n - i) : 3;
        b64_emit(p + i, chunk, out + o);
        o += 4;
    }
    out[o] = '\0';
    return PSRP_OK;
}

psrp_result_t psrp_base64_encode_buf(psrp_buffer_t *b, const void *src, size_t n)
{
    const uint8_t *p = (const uint8_t *)src;
    size_t i;
    psrp_result_t rc;

    if (!b) return PSRP_ERR_INVALID_ARG;
    if (n && !src) return PSRP_ERR_INVALID_ARG;

    rc = psrp_buffer_reserve(b, psrp_base64_encoded_len(n));
    if (rc != PSRP_OK) return rc;

    for (i = 0; i < n; i += 3) {
        char q[4];
        size_t chunk = (n - i < 3) ? (n - i) : 3;
        b64_emit(p + i, chunk, q);
        rc = psrp_buffer_append(b, q, 4);
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}

psrp_result_t psrp_base64_decode(const char *b64, size_t b64_len, psrp_buffer_t *out)
{
    size_t i;
    int quad[4];
    int nq = 0;
    int pad = 0;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (b64_len && !b64) return PSRP_ERR_INVALID_ARG;

    for (i = 0; i < b64_len; i++) {
        int v = b64_val(b64[i]);
        if (v == -2) continue;               /* whitespace */
        if (v == -1) return PSRP_ERR_MALFORMED;
        if (v == -3) {                        /* '=' */
            pad++;
            if (pad > 2) return PSRP_ERR_MALFORMED;
            quad[nq++] = 0;
        } else {
            if (pad) return PSRP_ERR_MALFORMED; /* data after padding */
            quad[nq++] = v;
        }

        if (nq == 4) {
            uint32_t v32 = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                           ((uint32_t)quad[2] << 6) | (uint32_t)quad[3];
            uint8_t t[3];
            size_t emit = 3 - (size_t)pad;
            t[0] = (uint8_t)(v32 >> 16);
            t[1] = (uint8_t)(v32 >> 8);
            t[2] = (uint8_t)v32;
            rc = psrp_buffer_append(out, t, emit);
            if (rc != PSRP_OK) return rc;
            nq = 0;
            /* Padding may only appear in the final quad. */
            if (pad) {
                size_t j;
                for (j = i + 1; j < b64_len; j++) {
                    int w = b64_val(b64[j]);
                    if (w == -2) continue;
                    return PSRP_ERR_MALFORMED;
                }
                return PSRP_OK;
            }
        }
    }

    if (nq != 0) return PSRP_ERR_MALFORMED; /* not a multiple of 4 */
    return PSRP_OK;
}

/* ------------------------------------------------------------------- hex -- */

psrp_result_t psrp_hex_encode(const void *src, size_t n, char *out, size_t out_size)
{
    static const char *digits = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)src;
    size_t i;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !src) return PSRP_ERR_INVALID_ARG;
    if (out_size < n * 2 + 1) return PSRP_ERR_TOO_SMALL;

    for (i = 0; i < n; i++) {
        out[i * 2] = digits[(p[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[p[i] & 0xF];
    }
    out[n * 2] = '\0';
    return PSRP_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

psrp_result_t psrp_hex_decode(const char *hex, size_t hex_len, psrp_buffer_t *out)
{
    size_t i;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (hex_len && !hex) return PSRP_ERR_INVALID_ARG;
    if (hex_len % 2) return PSRP_ERR_MALFORMED;

    for (i = 0; i < hex_len; i += 2) {
        int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
        uint8_t byte;
        if (hi < 0 || lo < 0) return PSRP_ERR_MALFORMED;
        byte = (uint8_t)((hi << 4) | lo);
        rc = psrp_buffer_append_u8(out, byte);
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}

/* ------------------------------------------------------------- utf-8/16 --- */

/* Decodes one code point. Returns bytes consumed, or 0 if invalid. */
static size_t utf8_decode(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint32_t c;
    if (n == 0) return 0;

    if (s[0] < 0x80) { *cp = s[0]; return 1; }

    if ((s[0] & 0xE0) == 0xC0) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        c = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        if (c < 0x80) return 0;                    /* overlong */
        *cp = c; return 2;
    }
    if ((s[0] & 0xF0) == 0xE0) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        c = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
            (uint32_t)(s[2] & 0x3F);
        if (c < 0x800) return 0;                   /* overlong */
        if (c >= 0xD800 && c <= 0xDFFF) return 0;  /* surrogate (CESU-8) */
        *cp = c; return 3;
    }
    if ((s[0] & 0xF8) == 0xF0) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80) return 0;
        c = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        if (c < 0x10000 || c > 0x10FFFF) return 0; /* overlong / out of range */
        *cp = c; return 4;
    }
    return 0;
}

bool psrp_utf8_valid(const void *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    if (n && !s) return false;
    while (i < n) {
        uint32_t cp;
        size_t used = utf8_decode(p + i, n - i, &cp);
        if (used == 0) return false;
        i += used;
    }
    return true;
}

static psrp_result_t emit_utf16(psrp_buffer_t *out, uint32_t cp)
{
    psrp_result_t rc;
    if (cp < 0x10000) {
        rc = psrp_buffer_append_u8(out, (uint8_t)(cp & 0xFF));
        if (rc != PSRP_OK) return rc;
        return psrp_buffer_append_u8(out, (uint8_t)(cp >> 8));
    } else {
        uint32_t v = cp - 0x10000;
        uint16_t hi = (uint16_t)(0xD800 + (v >> 10));
        uint16_t lo = (uint16_t)(0xDC00 + (v & 0x3FF));
        rc = psrp_buffer_append_u8(out, (uint8_t)(hi & 0xFF));
        if (rc != PSRP_OK) return rc;
        rc = psrp_buffer_append_u8(out, (uint8_t)(hi >> 8));
        if (rc != PSRP_OK) return rc;
        rc = psrp_buffer_append_u8(out, (uint8_t)(lo & 0xFF));
        if (rc != PSRP_OK) return rc;
        return psrp_buffer_append_u8(out, (uint8_t)(lo >> 8));
    }
}

psrp_result_t psrp_utf8_to_utf16(const void *utf8, size_t n, psrp_buffer_t *out)
{
    const uint8_t *p = (const uint8_t *)utf8;
    size_t i = 0;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !utf8) return PSRP_ERR_INVALID_ARG;

    while (i < n) {
        uint32_t cp;
        psrp_result_t rc;
        size_t used = utf8_decode(p + i, n - i, &cp);
        if (used == 0) return PSRP_ERR_MALFORMED;
        rc = emit_utf16(out, cp);
        if (rc != PSRP_OK) return rc;
        i += used;
    }
    return PSRP_OK;
}

static psrp_result_t emit_utf8(psrp_buffer_t *out, uint32_t cp)
{
    uint8_t t[4];
    size_t n;
    if (cp < 0x80) {
        t[0] = (uint8_t)cp; n = 1;
    } else if (cp < 0x800) {
        t[0] = (uint8_t)(0xC0 | (cp >> 6));
        t[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (uint8_t)(0xE0 | (cp >> 12));
        t[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (uint8_t)(0xF0 | (cp >> 18));
        t[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return psrp_buffer_append(out, t, n);
}

psrp_result_t psrp_utf16_to_utf8(const void *utf16, size_t n, psrp_buffer_t *out)
{
    const uint8_t *p = (const uint8_t *)utf16;
    size_t i = 0;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !utf16) return PSRP_ERR_INVALID_ARG;
    if (n % 2) return PSRP_ERR_MALFORMED;

    while (i < n) {
        uint32_t cp = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);
        psrp_result_t rc;
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;
            if (i + 2 > n) return PSRP_ERR_MALFORMED;  /* unpaired high */
            lo = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);
            if (lo < 0xDC00 || lo > 0xDFFF) return PSRP_ERR_MALFORMED;
            i += 2;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return PSRP_ERR_MALFORMED;                 /* unpaired low */
        }
        rc = emit_utf8(out, cp);
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}
