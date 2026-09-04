#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_messages.h"
#include "psrp/psrp_clixml.h"
#include "psrp_test.h"

static void check_xml(const psrp_buffer_t *b, const char *want)
{
    ASSERT_EQ_MEM(b->data, b->len, want, strlen(want));
}

/* ------------------------------------------------- set min / max -------- */

/* Matches the 2.2.2.6 example: the count first, then ci as a Signed Long. */
PSRP_TEST(set_max_runspaces_matches_spec_example)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_set_max_runspaces(1, 3, &xml));
    check_xml(&xml,
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"MaxRunspaces\">3</I32>"
        "<I64 N=\"ci\">1</I64>"
        "</MS></Obj>");
    psrp_buffer_free(&xml);
}

PSRP_TEST(set_min_runspaces_shape)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_set_min_runspaces(42, 2, &xml));
    check_xml(&xml,
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"MinRunspaces\">2</I32>"
        "<I64 N=\"ci\">42</I64>"
        "</MS></Obj>");
    psrp_buffer_free(&xml);
}

/* ci is a Signed Long, so it must survive values beyond 32 bits. */
PSRP_TEST(call_id_is_64_bit)
{
    psrp_buffer_t xml;
    psrp_value_t root;
    const psrp_value_t *ci;
    psrp_buffer_init(&xml);
    psrp_value_init(&root);
    ASSERT_OK(psrp_build_get_available_runspaces(9223372036854775807LL, &xml));
    ASSERT_OK(psrp_clixml_deserialize(xml.data, xml.len, &root));
    ci = psrp_object_find(root.as.obj, "ci");
    ASSERT_NOT_NULL(ci);
    ASSERT_EQ_I(ci->kind, PSRP_VAL_INT64);
    ASSERT_TRUE(ci->as.i64 == 9223372036854775807LL);
    psrp_value_free(&root);
    psrp_buffer_free(&xml);
}

PSRP_TEST(get_available_and_reset_carry_only_ci)
{
    psrp_buffer_t a, b;
    psrp_buffer_init(&a);
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_build_get_available_runspaces(7, &a));
    check_xml(&a, "<Obj RefId=\"0\"><MS><I64 N=\"ci\">7</I64></MS></Obj>");
    ASSERT_OK(psrp_build_reset_runspace_state(8, &b));
    check_xml(&b, "<Obj RefId=\"0\"><MS><I64 N=\"ci\">8</I64></MS></Obj>");
    psrp_buffer_free(&a);
    psrp_buffer_free(&b);
}

/* ------------------------------------------------ connect runspacepool -- */

PSRP_TEST(connect_runspacepool_with_bounds)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_connect_runspacepool(1, 5, &xml));
    check_xml(&xml,
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"MinRunspaces\">1</I32>"
        "<I32 N=\"MaxRunspaces\">5</I32>"
        "</MS></Obj>");
    psrp_buffer_free(&xml);
}

/* Omitting both bounds is the spec's "empty" form: a single runspace. */
PSRP_TEST(connect_runspacepool_without_bounds_is_empty)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_connect_runspacepool(-1, -1, &xml));
    check_xml(&xml, "<Obj RefId=\"0\"></Obj>");
    psrp_buffer_free(&xml);
}

PSRP_TEST(connect_runspacepool_with_one_bound)
{
    psrp_buffer_t xml;
    psrp_buffer_init(&xml);
    ASSERT_OK(psrp_build_connect_runspacepool(-1, 5, &xml));
    check_xml(&xml,
        "<Obj RefId=\"0\"><MS><I32 N=\"MaxRunspaces\">5</I32></MS></Obj>");
    psrp_buffer_free(&xml);
}

/* --------------------------------------------- runspace availability ---- */

/* Answering set-min/max: the response is a Boolean. */
PSRP_TEST(runspace_availability_boolean_response)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<B N=\"SetMinMaxRunspacesResponse\">true</B>"
        "<I64 N=\"ci\">1</I64>"
        "</MS></Obj>";
    psrp_runspace_availability_t r;
    ASSERT_OK(psrp_parse_runspace_availability(xml, sizeof xml - 1, &r));
    ASSERT_TRUE(r.ci == 1);
    ASSERT_FALSE(r.is_count);
    ASSERT_TRUE(r.accepted);
}

PSRP_TEST(runspace_availability_rejected)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<B N=\"SetMinMaxRunspacesResponse\">false</B>"
        "<I64 N=\"ci\">2</I64>"
        "</MS></Obj>";
    psrp_runspace_availability_t r;
    ASSERT_OK(psrp_parse_runspace_availability(xml, sizeof xml - 1, &r));
    ASSERT_FALSE(r.is_count);
    ASSERT_FALSE(r.accepted);
}

/* Answering get-available: the same property carries a Signed Long instead. */
PSRP_TEST(runspace_availability_count_response)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I64 N=\"SetMinMaxRunspacesResponse\">3</I64>"
        "<I64 N=\"ci\">5</I64>"
        "</MS></Obj>";
    psrp_runspace_availability_t r;
    ASSERT_OK(psrp_parse_runspace_availability(xml, sizeof xml - 1, &r));
    ASSERT_TRUE(r.ci == 5);
    ASSERT_TRUE(r.is_count);
    ASSERT_TRUE(r.count == 3);
}

/* A server that narrows the count to I32 should still be understood. */
PSRP_TEST(runspace_availability_accepts_i32_count)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"SetMinMaxRunspacesResponse\">4</I32>"
        "<I64 N=\"ci\">6</I64>"
        "</MS></Obj>";
    psrp_runspace_availability_t r;
    ASSERT_OK(psrp_parse_runspace_availability(xml, sizeof xml - 1, &r));
    ASSERT_TRUE(r.is_count);
    ASSERT_TRUE(r.count == 4);
}

PSRP_TEST(runspace_availability_requires_ci_and_response)
{
    static const char no_ci[] =
        "<Obj RefId=\"0\"><MS>"
        "<B N=\"SetMinMaxRunspacesResponse\">true</B></MS></Obj>";
    static const char no_resp[] =
        "<Obj RefId=\"0\"><MS><I64 N=\"ci\">1</I64></MS></Obj>";
    static const char bad_type[] =
        "<Obj RefId=\"0\"><MS>"
        "<S N=\"SetMinMaxRunspacesResponse\">yes</S>"
        "<I64 N=\"ci\">1</I64></MS></Obj>";
    psrp_runspace_availability_t r;
    ASSERT_ERR(psrp_parse_runspace_availability(no_ci, sizeof no_ci - 1, &r),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_runspace_availability(no_resp, sizeof no_resp - 1, &r),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_runspace_availability(bad_type, sizeof bad_type - 1, &r),
               PSRP_ERR_MALFORMED);
}

/* ------------------------------------------------- pool init data ------- */

PSRP_TEST(runspacepool_init_data_parses)
{
    static const char xml[] =
        "<Obj RefId=\"0\"><MS>"
        "<I32 N=\"MinRunspaces\">1</I32>"
        "<I32 N=\"MaxRunspaces\">5</I32>"
        "</MS></Obj>";
    psrp_runspacepool_init_data_t d;
    ASSERT_OK(psrp_parse_runspacepool_init_data(xml, sizeof xml - 1, &d));
    ASSERT_EQ_I(d.min_runspaces, 1);
    ASSERT_EQ_I(d.max_runspaces, 5);
}

/* Both properties are optional; absent must be distinguishable from zero. */
PSRP_TEST(runspacepool_init_data_absent_bounds_are_minus_one)
{
    static const char xml[] = "<Obj RefId=\"0\"></Obj>";
    psrp_runspacepool_init_data_t d;
    ASSERT_OK(psrp_parse_runspacepool_init_data(xml, sizeof xml - 1, &d));
    ASSERT_EQ_I(d.min_runspaces, -1);
    ASSERT_EQ_I(d.max_runspaces, -1);
}

PSRP_TEST(pool_parsers_reject_bad_input)
{
    psrp_runspacepool_init_data_t d;
    psrp_runspace_availability_t r;
    ASSERT_ERR(psrp_parse_runspacepool_init_data("<S>x</S>", 8, &d),
               PSRP_ERR_MALFORMED);
    ASSERT_ERR(psrp_parse_runspace_availability("nope", 4, &r), PSRP_ERR_XML);
    ASSERT_ERR(psrp_parse_runspace_availability("<Obj/>", 6, NULL),
               PSRP_ERR_INVALID_ARG);
}

PSRP_TEST(pool_builders_reject_null_output)
{
    ASSERT_ERR(psrp_build_set_max_runspaces(1, 1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_get_available_runspaces(1, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_build_connect_runspacepool(1, 1, NULL), PSRP_ERR_INVALID_ARG);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(set_max_runspaces_matches_spec_example),
    PSRP_TEST_CASE(set_min_runspaces_shape),
    PSRP_TEST_CASE(call_id_is_64_bit),
    PSRP_TEST_CASE(get_available_and_reset_carry_only_ci),
    PSRP_TEST_CASE(connect_runspacepool_with_bounds),
    PSRP_TEST_CASE(connect_runspacepool_without_bounds_is_empty),
    PSRP_TEST_CASE(connect_runspacepool_with_one_bound),
    PSRP_TEST_CASE(runspace_availability_boolean_response),
    PSRP_TEST_CASE(runspace_availability_rejected),
    PSRP_TEST_CASE(runspace_availability_count_response),
    PSRP_TEST_CASE(runspace_availability_accepts_i32_count),
    PSRP_TEST_CASE(runspace_availability_requires_ci_and_response),
    PSRP_TEST_CASE(runspacepool_init_data_parses),
    PSRP_TEST_CASE(runspacepool_init_data_absent_bounds_are_minus_one),
    PSRP_TEST_CASE(pool_parsers_reject_bad_input),
    PSRP_TEST_CASE(pool_builders_reject_null_output),
};

PSRP_TEST_MAIN(cases)
