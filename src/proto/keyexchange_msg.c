/* keyexchange_msg.c - the two key-exchange messages that need no key.
 *
 * PUBLIC_KEY_REQUEST carries a serialized empty string (2.2.2.5) and nothing
 * else, so building and recognising it is pure CLIXML with no cryptography in
 * sight. These lived in crypto_cng.c by association; they belong here, where
 * every platform gets them regardless of whether it has a crypto backend.
 */

#include "psrp/psrp_crypto.h"
#include "psrp/psrp_clixml.h"

psrp_result_t psrp_build_public_key_request(psrp_buffer_t *out)
{
    psrp_value_t v;
    psrp_result_t rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    psrp_value_init(&v);
    /* 2.2.2.5: a serialized empty string. */
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, "", 0);
    if (rc == PSRP_OK) rc = psrp_clixml_serialize(&v, out);
    psrp_value_free(&v);
    return rc;
}

bool psrp_is_public_key_request(const void *xml, size_t n)
{
    psrp_value_t v;
    bool ok;
    psrp_value_init(&v);
    if (psrp_clixml_deserialize(xml, n, &v) != PSRP_OK) return false;
    ok = (v.kind == PSRP_VAL_STRING && v.as.text.len == 0);
    psrp_value_free(&v);
    return ok;
}
