/* psrp_codec.h - internal encoding helpers: base64, hex, UTF-8/UTF-16.
 *
 * Not part of the public API. UTF-16 conversion exists because XmlLite (our
 * XML backend) works in UTF-16 while PSRP payloads are UTF-8 on the wire.
 */
#ifndef PSRP_CODEC_H
#define PSRP_CODEC_H

#include "psrp/psrp_types.h"
#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- base64 -- */

/* Number of bytes psrp_base64_encode writes, excluding the NUL. */
size_t psrp_base64_encoded_len(size_t raw_len);
/* Upper bound on decoded size for a base64 text of this length. */
size_t psrp_base64_decoded_cap(size_t b64_len);

/* Writes standard base64 with '=' padding plus a NUL. `out_size` must be at
 * least psrp_base64_encoded_len(n) + 1. */
psrp_result_t psrp_base64_encode(const void *src, size_t n, char *out, size_t out_size);

/* Appends standard base64 (no NUL) to `b`. */
psrp_result_t psrp_base64_encode_buf(psrp_buffer_t *b, const void *src, size_t n);

/* Decodes `b64_len` characters. Whitespace (space, tab, CR, LF) is skipped so
 * line-wrapped base64 from XML decodes cleanly. Returns PSRP_ERR_MALFORMED on
 * an invalid character, bad padding, or a length that is not a multiple of 4
 * after whitespace removal. */
psrp_result_t psrp_base64_decode(const char *b64, size_t b64_len,
                                 psrp_buffer_t *out);

/* ------------------------------------------------------------------- hex -- */

/* Lowercase hex, NUL-terminated; out_size must be >= 2*n + 1. */
psrp_result_t psrp_hex_encode(const void *src, size_t n, char *out, size_t out_size);
/* Accepts upper or lower case; rejects odd lengths and non-hex characters. */
psrp_result_t psrp_hex_decode(const char *hex, size_t hex_len, psrp_buffer_t *out);

/* ------------------------------------------------------------- utf-8/16 --- */

/* Validates a UTF-8 sequence: rejects overlong forms, surrogates encoded as
 * UTF-8 (CESU-8), and code points above U+10FFFF. */
bool psrp_utf8_valid(const void *s, size_t n);

/* Converts UTF-8 to UTF-16LE. Appends code units (2 bytes each, no BOM, no
 * terminator) to `out`. Invalid input yields PSRP_ERR_MALFORMED. */
psrp_result_t psrp_utf8_to_utf16(const void *utf8, size_t n, psrp_buffer_t *out);

/* Converts UTF-16LE (n = number of BYTES) to UTF-8, appending to `out`.
 * Unpaired surrogates yield PSRP_ERR_MALFORMED. */
psrp_result_t psrp_utf16_to_utf8(const void *utf16, size_t n, psrp_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_CODEC_H */
