/* RunspacePool control messages: SET_MAX_RUNSPACES (2.2.2.6),
 * SET_MIN_RUNSPACES (2.2.2.7), RUNSPACE_AVAILABILITY (2.2.2.8),
 * GET_AVAILABLE_RUNSPACES (2.2.2.11), CONNECT_RUNSPACEPOOL (2.2.2.29),
 * RUNSPACEPOOL_INIT_DATA (2.2.2.30) and RESET_RUNSPACE_STATE (2.2.2.31).
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"

/* Serializes an object with the given int properties. A NULL name ends the
 * list; a property whose value is `omit` is skipped. */
static psrp_result_t build_object(psrp_buffer_t *out,
                                  const char *i64_name, int64_t i64_value,
                                  bool have_i64,
                                  const char *i32a_name, int32_t i32a_value,
                                  bool have_i32a,
                                  const char *i32b_name, int32_t i32b_value,
                                  bool have_i32b)
{
    psrp_object_t *o;
    psrp_value_t v, wrapper;
    psrp_result_t rc = PSRP_OK;

    if (!out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);
    psrp_value_init(&v);

    if (have_i32a) {
        psrp_value_set_int32(&v, i32a_value);
        rc = psrp_object_add_extended(o, i32a_name, &v);
        psrp_value_free(&v);
    }
    if (rc == PSRP_OK && have_i32b) {
        psrp_value_set_int32(&v, i32b_value);
        rc = psrp_object_add_extended(o, i32b_name, &v);
        psrp_value_free(&v);
    }
    if (rc == PSRP_OK && have_i64) {
        psrp_value_set_int64(&v, i64_value);
        rc = psrp_object_add_extended(o, i64_name, &v);
        psrp_value_free(&v);
    }
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, o);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

/* The spec's examples put the count before ci, so we do too. */
psrp_result_t psrp_build_set_max_runspaces(int64_t ci, int32_t max_runspaces,
                                           psrp_buffer_t *out)
{
    return build_object(out, "ci", ci, true,
                        "MaxRunspaces", max_runspaces, true,
                        NULL, 0, false);
}

psrp_result_t psrp_build_set_min_runspaces(int64_t ci, int32_t min_runspaces,
                                           psrp_buffer_t *out)
{
    return build_object(out, "ci", ci, true,
                        "MinRunspaces", min_runspaces, true,
                        NULL, 0, false);
}

psrp_result_t psrp_build_get_available_runspaces(int64_t ci, psrp_buffer_t *out)
{
    return build_object(out, "ci", ci, true, NULL, 0, false, NULL, 0, false);
}

psrp_result_t psrp_build_reset_runspace_state(int64_t ci, psrp_buffer_t *out)
{
    return build_object(out, "ci", ci, true, NULL, 0, false, NULL, 0, false);
}

psrp_result_t psrp_build_connect_runspacepool(int32_t min_runspaces,
                                              int32_t max_runspaces,
                                              psrp_buffer_t *out)
{
    /* Both bounds are optional. Omitting both yields an object with no
     * properties, which the spec defines as a single-runspace pool. */
    return build_object(out, NULL, 0, false,
                        "MinRunspaces", min_runspaces, min_runspaces >= 0,
                        "MaxRunspaces", max_runspaces, max_runspaces >= 0);
}

/* ------------------------------------------------------------ parsers ---- */

static psrp_result_t root_object(const void *xml, size_t n, psrp_value_t *root)
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

psrp_result_t psrp_parse_runspace_availability(
    const void *xml, size_t n, psrp_runspace_availability_t *out)
{
    psrp_value_t root;
    const psrp_value_t *ci, *resp;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    ci = psrp_object_find(root.as.obj, "ci");
    if (!ci || ci->kind != PSRP_VAL_INT64) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    out->ci = ci->as.i64;

    /* Boolean when answering set-min/max, Signed Long for get-available. */
    resp = psrp_object_find(root.as.obj, "SetMinMaxRunspacesResponse");
    if (!resp) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    if (resp->kind == PSRP_VAL_BOOL) {
        out->is_count = false;
        out->accepted = resp->as.b;
    } else if (resp->kind == PSRP_VAL_INT64) {
        out->is_count = true;
        out->count = resp->as.i64;
    } else if (resp->kind == PSRP_VAL_INT32) {
        /* Some servers narrow the count to an I32; accept it rather than
         * failing over a width difference. */
        out->is_count = true;
        out->count = resp->as.i32;
    } else {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    psrp_value_free(&root);
    return PSRP_OK;
}

psrp_result_t psrp_parse_runspacepool_init_data(
    const void *xml, size_t n, psrp_runspacepool_init_data_t *out)
{
    psrp_value_t root;
    const psrp_value_t *v;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    /* Absent is -1, since 0 is a legal runspace count. */
    out->min_runspaces = -1;
    out->max_runspaces = -1;

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    v = psrp_object_find(root.as.obj, "MinRunspaces");
    if (v && v->kind == PSRP_VAL_INT32) out->min_runspaces = v->as.i32;
    v = psrp_object_find(root.as.obj, "MaxRunspaces");
    if (v && v->kind == PSRP_VAL_INT32) out->max_runspaces = v->as.i32;

    psrp_value_free(&root);
    return PSRP_OK;
}
