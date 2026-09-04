/* INIT_RUNSPACEPOOL (2.2.2.2) and CREATE_PIPELINE (2.2.2.10), plus the
 * HostInfo (2.2.3.14), Pipeline (2.2.3.11), Command (2.2.3.12) and Command
 * Parameter (2.2.3.13) types they carry.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"
#include "psrp/psrp_host.h"

/* RefIds must be unique within a serialized document. Objects and type-name
 * lists are numbered in separate spaces, as PowerShell does. */
typedef struct refgen {
    int64_t next_obj;
    int64_t next_tn;
} refgen_t;

static int64_t next_obj(refgen_t *g) { return g->next_obj++; }
static int64_t next_tn(refgen_t *g) { return g->next_tn++; }

/* ------------------------------------------------------------ helpers ---- */

static psrp_result_t add_bool(psrp_object_t *o, const char *name, bool b)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_bool(&v, b);
    rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_i32(psrp_object_t *o, const char *name, int32_t x)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_int32(&v, x);
    rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_string(psrp_object_t *o, const char *name,
                                const char *text)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, text, strlen(text));
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_null(psrp_object_t *o, const char *name)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_null(&v);
    rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

/* Attaches `child` to `parent` under `name`, consuming `child` either way. */
static psrp_result_t add_object(psrp_object_t *parent, const char *name,
                                psrp_object_t *child)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    rc = psrp_value_set_object(&v, child);
    if (rc != PSRP_OK) { psrp_object_free(child); return rc; }
    rc = psrp_object_add_extended(parent, name, &v);
    psrp_value_free(&v);
    return rc;
}

/* A .NET enum serializes as an extended primitive object: type names, the
 * ToString of the enum member, and the numeric value (2.2.5.2.5). */
static psrp_object_t *build_enum(refgen_t *g, const char *type_name,
                                 const char *to_string, int32_t value)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v;
    psrp_result_t rc;

    if (!o) return NULL;
    psrp_object_set_ref_id(o, next_obj(g));
    psrp_object_set_type_ref_id(o, next_tn(g));

    rc = psrp_object_add_type_name(o, type_name);
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.Enum");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.ValueType");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.Object");
    if (rc == PSRP_OK)
        rc = psrp_object_set_to_string(o, to_string, strlen(to_string));
    if (rc == PSRP_OK) {
        psrp_value_init(&v);
        psrp_value_set_int32(&v, value);
        rc = psrp_object_set_primitive(o, &v);
        psrp_value_free(&v);
    }
    if (rc != PSRP_OK) { psrp_object_free(o); return NULL; }
    return o;
}

static const char *thread_options_name(int32_t v)
{
    switch (v) {
    case PSRP_THREAD_USE_NEW_THREAD:     return "UseNewThread";
    case PSRP_THREAD_REUSE_THREAD:       return "ReuseThread";
    case PSRP_THREAD_USE_CURRENT_THREAD: return "UseCurrentThread";
    default:                             return "Default";
    }
}

static const char *apartment_state_name(int32_t v)
{
    switch (v) {
    case PSRP_APARTMENT_STA: return "STA";
    case PSRP_APARTMENT_MTA: return "MTA";
    default:                 return "Unknown";
    }
}

/* --------------------------------------------------------- 2.2.3.14 ----- */

void psrp_host_info_null(psrp_host_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->is_host_null = true;
    out->is_host_ui_null = true;
    out->is_host_raw_ui_null = true;
    out->use_runspace_host = true;
    out->default_data = NULL;   /* no console to describe */
}

static psrp_object_t *build_host_info(refgen_t *g, const psrp_host_info_t *h)
{
    psrp_object_t *o = psrp_object_new();
    psrp_result_t rc;

    if (!o) return NULL;
    psrp_object_set_ref_id(o, next_obj(g));

    /* Underscored names: the spec's prose omits the underscore but every
     * example, and PowerShell itself, use it. */
    rc = add_bool(o, "_isHostNull", h->is_host_null);
    if (rc == PSRP_OK) rc = add_bool(o, "_isHostUINull", h->is_host_ui_null);
    if (rc == PSRP_OK) rc = add_bool(o, "_isHostRawUINull", h->is_host_raw_ui_null);
    if (rc == PSRP_OK) rc = add_bool(o, "_useRunspaceHost", h->use_runspace_host);
    /* _hostDefaultData is present only when the caller describes a console;
     * a null host omits it entirely, matching the spec's null-host example. */
    if (rc == PSRP_OK && h->default_data) {
        psrp_value_t data;
        psrp_value_init(&data);
        rc = psrp_host_build_default_data(h->default_data, &data);
        if (rc == PSRP_OK)
            rc = psrp_object_add_extended(o, "_hostDefaultData", &data);
        psrp_value_free(&data);
    }
    if (rc != PSRP_OK) { psrp_object_free(o); return NULL; }
    return o;
}

/* ---------------------------------------------------- INIT_RUNSPACEPOOL -- */

void psrp_init_runspacepool_defaults(psrp_init_runspacepool_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->min_runspaces = 1;
    out->max_runspaces = 1;
    out->thread_options = PSRP_THREAD_DEFAULT;
    out->apartment_state = PSRP_APARTMENT_UNKNOWN;
    psrp_host_info_null(&out->host);
}

psrp_result_t psrp_build_init_runspacepool(const psrp_init_runspacepool_t *init,
                                           psrp_buffer_t *out)
{
    refgen_t g;
    psrp_object_t *root, *child;
    psrp_value_t wrapper;
    psrp_result_t rc;

    if (!init || !out) return PSRP_ERR_INVALID_ARG;
    g.next_obj = 0;
    g.next_tn = 0;

    root = psrp_object_new();
    if (!root) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(root, next_obj(&g));

    /* Property order follows the 2.2.2.2 example. */
    rc = add_i32(root, "MinRunspaces", init->min_runspaces);
    if (rc == PSRP_OK) rc = add_i32(root, "MaxRunspaces", init->max_runspaces);

    if (rc == PSRP_OK) {
        child = build_enum(&g, "System.Management.Automation.Runspaces.PSThreadOptions",
                           thread_options_name(init->thread_options),
                           init->thread_options);
        rc = child ? add_object(root, "PSThreadOptions", child) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        child = build_enum(&g, "System.Threading.ApartmentState",
                           apartment_state_name(init->apartment_state),
                           init->apartment_state);
        rc = child ? add_object(root, "ApartmentState", child) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        child = build_host_info(&g, &init->host);
        rc = child ? add_object(root, "HostInfo", child) : PSRP_ERR_NOMEM;
    }
    /* ApplicationArguments is optional; Null is explicitly allowed. */
    if (rc == PSRP_OK) rc = add_null(root, "ApplicationArguments");

    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, root);
    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

/* ------------------------------------------------------- Command ------- */

typedef struct cmd_param {
    char *name;            /* NULL for a positional argument */
    psrp_value_t value;
} cmd_param_t;

struct psrp_command {
    char *cmd;
    bool is_script;
    cmd_param_t *params;
    size_t param_count;
};

psrp_command_t *psrp_command_new(const char *cmd, bool is_script)
{
    psrp_command_t *c;
    size_t n;
    if (!cmd) return NULL;
    c = (psrp_command_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    n = strlen(cmd);
    c->cmd = (char *)malloc(n + 1);
    if (!c->cmd) { free(c); return NULL; }
    memcpy(c->cmd, cmd, n + 1);
    c->is_script = is_script;
    return c;
}

void psrp_command_free(psrp_command_t *c)
{
    size_t i;
    if (!c) return;
    for (i = 0; i < c->param_count; i++) {
        free(c->params[i].name);
        psrp_value_free(&c->params[i].value);
    }
    free(c->params);
    free(c->cmd);
    free(c);
}

psrp_result_t psrp_command_add_parameter(psrp_command_t *c, const char *name,
                                         psrp_value_t *value)
{
    cmd_param_t *grown;
    char *name_copy = NULL;

    if (!c || !value) return PSRP_ERR_INVALID_ARG;
    if (name) {
        size_t n = strlen(name);
        name_copy = (char *)malloc(n + 1);
        if (!name_copy) return PSRP_ERR_NOMEM;
        memcpy(name_copy, name, n + 1);
    }
    grown = (cmd_param_t *)realloc(c->params, (c->param_count + 1) * sizeof *grown);
    if (!grown) { free(name_copy); return PSRP_ERR_NOMEM; }
    c->params = grown;
    c->params[c->param_count].name = name_copy;
    c->params[c->param_count].value = *value;   /* move */
    psrp_value_init(value);
    c->param_count++;
    return PSRP_OK;
}

psrp_result_t psrp_command_add_string_parameter(psrp_command_t *c,
                                                const char *name,
                                                const char *value)
{
    psrp_value_t v;
    psrp_result_t rc;
    if (!value) return PSRP_ERR_INVALID_ARG;
    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, value, strlen(value));
    if (rc == PSRP_OK) rc = psrp_command_add_parameter(c, name, &v);
    psrp_value_free(&v);
    return rc;
}

/* Builds one 2.2.3.12 Command object. */
static psrp_object_t *build_command(refgen_t *g, const psrp_command_t *c)
{
    psrp_object_t *o = psrp_object_new();
    psrp_object_t *args;
    psrp_result_t rc;
    size_t i;

    if (!o) return NULL;
    psrp_object_set_ref_id(o, next_obj(g));

    rc = add_string(o, "Cmd", c->cmd);
    if (rc == PSRP_OK) rc = add_bool(o, "IsScript", c->is_script);
    /* Null means "let the server decide", which is what PowerShell sends. */
    if (rc == PSRP_OK) rc = add_null(o, "UseLocalScope");

    /* Merge flags: no merging, which keeps the streams separate so the caller
     * sees error/warning/verbose records as themselves. */
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeMyResult", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeToResult", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergePreviousResults", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeError", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeWarning", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeVerbose", e) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        psrp_object_t *e = build_enum(g,
            "System.Management.Automation.Runspaces.PipelineResultTypes",
            "None", PSRP_RESULT_NONE);
        rc = e ? add_object(o, "MergeDebug", e) : PSRP_ERR_NOMEM;
    }

    /* Args: a list of 2.2.3.13 Command Parameter objects. */
    if (rc == PSRP_OK) {
        args = psrp_object_new();
        if (!args) { psrp_object_free(o); return NULL; }
        psrp_object_set_ref_id(args, next_obj(g));
        psrp_object_set_container(args, PSRP_CONTAINER_LIST);

        for (i = 0; i < c->param_count && rc == PSRP_OK; i++) {
            psrp_object_t *p = psrp_object_new();
            psrp_value_t pv, copy;
            if (!p) { rc = PSRP_ERR_NOMEM; break; }
            psrp_object_set_ref_id(p, next_obj(g));

            if (c->params[i].name)
                rc = add_string(p, "N", c->params[i].name);
            else
                rc = add_null(p, "N");    /* positional argument */

            /* The stored value is reused if the command is serialized twice,
             * so serialize a copy rather than moving it out. */
            if (rc == PSRP_OK) {
                psrp_buffer_t tmp;
                psrp_buffer_init(&tmp);
                psrp_value_init(&copy);
                rc = psrp_clixml_serialize_named(&c->params[i].value, "V", &tmp);
                if (rc == PSRP_OK) {
                    /* Re-parse to obtain an owned deep copy. */
                    psrp_value_t parsed;
                    psrp_value_init(&parsed);
                    rc = psrp_clixml_deserialize(tmp.data, tmp.len, &parsed);
                    if (rc == PSRP_OK) rc = psrp_object_add_extended(p, "V", &parsed);
                    psrp_value_free(&parsed);
                }
                psrp_buffer_free(&tmp);
                psrp_value_free(&copy);
            }

            if (rc == PSRP_OK) {
                psrp_value_init(&pv);
                rc = psrp_value_set_object(&pv, p);
                if (rc == PSRP_OK) rc = psrp_object_add_item(args, &pv);
                psrp_value_free(&pv);
                if (rc != PSRP_OK) p = NULL;   /* ownership already taken */
            } else {
                psrp_object_free(p);
            }
        }

        if (rc == PSRP_OK) rc = add_object(o, "Args", args);
        else psrp_object_free(args);
    }

    if (rc != PSRP_OK) { psrp_object_free(o); return NULL; }
    return o;
}

/* ------------------------------------------------------ CREATE_PIPELINE -- */

void psrp_create_pipeline_defaults(psrp_create_pipeline_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->no_input = true;
    out->add_to_history = false;
    out->is_nested = false;
    out->apartment_state = PSRP_APARTMENT_UNKNOWN;
    out->remote_stream_options = 0;
    psrp_host_info_null(&out->host);
}

psrp_result_t psrp_build_create_pipeline(const psrp_create_pipeline_t *opts,
                                         psrp_command_t *const *commands,
                                         size_t count, psrp_buffer_t *out)
{
    refgen_t g;
    psrp_object_t *root, *ps, *cmds, *child;
    psrp_value_t wrapper;
    psrp_result_t rc;
    size_t i;

    if (!opts || !out) return PSRP_ERR_INVALID_ARG;
    if (count && !commands) return PSRP_ERR_INVALID_ARG;
    if (count == 0) return PSRP_ERR_INVALID_ARG;   /* a pipeline needs a command */

    g.next_obj = 0;
    g.next_tn = 0;

    root = psrp_object_new();
    if (!root) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(root, next_obj(&g));

    /* PowerShell property: the pipeline description (2.2.3.11). */
    ps = psrp_object_new();
    if (!ps) { psrp_object_free(root); return PSRP_ERR_NOMEM; }
    psrp_object_set_ref_id(ps, next_obj(&g));

    cmds = psrp_object_new();
    if (!cmds) { psrp_object_free(ps); psrp_object_free(root); return PSRP_ERR_NOMEM; }
    psrp_object_set_ref_id(cmds, next_obj(&g));
    psrp_object_set_type_ref_id(cmds, next_tn(&g));
    rc = psrp_object_add_type_name(cmds,
        "System.Collections.Generic.List`1[["
        "System.Management.Automation.PSObject, System.Management.Automation, "
        "Version=1.0.0.0, Culture=neutral, PublicKeyToken=31bf3856ad364e35]]");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(cmds, "System.Object");
    psrp_object_set_container(cmds, PSRP_CONTAINER_LIST);

    for (i = 0; i < count && rc == PSRP_OK; i++) {
        psrp_object_t *c;
        psrp_value_t cv;
        if (!commands[i]) { rc = PSRP_ERR_INVALID_ARG; break; }
        c = build_command(&g, commands[i]);
        if (!c) { rc = PSRP_ERR_NOMEM; break; }
        psrp_value_init(&cv);
        rc = psrp_value_set_object(&cv, c);
        if (rc == PSRP_OK) rc = psrp_object_add_item(cmds, &cv);
        else psrp_object_free(c);
        psrp_value_free(&cv);
    }

    if (rc == PSRP_OK) rc = add_object(ps, "Cmds", cmds);
    else psrp_object_free(cmds);

    if (rc == PSRP_OK) rc = add_bool(ps, "IsNested", opts->is_nested);
    if (rc == PSRP_OK) rc = add_string(ps, "History", "");
    if (rc == PSRP_OK) rc = add_bool(ps, "RedirectShellErrorOutputPipe", true);

    if (rc == PSRP_OK) rc = add_object(root, "PowerShell", ps);
    else psrp_object_free(ps);

    if (rc == PSRP_OK) rc = add_bool(root, "NoInput", opts->no_input);
    if (rc == PSRP_OK) {
        child = build_enum(&g, "System.Threading.ApartmentState",
                           apartment_state_name(opts->apartment_state),
                           opts->apartment_state);
        rc = child ? add_object(root, "ApartmentState", child) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) {
        child = build_enum(&g,
            "System.Management.Automation.RemoteStreamOptions",
            "0", opts->remote_stream_options);
        rc = child ? add_object(root, "RemoteStreamOptions", child)
                   : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) rc = add_bool(root, "AddToHistory", opts->add_to_history);
    if (rc == PSRP_OK) {
        child = build_host_info(&g, &opts->host);
        rc = child ? add_object(root, "HostInfo", child) : PSRP_ERR_NOMEM;
    }
    if (rc == PSRP_OK) rc = add_bool(root, "IsNested", opts->is_nested);

    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, root);
    if (rc != PSRP_OK) { psrp_object_free(root); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}
