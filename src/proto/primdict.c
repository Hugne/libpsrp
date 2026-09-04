/* Primitive Dictionary ([MS-PSRP] 2.2.3.18).
 *
 * A dictionary narrowed to string keys and primitive values. The narrowing is
 * the whole point of the type, so it is enforced on insert rather than left
 * for the far side to reject: ScriptBlock and SecureString are excluded, and
 * a nested value may only be a list of primitives or another primitive
 * dictionary.
 */

#include <string.h>

#include "psrp/psrp_messages.h"

static bool is_primitive(psrp_value_kind_t k)
{
    if (k == PSRP_VAL_OBJECT) return false;
    /* 2.2.3.18 excludes exactly these two. SecureString is excluded because
     * its ciphertext is only meaningful after a key exchange, which an
     * ApplicationArguments bag is sent before. */
    if (k == PSRP_VAL_SCRIPTBLOCK || k == PSRP_VAL_SECURESTRING) return false;
    return true;
}

static bool is_primitive_dictionary(const psrp_object_t *o);

/* A permitted nested value: a list whose items are all primitives, or another
 * primitive dictionary. */
static bool is_allowed_object(const psrp_object_t *o)
{
    size_t i, n;

    if (psrp_object_container(o) == PSRP_CONTAINER_LIST) {
        n = psrp_object_item_count(o);
        for (i = 0; i < n; i++) {
            const psrp_value_t *it = psrp_object_item(o, i);
            if (!it || !is_primitive(it->kind)) return false;
        }
        return true;
    }
    return is_primitive_dictionary(o);
}

static bool is_primitive_dictionary(const psrp_object_t *o)
{
    size_t i, n;

    if (psrp_object_container(o) != PSRP_CONTAINER_DICT) return false;
    n = psrp_object_entry_count(o);
    for (i = 0; i < n; i++) {
        const psrp_dict_entry_t *e = psrp_object_entry(o, i);
        if (e->key.kind != PSRP_VAL_STRING) return false;
        if (e->value.kind == PSRP_VAL_OBJECT) {
            if (!is_allowed_object(e->value.as.obj)) return false;
        } else if (!is_primitive(e->value.kind)) {
            return false;
        }
    }
    return true;
}

psrp_result_t psrp_primitive_dictionary_new(psrp_value_t *out)
{
    psrp_object_t *o;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);
    psrp_object_set_type_ref_id(o, 0);
    psrp_object_set_container(o, PSRP_CONTAINER_DICT);

    rc = psrp_object_add_type_name(o,
        "System.Management.Automation.PSPrimitiveDictionary");
    if (rc == PSRP_OK)
        rc = psrp_object_add_type_name(o, "System.Collections.Hashtable");
    if (rc == PSRP_OK)
        rc = psrp_object_add_type_name(o, "System.Object");
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    rc = psrp_value_set_object(out, o);
    if (rc != PSRP_OK) psrp_object_free(o);
    return rc;
}

psrp_result_t psrp_primitive_dictionary_add(psrp_value_t *dict,
                                            const char *key,
                                            psrp_value_t *value)
{
    psrp_value_t k;
    psrp_result_t rc;

    if (!dict || !key || !value) return PSRP_ERR_INVALID_ARG;
    if (dict->kind != PSRP_VAL_OBJECT) return PSRP_ERR_INVALID_ARG;
    if (psrp_object_container(dict->as.obj) != PSRP_CONTAINER_DICT)
        return PSRP_ERR_INVALID_ARG;

    if (value->kind == PSRP_VAL_OBJECT) {
        if (!is_allowed_object(value->as.obj)) return PSRP_ERR_INVALID_ARG;
    } else if (!is_primitive(value->kind)) {
        return PSRP_ERR_INVALID_ARG;
    }

    psrp_value_init(&k);
    rc = psrp_value_set_text(&k, PSRP_VAL_STRING, key, strlen(key));
    if (rc == PSRP_OK) rc = psrp_object_add_entry(dict->as.obj, &k, value);
    psrp_value_free(&k);
    return rc;
}

psrp_result_t psrp_primitive_dictionary_add_string(psrp_value_t *dict,
                                                   const char *key,
                                                   const char *value)
{
    psrp_value_t v;
    psrp_result_t rc;

    if (!value) return PSRP_ERR_INVALID_ARG;
    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, value, strlen(value));
    if (rc == PSRP_OK) rc = psrp_primitive_dictionary_add(dict, key, &v);
    psrp_value_free(&v);
    return rc;
}
