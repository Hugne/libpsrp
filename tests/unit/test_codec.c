#include <string.h>

#include "psrp/psrp.h"
#include "internal/psrp_codec.h"
#include "psrp_test.h"

/* ---------------------------------------------------------------- base64 -- */

/* RFC 4648 section 10 test vectors. */
static void check_b64(const char *raw, const char *want)
{
    char out[64];
    psrp_buffer_t back;
    size_t n = strlen(raw);

    ASSERT_OK(psrp_base64_encode(raw, n, out, sizeof out));
    ASSERT_EQ_STR(out, want);
    ASSERT_EQ_SZ(psrp_base64_encoded_len(n), strlen(want));

    psrp_buffer_init(&back);
    ASSERT_OK(psrp_base64_decode(want, strlen(want), &back));
    ASSERT_EQ_MEM(back.data, back.len, raw, n);
    psrp_buffer_free(&back);
}

PSRP_TEST(base64_rfc4648_vectors)
{
    check_b64("", "");
    check_b64("f", "Zg==");
    check_b64("fo", "Zm8=");
    check_b64("foo", "Zm9v");
    check_b64("foob", "Zm9vYg==");
    check_b64("fooba", "Zm9vYmE=");
    check_b64("foobar", "Zm9vYmFy");
}

PSRP_TEST(base64_encode_buf_matches_encode)
{
    static const uint8_t raw[] = { 0x00, 0xFF, 0x10, 0x7F, 0x80 };
    psrp_buffer_t b;
    char out[32];
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_base64_encode_buf(&b, raw, sizeof raw));
    ASSERT_OK(psrp_base64_encode(raw, sizeof raw, out, sizeof out));
    ASSERT_EQ_MEM(b.data, b.len, out, strlen(out));
    psrp_buffer_free(&b);
}

/* Every byte value must survive an encode/decode cycle. */
PSRP_TEST(base64_roundtrip_all_bytes)
{
    uint8_t raw[256];
    psrp_buffer_t enc, dec;
    int i;
    for (i = 0; i < 256; i++) raw[i] = (uint8_t)i;

    psrp_buffer_init(&enc);
    psrp_buffer_init(&dec);
    ASSERT_OK(psrp_base64_encode_buf(&enc, raw, sizeof raw));
    ASSERT_OK(psrp_base64_decode((const char *)enc.data, enc.len, &dec));
    ASSERT_EQ_MEM(dec.data, dec.len, raw, sizeof raw);
    psrp_buffer_free(&enc);
    psrp_buffer_free(&dec);
}

/* Every length 0..32 must round-trip, exercising all padding cases. */
PSRP_TEST(base64_roundtrip_all_lengths)
{
    uint8_t raw[32];
    size_t n;
    for (n = 0; n < sizeof raw; n++) raw[n] = (uint8_t)(n * 7 + 1);

    for (n = 0; n <= sizeof raw; n++) {
        psrp_buffer_t enc, dec;
        psrp_buffer_init(&enc);
        psrp_buffer_init(&dec);
        ASSERT_OK(psrp_base64_encode_buf(&enc, raw, n));
        ASSERT_EQ_SZ(enc.len, psrp_base64_encoded_len(n));
        ASSERT_OK(psrp_base64_decode((const char *)enc.data, enc.len, &dec));
        ASSERT_EQ_MEM(dec.data, dec.len, raw, n);
        psrp_buffer_free(&enc);
        psrp_buffer_free(&dec);
    }
}

/* XML often wraps base64 across lines; whitespace must be ignored. */
PSRP_TEST(base64_decode_skips_whitespace)
{
    psrp_buffer_t out;
    const char *wrapped = "Zm9v\r\n  YmFy";
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_base64_decode(wrapped, strlen(wrapped), &out));
    ASSERT_EQ_MEM(out.data, out.len, "foobar", 6u);
    psrp_buffer_free(&out);
}

PSRP_TEST(base64_decode_rejects_bad_input)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);

    /* invalid character */
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_base64_decode("Zm9!", 4, &out), PSRP_ERR_MALFORMED);
    /* length not a multiple of 4 */
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_base64_decode("Zm9", 3, &out), PSRP_ERR_MALFORMED);
    /* data after padding */
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_base64_decode("Zg==Zg==", 8, &out), PSRP_ERR_MALFORMED);
    /* too much padding */
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_base64_decode("Z===", 4, &out), PSRP_ERR_MALFORMED);

    psrp_buffer_free(&out);
}

PSRP_TEST(base64_encode_rejects_small_buffer)
{
    char out[4];
    ASSERT_ERR(psrp_base64_encode("foobar", 6, out, sizeof out), PSRP_ERR_TOO_SMALL);
}

/* ------------------------------------------------------------------- hex -- */

PSRP_TEST(hex_encode_decode_roundtrip)
{
    static const uint8_t raw[] = { 0x00, 0x0f, 0xa5, 0xff, 0x10 };
    char out[16];
    psrp_buffer_t back;

    ASSERT_OK(psrp_hex_encode(raw, sizeof raw, out, sizeof out));
    ASSERT_EQ_STR(out, "000fa5ff10");

    psrp_buffer_init(&back);
    ASSERT_OK(psrp_hex_decode(out, strlen(out), &back));
    ASSERT_EQ_MEM(back.data, back.len, raw, sizeof raw);
    psrp_buffer_free(&back);
}

PSRP_TEST(hex_decode_accepts_uppercase_rejects_junk)
{
    psrp_buffer_t out;
    static const uint8_t want[] = { 0xAB, 0xCD };
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_hex_decode("ABCD", 4, &out));
    ASSERT_EQ_MEM(out.data, out.len, want, sizeof want);

    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_hex_decode("ABC", 3, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_hex_decode("AZ", 2, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_free(&out);
}

PSRP_TEST(hex_encode_rejects_small_buffer)
{
    static const uint8_t raw[] = { 1, 2 };
    char out[4];  /* needs 5 */
    ASSERT_ERR(psrp_hex_encode(raw, sizeof raw, out, sizeof out), PSRP_ERR_TOO_SMALL);
}

/* ------------------------------------------------------------------ utf8 -- */

PSRP_TEST(utf8_accepts_valid)
{
    ASSERT_TRUE(psrp_utf8_valid("", 0));
    ASSERT_TRUE(psrp_utf8_valid("hello", 5));
    ASSERT_TRUE(psrp_utf8_valid("\xC3\xA9", 2));               /* e-acute      */
    ASSERT_TRUE(psrp_utf8_valid("\xE2\x82\xAC", 3));           /* euro sign    */
    ASSERT_TRUE(psrp_utf8_valid("\xF0\x9F\x92\xA9", 4));       /* U+1F4A9      */
    ASSERT_TRUE(psrp_utf8_valid("\xF4\x8F\xBF\xBF", 4));       /* U+10FFFF max */
}

PSRP_TEST(utf8_rejects_invalid)
{
    ASSERT_FALSE(psrp_utf8_valid("\xC0\xAF", 2));          /* overlong slash   */
    ASSERT_FALSE(psrp_utf8_valid("\xE0\x80\xAF", 3));      /* overlong 3-byte  */
    ASSERT_FALSE(psrp_utf8_valid("\xED\xA0\x80", 3));      /* surrogate D800   */
    ASSERT_FALSE(psrp_utf8_valid("\xF5\x80\x80\x80", 4));  /* > U+10FFFF       */
    ASSERT_FALSE(psrp_utf8_valid("\xC3", 1));              /* truncated        */
    ASSERT_FALSE(psrp_utf8_valid("\x80", 1));              /* stray cont. byte */
    ASSERT_FALSE(psrp_utf8_valid("\xE2\x82", 2));          /* truncated 3-byte */
}

static void check_utf_roundtrip(const char *utf8, size_t n)
{
    psrp_buffer_t u16, back;
    psrp_buffer_init(&u16);
    psrp_buffer_init(&back);
    ASSERT_OK(psrp_utf8_to_utf16(utf8, n, &u16));
    ASSERT_OK(psrp_utf16_to_utf8(u16.data, u16.len, &back));
    ASSERT_EQ_MEM(back.data, back.len, utf8, n);
    psrp_buffer_free(&u16);
    psrp_buffer_free(&back);
}

PSRP_TEST(utf_roundtrip_ascii_and_bmp_and_astral)
{
    check_utf_roundtrip("", 0);
    check_utf_roundtrip("hello world", 11);
    check_utf_roundtrip("\xC3\xA9\xC3\xA8", 4);          /* Latin-1 range   */
    check_utf_roundtrip("\xE2\x82\xAC", 3);              /* BMP             */
    check_utf_roundtrip("\xF0\x9F\x92\xA9", 4);          /* astral, surrogate pair */
    check_utf_roundtrip("a\xF0\x9F\x92\xA9""b", 6);      /* mixed           */
}

PSRP_TEST(utf8_to_utf16_encodes_surrogate_pair)
{
    psrp_buffer_t u16;
    /* U+1F4A9 -> D83D DCA9, little-endian code units. */
    static const uint8_t want[] = { 0x3D, 0xD8, 0xA9, 0xDC };
    psrp_buffer_init(&u16);
    ASSERT_OK(psrp_utf8_to_utf16("\xF0\x9F\x92\xA9", 4, &u16));
    ASSERT_EQ_MEM(u16.data, u16.len, want, sizeof want);
    psrp_buffer_free(&u16);
}

PSRP_TEST(utf8_to_utf16_rejects_bad_input)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_utf8_to_utf16("\xED\xA0\x80", 3, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_utf8_to_utf16("\xC3", 1, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_free(&out);
}

PSRP_TEST(utf16_to_utf8_rejects_bad_input)
{
    psrp_buffer_t out;
    /* odd byte count */
    static const uint8_t odd[] = { 0x41, 0x00, 0x42 };
    /* unpaired high surrogate at end */
    static const uint8_t hi[] = { 0x3D, 0xD8 };
    /* unpaired low surrogate */
    static const uint8_t lo[] = { 0xA9, 0xDC };
    /* high surrogate followed by a non-surrogate */
    static const uint8_t bad_pair[] = { 0x3D, 0xD8, 0x41, 0x00 };

    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_utf16_to_utf8(odd, sizeof odd, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_utf16_to_utf8(hi, sizeof hi, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_utf16_to_utf8(lo, sizeof lo, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_utf16_to_utf8(bad_pair, sizeof bad_pair, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_free(&out);
}

PSRP_TEST(utf8_embedded_nul_is_valid)
{
    /* PSRP strings can contain U+0000; length-driven APIs must cope. */
    static const char s[] = { 'a', '\0', 'b' };
    ASSERT_TRUE(psrp_utf8_valid(s, 3));
    check_utf_roundtrip(s, 3);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(base64_rfc4648_vectors),
    PSRP_TEST_CASE(base64_encode_buf_matches_encode),
    PSRP_TEST_CASE(base64_roundtrip_all_bytes),
    PSRP_TEST_CASE(base64_roundtrip_all_lengths),
    PSRP_TEST_CASE(base64_decode_skips_whitespace),
    PSRP_TEST_CASE(base64_decode_rejects_bad_input),
    PSRP_TEST_CASE(base64_encode_rejects_small_buffer),
    PSRP_TEST_CASE(hex_encode_decode_roundtrip),
    PSRP_TEST_CASE(hex_decode_accepts_uppercase_rejects_junk),
    PSRP_TEST_CASE(hex_encode_rejects_small_buffer),
    PSRP_TEST_CASE(utf8_accepts_valid),
    PSRP_TEST_CASE(utf8_rejects_invalid),
    PSRP_TEST_CASE(utf_roundtrip_ascii_and_bmp_and_astral),
    PSRP_TEST_CASE(utf8_to_utf16_encodes_surrogate_pair),
    PSRP_TEST_CASE(utf8_to_utf16_rejects_bad_input),
    PSRP_TEST_CASE(utf16_to_utf8_rejects_bad_input),
    PSRP_TEST_CASE(utf8_embedded_nul_is_valid),
};

PSRP_TEST_MAIN(cases)
