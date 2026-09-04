/* Host method parameter encoding ([MS-PSRP] 2.2.6).
 *
 * The awkward part of 2.2.6 is that most parameters are not encoded at all.
 * 2.2.6.1.1 says any plainly serializable value travels as itself, so a
 * reader cannot assume a wrapper is present. Only the container shapes get
 * one: lists and collections become a T/V pair, and arrays become an object
 * carrying `mae` and `mal`.
 *
 * So the rule here is: try to recognise a wrapper, and hand back the value
 * unchanged when there is none. Guessing wrong in the other direction would
 * hand callers the wrapper object where they expected a string.
 */

#include <string.h>

#include "psrp/psrp_host.h"

size_t psrp_host_param_count(const psrp_value_t *mp)
{
    if (!mp || mp->kind != PSRP_VAL_OBJECT) return 0;
    return psrp_object_item_count(mp->as.obj);
}

const psrp_value_t *psrp_host_param(const psrp_value_t *mp, size_t index)
{
    if (!mp || mp->kind != PSRP_VAL_OBJECT) return NULL;
    return psrp_object_item(mp->as.obj, index);
}

const psrp_value_t *psrp_host_param_unwrap(const psrp_value_t *v,
                                           const char **type_name)
{
    const psrp_value_t *t, *inner;

    if (type_name) *type_name = NULL;
    if (!v) return NULL;
    if (v->kind != PSRP_VAL_OBJECT) return v;

    t = psrp_object_find(v->as.obj, "T");
    inner = psrp_object_find(v->as.obj, "V");
    if (!t || !inner || t->kind != PSRP_VAL_STRING) return v;

    /* A T/V pair only counts as a wrapper when those are the whole object.
     * An ordinary object that happens to have properties named T and V is
     * not one, and unwrapping it would silently drop its other properties. */
    if (psrp_object_extended_count(v->as.obj) != 2 ||
        psrp_object_adapted_count(v->as.obj) != 0)
        return v;

    if (type_name) *type_name = t->as.text.ptr;
    return inner;
}

psrp_result_t psrp_host_param_array(const psrp_value_t *v,
                                    const psrp_value_t **elements,
                                    const psrp_value_t **dimensions)
{
    const psrp_value_t *mae, *mal;

    if (elements) *elements = NULL;
    if (dimensions) *dimensions = NULL;
    if (!v || v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;

    mae = psrp_object_find(v->as.obj, "mae");
    mal = psrp_object_find(v->as.obj, "mal");
    if (!mae || !mal) return PSRP_ERR_MALFORMED;
    if (mae->kind != PSRP_VAL_OBJECT || mal->kind != PSRP_VAL_OBJECT)
        return PSRP_ERR_MALFORMED;

    /* 2.2.6.1.4: mal MUST have at least one element. An empty one would mean
     * an array of no dimensions, which is not a thing. */
    if (psrp_object_item_count(mal->as.obj) == 0) return PSRP_ERR_MALFORMED;

    if (elements) *elements = mae;
    if (dimensions) *dimensions = mal;
    return PSRP_OK;
}
