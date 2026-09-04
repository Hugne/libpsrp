/* Client-side PSRP state machine ([MS-PSRP] 3.1). Sans-IO: bytes in, bytes
 * and events out. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_session.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_fragment.h"
#include "psrp/psrp_clixml.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_metadata.h"

typedef struct event_node {
    struct event_node *next;
    psrp_event_t ev;
} event_node_t;

/* 3.1.1.2.5 RunspacePool Information CI Table. Every SET_MAX_RUNSPACES,
 * SET_MIN_RUNSPACES, GET_AVAILABLE_RUNSPACES and RESET_RUNSPACE_STATE carries
 * a unique call identifier that the matching RUNSPACE_AVAILABILITY quotes
 * back. Keeping the table is what lets a caller tell which reply belongs to
 * which request when several are outstanding. */
typedef struct ci_node {
    struct ci_node *next;
    int64_t ci;
    uint32_t message_type;
} ci_node_t;

/* 3.1.1.2.6 Pipeline Table (and 3.1.1.1.2, which is the same set keyed
 * globally). A pipeline is entered when created and removed once it reaches
 * Completed, Failed or Stopped. */
typedef struct pipe_node {
    struct pipe_node *next;
    psrp_guid_t id;
    int32_t state;
} pipe_node_t;

struct psrp_session {
    psrp_guid_t pool_id;
    int32_t pool_state;

    psrp_session_capability_t local_capability;
    psrp_session_capability_t server_capability;
    bool have_server_capability;

    psrp_init_runspacepool_t init;

    psrp_defrag_t *defrag;
    psrp_buffer_t outgoing;
    psrp_buffer_t outgoing_pr;   /* host responses; the WSMan "pr" stream */

    /* 2.2.4: ObjectId must be greater than zero and unique within the pool. */
    uint64_t next_object_id;

    ci_node_t *ci_head;
    int64_t next_ci;

    /* 3.1.1.2.7 session key and the 3.1.1.2.8 transfer timeout. */
    psrp_crypto_t *crypto;
    bool key_exchange_running;
    uint32_t key_timeout_ms;
    uint32_t key_elapsed_ms;

    pipe_node_t *pipe_head;

    event_node_t *ev_head;
    event_node_t *ev_tail;
};

/* Defined further down, next to the other send paths; declared here because
 * the key exchange needs it and sits above it. */
static psrp_result_t emit(psrp_session_t *s, psrp_buffer_t *out, uint32_t type,
                          const psrp_guid_t *pipeline_id,
                          const void *data, size_t len);

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
    psrp_buffer_init(&s->outgoing_pr);
    s->pool_state = PSRP_RUNSPACE_BEFORE_OPEN;
    s->next_object_id = 1;      /* ObjectId 0 is illegal */
    s->next_ci = 1;
    s->key_timeout_ms = 60000;  /* 3.1.1.2.8 recommends this value */
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
    while (s->ci_head) {
        ci_node_t *next = s->ci_head->next;
        free(s->ci_head);
        s->ci_head = next;
    }
    while (s->pipe_head) {
        pipe_node_t *next = s->pipe_head->next;
        free(s->pipe_head);
        s->pipe_head = next;
    }
    psrp_crypto_free(s->crypto);
    psrp_defrag_free(s->defrag);
    psrp_buffer_free(&s->outgoing);
    psrp_buffer_free(&s->outgoing_pr);
    free(s);
}

/* ------------------------------- session key exchange (3.1.5.4.3-5) ----- */

psrp_crypto_t *psrp_session_crypto(psrp_session_t *s)
{
    return s ? s->crypto : NULL;
}

bool psrp_session_has_session_key(const psrp_session_t *s)
{
    return s && s->crypto && psrp_crypto_has_session_key(s->crypto);
}

void psrp_session_set_key_timeout(psrp_session_t *s, uint32_t milliseconds)
{
    if (s) s->key_timeout_ms = milliseconds;
}

uint32_t psrp_session_key_timeout(const psrp_session_t *s)
{
    return s ? s->key_timeout_ms : 0;
}

/* Builds and queues a PUBLIC_KEY, starting the transfer timer. Shared by the
 * higher-layer request and the automatic reply to PUBLIC_KEY_REQUEST. */
static psrp_result_t send_public_key(psrp_session_t *s)
{
    psrp_buffer_t body;
    psrp_result_t rc;

    if (!s->crypto) {
        rc = psrp_crypto_new(&s->crypto);
        if (rc != PSRP_OK) return rc;
    }

    psrp_buffer_init(&body);
    rc = psrp_build_public_key(s->crypto, &body);
    if (rc == PSRP_OK)
        rc = emit(s, &s->outgoing, PSRP_MSG_PUBLIC_KEY, NULL, body.data,
                  body.len);
    psrp_buffer_free(&body);
    if (rc != PSRP_OK) return rc;

    /* 3.1.6: the timer starts when the message is sent, not when it is
     * flushed, because that is when the client stops being able to act. */
    s->key_exchange_running = true;
    s->key_elapsed_ms = 0;
    return PSRP_OK;
}

psrp_result_t psrp_session_start_key_exchange(psrp_session_t *s)
{
    if (!s) return PSRP_ERR_INVALID_ARG;
    if (s->pool_state != PSRP_RUNSPACE_OPENED) return PSRP_ERR_STATE;
    /* 3.1.4.8 step 1: ignore the request outright when a key is already
     * registered or an exchange is under way. Ignoring is success, not an
     * error; the caller asked for a key and there is going to be one. */
    if (psrp_session_has_session_key(s) || s->key_exchange_running)
        return PSRP_OK;
    return send_public_key(s);
}

psrp_result_t psrp_session_tick(psrp_session_t *s, uint32_t elapsed_ms)
{
    psrp_event_t e;

    if (!s) return PSRP_ERR_INVALID_ARG;
    if (!s->key_exchange_running || s->key_timeout_ms == 0) return PSRP_OK;

    /* Saturate rather than wrap: a caller reporting a very long gap must not
     * loop the counter back under the timeout. */
    if (elapsed_ms > (uint32_t)-1 - s->key_elapsed_ms)
        s->key_elapsed_ms = (uint32_t)-1;
    else
        s->key_elapsed_ms += elapsed_ms;

    if (s->key_elapsed_ms < s->key_timeout_ms) return PSRP_OK;

    /* 3.1.2 and 3.1.6: expiry closes the RunspacePool. */
    s->key_exchange_running = false;
    s->pool_state = PSRP_RUNSPACE_BROKEN;
    event_init(&e, PSRP_EVENT_SESSION_KEY_TIMEOUT, PSRP_MSG_PUBLIC_KEY, NULL);
    e.state = PSRP_RUNSPACE_BROKEN;
    return event_push(s, &e);
}

/* ------------------------------------------------- CI table (3.1.1.2.5) -- */

static psrp_result_t ci_add(psrp_session_t *s, uint32_t type, int64_t *ci_out)
{
    ci_node_t *n = (ci_node_t *)calloc(1, sizeof *n);
    if (!n) return PSRP_ERR_NOMEM;
    n->ci = s->next_ci++;
    n->message_type = type;
    n->next = s->ci_head;
    s->ci_head = n;
    if (ci_out) *ci_out = n->ci;
    return PSRP_OK;
}

/* Removes a call identifier, reporting whether it was actually outstanding.
 * A reply quoting an unknown one is worth surfacing rather than swallowing:
 * it means the client and server disagree about what is in flight. */
static bool ci_remove(psrp_session_t *s, int64_t ci)
{
    ci_node_t **link = &s->ci_head;
    while (*link) {
        if ((*link)->ci == ci) {
            ci_node_t *dead = *link;
            *link = dead->next;
            free(dead);
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

size_t psrp_session_pending_call_count(const psrp_session_t *s)
{
    size_t n = 0;
    const ci_node_t *p;
    if (!s) return 0;
    for (p = s->ci_head; p; p = p->next) n++;
    return n;
}

/* ------------------------------------------- pipeline table (3.1.1.2.6) -- */

static pipe_node_t *pipe_find(psrp_session_t *s, const psrp_guid_t *id)
{
    pipe_node_t *p;
    for (p = s->pipe_head; p; p = p->next)
        if (memcmp(&p->id, id, sizeof *id) == 0) return p;
    return NULL;
}

static psrp_result_t pipe_add(psrp_session_t *s, const psrp_guid_t *id)
{
    pipe_node_t *n = (pipe_node_t *)calloc(1, sizeof *n);
    if (!n) return PSRP_ERR_NOMEM;
    n->id = *id;
    /* 3.1.5.4.10: the client initialises the state to Running as it sends. */
    n->state = PSRP_INVOCATION_RUNNING;
    n->next = s->pipe_head;
    s->pipe_head = n;
    return PSRP_OK;
}

static void pipe_remove(psrp_session_t *s, const psrp_guid_t *id)
{
    pipe_node_t **link = &s->pipe_head;
    while (*link) {
        if (memcmp(&(*link)->id, id, sizeof *id) == 0) {
            pipe_node_t *dead = *link;
            *link = dead->next;
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

static bool pipeline_is_running(psrp_session_t *s, const psrp_guid_t *id)
{
    const pipe_node_t *p = pipe_find(s, id);
    return p && p->state == PSRP_INVOCATION_RUNNING;
}

size_t psrp_session_pipeline_count(const psrp_session_t *s)
{
    size_t n = 0;
    const pipe_node_t *p;
    if (!s) return 0;
    for (p = s->pipe_head; p; p = p->next) n++;
    return n;
}

psrp_result_t psrp_session_pipeline_state(const psrp_session_t *s,
                                          const psrp_guid_t *id,
                                          int32_t *out)
{
    const pipe_node_t *p;
    if (!s || !id || !out) return PSRP_ERR_INVALID_ARG;
    for (p = s->pipe_head; p; p = p->next) {
        if (memcmp(&p->id, id, sizeof *id) == 0) {
            *out = p->state;
            return PSRP_OK;
        }
    }
    return PSRP_ERR_NOT_FOUND;
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
    /* 3.1.5.4.10: CREATE_PIPELINE is sent when the pool is Opened. */
    if (s->pool_state != PSRP_RUNSPACE_OPENED) return PSRP_ERR_STATE;

    rc = psrp_guid_generate(&pid);
    if (rc != PSRP_OK) return rc;

    psrp_create_pipeline_defaults(&opts);
    psrp_buffer_init(&body);
    rc = psrp_build_create_pipeline(&opts, commands, count, &body);
    if (rc == PSRP_OK)
        rc = emit(s, out, PSRP_MSG_CREATE_PIPELINE, &pid, body.data, body.len);
    psrp_buffer_free(&body);
    if (rc != PSRP_OK) return rc;

    rc = pipe_add(s, &pid);
    if (rc != PSRP_OK) return rc;
    if (pipeline_id_out) *pipeline_id_out = pid;
    return PSRP_OK;
}

psrp_result_t psrp_session_send_input(psrp_session_t *s,
                                      const psrp_guid_t *pipeline_id,
                                      const psrp_value_t *value)
{
    psrp_buffer_t body;
    psrp_result_t rc;

    if (!s || !pipeline_id || !value) return PSRP_ERR_INVALID_ARG;
    /* 3.1.5.4.17: the pipeline must be Running. Once it has completed it is
     * gone from the table, so a not-found lookup is the same refusal. */
    if (!pipeline_is_running(s, pipeline_id)) return PSRP_ERR_STATE;
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
    /* 3.1.5.4.18: only while the pipeline is Running. */
    if (!pipeline_is_running(s, pipeline_id)) return PSRP_ERR_STATE;
    /* 2.2.2.18: the Data field is empty. */
    return emit(s, &s->outgoing, PSRP_MSG_END_OF_PIPELINE_INPUT, pipeline_id,
                NULL, 0);
}

/* ------------------------------------------- RunspacePool operations ---- */
/*
 * 3.1.5.4.6, .7, .11 and .31 all share a shape: the pool must be Opened, the
 * client mints a unique call identifier and records it, and the server answers
 * with a RUNSPACE_AVAILABILITY quoting it back.
 */
typedef psrp_result_t (*pool_build_fn)(int64_t ci, int32_t arg,
                                       psrp_buffer_t *out);

static psrp_result_t pool_request(psrp_session_t *s, uint32_t type,
                                  pool_build_fn build, int32_t arg,
                                  int64_t *ci_out)
{
    psrp_buffer_t body;
    int64_t ci = 0;
    psrp_result_t rc;

    if (!s) return PSRP_ERR_INVALID_ARG;
    if (s->pool_state != PSRP_RUNSPACE_OPENED) return PSRP_ERR_STATE;

    rc = ci_add(s, type, &ci);
    if (rc != PSRP_OK) return rc;

    psrp_buffer_init(&body);
    rc = build(ci, arg, &body);
    if (rc == PSRP_OK)
        rc = emit(s, &s->outgoing, type, NULL, body.data, body.len);
    psrp_buffer_free(&body);

    /* Leaving a call identifier in the table for a message that never went
     * out would make the pending count lie. */
    if (rc != PSRP_OK) { ci_remove(s, ci); return rc; }
    if (ci_out) *ci_out = ci;
    return PSRP_OK;
}

static psrp_result_t build_max(int64_t ci, int32_t n, psrp_buffer_t *out)
{ return psrp_build_set_max_runspaces(ci, n, out); }
static psrp_result_t build_min(int64_t ci, int32_t n, psrp_buffer_t *out)
{ return psrp_build_set_min_runspaces(ci, n, out); }
static psrp_result_t build_available(int64_t ci, int32_t n, psrp_buffer_t *out)
{ (void)n; return psrp_build_get_available_runspaces(ci, out); }
static psrp_result_t build_reset(int64_t ci, int32_t n, psrp_buffer_t *out)
{ (void)n; return psrp_build_reset_runspace_state(ci, out); }

psrp_result_t psrp_session_set_max_runspaces(psrp_session_t *s, int32_t count,
                                             int64_t *ci_out)
{
    return pool_request(s, PSRP_MSG_SET_MAX_RUNSPACES, build_max, count, ci_out);
}

psrp_result_t psrp_session_set_min_runspaces(psrp_session_t *s, int32_t count,
                                             int64_t *ci_out)
{
    return pool_request(s, PSRP_MSG_SET_MIN_RUNSPACES, build_min, count, ci_out);
}

psrp_result_t psrp_session_get_available_runspaces(psrp_session_t *s,
                                                   int64_t *ci_out)
{
    return pool_request(s, PSRP_MSG_GET_AVAILABLE_RUNSPACES, build_available, 0,
                        ci_out);
}

psrp_result_t psrp_session_reset_runspace_state(psrp_session_t *s,
                                                int64_t *ci_out)
{
    return pool_request(s, PSRP_MSG_RESET_RUNSPACE_STATE, build_reset, 0,
                        ci_out);
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

psrp_result_t psrp_session_take_priority_output(psrp_session_t *s,
                                                psrp_buffer_t *out)
{
    psrp_result_t rc;
    if (!s || !out) return PSRP_ERR_INVALID_ARG;
    if (s->outgoing_pr.len == 0) return PSRP_ERR_NOT_FOUND;
    rc = psrp_buffer_append(out, s->outgoing_pr.data, s->outgoing_pr.len);
    if (rc == PSRP_OK) psrp_buffer_reset(&s->outgoing_pr);
    return rc;
}

psrp_result_t psrp_session_respond_to_host_call(psrp_session_t *s,
                                                const psrp_guid_t *pipeline_id,
                                                int64_t call_id,
                                                int32_t method_id,
                                                const psrp_value_t *return_value,
                                                const char *error_message)
{
    psrp_buffer_t body;
    psrp_result_t rc;
    uint32_t type;

    if (!s) return PSRP_ERR_INVALID_ARG;
    /* Responding to a method that returns nothing is a protocol violation,
     * so refuse rather than confuse the server. */
    if (!psrp_host_method_returns_value(method_id)) return PSRP_ERR_STATE;

    psrp_buffer_init(&body);
    if (return_value)
        rc = psrp_build_host_response(call_id, method_id, return_value, &body);
    else
        rc = psrp_build_host_response_error(call_id, method_id, error_message,
                                            &body);

    type = pipeline_id ? PSRP_MSG_PIPELINE_HOST_RESPONSE
                       : PSRP_MSG_RUNSPACEPOOL_HOST_RESPONSE;
    if (rc == PSRP_OK)
        rc = emit(s, &s->outgoing_pr, type, pipeline_id, body.data, body.len);
    psrp_buffer_free(&body);
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
/* 3.1.5.4.1.2 negotiation check.
 *
 * The spec's table says protocolversion must be 2.1 or 2.2, PSVersion must be
 * 2.0 and SerializationVersion must be 1.1.0.1. Two of those three no longer
 * describe any server anyone runs. Windows PowerShell 5.1 announces PSVersion
 * 5.1, PowerShell 7 announces 7.x and protocolversion 2.3. Enforcing the
 * table literally would mark every modern server Broken and leave the library
 * able to talk only to PowerShell 2.0.
 *
 * So the rule here follows the intent instead: protocolversion must be major
 * 2 with minor 1 or greater, which is the "2.1 or 2.2" floor extended the way
 * the version numbering plainly intends. SerializationVersion is still checked
 * against 1.1.0.1, because that one really has not moved and a different value
 * would mean the CLIXML on the wire is not what this library writes. PSVersion
 * is not checked at all: it reports the server's PowerShell build, and no
 * value of it changes how the protocol behaves.
 */
static bool capability_acceptable(const psrp_session_capability_t *c)
{
    unsigned major = 0, minor = 0;

    if (sscanf(c->protocol_version, "%u.%u", &major, &minor) != 2)
        return false;
    if (major != 2 || minor < 1) return false;

    if (c->serialization_version[0] &&
        strcmp(c->serialization_version, "1.1.0.1") != 0)
        return false;
    return true;
}

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
        /* 3.1.5.4.1.2: validate the announced versions and move the pool to
         * NegotiationSucceeded, or to Broken when they do not match. The
         * failure is reported as a state change rather than an error return,
         * because a broken negotiation is a protocol outcome the caller has
         * to see, not a decoding fault. */
        if (capability_acceptable(&s->server_capability))
            s->pool_state = PSRP_RUNSPACE_NEGOTIATION_SUCCEEDED;
        else
            s->pool_state = PSRP_RUNSPACE_BROKEN;
        event_init(&e, PSRP_EVENT_SESSION_CAPABILITY, m->type, NULL);
        e.state = s->pool_state;
        e.text = dup_cstr(s->server_capability.protocol_version);
        return event_push(s, &e);

    case PSRP_MSG_RUNSPACEPOOL_STATE: {
        psrp_runspacepool_state_msg_t st;
        rc = psrp_parse_runspacepool_state(xml, xml_len, &st);
        if (rc != PSRP_OK) return rc;
        /* 3.1.5.4.9: once Closed or Broken, this message is ignored. Acting
         * on it would let a late message resurrect a dead pool. */
        if (s->pool_state == PSRP_RUNSPACE_CLOSED ||
            s->pool_state == PSRP_RUNSPACE_BROKEN) {
            psrp_runspacepool_state_msg_free(&st);
            return PSRP_OK;
        }
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
        pipe_node_t *p;
        rc = psrp_parse_pipeline_state(xml, xml_len, &st);
        if (rc != PSRP_OK) return rc;
        /* 3.1.5.4.21: a PIPELINE_STATE aimed at the pool rather than at a
         * pipeline is ignored, as is one for a pipeline that is not Running. */
        p = psrp_guid_is_empty(&m->pid) ? NULL : pipe_find(s, &m->pid);
        if (!p || p->state != PSRP_INVOCATION_RUNNING) {
            psrp_pipeline_state_msg_free(&st);
            return PSRP_OK;
        }
        p->state = st.state;
        /* A finished pipeline leaves both tables. */
        if (st.state == PSRP_INVOCATION_COMPLETED ||
            st.state == PSRP_INVOCATION_FAILED ||
            st.state == PSRP_INVOCATION_STOPPED)
            pipe_remove(s, &m->pid);
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

    case PSRP_MSG_RUNSPACE_AVAILABILITY: {
        psrp_runspace_availability_t ra;
        rc = psrp_parse_runspace_availability(xml, xml_len, &ra);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_RUNSPACE_AVAILABILITY, m->type, NULL);
        /* 3.1.5.4.8: the call identifier leaves the CI table. `state` says
         * whether it was one we were actually waiting for; a reply we never
         * asked for means the two sides disagree about what is in flight. */
        e.state = ci_remove(s, ra.ci) ? 1 : 0;
        e.call_id = ra.ci;
        e.count = ra.is_count ? ra.count : (ra.accepted ? 1 : 0);
        e.has_count = ra.is_count;
        return event_push(s, &e);
    }

    case PSRP_MSG_RUNSPACEPOOL_INIT_DATA: {
        psrp_runspacepool_init_data_t d;
        rc = psrp_parse_runspacepool_init_data(xml, xml_len, &d);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_POOL_INIT_DATA, m->type, NULL);
        e.state = d.min_runspaces;
        e.count = d.max_runspaces;
        e.has_count = true;
        return event_push(s, &e);
    }

    case PSRP_MSG_PUBLIC_KEY_REQUEST:
        /* 3.1.5.4.5: the client MUST answer with a PUBLIC_KEY. There is
         * nothing here for a caller to decide, so reply now and tell them
         * only so they know to flush the output. */
        rc = send_public_key(s);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_PUBLIC_KEY_REQUESTED, m->type, NULL);
        return event_push(s, &e);

    case PSRP_MSG_ENCRYPTED_SESSION_KEY:
        /* 3.1.5.4.4. Without a crypto context there is no private key to
         * decrypt with, which means this arrived unrequested. */
        if (!s->crypto) return PSRP_ERR_STATE;
        rc = psrp_parse_encrypted_session_key(s->crypto, xml, xml_len);
        if (rc != PSRP_OK) return rc;
        /* 3.1.6: receiving the key cancels the transfer timer. */
        s->key_exchange_running = false;
        s->key_elapsed_ms = 0;
        event_init(&e, PSRP_EVENT_SESSION_KEY_READY, m->type, NULL);
        return event_push(s, &e);

    case PSRP_MSG_USER_EVENT: {
        psrp_user_event_t ue;
        rc = psrp_parse_user_event(xml, xml_len, &ue);
        if (rc != PSRP_OK) return rc;
        event_init(&e, PSRP_EVENT_USER_EVENT, m->type, &m->pid);
        e.state = ue.event_id;
        e.text = ue.source_identifier;
        ue.source_identifier = NULL;
        psrp_user_event_free(&ue);
        return event_push(s, &e);
    }

    case PSRP_MSG_APPLICATION_PRIVATE_DATA:
        event_init(&e, PSRP_EVENT_APPLICATION_PRIVATE_DATA, m->type, NULL);
        /* Opaque to PSRP; hand the caller the whole object. */
        (void)psrp_parse_pipeline_output(xml, xml_len, &e.value);
        return event_push(s, &e);

    case PSRP_MSG_RUNSPACEPOOL_HOST_CALL:
    case PSRP_MSG_PIPELINE_HOST_CALL: {
        psrp_host_call_t hc;
        event_init(&e, PSRP_EVENT_HOST_CALL, m->type, &m->pid);
        (void)psrp_parse_pipeline_output(xml, xml_len, &e.value);
        /* 3.1.5.4.16 and .28 require the response to quote the same ci, so
         * surface it rather than making the caller dig it out again. */
        if (psrp_parse_host_call(xml, xml_len, &hc) == PSRP_OK) {
            e.call_id = hc.call_id;
            e.state = hc.method_id;
            psrp_host_call_free(&hc);
        }
        return event_push(s, &e);
    }

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
