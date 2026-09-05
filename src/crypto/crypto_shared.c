/* crypto_shared.c - the parts of the crypto seam that are not platform code.
 *
 * Everything here is built on the primitives a backend provides -- key
 * generation, RSA, and raw AES -- and contains no platform calls of its own:
 * UTF-16 conversion, base64, the SecureString wire form, and the two key
 * exchange messages. It used to live in crypto_cng.c, where it was invisible
 * that it was portable; splitting it out means a second backend implements
 * six functions rather than reimplementing twelve, and the CLIXML shape of a
 * PUBLIC_KEY message cannot drift between platforms because there is only one
 * copy of it.
 *
 * See psrp_crypto.h for the contract and crypto_cng.c / crypto_openssl.c for
 * the primitives.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_crypto.h"
#include "psrp/psrp_clixml.h"
#include "internal/psrp_codec.h"

psrp_result_t psrp_crypto_protect_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_value_t *out)
{
    psrp_buffer_t cipher, b64;
    psrp_result_t rc;

    if (!c || !utf8 || !out) return PSRP_ERR_INVALID_ARG;
    if (!psrp_crypto_has_session_key(c)) return PSRP_ERR_STATE;

    psrp_buffer_init(&cipher);
    psrp_buffer_init(&b64);
    rc = psrp_crypto_encrypt_string(c, utf8, len, &cipher);
    if (rc == PSRP_OK) rc = psrp_base64_encode_buf(&b64, cipher.data, cipher.len);
    if (rc == PSRP_OK)
        rc = psrp_value_set_text(out, PSRP_VAL_SECURESTRING,
                                 (const char *)b64.data, b64.len);
    /* The ciphertext is not secret, but the plaintext this was made from is
     * the caller's business; nothing of it lingers here. */
    psrp_buffer_free(&cipher);
    psrp_buffer_free(&b64);
    return rc;
}

psrp_result_t psrp_crypto_unprotect_value(psrp_crypto_t *c,
                                          const psrp_value_t *secure_string,
                                          psrp_buffer_t *out_utf8)
{
    psrp_buffer_t cipher;
    psrp_result_t rc;

    if (!c || !secure_string || !out_utf8) return PSRP_ERR_INVALID_ARG;
    if (secure_string->kind != PSRP_VAL_SECURESTRING) return PSRP_ERR_MALFORMED;
    if (!psrp_crypto_has_session_key(c)) return PSRP_ERR_STATE;

    psrp_buffer_init(&cipher);
    rc = psrp_base64_decode(secure_string->as.text.ptr,
                            secure_string->as.text.len, &cipher);
    if (rc == PSRP_OK)
        rc = psrp_crypto_decrypt_string(c, cipher.data, cipher.len, out_utf8);
    psrp_buffer_free(&cipher);
    return rc;
}

psrp_result_t psrp_crypto_encrypt_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_buffer_t *out)
{
    psrp_buffer_t u16;
    psrp_result_t rc;
    psrp_buffer_init(&u16);
    /* PowerShell encrypts the string's UTF-16LE representation. */
    rc = psrp_utf8_to_utf16(utf8, len, &u16);
    if (rc == PSRP_OK) rc = psrp_crypto_encrypt(c, u16.data, u16.len, out);
    psrp_buffer_free(&u16);
    return rc;
}

psrp_result_t psrp_crypto_decrypt_string(psrp_crypto_t *c,
                                         const void *ciphertext, size_t len,
                                         psrp_buffer_t *out)
{
    psrp_buffer_t u16;
    psrp_result_t rc;
    psrp_buffer_init(&u16);
    rc = psrp_crypto_decrypt(c, ciphertext, len, &u16);
    if (rc == PSRP_OK) rc = psrp_utf16_to_utf8(u16.data, u16.len, out);
    psrp_buffer_free(&u16);
    return rc;
}

/* ------------------------------------------ key exchange messages ------ */

psrp_result_t psrp_build_public_key(psrp_crypto_t *c, psrp_buffer_t *out)
{
    psrp_buffer_t blob, b64;
    psrp_object_t *o;
    psrp_value_t v, wrapper;
    psrp_result_t rc;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;

    psrp_buffer_init(&blob);
    psrp_buffer_init(&b64);
    rc = psrp_crypto_export_public_key(c, &blob);
    if (rc == PSRP_OK) rc = psrp_base64_encode_buf(&b64, blob.data, blob.len);
    if (rc != PSRP_OK) {
        psrp_buffer_free(&blob);
        psrp_buffer_free(&b64);
        return rc;
    }

    o = psrp_object_new();
    if (!o) {
        psrp_buffer_free(&blob);
        psrp_buffer_free(&b64);
        return PSRP_ERR_NOMEM;
    }
    psrp_object_set_ref_id(o, 0);

    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, (const char *)b64.data,
                             b64.len);
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, "PublicKey", &v);
    psrp_value_free(&v);
    psrp_buffer_free(&blob);
    psrp_buffer_free(&b64);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, o);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

psrp_result_t psrp_parse_encrypted_session_key(psrp_crypto_t *c,
                                               const void *xml, size_t n)
{
    psrp_value_t root;
    const psrp_value_t *v;
    psrp_buffer_t blob;
    psrp_result_t rc;

    if (!c) return PSRP_ERR_INVALID_ARG;

    psrp_value_init(&root);
    rc = psrp_clixml_deserialize(xml, n, &root);
    if (rc != PSRP_OK) return rc;
    if (root.kind != PSRP_VAL_OBJECT) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    v = psrp_object_find(root.as.obj, "EncryptedSessionKey");
    if (!v || v->kind != PSRP_VAL_STRING) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    psrp_buffer_init(&blob);
    rc = psrp_base64_decode(v->as.text.ptr, v->as.text.len, &blob);
    if (rc == PSRP_OK)
        rc = psrp_crypto_import_session_key(c, blob.data, blob.len);
    psrp_buffer_free(&blob);
    psrp_value_free(&root);
    return rc;
}
