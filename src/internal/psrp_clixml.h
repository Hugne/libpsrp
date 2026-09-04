/* psrp_clixml.h - internal CLIXML helpers ([MS-PSRP] 2.2.5).
 *
 * Not part of the public API.
 */
#ifndef PSRP_CLIXML_INTERNAL_H
#define PSRP_CLIXML_INTERNAL_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------- 2.2.5.3.2 Encoding Strings -- */
/*
 * Control and surrogate characters are escaped as _xHHHH_ using the UTF-16
 * code unit, most significant digit first, with UPPERCASE hex digits.
 *
 * The spec says an underscore "only requires escaping when it is followed by a
 * character sequence that, together with the underscore, can be
 * misinterpreted as an escape sequence". Real PowerShell is blunter and more
 * conservative: it escapes '_' whenever the next code unit is 'x' or 'X',
 * without checking that a valid _xHHHH_ actually follows. Verified against
 * [System.Management.Automation.PSSerializer]:
 *
 *   "Order_Details"  -> Order_Details        (next char is 'D')
 *   "a__b"           -> a__b                 (next char is '_')
 *   "abc_"           -> abc_                 (nothing follows)
 *   "Order_x0020_"   -> Order_x005F_x0020_
 *   "Order_X0020_"   -> Order_x005F_X0020_   (uppercase X too)
 *   "Order_x20_"     -> Order_x005F_x20_     (short form still escaped)
 *   "Order_xZZZZ_"   -> Order_x005F_xZZZZ_   (invalid hex still escaped)
 *
 * We match PowerShell, because interoperating with it matters more than
 * matching a loose prose description.
 *
 * Escaped set: code units 0x00-0x1F, 0x7F-0x9F (.NET char.IsControl) and
 * surrogates 0xD800-0xDFFF. Note U+00A0 is NOT escaped. Because escaping is
 * defined over UTF-16, a non-BMP character always becomes two escapes, e.g.
 * U+1F4A9 -> _xD83D__xDCA9_.
 *
 * These functions deal in UTF-8 on both sides. XML metacharacters (& < >) are
 * NOT handled here; that is the XML writer's job.
 */

psrp_result_t psrp_clixml_encode_string(const void *utf8, size_t n,
                                        psrp_buffer_t *out);

/* Reverses the above. Accepts lowercase or uppercase hex digits and both
 * '_x' and '_X' introducers. A '_' not starting a well-formed escape is
 * literal, which is what makes "a__b" and "abc_" round-trip. */
psrp_result_t psrp_clixml_decode_string(const void *text, size_t n,
                                        psrp_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_CLIXML_INTERNAL_H */
