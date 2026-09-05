/** @file
 * psrp_message.h - [MS-PSRP] 2.2.1 PowerShell Remoting Protocol Message
 *                  and 2.2.2 Message Types.
 *
 * Wire layout (40-byte header, then the payload):
 *
 *   Destination  4 bytes   1 = to client, 2 = to server
 *   MessageType  4 bytes   see psrp_message_type_t
 *   RPID        16 bytes   RunspacePool instance id (GUID)
 *   PID         16 bytes   pipeline instance id (GUID), zero when not a pipeline
 *   Data        variable   UTF-8 encoded XML (CLIXML), per 2.2.2
 *
 * Byte order: [MS-PSRP] 2.2 states "All messages are little-endian, except
 * where otherwise specified." The fragment header (2.2.4) is the documented
 * exception and is big-endian; this header is little-endian. GUIDs use the
 * .NET field layout (see psrp_guid_to_wire).
 */
#ifndef PSRP_MESSAGE_H
#define PSRP_MESSAGE_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSRP_MESSAGE_HEADER_SIZE 40u

typedef enum psrp_destination {
    PSRP_DEST_CLIENT = 0x00000001u,
    PSRP_DEST_SERVER = 0x00000002u
} psrp_destination_t;

/** All 31 message types from [MS-PSRP] 2.2.2. */
typedef enum psrp_message_type {
    /** Session / RunspacePool setup */
    PSRP_MSG_SESSION_CAPABILITY       = 0x00010002u,
    PSRP_MSG_INIT_RUNSPACEPOOL        = 0x00010004u,
    PSRP_MSG_PUBLIC_KEY               = 0x00010005u,
    PSRP_MSG_ENCRYPTED_SESSION_KEY    = 0x00010006u,
    PSRP_MSG_PUBLIC_KEY_REQUEST       = 0x00010007u,
    PSRP_MSG_CONNECT_RUNSPACEPOOL     = 0x00010008u,

    /** RunspacePool */
    PSRP_MSG_SET_MAX_RUNSPACES        = 0x00021002u,
    PSRP_MSG_SET_MIN_RUNSPACES        = 0x00021003u,
    PSRP_MSG_RUNSPACE_AVAILABILITY    = 0x00021004u,
    PSRP_MSG_RUNSPACEPOOL_STATE       = 0x00021005u,
    PSRP_MSG_CREATE_PIPELINE          = 0x00021006u,
    PSRP_MSG_GET_AVAILABLE_RUNSPACES  = 0x00021007u,
    PSRP_MSG_USER_EVENT               = 0x00021008u,
    PSRP_MSG_APPLICATION_PRIVATE_DATA = 0x00021009u,
    PSRP_MSG_GET_COMMAND_METADATA     = 0x0002100Au,
    PSRP_MSG_RUNSPACEPOOL_INIT_DATA   = 0x0002100Bu,
    PSRP_MSG_RESET_RUNSPACE_STATE     = 0x0002100Cu,
    PSRP_MSG_RUNSPACEPOOL_HOST_CALL     = 0x00021100u,
    PSRP_MSG_RUNSPACEPOOL_HOST_RESPONSE = 0x00021101u,

    /** Pipeline */
    PSRP_MSG_PIPELINE_INPUT           = 0x00041002u,
    PSRP_MSG_END_OF_PIPELINE_INPUT    = 0x00041003u,
    PSRP_MSG_PIPELINE_OUTPUT          = 0x00041004u,
    PSRP_MSG_ERROR_RECORD             = 0x00041005u,
    PSRP_MSG_PIPELINE_STATE           = 0x00041006u,
    PSRP_MSG_DEBUG_RECORD             = 0x00041007u,
    PSRP_MSG_VERBOSE_RECORD           = 0x00041008u,
    PSRP_MSG_WARNING_RECORD           = 0x00041009u,
    /** Note the jump: PROGRESS/INFORMATION are 0x...10 and 0x...11, not 0x0A/0x0B. */
    PSRP_MSG_PROGRESS_RECORD          = 0x00041010u,
    PSRP_MSG_INFORMATION_RECORD       = 0x00041011u,
    PSRP_MSG_PIPELINE_HOST_CALL       = 0x00041100u,
    PSRP_MSG_PIPELINE_HOST_RESPONSE   = 0x00041101u
} psrp_message_type_t;

typedef struct psrp_message {
    uint32_t destination;          /**< psrp_destination_t */
    uint32_t type;                 /**< psrp_message_type_t; may be unknown */
    psrp_guid_t rpid;
    psrp_guid_t pid;
    const uint8_t *data;           /**< borrowed on decode; UTF-8 XML */
    size_t data_len;
} psrp_message_t;

/** Appends the encoded message to `out`. No UTF-8 BOM is emitted. */
psrp_result_t psrp_message_encode(psrp_buffer_t *out, const psrp_message_t *m);

/** Decodes a complete message. `out->data` borrows from `buf`.
 * Returns PSRP_ERR_TRUNCATED if fewer than 40 bytes are supplied, and
 * PSRP_ERR_MALFORMED if Destination is neither 1 nor 2.
 *
 * An unrecognised MessageType is deliberately NOT an error: it is preserved
 * so the state machine can decide (log, ignore, or fail). Rejecting here
 * would make us brittle against protocol additions. */
psrp_result_t psrp_message_decode(const void *buf, size_t len, psrp_message_t *out);

/** The Data field with any leading UTF-8 BOM removed. PowerShell emits a BOM on
 * some messages; we tolerate it on read and never write one. */
void psrp_message_xml(const psrp_message_t *m, const uint8_t **xml, size_t *xml_len);

/** Symbolic name, e.g. "SESSION_CAPABILITY"; "UNKNOWN" if unrecognised. */
const char *psrp_message_type_name(uint32_t type);

/** True if `type` is one of the 31 types defined in 2.2.2. */
bool psrp_message_type_known(uint32_t type);

/** True if the message targets a pipeline (PID is meaningful). */
bool psrp_message_type_is_pipeline(uint32_t type);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_MESSAGE_H */
