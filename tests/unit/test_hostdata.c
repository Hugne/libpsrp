#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

static bool xml_contains(const psrp_buffer_t *b, const char *needle)
{
    size_t n = strlen(needle), i;
    if (b->len < n) return false;
    for (i = 0; i + n <= b->len; i++)
        if (memcmp(b->data + i, needle, n) == 0) return true;
    return false;
}

/* ------------------------------------------------------------- colour -- */

PSRP_TEST(console_color_names)
{
    ASSERT_EQ_STR(psrp_console_color_name(PSRP_COLOR_DARK_BLUE), "DarkBlue");
    ASSERT_EQ_STR(psrp_console_color_name(PSRP_COLOR_GRAY), "Gray");
    ASSERT_EQ_STR(psrp_console_color_name(PSRP_COLOR_WHITE), "White");
    ASSERT_EQ_STR(psrp_console_color_name(16), "Unknown");
    ASSERT_EQ_STR(psrp_console_color_name(-1), "Unknown");
}

/* The spec's table starts at 1, but the type it names is System.ConsoleColor
 * where 0 is Black. Rejecting 0 would break on a black foreground. */
PSRP_TEST(console_color_zero_is_black)
{
    ASSERT_EQ_I(PSRP_COLOR_BLACK, 0);
    ASSERT_EQ_STR(psrp_console_color_name(0), "Black");
}

PSRP_TEST(color_wrapper_shape_matches_spec_example)
{
    psrp_value_t v;
    psrp_buffer_t xml;
    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_color(PSRP_COLOR_DARK_MAGENTA, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    /* 2.2.3.3's example: a T naming the type and V holding the int. */
    ASSERT_EQ_MEM(xml.data, xml.len,
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"T\">System.ConsoleColor</S>"
        "<I32 N=\"V\">5</I32>"
        "</MS></Obj>",
        strlen("<Obj RefId=\"0\"><MS>"
               "<S N=\"T\">System.ConsoleColor</S>"
               "<I32 N=\"V\">5</I32>"
               "</MS></Obj>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

/* --------------------------------------------------- coordinates/size -- */

PSRP_TEST(coordinates_wrapper_shape)
{
    psrp_value_t v;
    psrp_buffer_t xml;
    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_coordinates(3, 4, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_TRUE(xml_contains(&xml,
        "<S N=\"T\">System.Management.Automation.Host.Coordinates</S>"));
    ASSERT_TRUE(xml_contains(&xml, "<I32 N=\"x\">3</I32>"));
    ASSERT_TRUE(xml_contains(&xml, "<I32 N=\"y\">4</I32>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

/* The 2.2.2.2 example uses width/height, not x/y. */
PSRP_TEST(size_wrapper_shape)
{
    psrp_value_t v;
    psrp_buffer_t xml;
    psrp_value_init(&v);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_size(181, 98, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_TRUE(xml_contains(&xml,
        "<S N=\"T\">System.Management.Automation.Host.Size</S>"));
    ASSERT_TRUE(xml_contains(&xml, "<I32 N=\"width\">181</I32>"));
    ASSERT_TRUE(xml_contains(&xml, "<I32 N=\"height\">98</I32>"));
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
}

PSRP_TEST(host_value_round_trips)
{
    psrp_value_t v;
    int32_t a = 0, b = 0;

    psrp_value_init(&v);
    ASSERT_OK(psrp_host_make_coordinates(-5, 7, &v));
    ASSERT_OK(psrp_host_read_coordinates(&v, &a, &b));
    ASSERT_EQ_I(a, -5);
    ASSERT_EQ_I(b, 7);
    psrp_value_free(&v);

    ASSERT_OK(psrp_host_make_size(120, 50, &v));
    ASSERT_OK(psrp_host_read_size(&v, &a, &b));
    ASSERT_EQ_I(a, 120);
    ASSERT_EQ_I(b, 50);
    psrp_value_free(&v);

    ASSERT_OK(psrp_host_make_color(PSRP_COLOR_CYAN, &v));
    ASSERT_OK(psrp_host_read_color(&v, &a));
    ASSERT_EQ_I(a, PSRP_COLOR_CYAN);
    psrp_value_free(&v);
}

/* Survives an XML round trip, which is how these actually travel. */
PSRP_TEST(host_value_survives_serialization)
{
    psrp_value_t v, back;
    psrp_buffer_t xml;
    int32_t w = 0, h = 0;

    psrp_value_init(&v);
    psrp_value_init(&back);
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_host_make_size(80, 25, &v));
    ASSERT_OK(psrp_clixml_serialize(&v, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &back));
    ASSERT_OK(psrp_host_read_size(&back, &w, &h));
    ASSERT_EQ_I(w, 80);
    ASSERT_EQ_I(h, 25);
    psrp_buffer_free(&xml);
    psrp_value_free(&v);
    psrp_value_free(&back);
}

/* Readers must check the T property, not just the shape: a Size is not a
 * Coordinates even though both hold two ints. */
PSRP_TEST(host_value_readers_check_the_type)
{
    psrp_value_t size, coords, color;
    int32_t a = 0, b = 0;

    psrp_value_init(&size);
    psrp_value_init(&coords);
    psrp_value_init(&color);
    ASSERT_OK(psrp_host_make_size(10, 20, &size));
    ASSERT_OK(psrp_host_make_coordinates(1, 2, &coords));
    ASSERT_OK(psrp_host_make_color(1, &color));

    ASSERT_ERR(psrp_host_read_coordinates(&size, &a, &b), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_size(&coords, &a, &b), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_color(&size, &a), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_size(&color, &a, &b), PSRP_ERR_MALFORMED);

    psrp_value_free(&size);
    psrp_value_free(&coords);
    psrp_value_free(&color);
}

PSRP_TEST(host_value_readers_reject_junk)
{
    psrp_value_t v;
    int32_t a = 0, b = 0;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "not an object"));
    ASSERT_ERR(psrp_host_read_size(&v, &a, &b), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_color(&v, &a), PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_host_read_coordinates(NULL, &a, &b), PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

PSRP_TEST(host_makers_reject_null_output)
{
    ASSERT_ERR(psrp_host_make_size(1, 1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_coordinates(1, 1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_color(1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_string("x", NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_make_string(NULL, NULL), PSRP_ERR_INVALID_ARG);
}

/* -------------------------------------------------- _hostDefaultData --- */

PSRP_TEST(host_default_data_has_all_ten_keys)
{
    psrp_host_default_data_t d;
    psrp_value_t v;
    const psrp_value_t *data;
    int32_t key;
    bool seen[10];
    size_t i;

    psrp_host_default_data_defaults(&d);
    psrp_value_init(&v);
    ASSERT_OK(psrp_host_build_default_data(&d, &v));

    data = psrp_object_find(v.as.obj, "data");
    ASSERT_NOT_NULL(data);
    ASSERT_EQ_I(psrp_object_container(data->as.obj), PSRP_CONTAINER_DICT);
    /* 2.2.3.14 lists exactly ten required entries. */
    ASSERT_EQ_SZ(psrp_object_entry_count(data->as.obj), 10u);

    memset(seen, 0, sizeof seen);
    for (i = 0; i < psrp_object_entry_count(data->as.obj); i++) {
        const psrp_dict_entry_t *e = psrp_object_entry(data->as.obj, i);
        ASSERT_EQ_I(e->key.kind, PSRP_VAL_INT32);
        key = e->key.as.i32;
        ASSERT_TRUE(key >= 0 && key <= 9);
        ASSERT_FALSE(seen[key]);      /* no duplicates */
        seen[key] = true;
    }
    for (i = 0; i < 10; i++)
        if (!seen[i]) PSRP_FAIL("key %zu missing from _hostDefaultData", i);

    psrp_value_free(&v);
}

/* Each key must carry the type the table specifies. */
PSRP_TEST(host_default_data_entry_types)
{
    psrp_host_default_data_t d;
    psrp_value_t v;
    const psrp_value_t *data;
    size_t i;
    int32_t a = 0, b = 0;

    psrp_host_default_data_defaults(&d);
    d.window_width = 132;
    d.window_height = 43;
    psrp_value_init(&v);
    ASSERT_OK(psrp_host_build_default_data(&d, &v));
    data = psrp_object_find(v.as.obj, "data");
    ASSERT_NOT_NULL(data);

    for (i = 0; i < psrp_object_entry_count(data->as.obj); i++) {
        const psrp_dict_entry_t *e = psrp_object_entry(data->as.obj, i);
        switch (e->key.as.i32) {
        case 0: case 1:              /* Color */
            ASSERT_OK(psrp_host_read_color(&e->value, &a));
            break;
        case 2: case 3:              /* Coordinates */
            ASSERT_OK(psrp_host_read_coordinates(&e->value, &a, &b));
            break;
        case 4:                      /* bare Int32, not a Size */
            ASSERT_ERR(psrp_host_read_size(&e->value, &a, &b),
                       PSRP_ERR_MALFORMED);
            break;
        case 5: case 6: case 7: case 8:   /* Size */
            ASSERT_OK(psrp_host_read_size(&e->value, &a, &b));
            if (e->key.as.i32 == 6) {
                ASSERT_EQ_I(a, 132);
                ASSERT_EQ_I(b, 43);
            }
            break;
        case 9:                      /* String */
            ASSERT_EQ_I(e->value.kind, PSRP_VAL_OBJECT);
            break;
        default:
            PSRP_FAIL("unexpected key %d", e->key.as.i32);
        }
    }
    psrp_value_free(&v);
}

/* ---------------------------------------------- HostInfo integration -- */

/* A null host still omits the dictionary, as the spec's example shows. */
PSRP_TEST(null_host_omits_default_data)
{
    psrp_init_runspacepool_t init;
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    psrp_init_runspacepool_defaults(&init);
    ASSERT_NULL(init.host.default_data);
    ASSERT_OK(psrp_build_init_runspacepool(&init, &xml));
    ASSERT_FALSE(xml_contains(&xml, "_hostDefaultData"));
    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_isHostNull\">true</B>"));
    psrp_buffer_free(&xml);
}

/* Supplying console data adds the dictionary and flips the flags. */
PSRP_TEST(populated_host_emits_default_data)
{
    psrp_init_runspacepool_t init;
    psrp_host_default_data_t d;
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *host, *hdd;

    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    psrp_init_runspacepool_defaults(&init);
    psrp_host_default_data_defaults(&d);
    d.window_title = "libpsrp test";
    init.host.is_host_null = false;
    init.host.is_host_ui_null = false;
    init.host.is_host_raw_ui_null = false;
    init.host.default_data = &d;

    ASSERT_OK(psrp_build_init_runspacepool(&init, &xml));
    ASSERT_TRUE(xml_contains(&xml, "_hostDefaultData"));
    ASSERT_TRUE(xml_contains(&xml, "<B N=\"_isHostNull\">false</B>"));
    ASSERT_TRUE(xml_contains(&xml, "libpsrp test"));

    /* And it parses back to the expected structure. */
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    host = psrp_object_find(root.as.obj, "HostInfo");
    ASSERT_NOT_NULL(host);
    hdd = psrp_object_find(host->as.obj, "_hostDefaultData");
    ASSERT_NOT_NULL(hdd);
    ASSERT_NOT_NULL(psrp_object_find(hdd->as.obj, "data"));

    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(host_default_data_rejects_null_args)
{
    psrp_host_default_data_t d;
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_host_default_data_defaults(&d);
    ASSERT_ERR(psrp_host_build_default_data(NULL, &v), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_host_build_default_data(&d, NULL), PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(console_color_names),
    PSRP_TEST_CASE(console_color_zero_is_black),
    PSRP_TEST_CASE(color_wrapper_shape_matches_spec_example),
    PSRP_TEST_CASE(coordinates_wrapper_shape),
    PSRP_TEST_CASE(size_wrapper_shape),
    PSRP_TEST_CASE(host_value_round_trips),
    PSRP_TEST_CASE(host_value_survives_serialization),
    PSRP_TEST_CASE(host_value_readers_check_the_type),
    PSRP_TEST_CASE(host_value_readers_reject_junk),
    PSRP_TEST_CASE(host_makers_reject_null_output),
    PSRP_TEST_CASE(host_default_data_has_all_ten_keys),
    PSRP_TEST_CASE(host_default_data_entry_types),
    PSRP_TEST_CASE(null_host_omits_default_data),
    PSRP_TEST_CASE(populated_host_emits_default_data),
    PSRP_TEST_CASE(host_default_data_rejects_null_args),
};

PSRP_TEST_MAIN(cases)
