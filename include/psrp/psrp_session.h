/* psrp_session.h - the client-side PSRP state machine ([MS-PSRP] 3.1).
 *
 * This is the sans-IO core: it performs no I/O at all. The caller pumps bytes
 * in with psrp_session_receive and pulls bytes out with the payload
 * functions, then moves them over whatever transport it likes. Everything the
 * server says arrives as an event queue.
 *
 * The three payload functions exist because WSMan carries PSRP data in three
 * different requests, and a caller has to know which is which:
 *
 *   psrp_session_open_payload      -> the shell Create request's open content
 *   psrp_session_pipeline_payload  -> a RunShellCommand request's arguments
 *   psrp_session_take_output       -> a Send request (pipeline input, replies)
 *
 * Typical flow:
 *
 *   s = psrp_session_new();
 *   psrp_session_open_payload(s, &buf);        // send with shell Create
 *   ... psrp_session_receive(s, data, len);    // feed everything received
 *   while (psrp_session_next_event(s, &e) == PSRP_OK) { ... }
 *   // once the pool reports Opened:
 *   psrp_session_pipeline_payload(s, &cmd, 1, &pipeline_id, &buf);
 */
#ifndef PSRP_SESSION_H
#define PSRP_SESSION_H

#include "psrp/psrp_messages.h"
#include "psrp/psrp_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_session psrp_session_t;

typedef enum psrp_event_kind {
    PSRP_EVENT_NONE = 0,
    PSRP_EVENT_SESSION_CAPABILITY,   /* server announced its versions */
    PSRP_EVENT_POOL_STATE,           /* RunspacePool state changed */
    PSRP_EVENT_APPLICATION_PRIVATE_DATA,
    PSRP_EVENT_PIPELINE_OUTPUT,      /* `value` holds the object */
    PSRP_EVENT_PIPELINE_STATE,       /* `state` holds PSInvocationState */
    PSRP_EVENT_ERROR_RECORD,         /* `text` holds the message */
    PSRP_EVENT_DEBUG_RECORD,
    PSRP_EVENT_VERBOSE_RECORD,
    PSRP_EVENT_WARNING_RECORD,
    PSRP_EVENT_PROGRESS_RECORD,
    PSRP_EVENT_INFORMATION_RECORD,
    PSRP_EVENT_HOST_CALL,            /* server wants the client's host */
    PSRP_EVENT_USER_EVENT,           /* `text` holds the source identifier */
    PSRP_EVENT_RUNSPACE_AVAILABILITY,/* reply to a set/get runspaces request */
    PSRP_EVENT_POOL_INIT_DATA,       /* reply to CONNECT_RUNSPACEPOOL */
    PSRP_EVENT_UNKNOWN_MESSAGE       /* surfaced, not silently dropped */
} psrp_event_kind_t;

typedef struct psrp_event {
    psrp_event_kind_t kind;
    uint32_t message_type;       /* the raw PSRP message type */
    psrp_guid_t pipeline_id;     /* zero for pool-level messages */
    int32_t state;               /* pool or pipeline state, else -1.
                                  * For a host call, the method id; for
                                  * RUNSPACE_AVAILABILITY, 1 when the call
                                  * identifier was one we were waiting for;
                                  * for POOL_INIT_DATA, the minimum runspaces.
                                  * For SESSION_CAPABILITY, the pool state the
                                  * negotiation produced. */
    int64_t call_id;             /* host call ci, or the ci being answered */
    int64_t count;               /* RUNSPACE_AVAILABILITY count or POOL_INIT_
                                  * DATA maximum; see has_count */
    bool has_count;              /* false when the reply was a plain Boolean */
    char *text;                  /* owned; record message or error text */
    psrp_value_t value;          /* owned; PIPELINE_OUTPUT payload */
} psrp_event_t;

void psrp_event_free(psrp_event_t *e);

psrp_session_t *psrp_session_new(void);
void psrp_session_free(psrp_session_t *s);

/* Overrides the defaults used when building the open payload. Must be called
 * before psrp_session_open_payload. */
psrp_result_t psrp_session_configure(psrp_session_t *s,
                                     const psrp_init_runspacepool_t *init);

/* The RunspacePool id (RPID) carried by every message. Generated at
 * construction. */
const psrp_guid_t *psrp_session_pool_id(const psrp_session_t *s);

/* Current RunspacePool state (psrp_runspace_pool_state_t). */
int32_t psrp_session_pool_state(const psrp_session_t *s);

/* Versions the server announced; valid once PSRP_EVENT_SESSION_CAPABILITY has
 * been seen. */
const psrp_session_capability_t *psrp_session_server_capability(
    const psrp_session_t *s);

/* Builds SESSION_CAPABILITY + INIT_RUNSPACEPOOL, fragmented and ready to be
 * carried in the transport's shell-create request. */
psrp_result_t psrp_session_open_payload(psrp_session_t *s, psrp_buffer_t *out);

/* Builds CREATE_PIPELINE for `count` commands and allocates the pipeline id.
 * The bytes go in the transport's run-command request. */
psrp_result_t psrp_session_pipeline_payload(psrp_session_t *s,
                                            psrp_command_t *const *commands,
                                            size_t count,
                                            psrp_guid_t *pipeline_id_out,
                                            psrp_buffer_t *out);

/* Queues a PIPELINE_INPUT object for the given pipeline. */
psrp_result_t psrp_session_send_input(psrp_session_t *s,
                                      const psrp_guid_t *pipeline_id,
                                      const psrp_value_t *value);
/* Queues END_OF_PIPELINE_INPUT, whose Data field is empty by definition. */
psrp_result_t psrp_session_end_input(psrp_session_t *s,
                                     const psrp_guid_t *pipeline_id);

/* Moves any queued outgoing bytes into `out`, emptying the queue. Returns
 * PSRP_ERR_NOT_FOUND when there is nothing to send. */
/* RunspacePool operations (3.1.5.4.6, .7, .11, .31). Each requires the pool
 * to be Opened, mints a unique call identifier, records it in the pool's CI
 * table (3.1.1.2.5) and queues the message for the next take_output. The
 * server answers with PSRP_EVENT_RUNSPACE_AVAILABILITY quoting the same
 * identifier, which removes it from the table. */
psrp_result_t psrp_session_set_max_runspaces(psrp_session_t *s, int32_t count,
                                             int64_t *ci_out);
psrp_result_t psrp_session_set_min_runspaces(psrp_session_t *s, int32_t count,
                                             int64_t *ci_out);
psrp_result_t psrp_session_get_available_runspaces(psrp_session_t *s,
                                                   int64_t *ci_out);
psrp_result_t psrp_session_reset_runspace_state(psrp_session_t *s,
                                                int64_t *ci_out);

/* Call identifiers still awaiting a RUNSPACE_AVAILABILITY. */
size_t psrp_session_pending_call_count(const psrp_session_t *s);

/* Pipelines in the pool's pipeline table (3.1.1.2.6). A pipeline is entered
 * when created and removed once it reports Completed, Failed or Stopped, so
 * this counts what is still in flight. */
size_t psrp_session_pipeline_count(const psrp_session_t *s);

/* State of a tracked pipeline. Returns PSRP_ERR_NOT_FOUND once the pipeline
 * has finished and left the table. */
psrp_result_t psrp_session_pipeline_state(const psrp_session_t *s,
                                          const psrp_guid_t *id,
                                          int32_t *out);

psrp_result_t psrp_session_take_output(psrp_session_t *s, psrp_buffer_t *out);

/* Answers a host call. `pipeline_id` selects PIPELINE_HOST_RESPONSE when
 * non-NULL and RUNSPACEPOOL_HOST_RESPONSE otherwise, matching the call that
 * arrived. Pass `return_value` for a successful result, or NULL together with
 * `error_message` to report that the host could not do it.
 *
 * Only call this for methods where psrp_host_method_returns_value() is true:
 * the spec forbids responding to the others. */
psrp_result_t psrp_session_respond_to_host_call(psrp_session_t *s,
                                                const psrp_guid_t *pipeline_id,
                                                int64_t call_id,
                                                int32_t method_id,
                                                const psrp_value_t *return_value,
                                                const char *error_message);

/* Host responses travel on the WSMan "pr" stream rather than "stdin"
 * (3.1.5.3.5), so they are queued separately and drained with this. */
psrp_result_t psrp_session_take_priority_output(psrp_session_t *s,
                                                psrp_buffer_t *out);

/* Feeds bytes received from the transport. Complete messages are decoded and
 * turned into events. */
psrp_result_t psrp_session_receive(psrp_session_t *s, const void *data,
                                   size_t len);

/* Pops the oldest event. Returns PSRP_ERR_NOT_FOUND when the queue is empty.
 * The caller owns the event and must psrp_event_free it. */
psrp_result_t psrp_session_next_event(psrp_session_t *s, psrp_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_SESSION_H */
