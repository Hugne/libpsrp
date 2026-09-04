/* Command metadata and user events ([MS-PSRP] 2.2.2.12, 2.2.2.14,
 * 2.2.3.19-23). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_metadata.h"
#include "psrp/psrp_clixml.h"

const char *psrp_command_type_name(int32_t command_type)
{
    switch (command_type) {
    case PSRP_COMMAND_TYPE_ALIAS:           return "Alias";
    case PSRP_COMMAND_TYPE_FUNCTION:        return "Function";
    case PSRP_COMMAND_TYPE_FILTER:          return "Filter";
    case PSRP_COMMAND_TYPE_CMDLET:          return "Cmdlet";
    case PSRP_COMMAND_TYPE_EXTERNAL_SCRIPT: return "ExternalScript";
    case PSRP_COMMAND_TYPE_APPLICATION:     return "Application";
    case PSRP_COMMAND_TYPE_SCRIPT:          return "Script";
    case PSRP_COMMAND_TYPE_CONFIGURATION:   return "Configuration";
    case PSRP_COMMAND_TYPE_ALL:             return "All";
    default:                                return NULL;   /* a set, or unknown */
    }
}

/* ------------------------------------------------------------ helpers -- */

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
    switch (v->kind) {
    case PSRP_VAL_STRING:
    case PSRP_VAL_URI:
    case PSRP_VAL_DATETIME:
        return dup_n(v->as.text.ptr, v->as.text.len);
    default:
        return NULL;      /* Null or another type means "no value" */
    }
}

static int32_t i32_prop(const psrp_object_t *o, const char *name, int32_t missing)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (!v) return missing;
    if (v->kind == PSRP_VAL_INT32) return v->as.i32;
    /* An enum arrives as an extended primitive object wrapping the int. */
    if (v->kind == PSRP_VAL_OBJECT) {
        const psrp_value_t *prim = psrp_object_primitive(v->as.obj);
        if (prim && prim->kind == PSRP_VAL_INT32) return prim->as.i32;
    }
    return missing;
}

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

/* -------------------------------------------- GET_COMMAND_METADATA ----- */

psrp_result_t psrp_build_get_command_metadata(const char *const *name_patterns,
                                              size_t pattern_count,
                                              int32_t command_type,
                                              psrp_buffer_t *out)
{
    psrp_object_t *root, *names, *type_obj;
    psrp_value_t v, wrapper;
    psrp_result_t rc = PSRP_OK;
    size_t i;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (pattern_count && !name_patterns) return PSRP_ERR_INVALID_ARG;

    root = psrp_object_new();
    if (!root) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(root, 0);
    psrp_value_init(&v);

    /* Name: a list of Wildcards, or Null meaning "*". */
    if (pattern_count) {
        names = psrp_object_new();
        if (!names) { psrp_object_free(root); return PSRP_ERR_NOMEM; }
        psrp_object_set_ref_id(names, 1);
        psrp_object_set_container(names, PSRP_CONTAINER_LIST);
        for (i = 0; i < pattern_count && rc == PSRP_OK; i++) {
            if (!name_patterns[i]) { rc = PSRP_ERR_INVALID_ARG; break; }
            rc = psrp_value_set_text(&v, PSRP_VAL_STRING, name_patterns[i],
                                     strlen(name_patterns[i]));
            if (rc == PSRP_OK) rc = psrp_object_add_item(names, &v);
            psrp_value_free(&v);
        }
        if (rc == PSRP_OK) {
            rc = psrp_value_set_object(&v, names);
            if (rc == PSRP_OK) rc = psrp_object_add_extended(root, "Name", &v);
            else psrp_object_free(names);
            psrp_value_free(&v);
        } else {
            psrp_object_free(names);
        }
    } else {
        psrp_value_set_null(&v);
        rc = psrp_object_add_extended(root, "Name", &v);
        psrp_value_free(&v);
    }

    /* CommandType: an enum object. */
    if (rc == PSRP_OK) {
        const char *name = psrp_command_type_name(command_type);
        char fallback[32];
        if (!name) {
            /* A combination of flags has no single name; .NET would render a
             * comma-separated list, and the number is unambiguous. */
            snprintf(fallback, sizeof fallback, "%ld", (long)command_type);
            name = fallback;
        }
        type_obj = psrp_object_new();
        if (!type_obj) { psrp_object_free(root); return PSRP_ERR_NOMEM; }
        psrp_object_set_ref_id(type_obj, 2);
        psrp_object_set_type_ref_id(type_obj, 0);
        rc = psrp_object_add_type_name(type_obj,
            "System.Management.Automation.CommandTypes");
        if (rc == PSRP_OK) rc = psrp_object_add_type_name(type_obj, "System.Enum");
        if (rc == PSRP_OK) rc = psrp_object_add_type_name(type_obj, "System.ValueType");
        if (rc == PSRP_OK) rc = psrp_object_add_type_name(type_obj, "System.Object");
        if (rc == PSRP_OK)
            rc = psrp_object_set_to_string(type_obj, name, strlen(name));
        if (rc == PSRP_OK) {
            psrp_value_set_int32(&v, command_type);
            rc = psrp_object_set_primitive(type_obj, &v);
            psrp_value_free(&v);
        }
        if (rc == PSRP_OK) {
            rc = psrp_value_set_object(&v, type_obj);
            if (rc == PSRP_OK)
                rc = psrp_object_add_extended(root, "CommandType", &v);
            else psrp_object_free(type_obj);
            psrp_value_free(&v);
        } else {
            psrp_object_free(type_obj);
        }
    }

    /* Null Namespace means a list with one empty string, and Null
     * ArgumentList means none: both are what we want. */
    if (rc == PSRP_OK) {
        psrp_value_set_null(&v);
        rc = psrp_object_add_extended(root, "Namespace", &v);
        psrp_value_free(&v);
    }
    if (rc == PSRP_OK) {
        psrp_value_set_null(&v);
        rc = psrp_object_add_extended(root, "ArgumentList", &v);
        psrp_value_free(&v);
    }

    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, root);
    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

psrp_result_t psrp_parse_command_metadata_count(const void *xml, size_t n,
                                                int32_t *count)
{
    psrp_value_t root;
    const psrp_value_t *v;
    psrp_result_t rc;

    if (!count) return PSRP_ERR_INVALID_ARG;
    *count = -1;

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    v = psrp_object_find(root.as.obj, "Count");
    if (!v || v->kind != PSRP_VAL_INT32) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    *count = v->as.i32;
    psrp_value_free(&root);
    return PSRP_OK;
}

psrp_result_t psrp_parse_command_metadata(const void *xml, size_t n,
                                          psrp_command_metadata_t *out)
{
    psrp_value_t root;
    const psrp_value_t *params;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->command_type = -1;

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    out->name = text_prop(root.as.obj, "Name");
    out->command_namespace = text_prop(root.as.obj, "Namespace");
    out->help_uri = text_prop(root.as.obj, "HelpUri");
    out->command_type = i32_prop(root.as.obj, "CommandType", -1);

    /* Parameters is a dictionary keyed by parameter name; the values are
     * ParameterMetadata objects we do not need to unpack to answer "what
     * parameters does this command take". */
    params = psrp_object_find(root.as.obj, "Parameters");
    if (params && params->kind == PSRP_VAL_OBJECT) {
        size_t count = psrp_object_entry_count(params->as.obj);
        size_t i, kept = 0;
        if (count) {
            out->parameter_names = (char **)calloc(count, sizeof *out->parameter_names);
            if (!out->parameter_names) {
                psrp_value_free(&root);
                psrp_command_metadata_free(out);
                return PSRP_ERR_NOMEM;
            }
            for (i = 0; i < count; i++) {
                const psrp_dict_entry_t *e = psrp_object_entry(params->as.obj, i);
                if (e->key.kind != PSRP_VAL_STRING) continue;
                out->parameter_names[kept] = dup_n(e->key.as.text.ptr,
                                                   e->key.as.text.len);
                if (!out->parameter_names[kept]) {
                    psrp_value_free(&root);
                    psrp_command_metadata_free(out);
                    return PSRP_ERR_NOMEM;
                }
                kept++;
            }
            out->parameter_count = kept;
        }
    }

    psrp_value_free(&root);
    if (!out->name) {           /* 2.2.3.22: Name is a non-empty string */
        psrp_command_metadata_free(out);
        return PSRP_ERR_MALFORMED;
    }
    return PSRP_OK;
}

void psrp_command_metadata_free(psrp_command_metadata_t *m)
{
    size_t i;
    if (!m) return;
    free(m->name);
    free(m->command_namespace);
    free(m->help_uri);
    for (i = 0; i < m->parameter_count; i++) free(m->parameter_names[i]);
    free(m->parameter_names);
    memset(m, 0, sizeof *m);
    m->command_type = -1;
}

/* ------------------------------------------------------- USER_EVENT ---- */

psrp_result_t psrp_parse_user_event(const void *xml, size_t n,
                                    psrp_user_event_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->event_id = -1;

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;

    /* The property names contain literal dots. */
    out->event_id = i32_prop(root.as.obj, "PSEventArgs.EventIdentifier", -1);
    out->source_identifier = text_prop(root.as.obj, "PSEventArgs.SourceIdentifier");
    out->time_generated = text_prop(root.as.obj, "PSEventArgs.TimeGenerated");
    out->computer_name = text_prop(root.as.obj, "PSEventArgs.ComputerName");

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_user_event_free(psrp_user_event_t *e)
{
    if (!e) return;
    free(e->source_identifier);
    free(e->time_generated);
    free(e->computer_name);
    memset(e, 0, sizeof *e);
    e->event_id = -1;
}
