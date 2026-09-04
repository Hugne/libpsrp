/* Pipeline stream records ([MS-PSRP] 2.2.2.19-26, 2.2.3.15, 2.2.3.16,
 * 2.2.5.1.25). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_records.h"
#include "psrp/psrp_clixml.h"

const char *psrp_error_category_name(int32_t category)
{
    switch (category) {
    case 0:  return "NotSpecified";
    case 1:  return "OpenError";
    case 2:  return "CloseError";
    case 3:  return "DeviceError";
    case 4:  return "DeadlockDetected";
    case 5:  return "InvalidArgument";
    case 6:  return "InvalidData";
    case 7:  return "InvalidOperation";
    case 8:  return "InvalidResult";
    case 9:  return "InvalidType";
    case 10: return "MetadataError";
    case 11: return "NotImplemented";
    case 12: return "NotInstalled";
    case 13: return "ObjectNotFound";
    case 14: return "OperationStopped";
    case 15: return "OperationTimeout";
    case 16: return "SyntaxError";
    case 17: return "ParserError";
    case 18: return "PermissionDenied";
    case 19: return "ResourceBusy";
    case 20: return "ResourceExists";
    case 21: return "ResourceUnavailable";
    case 22: return "ReadError";
    /* 23 and 24 are not defined by the spec's table. */
    case 25: return "SecurityError";
    default: return "Unknown";
    }
}

/* ------------------------------------------------------------ helpers ---- */

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Copies a string-valued property, or NULL when absent or Null. Non-string
 * kinds are ignored rather than coerced, so a caller can trust the type. */
static char *text_prop(const psrp_object_t *o, const char *name)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v) return NULL;
    switch (v->kind) {
    case PSRP_VAL_STRING:
    case PSRP_VAL_VERSION:
    case PSRP_VAL_DATETIME:
    case PSRP_VAL_URI:
    case PSRP_VAL_DECIMAL:
    case PSRP_VAL_DURATION:
        return dup_n(v->as.text.ptr, v->as.text.len);
    default:
        return NULL;
    }
}

static int32_t i32_prop(const psrp_object_t *o, const char *name, int32_t missing)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v || v->kind != PSRP_VAL_INT32) return missing;
    return v->as.i32;
}

static bool bool_prop(const psrp_object_t *o, const char *name)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    return v && v->kind == PSRP_VAL_BOOL && v->as.b;
}

/* Parses the payload and requires a complex object at the root. */
static psrp_result_t parse_root_object(const void *xml, size_t n,
                                       psrp_value_t *root)
{
    psrp_result_t rc;
    psrp_value_init(root);
    rc = psrp_clixml_deserialize(xml, n, root);
    if (rc != PSRP_OK) return rc;
    if (root->kind != PSRP_VAL_OBJECT) {
        psrp_value_free(root);
        return PSRP_ERR_MALFORMED;
    }
    return PSRP_OK;
}

/* Copies an object's ToString. */
static char *to_string_of(const psrp_object_t *o)
{
    size_t len = 0;
    const char *ts = psrp_object_to_string(o, &len);
    return ts ? dup_n(ts, len) : NULL;
}

/* ------------------------------------------------------- ErrorRecord ---- */

psrp_result_t psrp_parse_error_record(const void *xml, size_t n,
                                      psrp_error_record_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->category = -1;

    rc = parse_root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    out->message = to_string_of(root.as.obj);
    out->fully_qualified_error_id =
        text_prop(root.as.obj, "FullyQualifiedErrorId");
    out->category_message = text_prop(root.as.obj, "ErrorCategory_Message");
    out->category_reason = text_prop(root.as.obj, "ErrorCategory_Reason");
    out->category_activity = text_prop(root.as.obj, "ErrorCategory_Activity");
    out->target_name = text_prop(root.as.obj, "ErrorCategory_TargetName");
    out->category = i32_prop(root.as.obj, "ErrorCategory_Category", -1);

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_error_record_free(psrp_error_record_t *r)
{
    if (!r) return;
    free(r->message);
    free(r->fully_qualified_error_id);
    free(r->category_message);
    free(r->category_reason);
    free(r->category_activity);
    free(r->target_name);
    memset(r, 0, sizeof *r);
    r->category = -1;
}

/* --------------------------------------------- informational records ---- */

psrp_result_t psrp_parse_informational_record(const void *xml, size_t n,
                                              psrp_informational_record_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    rc = parse_root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    out->message = text_prop(root.as.obj, "InformationalRecord_Message");
    out->has_invocation_info =
        bool_prop(root.as.obj, "InformationalRecord_SerializeInvocationInfo");

    /* Some servers send a bare string rather than the record object; fall back
     * to the ToString so the caller still gets the text. */
    if (!out->message) out->message = to_string_of(root.as.obj);

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_informational_record_free(psrp_informational_record_t *r)
{
    if (!r) return;
    free(r->message);
    memset(r, 0, sizeof *r);
}

/* ----------------------------------------------------- ProgressRecord --- */

psrp_result_t psrp_parse_progress_record(const void *xml, size_t n,
                                         psrp_progress_record_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->activity_id = -1;
    out->parent_activity_id = -1;
    out->percent_complete = -1;
    out->seconds_remaining = -1;

    rc = parse_root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    out->activity = text_prop(root.as.obj, "Activity");
    out->status_description = text_prop(root.as.obj, "StatusDescription");
    out->current_operation = text_prop(root.as.obj, "CurrentOperation");
    out->activity_id = i32_prop(root.as.obj, "ActivityId", -1);
    out->parent_activity_id = i32_prop(root.as.obj, "ParentActivityId", -1);
    out->percent_complete = i32_prop(root.as.obj, "PercentComplete", -1);
    out->seconds_remaining = i32_prop(root.as.obj, "SecondsRemaining", -1);

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_progress_record_free(psrp_progress_record_t *r)
{
    if (!r) return;
    free(r->activity);
    free(r->status_description);
    free(r->current_operation);
    memset(r, 0, sizeof *r);
}

/* -------------------------------------------------- InformationRecord --- */

psrp_result_t psrp_parse_information_record(const void *xml, size_t n,
                                            psrp_information_record_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    rc = parse_root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    /* psrp_object_find searches extended then adapted, so this works whether
     * the server used <MS> or <Props> (the spec's example uses <Props>). */
    out->message_data = text_prop(root.as.obj, "MessageData");
    out->source = text_prop(root.as.obj, "Source");
    out->time_generated = text_prop(root.as.obj, "TimeGenerated");
    if (!out->message_data) out->message_data = to_string_of(root.as.obj);

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_information_record_free(psrp_information_record_t *r)
{
    if (!r) return;
    free(r->message_data);
    free(r->source);
    free(r->time_generated);
    memset(r, 0, sizeof *r);
}

/* -------------------------------------------------------- pipeline I/O -- */

psrp_result_t psrp_parse_pipeline_output(const void *xml, size_t n,
                                         psrp_value_t *out)
{
    if (!out) return PSRP_ERR_INVALID_ARG;
    return psrp_clixml_deserialize(xml, n, out);
}

psrp_result_t psrp_build_pipeline_input(const psrp_value_t *v,
                                        psrp_buffer_t *out)
{
    return psrp_clixml_serialize(v, out);
}

psrp_result_t psrp_value_to_text(const psrp_value_t *v, psrp_buffer_t *out)
{
    char num[64];

    if (!v || !out) return PSRP_ERR_INVALID_ARG;

    switch (v->kind) {
    case PSRP_VAL_NULL:
        return PSRP_OK;                     /* renders as nothing */

    case PSRP_VAL_STRING:
    case PSRP_VAL_VERSION:
    case PSRP_VAL_DATETIME:
    case PSRP_VAL_DURATION:
    case PSRP_VAL_DECIMAL:
    case PSRP_VAL_URI:
    case PSRP_VAL_XMLDOC:
    case PSRP_VAL_SCRIPTBLOCK:
    case PSRP_VAL_SECURESTRING:
        return psrp_buffer_append(out, v->as.text.ptr, v->as.text.len);

    case PSRP_VAL_BOOL:
        /* PowerShell renders booleans as True/False. */
        return psrp_buffer_append_str(out, v->as.b ? "True" : "False");

    case PSRP_VAL_CHAR: {
        /* The value is a UTF-16 code unit; render the character itself. */
        uint16_t u = v->as.ch;
        if (u < 0x80) {
            uint8_t c = (uint8_t)u;
            return psrp_buffer_append(out, &c, 1);
        }
        snprintf(num, sizeof num, "%u", (unsigned)u);
        break;
    }

    case PSRP_VAL_UINT8:  snprintf(num, sizeof num, "%u", (unsigned)v->as.u8); break;
    case PSRP_VAL_INT8:   snprintf(num, sizeof num, "%d", (int)v->as.i8); break;
    case PSRP_VAL_UINT16: snprintf(num, sizeof num, "%u", (unsigned)v->as.u16); break;
    case PSRP_VAL_INT16:  snprintf(num, sizeof num, "%d", (int)v->as.i16); break;
    case PSRP_VAL_UINT32: snprintf(num, sizeof num, "%lu", (unsigned long)v->as.u32); break;
    case PSRP_VAL_INT32:  snprintf(num, sizeof num, "%ld", (long)v->as.i32); break;
    case PSRP_VAL_UINT64: snprintf(num, sizeof num, "%llu", (unsigned long long)v->as.u64); break;
    case PSRP_VAL_INT64:  snprintf(num, sizeof num, "%lld", (long long)v->as.i64); break;
    case PSRP_VAL_SINGLE: snprintf(num, sizeof num, "%g", (double)v->as.f32); break;
    case PSRP_VAL_DOUBLE: snprintf(num, sizeof num, "%g", v->as.f64); break;

    case PSRP_VAL_GUID: {
        char guid[PSRP_GUID_BUF_SIZE];
        psrp_result_t rc = psrp_guid_format(&v->as.guid, guid, sizeof guid);
        if (rc != PSRP_OK) return rc;
        return psrp_buffer_append_str(out, guid);
    }

    case PSRP_VAL_BYTES:
        /* No sensible text form; callers wanting the bytes have them. */
        return PSRP_OK;

    case PSRP_VAL_OBJECT: {
        size_t len = 0;
        const char *ts = psrp_object_to_string(v->as.obj, &len);
        const psrp_value_t *prim;
        if (ts) return psrp_buffer_append(out, ts, len);
        /* An extended primitive renders as its underlying value. */
        prim = psrp_object_primitive(v->as.obj);
        if (prim) return psrp_value_to_text(prim, out);
        return PSRP_OK;
    }

    default:
        return PSRP_ERR_UNSUPPORTED;
    }

    return psrp_buffer_append_str(out, num);
}
