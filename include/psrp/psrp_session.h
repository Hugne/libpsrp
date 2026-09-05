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
 *   psrp_session_pipeline_payload(s, &cmd, 1, PSRP_PIPELINE_NO_INPUT,
 *                                 &pipeline_id, &buf);
 */
#ifndef PSRP_SESSION_H
#define PSRP_SESSION_H

#include "psrp/psrp_messages.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_crypto.h"
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
    PSRP_EVENT_PUBLIC_KEY_REQUESTED, /* server asked for our public key */
    PSRP_EVENT_SESSION_KEY_READY,    /* session key installed; SS now usable */
    PSRP_EVENT_SESSION_KEY_TIMEOUT,  /* the exchange timed out; pool broken */
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
/* Reports the machine's time zone in SESSION_CAPABILITY (2.2.3.10). The spec
 * says a client SHOULD send it; a session does not by default, because doing
 * so tells the server where the client is and that should be a choice. Must be
 * called before psrp_session_open_payload. */
psrp_result_t psrp_session_send_timezone(psrp_session_t *s);

/* Declares that this client provides a host, so the server routes host method
 * calls to it instead of handling them itself.
 *
 * Without this, every HostInfo the session sends says _isHostNull, and a
 * server told that runs Write-Host and prompts against its own default host:
 * the output goes nowhere the client can see and no RUNSPACEPOOL_HOST_CALL or
 * PIPELINE_HOST_CALL ever arrives. The whole host-call path is unreachable,
 * which is not obvious from the outside because scripts still run and still
 * produce ordinary output.
 *
 * `console` describes the terminal being offered; pass NULL for a plausible
 * headless default. It is copied. Must be called before
 * psrp_session_open_payload, since the pool's HostInfo goes out with it.
 *
 * A caller that declares a host must answer the calls it receives: every
 * PSRP_EVENT_HOST_CALL whose method returns a value needs a matching
 * psrp_session_respond_to_host_call, or the pipeline waits forever. */
psrp_result_t psrp_session_provide_host(psrp_session_t *s,
                                        const psrp_host_default_data_t *console);

psrp_result_t psrp_session_open_payload(psrp_session_t *s, psrp_buffer_t *out);

/* ------------------- disconnect and reconnect (3.1.4.9, 3.1.4.10) ------ */
/*
 * A disconnected RunspacePool keeps running on the server. The same client can
 * come back to it with a wxf:Reconnect, or a different client can adopt it
 * with a wxf:Connect after discovering its ShellID.
 *
 * The transport owns those three messages; the session owns the state they
 * imply, so it is told the outcome rather than performing the exchange.
 */

/* Adopts an existing pool. The identifier is the ShellID of the pool being
 * connected to (3.1.1.2.4), discovered by enumerating the server's shells.
 * Only valid before the session has sent anything. */
psrp_result_t psrp_session_adopt_pool(psrp_session_t *s, const psrp_guid_t *id);

/* 3.1.4.10.3 steps 3 and 4. Produces the open content for a wxf:Connect:
 * SESSION_CAPABILITY followed by CONNECT_RUNSPACEPOOL. The pool must be in
 * BeforeOpen or Connecting; it ends in NegotiationSent. */
psrp_result_t psrp_session_connect_payload(psrp_session_t *s,
                                           psrp_buffer_t *out);

/* 3.1.4.9 step 3. The transport reports a wxf:DisconnectResponse. The pool and
 * every pipeline in it become Disconnected. A pool that is not Opened ignores
 * the request, which the spec asks for explicitly, so this returns PSRP_OK
 * without changing anything. */
psrp_result_t psrp_session_notify_disconnected(psrp_session_t *s);

/* 3.1.4.10.2 step 2. The transport reports a wxf:ReconnectResponse. The pool
 * returns to Opened and its pipelines to Running. */
psrp_result_t psrp_session_notify_reconnected(psrp_session_t *s);

/* A wxf:Fault during any of these breaks the pool (3.1.4.9 step 3,
 * 3.1.4.10.2 step 2, 3.1.5.3.13). */
psrp_result_t psrp_session_notify_fault(psrp_session_t *s, const char *reason);

/* Builds CREATE_PIPELINE for `count` commands and allocates the pipeline id.
 * The bytes go in the transport's run-command request. */
/* Whether a pipeline will be fed input.
 *
 * This has to be declared when the pipeline is created, not when input is
 * sent: it becomes the NoInput property of CREATE_PIPELINE (2.2.2.10), and a
 * server told NoInput=true closes the input stream immediately and discards
 * anything that arrives afterwards. Getting it wrong is silent -- the input
 * is simply ignored and the script sees an empty $input -- which is exactly
 * how it went unnoticed until a test actually fed a live pipeline. */
typedef enum psrp_pipeline_input {
    PSRP_PIPELINE_NO_INPUT = 0,      /* nothing will be sent */
    PSRP_PIPELINE_EXPECT_INPUT = 1   /* psrp_session_send_input will follow */
} psrp_pipeline_input_t;

psrp_result_t psrp_session_pipeline_payload(psrp_session_t *s,
                                            psrp_command_t *const *commands,
                                            size_t count,
                                            psrp_pipeline_input_t input,
                                            psrp_guid_t *pipeline_id_out,
                                            psrp_buffer_t *out);

/* Queues a PIPELINE_INPUT object for the given pipeline. */
/* Sends one object as pipeline input. The pipeline must have been created
 * with PSRP_PIPELINE_EXPECT_INPUT; otherwise the server has already closed
 * the input stream and will discard this silently. */
/* 3.1.5.4.14 GET_COMMAND_METADATA. Sent as a pipeline, exactly like
 * CREATE_PIPELINE: the payload goes in a RunShellCommand request and the
 * results come back on that pipeline's output stream, first a count object and
 * then one CommandMetadata per command. Read them with
 * psrp_command_metadata_count_from_value and psrp_command_metadata_from_value.
 *
 * Passing no patterns asks for everything, which 2.2.2.14 defines as a single
 * "*". */
psrp_result_t psrp_session_command_metadata_payload(
    psrp_session_t *s,
    const char *const *name_patterns, size_t pattern_count,
    int32_t command_type,
    const char *const *namespaces, size_t namespace_count,
    psrp_guid_t *pipeline_id_out,
    psrp_buffer_t *out);

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

/* ---------------------- session key exchange (3.1.4.8, 3.1.5.4.3-5) ----- */
/*
 * A SecureString can only be sent once a session key is in place. The server
 * may ask for the client's public key on its own (PUBLIC_KEY_REQUEST), or the
 * higher layer may start the exchange itself.
 *
 * A PUBLIC_KEY_REQUEST is answered automatically: the spec says the client
 * MUST respond, and there is nothing for a caller to decide. The reply is
 * queued for the next take_output, and PSRP_EVENT_PUBLIC_KEY_REQUESTED is
 * raised so the caller knows to flush it.
 */

/* 3.1.4.8. Requires an Opened pool. Does nothing and returns PSRP_OK if a key
 * is already installed or an exchange is already running, which is what the
 * spec asks for rather than an error. */
psrp_result_t psrp_session_start_key_exchange(psrp_session_t *s);

/* True once the session key is installed and SecureStrings can be sent. */
bool psrp_session_has_session_key(const psrp_session_t *s);

/* The session's crypto context, for encrypting a SecureString. NULL until a
 * key exchange has been started. Owned by the session. */
psrp_crypto_t *psrp_session_crypto(psrp_session_t *s);

/* 3.1.1.2.8 SessionKeyTransferTimeoutms, defaulting to the 60000 the spec
 * recommends. Setting 0 disables the timer. */
void psrp_session_set_key_timeout(psrp_session_t *s, uint32_t milliseconds);
uint32_t psrp_session_key_timeout(const psrp_session_t *s);

/* Advances the session key transfer timer (3.1.2, 3.1.6). The library does no
 * I/O and reads no clock, so the caller reports elapsed time. On expiry the
 * pool is marked Broken and PSRP_EVENT_SESSION_KEY_TIMEOUT is raised, which is
 * the closure the spec requires. Harmless to call when no timer is running. */
psrp_result_t psrp_session_tick(psrp_session_t *s, uint32_t elapsed_ms);

/* Everything queued for the ordinary stream, every destination, pool first.
 * Enough for a transport that runs one pipeline at a time, where the
 * destination does not matter. A transport running several pipelines needs
 * to know which bytes go to which command, and uses the per-destination
 * calls below instead. */
psrp_result_t psrp_session_take_output(psrp_session_t *s, psrp_buffer_t *out);

/* The bytes queued for one destination: the pool when `pipeline_id` is NULL,
 * else that pipeline; on the priority stream when `priority` is set. Returns
 * PSRP_ERR_NOT_FOUND when that queue is empty. */
psrp_result_t psrp_session_take_output_for(psrp_session_t *s,
                                           const psrp_guid_t *pipeline_id,
                                           bool priority, psrp_buffer_t *out);

/* Walks the destinations with something queued. Start with *cursor = 0 and
 * call until it returns false; each true fills in one destination whose
 * queue is non-empty. `pipeline_id` is the empty GUID for the pool. */
bool psrp_session_next_pending(const psrp_session_t *s, size_t *cursor,
                               psrp_guid_t *pipeline_id, bool *priority);

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
