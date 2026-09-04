/* InvocationInfo-specific extended properties ([MS-PSRP] 2.2.3.15.1).
 *
 * These describe the higher-layer command that produced an error or
 * informational record. The spec is explicit that a PSRP implementation MUST
 * NOT interpret them, so everything here reads and hands over; nothing acts on
 * what it finds.
 *
 * Every property is optional. PowerShell fills them in only when the record
 * was asked to serialize its invocation info, so a record with none of them is
 * ordinary rather than broken.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_records.h"
#include "psrp/psrp_clixml.h"

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *text_prop(const psrp_object_t *o, const char *name)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v) return NULL;
    if (v->kind != PSRP_VAL_STRING) return NULL;   /* Null means no value */
    return dup_n(v->as.text.ptr, v->as.text.len);
}

/* Reads a Signed Int, seeing through the enum wrapper an object-valued
 * property uses. CommandOrigin arrives that way; the plain counts do not. */
static int32_t i32_prop(const psrp_object_t *o, const char *name, int32_t missing)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v) return missing;
    if (v->kind == PSRP_VAL_INT32) return v->as.i32;
    if (v->kind == PSRP_VAL_OBJECT) {
        const psrp_value_t *prim = psrp_object_primitive(v->as.obj);
        if (prim && prim->kind == PSRP_VAL_INT32) return prim->as.i32;
    }
    return missing;
}

static int64_t i64_prop(const psrp_object_t *o, const char *name, int64_t missing)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v) return missing;
    if (v->kind == PSRP_VAL_INT64) return v->as.i64;
    /* A history id small enough to fit an I32 is sometimes narrowed. */
    if (v->kind == PSRP_VAL_INT32) return v->as.i32;
    return missing;
}

/* Copies a property that holds an arbitrary object graph. */
static void keep_value(const psrp_object_t *o, const char *name,
                       psrp_value_t *dst)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    psrp_value_init(dst);
    if (!v || v->kind == PSRP_VAL_NULL) return;
    if (psrp_value_clone(v, dst) != PSRP_OK) psrp_value_init(dst);
}

psrp_result_t psrp_parse_invocation_info(const void *xml, size_t n,
                                         psrp_invocation_info_t *out)
{
    psrp_value_t root;
    const psrp_value_t *flag;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->command_origin = -1;
    out->offset_in_line = -1;
    out->script_line_number = -1;
    out->pipeline_length = -1;
    out->pipeline_position = -1;
    out->history_id = -1;
    psrp_value_init(&out->bound_parameters);
    psrp_value_init(&out->unbound_arguments);
    psrp_value_init(&out->pipeline_iteration_info);

    psrp_value_init(&root);
    rc = psrp_clixml_deserialize(xml, n, &root);
    if (rc != PSRP_OK) return rc;
    if (root.kind != PSRP_VAL_OBJECT) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    out->invocation_name = text_prop(root.as.obj, "InvocationInfo_InvocationName");
    out->line = text_prop(root.as.obj, "InvocationInfo_Line");
    out->position_message = text_prop(root.as.obj, "InvocationInfo_PositionMessage");
    out->script_name = text_prop(root.as.obj, "InvocationInfo_ScriptName");

    out->command_origin = i32_prop(root.as.obj, "InvocationInfo_CommandOrigin", -1);
    out->offset_in_line = i32_prop(root.as.obj, "InvocationInfo_OffsetInLine", -1);
    out->script_line_number =
        i32_prop(root.as.obj, "InvocationInfo_ScriptLineNumber", -1);
    out->pipeline_length = i32_prop(root.as.obj, "InvocationInfo_PipelineLength", -1);
    out->pipeline_position =
        i32_prop(root.as.obj, "InvocationInfo_PipelinePosition", -1);
    out->history_id = i64_prop(root.as.obj, "InvocationInfo_HistoryId", -1);

    /* Absent and false are different answers to "was it expecting input", so
     * the flag is tracked separately rather than folded into the value. */
    flag = psrp_object_find(root.as.obj, "InvocationInfo_ExpectingInput");
    if (flag && flag->kind == PSRP_VAL_BOOL) {
        out->has_expecting_input = true;
        out->expecting_input = flag->as.b;
    }

    keep_value(root.as.obj, "InvocationInfo_BoundParameters",
               &out->bound_parameters);
    keep_value(root.as.obj, "InvocationInfo_UnboundArguments",
               &out->unbound_arguments);
    keep_value(root.as.obj, "InvocationInfo_PipelineIterationInfo",
               &out->pipeline_iteration_info);

    psrp_value_free(&root);
    return PSRP_OK;
}

bool psrp_invocation_info_present(const psrp_invocation_info_t *i)
{
    if (!i) return false;
    return i->invocation_name || i->line || i->position_message ||
           i->script_name || i->has_expecting_input ||
           i->command_origin >= 0 || i->offset_in_line >= 0 ||
           i->script_line_number >= 0 || i->pipeline_length >= 0 ||
           i->pipeline_position >= 0 || i->history_id >= 0 ||
           i->bound_parameters.kind != PSRP_VAL_NULL ||
           i->unbound_arguments.kind != PSRP_VAL_NULL ||
           i->pipeline_iteration_info.kind != PSRP_VAL_NULL;
}

void psrp_invocation_info_free(psrp_invocation_info_t *i)
{
    if (!i) return;
    free(i->invocation_name);
    free(i->line);
    free(i->position_message);
    free(i->script_name);
    psrp_value_free(&i->bound_parameters);
    psrp_value_free(&i->unbound_arguments);
    psrp_value_free(&i->pipeline_iteration_info);
    memset(i, 0, sizeof *i);
    i->command_origin = -1;
    i->offset_in_line = -1;
    i->script_line_number = -1;
    i->pipeline_length = -1;
    i->pipeline_position = -1;
    i->history_id = -1;
}
