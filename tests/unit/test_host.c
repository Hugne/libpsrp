#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* ------------------------------------------------------ method table ---- */

/* Identifiers transcribed independently from the 2.2.3.17 tables. */
PSRP_TEST(host_method_ids_match_spec)
{
    ASSERT_EQ_I(PSRP_HOST_GET_NAME, 1);
    ASSERT_EQ_I(PSRP_HOST_GET_CURRENT_UI_CULTURE, 5);
    ASSERT_EQ_I(PSRP_HOST_SET_SHOULD_EXIT, 6);
    ASSERT_EQ_I(PSRP_HOST_READ_LINE, 11);
    ASSERT_EQ_I(PSRP_HOST_READ_LINE_AS_SECURE_STRING, 12);
    ASSERT_EQ_I(PSRP_HOST_WRITE1, 13);
    ASSERT_EQ_I(PSRP_HOST_WRITE_WARNING_LINE, 22);
    ASSERT_EQ_I(PSRP_HOST_PROMPT, 23);
    ASSERT_EQ_I(PSRP_HOST_PROMPT_FOR_CREDENTIAL1, 24);
    ASSERT_EQ_I(PSRP_HOST_PROMPT_FOR_CHOICE, 26);
    ASSERT_EQ_I(PSRP_HOST_GET_FOREGROUND_COLOR, 27);
    ASSERT_EQ_I(PSRP_HOST_GET_BACKGROUND_COLOR, 29);
    ASSERT_EQ_I(PSRP_HOST_GET_CURSOR_SIZE, 35);
    ASSERT_EQ_I(PSRP_HOST_GET_BUFFER_CONTENTS, 50);
    ASSERT_EQ_I(PSRP_HOST_GET_RUNSPACE, 55);
    ASSERT_EQ_I(PSRP_HOST_PROMPT_FOR_CHOICE_MULTIPLE_SELECTION, 56);
}

PSRP_TEST(host_method_names)
{
    ASSERT_EQ_STR(psrp_host_method_name(11), "ReadLine");
    ASSERT_EQ_STR(psrp_host_method_name(20), "WriteProgress");
    ASSERT_EQ_STR(psrp_host_method_name(56),
                  "PromptForChoiceMultipleSelection");
    ASSERT_EQ_STR(psrp_host_method_name(0), "Unknown");
    ASSERT_EQ_STR(psrp_host_method_name(999), "Unknown");
}

/* The response rule cuts both ways: methods with a return value MUST be
 * answered, methods without MUST NOT be. */
PSRP_TEST(host_methods_that_require_a_response)
{
    /* Read-only properties and readers all return something. */
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_GET_NAME));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_GET_VERSION));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_GET_INSTANCE_ID));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_READ_LINE));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_READ_KEY));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_PROMPT));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_GET_BUFFER_SIZE));
    ASSERT_TRUE(psrp_host_method_returns_value(PSRP_HOST_GET_KEY_AVAILABLE));

    /* The write and setter families return nothing. */
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_WRITE1));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_WRITE_LINE1));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_WRITE_ERROR_LINE));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_WRITE_PROGRESS));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_SET_SHOULD_EXIT));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_SET_BUFFER_SIZE));
    ASSERT_FALSE(psrp_host_method_returns_value(PSRP_HOST_FLUSH_INPUT_BUFFER));

    /* An unknown method is treated as expecting nothing: inventing a reply to
     * a call we do not understand would be worse than staying quiet. */
    ASSERT_FALSE(psrp_host_method_returns_value(999));
}

/* Every id in the table must have a name, and vice versa. */
PSRP_TEST(host_method_table_is_complete_and_consistent)
{
    int id;
    for (id = 1; id <= 56; id++) {
        const char *name = psrp_host_method_name(id);
        if (strcmp(name, "Unknown") == 0)
            PSRP_FAIL("method id %d has no name", id);
    }
    ASSERT_EQ_STR(psrp_host_method_name(57), "Unknown");
}

/* ------------------------------------------------------------- parse ---- */

/* The 2.2.2.15 example: a ReadLine call. */
PSRP_TEST(host_call_parses_spec_example)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"ci\">1</I64>"
        "<Obj N=\"mi\" RefId=\"1\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.Remoting.RemoteHostMethodId</T>"
            "<T>System.Enum</T><T>System.ValueType</T><T>System.Object</T>"
          "</TN>"
          "<ToString>ReadLine</ToString>"
          "<I32>11</I32>"
        "</Obj>"
        "</MS></Obj>";
    psrp_host_call_t call;
    ASSERT_OK(psrp_parse_host_call(xml, sizeof xml - 1, &call));
    ASSERT_TRUE(call.call_id == 1);
    ASSERT_EQ_I(call.method_id, PSRP_HOST_READ_LINE);
    ASSERT_EQ_STR(psrp_host_method_name(call.method_id), "ReadLine");
    ASSERT_TRUE(psrp_host_method_returns_value(call.method_id));
    psrp_host_call_free(&call);
    psrp_host_call_free(&call);      /* idempotent */
}

/* A server that sends mi as a bare I32 rather than an enum object. */
PSRP_TEST(host_call_accepts_bare_int_method_id)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"ci\">7</I64><I32 N=\"mi\">13</I32>"
        "</MS></Obj>";
    psrp_host_call_t call;
    ASSERT_OK(psrp_parse_host_call(xml, sizeof xml - 1, &call));
    ASSERT_TRUE(call.call_id == 7);
    ASSERT_EQ_I(call.method_id, PSRP_HOST_WRITE1);
    psrp_host_call_free(&call);
}

PSRP_TEST(host_call_captures_parameters)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"ci\">2</I64><I32 N=\"mi\">15</I32>"
        "<Obj N=\"mp\" RefId=\"1\"><LST><S>hello</S></LST></Obj>"
        "</MS></Obj>";
    psrp_host_call_t call;
    ASSERT_OK(psrp_parse_host_call(xml, sizeof xml - 1, &call));
    ASSERT_EQ_I(call.parameters.kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_item_count(call.parameters.as.obj), 1u);
    ASSERT_EQ_STR(psrp_object_item(call.parameters.as.obj, 0)->as.text.ptr,
                  "hello");
    psrp_host_call_free(&call);
}

PSRP_TEST(host_call_without_parameters_is_null)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"ci\">3</I64><I32 N=\"mi\">11</I32></MS></Obj>";
    psrp_host_call_t call;
    ASSERT_OK(psrp_parse_host_call(xml, sizeof xml - 1, &call));
    ASSERT_EQ_I(call.parameters.kind, PSRP_VAL_NULL);
    psrp_host_call_free(&call);
}

PSRP_TEST(host_call_rejects_bad_input)
{
    psrp_host_call_t call;
    /* Missing ci. */
    ASSERT_ERR(psrp_parse_host_call(
        "<Obj><MS><I32 N=\"mi\">11</I32></MS></Obj>", 40, &call),
        PSRP_ERR_MALFORMED);
    /* Missing mi. */
    ASSERT_ERR(psrp_parse_host_call(
        "<Obj><MS><I64 N=\"ci\">1</I64></MS></Obj>", 39, &call),
        PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_host_call("<S>x</S>", 8, &call), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_host_call("junk", 4, &call), PSRP_ERR_XML);
    ASSERT_ERR(psrp_parse_host_call("<Obj/>", 6, NULL), PSRP_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------- build ---- */

PSRP_TEST(host_response_carries_value_call_id_and_method)
{
    psrp_buffer_t xml;
    psrp_value_t v, root;
    const psrp_value_t *mr, *ci, *mi;

    psrp_buffer_init(&xml);
    psrp_value_init(&v);
    psrp_value_init(&root);
    ASSERT_OK(psrp_value_set_string(&v, "Line read from the host"));
    ASSERT_OK(psrp_build_host_response(1, PSRP_HOST_READ_LINE, &v, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    mr = psrp_object_find(root.as.obj, "mr");
    ASSERT_NOT_NULL(mr);
    ASSERT_EQ_STR(mr->as.text.ptr, "Line read from the host");

    ci = psrp_object_find(root.as.obj, "ci");
    ASSERT_NOT_NULL(ci);
    ASSERT_EQ_I(ci->kind, PSRP_VAL_INT64);
    ASSERT_TRUE(ci->as.i64 == 1);

    /* mi round-trips as the enum object, with ToString tracking the id. */
    mi = psrp_object_find(root.as.obj, "mi");
    ASSERT_NOT_NULL(mi);
    ASSERT_EQ_I(mi->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_I(psrp_object_primitive(mi->as.obj)->as.i32, PSRP_HOST_READ_LINE);
    {
        size_t len = 0;
        ASSERT_EQ_MEM(psrp_object_to_string(mi->as.obj, &len), len,
                      "ReadLine", 8u);
    }

    psrp_value_free(&v);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(host_response_with_null_value)
{
    psrp_buffer_t xml;
    psrp_value_t root;
    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_host_response(4, PSRP_HOST_GET_NAME, NULL, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    ASSERT_EQ_I(psrp_object_find(root.as.obj, "mr")->kind, PSRP_VAL_NULL);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

/* An unsupported method is answered with `me`, not with silence. */
PSRP_TEST(host_response_error_uses_me_property)
{
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *me;
    size_t len = 0;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_host_response_error(9, PSRP_HOST_READ_KEY,
                                             "no console here", &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    me = psrp_object_find(root.as.obj, "me");
    ASSERT_NOT_NULL(me);
    ASSERT_EQ_I(me->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_MEM(psrp_object_to_string(me->as.obj, &len), len,
                  "no console here", 15u);
    /* 2.2.2.16 says this SHOULD be RemoteHostExecutionException. */
    ASSERT_EQ_STR(psrp_object_find(me->as.obj, "FullyQualifiedErrorId")
                      ->as.text.ptr, "RemoteHostExecutionException");
    /* No mr when reporting an error. */
    ASSERT_NULL(psrp_object_find(root.as.obj, "mr"));

    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(host_response_error_defaults_its_message)
{
    psrp_buffer_t xml;
    psrp_value_t root;
    size_t len = 0;
    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_host_response_error(1, PSRP_HOST_READ_LINE, NULL, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    ASSERT_TRUE(psrp_object_to_string(
        psrp_object_find(root.as.obj, "me")->as.obj, &len) != NULL);
    ASSERT_TRUE(len > 0);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(host_builders_reject_null_output)
{
    ASSERT_ERR(psrp_build_host_response(1, 11, NULL, NULL),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_host_response_error(1, 11, "x", NULL),
               PSRP_ERR_INVALID_ARG);
}

/* --------------------------------------------- session integration ------ */

/* Responses go on the priority queue, because 3.1.5.3.5 puts them on the
 * WSMan "pr" stream rather than "stdin". */
PSRP_TEST(session_queues_host_response_on_priority_stream)
{
    psrp_session_t *s = psrp_session_new();
    psrp_value_t v;
    psrp_buffer_t normal, priority;
    psrp_guid_t pid;

    ASSERT_NOT_NULL(s);
    psrp_value_init(&v);
    psrp_buffer_init(&normal);
    psrp_buffer_init(&priority);
    ASSERT_OK(psrp_guid_generate(&pid));

    ASSERT_OK(psrp_value_set_string(&v, "typed line"));
    ASSERT_OK(psrp_session_respond_to_host_call(s, &pid, 1,
                                                PSRP_HOST_READ_LINE, &v, NULL));

    /* Nothing on the normal queue; the response is on the priority one. */
    ASSERT_ERR(psrp_session_take_output(s, &normal), PSRP_ERR_NOT_FOUND);
    ASSERT_OK(psrp_session_take_priority_output(s, &priority));
    ASSERT_TRUE(priority.len > 0);
    ASSERT_ERR(psrp_session_take_priority_output(s, &priority),
               PSRP_ERR_NOT_FOUND);

    psrp_value_free(&v);
    psrp_buffer_free(&normal);
    psrp_buffer_free(&priority);
    psrp_session_free(s);
}

/* A pipeline-scoped call gets PIPELINE_HOST_RESPONSE; a pool-scoped one gets
 * RUNSPACEPOOL_HOST_RESPONSE. */
PSRP_TEST(session_picks_the_right_response_message_type)
{
    psrp_session_t *s = psrp_session_new();
    psrp_value_t v;
    psrp_buffer_t wire;
    psrp_defrag_t *d;
    psrp_buffer_t msg;
    psrp_message_t m;
    psrp_guid_t pid;
    uint64_t oid = 0;

    ASSERT_NOT_NULL(s);
    psrp_value_init(&v);
    ASSERT_OK(psrp_guid_generate(&pid));
    ASSERT_OK(psrp_value_set_string(&v, "x"));

    /* Pipeline-scoped. */
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_respond_to_host_call(s, &pid, 1,
                                                PSRP_HOST_READ_LINE, &v, NULL));
    ASSERT_OK(psrp_session_take_priority_output(s, &wire));
    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    psrp_buffer_init(&msg);
    ASSERT_OK(psrp_defrag_push(d, wire.data, wire.len));
    ASSERT_OK(psrp_defrag_next(d, &oid, &msg));
    ASSERT_OK(psrp_message_decode(msg.data, msg.len, &m));
    ASSERT_EQ_SZ(m.type, PSRP_MSG_PIPELINE_HOST_RESPONSE);
    ASSERT_TRUE(psrp_guid_equal(&m.pid, &pid));
    psrp_defrag_free(d);
    psrp_buffer_free(&msg);
    psrp_buffer_free(&wire);

    /* Pool-scoped. */
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_respond_to_host_call(s, NULL, 2,
                                                PSRP_HOST_GET_NAME, &v, NULL));
    ASSERT_OK(psrp_session_take_priority_output(s, &wire));
    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    psrp_buffer_init(&msg);
    ASSERT_OK(psrp_defrag_push(d, wire.data, wire.len));
    ASSERT_OK(psrp_defrag_next(d, &oid, &msg));
    ASSERT_OK(psrp_message_decode(msg.data, msg.len, &m));
    ASSERT_EQ_SZ(m.type, PSRP_MSG_RUNSPACEPOOL_HOST_RESPONSE);
    ASSERT_TRUE(psrp_guid_is_empty(&m.pid));
    psrp_defrag_free(d);
    psrp_buffer_free(&msg);
    psrp_buffer_free(&wire);

    psrp_value_free(&v);
    psrp_session_free(s);
}

/* Responding to a method that returns nothing is a protocol violation, so the
 * session refuses rather than confusing the server. */
PSRP_TEST(session_refuses_to_answer_a_void_method)
{
    psrp_session_t *s = psrp_session_new();
    psrp_value_t v;
    psrp_buffer_t priority;

    ASSERT_NOT_NULL(s);
    psrp_value_init(&v);
    psrp_buffer_init(&priority);
    ASSERT_OK(psrp_value_set_string(&v, "x"));

    ASSERT_ERR(psrp_session_respond_to_host_call(s, NULL, 1, PSRP_HOST_WRITE1,
                                                 &v, NULL), PSRP_ERR_STATE);
    ASSERT_ERR(psrp_session_respond_to_host_call(s, NULL, 1,
                                                 PSRP_HOST_WRITE_LINE1, &v, NULL),
               PSRP_ERR_STATE);
    /* Nothing was queued. */
    ASSERT_ERR(psrp_session_take_priority_output(s, &priority),
               PSRP_ERR_NOT_FOUND);

    psrp_value_free(&v);
    psrp_buffer_free(&priority);
    psrp_session_free(s);
}

/* A host call arriving from the server surfaces as an event the caller can
 * parse and answer. */
PSRP_TEST(host_call_event_round_trip)
{
    psrp_session_t *s = psrp_session_new();
    psrp_message_t m;
    psrp_buffer_t body, wire, xml;
    psrp_event_t e;
    psrp_host_call_t call;
    psrp_guid_t pid;
    static const char call_xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"ci\">5</I64><I32 N=\"mi\">11</I32></MS></Obj>";

    ASSERT_NOT_NULL(s);
    ASSERT_OK(psrp_guid_generate(&pid));

    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = PSRP_MSG_PIPELINE_HOST_CALL;
    m.rpid = *psrp_session_pool_id(s);
    m.pid = pid;
    m.data = (const uint8_t *)call_xml;
    m.data_len = sizeof call_xml - 1;

    psrp_buffer_init(&body);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_message_encode(&body, &m));
    ASSERT_OK(psrp_fragment_split(&wire, 1, body.data, body.len, 0));
    ASSERT_OK(psrp_session_receive(s, wire.data, wire.len));

    ASSERT_OK(psrp_session_next_event(s, &e));
    ASSERT_EQ_I(e.kind, PSRP_EVENT_HOST_CALL);
    ASSERT_EQ_SZ(e.message_type, PSRP_MSG_PIPELINE_HOST_CALL);
    ASSERT_TRUE(psrp_guid_equal(&e.pipeline_id, &pid));

    /* The event carries the raw object; parse it into a typed call. */
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_clixml_serialize(&e.value, &xml));
    ASSERT_OK(psrp_parse_host_call(xml.data, xml.len, &call));
    ASSERT_TRUE(call.call_id == 5);
    ASSERT_EQ_I(call.method_id, PSRP_HOST_READ_LINE);

    psrp_host_call_free(&call);
    psrp_event_free(&e);
    psrp_buffer_free(&xml);
    psrp_buffer_free(&body);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(host_method_ids_match_spec),
    PSRP_TEST_CASE(host_method_names),
    PSRP_TEST_CASE(host_methods_that_require_a_response),
    PSRP_TEST_CASE(host_method_table_is_complete_and_consistent),
    PSRP_TEST_CASE(host_call_parses_spec_example),
    PSRP_TEST_CASE(host_call_accepts_bare_int_method_id),
    PSRP_TEST_CASE(host_call_captures_parameters),
    PSRP_TEST_CASE(host_call_without_parameters_is_null),
    PSRP_TEST_CASE(host_call_rejects_bad_input),
    PSRP_TEST_CASE(host_response_carries_value_call_id_and_method),
    PSRP_TEST_CASE(host_response_with_null_value),
    PSRP_TEST_CASE(host_response_error_uses_me_property),
    PSRP_TEST_CASE(host_response_error_defaults_its_message),
    PSRP_TEST_CASE(host_builders_reject_null_output),
    PSRP_TEST_CASE(session_queues_host_response_on_priority_stream),
    PSRP_TEST_CASE(session_picks_the_right_response_message_type),
    PSRP_TEST_CASE(session_refuses_to_answer_a_void_method),
    PSRP_TEST_CASE(host_call_event_round_trip),
};

PSRP_TEST_MAIN(cases)
