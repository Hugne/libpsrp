#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

static bool xml_contains(const psrp_buffer_t *b, const char *needle)
{
    size_t n = strlen(needle), i;
    if (b->len < n) return false;
    for (i = 0; i + n <= b->len; i++)
        if (memcmp(b->data + i, needle, n) == 0) return true;
    return false;
}

/* Serializes then reparses, which is how these actually travel. */
static void round_trip(const psrp_value_t *v, psrp_value_t *back)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_clixml_serialize(v, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, back));
    psrp_buffer_free(&xml);
}

/* ------------------------------------------------------------- enums --- */

PSRP_TEST(buffer_cell_type_names)
{
    ASSERT_EQ_STR(psrp_buffer_cell_type_name(PSRP_BUFFER_CELL_COMPLETE),
                  "Complete");
    ASSERT_EQ_STR(psrp_buffer_cell_type_name(PSRP_BUFFER_CELL_LEADING),
                  "Leading");
    ASSERT_EQ_STR(psrp_buffer_cell_type_name(PSRP_BUFFER_CELL_TRAILING),
                  "Trailing");
    ASSERT_EQ_STR(psrp_buffer_cell_type_name(3), "Unknown");
    ASSERT_EQ_I(PSRP_BUFFER_CELL_COMPLETE, 0);
    ASSERT_EQ_I(PSRP_BUFFER_CELL_TRAILING, 2);
}

PSRP_TEST(command_origin_names)
{
    ASSERT_EQ_STR(psrp_command_origin_name(PSRP_COMMAND_ORIGIN_RUNSPACE),
                  "Runspace");
    ASSERT_EQ_STR(psrp_command_origin_name(PSRP_COMMAND_ORIGIN_INTERNAL),
                  "Internal");
    ASSERT_EQ_STR(psrp_command_origin_name(2), "Unknown");
}

/* Values transcribed independently from the 2.2.3.27 table. */
PSRP_TEST(control_key_state_flags_match_spec)
{
    ASSERT_EQ_I(PSRP_CONTROL_KEY_RIGHT_ALT, 0x0001);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_LEFT_ALT, 0x0002);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_RIGHT_CTRL, 0x0004);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_LEFT_CTRL, 0x0008);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_SHIFT, 0x0010);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_NUM_LOCK, 0x0020);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_SCROLL_LOCK, 0x0040);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_CAPS_LOCK, 0x0080);
    ASSERT_EQ_I(PSRP_CONTROL_KEY_ENHANCED, 0x0100);
}

/* ----------------------------------------------------------- KeyInfo --- */

/* The 2.2.3.26 example presses 'A', virtual key code 65. */
PSRP_TEST(key_info_round_trip)
{
    psrp_key_info_t k, back;
    psrp_value_t v, reparsed;

    memset(&k, 0, sizeof k);
    k.virtual_key_code = 65;
    k.character = 'A';
    k.control_key_state = PSRP_CONTROL_KEY_SHIFT | PSRP_CONTROL_KEY_LEFT_CTRL;
    k.key_down = true;

    psrp_value_init(&v);
    psrp_value_init(&reparsed);
    ASSERT_OK(psrp_host_make_key_info(&k, &v));
    round_trip(&v, &reparsed);
    ASSERT_OK(psrp_host_read_key_info(&reparsed, &back));

    ASSERT_EQ_I(back.virtual_key_code, 65);
    ASSERT_EQ_I(back.character, 'A');
    ASSERT_EQ_I(back.control_key_state,
                PSRP_CONTROL_KEY_SHIFT | PSRP_CONTROL_KEY_LEFT_CTRL);
    ASSERT_TRUE(back.key_down);

    psrp_value_free(&v);
    psrp_value_free(&reparsed);
}

/* KeyInfo uses extended properties, so its members land in <MS>. */
PSRP_TEST(key_info_uses_extended_properties)
{
    psrp_key_info_t k;
    psrp_value_t v;
    psrp_buffer_t xml;

    memset(&k, 0, sizeof k);
    k.virtual_key_code = 65;
    k.character = 'A';
    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_key_info(&k, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_TRUE(xml_contains(&xml, "<MS>"));
    ASSERT_FALSE(xml_contains(&xml, "<Props>"));
    ASSERT_TRUE(xml_contains(&xml, "<I32 N=\"virtualKeyCode\">65</I32>"));
    /* Character serializes as its numeric code unit, per 2.2.5.1.2. */
    ASSERT_TRUE(xml_contains(&xml, "<C N=\"character\">65</C>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

PSRP_TEST(key_info_rejects_junk)
{
    psrp_key_info_t k;
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "not an object"));
    ASSERT_ERR(psrp_host_read_key_info(&v, &k), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_key_info(NULL, &k), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_make_key_info(NULL, &v), PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);
}

/* -------------------------------------------------------- BufferCell --- */

PSRP_TEST(buffer_cell_round_trip)
{
    psrp_buffer_cell_t c, back;
    psrp_value_t v, reparsed;

    memset(&c, 0, sizeof c);
    c.character = 'x';
    c.foreground_color = PSRP_COLOR_WHITE;
    c.background_color = PSRP_COLOR_BLACK;
    c.cell_type = PSRP_BUFFER_CELL_LEADING;

    psrp_value_init(&v);
    psrp_value_init(&reparsed);
    ASSERT_OK(psrp_host_make_buffer_cell(&c, &v));
    round_trip(&v, &reparsed);
    ASSERT_OK(psrp_host_read_buffer_cell(&reparsed, &back));

    ASSERT_EQ_I(back.character, 'x');
    ASSERT_EQ_I(back.foreground_color, PSRP_COLOR_WHITE);
    /* Black is 0, the value the spec's colour table omits. */
    ASSERT_EQ_I(back.background_color, PSRP_COLOR_BLACK);
    ASSERT_EQ_I(back.cell_type, PSRP_BUFFER_CELL_LEADING);

    psrp_value_free(&v);
    psrp_value_free(&reparsed);
}

/* BufferCell uses adapted properties, and its colours are Color wrappers
 * rather than bare ints. */
PSRP_TEST(buffer_cell_uses_adapted_properties_and_color_wrappers)
{
    psrp_buffer_cell_t c;
    psrp_value_t v;
    psrp_buffer_t xml;

    memset(&c, 0, sizeof c);
    c.character = 'q';
    c.foreground_color = PSRP_COLOR_RED;
    c.background_color = PSRP_COLOR_BLUE;

    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_buffer_cell(&c, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_TRUE(xml_contains(&xml, "<Props>"));
    ASSERT_FALSE(xml_contains(&xml, "<MS><C"));
    ASSERT_TRUE(xml_contains(&xml, "<S N=\"T\">System.ConsoleColor</S>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

PSRP_TEST(buffer_cell_requires_color_wrappers)
{
    psrp_buffer_cell_t c;
    psrp_value_t v;
    /* An object whose colours are bare ints is not a BufferCell. */
    static const char xml[] =
        "<Obj RefId=\"0\"><Props>"
        "<C N=\"character\">120</C>"
        "<I32 N=\"foregroundColor\">15</I32>"
        "<I32 N=\"backgroundColor\">0</I32>"
        "</Props></Obj>";
    psrp_value_init(&v);
    ASSERT_OK(psrp_clixml_deserialize(xml, sizeof xml - 1, &v));
    ASSERT_ERR(psrp_host_read_buffer_cell(&v, &c), PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

/* ------------------------------------------------------- PSCredential -- */

PSRP_TEST(credential_round_trip)
{
    psrp_value_t v, reparsed;
    char *user = NULL, *pass = NULL;

    psrp_value_init(&v);
    psrp_value_init(&reparsed);
    ASSERT_OK(psrp_host_make_credential("CLAUDE\\Administrator",
                                        "Zm9vYmFy", &v));
    round_trip(&v, &reparsed);
    ASSERT_OK(psrp_host_read_credential(&reparsed, &user, &pass));
    ASSERT_EQ_STR(user, "CLAUDE\\Administrator");
    ASSERT_EQ_STR(pass, "Zm9vYmFy");
    free(user);
    free(pass);
    psrp_value_free(&v);
    psrp_value_free(&reparsed);
}

/* 2.2.3.25 says MUST for the type names, and the password must be a
 * SecureString rather than a plain string. */
PSRP_TEST(credential_shape_matches_spec)
{
    psrp_value_t v;
    psrp_buffer_t xml;
    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_credential("user", "Y2lwaGVy", &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_TRUE(xml_contains(&xml,
        "<T>System.Management.Automation.PSCredential</T>"));
    ASSERT_TRUE(xml_contains(&xml, "<T>System.Object</T>"));
    ASSERT_TRUE(xml_contains(&xml, "<Props>"));
    ASSERT_TRUE(xml_contains(&xml, "<SS N=\"Password\">Y2lwaGVy</SS>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

/* A plain-string password is not a credential: refusing it stops a plaintext
 * password being read as if it were protected. */
PSRP_TEST(credential_rejects_plaintext_password)
{
    psrp_value_t v;
    char *user = NULL, *pass = NULL;
    static const char xml[] =
        "<Obj RefId=\"0\"><Props>"
        "<S N=\"UserName\">u</S><S N=\"Password\">hunter2</S>"
        "</Props></Obj>";
    psrp_value_init(&v);
    ASSERT_OK(psrp_clixml_deserialize(xml, sizeof xml - 1, &v));
    ASSERT_ERR(psrp_host_read_credential(&v, &user, &pass),
               PSRP_ERR_MALFORMED);
    ASSERT_NULL(user);
    ASSERT_NULL(pass);
    psrp_value_free(&v);
}

PSRP_TEST(credential_rejects_null_args)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_host_make_credential(NULL, "x", &v), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_credential("u", NULL, &v), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_credential("u", "x", NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_read_credential(NULL, NULL, NULL), PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(buffer_cell_type_names),
    PSRP_TEST_CASE(command_origin_names),
    PSRP_TEST_CASE(control_key_state_flags_match_spec),
    PSRP_TEST_CASE(key_info_round_trip),
    PSRP_TEST_CASE(key_info_uses_extended_properties),
    PSRP_TEST_CASE(key_info_rejects_junk),
    PSRP_TEST_CASE(buffer_cell_round_trip),
    PSRP_TEST_CASE(buffer_cell_uses_adapted_properties_and_color_wrappers),
    PSRP_TEST_CASE(buffer_cell_requires_color_wrappers),
    PSRP_TEST_CASE(credential_round_trip),
    PSRP_TEST_CASE(credential_shape_matches_spec),
    PSRP_TEST_CASE(credential_rejects_plaintext_password),
    PSRP_TEST_CASE(credential_rejects_null_args),
};

PSRP_TEST_MAIN(cases)
