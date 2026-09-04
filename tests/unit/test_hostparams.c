/* Host method parameter encoding (2.2.6) and the Primitive Dictionary
 * (2.2.3.18), plus the deep clone both rely on. */

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

/* Parses XML into a value the caller must free. */
static void parse(const char *xml, size_t n, psrp_value_t *out)
{
    psrp_value_init(out);
    if (psrp_clixml_deserialize(xml, n, out) != PSRP_OK) {
        psrp_value_free(out);
        psrp_value_init(out);
    }
}

/* ------------------------------------------------- parameter list ------- */

PSRP_TEST(param_list_counts_and_indexes)
{
    /* An `mp` as it arrives on the wire: a list object holding two values. */
    static const char xml[] =
        "<Obj RefId=\"0\"><TN RefId=\"0\"><T>System.Collections.ArrayList</T>"
        "<T>System.Object</T></TN><LST><S>hello</S><I32>7</I32></LST></Obj>";
    psrp_value_t mp;
    const psrp_value_t *p;

    parse(xml, sizeof xml - 1, &mp);
    ASSERT_EQ_I((int)mp.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_host_param_count(&mp), 2u);

    p = psrp_host_param(&mp, 0);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_I((int)p->kind, (int)PSRP_VAL_STRING);
    ASSERT_EQ_STR(p->as.text.ptr, "hello");

    p = psrp_host_param(&mp, 1);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_I(p->as.i32, 7);

    ASSERT_NULL(psrp_host_param(&mp, 2));
    psrp_value_free(&mp);
}

PSRP_TEST(param_count_of_absent_mp_is_zero)
{
    psrp_value_t v;
    psrp_value_init(&v);              /* Null */
    ASSERT_EQ_SZ(psrp_host_param_count(&v), 0u);
    ASSERT_EQ_SZ(psrp_host_param_count(NULL), 0u);
    ASSERT_NULL(psrp_host_param(NULL, 0));
    psrp_value_free(&v);
}

/* ------------------------------------------------------- unwrapping ----- */

PSRP_TEST(plain_parameter_is_not_encoded)
{
    /* 2.2.6.1.1: a serializable value travels as itself, so unwrap must give
     * it back untouched rather than looking for a wrapper that is not there. */
    psrp_value_t v;
    const psrp_value_t *inner;
    const char *type = "unchanged";

    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "plain"));
    inner = psrp_host_param_unwrap(&v, &type);
    ASSERT_TRUE(inner == &v);
    ASSERT_NULL(type);
    psrp_value_free(&v);
}

PSRP_TEST(list_parameter_unwraps_to_its_value)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"T\">System.Collections.ArrayList</S>"
        "<Obj N=\"V\" RefId=\"1\"><LST><S>a</S><S>b</S></LST></Obj>"
        "</MS></Obj>";
    psrp_value_t v;
    const psrp_value_t *inner;
    const char *type = NULL;

    parse(xml, sizeof xml - 1, &v);
    ASSERT_EQ_I((int)v.kind, (int)PSRP_VAL_OBJECT);

    inner = psrp_host_param_unwrap(&v, &type);
    ASSERT_NOT_NULL(inner);
    ASSERT_TRUE(inner != &v);
    ASSERT_EQ_STR(type, "System.Collections.ArrayList");
    ASSERT_EQ_I((int)inner->kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_item_count(inner->as.obj), 2u);
    psrp_value_free(&v);
}

PSRP_TEST(object_with_extra_properties_is_not_a_wrapper)
{
    /* T and V are short enough to collide with real property names. Treating
     * this as a wrapper would silently discard the Other property. */
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"T\">System.String</S><S N=\"V\">x</S><S N=\"Other\">y</S>"
        "</MS></Obj>";
    psrp_value_t v;
    const psrp_value_t *inner;
    const char *type = NULL;

    parse(xml, sizeof xml - 1, &v);
    inner = psrp_host_param_unwrap(&v, &type);
    ASSERT_TRUE(inner == &v);
    ASSERT_NULL(type);
    psrp_value_free(&v);
}

PSRP_TEST(unwrap_of_null_is_null)
{
    ASSERT_NULL(psrp_host_param_unwrap(NULL, NULL));
}

/* ----------------------------------------------------------- arrays ----- */

PSRP_TEST(array_parameter_exposes_elements_and_dimensions)
{
    /* The 2.2.6.1.4 shape: a 2x3 array flattened deepest-first. */
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<Obj N=\"mae\" RefId=\"1\"><LST>"
        "<I32>1</I32><I32>2</I32><I32>3</I32>"
        "<I32>4</I32><I32>5</I32><I32>6</I32></LST></Obj>"
        "<Obj N=\"mal\" RefId=\"2\"><LST><I32>2</I32><I32>3</I32></LST></Obj>"
        "</MS></Obj>";
    psrp_value_t v;
    const psrp_value_t *elems = NULL, *dims = NULL;

    parse(xml, sizeof xml - 1, &v);
    ASSERT_OK(psrp_host_param_array(&v, &elems, &dims));
    ASSERT_NOT_NULL(elems);
    ASSERT_NOT_NULL(dims);
    ASSERT_EQ_SZ(psrp_object_item_count(elems->as.obj), 6u);
    ASSERT_EQ_SZ(psrp_object_item_count(dims->as.obj), 2u);
    ASSERT_EQ_I(psrp_object_item(dims->as.obj, 0)->as.i32, 2);
    ASSERT_EQ_I(psrp_object_item(dims->as.obj, 1)->as.i32, 3);
    /* Deepest-first ordering means a[0,1] is the second element. */
    ASSERT_EQ_I(psrp_object_item(elems->as.obj, 1)->as.i32, 2);
    psrp_value_free(&v);
}

PSRP_TEST(array_requires_at_least_one_dimension)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<Obj N=\"mae\" RefId=\"1\"><LST></LST></Obj>"
        "<Obj N=\"mal\" RefId=\"2\"><LST></LST></Obj>"
        "</MS></Obj>";
    psrp_value_t v;
    parse(xml, sizeof xml - 1, &v);
    ASSERT_ERR(psrp_host_param_array(&v, NULL, NULL), PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

PSRP_TEST(array_rejects_a_plain_value)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "not an array"));
    ASSERT_ERR(psrp_host_param_array(&v, NULL, NULL), PSRP_ERR_MALFORMED);
    psrp_value_free(&v);
}

/* --------------------------------------------- primitive dictionary ----- */

PSRP_TEST(primitive_dictionary_round_trips)
{
    psrp_value_t d, n;
    psrp_buffer_t out;
    psrp_value_t back;
    const psrp_dict_entry_t *e;

    psrp_value_init(&d);
    ASSERT_OK(psrp_primitive_dictionary_new(&d));
    ASSERT_OK(psrp_primitive_dictionary_add_string(&d, "greeting", "hi"));

    psrp_value_init(&n);
    psrp_value_set_int32(&n, 42);
    ASSERT_OK(psrp_primitive_dictionary_add(&d, "answer", &n));
    psrp_value_free(&n);

    psrp_buffer_init(&out);
    ASSERT_OK(psrp_clixml_serialize(&d, &out));
    ASSERT_TRUE(xml_contains(&out,
        "System.Management.Automation.PSPrimitiveDictionary"));
    ASSERT_TRUE(xml_contains(&out, "System.Collections.Hashtable"));
    ASSERT_TRUE(xml_contains(&out, "<DCT>"));

    parse((const char *)out.data, out.len, &back);
    ASSERT_EQ_I((int)back.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_entry_count(back.as.obj), 2u);
    e = psrp_object_entry(back.as.obj, 0);
    ASSERT_EQ_STR(e->key.as.text.ptr, "greeting");
    ASSERT_EQ_STR(e->value.as.text.ptr, "hi");
    e = psrp_object_entry(back.as.obj, 1);
    ASSERT_EQ_STR(e->key.as.text.ptr, "answer");
    ASSERT_EQ_I(e->value.as.i32, 42);

    psrp_value_free(&back);
    psrp_buffer_free(&out);
    psrp_value_free(&d);
}

PSRP_TEST(primitive_dictionary_refuses_securestring_and_scriptblock)
{
    /* 2.2.3.18 excludes both by name. A SecureString in particular could not
     * be read anyway: this bag is sent before any key exchange. */
    psrp_value_t d, v;

    psrp_value_init(&d);
    ASSERT_OK(psrp_primitive_dictionary_new(&d));

    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_SECURESTRING, "AAAA", 4));
    ASSERT_ERR(psrp_primitive_dictionary_add(&d, "pw", &v),
               PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);

    ASSERT_OK(psrp_value_set_text(&v, PSRP_VAL_SCRIPTBLOCK, "{ 1 }", 5));
    ASSERT_ERR(psrp_primitive_dictionary_add(&d, "sb", &v),
               PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);

    ASSERT_EQ_SZ(psrp_object_entry_count(d.as.obj), 0u);
    psrp_value_free(&d);
}

PSRP_TEST(primitive_dictionary_accepts_a_list_and_a_nested_dictionary)
{
    psrp_value_t d, inner, list, item;
    psrp_object_t *lst;

    psrp_value_init(&d);
    ASSERT_OK(psrp_primitive_dictionary_new(&d));

    lst = psrp_object_new();
    ASSERT_NOT_NULL(lst);
    psrp_object_set_ref_id(lst, 1);
    psrp_object_set_container(lst, PSRP_CONTAINER_LIST);
    psrp_value_init(&item);
    ASSERT_OK(psrp_value_set_string(&item, "one"));
    ASSERT_OK(psrp_object_add_item(lst, &item));
    psrp_value_free(&item);
    psrp_value_init(&list);
    ASSERT_OK(psrp_value_set_object(&list, lst));
    ASSERT_OK(psrp_primitive_dictionary_add(&d, "names", &list));
    psrp_value_free(&list);

    psrp_value_init(&inner);
    ASSERT_OK(psrp_primitive_dictionary_new(&inner));
    ASSERT_OK(psrp_primitive_dictionary_add_string(&inner, "k", "v"));
    ASSERT_OK(psrp_primitive_dictionary_add(&d, "nested", &inner));
    psrp_value_free(&inner);

    ASSERT_EQ_SZ(psrp_object_entry_count(d.as.obj), 2u);
    psrp_value_free(&d);
}

PSRP_TEST(primitive_dictionary_refuses_a_list_of_objects)
{
    /* A list is only allowed when its items are primitives. */
    psrp_value_t d, list, item;
    psrp_object_t *lst, *inner;

    psrp_value_init(&d);
    ASSERT_OK(psrp_primitive_dictionary_new(&d));

    lst = psrp_object_new();
    ASSERT_NOT_NULL(lst);
    psrp_object_set_container(lst, PSRP_CONTAINER_LIST);
    inner = psrp_object_new();
    ASSERT_NOT_NULL(inner);
    psrp_value_init(&item);
    ASSERT_OK(psrp_value_set_object(&item, inner));
    ASSERT_OK(psrp_object_add_item(lst, &item));
    psrp_value_free(&item);

    psrp_value_init(&list);
    ASSERT_OK(psrp_value_set_object(&list, lst));
    ASSERT_ERR(psrp_primitive_dictionary_add(&d, "bad", &list),
               PSRP_ERR_INVALID_ARG);
    psrp_value_free(&list);
    psrp_value_free(&d);
}

PSRP_TEST(primitive_dictionary_rejects_a_non_dictionary)
{
    psrp_value_t v;
    psrp_value_init(&v);
    ASSERT_OK(psrp_value_set_string(&v, "x"));
    ASSERT_ERR(psrp_primitive_dictionary_add_string(&v, "k", "v"),
               PSRP_ERR_INVALID_ARG);
    psrp_value_free(&v);
}

/* ------------------------------------------------------------ clone ----- */

PSRP_TEST(clone_copies_deeply)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><TN RefId=\"0\"><T>Some.Type</T></TN>"
        "<ToString>text</ToString>"
        "<Props><S N=\"a\">1</S></Props>"
        "<MS><Obj N=\"b\" RefId=\"1\"><LST><I32>9</I32></LST></Obj></MS>"
        "</Obj>";
    psrp_value_t src, copy;
    const psrp_value_t *b;

    parse(xml, sizeof xml - 1, &src);
    psrp_value_init(&copy);
    ASSERT_OK(psrp_value_clone(&src, &copy));

    /* Freeing the original must not disturb the copy: that is the whole
     * reason clone exists, since every add_* takes ownership. */
    psrp_value_free(&src);

    ASSERT_EQ_I((int)copy.kind, (int)PSRP_VAL_OBJECT);
    ASSERT_EQ_SZ(psrp_object_type_name_count(copy.as.obj), 1u);
    ASSERT_EQ_STR(psrp_object_to_string(copy.as.obj, NULL), "text");
    ASSERT_EQ_SZ(psrp_object_adapted_count(copy.as.obj), 1u);
    b = psrp_object_find(copy.as.obj, "b");
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_I(psrp_object_item(b->as.obj, 0)->as.i32, 9);
    psrp_value_free(&copy);
}

PSRP_TEST(clone_copies_bytes_and_dictionaries)
{
    psrp_value_t d, bytes, copy;
    const psrp_dict_entry_t *e;
    static const unsigned char raw[3] = { 1, 2, 3 };

    psrp_value_init(&d);
    ASSERT_OK(psrp_primitive_dictionary_new(&d));
    psrp_value_init(&bytes);
    ASSERT_OK(psrp_value_set_bytes(&bytes, raw, sizeof raw));
    ASSERT_OK(psrp_primitive_dictionary_add(&d, "raw", &bytes));
    psrp_value_free(&bytes);

    psrp_value_init(&copy);
    ASSERT_OK(psrp_value_clone(&d, &copy));
    psrp_value_free(&d);

    ASSERT_EQ_SZ(psrp_object_entry_count(copy.as.obj), 1u);
    e = psrp_object_entry(copy.as.obj, 0);
    ASSERT_EQ_MEM(e->value.as.bytes.ptr, e->value.as.bytes.len,
                  raw, sizeof raw);
    psrp_value_free(&copy);
}

/* ----------------------------------------- ApplicationArguments wiring -- */

PSRP_TEST(init_runspacepool_carries_application_arguments)
{
    psrp_init_runspacepool_t init;
    psrp_value_t args;
    psrp_buffer_t out;

    psrp_value_init(&args);
    ASSERT_OK(psrp_primitive_dictionary_new(&args));
    ASSERT_OK(psrp_primitive_dictionary_add_string(&args, "PSVersionTable",
                                                   "5.1"));

    memset(&init, 0, sizeof init);
    init.min_runspaces = 1;
    init.max_runspaces = 1;
    init.application_arguments = &args;

    psrp_buffer_init(&out);
    ASSERT_OK(psrp_build_init_runspacepool(&init, &out));
    ASSERT_TRUE(xml_contains(&out, "N=\"ApplicationArguments\""));
    ASSERT_TRUE(xml_contains(&out, "PSVersionTable"));

    /* Building must not consume the caller's bag; a second pool can reuse it. */
    ASSERT_EQ_SZ(psrp_object_entry_count(args.as.obj), 1u);

    psrp_buffer_free(&out);
    psrp_value_free(&args);
}

PSRP_TEST(init_runspacepool_without_arguments_sends_null)
{
    psrp_init_runspacepool_t init;
    psrp_buffer_t out;

    memset(&init, 0, sizeof init);
    init.min_runspaces = 1;
    init.max_runspaces = 1;

    psrp_buffer_init(&out);
    ASSERT_OK(psrp_build_init_runspacepool(&init, &out));
    ASSERT_TRUE(xml_contains(&out, "ApplicationArguments"));
    ASSERT_FALSE(xml_contains(&out, "PSPrimitiveDictionary"));
    psrp_buffer_free(&out);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(param_list_counts_and_indexes),
    PSRP_TEST_CASE(param_count_of_absent_mp_is_zero),
    PSRP_TEST_CASE(plain_parameter_is_not_encoded),
    PSRP_TEST_CASE(list_parameter_unwraps_to_its_value),
    PSRP_TEST_CASE(object_with_extra_properties_is_not_a_wrapper),
    PSRP_TEST_CASE(unwrap_of_null_is_null),
    PSRP_TEST_CASE(array_parameter_exposes_elements_and_dimensions),
    PSRP_TEST_CASE(array_requires_at_least_one_dimension),
    PSRP_TEST_CASE(array_rejects_a_plain_value),
    PSRP_TEST_CASE(primitive_dictionary_round_trips),
    PSRP_TEST_CASE(primitive_dictionary_refuses_securestring_and_scriptblock),
    PSRP_TEST_CASE(primitive_dictionary_accepts_a_list_and_a_nested_dictionary),
    PSRP_TEST_CASE(primitive_dictionary_refuses_a_list_of_objects),
    PSRP_TEST_CASE(primitive_dictionary_rejects_a_non_dictionary),
    PSRP_TEST_CASE(clone_copies_deeply),
    PSRP_TEST_CASE(clone_copies_bytes_and_dictionaries),
    PSRP_TEST_CASE(init_runspacepool_carries_application_arguments),
    PSRP_TEST_CASE(init_runspacepool_without_arguments_sends_null),
};

PSRP_TEST_MAIN(cases)
