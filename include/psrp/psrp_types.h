/* psrp_types.h - fundamental types shared across libpsrp. */
#ifndef PSRP_TYPES_H
#define PSRP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psrp/psrp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A GUID as 16 raw bytes in RFC 4122 / big-endian textual order, i.e. the
 * order the characters appear in "aabbccdd-eeff-...". The .NET/little-endian
 * wire layout used by PSRP message headers is converted explicitly by the
 * message layer; see psrp_guid_to_wire/psrp_guid_from_wire. Keeping the
 * in-memory form canonical avoids endianness confusion everywhere else. */
typedef struct psrp_guid {
    uint8_t bytes[16];
} psrp_guid_t;

/* Length of "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", excluding the NUL. */
#define PSRP_GUID_STR_LEN 36
/* Buffer size for psrp_guid_format(), including the NUL. */
#define PSRP_GUID_BUF_SIZE (PSRP_GUID_STR_LEN + 1)

/* The all-zero GUID, used by the message header when no pipeline applies. */
extern const psrp_guid_t psrp_guid_empty;

bool psrp_guid_equal(const psrp_guid_t *a, const psrp_guid_t *b);
bool psrp_guid_is_empty(const psrp_guid_t *g);

/* Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (case-insensitive). Optional
 * surrounding braces are rejected: callers should strip them first.
 * Returns PSRP_ERR_MALFORMED if the text is not exactly 36 valid characters. */
psrp_result_t psrp_guid_parse(const char *str, psrp_guid_t *out);

/* Format lowercase, hyphenated, NUL-terminated. `out` must be at least
 * PSRP_GUID_BUF_SIZE bytes. */
psrp_result_t psrp_guid_format(const psrp_guid_t *g, char *out, size_t out_size);

/* Fills `out` with a random (RFC 4122 version 4) GUID from the platform's
 * cryptographic RNG. Runspace pool and pipeline ids must be unique, so this
 * must not fall back to a predictable source; it returns PSRP_ERR_INTERNAL if
 * the platform RNG is unavailable. */
psrp_result_t psrp_guid_generate(psrp_guid_t *out);

/* Convert to/from the little-endian field layout .NET's Guid uses on the wire
 * (first three fields byte-swapped, last eight bytes as-is). */
void psrp_guid_to_wire(const psrp_guid_t *g, uint8_t out[16]);
void psrp_guid_from_wire(const uint8_t in[16], psrp_guid_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TYPES_H */
