/* crypto_none.c - the crypto seam with no backend behind it.
 *
 * There is no POSIX crypto implementation yet (TODO PSRP-03); the Windows one
 * is CNG, in crypto_cng.c. This file exists so the rest of the library links
 * on a platform without one, and it is deliberately an explicit ABSENCE
 * rather than a stub that pretends:
 *
 *   - psrp_crypto_new fails with PSRP_ERR_UNSUPPORTED rather than handing
 *     back a context that would misbehave later;
 *   - every operation returns PSRP_ERR_UNSUPPORTED;
 *   - nothing returns a plausible-looking wrong answer, and in particular
 *     nothing generates a key from a weak source. A silent fallback here
 *     would be a security bug, not a portability convenience.
 *
 * The consequence is that on a platform using this file, the session key
 * exchange and SecureString protection report PSRP_ERR_UNSUPPORTED and every
 * other part of the protocol works. That is a truthful description of the
 * port's state. When an OpenSSL backend lands, this file goes away.
 *
 * The two functions that do not need a key -- building and recognising a
 * PUBLIC_KEY_REQUEST -- live in the backend file by convention, so they are
 * implemented here for real: they are pure message construction, and a
 * platform without crypto can still parse what a server sent.
 */

#include <string.h>

#include "psrp/psrp_crypto.h"
#include "psrp/psrp_buffer.h"

struct psrp_crypto { int unused; };

psrp_result_t psrp_crypto_new(psrp_crypto_t **out)
{
    if (out) *out = NULL;
    return PSRP_ERR_UNSUPPORTED;
}

void psrp_crypto_free(psrp_crypto_t *c)
{
    (void)c;
}

bool psrp_crypto_has_session_key(const psrp_crypto_t *c)
{
    (void)c;
    return false;
}

psrp_result_t psrp_crypto_set_session_key(psrp_crypto_t *c, const void *key,
                                          size_t n)
{
    (void)c; (void)key; (void)n;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_export_public_key(psrp_crypto_t *c,
                                            psrp_buffer_t *out)
{
    (void)c; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_import_session_key(psrp_crypto_t *c,
                                             const void *blob, size_t n)
{
    (void)c; (void)blob; (void)n;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_encrypt(psrp_crypto_t *c, const void *plaintext,
                                  size_t n, psrp_buffer_t *out)
{
    (void)c; (void)plaintext; (void)n; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_decrypt(psrp_crypto_t *c, const void *ciphertext,
                                  size_t n, psrp_buffer_t *out)
{
    (void)c; (void)ciphertext; (void)n; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_encrypt_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_buffer_t *out)
{
    (void)c; (void)utf8; (void)len; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_decrypt_string(psrp_crypto_t *c,
                                         const void *ciphertext, size_t len,
                                         psrp_buffer_t *out)
{
    (void)c; (void)ciphertext; (void)len; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_protect_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_value_t *out)
{
    (void)c; (void)utf8; (void)len; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_crypto_unprotect_value(psrp_crypto_t *c,
                                          const psrp_value_t *secure_string,
                                          psrp_buffer_t *out_utf8)
{
    (void)c; (void)secure_string; (void)out_utf8;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_build_public_key(psrp_crypto_t *c, psrp_buffer_t *out)
{
    (void)c; (void)out;
    return PSRP_ERR_UNSUPPORTED;
}

psrp_result_t psrp_parse_encrypted_session_key(psrp_crypto_t *c,
                                               const void *xml, size_t n)
{
    (void)c; (void)xml; (void)n;
    return PSRP_ERR_UNSUPPORTED;
}
