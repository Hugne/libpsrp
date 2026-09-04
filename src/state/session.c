/* Client-side PSRP state machine ([MS-PSRP] 3.1). Sans-IO: bytes in, bytes
 * and events out. */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_session.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
#include "psrp/psrp_clixml.h"

typedef struct event_node {
    struct event_node *next;
    psrp_event_t ev;
} event_node_t;

struct psrp_session {
    psrp_guid_t pool_id;
    int32_t pool_state;

    psrp_session_capability_t local_capability;
    psrp_session_capability_t server_capability;
    bool have_server_capability;

    psrp_init_runspacepool_t init;

    psrp_defrag_t *defrag;
    psrp_buffer_t outgoing;

    /* 2.2.4: ObjectId must be greater than zero and unique within the pool. */
    uint64_t next_object_id;

    event_node_t *ev_head;
    event_node_t *ev_tail;
};

void psrp_event_free(psrp_event_t *e)
{
    if (!e) return;
    free(e->text);
    psrp_value_free(&e->value);
    memset(e, 0, sizeof *e);
    e->state = -1;
}

static void event_init(psrp_event_t *e, psrp_event_kind_t kind, uint32_t type,
                       const psrp_guid_t *pipeline)
{
    memset(e, 0, sizeof *e);
    psrp_value_init(&e->value);
    e->kind = kind;
    e->message_type = type;
    e->state = -1;
    e->pipeline_id = pipeline ? *pipeline : psrp_guid_empty;
}

static psrp_result_t event_push(psrp_session_t *s, psrp_event_t *e)
{
    event_node_t *n = (event_node_t *)calloc(1, sizeof *n);
    if (!n) return PSRP_ERR_NOMEM;
    n->ev = *e;                 /* move */
    memset(e, 0, sizeof *e);
    psrp_value_init(&e->value);
    if (s->ev_tail) s->ev_tail->next = n;
    else s->ev_head = n;
    s->ev_tail = n;
    return PSRP_OK;
}

psrp_session_t *psrp_session_new(void)
{
    psrp_session_t *s = (psrp_session_t *)calloc(1, sizeof *s);
    if (!s) return NULL;

    if (psrp_guid_generate(&s->pool_id) != PSRP_OK) { free(s); return NULL; }

    s->defrag = psrp_defrag_new();
    if (!s->defrag) { free(s); return NULL; }

    psrp_buffer_init(&s->outgoing);
    s->pool_state = PSRP_RUNSPACE_BEFORE_OPEN;
    s->next_object_id = 1;      /* ObjectId 0 is illegal */
    psrp_session_capability_defaults(&s->local_capability);
    psrp_init_runspacepool_defaults(&s->init);
    return s;
}

void psrp_session_free(psrp_session_t *s)
{
    event_node_t *n;
    if (!s) return;
    n = s->ev_head;
    while (n) {
        event_node_t *next = n->next;
        psrp_event_free(&n->ev);
        free(n);
        n = next;
    }
    psrp_defrag_free(s->defrag);
    psrp_buffer_free(&s->outgoing);
    free(s);
}

psrp_result_t psrp_session_configure(psrp_session_t *s,
                                     const psrp_init_runspacepool_t *init)
{
    if (!s || !init) return PSRP_ERR_INVALID_ARG;
    if (s->pool_state != PSRP_RUNSPACE_BEFORE_OPEN) return PSRP_ERR_STATE;
    s->init = *init;
    return PSRP_OK;
}

const psrp_guid_t *psrp_session_pool_id(const psrp_session_t *s)
{
    return s ? &s->pool_id : NULL;
}

int32_t psrp_session_pool_state(const psrp_session_t *s)
{
    return s ? s->pool_state : -1;
}

const psrp_session_capability_t *psrp_session_server_capability(
    const psrp_session_t *s)
{
    if (!s || !s->have_server_capability) return NULL;
    return &s->server_capability;
}

/* Frames a payload as a PSRP message and appends its fragments to `out`. */
static psrp_result_t emit(psrp_session_t *s, psrp_buffer_t *out, uint32_t type,
                          const psrp_guid_t *pipeline_id,
                          const void *data, size_t len)
{
    psrp_message_t m;
    psrp_buffer_t msg;
    psrp_result_t rc;

    memset(&m, 0, sizeof m);
    m.destination = PSRP_DEST_SERVER;
    m.type = type;
    m.rpid = s->pool_id;
    m.pid = pipeline_id ? *pipeline_id : psrp_guid_empty;
    m.data = (const uint8_t *)data;
    m.data_len = len;

    psrp_buffer_init(&msg);
    rc = psrp_message_encode(&msg, &m);
    if (rc == PSRP_OK)
        rc = psrp_fragment_split(out, s->next_object_id++, msg.data, msg.len, 0);
    psrp_buffer_free(&msg);
    return rc;
}

psrp_result_t psrp_session_open_payload(psrp_session_t *s, psrp_buffer_t *out)
{
    psrp_buffer_t cap, init;
    psrp_result_t rc;

    if (!s || !out) return PSRP_ERR_INVALID_ARG;
    if (s->pool_state != PSRP_RUNSPACE_BEFORE_OPEN) return PSRP_ERR_STATE;

    psrp_buffer_init(&cap);
    psrp_buffer_init(&init);

    rc = psrp_build_session_capability(&s->local_capability, &cap);
    if (rc == PSRP_OK) rc = psrp_build_init_runspacepool(&s->init, &init);

    /* Both messages travel together in the shell-create payload; 2.2.4 notes
     * this is exactly what a single wxf:Create may carry. */
    if (rc == PSRP_OK)
        rc = emit(s, out, PSRP_MSG_SESSION_CAPABILITY, NULL, cap.data, cap.len);
    if (rc == PSRP_OK)
        rc = emit(s, out, PSRP_MSG_INIT_RUNSPACEPOOL, NULL, init.data, init.len);

    psrp_buffer_free(&cap);
    psrp_buffer_free(&init);

    if (rc == PSRP_OK) s->pool_state = PSRP_RUNSPACE_NEGOTIATION_SENT;
    return rc;
}

psrp_result_t psrp_session_pipeline_payload(psrp_session_t *s,
                                            psrp_command_t *const *commands,
                                            size_t count,
                                            psrp_guid_t *pipeline_id_out,
                                            psrp_buffer_t *out)
{
    psrp_create_pipeline_t opts;
    psrp_buffer_t body;
    psrp_guid_t pid;
    psrp_result_t rc;

    if (!s || !out || !commands || count == 0) return PSRP_ERR_INVALID_ARG;

    rc = psrp_guid_generate(&pid);
    if (rc != PSRP_OK) return rc;

    psrp_create_pipeline_defaults(&opts);
    psrp_buffer_init(&body);
    rc = psrp_build_create_pipeline(&opts, commands, count, &body);
    if (rc == PSRP_OK)
        rc = emit(s, out, PSRP_MSG_CREATE_PIPELINE, &pid, body.data, body.len);
    psrp_buffer_free(&body);

    if (rc == PSRP_OK && pipeline_id_out) *pipeline_id_out = pid;
    return rc;
}

psrp_result_t psrp_session_send_input(psrp_session_t *s,
                                      const psrp_guid_t *pipeline_id,
                                      const psrp_value_t *value)
{
    psrp_buffer_t body;
    psrp_result_t rc;

    if (!s || !pipeline_id || !value) return PSRP_ERR_INVALID_ARG;
    psrp_buffer_init(&body);
    rc = psrp_build_pipeline_input(value, &body);
    if (rc == PSRP_OK)
        rc = emit(s, &s->outgoing, PSRP_MSG_PIPELINE_INPUT, pipeline_id,
                  body.data, body.len);
    psrp_buffer_free(&body);
    return rc;
}

psrp_result_t psrp_session_end_input(psrp_session_t *s,
                                     const psrp_guid_t *pipeline_id)
{
    if (!s || !pipeline_id) return PSRP_ERR_INVALID_ARG;
    /* 2.2.2.18: the Data field is empty. */
    return emit(s, &s->outgoing, PSRP_MSG_END_OF_PIPELINE_INPUT, pipeline_id,
                NULL, 0);
}

psrp_result_t psrp_session_take_output(psrp_session_t *s, psrp_buffer_t *out)
{
    psrp_result_t rc;
    if (!s || !out) return PSRP_ERR_INVALID_ARG;
    if (s->outgoing.len == 0) return PSRP_ERR_NOT_FOUND;
    rc = psrp_buffer_append(out, s->outgoing.data, s->outgoing.len);
    if (rc == PSRP_OK) psrp_buffer_reset(&s->outgoing);
    return rc;
}

/* ------------------------------------------------------- dispatching ---- */

static char *dup_cstr(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* Maps a record-carrying message type onto its event kind. */
static psrp_event_kind_t record_kind(uint32_t type)
{
    switch (type) {
    case PSRP_MSG_ERROR_RECORD:       return PSRP_EVENT_ERROR_RECORD;
    case PSRP_MSG_DEBUG_RECORD:       return PSRP_EVENT_DEBUG_RECORD;
    case PSRP_MSG_VERBOSE_RECORD:     return PSRP_EVENT_VERBOSE_RECORD;
    case PSRP_MSG_WARNING_RECORD:     return PSRP_EVENT_WARNING_RECORD;
    case PSRP_MSG_PROGRESS_RECORD:    return PSRP_EVENT_PROGRESS_RECORD;
    case PSRP_MSG_INFORMATION_RECORD: return PSRP_EVENT_INFORMATION_RECORD;
    default:                          return PSRP_EVENT_NONE;
    }
}

static psrp_result_t dispatch(psrp_session_t *s, const psrp_message_t *m)
{
    const uint8_t *xml = NULL;
    size_t xml_len = 0;
    psrp_event_t e;
    psrp_result_t rc = PSRP_OK;

    psrp_message_xml(m, &xml, &xml_len);

    switch (m->type) {
    case PSRP_MSG_SESSION_CAPABILITY:
        rc = psrp_parse_session_capability(xml, xml_len, &s->server_capability);
        if (rc != PSRP_OK) return rc;
        s->have_server_capability = true;
        event_init(&e, PSRP_EVENT_SESSION_CAPABILITY, m->type, NULL);
        e.text = dup_cstr(s->server_capability.protocol_version);
        return event_push(s, &e);

    case PSRP_MSG_RUNSPACEPOOL_STATE: {
        psrp_runspacepool_state_msg_t st;
        rc = psrp_parse_runspacepool_state(xml, xml_len, &st);
        if (rc != PSRP_OK) return rc;
        s->pool_state = st.state;
        event_init(&e, PSRP_EVENT_POOL_STATE, m->type, NULL);
        e.state = st.state;
        e.text = st.error_text;      /* move ownership */
        st.error_text = NULL;
        psrp_runspacepool_state_msg_free(&st);
        return event_push(s, &e);
    }

    case PSRP_MSG_PIPELINE_STATE: {
        psrp_pipeline_state_msg_t st;
        rc = psrp_parse_pipeline_state(xml, xml_len, &st);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_PIPELINE_STATE, m->type, &m->pid);
        e.state = st.state;
        e.text = st.error_text;
        st.error_text = NULL;
        psrp_pipeline_state_msg_free(&st);
        return event_push(s, &e);
    }

    case PSRP_MSG_PIPELINE_OUTPUT:
        event_init(&e, PSRP_EVENT_PIPELINE_OUTPUT, m->type, &m->pid);
        rc = psrp_parse_pipeline_output(xml, xml_len, &e.value);
        if (rc != PSRP_OK) { psrp_event_free(&e); return rc; }
        return event_push(s, &e);

    case PSRP_MSG_ERROR_RECORD: {
        psrp_error_record_t er;
        rc = psrp_parse_error_record(xml, xml_len, &er);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_ERROR_RECORD, m->type, &m->pid);
        e.text = er.message;
        er.message = NULL;
        e.state = er.category;
        psrp_error_record_free(&er);
        return event_push(s, &e);
    }

    case PSRP_MSG_DEBUG_RECORD:
    case PSRP_MSG_VERBOSE_RECORD:
    case PSRP_MSG_WARNING_RECORD: {
        psrp_informational_record_t ir;
        rc = psrp_parse_informational_record(xml, xml_len, &ir);
        if (rc != PSRP_OK) return rc;
        event_init(&e, record_kind(m->type), m->type, &m->pid);
        e.text = ir.message;
        ir.message = NULL;
        psrp_informational_record_free(&ir);
        return event_push(s, &e);
    }

    case PSRP_MSG_PROGRESS_RECORD: {
        psrp_progress_record_t pr;
        rc = psrp_parse_progress_record(xml, xml_len, &pr);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_PROGRESS_RECORD, m->type, &m->pid);
        e.text = pr.activity;
        pr.activity = NULL;
        e.state = pr.percent_complete;
        psrp_progress_record_free(&pr);
        return event_push(s, &e);
    }

    case PSRP_MSG_INFORMATION_RECORD: {
        psrp_information_record_t ir;
        rc = psrp_parse_information_record(xml, xml_len, &ir);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_INFORMATION_RECORD, m->type, &m->pid);
        e.text = ir.message_data;
        ir.message_data = NULL;
        psrp_information_record_free(&ir);
        return event_push(s, &e);
    }

    case PSRP_MSG_APPLICATION_PRIVATE_DATA:
        event_init(&e, PSRP_EVENT_APPLICATION_PRIVATE_DATA, m->type, NULL);
        /* Opaque to PSRP; hand the caller the whole object. */
        (void)psrp_parse_pipeline_output(xml, xml_len, &e.value);
        return event_push(s, &e);

    case PSRP_MSG_RUNSPACEPOOL_HOST_CALL:
    case PSRP_MSG_PIPELINE_HOST_CALL:
        event_init(&e, PSRP_EVENT_HOST_CALL, m->type, &m->pid);
        (void)psrp_parse_pipeline_output(xml, xml_len, &e.value);
        return event_push(s, &e);

    default:
        /* Surfaced rather than dropped, so a caller can log or ignore
         * deliberately instead of wondering where a message went. */
        event_init(&e, PSRP_EVENT_UNKNOWN_MESSAGE, m->type, &m->pid);
        return event_push(s, &e);
    }
}

psrp_result_t psrp_session_receive(psrp_session_t *s, const void *data,
                                   size_t len)
{
    psrp_result_t rc;

    if (!s) return PSRP_ERR_INVALID_ARG;
    rc = psrp_defrag_push(s->defrag, data, len);
    if (rc != PSRP_OK) return rc;

    for (;;) {
        psrp_buffer_t msg;
        psrp_message_t m;
        uint64_t oid = 0;

        psrp_buffer_init(&msg);
        rc = psrp_defrag_next(s->defrag, &oid, &msg);
        if (rc == PSRP_ERR_NOT_FOUND) { psrp_buffer_free(&msg); return PSRP_OK; }
        if (rc != PSRP_OK) { psrp_buffer_free(&msg); return rc; }

        rc = psrp_message_decode(msg.data, msg.len, &m);
        if (rc == PSRP_OK) rc = dispatch(s, &m);
        psrp_buffer_free(&msg);
        if (rc != PSRP_OK) return rc;
    }
}

psrp_result_t psrp_session_next_event(psrp_session_t *s, psrp_event_t *out)
{
    event_node_t *n;
    if (!s || !out) return PSRP_ERR_INVALID_ARG;
    n = s->ev_head;
    if (!n) return PSRP_ERR_NOT_FOUND;
    *out = n->ev;               /* move */
    s->ev_head = n->next;
    if (!s->ev_head) s->ev_tail = NULL;
    free(n);
    return PSRP_OK;
}
