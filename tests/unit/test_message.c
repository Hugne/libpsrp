#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
#include "psrp_test.h"

/* Every type code from [MS-PSRP] 2.2.2, transcribed from the spec table.
 * This is the authoritative list; a typo here is a wire-compat bug. */
static const struct { uint32_t code; const char *name; } kSpecTypes[] = {
    { 0x00010002u, "SESSION_CAPABILITY" },
    { 0x00010004u, "INIT_RUNSPACEPOOL" },
    { 0x00010005u, "PUBLIC_KEY" },
    { 0x00010006u, "ENCRYPTED_SESSION_KEY" },
    { 0x00010007u, "PUBLIC_KEY_REQUEST" },
    { 0x00010008u, "CONNECT_RUNSPACEPOOL" },
    { 0x00021002u, "SET_MAX_RUNSPACES" },
    { 0x00021003u, "SET_MIN_RUNSPACES" },
    { 0x00021004u, "RUNSPACE_AVAILABILITY" },
    { 0x00021005u, "RUNSPACEPOOL_STATE" },
    { 0x00021006u, "CREATE_PIPELINE" },
    { 0x00021007u, "GET_AVAILABLE_RUNSPACES" },
    { 0x00021008u, "USER_EVENT" },
    { 0x00021009u, "APPLICATION_PRIVATE_DATA" },
    { 0x0002100Au, "GET_COMMAND_METADATA" },
    { 0x0002100Bu, "RUNSPACEPOOL_INIT_DATA" },
    { 0x0002100Cu, "RESET_RUNSPACE_STATE" },
    { 0x00021100u, "RUNSPACEPOOL_HOST_CALL" },
    { 0x00021101u, "RUNSPACEPOOL_HOST_RESPONSE" },
    { 0x00041002u, "PIPELINE_INPUT" },
    { 0x00041003u, "END_OF_PIPELINE_INPUT" },
    { 0x00041004u, "PIPELINE_OUTPUT" },
    { 0x00041005u, "ERROR_RECORD" },
    { 0x00041006u, "PIPELINE_STATE" },
    { 0x00041007u, "DEBUG_RECORD" },
    { 0x00041008u, "VERBOSE_RECORD" },
    { 0x00041009u, "WARNING_RECORD" },
    { 0x00041010u, "PROGRESS_RECORD" },
    { 0x00041011u, "INFORMATION_RECORD" },
    { 0x00041100u, "PIPELINE_HOST_CALL" },
    { 0x00041101u, "PIPELINE_HOST_RESPONSE" }
};
#define SPEC_TYPE_COUNT (sizeof kSpecTypes / sizeof kSpecTypes[0])

PSRP_TEST(message_type_table_matches_spec)
{
    size_t i;
    ASSERT_EQ_SZ(SPEC_TYPE_COUNT, 31u);   /* 2.2.2 defines 31 message types */
    for (i = 0; i < SPEC_TYPE_COUNT; i++) {
        ASSERT_TRUE(psrp_message_type_known(kSpecTypes[i].code));
        ASSERT_EQ_STR(psrp_message_type_name(kSpecTypes[i].code),
                      kSpecTypes[i].name);
    }
}

PSRP_TEST(message_type_enum_values_match_spec)
{
    /* Guards against a transcription slip in the public enum. */
    ASSERT_EQ_SZ(PSRP_MSG_SESSION_CAPABILITY,       0x00010002u);
    ASSERT_EQ_SZ(PSRP_MSG_INIT_RUNSPACEPOOL,        0x00010004u);
    ASSERT_EQ_SZ(PSRP_MSG_CONNECT_RUNSPACEPOOL,     0x00010008u);
    ASSERT_EQ_SZ(PSRP_MSG_RUNSPACEPOOL_STATE,       0x00021005u);
    ASSERT_EQ_SZ(PSRP_MSG_CREATE_PIPELINE,          0x00021006u);
    ASSERT_EQ_SZ(PSRP_MSG_GET_COMMAND_METADATA,     0x0002100Au);
    ASSERT_EQ_SZ(PSRP_MSG_RESET_RUNSPACE_STATE,     0x0002100Cu);
    ASSERT_EQ_SZ(PSRP_MSG_PIPELINE_OUTPUT,          0x00041004u);
    ASSERT_EQ_SZ(PSRP_MSG_WARNING_RECORD,           0x00041009u);
    /* The table jumps from 0x...09 to 0x...10 here - easy to get wrong. */
    ASSERT_EQ_SZ(PSRP_MSG_PROGRESS_RECORD,          0x00041010u);
    ASSERT_EQ_SZ(PSRP_MSG_INFORMATION_RECORD,       0x00041011u);
    ASSERT_EQ_SZ(PSRP_MSG_PIPELINE_HOST_RESPONSE,   0x00041101u);
}

PSRP_TEST(message_type_names_are_unique)
{
    size_t i, j;
    for (i = 0; i < SPEC_TYPE_COUNT; i++)
        for (j = i + 1; j < SPEC_TYPE_COUNT; j++) {
            if (kSpecTypes[i].code == kSpecTypes[j].code)
                PSRP_FAIL("duplicate code 0x%08lx",
                          (unsigned long)kSpecTypes[i].code);
            if (strcmp(kSpecTypes[i].name, kSpecTypes[j].name) == 0)
                PSRP_FAIL("duplicate name %s", kSpecTypes[i].name);
        }
}

PSRP_TEST(message_unknown_type_is_reported_not_rejected)
{
    ASSERT_FALSE(psrp_message_type_known(0x00099999u));
    ASSERT_EQ_STR(psrp_message_type_name(0x00099999u), "UNKNOWN");
    ASSERT_FALSE(psrp_message_type_is_pipeline(0x00049999u));
}

PSRP_TEST(message_pipeline_targeting)
{
    ASSERT_TRUE(psrp_message_type_is_pipeline(PSRP_MSG_PIPELINE_OUTPUT));
    ASSERT_TRUE(psrp_message_type_is_pipeline(PSRP_MSG_ERROR_RECORD));
    ASSERT_TRUE(psrp_message_type_is_pipeline(PSRP_MSG_PIPELINE_HOST_CALL));
    ASSERT_FALSE(psrp_message_type_is_pipeline(PSRP_MSG_SESSION_CAPABILITY));
    ASSERT_FALSE(psrp_message_type_is_pipeline(PSRP_MSG_RUNSPACEPOOL_STATE));
}

/* ------------------------------------------------------- wire layout ----- */

/* Byte-exact header: little-endian integers (2.2), .NET GUID field order. */
PSRP_TEST(message_encode_wire_layout)
{
    psrp_buffer_t b;
    psrp_message_t m;
    static const uint8_t want[] = {
        0x02, 0x00, 0x00, 0x00,                          /* Destination = 2 (LE) */
        0x02, 0x00, 0x01, 0x00,                          /* Type = 0x00010002 (LE) */
        /* RPID 00112233-4455-6677-8899-aabbccddeeff in .NET wire order */
        0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        /* PID all zero */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'h', 'i'
    };

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_SERVER;
    m.type = PSRP_MSG_SESSION_CAPABILITY;
    ASSERT_OK(psrp_guid_parse("00112233-4455-6677-8899-aabbccddeeff", &m.rpid));
    m.pid = psrp_guid_empty;
    m.data = (const uint8_t *)"hi";
    m.data_len = 2;

    ASSERT_OK(psrp_message_encode(&b, &m));
    ASSERT_EQ_SZ(b.len, PSRP_MESSAGE_HEADER_SIZE + 2u);
    ASSERT_EQ_MEM(b.data, b.len, want, sizeof want);
    psrp_buffer_free(&b);
}

PSRP_TEST(message_roundtrip)
{
    psrp_buffer_t b;
    psrp_message_t m, g;
    static const char xml[] = "<Obj RefId=\"0\"><MS/></Obj>";

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_PIPELINE_OUTPUT;
    ASSERT_OK(psrp_guid_parse("11112222-3333-4444-5555-666677778888", &m.rpid));
    ASSERT_OK(psrp_guid_parse("99990000-aaaa-bbbb-cccc-ddddeeeeffff", &m.pid));
    m.data = (const uint8_t *)xml;
    m.data_len = sizeof xml - 1;
    ASSERT_OK(psrp_message_encode(&b, &m));

    ASSERT_OK(psrp_message_decode(b.data, b.len, &g));
    ASSERT_EQ_SZ(g.destination, PSRP_DEST_CLIENT);
    ASSERT_EQ_SZ(g.type, PSRP_MSG_PIPELINE_OUTPUT);
    ASSERT_TRUE(psrp_guid_equal(&g.rpid, &m.rpid));
    ASSERT_TRUE(psrp_guid_equal(&g.pid, &m.pid));
    ASSERT_EQ_MEM(g.data, g.data_len, xml, sizeof xml - 1);
    psrp_buffer_free(&b);
}

PSRP_TEST(message_roundtrip_empty_data)
{
    psrp_buffer_t b;
    psrp_message_t m, g;

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_SERVER;
    m.type = PSRP_MSG_END_OF_PIPELINE_INPUT;
    ASSERT_OK(psrp_message_encode(&b, &m));
    ASSERT_EQ_SZ(b.len, PSRP_MESSAGE_HEADER_SIZE);

    ASSERT_OK(psrp_message_decode(b.data, b.len, &g));
    ASSERT_EQ_SZ(g.data_len, 0u);
    ASSERT_NULL(g.data);
    psrp_buffer_free(&b);
}

/* Every type must survive a header round-trip; catches enum/pack slips. */
PSRP_TEST(message_roundtrip_every_type)
{
    size_t i;
    for (i = 0; i < SPEC_TYPE_COUNT; i++) {
        psrp_buffer_t b;
        psrp_message_t m, g;
        psrp_buffer_init(&b);
        memset(&m, 0, sizeof m);
        m.destination = PSRP_DEST_SERVER;
        m.type = kSpecTypes[i].code;
        ASSERT_OK(psrp_message_encode(&b, &m));
        ASSERT_OK(psrp_message_decode(b.data, b.len, &g));
        ASSERT_EQ_SZ(g.type, kSpecTypes[i].code);
        ASSERT_EQ_STR(psrp_message_type_name(g.type), kSpecTypes[i].name);
        psrp_buffer_free(&b);
    }
}

PSRP_TEST(message_decode_truncated)
{
    psrp_buffer_t b;
    psrp_message_t m, g;
    size_t i;

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_RUNSPACEPOOL_STATE;
    ASSERT_OK(psrp_message_encode(&b, &m));

    for (i = 0; i < PSRP_MESSAGE_HEADER_SIZE; i++)
        ASSERT_ERR(psrp_message_decode(b.data, i, &g), PSRP_ERR_TRUNCATED);
    ASSERT_OK(psrp_message_decode(b.data, PSRP_MESSAGE_HEADER_SIZE, &g));
    psrp_buffer_free(&b);
}

PSRP_TEST(message_decode_rejects_bad_destination)
{
    psrp_buffer_t b;
    psrp_message_t m, g;

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_SESSION_CAPABILITY;
    ASSERT_OK(psrp_message_encode(&b, &m));

    b.data[0] = 0x00;   /* destination 0 */
    ASSERT_ERR(psrp_message_decode(b.data, b.len, &g), PSRP_ERR_MALFORMED);
    b.data[0] = 0x03;   /* destination 3 */
    ASSERT_ERR(psrp_message_decode(b.data, b.len, &g), PSRP_ERR_MALFORMED);
    b.data[0] = 0x01;
    ASSERT_OK(psrp_message_decode(b.data, b.len, &g));
    psrp_buffer_free(&b);
}

PSRP_TEST(message_decode_preserves_unknown_type)
{
    psrp_buffer_t b;
    psrp_message_t m, g;

    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = 0x00012345u;             /* not in the spec table */
    ASSERT_OK(psrp_message_encode(&b, &m));
    /* Decoding must succeed and hand the raw value to the caller. */
    ASSERT_OK(psrp_message_decode(b.data, b.len, &g));
    ASSERT_EQ_SZ(g.type, 0x00012345u);
    ASSERT_FALSE(psrp_message_type_known(g.type));
    psrp_buffer_free(&b);
}

PSRP_TEST(message_encode_rejects_bad_destination)
{
    psrp_buffer_t b;
    psrp_message_t m;
    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = 0;
    m.type = PSRP_MSG_SESSION_CAPABILITY;
    ASSERT_ERR(psrp_message_encode(&b, &m), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&b);
}

/* --------------------------------------------------------------- BOM ----- */

PSRP_TEST(message_xml_strips_utf8_bom)
{
    psrp_message_t m;
    const uint8_t *xml = NULL;
    size_t n = 0;
    static const uint8_t with_bom[] = { 0xEF, 0xBB, 0xBF, '<', 'O', '/', '>' };
    static const uint8_t without[]  = { '<', 'O', '/', '>' };

    memset(&m, 0, sizeof m);
    m.data = with_bom; m.data_len = sizeof with_bom;
    psrp_message_xml(&m, &xml, &n);
    ASSERT_EQ_MEM(xml, n, without, sizeof without);

    /* No BOM: passed through untouched. */
    m.data = without; m.data_len = sizeof without;
    psrp_message_xml(&m, &xml, &n);
    ASSERT_EQ_MEM(xml, n, without, sizeof without);

    /* Empty data. */
    m.data = NULL; m.data_len = 0;
    psrp_message_xml(&m, &xml, &n);
    ASSERT_EQ_SZ(n, 0u);
    ASSERT_NULL(xml);
}

PSRP_TEST(message_encode_writes_no_bom)
{
    psrp_buffer_t b;
    psrp_message_t m;
    psrp_buffer_init(&b);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_SERVER;
    m.type = PSRP_MSG_SESSION_CAPABILITY;
    m.data = (const uint8_t *)"<Obj/>";
    m.data_len = 6;
    ASSERT_OK(psrp_message_encode(&b, &m));
    ASSERT_EQ_I(b.data[PSRP_MESSAGE_HEADER_SIZE], '<');
    psrp_buffer_free(&b);
}

/* --------------------------------------------- message over fragments ---- */

/* The layer above fragments: a message must survive being split, streamed in
 * awkward chunks, reassembled, and decoded. */
PSRP_TEST(message_survives_fragmentation_roundtrip)
{
    psrp_buffer_t msg, wire, reassembled;
    psrp_message_t m, g;
    psrp_defrag_t *d;
    char *xml;
    size_t i, xml_len = 5000;
    uint64_t oid = 0;

    xml = (char *)malloc(xml_len);
    ASSERT_NOT_NULL(xml);
    for (i = 0; i < xml_len; i++) xml[i] = (char)('a' + (i % 26));

    psrp_buffer_init(&msg);
    psrp_buffer_init(&wire);
    psrp_buffer_init(&reassembled);

    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_PIPELINE_OUTPUT;
    ASSERT_OK(psrp_guid_parse("deadbeef-1234-5678-9abc-def012345678", &m.rpid));
    m.data = (const uint8_t *)xml;
    m.data_len = xml_len;
    ASSERT_OK(psrp_message_encode(&msg, &m));

    ASSERT_OK(psrp_fragment_split(&wire, 1, msg.data, msg.len, 512));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    for (i = 0; i < wire.len; i += 37)          /* deliberately awkward chunk */
        ASSERT_OK(psrp_defrag_push(d, wire.data + i,
                                   (wire.len - i > 37) ? 37 : wire.len - i));
    ASSERT_OK(psrp_defrag_next(d, &oid, &reassembled));
    ASSERT_EQ_SZ(oid, 1u);
    ASSERT_EQ_MEM(reassembled.data, reassembled.len, msg.data, msg.len);

    ASSERT_OK(psrp_message_decode(reassembled.data, reassembled.len, &g));
    ASSERT_EQ_SZ(g.type, PSRP_MSG_PIPELINE_OUTPUT);
    ASSERT_TRUE(psrp_guid_equal(&g.rpid, &m.rpid));
    ASSERT_EQ_MEM(g.data, g.data_len, xml, xml_len);

    psrp_defrag_free(d);
    psrp_buffer_free(&msg);
    psrp_buffer_free(&wire);
    psrp_buffer_free(&reassembled);
    free(xml);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(message_type_table_matches_spec),
    PSRP_TEST_CASE(message_type_enum_values_match_spec),
    PSRP_TEST_CASE(message_type_names_are_unique),
    PSRP_TEST_CASE(message_unknown_type_is_reported_not_rejected),
    PSRP_TEST_CASE(message_pipeline_targeting),
    PSRP_TEST_CASE(message_encode_wire_layout),
    PSRP_TEST_CASE(message_roundtrip),
    PSRP_TEST_CASE(message_roundtrip_empty_data),
    PSRP_TEST_CASE(message_roundtrip_every_type),
    PSRP_TEST_CASE(message_decode_truncated),
    PSRP_TEST_CASE(message_decode_rejects_bad_destination),
    PSRP_TEST_CASE(message_decode_preserves_unknown_type),
    PSRP_TEST_CASE(message_encode_rejects_bad_destination),
    PSRP_TEST_CASE(message_xml_strips_utf8_bom),
    PSRP_TEST_CASE(message_encode_writes_no_bom),
    PSRP_TEST_CASE(message_survives_fragmentation_roundtrip),
};

PSRP_TEST_MAIN(cases)
