#ifndef _WIN32
/* fileno, dup and dup2 are POSIX; -std=c11 hides them without this, and one
 * case here captures stderr to prove the parsers never write to it. */
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_clixml.h"
#include "psrp/winrm.h"
#include "psrp_test.h"

/* Deep equality, used to prove serialize -> deserialize is lossless. */
static bool values_equal(const psrp_value_t *a, const psrp_value_t *b);

static bool objects_equal(const psrp_object_t *x, const psrp_object_t *y)
{
    size_t i;
    const char *sx, *sy;
    size_t lx = 0, ly = 0;

    if (psrp_object_ref_id(x) != psrp_object_ref_id(y)) return false;
    if (psrp_object_type_ref_id(x) != psrp_object_type_ref_id(y)) return false;
    if (psrp_object_type_name_count(x) != psrp_object_type_name_count(y)) return false;
    for (i = 0; i < psrp_object_type_name_count(x); i++)
        if (strcmp(psrp_object_type_name(x, i), psrp_object_type_name(y, i)) != 0)
            return false;

    sx = psrp_object_to_string(x, &lx);
    sy = psrp_object_to_string(y, &ly);
    if ((sx == NULL) != (sy == NULL)) return false;
    if (sx && (lx != ly || memcmp(sx, sy, lx) != 0)) return false;

    if (psrp_object_adapted_count(x) != psrp_object_adapted_count(y)) return false;
    for (i = 0; i < psrp_object_adapted_count(x); i++) {
        const psrp_property_t *px = psrp_object_adapted(x, i);
        const psrp_property_t *py = psrp_object_adapted(y, i);
        if ((px->name == NULL) != (py->name == NULL)) return false;
        if (px->name && strcmp(px->name, py->name) != 0) return false;
        if (!values_equal(&px->value, &py->value)) return false;
    }
    if (psrp_object_extended_count(x) != psrp_object_extended_count(y)) return false;
    for (i = 0; i < psrp_object_extended_count(x); i++) {
        const psrp_property_t *px = psrp_object_extended(x, i);
        const psrp_property_t *py = psrp_object_extended(y, i);
        if ((px->name == NULL) != (py->name == NULL)) return false;
        if (px->name && strcmp(px->name, py->name) != 0) return false;
        if (!values_equal(&px->value, &py->value)) return false;
    }

    if (psrp_object_container(x) != psrp_object_container(y)) return false;
    if (psrp_object_item_count(x) != psrp_object_item_count(y)) return false;
    for (i = 0; i < psrp_object_item_count(x); i++)
        if (!values_equal(psrp_object_item(x, i), psrp_object_item(y, i)))
            return false;
    if (psrp_object_entry_count(x) != psrp_object_entry_count(y)) return false;
    for (i = 0; i < psrp_object_entry_count(x); i++) {
        const psrp_dict_entry_t *ex = psrp_object_entry(x, i);
        const psrp_dict_entry_t *ey = psrp_object_entry(y, i);
        if (!values_equal(&ex->key, &ey->key)) return false;
        if (!values_equal(&ex->value, &ey->value)) return false;
    }

    {
        const psrp_value_t *px = psrp_object_primitive(x);
        const psrp_value_t *py = psrp_object_primitive(y);
        if ((px == NULL) != (py == NULL)) return false;
        if (px && !values_equal(px, py)) return false;
    }
    return true;
}

static bool values_equal(const psrp_value_t *a, const psrp_value_t *b)
{
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case PSRP_VAL_NULL:   return true;
    case PSRP_VAL_BOOL:   return a->as.b == b->as.b;
    case PSRP_VAL_CHAR:   return a->as.ch == b->as.ch;
    case PSRP_VAL_UINT8:  return a->as.u8 == b->as.u8;
    case PSRP_VAL_INT8:   return a->as.i8 == b->as.i8;
    case PSRP_VAL_UINT16: return a->as.u16 == b->as.u16;
    case PSRP_VAL_INT16:  return a->as.i16 == b->as.i16;
    case PSRP_VAL_UINT32: return a->as.u32 == b->as.u32;
    case PSRP_VAL_INT32:  return a->as.i32 == b->as.i32;
    case PSRP_VAL_UINT64: return a->as.u64 == b->as.u64;
    case PSRP_VAL_INT64:  return a->as.i64 == b->as.i64;
    case PSRP_VAL_SINGLE: return a->as.f32 == b->as.f32;
    case PSRP_VAL_DOUBLE: return a->as.f64 == b->as.f64;
    case PSRP_VAL_GUID:   return psrp_guid_equal(&a->as.guid, &b->as.guid);
    case PSRP_VAL_BYTES:
        return a->as.bytes.len == b->as.bytes.len &&
               (a->as.bytes.len == 0 ||
                memcmp(a->as.bytes.ptr, b->as.bytes.ptr, a->as.bytes.len) == 0);
    case PSRP_VAL_OBJECT: return objects_equal(a->as.obj, b->as.obj);
    default:
        return a->as.text.len == b->as.text.len &&
               (a->as.text.len == 0 ||
                memcmp(a->as.text.ptr, b->as.text.ptr, a->as.text.len) == 0);
    }
}

/* Serialize -> deserialize -> compare. */
static void check_roundtrip(const psrp_value_t *v)
{
    psrp_buffer_t xml;
    psrp_value_t back;
    psrp_buffer_init(&xml);
    psrp_value_init(&back);
    ASSERT_OK(psrp_clixml_serialize(v, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &back));
    if (!values_equal(v, &back)) {
        ASSERT_OK(psrp_buffer_append_u8(&xml, 0));
        PSRP_FAIL("round-trip mismatch via %s", (const char *)xml.data);
    }
    psrp_value_free(&back);
    psrp_buffer_free(&xml);
}

static void parse_ok(const char *xml, psrp_value_t *out)
{
    ASSERT_OK(psrp_clixml_deserialize(xml, strlen(xml), out));
}

/* ---------------------------------------------------------- primitives -- */

PSRP_TEST(read_primitives)
{
    psrp_value_t v;
    psrp_value_init(&v);

    parse_ok("<I32>42</I32>", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_INT32); ASSERT_EQ_I(v.as.i32, 42);
    psrp_value_free(&v);

    parse_ok("<B>true</B>", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_BOOL); ASSERT_TRUE(v.as.b);
    psrp_value_free(&v);

    parse_ok("<B>false</B>", &v);
    ASSERT_FALSE(v.as.b);
    psrp_value_free(&v);

    parse_ok("<Nil />", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_NULL);
    psrp_value_free(&v);

    parse_ok("<C>97</C>", &v);
    ASSERT_EQ_I(v.as.ch, 97);
    psrp_value_free(&v);

    parse_ok("<By>254</By>", &v);
    ASSERT_EQ_I(v.as.u8, 254);
    psrp_value_free(&v);

    parse_ok("<U64>18446744073709551615</U64>", &v);
    ASSERT_EQ_SZ(v.as.u64, 18446744073709551615ull);
    psrp_value_free(&v);

    parse_ok("<I64>-9223372036854775808</I64>", &v);
    ASSERT_TRUE(v.as.i64 == (-9223372036854775807LL - 1));
    psrp_value_free(&v);

    parse_ok("<Db>12.34</Db>", &v);
    ASSERT_TRUE(v.as.f64 == 12.34);
    psrp_value_free(&v);

    parse_ok("<G>792e5b37-4505-47ef-b7d2-8711bb7affa8</G>", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_GUID);
    psrp_value_free(&v);

    parse_ok("<BA>AQIDBA==</BA>", &v);
    ASSERT_EQ_SZ(v.as.bytes.len, 4u);
    ASSERT_EQ_I(v.as.bytes.ptr[3], 4);
    psrp_value_free(&v);

    parse_ok("<Version>6.2.1.3</Version>", &v);
    ASSERT_EQ_STR(v.as.text.ptr, "6.2.1.3");
    psrp_value_free(&v);
}

/* XML entities are resolved by the parser; _xHHHH_ by us. */
PSRP_TEST(read_decodes_both_escaping_layers)
{
    psrp_value_t v;
    psrp_value_init(&v);

    parse_ok("<S>Order_x000A_Details</S>", &v);
    ASSERT_EQ_SZ(v.as.text.len, 13u);
    ASSERT_EQ_MEM(v.as.text.ptr, v.as.text.len, "Order\nDetails", 13u);
    psrp_value_free(&v);

    parse_ok("<S>a&lt;b&gt;&amp;c</S>", &v);
    ASSERT_EQ_STR(v.as.text.ptr, "a<b>&c");
    psrp_value_free(&v);

    /* Astral character arrives as two surrogate escapes. */
    parse_ok("<S>_xD83D__xDCA9_</S>", &v);
    ASSERT_EQ_MEM(v.as.text.ptr, v.as.text.len, "\xF0\x9F\x92\xA9", 4u);
    psrp_value_free(&v);

    /* Version is not an escaped kind: content is literal. */
    parse_ok("<Version>1_x0041_2</Version>", &v);
    ASSERT_EQ_STR(v.as.text.ptr, "1_x0041_2");
    psrp_value_free(&v);
}

PSRP_TEST(read_float_specials)
{
    psrp_value_t v;
    psrp_value_init(&v);
    parse_ok("<Db>INF</Db>", &v);
    ASSERT_TRUE(v.as.f64 > 1.7976931348623157e308);
    psrp_value_free(&v);
    parse_ok("<Db>-INF</Db>", &v);
    ASSERT_TRUE(v.as.f64 < -1.7976931348623157e308);
    psrp_value_free(&v);
    parse_ok("<Db>NaN</Db>", &v);
    ASSERT_TRUE(v.as.f64 != v.as.f64);
    psrp_value_free(&v);
}

PSRP_TEST(read_empty_elements)
{
    psrp_value_t v;
    psrp_value_init(&v);
    parse_ok("<S />", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_STRING);
    ASSERT_EQ_SZ(v.as.text.len, 0u);
    psrp_value_free(&v);

    parse_ok("<BA />", &v);
    ASSERT_EQ_SZ(v.as.bytes.len, 0u);
    psrp_value_free(&v);

    parse_ok("<S></S>", &v);
    ASSERT_EQ_SZ(v.as.text.len, 0u);
    psrp_value_free(&v);
}

/* ------------------------------------------------------ complex objects -- */

PSRP_TEST(read_custom_object)
{
    psrp_value_t v;
    const psrp_value_t *a;
    psrp_value_init(&v);
    parse_ok(
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.PSCustomObject</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<MS><I32 N=\"A\">1</I32><S N=\"B\">x</S></MS>"
        "</Obj>", &v);

    ASSERT_EQ_I(v.kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_I(psrp_object_ref_id(v.as.obj), 0);
    ASSERT_EQ_SZ(psrp_object_type_name_count(v.as.obj), 2u);
    ASSERT_EQ_STR(psrp_object_type_name(v.as.obj, 1), "System.Object");
    ASSERT_EQ_SZ(psrp_object_extended_count(v.as.obj), 2u);
    a = psrp_object_find(v.as.obj, "A");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ_I(a->as.i32, 1);
    a = psrp_object_find(v.as.obj, "B");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ_STR(a->as.text.ptr, "x");
    psrp_value_free(&v);
}

PSRP_TEST(read_dictionary)
{
    psrp_value_t v;
    const psrp_dict_entry_t *e;
    psrp_value_init(&v);
    parse_ok(
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\"><T>System.Collections.Hashtable</T></TN>"
          "<DCT><En><S N=\"Key\">k</S><S N=\"Value\">v</S></En></DCT>"
        "</Obj>", &v);
    ASSERT_EQ_I(psrp_object_container(v.as.obj), PSRP_CONTAINER_DICT);
    ASSERT_EQ_SZ(psrp_object_entry_count(v.as.obj), 1u);
    e = psrp_object_entry(v.as.obj, 0);
    ASSERT_EQ_STR(e->key.as.text.ptr, "k");
    ASSERT_EQ_STR(e->value.as.text.ptr, "v");
    psrp_value_free(&v);
}

PSRP_TEST(read_list_and_type_ref)
{
    psrp_value_t v;
    psrp_value_init(&v);
    parse_ok("<Obj RefId=\"1\"><TNRef RefId=\"0\" />"
             "<LST><I32>1</I32><I32>2</I32><I32>3</I32></LST></Obj>", &v);
    ASSERT_EQ_I(psrp_object_ref_id(v.as.obj), 1);
    ASSERT_EQ_I(psrp_object_type_ref_id(v.as.obj), 0);
    ASSERT_EQ_SZ(psrp_object_type_name_count(v.as.obj), 0u);
    ASSERT_EQ_I(psrp_object_container(v.as.obj), PSRP_CONTAINER_LIST);
    ASSERT_EQ_SZ(psrp_object_item_count(v.as.obj), 3u);
    ASSERT_EQ_I(psrp_object_item(v.as.obj, 2)->as.i32, 3);
    psrp_value_free(&v);
}

PSRP_TEST(read_tostring_and_adapted_props)
{
    psrp_value_t v;
    size_t len = 0;
    psrp_value_init(&v);
    parse_ok("<Obj RefId=\"0\"><ToString>a_x000A_b</ToString>"
             "<Props><I32 N=\"X\">5</I32></Props></Obj>", &v);
    ASSERT_EQ_MEM(psrp_object_to_string(v.as.obj, &len), len, "a\nb", 3u);
    ASSERT_EQ_SZ(psrp_object_adapted_count(v.as.obj), 1u);
    ASSERT_EQ_I(psrp_object_adapted(v.as.obj, 0)->value.as.i32, 5);
    psrp_value_free(&v);
}

PSRP_TEST(read_nested_objects)
{
    psrp_value_t v;
    const psrp_value_t *child;
    psrp_value_init(&v);
    parse_ok("<Obj RefId=\"0\"><MS>"
             "<Obj RefId=\"1\" N=\"Child\"><MS><S N=\"N\">deep</S></MS></Obj>"
             "</MS></Obj>", &v);
    child = psrp_object_find(v.as.obj, "Child");
    ASSERT_NOT_NULL(child);
    ASSERT_EQ_I(child->kind, PSRP_VAL_OBJECT);
    ASSERT_EQ_STR(psrp_object_find(child->as.obj, "N")->as.text.ptr, "deep");
    psrp_value_free(&v);
}

/* PSSerializer wraps output in <Objs> and pretty-prints it; both must parse. */
PSRP_TEST(read_accepts_objs_wrapper_and_whitespace)
{
    psrp_value_t v;
    psrp_value_init(&v);
    parse_ok("<Objs Version=\"1.1.0.1\" "
             "xmlns=\"http://schemas.microsoft.com/powershell/2004/04\">\r\n"
             "  <S>Order_x000A_Details</S>\r\n</Objs>", &v);
    ASSERT_EQ_I(v.kind, PSRP_VAL_STRING);
    ASSERT_EQ_MEM(v.as.text.ptr, v.as.text.len, "Order\nDetails", 13u);
    psrp_value_free(&v);
}

PSRP_TEST(read_accepts_pretty_printed_object)
{
    psrp_value_t v;
    psrp_value_init(&v);
    parse_ok(
        "<Obj RefId=\"0\">\n"
        "  <TN RefId=\"0\">\n"
        "    <T>System.Object</T>\n"
        "  </TN>\n"
        "  <MS>\n"
        "    <I32 N=\"A\">1</I32>\n"
        "  </MS>\n"
        "</Obj>\n", &v);
    ASSERT_EQ_SZ(psrp_object_type_name_count(v.as.obj), 1u);
    ASSERT_EQ_SZ(psrp_object_extended_count(v.as.obj), 1u);
    psrp_value_free(&v);
}

/* --------------------------------------------------------- round trips -- */

PSRP_TEST(roundtrip_all_primitive_kinds)
{
    psrp_value_t v;
    psrp_guid_t g;
    static const uint8_t raw[] = { 0, 1, 2, 250, 255 };
    psrp_value_init(&v);

    psrp_value_set_null(&v);                      check_roundtrip(&v);
    psrp_value_set_bool(&v, true);                check_roundtrip(&v);
    psrp_value_set_bool(&v, false);               check_roundtrip(&v);
    psrp_value_set_char(&v, 0xFFFF);              check_roundtrip(&v);
    psrp_value_set_uint8(&v, 255);                check_roundtrip(&v);
    psrp_value_set_int8(&v, -128);                check_roundtrip(&v);
    psrp_value_set_uint16(&v, 65535);             check_roundtrip(&v);
    psrp_value_set_int16(&v, -32768);             check_roundtrip(&v);
    psrp_value_set_uint32(&v, 4294967295u);       check_roundtrip(&v);
    psrp_value_set_int32(&v, -2147483647 - 1);    check_roundtrip(&v);
    psrp_value_set_uint64(&v, 18446744073709551615ull); check_roundtrip(&v);
    psrp_value_set_int64(&v, -9223372036854775807LL - 1); check_roundtrip(&v);
    psrp_value_set_single(&v, 12.34f);            check_roundtrip(&v);
    psrp_value_set_double(&v, 1.0 / 3.0);         check_roundtrip(&v);
    psrp_value_set_double(&v, 1e20);              check_roundtrip(&v);

    ASSERT_OK(psrp_guid_parse("792e5b37-4505-47ef-b7d2-8711bb7affa8", &g));
    psrp_value_set_guid(&v, &g);                  check_roundtrip(&v);

    ASSERT_OK(psrp_value_set_bytes(&v, raw, sizeof raw));  check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_bytes(&v, NULL, 0));          check_roundtrip(&v);

    ASSERT_OK(psrp_value_set_string(&v, "plain"));         check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_string(&v, ""));              check_roundtrip(&v);
    /* Content that stresses both escaping layers at once. */
    ASSERT_OK(psrp_value_set_string(&v, "a<b>&\"c\n\td_x0020_\xE2\x82\xAC"));
    check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_VERSION, "6.2.1.3", 7));
    check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_DECIMAL, "12.34", 5));
    check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_SCRIPTBLOCK, "get-command\n", 12));
    check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_URI, "http://a/b?c=d&e", 16));
    check_roundtrip(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_XMLDOC, "<a b=\"c\">d</a>", 14));
    check_roundtrip(&v);

    psrp_value_free(&v);
}

PSRP_TEST(roundtrip_complex_objects)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(o, 0);
    psrp_object_set_type_ref_id(o, 0);
    ASSERT_OK(psrp_object_add_type_name(o, "System.Object"));
    ASSERT_OK(psrp_object_set_to_string(o, "text\nform", 9));
    psrp_value_set_int32(&v, 5);
    ASSERT_OK(psrp_object_add_adapted(o, "adapted", &v));
    ASSERT_OK(psrp_value_set_string(&v, "ext"));
    ASSERT_OK(psrp_object_add_extended(o, "extended", &v));
    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_roundtrip(&wrapper);
    psrp_value_free(&wrapper);
}

PSRP_TEST(roundtrip_containers)
{
    psrp_container_kind_t kinds[3];
    size_t i;
    kinds[0] = PSRP_CONTAINER_LIST;
    kinds[1] = PSRP_CONTAINER_STACK;
    kinds[2] = PSRP_CONTAINER_QUEUE;

    for (i = 0; i < 3; i++) {
        psrp_object_t *o = psrp_object_new();
        psrp_value_t v, wrapper;
        int j;
        ASSERT_NOT_NULL(o);
        psrp_value_init(&v);
        psrp_value_init(&wrapper);
        psrp_object_set_ref_id(o, 0);
        psrp_object_set_container(o, kinds[i]);
        for (j = 0; j < 4; j++) {
            psrp_value_set_int32(&v, j);
            ASSERT_OK(psrp_object_add_item(o, &v));
        }
        ASSERT_OK(psrp_value_set_object(&wrapper, o));
        check_roundtrip(&wrapper);
        psrp_value_free(&wrapper);
    }
}

PSRP_TEST(roundtrip_dictionary_and_nesting)
{
    psrp_object_t *outer = psrp_object_new();
    psrp_object_t *inner = psrp_object_new();
    psrp_value_t k, val, wrapper;
    ASSERT_NOT_NULL(outer);
    ASSERT_NOT_NULL(inner);
    psrp_value_init(&k);
    psrp_value_init(&val);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(inner, 1);
    psrp_object_set_container(inner, PSRP_CONTAINER_LIST);
    psrp_value_set_int32(&val, 9);
    ASSERT_OK(psrp_object_add_item(inner, &val));

    psrp_object_set_ref_id(outer, 0);
    psrp_object_set_container(outer, PSRP_CONTAINER_DICT);
    ASSERT_OK(psrp_value_set_string(&k, "key\nwith escape"));
    ASSERT_OK(psrp_value_set_object(&val, inner));
    ASSERT_OK(psrp_object_add_entry(outer, &k, &val));

    ASSERT_OK(psrp_value_set_object(&wrapper, outer));
    check_roundtrip(&wrapper);
    psrp_value_free(&wrapper);
}

/* ------------------------------------------------------------ failures -- */

PSRP_TEST(read_rejects_bad_xml)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_clixml_deserialize("<S>unclosed", 11, &v), PSRP_ERR_XML);
    ASSERT_ERR(psrp_clixml_deserialize("<S></B>", 7, &v), PSRP_ERR_XML);
    ASSERT_ERR(psrp_clixml_deserialize("not xml at all", 14, &v), PSRP_ERR_XML);
    /* An empty document is not well-formed XML, so the XML layer rejects it
     * before we ever look for a root value. */
    ASSERT_ERR(psrp_clixml_deserialize("", 0, &v), PSRP_ERR_XML);
    psrp_value_free(&v);
}

PSRP_TEST(read_rejects_bad_numbers)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_clixml_deserialize("<I32>abc</I32>", 14, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<I32>12x</I32>", 14, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<I32></I32>", 11, &v),
               PSRP_ERR_MALFORMED);
    /* Out of range for the declared width. */
    ASSERT_ERR(psrp_clixml_deserialize("<By>256</By>", 12, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<I16>32768</I16>", 16, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<I32>2147483648</I32>", 21, &v),
               PSRP_ERR_MALFORMED);
    /* A negative value in an unsigned element must not wrap. */
    ASSERT_ERR(psrp_clixml_deserialize("<U32>-1</U32>", 13, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<B>maybe</B>", 12, &v),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_clixml_deserialize("<G>not-a-guid</G>", 17, &v),
               PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

PSRP_TEST(read_rejects_unknown_element)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_clixml_deserialize("<Bogus>1</Bogus>", 16, &v),
               PSRP_ERR_UNSUPPORTED);
    psrp_value_free(&v);
}

PSRP_TEST(read_rejects_child_element_in_primitive)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_clixml_deserialize("<S><I32>1</I32></S>", 19, &v),
               PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

/* Malformed input must not put anything on the caller's stderr.
 *
 * A library writing to a terminal it does not own is wrong regardless, and
 * malformed input is an ordinary outcome here -- it is what a hostile or
 * broken server sends. XmlLite is silent; libxml2 is not unless told, and
 * telling it has been got wrong twice. TODO PSRP-33 suppressed parse
 * diagnostics with parser flags, and TODO PSRP-49 found that an ENCODING
 * error takes a route those flags never covered, which surfaced as "input
 * conversion failed due to input error, bytes 0x00 0x00 0x00 0x00" on a CI
 * runner whose libxml2 was older than any development machine's.
 *
 * Both times the property was checked by something outside the suite noticing
 * output, which only works when the version in front of you happens to emit
 * it. This asserts the property directly, so it holds on whichever libxml2 is
 * installed and on the XmlLite backend too.
 *
 * The redirection is done to the file descriptor rather than with freopen,
 * because the descriptor can be put back exactly as it was: freopen has no
 * portable way to reopen the original stderr, and reopening the null device
 * instead would silence anything a later case tried to report.
 */
#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  define psrp_dup    _dup
#  define psrp_dup2   _dup2
#  define psrp_fileno _fileno
#  define psrp_close  _close
#  define psrp_open   _open
#  define PSRP_WRONLY (_O_WRONLY | _O_CREAT | _O_TRUNC)
#  define PSRP_MODE   (_S_IREAD | _S_IWRITE)
#  include <sys/stat.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define psrp_dup    dup
#  define psrp_dup2   dup2
#  define psrp_fileno fileno
#  define psrp_close  close
#  define psrp_open   open
#  define PSRP_WRONLY (O_WRONLY | O_CREAT | O_TRUNC)
#  define PSRP_MODE   0600
#endif

static void feed_the_parsers(void)
{
    /* Shapes that reach different layers: a NUL run is an encoding error, a
     * bad declaration is a decoding one, the rest are plain parse failures.
     * Every one of them must be silent. */
    static const struct { const char *data; size_t len; } kBad[] = {
        { "<S>\x01\x02\x03\x04</S>", 12 },
        { "<?xml version=\"1.0\" encoding=\"utf-8\"?><S>\xff\xfe\xfd</S>", 51 },
        { "<?xml version=\"1.0\" encoding=\"no-such-encoding\"?><S>x</S>", 56 },
        { "<S>unclosed", 11 },
        { "<S></B>", 7 },
        { "not xml at all", 14 },
        { "<<<<", 4 },
        { "", 0 },
        { "<Obj RefId=\"0\"><MS><I32 N=\"x\">notanumber</I32></MS></Obj>", 57 },
    };
    /* A NUL run cannot go in the table above, since the initialiser would
     * measure it wrong; it is the case that caused PSRP-49, so it is here. */
    static const char kNuls[] = { '<', 'S', '>', 0, 0, 0, 0, '<', '/', 'S', '>' };
    size_t i;

    for (i = 0; i <= sizeof kBad / sizeof kBad[0]; i++) {
        const char *data = i < sizeof kBad / sizeof kBad[0]
                         ? kBad[i].data : kNuls;
        size_t len = i < sizeof kBad / sizeof kBad[0]
                   ? kBad[i].len : sizeof kNuls;
        psrp_value_t v;
        winrm_shell_info_t info;

        psrp_value_init(&v);
        (void)psrp_clixml_deserialize(data, len, &v);
        psrp_value_free(&v);

        /* The enumeration parser reads server XML through the same seam. */
        if (winrm_parse_shell(data, len, &info) == PSRP_OK)
            winrm_shell_info_free(&info);
    }
}

PSRP_TEST(malformed_input_is_parsed_silently)
{
    static const char *const kPath = "stderr-capture.tmp";
    int saved, captured;
    long produced;
    FILE *f;

    fflush(stderr);
    saved = psrp_dup(psrp_fileno(stderr));
    captured = psrp_open(kPath, PSRP_WRONLY, PSRP_MODE);
    if (saved < 0 || captured < 0) {
        if (saved >= 0) psrp_close(saved);
        if (captured >= 0) psrp_close(captured);
        printf("  (cannot redirect stderr here; skipping)\n");
        return;
    }
    psrp_dup2(captured, psrp_fileno(stderr));

    feed_the_parsers();

    fflush(stderr);
    psrp_dup2(saved, psrp_fileno(stderr));
    psrp_close(captured);
    psrp_close(saved);

    f = fopen(kPath, "rb");
    produced = 0;
    if (f) {
        char buf[256];
        fseek(f, 0, SEEK_END);
        produced = ftell(f);
        if (produced > 0) {
            printf("  the parser wrote %ld byte(s) to stderr:\n", produced);
            rewind(f);
            while (fgets(buf, sizeof buf, f)) printf("    %s", buf);
        }
        fclose(f);
    }
    remove(kPath);
    ASSERT_TRUE(produced == 0);
}

PSRP_TEST(read_rejects_null_args)
{
    ASSERT_ERR(psrp_clixml_deserialize("<Nil />", 7, NULL),
               PSRP_ERR_INVALID_ARG);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(read_primitives),
    PSRP_TEST_CASE(read_decodes_both_escaping_layers),
    PSRP_TEST_CASE(read_float_specials),
    PSRP_TEST_CASE(read_empty_elements),
    PSRP_TEST_CASE(read_custom_object),
    PSRP_TEST_CASE(read_dictionary),
    PSRP_TEST_CASE(read_list_and_type_ref),
    PSRP_TEST_CASE(read_tostring_and_adapted_props),
    PSRP_TEST_CASE(read_nested_objects),
    PSRP_TEST_CASE(read_accepts_objs_wrapper_and_whitespace),
    PSRP_TEST_CASE(read_accepts_pretty_printed_object),
    PSRP_TEST_CASE(roundtrip_all_primitive_kinds),
    PSRP_TEST_CASE(roundtrip_complex_objects),
    PSRP_TEST_CASE(roundtrip_containers),
    PSRP_TEST_CASE(roundtrip_dictionary_and_nesting),
    PSRP_TEST_CASE(read_rejects_bad_xml),
    PSRP_TEST_CASE(read_rejects_bad_numbers),
    PSRP_TEST_CASE(read_rejects_unknown_element),
    PSRP_TEST_CASE(read_rejects_child_element_in_primitive),
    PSRP_TEST_CASE(read_rejects_null_args),
    PSRP_TEST_CASE(malformed_input_is_parsed_silently),
};

PSRP_TEST_MAIN(cases)
