/** @file
 * psrp_timezone.h - TimeZone ([MS-PSRP] 2.2.3.10).
 *
 * SESSION_CAPABILITY carries the client's time zone as a byte array holding a
 * .NET object graph serialized in MS-NRBF, the binary remoting format. It is
 * the one place PSRP reaches outside its own CLIXML encoding, and the graph is
 * fixed: a System.CurrentSystemTimeZone with four fields (2.2.3.10.1).
 *
 * The m_CachedDaylightChanges hashtable exists only to cache values that can
 * be recomputed from the other fields, and 2.2.3.10.1 says outright that it
 * MAY be ignored. It is therefore always written empty, which is also what
 * .NET itself produces for a freshly read time zone.
 */
#ifndef PSRP_TIMEZONE_H
#define PSRP_TIMEZONE_H

#include "psrp/psrp_buffer.h"
#include "psrp/psrp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Windows time zone names are at most 32 UTF-16 units; 128 bytes leaves room
 * for any of them in UTF-8 with the terminator. */
#define PSRP_TIMEZONE_NAME_MAX 128

typedef struct psrp_timezone {
    /** Standard offset from UTC in 100-nanosecond ticks, ignoring daylight
     * saving. The spec's own example is -8 hours for Pacific Standard Time.
     * Note the sign: this is the offset *to* UTC as .NET stores it, so a zone
     * ahead of UTC has a positive value. */
    int64_t ticks_offset;

    /** Either name may be absent, which is written as a null field. .NET fills
     * these lazily, so a time zone that has not been asked for its names
     * really does serialize with both null. An empty daylight name is the
     * documented way to say the zone has no daylight saving. */
    bool has_standard_name;
    bool has_daylight_name;
    char standard_name[PSRP_TIMEZONE_NAME_MAX];
    char daylight_name[PSRP_TIMEZONE_NAME_MAX];
} psrp_timezone_t;

/** Fills in the machine's current time zone. Windows only. */
psrp_result_t psrp_timezone_current(psrp_timezone_t *out);

/** Writes the MS-NRBF graph. The result is exactly the byte array that goes in
 * the SESSION_CAPABILITY TimeZone property. */
psrp_result_t psrp_timezone_serialize(const psrp_timezone_t *tz,
                                      psrp_buffer_t *out);

/** Reads a graph back. Accepts the shape 2.2.3.10.1 defines and nothing else:
 * a stream describing some other class is not a time zone, and guessing at one
 * would be worse than refusing. Returns PSRP_ERR_MALFORMED otherwise. */
psrp_result_t psrp_timezone_parse(const void *data, size_t len,
                                  psrp_timezone_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TIMEZONE_H */
