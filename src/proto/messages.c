/* Typed PSRP message bodies ([MS-PSRP] 2.2.2) over the CLIXML layer. */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"

const char *psrp_runspace_pool_state_name(int32_t state)
{
    switch (state) {
    case PSRP_RUNSPACE_BEFORE_OPEN:            return "BeforeOpen";
    case PSRP_RUNSPACE_OPENING:                return "Opening";
    case PSRP_RUNSPACE_OPENED:                 return "Opened";
    case PSRP_RUNSPACE_CLOSED:                 return "Closed";
    case PSRP_RUNSPACE_CLOSING:                return "Closing";
    case PSRP_RUNSPACE_BROKEN:                 return "Broken";
    case PSRP_RUNSPACE_NEGOTIATION_SENT:       return "NegotiationSent";
    case PSRP_RUNSPACE_NEGOTIATION_SUCCEEDED:  return "NegotiationSucceeded";
    case PSRP_RUNSPACE_CONNECTING:             return "Connecting";
    case PSRP_RUNSPACE_DISCONNECTED:           return "Disconnected";
    default:                                   return "Unknown";
    }
}

const char *psrp_invocation_state_name(int32_t state)
{
    switch (state) {
    case PSRP_INVOCATION_NOT_STARTED:   return "NotStarted";
    case PSRP_INVOCATION_RUNNING:       return "Running";
    case PSRP_INVOCATION_STOPPING:      return "Stopping";
    case PSRP_INVOCATION_STOPPED:       return "Stopped";
    case PSRP_INVOCATION_COMPLETED:     return "Completed";
    case PSRP_INVOCATION_FAILED:        return "Failed";
    case PSRP_INVOCATION_DISCONNECTED:  return "Disconnected";
    default:                            return "Unknown";
    }
}

bool psrp_runspace_pool_state_is_terminal(int32_t state)
{
    return state == PSRP_RUNSPACE_CLOSED || state == PSRP_RUNSPACE_BROKEN;
}

bool psrp_invocation_state_is_terminal(int32_t state)
{
    return state == PSRP_INVOCATION_STOPPED ||
           state == PSRP_INVOCATION_COMPLETED ||
           state == PSRP_INVOCATION_FAILED;
}

/* ------------------------------------------------------------ helpers ---- */

static psrp_result_t add_version(psrp_object_t *o, const char *name,
                                 const char *text)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_VERSION, text, strlen(text));
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

/* Copies a text-valued property into a fixed buffer. Missing is not an error
 * here; the caller decides which properties are mandatory. */
static void copy_text_prop(const psrp_object_t *o, const char *name,
                           char *dst, size_t dst_size)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    size_t n;
    dst[0] = '\0';
    if (!v) return;
    if (v->kind != PSRP_VAL_VERSION && v->kind != PSRP_VAL_STRING) return;
    n = v->as.text.len;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, v->as.text.ptr, n);
    dst[n] = '\0';
}

/* Pulls the human-readable text out of an ErrorRecord (2.2.3.15). The record
 * is a complex object whose ToString carries the message. */
static char *error_text_of(const psrp_value_t *v)
{
    size_t len = 0;
    const char *ts;
    char *copy;
    if (!v || v->kind != PSRP_VAL_OBJECT) return NULL;
    ts = psrp_object_to_string(v->as.obj, &len);
    if (!ts) return NULL;
    copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, ts, len);
    copy[len] = '\0';
    return copy;
}

/* Both state messages share a shape: an I32 state plus an optional
 * ExceptionAsErrorRecord. */
static psrp_result_t parse_state_message(const void *xml, size_t n,
                                         const char *state_prop,
                                         int32_t *state_out,
                                         bool *has_error, char **error_text)
{
    psrp_value_t root;
    const psrp_value_t *sv;
    psrp_result_t rc;

    psrp_value_init(&root);
    rc = psrp_clixml_deserialize(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    if (root.kind != PSRP_VAL_OBJECT) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    sv = psrp_object_find(root.as.obj, state_prop);
    if (!sv || sv->kind != PSRP_VAL_INT32) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    *state_out = sv->as.i32;

    {
        const psrp_value_t *err =
            psrp_object_find(root.as.obj, "ExceptionAsErrorRecord");
        *has_error = err != NULL;
        *error_text = err ? error_text_of(err) : NULL;
    }

    psrp_value_free(&root);
    return PSRP_OK;
}

/* -------------------------------------------------- SESSION_CAPABILITY --- */

void psrp_session_capability_defaults(psrp_session_capability_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    /* 2.2.2.1 / 3.1.5.3.1. 2.2 is the version PowerShell 3.0+ speaks; the
     * serialization version has been 1.1.0.1 since the protocol shipped. */
    memcpy(out->ps_version, "2.0", 4);
    memcpy(out->protocol_version, "2.2", 4);
    memcpy(out->serialization_version, "1.1.0.1", 8);
}

psrp_result_t psrp_build_session_capability(const psrp_session_capability_t *cap,
                                            psrp_buffer_t *out)
{
    psrp_object_t *o;
    psrp_value_t wrapper;
    psrp_result_t rc;

    if (!cap || !out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);

    /* Property order follows the spec's own example. */
    rc = add_version(o, "protocolversion", cap->protocol_version);
    if (rc == PSRP_OK) rc = add_version(o, "PSVersion", cap->ps_version);
    if (rc == PSRP_OK)
        rc = add_version(o, "SerializationVersion", cap->serialization_version);
    /* TimeZone is a byte array holding the MS-NRBF graph (2.2.3.10). The spec
     * says SHOULD, so an absent one is simply left out rather than sent as
     * Null: the server treats a missing property and a null one differently. */
    if (rc == PSRP_OK && cap->timezone_blob && cap->timezone_len) {
        psrp_value_t tz;
        psrp_value_init(&tz);
        rc = psrp_value_set_bytes(&tz, cap->timezone_blob, cap->timezone_len);
        if (rc == PSRP_OK) rc = psrp_object_add_extended(o, "TimeZone", &tz);
        psrp_value_free(&tz);
    }
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, o);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

psrp_result_t psrp_parse_session_capability(const void *xml, size_t n,
                                            psrp_session_capability_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    psrp_value_init(&root);
    rc = psrp_clixml_deserialize(xml, n, &root);
    if (rc != PSRP_OK) return rc;
    if (root.kind != PSRP_VAL_OBJECT) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    copy_text_prop(root.as.obj, "PSVersion", out->ps_version,
                   sizeof out->ps_version);
    copy_text_prop(root.as.obj, "protocolversion", out->protocol_version,
                   sizeof out->protocol_version);
    copy_text_prop(root.as.obj, "SerializationVersion",
                   out->serialization_version,
                   sizeof out->serialization_version);
    psrp_value_free(&root);

    /* protocolversion is what drives negotiation, so its absence is fatal
     * while the others are merely informational. */
    if (out->protocol_version[0] == '\0') return PSRP_ERR_MALFORMED;
    return PSRP_OK;
}

/* ------------------------------------------------- state messages -------- */

psrp_result_t psrp_parse_runspacepool_state(const void *xml, size_t n,
                                            psrp_runspacepool_state_msg_t *out)
{
    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    return parse_state_message(xml, n, "RunspaceState", &out->state,
                               &out->has_error, &out->error_text);
}

void psrp_runspacepool_state_msg_free(psrp_runspacepool_state_msg_t *m)
{
    if (!m) return;
    free(m->error_text);
    memset(m, 0, sizeof *m);
}

psrp_result_t psrp_parse_pipeline_state(const void *xml, size_t n,
                                        psrp_pipeline_state_msg_t *out)
{
    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    return parse_state_message(xml, n, "PipelineState", &out->state,
                               &out->has_error, &out->error_text);
}

void psrp_pipeline_state_msg_free(psrp_pipeline_state_msg_t *m)
{
    if (!m) return;
    free(m->error_text);
    memset(m, 0, sizeof *m);
}
