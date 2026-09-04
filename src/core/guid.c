#include <string.h>

#include "psrp/psrp_types.h"

const psrp_guid_t psrp_guid_empty = { { 0 } };

bool psrp_guid_equal(const psrp_guid_t *a, const psrp_guid_t *b)
{
    if (!a || !b) return false;
    return memcmp(a->bytes, b->bytes, sizeof a->bytes) == 0;
}

bool psrp_guid_is_empty(const psrp_guid_t *g)
{
    return psrp_guid_equal(g, &psrp_guid_empty);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

psrp_result_t psrp_guid_parse(const char *str, psrp_guid_t *out)
{
    /* Hyphen positions in "8-4-4-4-12". */
    static const int hyphens[4] = { 8, 13, 18, 23 };
    size_t i;
    int bi = 0;
    int h;

    if (!str || !out) return PSRP_ERR_INVALID_ARG;
    if (strlen(str) != PSRP_GUID_STR_LEN) return PSRP_ERR_MALFORMED;

    for (i = 0; i < PSRP_GUID_STR_LEN; i++) {
        bool is_hyphen_pos = (i == (size_t)hyphens[0] || i == (size_t)hyphens[1] ||
                              i == (size_t)hyphens[2] || i == (size_t)hyphens[3]);
        if (is_hyphen_pos) {
            if (str[i] != '-') return PSRP_ERR_MALFORMED;
            continue;
        }
        if (hex_val(str[i]) < 0) return PSRP_ERR_MALFORMED;
    }

    for (i = 0; i < PSRP_GUID_STR_LEN; i++) {
        int hi, lo;
        if (str[i] == '-') continue;
        hi = hex_val(str[i]);
        lo = hex_val(str[i + 1]);
        if (lo < 0) return PSRP_ERR_MALFORMED;
        h = (hi << 4) | lo;
        out->bytes[bi++] = (uint8_t)h;
        i++; /* consumed two characters */
    }

    return (bi == 16) ? PSRP_OK : PSRP_ERR_MALFORMED;
}

psrp_result_t psrp_guid_format(const psrp_guid_t *g, char *out, size_t out_size)
{
    static const char *digits = "0123456789abcdef";
    size_t i, o = 0;

    if (!g || !out) return PSRP_ERR_INVALID_ARG;
    if (out_size < PSRP_GUID_BUF_SIZE) return PSRP_ERR_TOO_SMALL;

    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = digits[(g->bytes[i] >> 4) & 0xF];
        out[o++] = digits[g->bytes[i] & 0xF];
    }
    out[o] = '\0';
    return PSRP_OK;
}

/* .NET's Guid.ToByteArray() emits the first three fields little-endian and the
 * final eight bytes in order. [MS-PSRP] message headers carry GUIDs in that
 * layout, so convert explicitly rather than memcpy. */
void psrp_guid_to_wire(const psrp_guid_t *g, uint8_t out[16])
{
    if (!g || !out) return;
    out[0] = g->bytes[3];
    out[1] = g->bytes[2];
    out[2] = g->bytes[1];
    out[3] = g->bytes[0];
    out[4] = g->bytes[5];
    out[5] = g->bytes[4];
    out[6] = g->bytes[7];
    out[7] = g->bytes[6];
    memcpy(out + 8, g->bytes + 8, 8);
}

void psrp_guid_from_wire(const uint8_t in[16], psrp_guid_t *out)
{
    if (!in || !out) return;
    out->bytes[0] = in[3];
    out->bytes[1] = in[2];
    out->bytes[2] = in[1];
    out->bytes[3] = in[0];
    out->bytes[4] = in[5];
    out->bytes[5] = in[4];
    out->bytes[6] = in[7];
    out->bytes[7] = in[6];
    memcpy(out->bytes + 8, in + 8, 8);
}
