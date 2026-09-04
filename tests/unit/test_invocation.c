/* InvocationInfo-specific extended properties (2.2.3.15.1). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_records.h"
#include "psrp_test.h"

/* An error record as PowerShell emits one when asked to serialize its
 * invocation info: a mix of strings, counts, an enum-wrapped CommandOrigin,
 * and the two open-ended parameter collections. */
static const char kFullXml[] =
    "<Obj RefId=\"0\"><TN RefId=\"0\">"
    "<T>System.Management.Automation.ErrorRecord</T><T>System.Object</T></TN>"
    "<ToString>it went wrong</ToString><MS>"
    "<S N=\"InvocationInfo_InvocationName\">Get-Thing</S>"
    "<S N=\"InvocationInfo_Line\">Get-Thing -Name x | Out-Null</S>"
    "<S N=\"InvocationInfo_PositionMessage\">At line:1 char:1</S>"
    "<S N=\"InvocationInfo_ScriptName\">C:\\scripts\\run.ps1</S>"
    "<I32 N=\"InvocationInfo_OffsetInLine\">1</I32>"
    "<I32 N=\"InvocationInfo_ScriptLineNumber\">12</I32>"
    "<I32 N=\"InvocationInfo_PipelineLength\">2</I32>"
    "<I32 N=\"InvocationInfo_PipelinePosition\">1</I32>"
    "<I64 N=\"InvocationInfo_HistoryId\">7</I64>"
    "<B N=\"InvocationInfo_ExpectingInput\">false</B>"
    "<Obj N=\"InvocationInfo_CommandOrigin\" RefId=\"1\"><TN RefId=\"1\">"
    "<T>System.Management.Automation.CommandOrigin</T><T>System.Enum</T>"
    "<T>System.ValueType</T><T>System.Object</T></TN>"
    "<ToString>Runspace</ToString><I32>0</I32></Obj>"
    "<Obj N=\"InvocationInfo_BoundParameters\" RefId=\"2\"><DCT>"
    "<En><S N=\"Key\">Name</S><S N=\"Value\">x</S></En>"
    "</DCT></Obj>"
    "<Obj N=\"InvocationInfo_UnboundArguments\" RefId=\"3\"><LST>"
    "<S>extra</S></LST></Obj>"
    "<Obj N=\"InvocationInfo_PipelineIterationInfo\" RefId=\"4\"><LST>"
    "<I32>0</I32><I32>1</I32></LST></Obj>"
    "</MS></Obj>";

PSRP_TEST(reads_every_property)
{
    psrp_invocation_info_t i;

    ASSERT_OK(psrp_parse_invocation_info(kFullXml, sizeof kFullXml - 1, &i));
    ASSERT_TRUE(psrp_invocation_info_present(&i));

    ASSERT_EQ_STR(i.invocation_name, "Get-Thing");
    ASSERT_EQ_STR(i.line, "Get-Thing -Name x | Out-Null");
    ASSERT_EQ_STR(i.position_message, "At line:1 char:1");
    ASSERT_EQ_STR(i.script_name, "C:\\scripts\\run.ps1");

    ASSERT_EQ_I(i.offset_in_line, 1);
    ASSERT_EQ_I(i.script_line_number, 12);
    ASSERT_EQ_I(i.pipeline_length, 2);
    ASSERT_EQ_I(i.pipeline_position, 1);
    ASSERT_TRUE(i.history_id == 7);

    /* CommandOrigin arrives wrapped in an enum object, not as a bare int. A
     * reader expecting a plain I32 would silently see nothing. */
    ASSERT_EQ_I(i.command_origin, PSRP_COMMAND_ORIGIN_RUNSPACE);
    ASSERT_EQ_STR(psrp_command_origin_name(i.command_origin), "Runspace");

    ASSERT_TRUE(i.has_expecting_input);
    ASSERT_FALSE(i.expecting_input);

    psrp_invocation_info_free(&i);
}

PSRP_TEST(keeps_the_open_ended_collections_as_objects)
{
    /* The bound parameters and unbound arguments hold arbitrary values, so
     * flattening them to strings would lose their types. */
    psrp_invocation_info_t i;
    const psrp_dict_entry_t *e;

    ASSERT_OK(psrp_parse_invocation_info(kFullXml, sizeof kFullXml - 1, &i));

    ASSERT_EQ_I((int)i.bound_parameters.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_entry_count(i.bound_parameters.as.obj), 1u);
    e = psrp_object_entry(i.bound_parameters.as.obj, 0);
    ASSERT_EQ_STR(e->key.as.text.ptr, "Name");
    ASSERT_EQ_STR(e->value.as.text.ptr, "x");

    ASSERT_EQ_I((int)i.unbound_arguments.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_item_count(i.unbound_arguments.as.obj), 1u);

    ASSERT_EQ_I((int)i.pipeline_iteration_info.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_item_count(i.pipeline_iteration_info.as.obj), 2u);
    ASSERT_EQ_I(psrp_object_item(i.pipeline_iteration_info.as.obj, 1)->as.i32, 1);

    psrp_invocation_info_free(&i);
}

PSRP_TEST(a_record_without_invocation_info_is_not_an_error)
{
    /* PowerShell fills these in only when the record was asked to serialize
     * them, so their absence is ordinary. */
    static const char xml[] =
        "<Obj RefId=\"0\"><ToString>plain</ToString><MS>"
        "<S N=\"FullyQualifiedErrorId\">Oops</S></MS></Obj>";
    psrp_invocation_info_t i;

    ASSERT_OK(psrp_parse_invocation_info(xml, sizeof xml - 1, &i));
    ASSERT_FALSE(psrp_invocation_info_present(&i));
    ASSERT_NULL(i.invocation_name);
    ASSERT_EQ_I(i.pipeline_length, -1);
    ASSERT_EQ_I(i.command_origin, -1);
    ASSERT_FALSE(i.has_expecting_input);
    psrp_invocation_info_free(&i);
}

PSRP_TEST(absent_and_false_expecting_input_differ)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<B N=\"InvocationInfo_ExpectingInput\">true</B></MS></Obj>";
    psrp_invocation_info_t i;

    ASSERT_OK(psrp_parse_invocation_info(xml, sizeof xml - 1, &i));
    ASSERT_TRUE(i.has_expecting_input);
    ASSERT_TRUE(i.expecting_input);
    ASSERT_TRUE(psrp_invocation_info_present(&i));
    psrp_invocation_info_free(&i);
}

PSRP_TEST(a_null_property_reads_as_absent)
{
    /* PowerShell writes Null rather than omitting a property it has no value
     * for, and the two must mean the same thing to a caller. */
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<Nil N=\"InvocationInfo_ScriptName\" />"
        "<Nil N=\"InvocationInfo_BoundParameters\" />"
        "<S N=\"InvocationInfo_InvocationName\">Get-Thing</S>"
        "</MS></Obj>";
    psrp_invocation_info_t i;

    ASSERT_OK(psrp_parse_invocation_info(xml, sizeof xml - 1, &i));
    ASSERT_NULL(i.script_name);
    ASSERT_EQ_I((int)i.bound_parameters.kind, (int)PSRP_VAL_NULL);
    ASSERT_EQ_STR(i.invocation_name, "Get-Thing");
    psrp_invocation_info_free(&i);
}

PSRP_TEST(a_narrowed_history_id_is_accepted)
{
    /* The spec types HistoryId as a Signed Long, but a small one sometimes
     * arrives narrowed to an I32. Refusing it would drop a usable value. */
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"InvocationInfo_HistoryId\">3</I32></MS></Obj>";
    psrp_invocation_info_t i;

    ASSERT_OK(psrp_parse_invocation_info(xml, sizeof xml - 1, &i));
    ASSERT_TRUE(i.history_id == 3);
    psrp_invocation_info_free(&i);
}

PSRP_TEST(rejects_malformed_input)
{
    psrp_invocation_info_t i;
    ASSERT_TRUE(psrp_parse_invocation_info("<not xml", 8, &i) != PSRP_OK);
    /* A primitive at the root is well-formed XML but not a record. */
    ASSERT_ERR(psrp_parse_invocation_info("<S>x</S>", 8, &i),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_invocation_info("<Obj />", 7, NULL),
               PSRP_ERR_INVALID_ARG);
}

PSRP_TEST(free_is_idempotent)
{
    psrp_invocation_info_t i;
    ASSERT_OK(psrp_parse_invocation_info(kFullXml, sizeof kFullXml - 1, &i));
    psrp_invocation_info_free(&i);
    psrp_invocation_info_free(&i);
    psrp_invocation_info_free(NULL);
    ASSERT_NULL(i.invocation_name);
    ASSERT_EQ_I(i.pipeline_length, -1);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(reads_every_property),
    PSRP_TEST_CASE(keeps_the_open_ended_collections_as_objects),
    PSRP_TEST_CASE(a_record_without_invocation_info_is_not_an_error),
    PSRP_TEST_CASE(absent_and_false_expecting_input_differ),
    PSRP_TEST_CASE(a_null_property_reads_as_absent),
    PSRP_TEST_CASE(a_narrowed_history_id_is_accepted),
    PSRP_TEST_CASE(rejects_malformed_input),
    PSRP_TEST_CASE(free_is_idempotent),
};

PSRP_TEST_MAIN(cases)
