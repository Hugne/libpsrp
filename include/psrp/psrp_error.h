/** @file
 * psrp_error.h - result codes for libpsrp.
 *
 * Every fallible call returns psrp_result_t. PSRP_OK is zero; all errors are
 * negative so `if (rc)` and `if (rc < 0)` both read correctly.
 */
#ifndef PSRP_ERROR_H
#define PSRP_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum psrp_result {
    PSRP_OK = 0,

    /** caller errors */
    PSRP_ERR_INVALID_ARG = -1,   /**< NULL or out-of-range argument */
    PSRP_ERR_NOMEM = -2,         /**< allocation failed */
    PSRP_ERR_TOO_SMALL = -3,     /**< caller-supplied buffer too small */
    PSRP_ERR_STATE = -4,         /**< operation invalid in the current state */
    PSRP_ERR_NOT_FOUND = -5,

    /** wire / parsing errors */
    PSRP_ERR_MALFORMED = -6,     /**< data violates [MS-PSRP] */
    PSRP_ERR_TRUNCATED = -7,     /**< need more bytes to decode */
    PSRP_ERR_OVERFLOW = -8,      /**< length/count exceeds a protocol or size limit */
    PSRP_ERR_UNSUPPORTED = -9,   /**< well-formed but not implemented (see TODO.md) */

    /** subsystem errors */
    PSRP_ERR_TRANSPORT = -10,   /**< the carrier failed for some other reason */
    /** The server refused the credentials, or none could be obtained.
     *
     * Separate from PSRP_ERR_TRANSPORT because it is the one failure a caller
     * can usually do something about, and because "transport error" for a
     * mistyped password tells nobody anything. */
    PSRP_ERR_AUTH = -13,
    /** The endpoint could not be reached: the name did not resolve, the
     * connection was refused, or it timed out before any reply. */
    PSRP_ERR_UNREACHABLE = -14,
    PSRP_ERR_CRYPTO = -11,
    PSRP_ERR_XML = -12,

    PSRP_ERR_INTERNAL = -99
} psrp_result_t;

/** Stable, human-readable name for a result code. Never NULL. */
const char *psrp_strerror(psrp_result_t rc);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_ERROR_H */
