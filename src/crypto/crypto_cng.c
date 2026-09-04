/* Session key exchange and SecureString protection on Windows CNG
 * ([MS-PSRP] 2.2.2.3-5, 2.2.5.1.24).
 */

#include <windows.h>
#include <bcrypt.h>

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_crypto.h"
#include "psrp/psrp_clixml.h"
#include "internal/psrp_codec.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

struct psrp_crypto {
    BCRYPT_ALG_HANDLE rsa_alg;
    BCRYPT_KEY_HANDLE rsa_key;
    BCRYPT_ALG_HANDLE aes_alg;
    uint8_t session_key[PSRP_SESSION_KEY_BYTES];
    bool have_session_key;
};

/* CryptoAPI blobs store big numbers little-endian; CNG uses big-endian. */
static void reverse_bytes(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n / 2; i++) {
        uint8_t t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

psrp_result_t psrp_crypto_new(psrp_crypto_t **out)
{
    psrp_crypto_t *c;
    NTSTATUS st;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    c = (psrp_crypto_t *)calloc(1, sizeof *c);
    if (!c) return PSRP_ERR_NOMEM;

    st = BCryptOpenAlgorithmProvider(&c->rsa_alg, BCRYPT_RSA_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS) { psrp_crypto_free(c); return PSRP_ERR_CRYPTO; }

    st = BCryptOpenAlgorithmProvider(&c->aes_alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS) { psrp_crypto_free(c); return PSRP_ERR_CRYPTO; }
    st = BCryptSetProperty(c->aes_alg, BCRYPT_CHAINING_MODE,
                           (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                           sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (st != STATUS_SUCCESS) { psrp_crypto_free(c); return PSRP_ERR_CRYPTO; }

    /* 2.2.2.3 fixes the key at 2048 bits. */
    st = BCryptGenerateKeyPair(c->rsa_alg, &c->rsa_key, 2048, 0);
    if (st != STATUS_SUCCESS) { psrp_crypto_free(c); return PSRP_ERR_CRYPTO; }
    st = BCryptFinalizeKeyPair(c->rsa_key, 0);
    if (st != STATUS_SUCCESS) { psrp_crypto_free(c); return PSRP_ERR_CRYPTO; }

    *out = c;
    return PSRP_OK;
}

void psrp_crypto_free(psrp_crypto_t *c)
{
    if (!c) return;
    if (c->rsa_key) BCryptDestroyKey(c->rsa_key);
    if (c->rsa_alg) BCryptCloseAlgorithmProvider(c->rsa_alg, 0);
    if (c->aes_alg) BCryptCloseAlgorithmProvider(c->aes_alg, 0);
    SecureZeroMemory(c->session_key, sizeof c->session_key);
    free(c);
}

bool psrp_crypto_has_session_key(const psrp_crypto_t *c)
{
    return c && c->have_session_key;
}

psrp_result_t psrp_crypto_set_session_key(psrp_crypto_t *c, const void *key,
                                          size_t len)
{
    if (!c || !key) return PSRP_ERR_INVALID_ARG;
    if (len != PSRP_SESSION_KEY_BYTES) return PSRP_ERR_INVALID_ARG;
    memcpy(c->session_key, key, PSRP_SESSION_KEY_BYTES);
    c->have_session_key = true;
    return PSRP_OK;
}

/* ------------------------------------------------------- public key ----- */

psrp_result_t psrp_crypto_export_public_key(psrp_crypto_t *c, psrp_buffer_t *out)
{
    ULONG needed = 0;
    uint8_t *blob = NULL;
    BCRYPT_RSAKEY_BLOB *hdr;
    uint8_t *exp_be, *mod_be;
    uint8_t header[16];
    uint8_t exp_le[4];
    uint8_t *mod_le = NULL;
    NTSTATUS st;
    psrp_result_t rc = PSRP_OK;
    ULONG i;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;

    st = BCryptExportKey(c->rsa_key, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, 0,
                         &needed, 0);
    if (st != STATUS_SUCCESS || needed == 0) return PSRP_ERR_CRYPTO;
    blob = (uint8_t *)malloc(needed);
    if (!blob) return PSRP_ERR_NOMEM;
    st = BCryptExportKey(c->rsa_key, NULL, BCRYPT_RSAPUBLIC_BLOB, blob, needed,
                         &needed, 0);
    if (st != STATUS_SUCCESS) { free(blob); return PSRP_ERR_CRYPTO; }

    hdr = (BCRYPT_RSAKEY_BLOB *)blob;
    if (hdr->cbModulus != PSRP_RSA_MODULUS_BYTES || hdr->cbPublicExp > 4) {
        free(blob);
        return PSRP_ERR_CRYPTO;
    }
    exp_be = blob + sizeof *hdr;
    mod_be = exp_be + hdr->cbPublicExp;

    /* CryptoAPI PUBLICKEYBLOB header:
     *   06 02 00 00        PUBLICKEYBLOB, version 2, reserved
     *   00 A4 00 00        CALG_RSA_KEYX
     *   52 53 41 31        "RSA1"
     *   00 08 00 00        2048 bits, little-endian
     * then the exponent (4 bytes LE) and modulus (256 bytes LE). */
    memset(header, 0, sizeof header);
    header[0] = 0x06; header[1] = 0x02;
    header[4] = 0x00; header[5] = 0xA4;
    header[8] = 'R'; header[9] = 'S'; header[10] = 'A'; header[11] = '1';
    header[12] = 0x00; header[13] = 0x08;

    /* The exponent is big-endian and may be shorter than four bytes. */
    memset(exp_le, 0, sizeof exp_le);
    for (i = 0; i < hdr->cbPublicExp; i++)
        exp_le[i] = exp_be[hdr->cbPublicExp - 1 - i];

    mod_le = (uint8_t *)malloc(PSRP_RSA_MODULUS_BYTES);
    if (!mod_le) { free(blob); return PSRP_ERR_NOMEM; }
    memcpy(mod_le, mod_be, PSRP_RSA_MODULUS_BYTES);
    reverse_bytes(mod_le, PSRP_RSA_MODULUS_BYTES);

    rc = psrp_buffer_append(out, header, sizeof header);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, exp_le, sizeof exp_le);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, mod_le,
                                               PSRP_RSA_MODULUS_BYTES);

    free(mod_le);
    free(blob);
    return rc;
}

/* ------------------------------------------------------ session key ----- */

psrp_result_t psrp_crypto_import_session_key(psrp_crypto_t *c, const void *blob,
                                             size_t len)
{
    const uint8_t *p = (const uint8_t *)blob;
    uint8_t *cipher;
    uint8_t plain[PSRP_RSA_MODULUS_BYTES];
    ULONG produced = 0;
    NTSTATUS st;

    if (!c || !blob) return PSRP_ERR_INVALID_ARG;
    /* SIMPLEBLOB: 12-byte header then the RSA-encrypted key. */
    if (len != 12 + PSRP_RSA_MODULUS_BYTES) return PSRP_ERR_MALFORMED;
    if (p[0] != 0x01 || p[1] != 0x02) return PSRP_ERR_MALFORMED;

    cipher = (uint8_t *)malloc(PSRP_RSA_MODULUS_BYTES);
    if (!cipher) return PSRP_ERR_NOMEM;
    memcpy(cipher, p + 12, PSRP_RSA_MODULUS_BYTES);
    /* Stored little-endian by CryptoAPI convention; CNG wants big-endian. */
    reverse_bytes(cipher, PSRP_RSA_MODULUS_BYTES);

    st = BCryptDecrypt(c->rsa_key, cipher, PSRP_RSA_MODULUS_BYTES, NULL, NULL, 0,
                       plain, (ULONG)sizeof plain, &produced, BCRYPT_PAD_PKCS1);
    free(cipher);
    if (st != STATUS_SUCCESS) return PSRP_ERR_CRYPTO;
    if (produced != PSRP_SESSION_KEY_BYTES) return PSRP_ERR_MALFORMED;

    memcpy(c->session_key, plain, PSRP_SESSION_KEY_BYTES);
    c->have_session_key = true;
    SecureZeroMemory(plain, sizeof plain);
    return PSRP_OK;
}

/* -------------------------------------------------------------- AES ---- */

/* PowerShell encrypts SecureStrings with AES-256-CBC under the session key and
 * an all-zero IV. The spec (2.2.5.1.24) names the algorithm and mode but says
 * nothing about the IV, and there is nowhere on the wire to carry one, so a
 * fixed zero IV is the only interpretation that interoperates. */
static psrp_result_t aes_run(psrp_crypto_t *c, bool encrypt, const void *in,
                             size_t in_len, psrp_buffer_t *out)
{
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t iv[PSRP_AES_BLOCK_BYTES];
    uint8_t *buf = NULL;
    /* `cap` is what we allocated; `produced` is what the call reports. They
     * must stay separate: a failing BCryptDecrypt (a wrong key, say) can
     * write back a length larger than the buffer, and reusing one variable
     * for both would then overrun it while clearing. */
    ULONG cap = 0, produced = 0;
    NTSTATUS st;
    psrp_result_t rc = PSRP_OK;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;
    if (in_len && !in) return PSRP_ERR_INVALID_ARG;
    if (!c->have_session_key) return PSRP_ERR_STATE;

    st = BCryptGenerateSymmetricKey(c->aes_alg, &key, NULL, 0,
                                    c->session_key, PSRP_SESSION_KEY_BYTES, 0);
    if (st != STATUS_SUCCESS) return PSRP_ERR_CRYPTO;

    memset(iv, 0, sizeof iv);
    if (encrypt)
        st = BCryptEncrypt(key, (PUCHAR)in, (ULONG)in_len, NULL, iv, sizeof iv,
                           NULL, 0, &cap, BCRYPT_BLOCK_PADDING);
    else
        st = BCryptDecrypt(key, (PUCHAR)in, (ULONG)in_len, NULL, iv, sizeof iv,
                           NULL, 0, &cap, BCRYPT_BLOCK_PADDING);
    if (st != STATUS_SUCCESS) { BCryptDestroyKey(key); return PSRP_ERR_CRYPTO; }

    buf = (uint8_t *)malloc(cap ? cap : 1);
    if (!buf) { BCryptDestroyKey(key); return PSRP_ERR_NOMEM; }

    /* BCrypt advances the IV in place, so reset it for the real pass. */
    memset(iv, 0, sizeof iv);
    produced = 0;
    if (encrypt)
        st = BCryptEncrypt(key, (PUCHAR)in, (ULONG)in_len, NULL, iv, sizeof iv,
                           buf, cap, &produced, BCRYPT_BLOCK_PADDING);
    else
        st = BCryptDecrypt(key, (PUCHAR)in, (ULONG)in_len, NULL, iv, sizeof iv,
                           buf, cap, &produced, BCRYPT_BLOCK_PADDING);

    if (st != STATUS_SUCCESS || produced > cap) rc = PSRP_ERR_CRYPTO;
    else rc = psrp_buffer_append(out, buf, produced);

    SecureZeroMemory(buf, cap);
    free(buf);
    BCryptDestroyKey(key);
    return rc;
}

psrp_result_t psrp_crypto_encrypt(psrp_crypto_t *c, const void *plaintext,
                                  size_t len, psrp_buffer_t *out)
{
    return aes_run(c, true, plaintext, len, out);
}

psrp_result_t psrp_crypto_decrypt(psrp_crypto_t *c, const void *ciphertext,
                                  size_t len, psrp_buffer_t *out)
{
    if (len == 0 || (len % PSRP_AES_BLOCK_BYTES) != 0) return PSRP_ERR_MALFORMED;
    return aes_run(c, false, ciphertext, len, out);
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
