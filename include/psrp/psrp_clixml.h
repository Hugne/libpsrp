/* psrp_clixml.h - CLIXML serialization ([MS-PSRP] 2.2.5).
 *
 * Serializes the psrp_object.h value model to the XML that PSRP message Data
 * fields carry. Output is compact UTF-8 with no XML declaration and no
 * namespace declaration: PSRP message payloads in the spec's own examples look
 * like that, and 2.2.5 makes the xmlns optional.
 */
#ifndef PSRP_CLIXML_H
#define PSRP_CLIXML_H

#include "psrp/psrp_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Appends the CLIXML for `v` to `out`. */
psrp_result_t psrp_clixml_serialize(const psrp_value_t *v, psrp_buffer_t *out);

/* As above, but emits the N="name" attribute used for named members
 * (2.2.5.3.1). `name` may be NULL. */
psrp_result_t psrp_clixml_serialize_named(const psrp_value_t *v,
                                          const char *name,
                                          psrp_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_CLIXML_H */
