#include <string.h>

#include "psrp/psrp.h"
#include "psrp_test.h"

static const char *kGuidStr = "00112233-4455-6677-8899-aabbccddeeff";
static const uint8_t kGuidCanon[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};
/* .NET Guid.ToByteArray(): first three fields little-endian, last 8 as-is. */
static const uint8_t kGuidWire[16] = {
    0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

PSRP_TEST(guid_parse_canonical)
{
    psrp_guid_t g;
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    ASSERT_EQ_MEM(g.bytes, 16u, kGuidCanon, 16u);
}

PSRP_TEST(guid_parse_is_case_insensitive)
{
    psrp_guid_t lower, upper;
    ASSERT_OK(psrp_guid_parse("aabbccdd-eeff-0011-2233-445566778899", &lower));
    ASSERT_OK(psrp_guid_parse("AABBCCDD-EEFF-0011-2233-445566778899", &upper));
    ASSERT_TRUE(psrp_guid_equal(&lower, &upper));
}

PSRP_TEST(guid_format_roundtrip)
{
    psrp_guid_t g;
    char out[PSRP_GUID_BUF_SIZE];
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    ASSERT_OK(psrp_guid_format(&g, out, sizeof out));
    ASSERT_EQ_STR(out, kGuidStr);
}

PSRP_TEST(guid_format_is_lowercase)
{
    psrp_guid_t g;
    char out[PSRP_GUID_BUF_SIZE];
    ASSERT_OK(psrp_guid_parse("AABBCCDD-EEFF-0011-2233-445566778899", &g));
    ASSERT_OK(psrp_guid_format(&g, out, sizeof out));
    ASSERT_EQ_STR(out, "aabbccdd-eeff-0011-2233-445566778899");
}

PSRP_TEST(guid_format_rejects_small_buffer)
{
    psrp_guid_t g;
    char out[PSRP_GUID_BUF_SIZE];
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    ASSERT_ERR(psrp_guid_format(&g, out, PSRP_GUID_STR_LEN), PSRP_ERR_TOO_SMALL);
}

PSRP_TEST(guid_parse_rejects_malformed)
{
    psrp_guid_t g;
    /* too short */
    ASSERT_ERR(psrp_guid_parse("00112233-4455-6677-8899-aabbccddeef", &g),
               PSRP_ERR_MALFORMED);
    /* too long */
    ASSERT_ERR(psrp_guid_parse("00112233-4455-6677-8899-aabbccddeeff0", &g),
               PSRP_ERR_MALFORMED);
    /* hyphen in the wrong place */
    ASSERT_ERR(psrp_guid_parse("001122334-455-6677-8899-aabbccddeeff", &g),
               PSRP_ERR_MALFORMED);
    /* non-hex digit */
    ASSERT_ERR(psrp_guid_parse("0011223g-4455-6677-8899-aabbccddeeff", &g),
               PSRP_ERR_MALFORMED);
    /* braces are the caller's problem, not ours */
    ASSERT_ERR(psrp_guid_parse("{00112233-4455-6677-8899-aabbccddeeff}", &g),
               PSRP_ERR_MALFORMED);
    /* empty */
    ASSERT_ERR(psrp_guid_parse("", &g), PSRP_ERR_MALFORMED);
    /* NULL */
    ASSERT_ERR(psrp_guid_parse(NULL, &g), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_guid_parse(kGuidStr, NULL), PSRP_ERR_INVALID_ARG);
}

PSRP_TEST(guid_empty_helpers)
{
    psrp_guid_t z;
    psrp_guid_t g;
    memset(&z, 0, sizeof z);
    ASSERT_TRUE(psrp_guid_is_empty(&z));
    ASSERT_TRUE(psrp_guid_equal(&z, &psrp_guid_empty));
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    ASSERT_FALSE(psrp_guid_is_empty(&g));
    ASSERT_FALSE(psrp_guid_equal(&g, &psrp_guid_empty));
    ASSERT_FALSE(psrp_guid_equal(NULL, &g));
}

PSRP_TEST(guid_all_zero_string)
{
    psrp_guid_t g;
    char out[PSRP_GUID_BUF_SIZE];
    ASSERT_OK(psrp_guid_parse("00000000-0000-0000-0000-000000000000", &g));
    ASSERT_TRUE(psrp_guid_is_empty(&g));
    ASSERT_OK(psrp_guid_format(&g, out, sizeof out));
    ASSERT_EQ_STR(out, "00000000-0000-0000-0000-000000000000");
}

PSRP_TEST(guid_wire_layout_matches_dotnet)
{
    psrp_guid_t g;
    uint8_t wire[16];
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    psrp_guid_to_wire(&g, wire);
    ASSERT_EQ_MEM(wire, 16u, kGuidWire, 16u);
}

PSRP_TEST(guid_wire_roundtrip)
{
    psrp_guid_t g, back;
    uint8_t wire[16];
    ASSERT_OK(psrp_guid_parse(kGuidStr, &g));
    psrp_guid_to_wire(&g, wire);
    psrp_guid_from_wire(wire, &back);
    ASSERT_TRUE(psrp_guid_equal(&g, &back));
    ASSERT_EQ_MEM(back.bytes, 16u, kGuidCanon, 16u);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(guid_parse_canonical),
    PSRP_TEST_CASE(guid_parse_is_case_insensitive),
    PSRP_TEST_CASE(guid_format_roundtrip),
    PSRP_TEST_CASE(guid_format_is_lowercase),
    PSRP_TEST_CASE(guid_format_rejects_small_buffer),
    PSRP_TEST_CASE(guid_parse_rejects_malformed),
    PSRP_TEST_CASE(guid_empty_helpers),
    PSRP_TEST_CASE(guid_all_zero_string),
    PSRP_TEST_CASE(guid_wire_layout_matches_dotnet),
    PSRP_TEST_CASE(guid_wire_roundtrip),
};

PSRP_TEST_MAIN(cases)
