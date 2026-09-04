/* Differential test against psrpcore.
 *
 * Every other golden vector in this suite came from PowerShell itself. That is
 * the authoritative oracle, but it is a single one: a misreading shared
 * between our code and our reading of PowerShell's output would go unnoticed
 * by every test we have. psrpcore is an independent implementation of the same
 * specification, so a disagreement with it is worth knowing about.
 *
 * This half is direction A: psrpcore serialized, we parse. The corpus is
 * generated and committed by tools/differential.py, so this needs neither
 * Python nor psrpcore to run.
 *
 * Direction B, our output read back by psrpcore, needs both, so it lives in
 * that script and consumes this binary's --emit mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_clixml.h"
#include "psrp/psrp_records.h"
#include "psrp_test.h"
#include "differential_corpus.h"

/* Rebuilds each case's value with our own API, so --emit can serialize it.
 * Kept in the same order as the corpus; the name is asserted to match, since a
 * silent drift here would compare two different things and still pass. */
static psrp_result_t build_case(const char *name, psrp_value_t *v)
{
    psrp_value_init(v);

    if (strcmp(name, "string") == 0) return psrp_value_set_string(v, "hello");
    if (strcmp(name, "string_empty") == 0) return psrp_value_set_string(v, "");
    if (strcmp(name, "string_control") == 0)
        return psrp_value_set_text(v, PSRP_VAL_STRING, "a\0b", 3);
    if (strcmp(name, "string_newline") == 0)
        return psrp_value_set_string(v, "one\ntwo");
    if (strcmp(name, "string_underscore") == 0)
        return psrp_value_set_string(v, "_x0041_");
    if (strcmp(name, "string_markup") == 0)
        return psrp_value_set_string(v, "<&>\"'");
    if (strcmp(name, "string_unicode") == 0)
        return psrp_value_set_string(v, "caf\xc3\xa9 \xe4\xb8\xad");

    if (strcmp(name, "bool_true") == 0)  { psrp_value_set_bool(v, true);  return PSRP_OK; }
    if (strcmp(name, "bool_false") == 0) { psrp_value_set_bool(v, false); return PSRP_OK; }
    if (strcmp(name, "null") == 0)       { psrp_value_set_null(v);        return PSRP_OK; }

    if (strcmp(name, "byte") == 0)   { psrp_value_set_uint8(v, 255);   return PSRP_OK; }
    if (strcmp(name, "sbyte") == 0)  { psrp_value_set_int8(v, -128);   return PSRP_OK; }
    if (strcmp(name, "int16") == 0)  { psrp_value_set_int16(v, -32768); return PSRP_OK; }
    if (strcmp(name, "uint16") == 0) { psrp_value_set_uint16(v, 65535); return PSRP_OK; }
    if (strcmp(name, "int32") == 0)  { psrp_value_set_int32(v, -2147483647 - 1); return PSRP_OK; }
    if (strcmp(name, "uint32") == 0) { psrp_value_set_uint32(v, 4294967295u); return PSRP_OK; }
    if (strcmp(name, "int64") == 0)  { psrp_value_set_int64(v, INT64_MIN); return PSRP_OK; }
    if (strcmp(name, "uint64") == 0) { psrp_value_set_uint64(v, UINT64_MAX); return PSRP_OK; }

    if (strcmp(name, "single") == 0) { psrp_value_set_single(v, 1.5f);  return PSRP_OK; }
    if (strcmp(name, "double") == 0) { psrp_value_set_double(v, 1.25);  return PSRP_OK; }
    if (strcmp(name, "decimal") == 0)
        return psrp_value_set_text(v, PSRP_VAL_DECIMAL, "1.100", 5);
    if (strcmp(name, "char") == 0)   { psrp_value_set_char(v, 'A');     return PSRP_OK; }

    if (strcmp(name, "guid") == 0) {
        psrp_guid_t g;
        psrp_result_t rc = psrp_guid_parse("4358d2a2-8a0b-4b5d-9c43-1d2e3f405162",
                                           &g);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_guid(v, &g);
        return PSRP_OK;
    }
    if (strcmp(name, "version") == 0)
        return psrp_value_set_text(v, PSRP_VAL_VERSION, "6.2.1.3", 7);
    if (strcmp(name, "uri") == 0)
        return psrp_value_set_text(v, PSRP_VAL_URI, "http://example.com/a b", 22);
    if (strcmp(name, "bytes") == 0) {
        static const unsigned char raw[4] = { 0x00, 0x01, 0xFE, 0xFF };
        return psrp_value_set_bytes(v, raw, sizeof raw);
    }
    if (strcmp(name, "duration") == 0)
        return psrp_value_set_text(v, PSRP_VAL_DURATION, "P1DT3S", 6);
    if (strcmp(name, "xmldoc") == 0)
        return psrp_value_set_text(v, PSRP_VAL_XMLDOC, "<a b='c'/>", 10);

    /* Containers are rebuilt to match psrpcore's shape, type names included,
     * since those are part of what the other implementation has to read. */
    if (strcmp(name, "list") == 0 || strcmp(name, "list_empty") == 0 ||
        strcmp(name, "list_nested") == 0) {
        psrp_object_t *o = psrp_object_new();
        psrp_value_t item;
        psrp_result_t rc;
        if (!o) return PSRP_ERR_NOMEM;
        psrp_object_set_ref_id(o, 0);
        psrp_object_set_type_ref_id(o, 0);
        psrp_object_set_container(o, PSRP_CONTAINER_LIST);
        rc = psrp_object_add_type_name(o, "System.Collections.ArrayList");
        if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.Object");

        if (rc == PSRP_OK && strcmp(name, "list") == 0) {
            psrp_value_init(&item);
            psrp_value_set_int32(&item, 1);
            rc = psrp_object_add_item(o, &item);
            psrp_value_free(&item);
            if (rc == PSRP_OK) {
                rc = psrp_value_set_string(&item, "a");
                if (rc == PSRP_OK) rc = psrp_object_add_item(o, &item);
                psrp_value_free(&item);
            }
        } else if (rc == PSRP_OK && strcmp(name, "list_nested") == 0) {
            psrp_object_t *inner = psrp_object_new();
            if (!inner) { psrp_object_free(o); return PSRP_ERR_NOMEM; }
            psrp_object_set_ref_id(inner, 1);
            psrp_object_set_type_ref_id(inner, 1);
            psrp_object_set_container(inner, PSRP_CONTAINER_LIST);
            rc = psrp_object_add_type_name(inner, "System.Collections.ArrayList");
            if (rc == PSRP_OK)
                rc = psrp_object_add_type_name(inner, "System.Object");
            psrp_value_init(&item);
            if (rc == PSRP_OK) {
                psrp_value_set_int32(&item, 1);
                rc = psrp_object_add_item(inner, &item);
                psrp_value_free(&item);
            }
            if (rc == PSRP_OK) {
                rc = psrp_value_set_object(&item, inner);
                if (rc == PSRP_OK) rc = psrp_object_add_item(o, &item);
                else psrp_object_free(inner);
                psrp_value_free(&item);
            } else {
                psrp_object_free(inner);
            }
            if (rc == PSRP_OK) {
                psrp_value_init(&item);
                rc = psrp_value_set_string(&item, "x");
                if (rc == PSRP_OK) rc = psrp_object_add_item(o, &item);
                psrp_value_free(&item);
            }
        }

        if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
        rc = psrp_value_set_object(v, o);
        if (rc != PSRP_OK) psrp_object_free(o);
        return rc;
    }

    if (strcmp(name, "dict") == 0) {
        psrp_object_t *o = psrp_object_new();
        psrp_value_t k, val;
        psrp_result_t rc;
        if (!o) return PSRP_ERR_NOMEM;
        psrp_object_set_ref_id(o, 0);
        psrp_object_set_type_ref_id(o, 0);
        psrp_object_set_container(o, PSRP_CONTAINER_DICT);
        rc = psrp_object_add_type_name(o, "System.Collections.Hashtable");
        if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.Object");
        psrp_value_init(&k);
        psrp_value_init(&val);
        if (rc == PSRP_OK) rc = psrp_value_set_string(&k, "k");
        if (rc == PSRP_OK) rc = psrp_value_set_string(&val, "v");
        if (rc == PSRP_OK) rc = psrp_object_add_entry(o, &k, &val);
        psrp_value_free(&k);
        psrp_value_free(&val);
        if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
        rc = psrp_value_set_object(v, o);
        if (rc != PSRP_OK) psrp_object_free(o);
        return rc;
    }

    return PSRP_ERR_NOT_FOUND;
}

/* ------------------------------------------ direction A: they -> us ----- */

PSRP_TEST(we_parse_everything_psrpcore_writes)
{
    size_t i;

    for (i = 0; i < DIFFERENTIAL_CASE_COUNT; i++) {
        const differential_case_t *c = &kDifferentialCases[i];
        psrp_value_t v;
        psrp_result_t rc;

        psrp_value_init(&v);
        rc = psrp_clixml_deserialize(c->xml, c->xml_len, &v);
        if (rc != PSRP_OK) {
            psrp_value_free(&v);
            PSRP_FAIL("case %s: could not parse psrpcore's output (%s): %s",
                      c->name, psrp_strerror(rc), c->xml);
        }
        if (v.kind != c->kind) {
            int got = (int)v.kind;
            psrp_value_free(&v);
            PSRP_FAIL("case %s: kind %d, expected %d", c->name, got,
                      (int)c->kind);
        }
        psrp_value_free(&v);
    }
}

PSRP_TEST(we_agree_about_what_the_values_mean)
{
    /* Parsing without complaint is not agreement: the two implementations must
     * also decode the same value out of the same bytes. */
    size_t i;

    for (i = 0; i < DIFFERENTIAL_CASE_COUNT; i++) {
        const differential_case_t *c = &kDifferentialCases[i];
        psrp_value_t v;
        psrp_buffer_t text;

        if (!c->text && c->item_count < 0 && c->entry_count < 0) continue;

        psrp_value_init(&v);
        ASSERT_OK(psrp_clixml_deserialize(c->xml, c->xml_len, &v));

        if (c->text) {
            psrp_buffer_init(&text);
            ASSERT_OK(psrp_value_to_text(&v, &text));
            if (text.len != c->text_len ||
                (c->text_len && memcmp(text.data, c->text, c->text_len) != 0)) {
                psrp_buffer_free(&text);
                psrp_value_free(&v);
                PSRP_FAIL("case %s: text mismatch", c->name);
            }
            psrp_buffer_free(&text);
        }
        if (c->item_count >= 0) {
            ASSERT_EQ_I((int)v.kind, (int)PSRP_VAL_OBJECT);
            ASSERT_EQ_SZ(psrp_object_item_count(v.as.obj),
                         (size_t)c->item_count);
        }
        if (c->entry_count >= 0) {
            ASSERT_EQ_I((int)v.kind, (int)PSRP_VAL_OBJECT);
            ASSERT_EQ_SZ(psrp_object_entry_count(v.as.obj),
                         (size_t)c->entry_count);
        }
        psrp_value_free(&v);
    }
}

PSRP_TEST(every_buildable_case_can_be_rebuilt_with_our_own_api)
{
    /* --emit depends on this, and a case the builder does not know would be
     * silently skipped there rather than reported, so check it here.
     * Parse-only cases are exempt by design: they exist to widen direction A,
     * where an over-strict reader shows up, and hand-building each one would
     * cost more than it proves. */
    size_t i, buildable = 0;

    for (i = 0; i < DIFFERENTIAL_CASE_COUNT; i++) {
        const differential_case_t *c = &kDifferentialCases[i];
        psrp_value_t v;
        psrp_result_t rc;

        if (!c->buildable) continue;
        buildable++;
        rc = build_case(c->name, &v);
        if (rc != PSRP_OK) {
            psrp_value_free(&v);
            PSRP_FAIL("case %s: no builder (%s)", c->name, psrp_strerror(rc));
        }
        ASSERT_EQ_I((int)v.kind, (int)c->kind);
        psrp_value_free(&v);
    }

    /* If the flag were ever generated as all-zero this test would pass while
     * checking nothing, so require that it actually found some. */
    ASSERT_TRUE(buildable > 20);
}

PSRP_TEST(our_round_trip_of_their_output_is_stable)
{
    /* Reading psrpcore's document and writing it back must produce something
     * we read the same way again. A serializer that loses an escape would
     * survive a single parse but not this. */
    size_t i;

    for (i = 0; i < DIFFERENTIAL_CASE_COUNT; i++) {
        const differential_case_t *c = &kDifferentialCases[i];
        psrp_value_t first, second;
        psrp_buffer_t ours, again;

        psrp_value_init(&first);
        ASSERT_OK(psrp_clixml_deserialize(c->xml, c->xml_len, &first));

        psrp_buffer_init(&ours);
        ASSERT_OK(psrp_clixml_serialize(&first, &ours));

        psrp_value_init(&second);
        ASSERT_OK(psrp_clixml_deserialize(ours.data, ours.len, &second));

        psrp_buffer_init(&again);
        ASSERT_OK(psrp_clixml_serialize(&second, &again));
        ASSERT_EQ_MEM(again.data, again.len, ours.data, ours.len);

        psrp_buffer_free(&again);
        psrp_buffer_free(&ours);
        psrp_value_free(&second);
        psrp_value_free(&first);
    }
}

/* ---------------------------------------------------------- --emit ------ */

static int emit(void)
{
    size_t i;

    for (i = 0; i < DIFFERENTIAL_CASE_COUNT; i++) {
        const differential_case_t *c = &kDifferentialCases[i];
        psrp_value_t v;
        psrp_buffer_t xml;

        if (!c->buildable) continue;
        if (build_case(c->name, &v) != PSRP_OK) {
            psrp_value_free(&v);
            continue;
        }
        psrp_buffer_init(&xml);
        if (psrp_clixml_serialize(&v, &xml) == PSRP_OK &&
            psrp_buffer_append_u8(&xml, 0) == PSRP_OK)
            printf("%s\t%s\n", c->name, (const char *)xml.data);
        psrp_buffer_free(&xml);
        psrp_value_free(&v);
    }
    return 0;
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(we_parse_everything_psrpcore_writes),
    PSRP_TEST_CASE(we_agree_about_what_the_values_mean),
    PSRP_TEST_CASE(every_buildable_case_can_be_rebuilt_with_our_own_api),
    PSRP_TEST_CASE(our_round_trip_of_their_output_is_stable),
};

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--emit") == 0) return emit();
    return psrp_test_run(cases, sizeof cases / sizeof cases[0], argc, argv);
}
