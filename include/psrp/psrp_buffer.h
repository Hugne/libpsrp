/** @file
 * psrp_buffer.h - growable byte buffer (writer) and non-owning cursor (reader).
 *
 * PSRP is a big-endian binary protocol wrapping UTF-8 XML, so both halves
 * offer explicit big-endian integer helpers. Nothing here allocates unless it
 * has to, and every append is length-checked.
 */
#ifndef PSRP_BUFFER_H
#define PSRP_BUFFER_H

#include "psrp/psrp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ---------------------------------------------------------------- writer -- */

typedef struct psrp_buffer {
    uint8_t *data;
    size_t len;
    size_t cap;
} psrp_buffer_t;

/** Zero-initialises; no allocation. A zeroed psrp_buffer_t is always valid. */
void psrp_buffer_init(psrp_buffer_t *b);

/** Frees storage and returns the buffer to the empty state. Idempotent. */
void psrp_buffer_free(psrp_buffer_t *b);

/** Drops the contents but keeps the allocation. */
void psrp_buffer_reset(psrp_buffer_t *b);

/** Ensures room for at least `extra` more bytes beyond len. */
psrp_result_t psrp_buffer_reserve(psrp_buffer_t *b, size_t extra);

psrp_result_t psrp_buffer_append(psrp_buffer_t *b, const void *src, size_t n);
psrp_result_t psrp_buffer_append_u8(psrp_buffer_t *b, uint8_t v);
psrp_result_t psrp_buffer_append_u16be(psrp_buffer_t *b, uint16_t v);
psrp_result_t psrp_buffer_append_u32be(psrp_buffer_t *b, uint32_t v);
psrp_result_t psrp_buffer_append_u64be(psrp_buffer_t *b, uint64_t v);
psrp_result_t psrp_buffer_append_u32le(psrp_buffer_t *b, uint32_t v);
/** Appends a NUL-terminated string without its terminator. */
psrp_result_t psrp_buffer_append_str(psrp_buffer_t *b, const char *s);

/** Removes `n` bytes from the front, shifting the remainder down. Used by the
 * defragmenter as it consumes complete fragments from a receive buffer. */
psrp_result_t psrp_buffer_consume(psrp_buffer_t *b, size_t n);

/* Detaches the storage; the caller takes ownership and must free() it.
 * The buffer is left empty. */
uint8_t *psrp_buffer_detach(psrp_buffer_t *b, size_t *len_out);

/** ---------------------------------------------------------------- reader -- */

typedef struct psrp_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
} psrp_reader_t;

void psrp_reader_init(psrp_reader_t *r, const void *data, size_t len);
size_t psrp_reader_remaining(const psrp_reader_t *r);

/** All readers return PSRP_ERR_TRUNCATED (not MALFORMED) when the input simply
 * ends early, so callers can distinguish "need more bytes" from "bad bytes". */
psrp_result_t psrp_read_u8(psrp_reader_t *r, uint8_t *out);
psrp_result_t psrp_read_u16be(psrp_reader_t *r, uint16_t *out);
psrp_result_t psrp_read_u32be(psrp_reader_t *r, uint32_t *out);
psrp_result_t psrp_read_u64be(psrp_reader_t *r, uint64_t *out);
psrp_result_t psrp_read_u32le(psrp_reader_t *r, uint32_t *out);
psrp_result_t psrp_read_bytes(psrp_reader_t *r, void *dst, size_t n);
/** Borrows `n` bytes in place; no copy. Pointer is valid while the source is. */
psrp_result_t psrp_read_borrow(psrp_reader_t *r, size_t n, const uint8_t **out);
psrp_result_t psrp_reader_skip(psrp_reader_t *r, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_BUFFER_H */
