/* Session key exchange and SecureString protection on OpenSSL
 * ([MS-PSRP] 2.2.2.3-5, 2.2.5.1.24). The counterpart to crypto_cng.c.
 *
 * Only the primitives are here; everything built on them is in
 * crypto_shared.c. What matters is that these produce byte-for-byte what the
 * Windows backend produces, because either can be talking to a PowerShell
 * server that only knows one wire format:
 *
 *   - the public key goes out as a CryptoAPI PUBLICKEYBLOB, which stores its
 *     big numbers LITTLE-endian, while every crypto library works big-endian;
 *   - the session key arrives as a CryptoAPI SIMPLEBLOB, whose RSA ciphertext
 *     is likewise byte-reversed, and is PKCS#1 v1.5 padded, not OAEP;
 *   - SecureStrings are AES-256-CBC under an all-zero IV with PKCS#7 padding,
 *     over the string's UTF-16LE bytes.
 *
 * Each of those is a place where a reasonable-looking choice silently fails to
 * interoperate rather than failing loudly, so they are pinned by the same unit
 * tests on both platforms.
 */

#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "psrp/psrp_crypto.h"

struct psrp_crypto {
    EVP_PKEY *rsa;
    uint8_t session_key[PSRP_SESSION_KEY_BYTES];
    bool have_session_key;
};

/* CryptoAPI blobs store big numbers little-endian; OpenSSL is big-endian. */
static void reverse_bytes(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n / 2; i++) {
        uint8_t t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

/* OpenSSL has no SecureZeroMemory; OPENSSL_cleanse is the same guarantee that
 * the compiler will not optimise the wipe away. */
static void wipe(void *p, size_t n)
{
    OPENSSL_cleanse(p, n);
}

psrp_result_t psrp_crypto_new(psrp_crypto_t **out)
{
    psrp_crypto_t *c;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    c = (psrp_crypto_t *)calloc(1, sizeof *c);
    if (!c) return PSRP_ERR_NOMEM;

    /* 2.2.2.3 fixes the key at 2048 bits. */
    c->rsa = EVP_RSA_gen(2048);
    if (!c->rsa) { free(c); return PSRP_ERR_CRYPTO; }

    *out = c;
    return PSRP_OK;
}

void psrp_crypto_free(psrp_crypto_t *c)
{
    if (!c) return;
    if (c->rsa) EVP_PKEY_free(c->rsa);
    wipe(c->session_key, sizeof c->session_key);
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
    BIGNUM *n = NULL, *e = NULL;
    uint8_t header[16];
    uint8_t exp_le[4];
    uint8_t mod_le[PSRP_RSA_MODULUS_BYTES];
    psrp_result_t rc = PSRP_OK;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;

    if (!EVP_PKEY_get_bn_param(c->rsa, OSSL_PKEY_PARAM_RSA_N, &n) ||
        !EVP_PKEY_get_bn_param(c->rsa, OSSL_PKEY_PARAM_RSA_E, &e)) {
        BN_free(n); BN_free(e);
        return PSRP_ERR_CRYPTO;
    }
    if (BN_num_bytes(n) != PSRP_RSA_MODULUS_BYTES || BN_num_bytes(e) > 4) {
        BN_free(n); BN_free(e);
        return PSRP_ERR_CRYPTO;
    }

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

    /* bn2binpad gives big-endian, zero-padded to the width asked for, which
     * is what makes a short exponent land in the right place once reversed. */
    if (BN_bn2binpad(e, exp_le, (int)sizeof exp_le) < 0 ||
        BN_bn2binpad(n, mod_le, (int)sizeof mod_le) < 0) {
        BN_free(n); BN_free(e);
        return PSRP_ERR_CRYPTO;
    }
    reverse_bytes(exp_le, sizeof exp_le);
    reverse_bytes(mod_le, sizeof mod_le);

    rc = psrp_buffer_append(out, header, sizeof header);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, exp_le, sizeof exp_le);
    if (rc == PSRP_OK) rc = psrp_buffer_append(out, mod_le, sizeof mod_le);

    BN_free(n);
    BN_free(e);
    return rc;
}

/* ------------------------------------------------------ session key ----- */

psrp_result_t psrp_crypto_import_session_key(psrp_crypto_t *c, const void *blob,
                                             size_t len)
{
    const uint8_t *p = (const uint8_t *)blob;
    uint8_t cipher[PSRP_RSA_MODULUS_BYTES];
    uint8_t plain[PSRP_RSA_MODULUS_BYTES];
    EVP_PKEY_CTX *ctx = NULL;
    size_t produced = sizeof plain;
    psrp_result_t rc = PSRP_OK;

    if (!c || !blob) return PSRP_ERR_INVALID_ARG;
    /* SIMPLEBLOB: 12-byte header then the RSA-encrypted key. */
    if (len != 12 + PSRP_RSA_MODULUS_BYTES) return PSRP_ERR_MALFORMED;
    if (p[0] != 0x01 || p[1] != 0x02) return PSRP_ERR_MALFORMED;

    memcpy(cipher, p + 12, PSRP_RSA_MODULUS_BYTES);
    /* Stored little-endian by CryptoAPI convention. */
    reverse_bytes(cipher, PSRP_RSA_MODULUS_BYTES);

    ctx = EVP_PKEY_CTX_new(c->rsa, NULL);
    if (!ctx) return PSRP_ERR_CRYPTO;

    if (EVP_PKEY_decrypt_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0 ||
        EVP_PKEY_decrypt(ctx, plain, &produced, cipher, sizeof cipher) <= 0) {
        rc = PSRP_ERR_CRYPTO;
        goto done;
    }
    /* CRYPTO rather than MALFORMED, and this is where the two backends would
     * otherwise disagree. OpenSSL 3 applies implicit rejection to PKCS#1 v1.5:
     * rather than report a padding failure -- which is the oracle
     * Bleichenbacher and Marvin exploit -- it returns a pseudorandom plaintext
     * and reports success. So a ciphertext that is simply wrong arrives here
     * as a good decrypt of the wrong length, where CNG fails outright above.
     * Both are a cryptographic failure and both must say so. */
    if (produced != PSRP_SESSION_KEY_BYTES) {
        rc = PSRP_ERR_CRYPTO;
        goto done;
    }

    memcpy(c->session_key, plain, PSRP_SESSION_KEY_BYTES);
    c->have_session_key = true;

done:
    wipe(plain, sizeof plain);
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

/* -------------------------------------------------------------- AES ---- */

/* PowerShell encrypts SecureStrings with AES-256-CBC under the session key and
 * an all-zero IV. The spec (2.2.5.1.24) names the algorithm and mode but says
 * nothing about the IV, and there is nowhere on the wire to carry one, so a
 * fixed zero IV is the only interpretation that interoperates.
 *
 * OpenSSL pads with PKCS#7 by default, which is what BCRYPT_BLOCK_PADDING does
 * on the other side, so padding is left alone here deliberately. */
static psrp_result_t aes_run(psrp_crypto_t *c, bool encrypt, const void *in,
                             size_t in_len, psrp_buffer_t *out)
{
    EVP_CIPHER_CTX *ctx = NULL;
    uint8_t iv[PSRP_AES_BLOCK_BYTES];
    uint8_t *buf = NULL;
    size_t cap;
    int part = 0, total = 0;
    psrp_result_t rc = PSRP_OK;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;
    if (in_len && !in) return PSRP_ERR_INVALID_ARG;
    if (!c->have_session_key) return PSRP_ERR_STATE;

    /* One block of slack covers the padding EVP may add on the final call. */
    cap = in_len + PSRP_AES_BLOCK_BYTES;
    buf = (uint8_t *)malloc(cap);
    if (!buf) return PSRP_ERR_NOMEM;

    memset(iv, 0, sizeof iv);
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(buf); return PSRP_ERR_NOMEM; }

    if (EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, c->session_key, iv,
                          encrypt ? 1 : 0) != 1) {
        rc = PSRP_ERR_CRYPTO;
        goto done;
    }

    if (EVP_CipherUpdate(ctx, buf, &part, (const unsigned char *)in,
                         (int)in_len) != 1) {
        rc = PSRP_ERR_CRYPTO;
        goto done;
    }
    total = part;

    /* A wrong key shows up here, as a padding check failure on decrypt. */
    if (EVP_CipherFinal_ex(ctx, buf + total, &part) != 1) {
        rc = PSRP_ERR_CRYPTO;
        goto done;
    }
    total += part;

    if ((size_t)total > cap) { rc = PSRP_ERR_INTERNAL; goto done; }
    rc = psrp_buffer_append(out, buf, (size_t)total);

done:
    wipe(buf, cap);
    free(buf);
    EVP_CIPHER_CTX_free(ctx);
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
