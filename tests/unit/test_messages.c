#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* ------------------------------------------------------------- enums ----- */

/* Values transcribed independently from [MS-PSRP] 2.2.3.4 and 2.2.3.5. */
PSRP_TEST(state_enum_values_match_spec)
{
    ASSERT_EQ_I(PSRP_RUNSPACE_BEFORE_OPEN, 0);
    ASSERT_EQ_I(PSRP_RUNSPACE_OPENING, 1);
    ASSERT_EQ_I(PSRP_RUNSPACE_OPENED, 2);
    ASSERT_EQ_I(PSRP_RUNSPACE_CLOSED, 3);
    ASSERT_EQ_I(PSRP_RUNSPACE_CLOSING, 4);
    ASSERT_EQ_I(PSRP_RUNSPACE_BROKEN, 5);
    ASSERT_EQ_I(PSRP_RUNSPACE_NEGOTIATION_SENT, 6);
    ASSERT_EQ_I(PSRP_RUNSPACE_NEGOTIATION_SUCCEEDED, 7);
    ASSERT_EQ_I(PSRP_RUNSPACE_CONNECTING, 8);
    ASSERT_EQ_I(PSRP_RUNSPACE_DISCONNECTED, 9);

    ASSERT_EQ_I(PSRP_INVOCATION_NOT_STARTED, 0);
    ASSERT_EQ_I(PSRP_INVOCATION_RUNNING, 1);
    ASSERT_EQ_I(PSRP_INVOCATION_STOPPING, 2);
    ASSERT_EQ_I(PSRP_INVOCATION_STOPPED, 3);
    ASSERT_EQ_I(PSRP_INVOCATION_COMPLETED, 4);
    ASSERT_EQ_I(PSRP_INVOCATION_FAILED, 5);
    ASSERT_EQ_I(PSRP_INVOCATION_DISCONNECTED, 6);
}

PSRP_TEST(state_names)
{
    ASSERT_EQ_STR(psrp_runspace_pool_state_name(2), "Opened");
    ASSERT_EQ_STR(psrp_runspace_pool_state_name(7), "NegotiationSucceeded");
    ASSERT_EQ_STR(psrp_runspace_pool_state_name(99), "Unknown");
    ASSERT_EQ_STR(psrp_invocation_state_name(4), "Completed");
    ASSERT_EQ_STR(psrp_invocation_state_name(-1), "Unknown");
}

PSRP_TEST(terminal_states)
{
    ASSERT_TRUE(psrp_runspace_pool_state_is_terminal(PSRP_RUNSPACE_CLOSED));
    ASSERT_TRUE(psrp_runspace_pool_state_is_terminal(PSRP_RUNSPACE_BROKEN));
    ASSERT_FALSE(psrp_runspace_pool_state_is_terminal(PSRP_RUNSPACE_OPENED));
    ASSERT_FALSE(psrp_runspace_pool_state_is_terminal(PSRP_RUNSPACE_OPENING));

    ASSERT_TRUE(psrp_invocation_state_is_terminal(PSRP_INVOCATION_COMPLETED));
    ASSERT_TRUE(psrp_invocation_state_is_terminal(PSRP_INVOCATION_FAILED));
    ASSERT_TRUE(psrp_invocation_state_is_terminal(PSRP_INVOCATION_STOPPED));
    ASSERT_FALSE(psrp_invocation_state_is_terminal(PSRP_INVOCATION_RUNNING));
}

/* ------------------------------------------------ SESSION_CAPABILITY ----- */

PSRP_TEST(session_capability_build_matches_spec_shape)
{
    psrp_session_capability_t cap;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_session_capability_defaults(&cap);
    ASSERT_OK(psrp_build_session_capability(&cap, &xml));
    /* Property order and element types follow the 2.2.2.1 example. */
    ASSERT_EQ_MEM(xml.data, xml.len,
        "<Obj RefId=\"0\"><MS>"
        "<Version N=\"protocolversion\">2.2</Version>"
        "<Version N=\"PSVersion\">2.0</Version>"
        "<Version N=\"SerializationVersion\">1.1.0.1</Version>"
        "</MS></Obj>",
        strlen("<Obj RefId=\"0\"><MS>"
               "<Version N=\"protocolversion\">2.2</Version>"
               "<Version N=\"PSVersion\">2.0</Version>"
               "<Version N=\"SerializationVersion\">1.1.0.1</Version>"
               "</MS></Obj>"));
    psrp_buffer_free(&xml);
}

PSRP_TEST(session_capability_roundtrip)
{
    psrp_session_capability_t cap, back;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_session_capability_defaults(&cap);
    ASSERT_OK(psrp_build_session_capability(&cap, &xml));
    ASSERT_OK(psrp_parse_session_capability(xml.data, xml.len, &back));
    ASSERT_EQ_STR(back.protocol_version, "2.2");
    ASSERT_EQ_STR(back.ps_version, "2.0");
    ASSERT_EQ_STR(back.serialization_version, "1.1.0.1");
    psrp_buffer_free(&xml);
}

/* The spec's own example payload, including the optional TimeZone we ignore. */
PSRP_TEST(session_capability_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<Version N=\"protocolversion\">2.2</Version>"
        "<Version N=\"PSVersion\">2.0</Version>"
        "<Version N=\"SerializationVersion\">1.1.0.1</Version>"
        "<BA N=\"TimeZone\">AAEAAAD/////</BA>"
        "</MS></Obj>";
    psrp_session_capability_t cap;
    ASSERT_OK(psrp_parse_session_capability(xml, sizeof xml - 1, &cap));
    ASSERT_EQ_STR(cap.protocol_version, "2.2");
    ASSERT_EQ_STR(cap.serialization_version, "1.1.0.1");
}

PSRP_TEST(session_capability_requires_protocol_version)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS><Version N=\"PSVersion\">2.0</Version></MS></Obj>";
    psrp_session_capability_t cap;
    ASSERT_ERR(psrp_parse_session_capability(xml, sizeof xml - 1, &cap),
               PSRP_ERR_MALFORMED);
}

PSRP_TEST(session_capability_rejects_non_object)
{
    psrp_session_capability_t cap;
    ASSERT_ERR(psrp_parse_session_capability("<S>hi</S>", 9, &cap),
               PSRP_ERR_MALFORMED);
}

/* ----------------------------------------------- RUNSPACEPOOL_STATE ------ */

/* The example from 2.2.2.9. */
PSRP_TEST(runspacepool_state_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"1\"><MS><I32 N=\"RunspaceState\">2</I32></MS></Obj>";
    psrp_runspacepool_state_msg_t m;
    ASSERT_OK(psrp_parse_runspacepool_state(xml, sizeof xml - 1, &m));
    ASSERT_EQ_I(m.state, PSRP_RUNSPACE_OPENED);
    ASSERT_FALSE(m.has_error);
    ASSERT_NULL(m.error_text);
    psrp_runspacepool_state_msg_free(&m);
}

PSRP_TEST(runspacepool_state_captures_error_record)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"RunspaceState\">5</I32>"
        "<Obj N=\"ExceptionAsErrorRecord\" RefId=\"1\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.ErrorRecord</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<ToString>Something went wrong.</ToString>"
        "</Obj>"
        "</MS></Obj>";
    psrp_runspacepool_state_msg_t m;
    ASSERT_OK(psrp_parse_runspacepool_state(xml, sizeof xml - 1, &m));
    ASSERT_EQ_I(m.state, PSRP_RUNSPACE_BROKEN);
    ASSERT_TRUE(m.has_error);
    ASSERT_EQ_STR(m.error_text, "Something went wrong.");
    ASSERT_TRUE(psrp_runspace_pool_state_is_terminal(m.state));
    psrp_runspacepool_state_msg_free(&m);
    /* free must be idempotent */
    psrp_runspacepool_state_msg_free(&m);
}

PSRP_TEST(runspacepool_state_rejects_missing_property)
{
    static const char xml[] = "<Obj RefId=\"0\"><MS></MS></Obj>";
    psrp_runspacepool_state_msg_t m;
    ASSERT_ERR(psrp_parse_runspacepool_state(xml, sizeof xml - 1, &m),
               PSRP_ERR_MALFORMED);
}

/* ------------------------------------------------- PIPELINE_STATE -------- */

/* The example from 2.2.2.21, error record trimmed to its ToString. */
PSRP_TEST(pipeline_state_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"PipelineState\">3</I32>"
        "<Obj N=\"ExceptionAsErrorRecord\" RefId=\"1\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.ErrorRecord</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<ToString>The pipeline has been stopped.</ToString>"
        "</Obj>"
        "</MS></Obj>";
    psrp_pipeline_state_msg_t m;
    ASSERT_OK(psrp_parse_pipeline_state(xml, sizeof xml - 1, &m));
    ASSERT_EQ_I(m.state, PSRP_INVOCATION_STOPPED);
    ASSERT_TRUE(m.has_error);
    ASSERT_EQ_STR(m.error_text, "The pipeline has been stopped.");
    psrp_pipeline_state_msg_free(&m);
}

PSRP_TEST(pipeline_state_completed_has_no_error)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS><I32 N=\"PipelineState\">4</I32></MS></Obj>";
    psrp_pipeline_state_msg_t m;
    ASSERT_OK(psrp_parse_pipeline_state(xml, sizeof xml - 1, &m));
    ASSERT_EQ_I(m.state, PSRP_INVOCATION_COMPLETED);
    ASSERT_TRUE(psrp_invocation_state_is_terminal(m.state));
    ASSERT_FALSE(m.has_error);
    psrp_pipeline_state_msg_free(&m);
}

PSRP_TEST(state_parsers_reject_bad_input)
{
    psrp_pipeline_state_msg_t p;
    psrp_runspacepool_state_msg_t r;
    static const char wrong_type[] =
        "<Obj><MS><S N=\"PipelineState\">4</S></MS></Obj>";
    /* Wrong property type. */
    ASSERT_ERR(psrp_parse_pipeline_state(wrong_type, strlen(wrong_type), &p),
               PSRP_ERR_MALFORMED);
    /* Not CLIXML at all. */
    ASSERT_ERR(psrp_parse_runspacepool_state("garbage", 7, &r), PSRP_ERR_XML);
    ASSERT_ERR(psrp_parse_pipeline_state(NULL, 0, NULL), PSRP_ERR_INVALID_ARG);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(state_enum_values_match_spec),
    PSRP_TEST_CASE(state_names),
    PSRP_TEST_CASE(terminal_states),
    PSRP_TEST_CASE(session_capability_build_matches_spec_shape),
    PSRP_TEST_CASE(session_capability_roundtrip),
    PSRP_TEST_CASE(session_capability_parses_spec_example),
    PSRP_TEST_CASE(session_capability_requires_protocol_version),
    PSRP_TEST_CASE(session_capability_rejects_non_object),
    PSRP_TEST_CASE(runspacepool_state_parses_spec_example),
    PSRP_TEST_CASE(runspacepool_state_captures_error_record),
    PSRP_TEST_CASE(runspacepool_state_rejects_missing_property),
    PSRP_TEST_CASE(pipeline_state_parses_spec_example),
    PSRP_TEST_CASE(pipeline_state_completed_has_no_error),
    PSRP_TEST_CASE(state_parsers_reject_bad_input),
};

PSRP_TEST_MAIN(cases)
