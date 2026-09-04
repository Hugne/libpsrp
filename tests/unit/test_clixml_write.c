#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

/* Expected strings are PowerShell's PSSerializer output with its pretty-print
 * whitespace removed; we emit compact XML, which is equivalent. */
static void check_xml(const psrp_value_t *v, const char *want)
{
    psrp_buffer_t out;
    psrp_buffer_init(&out);
    ASSERT_OK(psrp_clixml_serialize(v, &out));
    ASSERT_EQ_MEM(out.data, out.len, want, strlen(want));
    psrp_buffer_free(&out);
}

/* ---------------------------------------------------------- primitives -- */

PSRP_TEST(write_null_is_self_closing)
{
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_value_set_null(&v);
    check_xml(&v, "<Nil />");
    psrp_value_free(&v);
}

PSRP_TEST(write_bool)
{
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_value_set_bool(&v, true);
    check_xml(&v, "<B>true</B>");
    psrp_value_set_bool(&v, false);
    check_xml(&v, "<B>false</B>");
    psrp_value_free(&v);
}

PSRP_TEST(write_integers_cover_full_ranges)
{
    psrp_value_t v;
    psrp_value_init(&v);

    psrp_value_set_uint8(&v, 254);            check_xml(&v, "<By>254</By>");
    psrp_value_set_uint8(&v, 0);              check_xml(&v, "<By>0</By>");
    psrp_value_set_int8(&v, -127);            check_xml(&v, "<SB>-127</SB>");
    psrp_value_set_uint16(&v, 65535);         check_xml(&v, "<U16>65535</U16>");
    psrp_value_set_int16(&v, -32767);         check_xml(&v, "<I16>-32767</I16>");
    psrp_value_set_uint32(&v, 4294967295u);   check_xml(&v, "<U32>4294967295</U32>");
    psrp_value_set_int32(&v, -2147483647 - 1);
    check_xml(&v, "<I32>-2147483648</I32>");
    psrp_value_set_int32(&v, 42);             check_xml(&v, "<I32>42</I32>");
    psrp_value_set_uint64(&v, 18446744073709551615ull);
    check_xml(&v, "<U64>18446744073709551615</U64>");
    psrp_value_set_int64(&v, (-9223372036854775807LL - 1));
    check_xml(&v, "<I64>-9223372036854775808</I64>");

    psrp_value_free(&v);
}

PSRP_TEST(write_char_is_numeric_code_unit)
{
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_value_set_char(&v, 97);        /* 'a', per the spec example */
    check_xml(&v, "<C>97</C>");
    psrp_value_free(&v);
}

/* PowerShell uses XML-schema lexical forms: shortest round-trip digits and an
 * uppercase exponent. */
PSRP_TEST(write_double_matches_powershell)
{
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_value_set_double(&v, 12.34);   check_xml(&v, "<Db>12.34</Db>");
    psrp_value_set_double(&v, 1.0);     check_xml(&v, "<Db>1</Db>");
    psrp_value_set_double(&v, 0.1);     check_xml(&v, "<Db>0.1</Db>");
    psrp_value_set_double(&v, 1e20);    check_xml(&v, "<Db>1E+20</Db>");
    psrp_value_free(&v);
}

PSRP_TEST(write_single_matches_powershell)
{
    psrp_value_t v;
    psrp_value_init(&v);
    psrp_value_set_single(&v, 12.34f);  check_xml(&v, "<Sg>12.34</Sg>");
    psrp_value_set_single(&v, 0.1f);    check_xml(&v, "<Sg>0.1</Sg>");
    psrp_value_free(&v);
}

/* Specials use the XML-schema spellings, not .NET's "Infinity". */
PSRP_TEST(write_float_specials)
{
    psrp_value_t v;
    psrp_value_init(&v);

    psrp_value_set_double(&v, (double)INFINITY);   check_xml(&v, "<Db>INF</Db>");
    psrp_value_set_double(&v, -(double)INFINITY);  check_xml(&v, "<Db>-INF</Db>");
    psrp_value_set_double(&v, (double)NAN);        check_xml(&v, "<Db>NaN</Db>");
    psrp_value_set_single(&v, (float)INFINITY);    check_xml(&v, "<Sg>INF</Sg>");
    psrp_value_set_single(&v, (float)NAN);         check_xml(&v, "<Sg>NaN</Sg>");
    psrp_value_free(&v);
}

/* Round-trip precision must survive even where the shortest form is long. */
PSRP_TEST(write_double_roundtrips_awkward_values)
{
    psrp_value_t v;
    psrp_buffer_t out;
    const double values[] = { 1.0 / 3.0, 1e-300, 123456789.123456789, -0.0 };
    size_t i;
    psrp_value_init(&v);
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        char *text;
        psrp_buffer_init(&out);
        psrp_value_set_double(&v, values[i]);
        ASSERT_OK(psrp_clixml_serialize(&v, &out));
        /* Extract the text between <Db> and </Db> and parse it back. */
        ASSERT_OK(psrp_buffer_append_u8(&out, 0));
        text = (char *)out.data + 4;
        text[strlen(text) - 5] = '\0';
        if (strtod(text, NULL) != values[i])
            PSRP_FAIL("double %g did not round-trip via \"%s\"", values[i], text);
        psrp_buffer_free(&out);
    }
    psrp_value_free(&v);
}

PSRP_TEST(write_guid_lowercase)
{
    psrp_value_t v;
    psrp_guid_t g;
    psrp_value_init(&v);
    ASSERT_OK(psrp_guid_parse("792e5b37-4505-47ef-b7d2-8711bb7affa8", &g));
    psrp_value_set_guid(&v, &g);
    check_xml(&v, "<G>792e5b37-4505-47ef-b7d2-8711bb7affa8</G>");
    psrp_value_free(&v);
}

PSRP_TEST(write_byte_array_is_base64)
{
    psrp_value_t v;
    static const uint8_t raw[] = { 1, 2, 3, 4 };
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_bytes(&v, raw, sizeof raw));
    check_xml(&v, "<BA>AQIDBA==</BA>");      /* spec example 2.2.5.1.17 */
    ASSERT_OK(psrp_value_set_bytes(&v, NULL, 0));
    check_xml(&v, "<BA></BA>");
    psrp_value_free(&v);
}

PSRP_TEST(write_text_kinds)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "This is a string"));
    check_xml(&v, "<S>This is a string</S>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_VERSION, "6.2.1.3", 7));
    check_xml(&v, "<Version>6.2.1.3</Version>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_DECIMAL, "12.34", 5));
    check_xml(&v, "<D>12.34</D>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_DURATION, "PT9.0269026S", 12));
    check_xml(&v, "<TS>PT9.0269026S</TS>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_DATETIME,
                                  "2008-04-11T10:42:32.2731993-07:00", 33));
    check_xml(&v, "<DT>2008-04-11T10:42:32.2731993-07:00</DT>");
    psrp_value_free(&v);
}

/* Only the kinds whose spec section cites 2.2.5.3.2 get _xHHHH_ escaping. */
PSRP_TEST(write_escapes_only_escaped_kinds)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_STRING, "a\nb", 3));
    check_xml(&v, "<S>a_x000A_b</S>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_SCRIPTBLOCK, "a\nb", 3));
    check_xml(&v, "<SBK>a_x000A_b</SBK>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_URI, "a\nb", 3));
    check_xml(&v, "<URI>a_x000A_b</URI>");
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_XMLDOC, "a\nb", 3));
    check_xml(&v, "<XD>a_x000A_b</XD>");
    /* Version is not an escaped kind; content passes through. */
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_VERSION, "1.0", 3));
    check_xml(&v, "<Version>1.0</Version>");
    psrp_value_free(&v);
}

/* XML metacharacters are entity-escaped; PowerShell leaves quotes literal. */
PSRP_TEST(write_xml_escaping)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "a<b>&\"c"));
    check_xml(&v, "<S>a&lt;b&gt;&amp;\"c</S>");
    psrp_value_free(&v);
}

PSRP_TEST(write_named_value_emits_n_attribute)
{
    psrp_value_t v;
    psrp_buffer_t out;
    psrp_value_init(&v);
    psrp_buffer_init(&out);
    psrp_value_set_int32(&v, 1);
    ASSERT_OK(psrp_clixml_serialize_named(&v, "A", &out));
    ASSERT_EQ_MEM(out.data, out.len, "<I32 N=\"A\">1</I32>", 18u);
    psrp_buffer_free(&out);
    psrp_value_free(&v);
}

/* ------------------------------------------------------ complex objects -- */

/* Golden: [pscustomobject]@{ A=1; B='x' } */
PSRP_TEST(write_custom_object_matches_powershell)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(o, 0);
    psrp_object_set_type_ref_id(o, 0);
    ASSERT_OK(psrp_object_add_type_name(o,
        "System.Management.Automation.PSCustomObject"));
    ASSERT_OK(psrp_object_add_type_name(o, "System.Object"));

    psrp_value_set_int32(&v, 1);
    ASSERT_OK(psrp_object_add_extended(o, "A", &v));
    ASSERT_OK(psrp_value_set_string(&v, "x"));
    ASSERT_OK(psrp_object_add_extended(o, "B", &v));

    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_xml(&wrapper,
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>System.Management.Automation.PSCustomObject</T>"
            "<T>System.Object</T>"
          "</TN>"
          "<MS><I32 N=\"A\">1</I32><S N=\"B\">x</S></MS>"
        "</Obj>");
    psrp_value_free(&wrapper);
}

/* Golden: @{ k='v' } */
PSRP_TEST(write_dictionary_matches_powershell)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t k, val, wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&k);
    psrp_value_init(&val);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(o, 0);
    psrp_object_set_type_ref_id(o, 0);
    ASSERT_OK(psrp_object_add_type_name(o, "System.Collections.Hashtable"));
    ASSERT_OK(psrp_object_add_type_name(o, "System.Object"));
    psrp_object_set_container(o, PSRP_CONTAINER_DICT);

    ASSERT_OK(psrp_value_set_string(&k, "k"));
    ASSERT_OK(psrp_value_set_string(&val, "v"));
    ASSERT_OK(psrp_object_add_entry(o, &k, &val));

    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_xml(&wrapper,
        "<Obj RefId=\"0\">"
          "<TN RefId=\"0\">"
            "<T>System.Collections.Hashtable</T><T>System.Object</T>"
          "</TN>"
          "<DCT><En><S N=\"Key\">k</S><S N=\"Value\">v</S></En></DCT>"
        "</Obj>");
    psrp_value_free(&wrapper);
}

/* Golden: the inner object of ,@(1,2,3) - shares its type via <TNRef>. */
PSRP_TEST(write_list_with_type_ref)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, wrapper;
    int i;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(o, 1);
    psrp_object_set_type_ref_id(o, 0);   /* no names -> <TNRef> */
    psrp_object_set_container(o, PSRP_CONTAINER_LIST);
    for (i = 1; i <= 3; i++) {
        psrp_value_set_int32(&v, i);
        ASSERT_OK(psrp_object_add_item(o, &v));
    }

    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_xml(&wrapper,
        "<Obj RefId=\"1\"><TNRef RefId=\"0\" />"
        "<LST><I32>1</I32><I32>2</I32><I32>3</I32></LST></Obj>");
    psrp_value_free(&wrapper);
}

PSRP_TEST(write_stack_and_queue_elements)
{
    psrp_container_kind_t kinds[2];
    const char *elems[2];
    size_t i;
    kinds[0] = PSRP_CONTAINER_STACK; elems[0] = "STK";
    kinds[1] = PSRP_CONTAINER_QUEUE; elems[1] = "QUE";

    for (i = 0; i < 2; i++) {
        psrp_object_t *o = psrp_object_new();
        psrp_value_t v, wrapper;
        char want[128];
        ASSERT_NOT_NULL(o);
        psrp_value_init(&v);
        psrp_value_init(&wrapper);
        psrp_object_set_ref_id(o, 0);
        psrp_object_set_container(o, kinds[i]);
        psrp_value_set_int32(&v, 7);
        ASSERT_OK(psrp_object_add_item(o, &v));
        ASSERT_OK(psrp_value_set_object(&wrapper, o));
        snprintf(want, sizeof want,
                 "<Obj RefId=\"0\"><%s><I32>7</I32></%s></Obj>",
                 elems[i], elems[i]);
        check_xml(&wrapper, want);
        psrp_value_free(&wrapper);
    }
}

PSRP_TEST(write_object_with_tostring_and_adapted_props)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(o, 0);
    ASSERT_OK(psrp_object_set_to_string(o, "text form", 9));
    psrp_value_set_int32(&v, 5);
    ASSERT_OK(psrp_object_add_adapted(o, "X", &v));

    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    /* Order: ToString, then <Props>, then <MS>. */
    check_xml(&wrapper,
        "<Obj RefId=\"0\"><ToString>text form</ToString>"
        "<Props><I32 N=\"X\">5</I32></Props></Obj>");
    psrp_value_free(&wrapper);
}

PSRP_TEST(write_nested_objects)
{
    psrp_object_t *outer = psrp_object_new();
    psrp_object_t *inner = psrp_object_new();
    psrp_value_t v, wrapper;
    ASSERT_NOT_NULL(outer);
    ASSERT_NOT_NULL(inner);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);

    psrp_object_set_ref_id(inner, 1);
    ASSERT_OK(psrp_value_set_string(&v, "deep"));
    ASSERT_OK(psrp_object_add_extended(inner, "N", &v));

    psrp_object_set_ref_id(outer, 0);
    ASSERT_OK(psrp_value_set_object(&v, inner));
    ASSERT_OK(psrp_object_add_extended(outer, "Child", &v));

    ASSERT_OK(psrp_value_set_object(&wrapper, outer));
    check_xml(&wrapper,
        "<Obj RefId=\"0\"><MS>"
        "<Obj RefId=\"1\" N=\"Child\"><MS><S N=\"N\">deep</S></MS></Obj>"
        "</MS></Obj>");
    psrp_value_free(&wrapper);
}

PSRP_TEST(write_property_name_is_escaped)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v, wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&wrapper);
    psrp_object_set_ref_id(o, 0);
    psrp_value_set_int32(&v, 1);
    ASSERT_OK(psrp_object_add_extended(o, "a\nb", &v));
    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_xml(&wrapper,
        "<Obj RefId=\"0\"><MS><I32 N=\"a_x000A_b\">1</I32></MS></Obj>");
    psrp_value_free(&wrapper);
}

PSRP_TEST(write_object_without_ref_id_omits_attribute)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t wrapper;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&wrapper);
    ASSERT_OK(psrp_value_set_object(&wrapper, o));
    check_xml(&wrapper, "<Obj></Obj>");
    psrp_value_free(&wrapper);
}

/* --------------------------------------------------- model housekeeping -- */

PSRP_TEST(value_free_is_idempotent_and_recursive)
{
    psrp_value_t v;
    psrp_object_t *o = psrp_object_new();
    psrp_value_t inner;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);
    psrp_value_init(&inner);
    ASSERT_OK(psrp_value_set_string(&inner, "x"));
    ASSERT_OK(psrp_object_add_extended(o, "p", &inner));
    /* add_extended took ownership and reset the source. */
    ASSERT_EQ_I(inner.kind, PSRP_VAL_NULL);
    ASSERT_OK(psrp_value_set_object(&v, o));
    psrp_value_free(&v);
    psrp_value_free(&v);          /* second free must be safe */
    ASSERT_EQ_I(v.kind, PSRP_VAL_NULL);
}

PSRP_TEST(value_setters_replace_previous_content)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "a long string that allocates"));
    psrp_value_set_int32(&v, 3);   /* must free the old text, not leak it */
    ASSERT_EQ_I(v.kind, PSRP_VAL_INT32);
    ASSERT_OK(psrp_value_set_string(&v, "back to text"));
    ASSERT_EQ_I(v.kind, PSRP_VAL_STRING);
    psrp_value_free(&v);
}

PSRP_TEST(value_text_keeps_embedded_nul)
{
    psrp_value_t v;
    static const char raw[] = { 'a', '\0', 'b' };
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_STRING, raw, sizeof raw));
    ASSERT_EQ_SZ(v.as.text.len, 3u);
    check_xml(&v, "<S>a_x0000_b</S>");
    psrp_value_free(&v);
}

PSRP_TEST(value_set_text_rejects_non_text_kind)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_ERR(psrp_value_set_text(&v, PSRP_VAL_INT32, "1", 1),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_value_set_text(&v, PSRP_VAL_BYTES, "1", 1),
               PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);
}

PSRP_TEST(kind_element_lookup_roundtrips)
{
    /* Every kind must have a unique element name that maps back. */
    int k;
    for (k = 0; k <= (int)PSRP_VAL_OBJECT; k++) {
        psrp_value_kind_t kind = (psrp_value_kind_t)k;
        psrp_value_kind_t back;
        const char *elem = psrp_value_kind_element(kind);
        ASSERT_NOT_NULL(elem);
        ASSERT_TRUE(psrp_value_kind_from_element(elem, &back));
        ASSERT_EQ_I(back, kind);
    }
    ASSERT_FALSE(psrp_value_kind_from_element("NoSuchElement", NULL));
}

PSRP_TEST(object_find_prefers_extended)
{
    psrp_object_t *o = psrp_object_new();
    psrp_value_t v;
    const psrp_value_t *found;
    ASSERT_NOT_NULL(o);
    psrp_value_init(&v);

    psrp_value_set_int32(&v, 1);
    ASSERT_OK(psrp_object_add_adapted(o, "dup", &v));
    psrp_value_set_int32(&v, 2);
    ASSERT_OK(psrp_object_add_extended(o, "dup", &v));

    found = psrp_object_find(o, "dup");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ_I(found->as.i32, 2);      /* extended shadows adapted */
    ASSERT_NULL(psrp_object_find(o, "missing"));
    psrp_object_free(o);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(write_null_is_self_closing),
    PSRP_TEST_CASE(write_bool),
    PSRP_TEST_CASE(write_integers_cover_full_ranges),
    PSRP_TEST_CASE(write_char_is_numeric_code_unit),
    PSRP_TEST_CASE(write_double_matches_powershell),
    PSRP_TEST_CASE(write_single_matches_powershell),
    PSRP_TEST_CASE(write_float_specials),
    PSRP_TEST_CASE(write_double_roundtrips_awkward_values),
    PSRP_TEST_CASE(write_guid_lowercase),
    PSRP_TEST_CASE(write_byte_array_is_base64),
    PSRP_TEST_CASE(write_text_kinds),
    PSRP_TEST_CASE(write_escapes_only_escaped_kinds),
    PSRP_TEST_CASE(write_xml_escaping),
    PSRP_TEST_CASE(write_named_value_emits_n_attribute),
    PSRP_TEST_CASE(write_custom_object_matches_powershell),
    PSRP_TEST_CASE(write_dictionary_matches_powershell),
    PSRP_TEST_CASE(write_list_with_type_ref),
    PSRP_TEST_CASE(write_stack_and_queue_elements),
    PSRP_TEST_CASE(write_object_with_tostring_and_adapted_props),
    PSRP_TEST_CASE(write_nested_objects),
    PSRP_TEST_CASE(write_property_name_is_escaped),
    PSRP_TEST_CASE(write_object_without_ref_id_omits_attribute),
    PSRP_TEST_CASE(value_free_is_idempotent_and_recursive),
    PSRP_TEST_CASE(value_setters_replace_previous_content),
    PSRP_TEST_CASE(value_text_keeps_embedded_nul),
    PSRP_TEST_CASE(value_set_text_rejects_non_text_kind),
    PSRP_TEST_CASE(kind_element_lookup_roundtrips),
    PSRP_TEST_CASE(object_find_prefers_extended),
};

PSRP_TEST_MAIN(cases)
