/* [MS-PSRP] 2.2.1 message header, 2.2.2 message types.
 *
 * Byte order: 2.2 "All messages are little-endian, except where otherwise
 * specified." The fragment header is the documented exception (big-endian);
 * this header is little-endian.
 */

#include <string.h>

#include "psrp/psrp_message.h"

static const struct {
    uint32_t type;
    const char *name;
} kTypeNames[] = {
    { PSRP_MSG_SESSION_CAPABILITY,          "SESSION_CAPABILITY" },
    { PSRP_MSG_INIT_RUNSPACEPOOL,           "INIT_RUNSPACEPOOL" },
    { PSRP_MSG_PUBLIC_KEY,                  "PUBLIC_KEY" },
    { PSRP_MSG_ENCRYPTED_SESSION_KEY,       "ENCRYPTED_SESSION_KEY" },
    { PSRP_MSG_PUBLIC_KEY_REQUEST,          "PUBLIC_KEY_REQUEST" },
    { PSRP_MSG_CONNECT_RUNSPACEPOOL,        "CONNECT_RUNSPACEPOOL" },
    { PSRP_MSG_SET_MAX_RUNSPACES,           "SET_MAX_RUNSPACES" },
    { PSRP_MSG_SET_MIN_RUNSPACES,           "SET_MIN_RUNSPACES" },
    { PSRP_MSG_RUNSPACE_AVAILABILITY,       "RUNSPACE_AVAILABILITY" },
    { PSRP_MSG_RUNSPACEPOOL_STATE,          "RUNSPACEPOOL_STATE" },
    { PSRP_MSG_CREATE_PIPELINE,             "CREATE_PIPELINE" },
    { PSRP_MSG_GET_AVAILABLE_RUNSPACES,     "GET_AVAILABLE_RUNSPACES" },
    { PSRP_MSG_USER_EVENT,                  "USER_EVENT" },
    { PSRP_MSG_APPLICATION_PRIVATE_DATA,    "APPLICATION_PRIVATE_DATA" },
    { PSRP_MSG_GET_COMMAND_METADATA,        "GET_COMMAND_METADATA" },
    { PSRP_MSG_RUNSPACEPOOL_INIT_DATA,      "RUNSPACEPOOL_INIT_DATA" },
    { PSRP_MSG_RESET_RUNSPACE_STATE,        "RESET_RUNSPACE_STATE" },
    { PSRP_MSG_RUNSPACEPOOL_HOST_CALL,      "RUNSPACEPOOL_HOST_CALL" },
    { PSRP_MSG_RUNSPACEPOOL_HOST_RESPONSE,  "RUNSPACEPOOL_HOST_RESPONSE" },
    { PSRP_MSG_PIPELINE_INPUT,              "PIPELINE_INPUT" },
    { PSRP_MSG_END_OF_PIPELINE_INPUT,       "END_OF_PIPELINE_INPUT" },
    { PSRP_MSG_PIPELINE_OUTPUT,             "PIPELINE_OUTPUT" },
    { PSRP_MSG_ERROR_RECORD,                "ERROR_RECORD" },
    { PSRP_MSG_PIPELINE_STATE,              "PIPELINE_STATE" },
    { PSRP_MSG_DEBUG_RECORD,                "DEBUG_RECORD" },
    { PSRP_MSG_VERBOSE_RECORD,              "VERBOSE_RECORD" },
    { PSRP_MSG_WARNING_RECORD,              "WARNING_RECORD" },
    { PSRP_MSG_PROGRESS_RECORD,             "PROGRESS_RECORD" },
    { PSRP_MSG_INFORMATION_RECORD,          "INFORMATION_RECORD" },
    { PSRP_MSG_PIPELINE_HOST_CALL,          "PIPELINE_HOST_CALL" },
    { PSRP_MSG_PIPELINE_HOST_RESPONSE,      "PIPELINE_HOST_RESPONSE" }
};
#define KTYPE_COUNT (sizeof kTypeNames / sizeof kTypeNames[0])

const char *psrp_message_type_name(uint32_t type)
{
    size_t i;
    for (i = 0; i < KTYPE_COUNT; i++)
        if (kTypeNames[i].type == type) return kTypeNames[i].name;
    return "UNKNOWN";
}

bool psrp_message_type_known(uint32_t type)
{
    size_t i;
    for (i = 0; i < KTYPE_COUNT; i++)
        if (kTypeNames[i].type == type) return true;
    return false;
}

/* Pipeline-targeted types all live in the 0x0004xxxx block, per the
 * "Target: pipeline" rows of 2.2.2. */
bool psrp_message_type_is_pipeline(uint32_t type)
{
    return psrp_message_type_known(type) && (type & 0xFFFF0000u) == 0x00040000u;
}

psrp_result_t psrp_message_encode(psrp_buffer_t *out, const psrp_message_t *m)
{
    psrp_result_t rc;
    uint8_t guid[16];

    if (!out || !m) return PSRP_ERR_INVALID_ARG;
    if (m->destination != PSRP_DEST_CLIENT && m->destination != PSRP_DEST_SERVER)
        return PSRP_ERR_INVALID_ARG;
    if (m->data_len && !m->data) return PSRP_ERR_INVALID_ARG;

    rc = psrp_buffer_reserve(out, PSRP_MESSAGE_HEADER_SIZE + m->data_len);
    if (rc != PSRP_OK) return rc;

    rc = psrp_buffer_append_u32le(out, m->destination);
    if (rc != PSRP_OK) return rc;
    rc = psrp_buffer_append_u32le(out, m->type);
    if (rc != PSRP_OK) return rc;

    psrp_guid_to_wire(&m->rpid, guid);
    rc = psrp_buffer_append(out, guid, sizeof guid);
    if (rc != PSRP_OK) return rc;

    psrp_guid_to_wire(&m->pid, guid);
    rc = psrp_buffer_append(out, guid, sizeof guid);
    if (rc != PSRP_OK) return rc;

    if (m->data_len)
        return psrp_buffer_append(out, m->data, m->data_len);
    return PSRP_OK;
}

psrp_result_t psrp_message_decode(const void *buf, size_t len, psrp_message_t *out)
{
    psrp_reader_t r;
    psrp_result_t rc;
    uint32_t destination, type;
    uint8_t guid[16];

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (len && !buf) return PSRP_ERR_INVALID_ARG;
    if (len < PSRP_MESSAGE_HEADER_SIZE) return PSRP_ERR_TRUNCATED;

    psrp_reader_init(&r, buf, len);
    rc = psrp_read_u32le(&r, &destination);
    if (rc != PSRP_OK) return rc;
    rc = psrp_read_u32le(&r, &type);
    if (rc != PSRP_OK) return rc;

    if (destination != PSRP_DEST_CLIENT && destination != PSRP_DEST_SERVER)
        return PSRP_ERR_MALFORMED;

    out->destination = destination;
    /* Unknown MessageType values are preserved, not rejected: see the header. */
    out->type = type;

    rc = psrp_read_bytes(&r, guid, sizeof guid);
    if (rc != PSRP_OK) return rc;
    psrp_guid_from_wire(guid, &out->rpid);

    rc = psrp_read_bytes(&r, guid, sizeof guid);
    if (rc != PSRP_OK) return rc;
    psrp_guid_from_wire(guid, &out->pid);

    out->data_len = len - PSRP_MESSAGE_HEADER_SIZE;
    out->data = out->data_len ? (const uint8_t *)buf + PSRP_MESSAGE_HEADER_SIZE
                              : NULL;
    return PSRP_OK;
}

void psrp_message_xml(const psrp_message_t *m, const uint8_t **xml, size_t *xml_len)
{
    static const uint8_t kBom[3] = { 0xEF, 0xBB, 0xBF };
    const uint8_t *p;
    size_t n;

    if (!xml || !xml_len) return;
    if (!m) { *xml = NULL; *xml_len = 0; return; }

    p = m->data;
    n = m->data_len;
    if (p && n >= sizeof kBom && memcmp(p, kBom, sizeof kBom) == 0) {
        p += sizeof kBom;
        n -= sizeof kBom;
    }
    *xml = n ? p : NULL;
    *xml_len = n;
}
