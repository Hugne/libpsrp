#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "internal/psrp_clixml.h"
#include "psrp_test.h"

/* Golden vectors captured from real PowerShell:
 *   [System.Management.Automation.PSSerializer]::Serialize($s)
 * These are ground truth for interop; the spec prose is looser than the
 * behaviour, especially around underscores. */
typedef struct { const char *raw; size_t raw_len; const char *enc; } vec_t;

#define V(raw_lit, enc_lit) { raw_lit, sizeof(raw_lit) - 1, enc_lit }

static const vec_t kVectors[] = {
    /* control characters -> _xHHHH_, uppercase hex */
    V("Order\nDetails",   "Order_x000A_Details"),
    V("a\tb",             "a_x0009_b"),
    V("a\rb",             "a_x000D_b"),
    V("\x7F",             "_x007F_"),
    /* C1 control (U+009F) is escaped ... */
    V("\xC2\x9F",         "_x009F_"),
    /* ... but U+00A0 is not a control and stays literal */
    V("\xC2\xA0",         "\xC2\xA0"),

    /* underscore: escaped only when followed by 'x' or 'X' */
    V("Order_Details",    "Order_Details"),
    V("a__b",             "a__b"),
    V("abc_",             "abc_"),
    V("Order_x0020_",     "Order_x005F_x0020_"),
    V("Order_X0020_",     "Order_x005F_X0020_"),
    /* PowerShell escapes even when what follows is not a valid escape */
    V("Order_x20_",       "Order_x005F_x20_"),
    V("Order_xZZZZ_",     "Order_x005F_xZZZZ_"),

    /* BMP passes through; astral becomes two surrogate escapes */
    V("\xE2\x82\xAC",     "\xE2\x82\xAC"),
    V("\xE2\x82\xAC\xF0\x9F\x92\xA9", "\xE2\x82\xAC_xD83D__xDCA9_"),
    V("\xF0\x9F\x92\xA9", "_xD83D__xDCA9_"),

    /* ordinary text and spaces untouched */
    V("a b",              "a b"),
    V("",                 ""),
    V("plain ascii",      "plain ascii"),
    /* XML metacharacters are the writer's problem, not the escaper's */
    V("a<b>&\"c",         "a<b>&\"c")
};
#define NVEC (sizeof kVectors / sizeof kVectors[0])

/* NUL is embedded, so it needs its own case (can't sit in a C literal above). */
static void check_encode(const char *raw, size_t raw_len, const char *want)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_clixml_encode_string(raw, raw_len, &out));
    ASSERT_EQ_MEM(out.data, out.len, want, strlen(want));
    psrp_buffer_free(&out);
}

static void check_decode(const char *enc, const char *want, size_t want_len)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_clixml_decode_string(enc, strlen(enc), &out));
    ASSERT_EQ_MEM(out.data, out.len, want, want_len);
    psrp_buffer_free(&out);
}

PSRP_TEST(clixml_encode_matches_powershell)
{
    size_t i;
    for (i = 0; i < NVEC; i++)
        check_encode(kVectors[i].raw, kVectors[i].raw_len, kVectors[i].enc);
}

PSRP_TEST(clixml_decode_matches_powershell)
{
    size_t i;
    for (i = 0; i < NVEC; i++)
        check_decode(kVectors[i].enc, kVectors[i].raw, kVectors[i].raw_len);
}

PSRP_TEST(clixml_roundtrip_all_vectors)
{
    size_t i;
    for (i = 0; i < NVEC; i++) {
        psrp_buffer_t enc, back;
        psrp_buffer_init(&enc);
        psrp_buffer_init(&back);
        ASSERT_OK(psrp_clixml_encode_string(kVectors[i].raw,
                                            kVectors[i].raw_len, &enc));
        ASSERT_OK(psrp_clixml_decode_string(enc.data, enc.len, &back));
        ASSERT_EQ_MEM(back.data, back.len, kVectors[i].raw, kVectors[i].raw_len);
        psrp_buffer_free(&enc);
        psrp_buffer_free(&back);
    }
}

PSRP_TEST(clixml_encodes_embedded_nul)
{
    static const char raw[] = { 'a', '\0', 'b' };
    check_encode(raw, sizeof raw, "a_x0000_b");
    check_decode("a_x0000_b", raw, sizeof raw);
}

/* PowerShell renders "\0\x01" as _x0000__x0001_. */
PSRP_TEST(clixml_encodes_consecutive_controls)
{
    static const char raw[] = { '\0', '\x01' };
    check_encode(raw, sizeof raw, "_x0000__x0001_");
    check_decode("_x0000__x0001_", raw, sizeof raw);
}

/* Every control code unit must escape, and nothing above 0x9F except
 * surrogates should. */
PSRP_TEST(clixml_escapes_exactly_the_control_range)
{
    unsigned u;
    for (u = 1; u < 0x100; u++) {      /* skip 0, covered separately */
        psrp_buffer_t src, enc;
        char utf8[3];
        size_t n;
        bool want_escaped = (u <= 0x1F) || (u >= 0x7F && u <= 0x9F);

        if (u < 0x80) { utf8[0] = (char)u; n = 1; }
        else { utf8[0] = (char)(0xC0u | (u >> 6));
               utf8[1] = (char)(0x80u | (u & 0x3Fu)); n = 2; }

        psrp_buffer_init(&src);
        psrp_buffer_init(&enc);
        ASSERT_OK(psrp_clixml_encode_string(utf8, n, &enc));
        if (want_escaped) {
            if (enc.len != 7 || enc.data[0] != '_')
                PSRP_FAIL("U+%04X should be escaped, got %zu bytes", u, enc.len);
        } else {
            if (enc.len == 7 && enc.data[0] == '_' && enc.data[1] == 'x')
                PSRP_FAIL("U+%04X should NOT be escaped", u);
        }
        psrp_buffer_free(&src);
        psrp_buffer_free(&enc);
    }
}

/* Decoding is lenient where PowerShell's encoder is not: accept uppercase
 * introducer and hex digits, since both round-trip to the same text. */
PSRP_TEST(clixml_decode_accepts_case_variants)
{
    check_decode("_x0041_", "A", 1);
    check_decode("_X0041_", "A", 1);
    check_decode("_x004a_", "J", 1);
    check_decode("_x004A_", "J", 1);
}

/* A '_' that does not begin a well-formed escape stays literal. */
PSRP_TEST(clixml_decode_leaves_non_escapes_literal)
{
    check_decode("a__b", "a__b", 4);
    check_decode("abc_", "abc_", 4);
    check_decode("_xZZZZ_", "_xZZZZ_", 7);
    check_decode("_x20_", "_x20_", 5);      /* short form is not an escape */
    check_decode("_x001_", "_x001_", 6);    /* too few digits */
    check_decode("_x0001", "_x0001", 6);    /* no trailing underscore */
    check_decode("_", "_", 1);
}

/* Two surrogate escapes must recombine into one astral character. */
PSRP_TEST(clixml_decode_recombines_surrogate_pair)
{
    check_decode("_xD83D__xDCA9_", "\xF0\x9F\x92\xA9", 4);
}

PSRP_TEST(clixml_rejects_invalid_utf8)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_clixml_encode_string("\xC3", 1, &out), PSRP_ERR_MALFORMED);
    psrp_buffer_reset(&out);
    ASSERT_ERR(psrp_clixml_encode_string("\xED\xA0\x80", 3, &out),
               PSRP_ERR_MALFORMED);
    psrp_buffer_free(&out);
}

PSRP_TEST(clixml_rejects_null_args)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_clixml_encode_string(NULL, 4, &out), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_clixml_encode_string("a", 1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_clixml_decode_string(NULL, 4, &out), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_clixml_decode_string("a", 1, NULL), PSRP_ERR_INVALID_ARG);
    /* Empty input is fine. */
    ASSERT_OK(psrp_clixml_encode_string(NULL, 0, &out));
    ASSERT_OK(psrp_clixml_decode_string(NULL, 0, &out));
    psrp_buffer_free(&out);
}

/* A long mixed string exercises buffer growth in both directions. */
PSRP_TEST(clixml_roundtrip_long_mixed_string)
{
    psrp_buffer_t src, enc, back;
    size_t i;

    psrp_buffer_init(&src);
    psrp_buffer_init(&enc);
    psrp_buffer_init(&back);
    for (i = 0; i < 500; i++) {
        ASSERT_OK(psrp_buffer_append_str(&src, "ab_"));
        ASSERT_OK(psrp_buffer_append_u8(&src, '\n'));
        ASSERT_OK(psrp_buffer_append_str(&src, "\xE2\x82\xAC"));
        ASSERT_OK(psrp_buffer_append_str(&src, "_x0020_"));
    }
    ASSERT_OK(psrp_clixml_encode_string(src.data, src.len, &enc));
    ASSERT_OK(psrp_clixml_decode_string(enc.data, enc.len, &back));
    ASSERT_EQ_MEM(back.data, back.len, src.data, src.len);

    psrp_buffer_free(&src);
    psrp_buffer_free(&enc);
    psrp_buffer_free(&back);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(clixml_encode_matches_powershell),
    PSRP_TEST_CASE(clixml_decode_matches_powershell),
    PSRP_TEST_CASE(clixml_roundtrip_all_vectors),
    PSRP_TEST_CASE(clixml_encodes_embedded_nul),
    PSRP_TEST_CASE(clixml_encodes_consecutive_controls),
    PSRP_TEST_CASE(clixml_escapes_exactly_the_control_range),
    PSRP_TEST_CASE(clixml_decode_accepts_case_variants),
    PSRP_TEST_CASE(clixml_decode_leaves_non_escapes_literal),
    PSRP_TEST_CASE(clixml_decode_recombines_surrogate_pair),
    PSRP_TEST_CASE(clixml_rejects_invalid_utf8),
    PSRP_TEST_CASE(clixml_rejects_null_args),
    PSRP_TEST_CASE(clixml_roundtrip_long_mixed_string),
};

PSRP_TEST_MAIN(cases)
