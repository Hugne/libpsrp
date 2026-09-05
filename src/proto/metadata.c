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
                                              const char *const *namespaces,
                                              size_t namespace_count,
                                              const psrp_value_t *argument_list,
                                              psrp_buffer_t *out)
{
    psrp_object_t *root, *names, *type_obj;
    psrp_value_t v, wrapper;
    psrp_result_t rc = PSRP_OK;
    size_t i;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (pattern_count && !name_patterns) return PSRP_ERR_INVALID_ARG;
    if (namespace_count && !namespaces) return PSRP_ERR_INVALID_ARG;
    /* 2.2.3.24 says ArgumentList MUST be a list. Catching that here beats
     * having the server reject the whole request for it. */
    if (argument_list) {
        if (argument_list->kind != PSRP_VAL_OBJECT ||
            psrp_object_container(argument_list->as.obj) != PSRP_CONTAINER_LIST)
            return PSRP_ERR_INVALID_ARG;
    }

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

    /* Namespace: a list of module names, or Null when none were given. The
     * spec defines Null as a list holding one empty string; see the header
     * for what that does to wildcards in practice. */
    if (rc == PSRP_OK && namespace_count) {
        psrp_object_t *ns = psrp_object_new();
        if (!ns) { psrp_object_free(root); return PSRP_ERR_NOMEM; }
        psrp_object_set_ref_id(ns, 3);
        psrp_object_set_container(ns, PSRP_CONTAINER_LIST);
        for (i = 0; i < namespace_count && rc == PSRP_OK; i++) {
            if (!namespaces[i]) { rc = PSRP_ERR_INVALID_ARG; break; }
            rc = psrp_value_set_text(&v, PSRP_VAL_STRING, namespaces[i],
                                     strlen(namespaces[i]));
            if (rc == PSRP_OK) rc = psrp_object_add_item(ns, &v);
            psrp_value_free(&v);
        }
        if (rc == PSRP_OK) {
            rc = psrp_value_set_object(&v, ns);
            if (rc == PSRP_OK)
                rc = psrp_object_add_extended(root, "Namespace", &v);
            else psrp_object_free(ns);
            psrp_value_free(&v);
        } else {
            psrp_object_free(ns);
        }
    } else if (rc == PSRP_OK) {
        psrp_value_set_null(&v);
        rc = psrp_object_add_extended(root, "Namespace", &v);
        psrp_value_free(&v);
    }
    /* ArgumentList (2.2.3.24), copied so the caller keeps theirs. */
    if (rc == PSRP_OK) {
        if (argument_list) rc = psrp_value_clone(argument_list, &v);
        else psrp_value_set_null(&v);
        if (rc == PSRP_OK)
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

psrp_result_t psrp_command_metadata_count_from_value(const psrp_value_t *v,
                                                     int32_t *count)
{
    const psrp_value_t *c;

    if (!v || !count) return PSRP_ERR_INVALID_ARG;
    *count = -1;
    if (v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;

    c = psrp_object_find(v->as.obj, "Count");
    if (!c || c->kind != PSRP_VAL_INT32) return PSRP_ERR_MALFORMED;
    *count = c->as.i32;
    return PSRP_OK;
}

psrp_result_t psrp_parse_command_metadata_count(const void *xml, size_t n,
                                                int32_t *count)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!count) return PSRP_ERR_INVALID_ARG;
    *count = -1;

    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) return rc;
    rc = psrp_command_metadata_count_from_value(&root, count);
    psrp_value_free(&root);
    return rc;
}

/* 2.2.3.23. `fallback_name` is the dictionary key, used when the metadata
 * object does not carry a Name of its own. Returns PSRP_ERR_NOMEM only; a
 * malformed parameter object yields a name-only entry rather than failing the
 * whole command, since one odd parameter should not cost the caller the other
 * fifty. */
static psrp_result_t read_parameter(const psrp_value_t *v,
                                    const char *fallback_name,
                                    psrp_parameter_metadata_t *out)
{
    const psrp_object_t *o;
    const psrp_value_t *aliases, *flag;
    size_t i, count;

    memset(out, 0, sizeof *out);
    out->name = dup_n(fallback_name, strlen(fallback_name));
    if (!out->name) return PSRP_ERR_NOMEM;

    if (!v || v->kind != PSRP_VAL_OBJECT) return PSRP_OK;
    o = v->as.obj;

    {
        char *name = text_prop(o, "Name");
        if (name) { free(out->name); out->name = name; }
    }
    out->parameter_type = text_prop(o, "ParameterType");

    flag = psrp_object_find(o, "SwitchParameter");
    if (flag && flag->kind == PSRP_VAL_BOOL) {
        out->is_switch = flag->as.b;
    } else if (out->parameter_type) {
        /* The spec defines SwitchParameter as exactly this comparison, so a
         * server that omits the property has not withheld anything. */
        out->is_switch = strcmp(out->parameter_type,
            "System.Management.Automation.SwitchParameter") == 0;
    }

    flag = psrp_object_find(o, "IsDynamic");
    out->is_dynamic = flag && flag->kind == PSRP_VAL_BOOL && flag->as.b;

    aliases = psrp_object_find(o, "Aliases");
    if (aliases && aliases->kind == PSRP_VAL_OBJECT) {
        count = psrp_object_item_count(aliases->as.obj);
        if (count) {
            out->aliases = (char **)calloc(count, sizeof *out->aliases);
            if (!out->aliases) return PSRP_ERR_NOMEM;
            for (i = 0; i < count; i++) {
                const psrp_value_t *a = psrp_object_item(aliases->as.obj, i);
                if (!a || a->kind != PSRP_VAL_STRING) continue;
                out->aliases[out->alias_count] =
                    dup_n(a->as.text.ptr, a->as.text.len);
                if (!out->aliases[out->alias_count]) return PSRP_ERR_NOMEM;
                out->alias_count++;
            }
        }
    }
    return PSRP_OK;
}

psrp_result_t psrp_parse_command_metadata(const void *xml, size_t n,
                                          psrp_command_metadata_t *out)
{
    psrp_value_t root;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    rc = root_object(xml, n, &root);
    if (rc != PSRP_OK) {
        memset(out, 0, sizeof *out);
        out->command_type = -1;
        return rc;
    }
    rc = psrp_command_metadata_from_value(&root, out);
    psrp_value_free(&root);
    return rc;
}

psrp_result_t psrp_command_metadata_from_value(const psrp_value_t *v,
                                               psrp_command_metadata_t *out)
{
    psrp_value_t root;
    const psrp_value_t *params;

    if (!v || !out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->command_type = -1;
    if (v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;

    /* The rest of this function reads through `root`, which the XML entry
     * point used to own. Borrowing rather than copying keeps the two paths
     * identical; nothing here frees it. */
    root = *v;

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
            out->parameters = (psrp_parameter_metadata_t *)
                calloc(count, sizeof *out->parameters);
            if (!out->parameter_names || !out->parameters) {
                psrp_command_metadata_free(out);
                return PSRP_ERR_NOMEM;
            }
            for (i = 0; i < count; i++) {
                const psrp_dict_entry_t *e = psrp_object_entry(params->as.obj, i);
                if (e->key.kind != PSRP_VAL_STRING) continue;
                out->parameter_names[kept] = dup_n(e->key.as.text.ptr,
                                                   e->key.as.text.len);
                if (!out->parameter_names[kept]) {
                    psrp_command_metadata_free(out);
                    return PSRP_ERR_NOMEM;
                }
                /* The value is a 2.2.3.23 ParameterMetadata. A parameter whose
                 * object is missing or unreadable still gets an entry carrying
                 * its name, so the two arrays stay index-aligned; a caller
                 * walking them in parallel must not have to check. */
                if (read_parameter(&e->value, out->parameter_names[kept],
                                   &out->parameters[kept]) != PSRP_OK) {
                    psrp_command_metadata_free(out);
                    return PSRP_ERR_NOMEM;
                }
                kept++;
            }
            out->parameter_count = kept;
        }
    }

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
    for (i = 0; i < m->parameter_count; i++) {
        size_t j;
        free(m->parameter_names[i]);
        if (m->parameters) {
            free(m->parameters[i].name);
            free(m->parameters[i].parameter_type);
            for (j = 0; j < m->parameters[i].alias_count; j++)
                free(m->parameters[i].aliases[j]);
            free(m->parameters[i].aliases);
        }
    }
    free(m->parameter_names);
    free(m->parameters);
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
