/* [MS-PSRP] 2.2.5.3.2 Encoding Strings.
 *
 * Escaping is defined over UTF-16 code units, so both directions convert
 * through UTF-16 even though the caller speaks UTF-8. That also makes
 * surrogate handling fall out for free: a non-BMP character is two code
 * units, hence two escapes, and decoding recombines the pair.
 */

#include <string.h>

#include "internal/psrp_clixml.h"
#include "internal/psrp_codec.h"

/* .NET char.IsControl: C0 plus DEL and C1. U+00A0 is deliberately excluded. */
static bool is_control_unit(uint16_t u)
{
    return u <= 0x1F || (u >= 0x7F && u <= 0x9F);
}

static bool is_surrogate_unit(uint16_t u)
{
    return u >= 0xD800 && u <= 0xDFFF;
}

static int hex_val(uint16_t c)
{
    if (c >= '0' && c <= '9') return (int)c - '0';
    if (c >= 'a' && c <= 'f') return (int)c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return (int)c - 'A' + 10;
    return -1;
}

/* Appends one BMP code unit as UTF-8. Callers must not pass surrogates. */
static psrp_result_t append_bmp_utf8(psrp_buffer_t *out, uint16_t u)
{
    uint8_t t[3];
    if (u < 0x80) {
        t[0] = (uint8_t)u;
        return psrp_buffer_append(out, t, 1);
    }
    if (u < 0x800) {
        t[0] = (uint8_t)(0xC0u | (u >> 6));
        t[1] = (uint8_t)(0x80u | (u & 0x3Fu));
        return psrp_buffer_append(out, t, 2);
    }
    t[0] = (uint8_t)(0xE0u | (u >> 12));
    t[1] = (uint8_t)(0x80u | ((u >> 6) & 0x3Fu));
    t[2] = (uint8_t)(0x80u | (u & 0x3Fu));
    return psrp_buffer_append(out, t, 3);
}

static psrp_result_t append_escape(psrp_buffer_t *out, uint16_t u)
{
    static const char *digits = "0123456789ABCDEF";   /* uppercase, per PS */
    char esc[7];
    esc[0] = '_';
    esc[1] = 'x';
    esc[2] = digits[(u >> 12) & 0xF];
    esc[3] = digits[(u >> 8) & 0xF];
    esc[4] = digits[(u >> 4) & 0xF];
    esc[5] = digits[u & 0xF];
    esc[6] = '_';
    return psrp_buffer_append(out, esc, sizeof esc);
}

static uint16_t unit_at(const psrp_buffer_t *u16, size_t index)
{
    const uint8_t *p = u16->data + index * 2;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

psrp_result_t psrp_clixml_encode_string(const void *utf8, size_t n,
                                        psrp_buffer_t *out)
{
    psrp_buffer_t u16;
    psrp_result_t rc;
    size_t count, i;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !utf8) return PSRP_ERR_INVALID_ARG;

    psrp_buffer_init(&u16);
    rc = psrp_utf8_to_utf16(utf8, n, &u16);
    if (rc != PSRP_OK) goto done;

    count = u16.len / 2;
    for (i = 0; i < count; i++) {
        uint16_t u = unit_at(&u16, i);

        if (is_control_unit(u) || is_surrogate_unit(u)) {
            rc = append_escape(out, u);
        } else if (u == '_' && i + 1 < count) {
            uint16_t next = unit_at(&u16, i + 1);
            /* PowerShell escapes '_' whenever 'x' or 'X' follows, without
             * validating that a real escape sequence comes after it. */
            if (next == 'x' || next == 'X')
                rc = append_escape(out, u);
            else
                rc = append_bmp_utf8(out, u);
        } else {
            rc = append_bmp_utf8(out, u);
        }
        if (rc != PSRP_OK) goto done;
    }

done:
    psrp_buffer_free(&u16);
    return rc;
}

psrp_result_t psrp_clixml_decode_string(const void *text, size_t n,
                                        psrp_buffer_t *out)
{
    psrp_buffer_t u16;
    psrp_result_t rc = PSRP_OK;
    const uint8_t *p = (const uint8_t *)text;
    size_t i = 0;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !text) return PSRP_ERR_INVALID_ARG;

    psrp_buffer_init(&u16);

    while (i < n) {
        /* An escape is exactly 7 ASCII bytes: _ x H H H H _ */
        if (p[i] == '_' && n - i >= 7 && (p[i + 1] == 'x' || p[i + 1] == 'X') &&
            p[i + 6] == '_') {
            int h0 = hex_val(p[i + 2]), h1 = hex_val(p[i + 3]);
            int h2 = hex_val(p[i + 4]), h3 = hex_val(p[i + 5]);
            if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                unsigned v = ((unsigned)h0 << 12) | ((unsigned)h1 << 8) |
                             ((unsigned)h2 << 4) | (unsigned)h3;
                rc = psrp_buffer_append_u8(&u16, (uint8_t)(v & 0xFFu));
                if (rc != PSRP_OK) goto done;
                rc = psrp_buffer_append_u8(&u16, (uint8_t)(v >> 8));
                if (rc != PSRP_OK) goto done;
                i += 7;
                continue;
            }
            /* Not valid hex: fall through and treat '_' literally. */
        }

        /* Decode one UTF-8 character and append it as UTF-16. */
        {
            psrp_buffer_t one;
            size_t used;
            uint8_t b = p[i];
            if (b < 0x80) used = 1;
            else if ((b & 0xE0) == 0xC0) used = 2;
            else if ((b & 0xF0) == 0xE0) used = 3;
            else if ((b & 0xF8) == 0xF0) used = 4;
            else { rc = PSRP_ERR_MALFORMED; goto done; }
            if (i + used > n) { rc = PSRP_ERR_MALFORMED; goto done; }

            psrp_buffer_init(&one);
            rc = psrp_utf8_to_utf16(p + i, used, &one);
            if (rc == PSRP_OK)
                rc = psrp_buffer_append(&u16, one.data, one.len);
            psrp_buffer_free(&one);
            if (rc != PSRP_OK) goto done;
            i += used;
        }
    }

    /* Converting back through UTF-16 recombines any surrogate pairs that were
     * written as two separate escapes. */
    rc = psrp_utf16_to_utf8(u16.data, u16.len, out);

done:
    psrp_buffer_free(&u16);
    return rc;
}
