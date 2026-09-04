/* Host method calls ([MS-PSRP] 2.2.2.15/16, 2.2.2.27/28, 2.2.3.17). */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_host.h"
#include "psrp/psrp_clixml.h"

/* 2.2.3.17. `returns` is what decides whether a response is required. */
static const struct {
    int32_t id;
    const char *name;
    bool returns;
} kMethods[] = {
    { 1,  "GetName",                          true  },
    { 2,  "GetVersion",                       true  },
    { 3,  "GetInstanceId",                    true  },
    { 4,  "GetCurrentCulture",                true  },
    { 5,  "GetCurrentUICulture",              true  },
    { 6,  "SetShouldExit",                    false },
    { 7,  "EnterNestedPrompt",                false },
    { 8,  "ExitNestedPrompt",                 false },
    { 9,  "NotifyBeginApplication",           false },
    { 10, "NotifyEndApplication",             false },
    { 11, "ReadLine",                         true  },
    { 12, "ReadLineAsSecureString",           true  },
    { 13, "Write1",                           false },
    { 14, "Write2",                           false },
    { 15, "WriteLine1",                       false },
    { 16, "WriteLine2",                       false },
    { 17, "WriteLine3",                       false },
    { 18, "WriteErrorLine",                   false },
    { 19, "WriteDebugLine",                   false },
    { 20, "WriteProgress",                    false },
    { 21, "WriteVerboseLine",                 false },
    { 22, "WriteWarningLine",                 false },
    { 23, "Prompt",                           true  },
    { 24, "PromptForCredential1",             true  },
    { 25, "PromptForCredential2",             true  },
    { 26, "PromptForChoice",                  true  },
    { 27, "GetForegroundColor",               true  },
    { 28, "SetForegroundColor",               false },
    { 29, "GetBackgroundColor",               true  },
    { 30, "SetBackgroundColor",               false },
    { 31, "GetCursorPosition",                true  },
    { 32, "SetCursorPosition",                false },
    { 33, "GetWindowPosition",                true  },
    { 34, "SetWindowPosition",                false },
    { 35, "GetCursorSize",                    true  },
    { 36, "SetCursorSize",                    false },
    { 37, "GetBufferSize",                    true  },
    { 38, "SetBufferSize",                    false },
    { 39, "GetWindowSize",                    true  },
    { 40, "SetWindowSize",                    false },
    { 41, "GetWindowTitle",                   true  },
    { 42, "SetWindowTitle",                   false },
    { 43, "GetMaxWindowSize",                 true  },
    { 44, "GetMaxPhysicalWindowSize",         true  },
    { 45, "GetKeyAvailable",                  true  },
    { 46, "ReadKey",                          true  },
    { 47, "FlushInputBuffer",                 false },
    { 48, "SetBufferContents1",               false },
    { 49, "SetBufferContents2",               false },
    { 50, "GetBufferContents",                true  },
    { 51, "ScrollBufferContents",             false },
    { 52, "PushRunspace",                     false },
    { 53, "PopRunspace",                      false },
    { 54, "GetIsRunspacePushed",              true  },
    { 55, "GetRunspace",                      true  },
    { 56, "PromptForChoiceMultipleSelection", true  }
};
#define KMETHOD_COUNT (sizeof kMethods / sizeof kMethods[0])

const char *psrp_host_method_name(int32_t method_id)
{
    size_t i;
    for (i = 0; i < KMETHOD_COUNT; i++)
        if (kMethods[i].id == method_id) return kMethods[i].name;
    return "Unknown";
}

bool psrp_host_method_returns_value(int32_t method_id)
{
    size_t i;
    for (i = 0; i < KMETHOD_COUNT; i++)
        if (kMethods[i].id == method_id) return kMethods[i].returns;
    /* An unknown method is safest treated as expecting nothing: inventing a
     * response to a call we do not understand is worse than staying quiet. */
    return false;
}

/* --------------------------------------------------------------- parse -- */

psrp_result_t psrp_parse_host_call(const void *xml, size_t n,
                                   psrp_host_call_t *out)
{
    psrp_value_t root;
    const psrp_value_t *ci, *mi, *mp;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    psrp_value_init(&out->parameters);

    psrp_value_init(&root);
    rc = psrp_clixml_deserialize(xml, n, &root);
    if (rc != PSRP_OK) return rc;
    if (root.kind != PSRP_VAL_OBJECT) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    ci = psrp_object_find(root.as.obj, "ci");
    if (!ci || ci->kind != PSRP_VAL_INT64) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    out->call_id = ci->as.i64;

    /* mi is an enum, i.e. an extended primitive object wrapping an I32. A
     * server that sends a bare I32 is accepted too. */
    mi = psrp_object_find(root.as.obj, "mi");
    if (!mi) {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }
    if (mi->kind == PSRP_VAL_INT32) {
        out->method_id = mi->as.i32;
    } else if (mi->kind == PSRP_VAL_OBJECT) {
        const psrp_value_t *prim = psrp_object_primitive(mi->as.obj);
        if (!prim || prim->kind != PSRP_VAL_INT32) {
            psrp_value_free(&root);
            return PSRP_ERR_MALFORMED;
        }
        out->method_id = prim->as.i32;
    } else {
        psrp_value_free(&root);
        return PSRP_ERR_MALFORMED;
    }

    /* mp is optional and its shape depends on the method; hand it over as-is
     * rather than pretending to understand every parameter encoding. */
    mp = psrp_object_find(root.as.obj, "mp");
    if (mp) {
        psrp_buffer_t tmp;
        psrp_buffer_init(&tmp);
        rc = psrp_clixml_serialize(mp, &tmp);
        if (rc == PSRP_OK)
            rc = psrp_clixml_deserialize(tmp.data, tmp.len, &out->parameters);
        psrp_buffer_free(&tmp);
        if (rc != PSRP_OK) {
            psrp_value_free(&root);
            return rc;
        }
    }

    psrp_value_free(&root);
    return PSRP_OK;
}

void psrp_host_call_free(psrp_host_call_t *c)
{
    if (!c) return;
    psrp_value_free(&c->parameters);
    memset(c, 0, sizeof *c);
    psrp_value_init(&c->parameters);
}

/* --------------------------------------------------------------- build -- */

/* The mi property is serialized as the enum object PowerShell expects. */
static psrp_result_t add_method_id(psrp_object_t *o, int32_t method_id)
{
    psrp_object_t *e = psrp_object_new();
    psrp_value_t v, wrapper;
    psrp_result_t rc;
    const char *name = psrp_host_method_name(method_id);

    if (!e) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(e, 1);
    psrp_object_set_type_ref_id(e, 0);
    rc = psrp_object_add_type_name(e,
        "System.Management.Automation.Remoting.RemoteHostMethodId");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(e, "System.Enum");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(e, "System.ValueType");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(e, "System.Object");
    if (rc == PSRP_OK) rc = psrp_object_set_to_string(e, name, strlen(name));
    if (rc == PSRP_OK) {
        psrp_value_init(&v);
        psrp_value_set_int32(&v, method_id);
        rc = psrp_object_set_primitive(e, &v);
        psrp_value_free(&v);
    }
    if (rc != PSRP_OK) { psrp_object_free(e); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, e);
    if (rc != PSRP_OK) { psrp_object_free(e); return rc; }
    rc = psrp_object_add_extended(o, "mi", &wrapper);
    psrp_value_free(&wrapper);
    return rc;
}

static psrp_result_t add_call_id(psrp_object_t *o, int64_t call_id)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_int64(&v, call_id);
    rc = psrp_object_add_extended(o, "ci", &v);
    psrp_value_free(&v);
    return rc;
}

/* Deep-copies a value by round-tripping it, so the caller keeps ownership. */
static psrp_result_t copy_value(const psrp_value_t *src, psrp_value_t *dst)
{
    psrp_buffer_t tmp;
    psrp_result_t rc;
    psrp_buffer_init(&tmp);
    rc = psrp_clixml_serialize(src, &tmp);
    if (rc == PSRP_OK) rc = psrp_clixml_deserialize(tmp.data, tmp.len, dst);
    psrp_buffer_free(&tmp);
    return rc;
}

static psrp_result_t finish(psrp_object_t *o, psrp_buffer_t *out)
{
    psrp_value_t wrapper;
    psrp_result_t rc;
    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, o);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    rc = psrp_clixml_serialize(&wrapper, out);
    psrp_value_free(&wrapper);
    return rc;
}

psrp_result_t psrp_build_host_response(int64_t call_id, int32_t method_id,
                                       const psrp_value_t *return_value,
                                       psrp_buffer_t *out)
{
    psrp_object_t *o;
    psrp_value_t v;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);

    psrp_value_init(&v);
    if (return_value) rc = copy_value(return_value, &v);
    else { psrp_value_set_null(&v); rc = PSRP_OK; }
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, "mr", &v);
    psrp_value_free(&v);

    if (rc == PSRP_OK) rc = add_call_id(o, call_id);
    if (rc == PSRP_OK) rc = add_method_id(o, method_id);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    return finish(o, out);
}

psrp_result_t psrp_build_host_response_error(int64_t call_id, int32_t method_id,
                                             const char *message,
                                             psrp_buffer_t *out)
{
    psrp_object_t *o, *err;
    psrp_value_t v, wrapper;
    psrp_result_t rc;
    const char *text = message ? message : "host method not supported";

    if (!out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);

    /* 2.2.2.16: the me property is an ErrorRecord whose
     * FullyQualifiedErrorId SHOULD be RemoteHostExecutionException. */
    err = psrp_object_new();
    if (!err) { psrp_object_free(o); return PSRP_ERR_NOMEM; }
    psrp_object_set_ref_id(err, 1);
    psrp_object_set_type_ref_id(err, 0);
    rc = psrp_object_add_type_name(err,
        "System.Management.Automation.ErrorRecord");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(err, "System.Object");
    if (rc == PSRP_OK) rc = psrp_object_set_to_string(err, text, strlen(text));
    if (rc == PSRP_OK) {
        psrp_value_init(&v);
        rc = psrp_value_set_text(&v, PSRP_VAL_STRING,
                                 "RemoteHostExecutionException",
                                 strlen("RemoteHostExecutionException"));
        if (rc == PSRP_OK)
            rc = psrp_object_add_extended(err, "FullyQualifiedErrorId", &v);
        psrp_value_free(&v);
    }
    if (rc != PSRP_OK) { psrp_object_free(err); psrp_object_free(o); return rc; }

    psrp_value_init(&wrapper);
    rc = psrp_value_set_object(&wrapper, err);
    if (rc != PSRP_OK) { psrp_object_free(err); psrp_object_free(o); return rc; }
    rc = psrp_object_add_extended(o, "me", &wrapper);
    psrp_value_free(&wrapper);

    if (rc == PSRP_OK) rc = add_call_id(o, call_id);
    if (rc == PSRP_OK) rc = add_method_id(o, method_id);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
    return finish(o, out);
}
