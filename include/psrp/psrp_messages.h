/* psrp_messages.h - typed bodies for individual PSRP messages ([MS-PSRP] 2.2.2)
 * and the enumerations they carry (2.2.3).
 *
 * Each message's Data field is CLIXML (2.2.5). These helpers build and parse
 * that payload; framing it into a message header is psrp_message.h's job, and
 * splitting it into fragments is psrp_fragment.h's.
 */
#ifndef PSRP_MESSAGES_H
#define PSRP_MESSAGES_H

#include "psrp/psrp_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- 2.2.3.4 --- */

typedef enum psrp_runspace_pool_state {
    PSRP_RUNSPACE_BEFORE_OPEN = 0,
    PSRP_RUNSPACE_OPENING = 1,
    PSRP_RUNSPACE_OPENED = 2,
    PSRP_RUNSPACE_CLOSED = 3,
    PSRP_RUNSPACE_CLOSING = 4,
    PSRP_RUNSPACE_BROKEN = 5,
    PSRP_RUNSPACE_NEGOTIATION_SENT = 6,
    PSRP_RUNSPACE_NEGOTIATION_SUCCEEDED = 7,
    PSRP_RUNSPACE_CONNECTING = 8,
    PSRP_RUNSPACE_DISCONNECTED = 9
} psrp_runspace_pool_state_t;

/* ------------------------------------------------------------- 2.2.3.5 --- */

typedef enum psrp_invocation_state {
    PSRP_INVOCATION_NOT_STARTED = 0,
    PSRP_INVOCATION_RUNNING = 1,
    PSRP_INVOCATION_STOPPING = 2,
    PSRP_INVOCATION_STOPPED = 3,
    PSRP_INVOCATION_COMPLETED = 4,
    PSRP_INVOCATION_FAILED = 5,
    PSRP_INVOCATION_DISCONNECTED = 6
} psrp_invocation_state_t;

/* Symbolic names; "Unknown" for values outside the spec's tables. */
const char *psrp_runspace_pool_state_name(int32_t state);
const char *psrp_invocation_state_name(int32_t state);

/* True once a pool/pipeline can no longer progress, so a driver knows to stop
 * waiting. */
bool psrp_runspace_pool_state_is_terminal(int32_t state);
bool psrp_invocation_state_is_terminal(int32_t state);

/* ----------------------------------------- 2.2.2.1 SESSION_CAPABILITY ---- */

/* Versions are text because the spec models them as Version values and we do
 * not need to interpret them; the highest protocolversion both ends support
 * is negotiated by comparing these. */
typedef struct psrp_session_capability {
    char ps_version[32];             /* PSVersion */
    char protocol_version[32];       /* protocolversion */
    char serialization_version[32];  /* SerializationVersion */
} psrp_session_capability_t;

/* Fills in the versions this implementation offers. */
void psrp_session_capability_defaults(psrp_session_capability_t *out);

psrp_result_t psrp_build_session_capability(const psrp_session_capability_t *cap,
                                            psrp_buffer_t *out);
/* The optional TimeZone property is ignored; PSRP does not interpret it. */
psrp_result_t psrp_parse_session_capability(const void *xml, size_t n,
                                            psrp_session_capability_t *out);

/* ---------------------------------------- 2.2.2.9 RUNSPACEPOOL_STATE ----- */

typedef struct psrp_runspacepool_state_msg {
    int32_t state;                   /* psrp_runspace_pool_state_t */
    bool has_error;                  /* ExceptionAsErrorRecord present */
    char *error_text;                /* owned; the record's ToString, or NULL */
} psrp_runspacepool_state_msg_t;

psrp_result_t psrp_parse_runspacepool_state(const void *xml, size_t n,
                                            psrp_runspacepool_state_msg_t *out);
void psrp_runspacepool_state_msg_free(psrp_runspacepool_state_msg_t *m);

/* ------------------------------------------- 2.2.2.21 PIPELINE_STATE ----- */

typedef struct psrp_pipeline_state_msg {
    int32_t state;                   /* psrp_invocation_state_t */
    bool has_error;
    char *error_text;                /* owned; the record's ToString, or NULL */
} psrp_pipeline_state_msg_t;

psrp_result_t psrp_parse_pipeline_state(const void *xml, size_t n,
                                        psrp_pipeline_state_msg_t *out);
void psrp_pipeline_state_msg_free(psrp_pipeline_state_msg_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_MESSAGES_H */
