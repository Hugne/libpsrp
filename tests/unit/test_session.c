#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_session.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* A scripted in-memory server. Because the session does no I/O, an entire
 * conversation can be driven here with no transport, no WinRM, and no
 * timing. */

typedef struct { psrp_buffer_t body; } msg_capture_t;

/* Decodes every PSRP message out of a fragmented client payload. */
static size_t decode_all(const psrp_buffer_t *wire, psrp_message_t *out,
                         psrp_buffer_t *bodies, size_t max)
{
    psrp_defrag_t *d = psrp_defrag_new();
    size_t count = 0;
    ASSERT_NOT_NULL(d);
    ASSERT_OK(psrp_defrag_push(d, wire->data, wire->len));
    for (;;) {
        uint64_t oid = 0;
        psrp_buffer_init(&bodies[count]);
        if (psrp_defrag_next(d, &oid, &bodies[count]) != PSRP_OK) {
            psrp_buffer_free(&bodies[count]);
            break;
        }
        ASSERT_OK(psrp_message_decode(bodies[count].data, bodies[count].len,
                                      &out[count]));
        count++;
        if (count == max) break;
    }
    psrp_defrag_free(d);
    return count;
}

static void free_bodies(psrp_buffer_t *bodies, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) psrp_buffer_free(&bodies[i]);
}

/* Frames a server->client message and feeds it to the session, optionally in
 * small chunks to prove reassembly does not depend on read boundaries. */
static void server_send(psrp_session_t *s, uint32_t type,
                        const psrp_guid_t *pid, const char *xml,
                        size_t chunk)
{
    psrp_message_t m;
    psrp_buffer_t body, wire;
    size_t i;

    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_CLIENT;
    m.type = type;
    m.rpid = *psrp_session_pool_id(s);
    m.pid = pid ? *pid : psrp_guid_empty;
    m.data = (const uint8_t *)xml;
    m.data_len = xml ? strlen(xml) : 0;

    psrp_buffer_init(&body);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_message_encode(&body, &m));
    ASSERT_OK(psrp_fragment_split(&wire, 1000 + type, body.data, body.len, 64));

    if (chunk == 0) chunk = wire.len ? wire.len : 1;
    for (i = 0; i < wire.len; i += chunk) {
        size_t n = wire.len - i;
        if (n > chunk) n = chunk;
        ASSERT_OK(psrp_session_receive(s, wire.data + i, n));
    }
    psrp_buffer_free(&body);
    psrp_buffer_free(&wire);
}

static const char *kCapabilityXml =
    "<Obj RefId=\"0\"><MS>"
    "<Version N=\"protocolversion\">2.3</Version>"
    "<Version N=\"PSVersion\">5.1</Version>"
    "<Version N=\"SerializationVersion\">1.1.0.1</Version>"
    "</MS></Obj>";

static const char *pool_state_xml(int state)
{
    static char buf[128];
    snprintf(buf, sizeof buf,
             "<Obj RefId=\"0\"><MS><I32 N=\"RunspaceState\">%d</I32></MS></Obj>",
             state);
    return buf;
}

static const char *pipeline_state_xml(int state)
{
    static char buf[128];
    snprintf(buf, sizeof buf,
             "<Obj RefId=\"0\"><MS><I32 N=\"PipelineState\">%d</I32></MS></Obj>",
             state);
    return buf;
}

/* Pops the next event, asserting it exists and has the expected kind. */
static void expect_event(psrp_session_t *s, psrp_event_kind_t kind,
                         psrp_event_t *out)
{
    psrp_result_t rc = psrp_session_next_event(s, out);
    if (rc != PSRP_OK)
        PSRP_FAIL("expected an event of kind %d, queue was empty", (int)kind);
    if (out->kind != kind) {
        psrp_event_kind_t got = out->kind;
        psrp_event_free(out);
        PSRP_FAIL("expected event kind %d, got %d", (int)kind, (int)got);
    }
}

/* ------------------------------------------------------------ basics ---- */

PSRP_TEST(session_starts_before_open_with_a_pool_id)
{
    psrp_session_t *s = psrp_session_new();
    const psrp_guid_t *id;
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_I(psrp_session_pool_state(s), PSRP_RUNSPACE_BEFORE_OPEN);
    id = psrp_session_pool_id(s);
    ASSERT_NOT_NULL(id);
    /* A pool id must be a real random GUID, not zero. */
    ASSERT_FALSE(psrp_guid_is_empty(id));
    ASSERT_NULL(psrp_session_server_capability(s));
    psrp_session_free(s);
    psrp_session_free(NULL);       /* must be safe */
}

PSRP_TEST(session_pool_ids_are_unique)
{
    psrp_session_t *a = psrp_session_new();
    psrp_session_t *b = psrp_session_new();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_FALSE(psrp_guid_equal(psrp_session_pool_id(a),
                                 psrp_session_pool_id(b)));
    psrp_session_free(a);
    psrp_session_free(b);
}

/* --------------------------------------------------------- open flow ---- */

PSRP_TEST(open_payload_carries_capability_then_init)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t wire;
    psrp_message_t msgs[4];
    psrp_buffer_t bodies[4];
    size_t n;

    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_open_payload(s, &wire));
    ASSERT_TRUE(wire.len > 0);
    /* Sending the negotiation moves the pool out of BeforeOpen. */
    ASSERT_EQ_I(psrp_session_pool_state(s), PSRP_RUNSPACE_NEGOTIATION_SENT);

    n = decode_all(&wire, msgs, bodies, 4);
    ASSERT_EQ_SZ(n, 2u);
    ASSERT_EQ_SZ(msgs[0].type, PSRP_MSG_SESSION_CAPABILITY);
    ASSERT_EQ_SZ(msgs[1].type, PSRP_MSG_INIT_RUNSPACEPOOL);
    /* Both are pool-level: addressed to the server, with no pipeline id. */
    ASSERT_EQ_SZ(msgs[0].destination, PSRP_DEST_SERVER);
    ASSERT_TRUE(psrp_guid_equal(&msgs[0].rpid, psrp_session_pool_id(s)));
    ASSERT_TRUE(psrp_guid_is_empty(&msgs[0].pid));
    ASSERT_TRUE(psrp_guid_equal(&msgs[1].rpid, psrp_session_pool_id(s)));

    free_bodies(bodies, n);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(open_payload_is_rejected_twice)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t wire;
    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_open_payload(s, &wire));
    ASSERT_ERR(psrp_session_open_payload(s, &wire), PSRP_ERR_STATE);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(configure_only_before_open)
{
    psrp_session_t *s = psrp_session_new();
    psrp_init_runspacepool_t init;
    psrp_buffer_t wire;
    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    psrp_init_runspacepool_defaults(&init);
    init.max_runspaces = 5;
    ASSERT_OK(psrp_session_configure(s, &init));
    ASSERT_OK(psrp_session_open_payload(s, &wire));
    /* Reconfiguring after the negotiation has been sent is a state error. */
    ASSERT_ERR(psrp_session_configure(s, &init), PSRP_ERR_STATE);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(capability_and_pool_state_update_the_session)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t wire;
    psrp_event_t e;

    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_session_open_payload(s, &wire));

    server_send(s, PSRP_MSG_SESSION_CAPABILITY, NULL, kCapabilityXml, 0);
    expect_event(s, PSRP_EVENT_SESSION_CAPABILITY, &e);
    ASSERT_EQ_STR(e.text, "2.3");
    psrp_event_free(&e);
    ASSERT_NOT_NULL(psrp_session_server_capability(s));
    ASSERT_EQ_STR(psrp_session_server_capability(s)->protocol_version, "2.3");
    ASSERT_EQ_STR(psrp_session_server_capability(s)->ps_version, "5.1");

    server_send(s, PSRP_MSG_RUNSPACEPOOL_STATE, NULL,
                pool_state_xml(PSRP_RUNSPACE_OPENED), 0);
    expect_event(s, PSRP_EVENT_POOL_STATE, &e);
    ASSERT_EQ_I(e.state, PSRP_RUNSPACE_OPENED);
    psrp_event_free(&e);
    ASSERT_EQ_I(psrp_session_pool_state(s), PSRP_RUNSPACE_OPENED);

    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

/* Transport reads arrive in arbitrary sizes; the session must not care. */
PSRP_TEST(receive_tolerates_any_chunking)
{
    size_t chunk;
    for (chunk = 1; chunk <= 24; chunk++) {
        psrp_session_t *s = psrp_session_new();
        psrp_event_t e;
        ASSERT_NOT_NULL(s);
        server_send(s, PSRP_MSG_RUNSPACEPOOL_STATE, NULL,
                    pool_state_xml(PSRP_RUNSPACE_OPENED), chunk);
        expect_event(s, PSRP_EVENT_POOL_STATE, &e);
        ASSERT_EQ_I(e.state, PSRP_RUNSPACE_OPENED);
        psrp_event_free(&e);
        psrp_session_free(s);
    }
}

/* ------------------------------------------------------- pipelines ------ */

PSRP_TEST(pipeline_payload_addresses_a_new_pipeline)
{
    psrp_session_t *s = psrp_session_new();
    psrp_command_t *cmd;
    psrp_buffer_t wire;
    psrp_guid_t pid, pid2;
    psrp_message_t msgs[2];
    psrp_buffer_t bodies[2];
    size_t n;

    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    cmd = psrp_command_new("$env:COMPUTERNAME", true);
    ASSERT_NOT_NULL(cmd);

    ASSERT_OK(psrp_session_pipeline_payload(s, &cmd, 1, &pid, &wire));
    ASSERT_FALSE(psrp_guid_is_empty(&pid));

    n = decode_all(&wire, msgs, bodies, 2);
    ASSERT_EQ_SZ(n, 1u);
    ASSERT_EQ_SZ(msgs[0].type, PSRP_MSG_CREATE_PIPELINE);
    ASSERT_TRUE(psrp_guid_equal(&msgs[0].rpid, psrp_session_pool_id(s)));
    ASSERT_TRUE(psrp_guid_equal(&msgs[0].pid, &pid));
    free_bodies(bodies, n);

    /* Each pipeline gets its own id. */
    psrp_buffer_reset(&wire);
    ASSERT_OK(psrp_session_pipeline_payload(s, &cmd, 1, &pid2, &wire));
    ASSERT_FALSE(psrp_guid_equal(&pid, &pid2));

    psrp_command_free(cmd);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(pipeline_payload_requires_commands)
{
    psrp_session_t *s = psrp_session_new();
    psrp_buffer_t wire;
    psrp_guid_t pid;
    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&wire);
    ASSERT_ERR(psrp_session_pipeline_payload(s, NULL, 0, &pid, &wire),
               PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

PSRP_TEST(pipeline_input_and_end_of_input)
{
    psrp_session_t *s = psrp_session_new();
    psrp_value_t v;
    psrp_buffer_t wire;
    psrp_guid_t pid;
    psrp_message_t msgs[4];
    psrp_buffer_t bodies[4];
    size_t n;

    ASSERT_NOT_NULL(s);
    psrp_value_init(&v);
    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_guid_generate(&pid));

    /* Nothing queued yet. */
    ASSERT_ERR(psrp_session_take_output(s, &wire), PSRP_ERR_NOT_FOUND);

    ASSERT_OK(psrp_value_set_string(&v, "one"));
    ASSERT_OK(psrp_session_send_input(s, &pid, &v));
    ASSERT_OK(psrp_session_end_input(s, &pid));
    ASSERT_OK(psrp_session_take_output(s, &wire));
    /* Draining empties the queue. */
    ASSERT_ERR(psrp_session_take_output(s, &wire), PSRP_ERR_NOT_FOUND);

    n = decode_all(&wire, msgs, bodies, 4);
    ASSERT_EQ_SZ(n, 2u);
    ASSERT_EQ_SZ(msgs[0].type, PSRP_MSG_PIPELINE_INPUT);
    ASSERT_TRUE(psrp_guid_equal(&msgs[0].pid, &pid));
    ASSERT_EQ_SZ(msgs[1].type, PSRP_MSG_END_OF_PIPELINE_INPUT);
    /* 2.2.2.18: END_OF_PIPELINE_INPUT carries no data at all. */
    ASSERT_EQ_SZ(msgs[1].data_len, 0u);

    free_bodies(bodies, n);
    psrp_value_free(&v);
    psrp_buffer_free(&wire);
    psrp_session_free(s);
}

/* --------------------------------------------------------- streams ------ */

PSRP_TEST(pipeline_output_and_state_events)
{
    psrp_session_t *s = psrp_session_new();
    psrp_guid_t pid;
    psrp_event_t e;
    psrp_buffer_t text;

    ASSERT_NOT_NULL(s);
    ASSERT_OK(psrp_guid_generate(&pid));

    server_send(s, PSRP_MSG_PIPELINE_OUTPUT, &pid, "<S>CLAUDE</S>", 0);
    expect_event(s, PSRP_EVENT_PIPELINE_OUTPUT, &e);
    ASSERT_TRUE(psrp_guid_equal(&e.pipeline_id, &pid));
    ASSERT_EQ_I(e.value.kind, PSRP_VAL_STRING);
    psrp_buffer_init(&text);
    ASSERT_OK(psrp_value_to_text(&e.value, &text));
    ASSERT_EQ_MEM(text.data, text.len, "CLAUDE", 6u);
    psrp_buffer_free(&text);
    psrp_event_free(&e);

    server_send(s, PSRP_MSG_PIPELINE_STATE, &pid,
                pipeline_state_xml(PSRP_INVOCATION_COMPLETED), 0);
    expect_event(s, PSRP_EVENT_PIPELINE_STATE, &e);
    ASSERT_EQ_I(e.state, PSRP_INVOCATION_COMPLETED);
    ASSERT_TRUE(psrp_invocation_state_is_terminal(e.state));
    psrp_event_free(&e);

    psrp_session_free(s);
}

PSRP_TEST(record_streams_become_events)
{
    psrp_session_t *s = psrp_session_new();
    psrp_guid_t pid;
    psrp_event_t e;

    ASSERT_NOT_NULL(s);
    ASSERT_OK(psrp_guid_generate(&pid));

    server_send(s, PSRP_MSG_ERROR_RECORD, &pid,
        "<Obj RefId=\"0\"><ToString>it broke</ToString>"
        "<MS><I32 N=\"ErrorCategory_Category\">13</I32></MS></Obj>", 0);
    expect_event(s, PSRP_EVENT_ERROR_RECORD, &e);
    ASSERT_EQ_STR(e.text, "it broke");
    ASSERT_EQ_I(e.state, 13);
    psrp_event_free(&e);

    server_send(s, PSRP_MSG_WARNING_RECORD, &pid,
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">careful</S></MS></Obj>", 0);
    expect_event(s, PSRP_EVENT_WARNING_RECORD, &e);
    ASSERT_EQ_STR(e.text, "careful");
    psrp_event_free(&e);

    server_send(s, PSRP_MSG_VERBOSE_RECORD, &pid,
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">chatty</S></MS></Obj>", 0);
    expect_event(s, PSRP_EVENT_VERBOSE_RECORD, &e);
    ASSERT_EQ_STR(e.text, "chatty");
    psrp_event_free(&e);

    server_send(s, PSRP_MSG_DEBUG_RECORD, &pid,
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"InformationalRecord_Message\">dbg</S></MS></Obj>", 0);
    expect_event(s, PSRP_EVENT_DEBUG_RECORD, &e);
    ASSERT_EQ_STR(e.text, "dbg");
    psrp_event_free(&e);

    server_send(s, PSRP_MSG_PROGRESS_RECORD, &pid,
        "<Obj RefId=\"0\"><MS><S N=\"Activity\">Copying</S>"
        "<I32 N=\"PercentComplete\">40</I32></MS></Obj>", 0);
    expect_event(s, PSRP_EVENT_PROGRESS_RECORD, &e);
    ASSERT_EQ_STR(e.text, "Copying");
    ASSERT_EQ_I(e.state, 40);
    psrp_event_free(&e);

    psrp_session_free(s);
}

/* An unrecognised type must surface, so it can be logged rather than
 * silently vanishing. */
PSRP_TEST(unknown_message_is_surfaced)
{
    psrp_session_t *s = psrp_session_new();
    psrp_event_t e;
    ASSERT_NOT_NULL(s);
    server_send(s, 0x00099999u, NULL, "<S>x</S>", 0);
    expect_event(s, PSRP_EVENT_UNKNOWN_MESSAGE, &e);
    ASSERT_EQ_SZ(e.message_type, 0x00099999u);
    psrp_event_free(&e);
    psrp_session_free(s);
}

PSRP_TEST(events_are_delivered_in_order_then_exhausted)
{
    psrp_session_t *s = psrp_session_new();
    psrp_guid_t pid;
    psrp_event_t e;

    ASSERT_NOT_NULL(s);
    ASSERT_OK(psrp_guid_generate(&pid));
    server_send(s, PSRP_MSG_PIPELINE_OUTPUT, &pid, "<S>first</S>", 0);
    server_send(s, PSRP_MSG_PIPELINE_OUTPUT, &pid, "<S>second</S>", 0);

    expect_event(s, PSRP_EVENT_PIPELINE_OUTPUT, &e);
    ASSERT_EQ_STR(e.value.as.text.ptr, "first");
    psrp_event_free(&e);
    expect_event(s, PSRP_EVENT_PIPELINE_OUTPUT, &e);
    ASSERT_EQ_STR(e.value.as.text.ptr, "second");
    psrp_event_free(&e);

    ASSERT_ERR(psrp_session_next_event(s, &e), PSRP_ERR_NOT_FOUND);
    psrp_session_free(s);
}

/* ------------------------------------------------ the whole conversation - */

/* The scenario the library exists for: open a pool, run a command, collect
 * the output, see the pipeline complete. Entirely in memory. */
PSRP_TEST(full_run_a_command_conversation)
{
    psrp_session_t *s = psrp_session_new();
    psrp_command_t *cmd;
    psrp_buffer_t open_wire, cmd_wire, text;
    psrp_guid_t pid;
    psrp_event_t e;
    bool saw_output = false, completed = false;

    ASSERT_NOT_NULL(s);
    psrp_buffer_init(&open_wire);
    psrp_buffer_init(&cmd_wire);

    /* 1. Client opens the pool. */
    ASSERT_OK(psrp_session_open_payload(s, &open_wire));

    /* 2. Server negotiates and reports the pool open. */
    server_send(s, PSRP_MSG_SESSION_CAPABILITY, NULL, kCapabilityXml, 7);
    server_send(s, PSRP_MSG_RUNSPACEPOOL_STATE, NULL,
                pool_state_xml(PSRP_RUNSPACE_OPENED), 7);
    expect_event(s, PSRP_EVENT_SESSION_CAPABILITY, &e); psrp_event_free(&e);
    expect_event(s, PSRP_EVENT_POOL_STATE, &e); psrp_event_free(&e);
    ASSERT_EQ_I(psrp_session_pool_state(s), PSRP_RUNSPACE_OPENED);

    /* 3. Client runs a command. */
    cmd = psrp_command_new("$env:COMPUTERNAME", true);
    ASSERT_NOT_NULL(cmd);
    ASSERT_OK(psrp_session_pipeline_payload(s, &cmd, 1, &pid, &cmd_wire));

    /* 4. Server streams output then the terminal state. */
    server_send(s, PSRP_MSG_PIPELINE_OUTPUT, &pid, "<S>CLAUDE</S>", 5);
    server_send(s, PSRP_MSG_PIPELINE_STATE, &pid,
                pipeline_state_xml(PSRP_INVOCATION_COMPLETED), 5);

    /* 5. Client drains events until the pipeline is done. */
    while (psrp_session_next_event(s, &e) == PSRP_OK) {
        if (e.kind == PSRP_EVENT_PIPELINE_OUTPUT) {
            ASSERT_TRUE(psrp_guid_equal(&e.pipeline_id, &pid));
            psrp_buffer_init(&text);
            ASSERT_OK(psrp_value_to_text(&e.value, &text));
            ASSERT_EQ_MEM(text.data, text.len, "CLAUDE", 6u);
            psrp_buffer_free(&text);
            saw_output = true;
        } else if (e.kind == PSRP_EVENT_PIPELINE_STATE) {
            if (psrp_invocation_state_is_terminal(e.state)) completed = true;
        }
        psrp_event_free(&e);
    }
    ASSERT_TRUE(saw_output);
    ASSERT_TRUE(completed);

    psrp_command_free(cmd);
    psrp_buffer_free(&open_wire);
    psrp_buffer_free(&cmd_wire);
    psrp_session_free(s);
}

PSRP_TEST(session_rejects_null_args)
{
    psrp_buffer_t b;
    psrp_event_t e;
    psrp_buffer_init(&b);
    ASSERT_ERR(psrp_session_open_payload(NULL, &b), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_session_receive(NULL, "x", 1), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_session_next_event(NULL, &e), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_session_take_output(NULL, &b), PSRP_ERR_INVALID_ARG);
    ASSERT_NULL(psrp_session_pool_id(NULL));
    psrp_buffer_free(&b);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(session_starts_before_open_with_a_pool_id),
    PSRP_TEST_CASE(session_pool_ids_are_unique),
    PSRP_TEST_CASE(open_payload_carries_capability_then_init),
    PSRP_TEST_CASE(open_payload_is_rejected_twice),
    PSRP_TEST_CASE(configure_only_before_open),
    PSRP_TEST_CASE(capability_and_pool_state_update_the_session),
    PSRP_TEST_CASE(receive_tolerates_any_chunking),
    PSRP_TEST_CASE(pipeline_payload_addresses_a_new_pipeline),
    PSRP_TEST_CASE(pipeline_payload_requires_commands),
    PSRP_TEST_CASE(pipeline_input_and_end_of_input),
    PSRP_TEST_CASE(pipeline_output_and_state_events),
    PSRP_TEST_CASE(record_streams_become_events),
    PSRP_TEST_CASE(unknown_message_is_surfaced),
    PSRP_TEST_CASE(events_are_delivered_in_order_then_exhausted),
    PSRP_TEST_CASE(full_run_a_command_conversation),
    PSRP_TEST_CASE(session_rejects_null_args),
};

PSRP_TEST_MAIN(cases)
