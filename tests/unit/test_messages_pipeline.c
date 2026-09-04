#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* Built payloads are verified by parsing them back with our own reader and
 * asserting the properties, rather than byte-comparing: RefId numbering is an
 * implementation detail, the property values are the contract. */

static bool xml_contains(const psrp_buffer_t *b, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;
    if (b->len < n) return false;
    for (i = 0; i + n <= b->len; i++)
        if (memcmp(b->data + i, needle, n) == 0) return true;
    return false;
}

static const psrp_value_t *prop(const psrp_value_t *obj, const char *name)
{
    if (!obj || obj->kind != PSRP_VAL_OBJECT) return NULL;
    return psrp_object_find(obj->as.obj, name);
}

/* ------------------------------------------------------------ HostInfo -- */

/* The prose says isHostNull; every example (and PowerShell) says _isHostNull.
 * Getting this wrong fails only against a real server, so pin it here. */
PSRP_TEST(host_info_uses_underscored_property_names)
{
    psrp_init_runspacepool_t init;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_init_runspacepool_defaults(&init);
    ASSERT_OK(psrp_build_init_runspacepool(&init, &xml));

    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_isHostNull\">true</B>"));
    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_isHostUINull\">true</B>"));
    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_isHostRawUINull\">true</B>"));
    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_useRunspaceHost\">true</B>"));
    /* A null host carries no _hostDefaultData, per the spec's own example. */
    ASSERT_FALSE(xml_contains(&xml, "_hostDefaultData"));
    psrp_buffer_free(&xml);
}

PSRP_TEST(host_info_null_sets_all_flags)
{
    psrp_host_info_t h;
    memset(&h, 0, sizeof h);
    psrp_host_info_null(&h);
    ASSERT_TRUE(h.is_host_null);
    ASSERT_TRUE(h.is_host_ui_null);
    ASSERT_TRUE(h.is_host_raw_ui_null);
    ASSERT_TRUE(h.use_runspace_host);
}

/* ------------------------------------------------- INIT_RUNSPACEPOOL ---- */

PSRP_TEST(init_runspacepool_defaults)
{
    psrp_init_runspacepool_t init;
    psrp_init_runspacepool_defaults(&init);
    ASSERT_EQ_I(init.min_runspaces, 1);
    ASSERT_EQ_I(init.max_runspaces, 1);
    ASSERT_EQ_I(init.thread_options, PSRP_THREAD_DEFAULT);
    ASSERT_EQ_I(init.apartment_state, PSRP_APARTMENT_UNKNOWN);
    ASSERT_TRUE(init.host.is_host_null);
}

PSRP_TEST(init_runspacepool_structure)
{
    psrp_init_runspacepool_t init;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *v;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_init_runspacepool_defaults(&init);
    init.min_runspaces = 1;
    init.max_runspaces = 4;
    ASSERT_OK(psrp_build_init_runspacepool(&init, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    v = prop(&root, "MinRunspaces");
    ASSERT_NOT_NULL(v); ASSERT_EQ_I(v->kind, PSRP_VAL_INT32); ASSERT_EQ_I(v->as.i32, 1);
    v = prop(&root, "MaxRunspaces");
    ASSERT_NOT_NULL(v); ASSERT_EQ_I(v->as.i32, 4);

    /* ApplicationArguments is optional and explicitly allowed to be Null. */
    v = prop(&root, "ApplicationArguments");
    ASSERT_NOT_NULL(v); ASSERT_EQ_I(v->kind, PSRP_VAL_NULL);

    /* Enums are extended primitive objects: type names, ToString, and value. */
    v = prop(&root, "PSThreadOptions");
    ASSERT_NOT_NULL(v); ASSERT_EQ_I(v->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_type_name_count(v->as.obj), 4u);
    ASSERT_EQ_STR(psrp_object_type_name(v->as.obj, 0),
                  "System.Management.Automation.Runspaces.PSThreadOptions");
    ASSERT_EQ_STR(psrp_object_type_name(v->as.obj, 3), "System.Object");
    ASSERT_NOT_NULL(psrp_object_primitive(v->as.obj));
    ASSERT_EQ_I(psrp_object_primitive(v->as.obj)->as.i32, PSRP_THREAD_DEFAULT);

    v = prop(&root, "ApartmentState");
    ASSERT_NOT_NULL(v);
    ASSERT_EQ_I(psrp_object_primitive(v->as.obj)->as.i32, PSRP_APARTMENT_UNKNOWN);

    v = prop(&root, "HostInfo");
    ASSERT_NOT_NULL(v); ASSERT_EQ_I(v->kind, PSRP_VAL_OBJECT);
    ASSERT_NOT_NULL(psrp_object_find(v->as.obj, "_isHostNull"));

    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(init_runspacepool_enum_tostring_tracks_value)
{
    psrp_init_runspacepool_t init;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_init_runspacepool_defaults(&init);
    init.apartment_state = PSRP_APARTMENT_STA;
    init.thread_options = PSRP_THREAD_REUSE_THREAD;
    ASSERT_OK(psrp_build_init_runspacepool(&init, &xml));
    ASSERT_TRUE(xml_contains(&xml, "<ToString>STA</ToString>"));
    ASSERT_TRUE(xml_contains(&xml, "<ToString>ReuseThread</ToString>"));
    psrp_buffer_free(&xml);
}

PSRP_TEST(init_runspacepool_rejects_null_args)
{
    psrp_buffer_t xml;
    psrp_init_runspacepool_t init;
    psrp_buffer_init(&xml);
    psrp_init_runspacepool_defaults(&init);
    ASSERT_ERR(psrp_build_init_runspacepool(NULL, &xml), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_init_runspacepool(&init, NULL), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&xml);
}

/* --------------------------------------------------- CREATE_PIPELINE ---- */

PSRP_TEST(create_pipeline_defaults)
{
    psrp_create_pipeline_t opts;
    psrp_create_pipeline_defaults(&opts);
    ASSERT_TRUE(opts.no_input);
    ASSERT_FALSE(opts.add_to_history);
    ASSERT_FALSE(opts.is_nested);
    ASSERT_TRUE(opts.host.is_host_null);
}

PSRP_TEST(create_pipeline_single_script_command)
{
    psrp_create_pipeline_t opts;
    psrp_command_t *cmd;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *ps, *cmds, *first;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_create_pipeline_defaults(&opts);

    cmd = psrp_command_new("$env:COMPUTERNAME", true);
    ASSERT_NOT_NULL(cmd);
    ASSERT_OK(psrp_build_create_pipeline(&opts, &cmd, 1, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    ASSERT_NOT_NULL(prop(&root, "NoInput"));
    ASSERT_TRUE(prop(&root, "NoInput")->as.b);
    ASSERT_NOT_NULL(prop(&root, "AddToHistory"));
    ASSERT_FALSE(prop(&root, "AddToHistory")->as.b);
    ASSERT_NOT_NULL(prop(&root, "HostInfo"));

    ps = prop(&root, "PowerShell");
    ASSERT_NOT_NULL(ps);
    ASSERT_EQ_I(ps->kind, PSRP_VAL_OBJECT);

    cmds = psrp_object_find(ps->as.obj, "Cmds");
    ASSERT_NOT_NULL(cmds);
    ASSERT_EQ_I(psrp_object_container(cmds->as.obj), PSRP_CONTAINER_LIST);
    ASSERT_EQ_SZ(psrp_object_item_count(cmds->as.obj), 1u);

    first = psrp_object_item(cmds->as.obj, 0);
    ASSERT_EQ_I(first->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_STR(psrp_object_find(first->as.obj, "Cmd")->as.text.ptr,
                  "$env:COMPUTERNAME");
    ASSERT_TRUE(psrp_object_find(first->as.obj, "IsScript")->as.b);
    /* UseLocalScope is Null so the server decides. */
    ASSERT_EQ_I(psrp_object_find(first->as.obj, "UseLocalScope")->kind,
                PSRP_VAL_NULL);

    psrp_command_free(cmd);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(create_pipeline_command_parameters)
{
    psrp_create_pipeline_t opts;
    psrp_command_t *cmd;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *ps, *cmds, *first, *args, *p0, *p1;
    psrp_value_t num;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_value_init(&num);
    psrp_create_pipeline_defaults(&opts);

    cmd = psrp_command_new("Get-Item", false);
    ASSERT_NOT_NULL(cmd);
    ASSERT_OK(psrp_command_add_string_parameter(cmd, "Path", "C:\\"));
    psrp_value_set_int32(&num, 7);
    ASSERT_OK(psrp_command_add_parameter(cmd, NULL, &num));   /* positional */
    /* add_parameter took ownership. */
    ASSERT_EQ_I(num.kind, PSRP_VAL_NULL);

    ASSERT_OK(psrp_build_create_pipeline(&opts, &cmd, 1, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    ps = prop(&root, "PowerShell");
    cmds = psrp_object_find(ps->as.obj, "Cmds");
    first = psrp_object_item(cmds->as.obj, 0);
    args = psrp_object_find(first->as.obj, "Args");
    ASSERT_NOT_NULL(args);
    ASSERT_EQ_SZ(psrp_object_item_count(args->as.obj), 2u);

    p0 = psrp_object_item(args->as.obj, 0);
    ASSERT_EQ_STR(psrp_object_find(p0->as.obj, "N")->as.text.ptr, "Path");
    ASSERT_EQ_STR(psrp_object_find(p0->as.obj, "V")->as.text.ptr, "C:\\");

    p1 = psrp_object_item(args->as.obj, 1);
    /* A positional argument has a Null name (2.2.3.13). */
    ASSERT_EQ_I(psrp_object_find(p1->as.obj, "N")->kind, PSRP_VAL_NULL);
    ASSERT_EQ_I(psrp_object_find(p1->as.obj, "V")->as.i32, 7);

    psrp_command_free(cmd);
    psrp_value_free(&root);
    psrp_value_free(&num);
    psrp_buffer_free(&xml);
}

PSRP_TEST(create_pipeline_multiple_commands_keep_order)
{
    psrp_create_pipeline_t opts;
    psrp_command_t *cmds_in[3];
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *ps, *cmds;
    size_t i;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_create_pipeline_defaults(&opts);
    cmds_in[0] = psrp_command_new("Get-Process", false);
    cmds_in[1] = psrp_command_new("Sort-Object", false);
    cmds_in[2] = psrp_command_new("Select-Object", false);
    for (i = 0; i < 3; i++) ASSERT_NOT_NULL(cmds_in[i]);

    ASSERT_OK(psrp_build_create_pipeline(&opts, cmds_in, 3, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));

    ps = prop(&root, "PowerShell");
    cmds = psrp_object_find(ps->as.obj, "Cmds");
    ASSERT_EQ_SZ(psrp_object_item_count(cmds->as.obj), 3u);
    ASSERT_EQ_STR(psrp_object_find(psrp_object_item(cmds->as.obj, 0)->as.obj,
                                   "Cmd")->as.text.ptr, "Get-Process");
    ASSERT_EQ_STR(psrp_object_find(psrp_object_item(cmds->as.obj, 2)->as.obj,
                                   "Cmd")->as.text.ptr, "Select-Object");

    for (i = 0; i < 3; i++) psrp_command_free(cmds_in[i]);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

/* A command may be serialized more than once, so building must not consume
 * the stored parameter values. */
PSRP_TEST(create_pipeline_can_be_built_twice)
{
    psrp_create_pipeline_t opts;
    psrp_command_t *cmd;
    psrp_buffer_t a, b;

    psrp_buffer_init(&a);
    psrp_buffer_init(&b);
    psrp_create_pipeline_defaults(&opts);
    cmd = psrp_command_new("Get-Item", false);
    ASSERT_NOT_NULL(cmd);
    ASSERT_OK(psrp_command_add_string_parameter(cmd, "Path", "C:\\"));

    ASSERT_OK(psrp_build_create_pipeline(&opts, &cmd, 1, &a));
    ASSERT_OK(psrp_build_create_pipeline(&opts, &cmd, 1, &b));
    ASSERT_EQ_MEM(a.data, a.len, b.data, b.len);

    psrp_command_free(cmd);
    psrp_buffer_free(&a);
    psrp_buffer_free(&b);
}

PSRP_TEST(create_pipeline_escapes_script_text)
{
    psrp_create_pipeline_t opts;
    psrp_command_t *cmd;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *ps, *cmds, *first;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_create_pipeline_defaults(&opts);
    /* Newlines and XML metacharacters must survive both escaping layers. */
    cmd = psrp_command_new("if ($a -lt 5) {\n  'x & y'\n}", true);
    ASSERT_NOT_NULL(cmd);
    ASSERT_OK(psrp_build_create_pipeline(&opts, &cmd, 1, &xml));
    ASSERT_TRUE(xml_contains(&xml, "_x000A_"));
    ASSERT_TRUE(xml_contains(&xml, "&amp;"));

    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    ps = prop(&root, "PowerShell");
    cmds = psrp_object_find(ps->as.obj, "Cmds");
    first = psrp_object_item(cmds->as.obj, 0);
    ASSERT_EQ_STR(psrp_object_find(first->as.obj, "Cmd")->as.text.ptr,
                  "if ($a -lt 5) {\n  'x & y'\n}");

    psrp_command_free(cmd);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(create_pipeline_requires_a_command)
{
    psrp_create_pipeline_t opts;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_create_pipeline_defaults(&opts);
    ASSERT_ERR(psrp_build_create_pipeline(&opts, NULL, 0, &xml),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_create_pipeline(NULL, NULL, 1, &xml),
               PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&xml);
}

PSRP_TEST(command_new_rejects_null_and_free_is_safe)
{
    ASSERT_NULL(psrp_command_new(NULL, false));
    psrp_command_free(NULL);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(host_info_uses_underscored_property_names),
    PSRP_TEST_CASE(host_info_null_sets_all_flags),
    PSRP_TEST_CASE(init_runspacepool_defaults),
    PSRP_TEST_CASE(init_runspacepool_structure),
    PSRP_TEST_CASE(init_runspacepool_enum_tostring_tracks_value),
    PSRP_TEST_CASE(init_runspacepool_rejects_null_args),
    PSRP_TEST_CASE(create_pipeline_defaults),
    PSRP_TEST_CASE(create_pipeline_single_script_command),
    PSRP_TEST_CASE(create_pipeline_command_parameters),
    PSRP_TEST_CASE(create_pipeline_multiple_commands_keep_order),
    PSRP_TEST_CASE(create_pipeline_can_be_built_twice),
    PSRP_TEST_CASE(create_pipeline_escapes_script_text),
    PSRP_TEST_CASE(create_pipeline_requires_a_command),
    PSRP_TEST_CASE(command_new_rejects_null_and_free_is_safe),
};

PSRP_TEST_MAIN(cases)
