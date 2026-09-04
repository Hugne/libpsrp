#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_buffer.h"

/* Cap a single allocation well below SIZE_MAX so growth arithmetic cannot
 * wrap. PSRP payloads are bounded by the transport's envelope size anyway. */
#define PSRP_BUFFER_MAX ((size_t)1 << 30)

void psrp_buffer_init(psrp_buffer_t *b)
{
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void psrp_buffer_free(psrp_buffer_t *b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void psrp_buffer_reset(psrp_buffer_t *b)
{
    if (b) b->len = 0;
}

psrp_result_t psrp_buffer_reserve(psrp_buffer_t *b, size_t extra)
{
    size_t need, cap;
    uint8_t *p;

    if (!b) return PSRP_ERR_INVALID_ARG;
    if (extra == 0) return PSRP_OK;
    if (extra > PSRP_BUFFER_MAX || b->len > PSRP_BUFFER_MAX - extra)
        return PSRP_ERR_OVERFLOW;

    need = b->len + extra;
    if (need <= b->cap) return PSRP_OK;

    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > PSRP_BUFFER_MAX / 2) { cap = need; break; }
        cap *= 2;
    }

    p = (uint8_t *)realloc(b->data, cap);
    if (!p) return PSRP_ERR_NOMEM;
    b->data = p;
    b->cap = cap;
    return PSRP_OK;
}

psrp_result_t psrp_buffer_append(psrp_buffer_t *b, const void *src, size_t n)
{
    psrp_result_t rc;

    if (!b) return PSRP_ERR_INVALID_ARG;
    if (n == 0) return PSRP_OK;
    if (!src) return PSRP_ERR_INVALID_ARG;

    rc = psrp_buffer_reserve(b, n);
    if (rc != PSRP_OK) return rc;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return PSRP_OK;
}

psrp_result_t psrp_buffer_append_u8(psrp_buffer_t *b, uint8_t v)
{
    return psrp_buffer_append(b, &v, 1);
}

psrp_result_t psrp_buffer_append_u16be(psrp_buffer_t *b, uint16_t v)
{
    uint8_t t[2];
    t[0] = (uint8_t)(v >> 8);
    t[1] = (uint8_t)v;
    return psrp_buffer_append(b, t, sizeof t);
}

psrp_result_t psrp_buffer_append_u32be(psrp_buffer_t *b, uint32_t v)
{
    uint8_t t[4];
    t[0] = (uint8_t)(v >> 24);
    t[1] = (uint8_t)(v >> 16);
    t[2] = (uint8_t)(v >> 8);
    t[3] = (uint8_t)v;
    return psrp_buffer_append(b, t, sizeof t);
}

psrp_result_t psrp_buffer_append_u64be(psrp_buffer_t *b, uint64_t v)
{
    uint8_t t[8];
    int i;
    for (i = 0; i < 8; i++)
        t[i] = (uint8_t)(v >> (56 - 8 * i));
    return psrp_buffer_append(b, t, sizeof t);
}

psrp_result_t psrp_buffer_append_u32le(psrp_buffer_t *b, uint32_t v)
{
    uint8_t t[4];
    t[0] = (uint8_t)v;
    t[1] = (uint8_t)(v >> 8);
    t[2] = (uint8_t)(v >> 16);
    t[3] = (uint8_t)(v >> 24);
    return psrp_buffer_append(b, t, sizeof t);
}

psrp_result_t psrp_buffer_append_str(psrp_buffer_t *b, const char *s)
{
    if (!s) return PSRP_ERR_INVALID_ARG;
    return psrp_buffer_append(b, s, strlen(s));
}

psrp_result_t psrp_buffer_consume(psrp_buffer_t *b, size_t n)
{
    if (!b) return PSRP_ERR_INVALID_ARG;
    if (n > b->len) return PSRP_ERR_INVALID_ARG;
    if (n == 0) return PSRP_OK;
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    return PSRP_OK;
}

uint8_t *psrp_buffer_detach(psrp_buffer_t *b, size_t *len_out)
{
    uint8_t *p;
    if (!b) return NULL;
    p = b->data;
    if (len_out) *len_out = b->len;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

/* ---------------------------------------------------------------- reader -- */

void psrp_reader_init(psrp_reader_t *r, const void *data, size_t len)
{
    if (!r) return;
    r->data = (const uint8_t *)data;
    r->len = data ? len : 0;
    r->pos = 0;
}

size_t psrp_reader_remaining(const psrp_reader_t *r)
{
    if (!r || r->pos > r->len) return 0;
    return r->len - r->pos;
}

static psrp_result_t reader_need(psrp_reader_t *r, size_t n)
{
    if (!r || !r->data) return PSRP_ERR_INVALID_ARG;
    if (psrp_reader_remaining(r) < n) return PSRP_ERR_TRUNCATED;
    return PSRP_OK;
}

psrp_result_t psrp_read_u8(psrp_reader_t *r, uint8_t *out)
{
    psrp_result_t rc = reader_need(r, 1);
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = r->data[r->pos++];
    return PSRP_OK;
}

psrp_result_t psrp_read_u16be(psrp_reader_t *r, uint16_t *out)
{
    psrp_result_t rc = reader_need(r, 2);
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = (uint16_t)(((uint16_t)r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return PSRP_OK;
}

psrp_result_t psrp_read_u32be(psrp_reader_t *r, uint32_t *out)
{
    psrp_result_t rc = reader_need(r, 4);
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = ((uint32_t)r->data[r->pos] << 24) |
           ((uint32_t)r->data[r->pos + 1] << 16) |
           ((uint32_t)r->data[r->pos + 2] << 8) |
           ((uint32_t)r->data[r->pos + 3]);
    r->pos += 4;
    return PSRP_OK;
}

psrp_result_t psrp_read_u64be(psrp_reader_t *r, uint64_t *out)
{
    psrp_result_t rc = reader_need(r, 8);
    uint64_t v = 0;
    int i;
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    for (i = 0; i < 8; i++)
        v = (v << 8) | r->data[r->pos + (size_t)i];
    r->pos += 8;
    *out = v;
    return PSRP_OK;
}

psrp_result_t psrp_read_u32le(psrp_reader_t *r, uint32_t *out)
{
    psrp_result_t rc = reader_need(r, 4);
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = ((uint32_t)r->data[r->pos]) |
           ((uint32_t)r->data[r->pos + 1] << 8) |
           ((uint32_t)r->data[r->pos + 2] << 16) |
           ((uint32_t)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return PSRP_OK;
}

psrp_result_t psrp_read_bytes(psrp_reader_t *r, void *dst, size_t n)
{
    psrp_result_t rc = reader_need(r, n);
    if (rc != PSRP_OK) return rc;
    if (n == 0) return PSRP_OK;
    if (!dst) return PSRP_ERR_INVALID_ARG;
    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    return PSRP_OK;
}

psrp_result_t psrp_read_borrow(psrp_reader_t *r, size_t n, const uint8_t **out)
{
    psrp_result_t rc = reader_need(r, n);
    if (rc != PSRP_OK) return rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = r->data + r->pos;
    r->pos += n;
    return PSRP_OK;
}

psrp_result_t psrp_reader_skip(psrp_reader_t *r, size_t n)
{
    psrp_result_t rc = reader_need(r, n);
    if (rc != PSRP_OK) return rc;
    r->pos += n;
    return PSRP_OK;
}
