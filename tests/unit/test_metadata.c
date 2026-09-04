#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_metadata.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
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

/* --------------------------------------------------------- CommandType -- */

PSRP_TEST(command_type_names)
{
    ASSERT_EQ_STR(psrp_command_type_name(PSRP_COMMAND_TYPE_CMDLET), "Cmdlet");
    ASSERT_EQ_STR(psrp_command_type_name(PSRP_COMMAND_TYPE_ALIAS), "Alias");
    ASSERT_EQ_STR(psrp_command_type_name(PSRP_COMMAND_TYPE_SCRIPT), "Script");
    ASSERT_EQ_STR(psrp_command_type_name(PSRP_COMMAND_TYPE_ALL), "All");
    /* The 2.2.3.22 example uses 8 for Cmdlet. */
    ASSERT_EQ_I(PSRP_COMMAND_TYPE_CMDLET, 8);
    /* A combination of flags has no single name. */
    ASSERT_NULL(psrp_command_type_name(PSRP_COMMAND_TYPE_ALIAS |
                                       PSRP_COMMAND_TYPE_CMDLET));
    ASSERT_NULL(psrp_command_type_name(0));
    ASSERT_NULL(psrp_command_type_name(12345));
}

/* ------------------------------------------------ GET_COMMAND_METADATA -- */

PSRP_TEST(get_command_metadata_with_patterns)
{
    static const char *patterns[2] = { "Get-*", "Set-Item" };
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *name, *type;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_get_command_metadata(patterns, 2,
                                              PSRP_COMMAND_TYPE_CMDLET, NULL, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    name = psrp_object_find(root.as.obj, "Name");
    ASSERT_NOT_NULL(name);
    ASSERT_EQ_I(name->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_I(psrp_object_container(name->as.obj), PSRP_CONTAINER_LIST);
    ASSERT_EQ_SZ(psrp_object_item_count(name->as.obj), 2u);
    ASSERT_EQ_STR(psrp_object_item(name->as.obj, 0)->as.text.ptr, "Get-*");
    ASSERT_EQ_STR(psrp_object_item(name->as.obj, 1)->as.text.ptr, "Set-Item");

    type = psrp_object_find(root.as.obj, "CommandType");
    ASSERT_NOT_NULL(type);
    ASSERT_EQ_I(psrp_object_primitive(type->as.obj)->as.i32,
                PSRP_COMMAND_TYPE_CMDLET);
    ASSERT_TRUE(xml_contains(&xml, "<ToString>Cmdlet</ToString>"));

    /* Both are explicitly Null, which the spec gives a defined meaning. */
    ASSERT_EQ_I(psrp_object_find(root.as.obj, "Namespace")->kind, PSRP_VAL_NULL);
    ASSERT_EQ_I(psrp_object_find(root.as.obj, "ArgumentList")->kind,
                PSRP_VAL_NULL);

    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

/* No patterns sends Null, which 2.2.2.14 defines as meaning a single "*". */
PSRP_TEST(get_command_metadata_without_patterns_sends_null)
{
    psrp_buffer_t xml;
    psrp_value_t root;
    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_get_command_metadata(NULL, 0,
                                              PSRP_COMMAND_TYPE_ALL, NULL, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    ASSERT_EQ_I(psrp_object_find(root.as.obj, "Name")->kind, PSRP_VAL_NULL);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

/* A flag combination has no name, so the numeric value is used instead. */
PSRP_TEST(get_command_metadata_with_combined_flags)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_get_command_metadata(NULL, 0,
        PSRP_COMMAND_TYPE_ALIAS | PSRP_COMMAND_TYPE_FUNCTION |
        PSRP_COMMAND_TYPE_FILTER | PSRP_COMMAND_TYPE_CMDLET, NULL, &xml));
    ASSERT_TRUE(xml_contains(&xml, "<I32>15</I32>"));
    ASSERT_TRUE(xml_contains(&xml, "<ToString>15</ToString>"));
    psrp_buffer_free(&xml);
}

PSRP_TEST(get_command_metadata_rejects_bad_args)
{
    psrp_buffer_t xml;
    static const char *bad[1] = { NULL };
    psrp_buffer_init(&xml);
    ASSERT_ERR(psrp_build_get_command_metadata(NULL, 0, 0, NULL, NULL),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_get_command_metadata(NULL, 2, 0, NULL, &xml),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_get_command_metadata(bad, 1, 0, NULL, &xml),
               PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&xml);
}

/* ------------------------------------------------------- result stream -- */

/* The 2.2.3.21 example. */
PSRP_TEST(command_metadata_count_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>Selected.Microsoft.PowerShell.Commands.GenericMeasureInfo</T>"
            "<T>System.Management.Automation.PSCustomObject</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<MS><I32 N=\"Count\">1</I32></MS>"
        "</Obj>";
    int32_t count = -1;
    ASSERT_OK(psrp_parse_command_metadata_count(xml, sizeof xml - 1, &count));
    ASSERT_EQ_I(count, 1);
}

PSRP_TEST(command_metadata_count_requires_count)
{
    int32_t count = 0;
    ASSERT_ERR(psrp_parse_command_metadata_count("<Obj />", 7, &count),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_command_metadata_count("<S>x</S>", 8, &count),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_command_metadata_count("<Obj />", 7, NULL),
               PSRP_ERR_INVALID_ARG);
}

/* Modelled on the 2.2.3.22 example. */
PSRP_TEST(command_metadata_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>Selected.System.Management.Automation.CmdletInfo</T>"
            "<T>System.Management.Automation.PSCustomObject</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<MS>"
            "<S N=\"Name\">Get-Variable</S>"
            "<S N=\"Namespace\">Microsoft.PowerShell.Utility</S>"
            "<S N=\"HelpUri\">http://go.microsoft.com/fwlink/?LinkID=113336</S>"
            "<Obj N=\"CommandType\" RefId=\"1\">"
              "<TN RefId=\"1\">"
                "<T>System.Management.Automation.CommandTypes</T>"
                "<T>System.Enum</T><T>System.ValueType</T><T>System.Object</T>"
              "</TN>"
              "<ToString>Cmdlet</ToString><I32>8</I32>"
            "</Obj>"
            "<Nil N=\"ResolvedCommandName\" />"
            "<Obj N=\"Parameters\" RefId=\"3\"><DCT>"
              "<En><S N=\"Key\">Name</S><S N=\"Value\">meta</S></En>"
              "<En><S N=\"Key\">Scope</S><S N=\"Value\">meta</S></En>"
            "</DCT></Obj>"
          "</MS>"
        "</Obj>";
    psrp_command_metadata_t m;
    ASSERT_OK(psrp_parse_command_metadata(xml, sizeof xml - 1, &m));
    ASSERT_EQ_STR(m.name, "Get-Variable");
    ASSERT_EQ_STR(m.command_namespace, "Microsoft.PowerShell.Utility");
    ASSERT_EQ_STR(m.help_uri, "http://go.microsoft.com/fwlink/?LinkID=113336");
    /* CommandType arrives as an enum object, not a bare int. */
    ASSERT_EQ_I(m.command_type, PSRP_COMMAND_TYPE_CMDLET);
    ASSERT_EQ_STR(psrp_command_type_name(m.command_type), "Cmdlet");
    ASSERT_EQ_SZ(m.parameter_count, 2u);
    ASSERT_EQ_STR(m.parameter_names[0], "Name");
    ASSERT_EQ_STR(m.parameter_names[1], "Scope");
    psrp_command_metadata_free(&m);
    psrp_command_metadata_free(&m);      /* idempotent */
}

/* HelpUri is Null when the higher layer gave none; that is not an error. */
PSRP_TEST(command_metadata_tolerates_null_help_uri)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"Name\">Get-Thing</S>"
        "<Nil N=\"HelpUri\" />"
        "</MS></Obj>";
    psrp_command_metadata_t m;
    ASSERT_OK(psrp_parse_command_metadata(xml, sizeof xml - 1, &m));
    ASSERT_EQ_STR(m.name, "Get-Thing");
    ASSERT_NULL(m.help_uri);
    ASSERT_NULL(m.command_namespace);
    ASSERT_EQ_I(m.command_type, -1);
    ASSERT_EQ_SZ(m.parameter_count, 0u);
    psrp_command_metadata_free(&m);
}

/* 2.2.3.22 says Name is a non-empty string, so its absence is malformed. */
PSRP_TEST(command_metadata_requires_a_name)
{
    psrp_command_metadata_t m;
    ASSERT_ERR(psrp_parse_command_metadata(
        "<Obj RefId=\"0\"><MS><S N=\"HelpUri\">u</S></MS></Obj>", 50, &m),
        PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_command_metadata("<S>x</S>", 8, &m),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_command_metadata("junk", 4, &m), PSRP_ERR_XML);
}

/* ---------------------------------------------------------- USER_EVENT -- */

PSRP_TEST(user_event_parses)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"PSEventArgs.EventIdentifier\">3</I32>"
        "<S N=\"PSEventArgs.SourceIdentifier\">MyEvent</S>"
        "<DT N=\"PSEventArgs.TimeGenerated\">2015-03-09T11:00:06.7899543-07:00</DT>"
        "<S N=\"PSEventArgs.ComputerName\">CLAUDE</S>"
        "</MS></Obj>";
    psrp_user_event_t e;
    ASSERT_OK(psrp_parse_user_event(xml, sizeof xml - 1, &e));
    ASSERT_EQ_I(e.event_id, 3);
    ASSERT_EQ_STR(e.source_identifier, "MyEvent");
    ASSERT_EQ_STR(e.time_generated, "2015-03-09T11:00:06.7899543-07:00");
    ASSERT_EQ_STR(e.computer_name, "CLAUDE");
    psrp_user_event_free(&e);
    psrp_user_event_free(&e);
}

/* ComputerName is explicitly allowed to be Null. */
PSRP_TEST(user_event_tolerates_null_computer_name)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"PSEventArgs.EventIdentifier\">1</I32>"
        "<Nil N=\"PSEventArgs.ComputerName\" />"
        "</MS></Obj>";
    psrp_user_event_t e;
    ASSERT_OK(psrp_parse_user_event(xml, sizeof xml - 1, &e));
    ASSERT_EQ_I(e.event_id, 1);
    ASSERT_NULL(e.computer_name);
    ASSERT_NULL(e.source_identifier);
    psrp_user_event_free(&e);
}

/* A USER_EVENT from the server now surfaces as its own event kind rather
 * than falling through to UNKNOWN_MESSAGE. */
PSRP_TEST(user_event_reaches_the_session_as_an_event)
{
    psrp_session_t *s = psrp_session_new();
    psrp_message_t m;
    psrp_buffer_t body, wire;
    psrp_event_t e;
    static const char payload[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"PSEventArgs.EventIdentifier\">7</I32>"
        "<S N=\"PSEventArgs.SourceIdentifier\">Timer</S>"
        "</MS></Obj>";

    ASSERT_NOT_NULL(s);
    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_USER_EVENT;
    m.rpid = *psrp_session_pool_id(s);
    m.data = (const uint8_t *)payload;
    m.data_len = sizeof payload - 1;

    psrp_buffer_init(&body);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_message_encode(&body, &m));
    ASSERT_OK(psrp_fragment_split(&wire, 1, body.data, body.len, 0));
    ASSERT_OK(psrp_session_receive(s, wire.data, wire.len));

    ASSERT_OK(psrp_session_next_event(s, &e));
    ASSERT_EQ_I(e.kind, PSRP_EVENT_USER_EVENT);
    ASSERT_EQ_I(e.state, 7);
    ASSERT_EQ_STR(e.text, "Timer");
    psrp_event_free(&e);

    psrp_buffer_free(&body);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(get_command_metadata_carries_an_argument_list)
{
    /* 2.2.3.24: a list of arbitrary objects the server's higher layer may use
     * to shape what parameter metadata comes back. */
    psrp_buffer_t xml;
    psrp_value_t args, item;
    psrp_object_t *lst;

    lst = psrp_object_new();
    ASSERT_NOT_NULL(lst);
    psrp_object_set_ref_id(lst, 5);
    psrp_object_set_container(lst, PSRP_CONTAINER_LIST);
    psrp_value_init(&item);
    ASSERT_OK(psrp_value_set_string(&item, "extra-argument"));
    ASSERT_OK(psrp_object_add_item(lst, &item));
    psrp_value_free(&item);

    psrp_value_init(&args);
    ASSERT_OK(psrp_value_set_object(&args, lst));

    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_get_command_metadata(NULL, 0,
                                              PSRP_COMMAND_TYPE_ALL,
                                              &args, &xml));
    ASSERT_TRUE(xml_contains(&xml, "N=\"ArgumentList\""));
    ASSERT_TRUE(xml_contains(&xml, "extra-argument"));
    /* The caller keeps their list; it was copied, not consumed. */
    ASSERT_EQ_SZ(psrp_object_item_count(args.as.obj), 1u);

    psrp_buffer_free(&xml);
    psrp_value_free(&args);
}

PSRP_TEST(get_command_metadata_rejects_a_non_list_argument_list)
{
    psrp_buffer_t xml;
    psrp_value_t args;

    psrp_value_init(&args);
    ASSERT_OK(psrp_value_set_string(&args, "not a list"));
    psrp_buffer_init(&xml);
    ASSERT_ERR(psrp_build_get_command_metadata(NULL, 0, 0, &args, &xml),
               PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&xml);
    psrp_value_free(&args);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(command_type_names),
    PSRP_TEST_CASE(get_command_metadata_with_patterns),
    PSRP_TEST_CASE(get_command_metadata_without_patterns_sends_null),
    PSRP_TEST_CASE(get_command_metadata_with_combined_flags),
    PSRP_TEST_CASE(get_command_metadata_rejects_bad_args),
    PSRP_TEST_CASE(get_command_metadata_carries_an_argument_list),
    PSRP_TEST_CASE(get_command_metadata_rejects_a_non_list_argument_list),
    PSRP_TEST_CASE(command_metadata_count_parses_spec_example),
    PSRP_TEST_CASE(command_metadata_count_requires_count),
    PSRP_TEST_CASE(command_metadata_parses_spec_example),
    PSRP_TEST_CASE(command_metadata_tolerates_null_help_uri),
    PSRP_TEST_CASE(command_metadata_requires_a_name),
    PSRP_TEST_CASE(user_event_parses),
    PSRP_TEST_CASE(user_event_tolerates_null_computer_name),
    PSRP_TEST_CASE(user_event_reaches_the_session_as_an_event),
};

PSRP_TEST_MAIN(cases)
