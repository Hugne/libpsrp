/* Platform RNG, POSIX. The counterpart to random_win.c; between them they are
 * the whole platform surface of GUID generation.
 *
 * getrandom(3) is the right interface -- no file descriptor to open, no
 * failure mode where the pool is not ready -- but it is a glibc extension, and
 * the library builds as strict C11 with extensions off, so it needs the
 * feature-test macro below. /dev/urandom stays as a genuine fallback rather
 * than dead code: getrandom can be missing on a kernel older than 3.17 and on
 * non-Linux POSIX systems.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdio.h>

#if defined(__linux__)
#  if defined(__has_include)
#    if __has_include(<sys/random.h>)
#      include <sys/random.h>
#      define PSRP_HAVE_GETRANDOM 1
#    endif
#  endif
#  include <errno.h>
#endif

#include "psrp/psrp_types.h"

static psrp_result_t fill_random(unsigned char *p, size_t n)
{
    size_t got = 0;

#if defined(PSRP_HAVE_GETRANDOM)
    while (got < n) {
        ssize_t rc = getrandom(p + got, n - got, 0);
        if (rc > 0) { got += (size_t)rc; continue; }
        if (rc < 0 && errno == EINTR) continue;
        break;      /* ENOSYS on an old kernel: fall through to urandom */
    }
    if (got == n) return PSRP_OK;
#endif

    {
        FILE *f = fopen("/dev/urandom", "rb");
        size_t rd;
        if (!f) return PSRP_ERR_INTERNAL;
        rd = fread(p + got, 1, n - got, f);
        fclose(f);
        if (rd != n - got) return PSRP_ERR_INTERNAL;
    }
    return PSRP_OK;
}

psrp_result_t psrp_guid_generate(psrp_guid_t *out)
{
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;

    rc = fill_random(out->bytes, sizeof out->bytes);
    if (rc != PSRP_OK) return rc;

    /* RFC 4122 section 4.4: version 4 in byte 6, RFC 4122 variant in byte 8.
     * Identical to the Windows path -- the randomness source may differ, the
     * layout must not. */
    out->bytes[6] = (uint8_t)((out->bytes[6] & 0x0F) | 0x40);
    out->bytes[8] = (uint8_t)((out->bytes[8] & 0x3F) | 0x80);
    return PSRP_OK;
}
