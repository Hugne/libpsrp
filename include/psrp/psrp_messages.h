/** @file
 * psrp_messages.h - typed bodies for individual PSRP messages ([MS-PSRP] 2.2.2)
 * and the enumerations they carry (2.2.3).
 *
 * Each message's Data field is CLIXML (2.2.5). These helpers build and parse
 * that payload; framing it into a message header is psrp_message.h's job, and
 * splitting it into fragments is psrp_fragment.h's.
 */
#ifndef PSRP_MESSAGES_H
#define PSRP_MESSAGES_H

#include "psrp/psrp_object.h"
#include "psrp/psrp_timezone.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ------------------------------------------------------------- 2.2.3.4 --- */

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

/** ------------------------------------------------------------- 2.2.3.5 --- */

typedef enum psrp_invocation_state {
    PSRP_INVOCATION_NOT_STARTED = 0,
    PSRP_INVOCATION_RUNNING = 1,
    PSRP_INVOCATION_STOPPING = 2,
    PSRP_INVOCATION_STOPPED = 3,
    PSRP_INVOCATION_COMPLETED = 4,
    PSRP_INVOCATION_FAILED = 5,
    PSRP_INVOCATION_DISCONNECTED = 6
} psrp_invocation_state_t;

/** Symbolic names; "Unknown" for values outside the spec's tables. */
const char *psrp_runspace_pool_state_name(int32_t state);
const char *psrp_invocation_state_name(int32_t state);

/** True once a pool/pipeline can no longer progress, so a driver knows to stop
 * waiting. */
bool psrp_runspace_pool_state_is_terminal(int32_t state);
bool psrp_invocation_state_is_terminal(int32_t state);

/* ----------------------------------------- 2.2.2.1 SESSION_CAPABILITY ---- */

/** Versions are text because the spec models them as Version values and we do
 * not need to interpret them; the highest protocolversion both ends support
 * is negotiated by comparing these. */
typedef struct psrp_session_capability {
    char ps_version[32];             /**< PSVersion */
    char protocol_version[32];       /**< protocolversion */
    char serialization_version[32];  /**< SerializationVersion */
    /** Optional TimeZone (2.2.3.10), an MS-NRBF blob. The spec says SHOULD, and
     * omitting it is what a client with no time zone to report does. Not
     * owned; psrp_session_capability_defaults leaves it empty. */
    const uint8_t *timezone_blob;
    size_t timezone_len;
} psrp_session_capability_t;

/** Fills in the versions this implementation offers. */
void psrp_session_capability_defaults(psrp_session_capability_t *out);

psrp_result_t psrp_build_session_capability(const psrp_session_capability_t *cap,
                                            psrp_buffer_t *out);
/** The optional TimeZone property is ignored; PSRP does not interpret it. */
psrp_result_t psrp_parse_session_capability(const void *xml, size_t n,
                                            psrp_session_capability_t *out);

/** ---------------------------------------- 2.2.2.9 RUNSPACEPOOL_STATE ----- */

typedef struct psrp_runspacepool_state_msg {
    int32_t state;                   /**< psrp_runspace_pool_state_t */
    bool has_error;                  /**< ExceptionAsErrorRecord present */
    char *error_text;                /**< owned; the record's ToString, or NULL */
} psrp_runspacepool_state_msg_t;

psrp_result_t psrp_parse_runspacepool_state(const void *xml, size_t n,
                                            psrp_runspacepool_state_msg_t *out);
void psrp_runspacepool_state_msg_free(psrp_runspacepool_state_msg_t *m);

/** ------------------------------------------- 2.2.2.21 PIPELINE_STATE ----- */

typedef struct psrp_pipeline_state_msg {
    int32_t state;                   /**< psrp_invocation_state_t */
    bool has_error;
    char *error_text;                /**< owned; the record's ToString, or NULL */
} psrp_pipeline_state_msg_t;

psrp_result_t psrp_parse_pipeline_state(const void *xml, size_t n,
                                        psrp_pipeline_state_msg_t *out);
void psrp_pipeline_state_msg_free(psrp_pipeline_state_msg_t *m);

/* ------------------------------------------------- 2.2.3.6 / 2.2.3.7 ----- */

/** PSRP does not interpret either of these; they are passed to the higher
 * layer on the server. */
typedef enum psrp_thread_options {
    PSRP_THREAD_DEFAULT = 0,
    PSRP_THREAD_USE_NEW_THREAD = 1,
    PSRP_THREAD_REUSE_THREAD = 2,
    PSRP_THREAD_USE_CURRENT_THREAD = 3
} psrp_thread_options_t;

typedef enum psrp_apartment_state {
    PSRP_APARTMENT_STA = 0,
    PSRP_APARTMENT_MTA = 1,
    PSRP_APARTMENT_UNKNOWN = 2
} psrp_apartment_state_t;

/** 2.2.3.8 RemoteStreamOptions: bit flags in a signed int. */
#define PSRP_STREAM_OPT_INVOCATION_INFO_TO_ERROR   0x01
#define PSRP_STREAM_OPT_INVOCATION_INFO_TO_WARNING 0x02
#define PSRP_STREAM_OPT_INVOCATION_INFO_TO_DEBUG   0x04
#define PSRP_STREAM_OPT_INVOCATION_INFO_TO_VERBOSE 0x08

/** 2.2.3.31 PipelineResultTypes: bit flags in a signed int. */
#define PSRP_RESULT_NONE    0x00
#define PSRP_RESULT_OUTPUT  0x01
#define PSRP_RESULT_ERROR   0x02
#define PSRP_RESULT_WARNING 0x04
#define PSRP_RESULT_VERBOSE 0x08
#define PSRP_RESULT_DEBUG   0x10
#define PSRP_RESULT_ALL     0x20

/* -------------------------------------------------- 2.2.3.14 HostInfo ---- */

/** Note the wire property names carry a leading underscore (_isHostNull and
 * friends) even though the spec's prose omits it; every example in the spec
 * uses the underscored form. */
typedef struct psrp_host_default_data psrp_host_default_data_t;

typedef struct psrp_host_info {
    bool is_host_null;
    bool is_host_ui_null;
    bool is_host_raw_ui_null;
    bool use_runspace_host;
    /** Optional console data (_hostDefaultData). NULL, the default, omits the
     * dictionary entirely, which is the shape the spec shows for a null host.
     * Point it at a filled-in struct to advertise a real console. Not owned. */
    const psrp_host_default_data_t *default_data;
} psrp_host_info_t;

/** A host that implements nothing. The spec shows exactly this shape, with the
 * four flags true and no _hostDefaultData, so a client with no console to
 * offer can say so honestly. See TODO PSRP-07 for the populated form. */
void psrp_host_info_null(psrp_host_info_t *out);

/** --------------------------------------------- 2.2.2.2 INIT_RUNSPACEPOOL - */

typedef struct psrp_init_runspacepool {
    int32_t min_runspaces;
    int32_t max_runspaces;
    int32_t thread_options;    /**< psrp_thread_options_t */
    int32_t apartment_state;   /**< psrp_apartment_state_t */
    psrp_host_info_t host;
    /** Optional ApplicationArguments (2.2.3.18). NULL sends Null, which the
     * spec explicitly permits. Not owned. */
    const psrp_value_t *application_arguments;
} psrp_init_runspacepool_t;

/** One runspace, default threading, unknown apartment, null host. */
void psrp_init_runspacepool_defaults(psrp_init_runspacepool_t *out);

psrp_result_t psrp_build_init_runspacepool(const psrp_init_runspacepool_t *init,
                                           psrp_buffer_t *out);

/** ---------------------------------- 2.2.3.12/2.2.3.13 Command + params --- */

typedef struct psrp_command psrp_command_t;

/** `cmd` is a command name or, when is_script is true, script text. */
psrp_command_t *psrp_command_new(const char *cmd, bool is_script);
void psrp_command_free(psrp_command_t *c);

/** Appends a parameter. `name` may be NULL for a positional argument, which
 * serializes as `<Nil N="N" />` per 2.2.3.13. Takes ownership of *value. */
psrp_result_t psrp_command_add_parameter(psrp_command_t *c, const char *name,
                                         psrp_value_t *value);
/** Convenience for a string-valued parameter. */
psrp_result_t psrp_command_add_string_parameter(psrp_command_t *c,
                                                const char *name,
                                                const char *value);

/** ---------------------------------------------- 2.2.2.10 CREATE_PIPELINE - */

typedef struct psrp_create_pipeline {
    bool no_input;             /**< NoInput */
    bool add_to_history;       /**< AddToHistory */
    bool is_nested;            /**< IsNested */
    int32_t apartment_state;   /**< ApartmentState */
    int32_t remote_stream_options;  /**< RemoteStreamOptions bit flags */
    psrp_host_info_t host;     /**< HostInfo */
} psrp_create_pipeline_t;

/** No input, not added to history, not nested, unknown apartment, null host. */
void psrp_create_pipeline_defaults(psrp_create_pipeline_t *out);

/** Serializes the CREATE_PIPELINE payload for `count` commands run as one
 * pipeline, in order. */
psrp_result_t psrp_build_create_pipeline(const psrp_create_pipeline_t *opts,
                                         psrp_command_t *const *commands,
                                         size_t count, psrp_buffer_t *out);

/* ------------------------------- RunspacePool control messages ---------- */
/*
 * The `ci` (call id) property correlates a request with the
 * RUNSPACE_AVAILABILITY that answers it. The caller chooses the values; the
 * protocol only requires that a response can be matched to its request.
 */

/** 2.2.2.6 SET_MAX_RUNSPACES / 2.2.2.7 SET_MIN_RUNSPACES */
psrp_result_t psrp_build_set_max_runspaces(int64_t ci, int32_t max_runspaces,
                                           psrp_buffer_t *out);
psrp_result_t psrp_build_set_min_runspaces(int64_t ci, int32_t min_runspaces,
                                           psrp_buffer_t *out);
/** 2.2.2.11 GET_AVAILABLE_RUNSPACES */
psrp_result_t psrp_build_get_available_runspaces(int64_t ci, psrp_buffer_t *out);
/** 2.2.2.31 RESET_RUNSPACE_STATE (protocol version 2.3 and later) */
psrp_result_t psrp_build_reset_runspace_state(int64_t ci, psrp_buffer_t *out);
/** 2.2.2.29 CONNECT_RUNSPACEPOOL (protocol version 2.2 and later).
 * Both bounds are optional; pass a negative value to omit one, or omit both
 * for the spec's "empty" form meaning a single runspace. */
psrp_result_t psrp_build_connect_runspacepool(int32_t min_runspaces,
                                              int32_t max_runspaces,
                                              psrp_buffer_t *out);

/** 2.2.2.8 RUNSPACE_AVAILABILITY. The response is a Boolean when answering a
 * set-min/max request and a Signed Long when answering get-available, so both
 * shapes are reported and `is_count` says which arrived. */
typedef struct psrp_runspace_availability {
    int64_t ci;
    bool is_count;        /**< true: `count` is valid; false: `accepted` is */
    bool accepted;
    int64_t count;
} psrp_runspace_availability_t;

psrp_result_t psrp_parse_runspace_availability(
    const void *xml, size_t n, psrp_runspace_availability_t *out);

/** 2.2.2.30 RUNSPACEPOOL_INIT_DATA. Both properties are optional; absent
 * bounds are reported as -1 rather than 0, which is a legal runspace count. */
typedef struct psrp_runspacepool_init_data {
    int32_t min_runspaces;
    int32_t max_runspaces;
} psrp_runspacepool_init_data_t;

psrp_result_t psrp_parse_runspacepool_init_data(
    const void *xml, size_t n, psrp_runspacepool_init_data_t *out);

/* ------------------------ 2.2.3.18 Primitive Dictionary ---------------- */
/*
 * A dictionary restricted to string keys and primitive values (or lists of
 * primitives, or nested primitive dictionaries). ApplicationArguments in
 * INIT_RUNSPACEPOOL is one, which is why it is here.
 *
 * It carries the type names the spec lists, which is how the far side knows
 * to reconstruct a PSPrimitiveDictionary rather than a plain Hashtable.
 */

/** Creates an empty primitive dictionary value. */
psrp_result_t psrp_primitive_dictionary_new(psrp_value_t *out);

/** Adds one entry, taking ownership of *value on success. ScriptBlock and
 * SecureString values are refused: 2.2.3.18 excludes both. */
psrp_result_t psrp_primitive_dictionary_add(psrp_value_t *dict,
                                            const char *key,
                                            psrp_value_t *value);

/** Convenience for the common string case. */
psrp_result_t psrp_primitive_dictionary_add_string(psrp_value_t *dict,
                                                   const char *key,
                                                   const char *value);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_MESSAGES_H */
