/* Platform RNG. Kept in its own file so the rest of the library stays
 * portable C; a POSIX version would sit alongside this one.
 */

#include <windows.h>
#include <bcrypt.h>

#include "psrp/psrp_types.h"

psrp_result_t psrp_guid_generate(psrp_guid_t *out)
{
    NTSTATUS st;

    if (!out) return PSRP_ERR_INVALID_ARG;

    st = BCryptGenRandom(NULL, out->bytes, (ULONG)sizeof out->bytes,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) return PSRP_ERR_INTERNAL;

    /* RFC 4122 section 4.4: set the version to 4 and the variant to RFC 4122.
     * The canonical byte order puts the version nibble in byte 6 and the
     * variant bits in byte 8. */
    out->bytes[6] = (uint8_t)((out->bytes[6] & 0x0F) | 0x40);
    out->bytes[8] = (uint8_t)((out->bytes[8] & 0x3F) | 0x80);
    return PSRP_OK;
}
