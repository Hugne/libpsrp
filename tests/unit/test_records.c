#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_records.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

static void check_text(const psrp_value_t *v, const char *want)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_value_to_text(v, &out));
    ASSERT_EQ_MEM(out.data, out.len, want, strlen(want));
    psrp_buffer_free(&out);
}

/* --------------------------------------------------------- categories --- */

PSRP_TEST(error_category_names_match_spec)
{
    ASSERT_EQ_STR(psrp_error_category_name(0), "NotSpecified");
    ASSERT_EQ_STR(psrp_error_category_name(5), "InvalidArgument");
    ASSERT_EQ_STR(psrp_error_category_name(13), "ObjectNotFound");
    ASSERT_EQ_STR(psrp_error_category_name(18), "PermissionDenied");
    ASSERT_EQ_STR(psrp_error_category_name(22), "ReadError");
    ASSERT_EQ_STR(psrp_error_category_name(25), "SecurityError");
    /* The spec's table skips 23 and 24. */
    ASSERT_EQ_STR(psrp_error_category_name(23), "Unknown");
    ASSERT_EQ_STR(psrp_error_category_name(24), "Unknown");
    ASSERT_EQ_STR(psrp_error_category_name(-1), "Unknown");
    ASSERT_EQ_STR(psrp_error_category_name(999), "Unknown");
}

/* -------------------------------------------------------- ErrorRecord --- */

PSRP_TEST(error_record_parses)
{
    static const char xml[] =
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.ErrorRecord</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<ToString>The term 'bogus' is not recognized.</ToString>"
          "<MS>"
            "<S N=\"FullyQualifiedErrorId\">CommandNotFoundException</S>"
            "<I32 N=\"ErrorCategory_Category\">13</I32>"
            "<S N=\"ErrorCategory_Reason\">CommandNotFoundException</S>"
            "<S N=\"ErrorCategory_TargetName\">bogus</S>"
            "<S N=\"ErrorCategory_Activity\">Invoke</S>"
            "<S N=\"ErrorCategory_Message\">ObjectNotFound: (bogus)</S>"
          "</MS>"
        "</Obj>";
    psrp_error_record_t r;
    ASSERT_OK(psrp_parse_error_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.message, "The term 'bogus' is not recognized.");
    ASSERT_EQ_STR(r.fully_qualified_error_id, "CommandNotFoundException");
    ASSERT_EQ_I(r.category, 13);
    ASSERT_EQ_STR(psrp_error_category_name(r.category), "ObjectNotFound");
    ASSERT_EQ_STR(r.category_reason, "CommandNotFoundException");
    ASSERT_EQ_STR(r.target_name, "bogus");
    ASSERT_EQ_STR(r.category_activity, "Invoke");
    ASSERT_EQ_STR(r.category_message, "ObjectNotFound: (bogus)");
    psrp_error_record_free(&r);
    psrp_error_record_free(&r);      /* idempotent */
}

/* Optional fields are Null far more often than absent; both mean "no value". */
PSRP_TEST(error_record_tolerates_null_and_missing_fields)
{
    static const char xml[] =
        "<Obj RefId=\"0\">"
          "<ToString>boom</ToString>"
          "<MS>"
            "<Nil N=\"ErrorCategory_Reason\" />"
            "<Nil N=\"ErrorCategory_TargetName\" />"
          "</MS>"
        "</Obj>";
    psrp_error_record_t r;
    ASSERT_OK(psrp_parse_error_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.message, "boom");
    ASSERT_NULL(r.category_reason);
    ASSERT_NULL(r.target_name);
    ASSERT_NULL(r.fully_qualified_error_id);
    ASSERT_EQ_I(r.category, -1);      /* absent, not zero */
    psrp_error_record_free(&r);
}

PSRP_TEST(error_record_rejects_non_object)
{
    psrp_error_record_t r;
    ASSERT_ERR(psrp_parse_error_record("<S>x</S>", 8, &r), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_error_record("junk", 4, &r), PSRP_ERR_XML);
    ASSERT_ERR(psrp_parse_error_record("<S/>", 4, NULL), PSRP_ERR_INVALID_ARG);
}

/* ------------------------------------------- informational records ------ */

PSRP_TEST(informational_record_parses)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">a warning</S>"
        "<B N=\"InformationalRecord_SerializeInvocationInfo\">false</B>"
        "</MS></Obj>";
    psrp_informational_record_t r;
    ASSERT_OK(psrp_parse_informational_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.message, "a warning");
    ASSERT_FALSE(r.has_invocation_info);
    psrp_informational_record_free(&r);
}

PSRP_TEST(informational_record_notes_invocation_info)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">verbose text</S>"
        "<B N=\"InformationalRecord_SerializeInvocationInfo\">true</B>"
        "</MS></Obj>";
    psrp_informational_record_t r;
    ASSERT_OK(psrp_parse_informational_record(xml, sizeof xml - 1, &r));
    ASSERT_TRUE(r.has_invocation_info);
    psrp_informational_record_free(&r);
}

/* Escaped content must survive into the parsed message. */
PSRP_TEST(informational_record_decodes_escapes)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">line1_x000A_line2 &amp; more</S>"
        "</MS></Obj>";
    psrp_informational_record_t r;
    ASSERT_OK(psrp_parse_informational_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.message, "line1\nline2 & more");
    psrp_informational_record_free(&r);
}

/* ---------------------------------------------------- ProgressRecord ---- */

/* The example from 2.2.5.1.25. */
PSRP_TEST(progress_record_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"Activity\">Activity Name</S>"
        "<I32 N=\"ActivityId\">4</I32>"
        "<S N=\"StatusDescription\">Good</S>"
        "<S N=\"CurrentOperation\">Down loading</S>"
        "<I32 N=\"ParentActivityId\">-1</I32>"
        "<I32 N=\"PercentComplete\">20</I32>"
        "<I32 N=\"SecondsRemaining\">30</I32>"
        "</MS></Obj>";
    psrp_progress_record_t r;
    ASSERT_OK(psrp_parse_progress_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.activity, "Activity Name");
    ASSERT_EQ_I(r.activity_id, 4);
    ASSERT_EQ_STR(r.status_description, "Good");
    ASSERT_EQ_STR(r.current_operation, "Down loading");
    ASSERT_EQ_I(r.parent_activity_id, -1);
    ASSERT_EQ_I(r.percent_complete, 20);
    ASSERT_EQ_I(r.seconds_remaining, 30);
    psrp_progress_record_free(&r);
}

PSRP_TEST(progress_record_defaults_missing_numbers_to_minus_one)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS><S N=\"Activity\">A</S></MS></Obj>";
    psrp_progress_record_t r;
    ASSERT_OK(psrp_parse_progress_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_I(r.percent_complete, -1);
    ASSERT_EQ_I(r.seconds_remaining, -1);
    ASSERT_NULL(r.status_description);
    psrp_progress_record_free(&r);
}

/* ------------------------------------------------- InformationRecord ---- */

/* The 2.2.2.26 example uses <Props> (adapted), not <MS>. */
PSRP_TEST(information_record_parses_adapted_properties)
{
    static const char xml[] =
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.InformationRecord</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<ToString>Information Data</ToString>"
          "<Props>"
            "<S N=\"MessageData\">Information Data</S>"
            "<S N=\"Source\">Write-Information</S>"
            "<DT N=\"TimeGenerated\">2015-03-09T11:00:06.7899543-07:00</DT>"
          "</Props>"
        "</Obj>";
    psrp_information_record_t r;
    ASSERT_OK(psrp_parse_information_record(xml, sizeof xml - 1, &r));
    ASSERT_EQ_STR(r.message_data, "Information Data");
    ASSERT_EQ_STR(r.source, "Write-Information");
    ASSERT_EQ_STR(r.time_generated, "2015-03-09T11:00:06.7899543-07:00");
    psrp_information_record_free(&r);
}

/* ------------------------------------------------------- pipeline I/O --- */

PSRP_TEST(pipeline_output_parses_any_value)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_parse_pipeline_output("<S>CLAUDE</S>", 13, &v));
    ASSERT_EQ_I(v.kind, PSRP_VAL_STRING);
    check_text(&v, "CLAUDE");
    psrp_value_free(&v);

    ASSERT_OK(psrp_parse_pipeline_output("<I32>7</I32>", 12, &v));
    check_text(&v, "7");
    psrp_value_free(&v);
}

PSRP_TEST(pipeline_input_roundtrips)
{
    psrp_value_t v, back;
    psrp_buffer_t xml;
    psrp_value_init(&v);
    psrp_value_init(&back);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_value_set_string(&v, "input line"));
    ASSERT_OK(psrp_build_pipeline_input(&v, &xml));
    ASSERT_OK(psrp_parse_pipeline_output(xml.data, xml.len, &back));
    check_text(&back, "input line");
    psrp_value_free(&v);
    psrp_value_free(&back);
    psrp_buffer_free(&xml);
}

/* ---------------------------------------------------------- rendering --- */

PSRP_TEST(value_to_text_covers_scalars)
{
    psrp_value_t v;
    psrp_guid_t g;
    psrp_value_init(&v);

    psrp_value_set_null(&v);            check_text(&v, "");
    /* PowerShell renders booleans capitalised. */
    psrp_value_set_bool(&v, true);      check_text(&v, "True");
    psrp_value_set_bool(&v, false);     check_text(&v, "False");
    psrp_value_set_int32(&v, -42);      check_text(&v, "-42");
    psrp_value_set_uint64(&v, 18446744073709551615ull);
    check_text(&v, "18446744073709551615");
    psrp_value_set_double(&v, 12.5);    check_text(&v, "12.5");
    psrp_value_set_char(&v, 'a');       check_text(&v, "a");

    ASSERT_OK(psrp_guid_parse("792e5b37-4505-47ef-b7d2-8711bb7affa8", &g));
    psrp_value_set_guid(&v, &g);
    check_text(&v, "792e5b37-4505-47ef-b7d2-8711bb7affa8");

    ASSERT_OK(psrp_value_set_string(&v, "plain"));  check_text(&v, "plain");
    psrp_value_free(&v);
}

PSRP_TEST(value_to_text_uses_object_tostring)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    ASSERT_OK(psrp_object_set_to_string(o, "rendered form", 13));
    ASSERT_OK(psrp_value_set_object(&v, o));
    check_text(&v, "rendered form");
    psrp_value_free(&v);
}

/* An extended primitive (an enum, say) renders as its underlying value when
 * it has no ToString. */
PSRP_TEST(value_to_text_falls_back_to_primitive)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, prim;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&prim);
    psrp_value_set_int32(&prim, 3);
    ASSERT_OK(psrp_object_set_primitive(o, &prim));
    ASSERT_OK(psrp_value_set_object(&v, o));
    check_text(&v, "3");
    psrp_value_free(&v);
    psrp_value_free(&prim);
}

PSRP_TEST(value_to_text_rejects_null_args)
{
    psrp_buffer_t out;
    psrp_value_t v;
    psrp_buffer_init(&out);
    psrp_value_init(&v);
    ASSERT_ERR(psrp_value_to_text(NULL, &out), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_value_to_text(&v, NULL), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&out);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(error_category_names_match_spec),
    PSRP_TEST_CASE(error_record_parses),
    PSRP_TEST_CASE(error_record_tolerates_null_and_missing_fields),
    PSRP_TEST_CASE(error_record_rejects_non_object),
    PSRP_TEST_CASE(informational_record_parses),
    PSRP_TEST_CASE(informational_record_notes_invocation_info),
    PSRP_TEST_CASE(informational_record_decodes_escapes),
    PSRP_TEST_CASE(progress_record_parses_spec_example),
    PSRP_TEST_CASE(progress_record_defaults_missing_numbers_to_minus_one),
    PSRP_TEST_CASE(information_record_parses_adapted_properties),
    PSRP_TEST_CASE(pipeline_output_parses_any_value),
    PSRP_TEST_CASE(pipeline_input_roundtrips),
    PSRP_TEST_CASE(value_to_text_covers_scalars),
    PSRP_TEST_CASE(value_to_text_uses_object_tostring),
    PSRP_TEST_CASE(value_to_text_falls_back_to_primitive),
    PSRP_TEST_CASE(value_to_text_rejects_null_args),
};

PSRP_TEST_MAIN(cases)
