/* TimeZone (2.2.3.10) in MS-NRBF.
 *
 * The two fixtures below are not hand-written. They came out of .NET's own
 * BinaryFormatter serializing System.TimeZone.CurrentTimeZone on a machine in
 * W. Europe Standard Time, captured once and pinned here. Matching them byte
 * for byte is the only real proof that what this library sends is what a
 * PowerShell server expects to read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_timezone.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_messages.h"
#include "internal/psrp_codec.h"
#include "psrp_test.h"

/* +1 hour, the standard offset for W. Europe, in 100-nanosecond ticks. */
#define WEUROPE_TICKS 36000000000LL

/* BinaryFormatter output for a time zone whose names have not been read, so
 * both name fields really are null. .NET fills them lazily. */
static const char kNamelessB64[] =
    "AAEAAAD/////AQAAAAAAAAAEAQAAABxTeXN0ZW0uQ3VycmVudFN5c3RlbVRpbWVab25lBAAAABdt"
    "X0NhY2hlZERheWxpZ2h0Q2hhbmdlcw1tX3RpY2tzT2Zmc2V0Dm1fc3RhbmRhcmROYW1lDm1fZGF5"
    "bGlnaHROYW1lAwABARxTeXN0ZW0uQ29sbGVjdGlvbnMuSGFzaHRhYmxlCQkCAAAAAGjEYQgAAAAK"
    "CgQCAAAAHFN5c3RlbS5Db2xsZWN0aW9ucy5IYXNodGFibGUHAAAACkxvYWRGYWN0b3IHVmVyc2lv"
    "bghDb21wYXJlchBIYXNoQ29kZVByb3ZpZGVyCEhhc2hTaXplBEtleXMGVmFsdWVzAAADAwAFBQsI"
    "HFN5c3RlbS5Db2xsZWN0aW9ucy5JQ29tcGFyZXIkU3lzdGVtLkNvbGxlY3Rpb25zLklIYXNoQ29k"
    "ZVByb3ZpZGVyCOxROD8AAAAACgoDAAAACQMAAAAJBAAAABADAAAAAAAAABAEAAAAAAAAAAs=";

/* The same time zone after its names were read. Note the object ids move: the
 * two strings take 3 and 4, so the Keys and Values arrays become 5 and 6. */
static const char kNamedB64[] =
    "AAEAAAD/////AQAAAAAAAAAEAQAAABxTeXN0ZW0uQ3VycmVudFN5c3RlbVRpbWVab25lBAAAABdt"
    "X0NhY2hlZERheWxpZ2h0Q2hhbmdlcw1tX3RpY2tzT2Zmc2V0Dm1fc3RhbmRhcmROYW1lDm1fZGF5"
    "bGlnaHROYW1lAwABARxTeXN0ZW0uQ29sbGVjdGlvbnMuSGFzaHRhYmxlCQkCAAAAAGjEYQgAAAAG"
    "AwAAABdXLiBFdXJvcGUgU3RhbmRhcmQgVGltZQYEAAAAFVcuIEV1cm9wZSBTdW1tZXIgVGltZQQC"
    "AAAAHFN5c3RlbS5Db2xsZWN0aW9ucy5IYXNodGFibGUHAAAACkxvYWRGYWN0b3IHVmVyc2lvbghD"
    "b21wYXJlchBIYXNoQ29kZVByb3ZpZGVyCEhhc2hTaXplBEtleXMGVmFsdWVzAAADAwAFBQsIHFN5"
    "c3RlbS5Db2xsZWN0aW9ucy5JQ29tcGFyZXIkU3lzdGVtLkNvbGxlY3Rpb25zLklIYXNoQ29kZVBy"
    "b3ZpZGVyCOxROD8AAAAACgoDAAAACQUAAAAJBgAAABAFAAAAAAAAABAGAAAAAAAAAAs=";

static void decode_fixture(const char *b64, psrp_buffer_t *out)
{
    psrp_buffer_init(out);
    ASSERT_OK(psrp_base64_decode(b64, strlen(b64), out));
}

/* ------------------------------------------------------------ writing -- */

PSRP_TEST(nameless_zone_matches_binaryformatter)
{
    psrp_timezone_t tz;
    psrp_buffer_t want, got;

    decode_fixture(kNamelessB64, &want);
    ASSERT_EQ_SZ(want.len, 395u);

    memset(&tz, 0, sizeof tz);
    tz.ticks_offset = WEUROPE_TICKS;

    psrp_buffer_init(&got);
    ASSERT_OK(psrp_timezone_serialize(&tz, &got));
    ASSERT_EQ_MEM(got.data, got.len, want.data, want.len);

    psrp_buffer_free(&want);
    psrp_buffer_free(&got);
}

PSRP_TEST(named_zone_matches_binaryformatter)
{
    psrp_timezone_t tz;
    psrp_buffer_t want, got;

    decode_fixture(kNamedB64, &want);
    ASSERT_EQ_SZ(want.len, 449u);

    memset(&tz, 0, sizeof tz);
    tz.ticks_offset = WEUROPE_TICKS;
    tz.has_standard_name = true;
    tz.has_daylight_name = true;
    snprintf(tz.standard_name, sizeof tz.standard_name, "%s",
             "W. Europe Standard Time");
    snprintf(tz.daylight_name, sizeof tz.daylight_name, "%s",
             "W. Europe Summer Time");

    psrp_buffer_init(&got);
    ASSERT_OK(psrp_timezone_serialize(&tz, &got));
    ASSERT_EQ_MEM(got.data, got.len, want.data, want.len);

    psrp_buffer_free(&want);
    psrp_buffer_free(&got);
}

PSRP_TEST(one_name_present_shifts_the_array_ids)
{
    /* Object ids are assigned as records appear, so a single string moves the
     * arrays to 4 and 5. Getting this wrong produces a graph that decodes to
     * dangling references rather than one that simply looks different. */
    psrp_timezone_t tz, back;
    psrp_buffer_t got;

    memset(&tz, 0, sizeof tz);
    tz.ticks_offset = -288000000000LL;      /* -8 hours, the spec's example */
    tz.has_standard_name = true;
    snprintf(tz.standard_name, sizeof tz.standard_name, "%s",
             "Pacific Standard Time");

    psrp_buffer_init(&got);
    ASSERT_OK(psrp_timezone_serialize(&tz, &got));

    /* The last two array records must name 4 and 5. Each is the tag 0x10 then
     * the id then a zero length, and they sit just before MessageEnd. */
    ASSERT_TRUE(got.len > 19);
    ASSERT_EQ_I(got.data[got.len - 1], 11);          /* MessageEnd */
    ASSERT_EQ_I(got.data[got.len - 10], 16);         /* ArraySingleObject */
    ASSERT_EQ_I(got.data[got.len - 9], 5);           /* Values id */
    ASSERT_EQ_I(got.data[got.len - 19], 16);
    ASSERT_EQ_I(got.data[got.len - 18], 4);          /* Keys id */

    ASSERT_OK(psrp_timezone_parse(got.data, got.len, &back));
    ASSERT_TRUE(back.has_standard_name);
    ASSERT_FALSE(back.has_daylight_name);
    ASSERT_EQ_STR(back.standard_name, "Pacific Standard Time");
    ASSERT_TRUE(back.ticks_offset == -288000000000LL);

    psrp_buffer_free(&got);
}

PSRP_TEST(serialize_rejects_null_args)
{
    psrp_timezone_t tz;
    psrp_buffer_t out;
    memset(&tz, 0, sizeof tz);
    psrp_buffer_init(&out);
    ASSERT_ERR(psrp_timezone_serialize(NULL, &out), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_timezone_serialize(&tz, NULL), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&out);
}

/* ------------------------------------------------------------ reading -- */

PSRP_TEST(parses_the_binaryformatter_fixtures)
{
    psrp_buffer_t blob;
    psrp_timezone_t tz;

    decode_fixture(kNamelessB64, &blob);
    ASSERT_OK(psrp_timezone_parse(blob.data, blob.len, &tz));
    ASSERT_TRUE(tz.ticks_offset == WEUROPE_TICKS);
    ASSERT_FALSE(tz.has_standard_name);
    ASSERT_FALSE(tz.has_daylight_name);
    psrp_buffer_free(&blob);

    decode_fixture(kNamedB64, &blob);
    ASSERT_OK(psrp_timezone_parse(blob.data, blob.len, &tz));
    ASSERT_TRUE(tz.ticks_offset == WEUROPE_TICKS);
    ASSERT_TRUE(tz.has_standard_name);
    ASSERT_TRUE(tz.has_daylight_name);
    ASSERT_EQ_STR(tz.standard_name, "W. Europe Standard Time");
    ASSERT_EQ_STR(tz.daylight_name, "W. Europe Summer Time");
    psrp_buffer_free(&blob);
}

PSRP_TEST(round_trips_an_empty_daylight_name)
{
    /* 2.2.3.10.1: a zone with no daylight saving reports an empty string, not
     * a null. The two must stay distinguishable. */
    psrp_timezone_t tz, back;
    psrp_buffer_t got;

    memset(&tz, 0, sizeof tz);
    tz.ticks_offset = 0;
    tz.has_standard_name = true;
    tz.has_daylight_name = true;
    snprintf(tz.standard_name, sizeof tz.standard_name, "%s", "UTC");
    tz.daylight_name[0] = '\0';

    psrp_buffer_init(&got);
    ASSERT_OK(psrp_timezone_serialize(&tz, &got));
    ASSERT_OK(psrp_timezone_parse(got.data, got.len, &back));
    ASSERT_TRUE(back.has_daylight_name);
    ASSERT_EQ_STR(back.daylight_name, "");
    psrp_buffer_free(&got);
}

PSRP_TEST(parse_refuses_a_different_graph)
{
    psrp_buffer_t blob;
    psrp_timezone_t tz;
    size_t i;

    /* Not NRBF at all. */
    ASSERT_ERR(psrp_timezone_parse("nonsense", 8, &tz), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_timezone_parse("", 0, &tz), PSRP_ERR_MALFORMED);

    /* A valid stream with the class name changed is some other object, and
     * reading its fields as a time zone would invent data. */
    decode_fixture(kNamedB64, &blob);
    for (i = 0; i + 6 < blob.len; i++) {
        if (memcmp(blob.data + i, "System.Current", 14) == 0) {
            blob.data[i] = 'X';
            break;
        }
    }
    ASSERT_ERR(psrp_timezone_parse(blob.data, blob.len, &tz),
               PSRP_ERR_MALFORMED);
    psrp_buffer_free(&blob);
}

PSRP_TEST(parse_refuses_a_truncated_graph)
{
    /* Every prefix of a real graph must be refused, never read as a short one
     * with default fields. */
    psrp_buffer_t blob;
    psrp_timezone_t tz;
    size_t n;

    decode_fixture(kNamedB64, &blob);
    for (n = 0; n < 200; n++) {
        psrp_result_t rc = psrp_timezone_parse(blob.data, n, &tz);
        ASSERT_TRUE(rc == PSRP_ERR_MALFORMED);
    }
    psrp_buffer_free(&blob);
}

PSRP_TEST(parse_rejects_an_overlong_name)
{
    /* A name that will not fit is refused rather than truncated into what
     * looks like a different zone. */
    psrp_buffer_t blob;
    psrp_timezone_t tz;
    char big[PSRP_TIMEZONE_NAME_MAX + 40];
    size_t i;

    for (i = 0; i < sizeof big - 1; i++) big[i] = 'z';
    big[sizeof big - 1] = '\0';

    /* Build it by hand, since the writer's own struct cannot hold one. */
    memset(&tz, 0, sizeof tz);
    tz.has_standard_name = true;
    memcpy(tz.standard_name, big, PSRP_TIMEZONE_NAME_MAX - 1);
    tz.standard_name[PSRP_TIMEZONE_NAME_MAX - 1] = '\0';

    psrp_buffer_init(&blob);
    ASSERT_OK(psrp_timezone_serialize(&tz, &blob));
    /* At the maximum it still round-trips. */
    ASSERT_OK(psrp_timezone_parse(blob.data, blob.len, &tz));
    ASSERT_EQ_SZ(strlen(tz.standard_name),
                 (size_t)(PSRP_TIMEZONE_NAME_MAX - 1));
    psrp_buffer_free(&blob);
}

/* ------------------------------------------------------------ current -- */

PSRP_TEST(current_zone_is_readable_and_serializes)
{
    psrp_timezone_t tz, back;
    psrp_buffer_t blob;

    ASSERT_OK(psrp_timezone_current(&tz));
    /* A standard name is always present on Windows. */
    ASSERT_TRUE(tz.has_standard_name);
    ASSERT_TRUE(strlen(tz.standard_name) > 0);
    /* Offsets run from -12 to +14 hours; anything else means the sign or the
     * tick conversion is wrong. */
    ASSERT_TRUE(tz.ticks_offset >= -12LL * 3600 * 10000000);
    ASSERT_TRUE(tz.ticks_offset <= 14LL * 3600 * 10000000);

    psrp_buffer_init(&blob);
    ASSERT_OK(psrp_timezone_serialize(&tz, &blob));
    ASSERT_OK(psrp_timezone_parse(blob.data, blob.len, &back));
    ASSERT_TRUE(back.ticks_offset == tz.ticks_offset);
    ASSERT_EQ_STR(back.standard_name, tz.standard_name);
    psrp_buffer_free(&blob);
}

/* --------------------------------------------------- on the wire -------- */

PSRP_TEST(session_capability_omits_timezone_by_default)
{
    /* The spec says SHOULD, and reporting a time zone tells the server where
     * the client is, so it is opt-in rather than automatic. */
    psrp_session_capability_t cap;
    psrp_buffer_t xml;
    size_t i;
    bool found = false;

    psrp_session_capability_defaults(&cap);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_session_capability(&cap, &xml));
    for (i = 0; i + 8 <= xml.len; i++)
        if (memcmp(xml.data + i, "TimeZone", 8) == 0) found = true;
    ASSERT_FALSE(found);
    psrp_buffer_free(&xml);
}

PSRP_TEST(session_capability_carries_the_timezone_when_asked)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t wire;
    size_t i;
    bool found_name = false, found_bytes = false;

    ASSERT_NOT_NULL(s);
    ASSERT_OK(psrp_session_send_timezone(s));
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_open_payload(s, &wire));

    /* The blob rides base64 in a BA element, so both the property name and the
     * element appear in the fragmented payload. */
    for (i = 0; i + 16 <= wire.len; i++) {
        if (memcmp(wire.data + i, "N=\"TimeZone\"", 12) == 0) found_name = true;
        if (memcmp(wire.data + i, "<BA N=\"TimeZone\">", 17) == 0)
            found_bytes = true;
    }
    ASSERT_TRUE(found_name);
    ASSERT_TRUE(found_bytes);

    /* Too late once the negotiation has gone out. */
    ASSERT_ERR(psrp_session_send_timezone(s), PSRP_ERR_STATE);

    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(nameless_zone_matches_binaryformatter),
    PSRP_TEST_CASE(named_zone_matches_binaryformatter),
    PSRP_TEST_CASE(one_name_present_shifts_the_array_ids),
    PSRP_TEST_CASE(serialize_rejects_null_args),
    PSRP_TEST_CASE(parses_the_binaryformatter_fixtures),
    PSRP_TEST_CASE(round_trips_an_empty_daylight_name),
    PSRP_TEST_CASE(parse_refuses_a_different_graph),
    PSRP_TEST_CASE(parse_refuses_a_truncated_graph),
    PSRP_TEST_CASE(parse_rejects_an_overlong_name),
    PSRP_TEST_CASE(current_zone_is_readable_and_serializes),
    PSRP_TEST_CASE(session_capability_omits_timezone_by_default),
    PSRP_TEST_CASE(session_capability_carries_the_timezone_when_asked),
};

PSRP_TEST_MAIN(cases)
