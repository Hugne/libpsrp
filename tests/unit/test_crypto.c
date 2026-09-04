#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_crypto.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* ------------------------------------------------------- public key ----- */

/* 2.2.2.3 pins the blob byte for byte: a 16-byte CryptoAPI PUBLICKEYBLOB
 * header, then a little-endian exponent and modulus. */
PSRP_TEST(public_key_blob_layout)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t blob;

    ASSERT_OK(psrp_crypto_new(&c));
    ASSERT_NOT_NULL(c);
    psrp_buffer_init(&blob);
    ASSERT_OK(psrp_crypto_export_public_key(c, &blob));

    ASSERT_EQ_SZ(blob.len, (size_t)PSRP_PUBLIC_KEY_BLOB_BYTES);
    ASSERT_EQ_SZ(blob.len, 276u);

    ASSERT_EQ_I(blob.data[0], 0x06);      /* PUBLICKEYBLOB */
    ASSERT_EQ_I(blob.data[1], 0x02);      /* version 2 */
    ASSERT_EQ_I(blob.data[2], 0x00);
    ASSERT_EQ_I(blob.data[3], 0x00);
    ASSERT_EQ_I(blob.data[4], 0x00);
    ASSERT_EQ_I(blob.data[5], 0xA4);      /* CALG_RSA_KEYX */
    ASSERT_EQ_I(blob.data[6], 0x00);
    ASSERT_EQ_I(blob.data[7], 0x00);
    ASSERT_EQ_MEM(blob.data + 8, 4u, "RSA1", 4u);
    ASSERT_EQ_I(blob.data[12], 0x00);     /* 2048 bits, little-endian */
    ASSERT_EQ_I(blob.data[13], 0x08);
    ASSERT_EQ_I(blob.data[14], 0x00);
    ASSERT_EQ_I(blob.data[15], 0x00);

    /* 65537 little-endian is 01 00 01 00. */
    ASSERT_EQ_I(blob.data[16], 0x01);
    ASSERT_EQ_I(blob.data[17], 0x00);
    ASSERT_EQ_I(blob.data[18], 0x01);
    ASSERT_EQ_I(blob.data[19], 0x00);

    /* A 2048-bit modulus has its top bit set, and stored little-endian that
     * lands in the final byte. This is what catches a missed byte-swap. */
    ASSERT_TRUE((blob.data[275] & 0x80) != 0);

    psrp_buffer_free(&blob);
    psrp_crypto_free(c);
    psrp_crypto_free(NULL);   /* must be safe */
}

PSRP_TEST(public_keys_differ_between_contexts)
{
    psrp_crypto_t *a = NULL, *b = NULL;
    psrp_buffer_t ka, kb;
    ASSERT_OK(psrp_crypto_new(&a));
    ASSERT_OK(psrp_crypto_new(&b));
    psrp_buffer_init(&ka);
    psrp_buffer_init(&kb);
    ASSERT_OK(psrp_crypto_export_public_key(a, &ka));
    ASSERT_OK(psrp_crypto_export_public_key(b, &kb));
    ASSERT_EQ_SZ(ka.len, kb.len);
    /* Each context must generate its own key pair. */
    ASSERT_TRUE(memcmp(ka.data, kb.data, ka.len) != 0);
    psrp_buffer_free(&ka);
    psrp_buffer_free(&kb);
    psrp_crypto_free(a);
    psrp_crypto_free(b);
}

PSRP_TEST(public_key_message_carries_base64_blob)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *pk;

    ASSERT_OK(psrp_crypto_new(&c));
    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_public_key(c, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    pk = psrp_object_find(root.as.obj, "PublicKey");
    ASSERT_NOT_NULL(pk);
    ASSERT_EQ_I(pk->kind, PSRP_VAL_STRING);
    /* base64 of 276 bytes is 368 characters. */
    ASSERT_EQ_SZ(pk->as.text.len, 368u);

    psrp_value_free(&root);
    psrp_buffer_free(&xml);
    psrp_crypto_free(c);
}

/* -------------------------------------------------- key exchange ------- */

/* 2.2.2.5: the payload is a serialized empty string. */
PSRP_TEST(public_key_request_is_an_empty_string)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_public_key_request(&xml));
    ASSERT_EQ_MEM(xml.data, xml.len, "<S></S>", 7u);
    ASSERT_TRUE(psrp_is_public_key_request(xml.data, xml.len));
    /* The spec's example form. */
    ASSERT_TRUE(psrp_is_public_key_request("<S></S>", 7));
    ASSERT_TRUE(psrp_is_public_key_request("<S />", 5));
    /* A non-empty string is not a key request. */
    ASSERT_FALSE(psrp_is_public_key_request("<S>x</S>", 8));
    ASSERT_FALSE(psrp_is_public_key_request("<Obj />", 7));
    psrp_buffer_free(&xml);
}

PSRP_TEST(session_key_starts_absent)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t out;
    ASSERT_OK(psrp_crypto_new(&c));
    ASSERT_FALSE(psrp_crypto_has_session_key(c));
    ASSERT_FALSE(psrp_crypto_has_session_key(NULL));
    /* Encrypting before the exchange is a state error, not a crypto one. */
    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_crypto_encrypt(c, "x", 1, &out), PSRP_ERR_STATE);
    psrp_buffer_free(&out);
    psrp_crypto_free(c);
}

PSRP_TEST(session_key_import_validates_the_blob)
{
    psrp_crypto_t *c = NULL;
    uint8_t blob[12 + PSRP_RSA_MODULUS_BYTES];
    ASSERT_OK(psrp_crypto_new(&c));
    memset(blob, 0, sizeof blob);

    /* Wrong length. */
    ASSERT_ERR(psrp_crypto_import_session_key(c, blob, 10), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_crypto_import_session_key(c, blob, sizeof blob - 1),
               PSRP_ERR_MALFORMED);
    /* Right length, wrong SIMPLEBLOB header. */
    blob[0] = 0x99;
    ASSERT_ERR(psrp_crypto_import_session_key(c, blob, sizeof blob),
               PSRP_ERR_MALFORMED);
    /* Correct header but the body will not decrypt. */
    blob[0] = 0x01; blob[1] = 0x02;
    ASSERT_ERR(psrp_crypto_import_session_key(c, blob, sizeof blob),
               PSRP_ERR_CRYPTO);

    psrp_crypto_free(c);
}

PSRP_TEST(encrypted_session_key_message_requires_the_property)
{
    psrp_crypto_t *c = NULL;
    ASSERT_OK(psrp_crypto_new(&c));
    ASSERT_ERR(psrp_parse_encrypted_session_key(c, "<Obj />", 7),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_encrypted_session_key(c, "<S>x</S>", 8),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_encrypted_session_key(c, "junk", 4), PSRP_ERR_XML);
    psrp_crypto_free(c);
}

/* ------------------------------------------------------------- AES ----- */

static void with_key(psrp_crypto_t **out)
{
    static const uint8_t key[PSRP_SESSION_KEY_BYTES] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    ASSERT_OK(psrp_crypto_new(out));
    ASSERT_OK(psrp_crypto_set_session_key(*out, key, sizeof key));
    ASSERT_TRUE(psrp_crypto_has_session_key(*out));
}

PSRP_TEST(aes_round_trip)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t enc, dec;
    static const char plain[] = "correct horse battery staple";

    with_key(&c);
    psrp_buffer_init(&enc);
    psrp_buffer_init(&dec);
    ASSERT_OK(psrp_crypto_encrypt(c, plain, sizeof plain - 1, &enc));
    /* CBC with padding always produces whole blocks, never the plaintext. */
    ASSERT_TRUE(enc.len > 0);
    ASSERT_EQ_SZ(enc.len % PSRP_AES_BLOCK_BYTES, 0u);
    ASSERT_TRUE(enc.len != sizeof plain - 1 ||
                memcmp(enc.data, plain, enc.len) != 0);

    ASSERT_OK(psrp_crypto_decrypt(c, enc.data, enc.len, &dec));
    ASSERT_EQ_MEM(dec.data, dec.len, plain, sizeof plain - 1);

    psrp_buffer_free(&enc);
    psrp_buffer_free(&dec);
    psrp_crypto_free(c);
}

/* Padding must handle an exact block multiple and the empty string. */
PSRP_TEST(aes_round_trip_edge_lengths)
{
    psrp_crypto_t *c = NULL;
    size_t n;
    uint8_t plain[64];

    with_key(&c);
    for (n = 0; n < sizeof plain; n++) plain[n] = (uint8_t)(n * 3 + 1);

    for (n = 0; n <= sizeof plain; n++) {
        psrp_buffer_t enc, dec;
        psrp_buffer_init(&enc);
        psrp_buffer_init(&dec);
        ASSERT_OK(psrp_crypto_encrypt(c, plain, n, &enc));
        ASSERT_EQ_SZ(enc.len % PSRP_AES_BLOCK_BYTES, 0u);
        /* Block padding always adds at least one byte. */
        ASSERT_TRUE(enc.len > n);
        ASSERT_OK(psrp_crypto_decrypt(c, enc.data, enc.len, &dec));
        ASSERT_EQ_MEM(dec.data, dec.len, plain, n);
        psrp_buffer_free(&enc);
        psrp_buffer_free(&dec);
    }
    psrp_crypto_free(c);
}

/* Encryption is deterministic here because the IV is fixed at zero, which is
 * what PowerShell does and what makes interop possible. */
PSRP_TEST(aes_is_deterministic_with_the_zero_iv)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t a, b;
    with_key(&c);
    psrp_buffer_init(&a);
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_crypto_encrypt(c, "same input", 10, &a));
    ASSERT_OK(psrp_crypto_encrypt(c, "same input", 10, &b));
    ASSERT_EQ_MEM(a.data, a.len, b.data, b.len);
    psrp_buffer_free(&a);
    psrp_buffer_free(&b);
    psrp_crypto_free(c);
}

PSRP_TEST(aes_decrypt_rejects_bad_lengths)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t out;
    uint8_t junk[17];
    with_key(&c);
    memset(junk, 0, sizeof junk);
    psrp_buffer_init(&out);
    /* Not a whole number of blocks. */
    ASSERT_ERR(psrp_crypto_decrypt(c, junk, 17, &out), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_crypto_decrypt(c, junk, 0, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_free(&out);
    psrp_crypto_free(c);
}

PSRP_TEST(different_keys_do_not_interoperate)
{
    psrp_crypto_t *a = NULL, *b = NULL;
    psrp_buffer_t enc, dec;
    uint8_t key_b[PSRP_SESSION_KEY_BYTES];

    with_key(&a);
    ASSERT_OK(psrp_crypto_new(&b));
    memset(key_b, 0xAB, sizeof key_b);
    ASSERT_OK(psrp_crypto_set_session_key(b, key_b, sizeof key_b));

    psrp_buffer_init(&enc);
    psrp_buffer_init(&dec);
    ASSERT_OK(psrp_crypto_encrypt(a, "secret", 6, &enc));
    /* Wrong key: either the padding check fails or the plaintext differs. */
    if (psrp_crypto_decrypt(b, enc.data, enc.len, &dec) == PSRP_OK)
        ASSERT_TRUE(dec.len != 6 || memcmp(dec.data, "secret", 6) != 0);

    psrp_buffer_free(&enc);
    psrp_buffer_free(&dec);
    psrp_crypto_free(a);
    psrp_crypto_free(b);
}

/* SecureString plaintext is the UTF-16LE form of the string. */
PSRP_TEST(string_round_trip_uses_utf16)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t enc, dec, raw;
    static const char text[] = "p\xC3\xA4ssw0rd";   /* has a non-ASCII char */

    with_key(&c);
    psrp_buffer_init(&enc);
    psrp_buffer_init(&dec);
    psrp_buffer_init(&raw);

    ASSERT_OK(psrp_crypto_encrypt_string(c, text, sizeof text - 1, &enc));
    ASSERT_OK(psrp_crypto_decrypt_string(c, enc.data, enc.len, &dec));
    ASSERT_EQ_MEM(dec.data, dec.len, text, sizeof text - 1);

    /* Decrypting without the UTF-8 conversion yields the UTF-16 bytes, two
     * per BMP character. */
    ASSERT_OK(psrp_crypto_decrypt(c, enc.data, enc.len, &raw));
    ASSERT_EQ_SZ(raw.len, 16u);      /* 8 characters */

    psrp_buffer_free(&enc);
    psrp_buffer_free(&dec);
    psrp_buffer_free(&raw);
    psrp_crypto_free(c);
}

PSRP_TEST(crypto_rejects_null_args)
{
    psrp_crypto_t *c = NULL;
    psrp_buffer_t out;
    uint8_t key[PSRP_SESSION_KEY_BYTES];
    ASSERT_ERR(psrp_crypto_new(NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_OK(psrp_crypto_new(&c));
    psrp_buffer_init(&out);
    memset(key, 0, sizeof key);
    ASSERT_ERR(psrp_crypto_export_public_key(NULL, &out), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_crypto_export_public_key(c, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_crypto_set_session_key(c, key, 31), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_crypto_set_session_key(c, NULL, 32), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_public_key(c, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_public_key_request(NULL), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&out);
    psrp_crypto_free(c);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(public_key_blob_layout),
    PSRP_TEST_CASE(public_keys_differ_between_contexts),
    PSRP_TEST_CASE(public_key_message_carries_base64_blob),
    PSRP_TEST_CASE(public_key_request_is_an_empty_string),
    PSRP_TEST_CASE(session_key_starts_absent),
    PSRP_TEST_CASE(session_key_import_validates_the_blob),
    PSRP_TEST_CASE(encrypted_session_key_message_requires_the_property),
    PSRP_TEST_CASE(aes_round_trip),
    PSRP_TEST_CASE(aes_round_trip_edge_lengths),
    PSRP_TEST_CASE(aes_is_deterministic_with_the_zero_iv),
    PSRP_TEST_CASE(aes_decrypt_rejects_bad_lengths),
    PSRP_TEST_CASE(different_keys_do_not_interoperate),
    PSRP_TEST_CASE(string_round_trip_uses_utf16),
    PSRP_TEST_CASE(crypto_rejects_null_args),
};

PSRP_TEST_MAIN(cases)
