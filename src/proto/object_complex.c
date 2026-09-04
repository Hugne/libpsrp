/* Complex object storage for CLIXML ([MS-PSRP] 2.2.5.2). */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_object.h"

struct psrp_object {
    int64_t ref_id;          /* <0 when no RefId attribute */
    int64_t type_ref_id;     /* <0 when no <TNRef> */

    char **type_names;
    size_t type_name_count;

    char *to_string;
    size_t to_string_len;

    psrp_property_t *adapted;
    size_t adapted_count;

    psrp_property_t *extended;
    size_t extended_count;

    psrp_container_kind_t container;
    psrp_value_t *items;
    size_t item_count;

    psrp_dict_entry_t *entries;
    size_t entry_count;

    bool has_primitive;
    psrp_value_t primitive;
};

psrp_object_t *psrp_object_new(void)
{
    psrp_object_t *o = (psrp_object_t *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->ref_id = -1;
    o->type_ref_id = -1;
    psrp_value_init(&o->primitive);
    return o;
}

void psrp_object_free(psrp_object_t *o)
{
    size_t i;
    if (!o) return;

    for (i = 0; i < o->type_name_count; i++) free(o->type_names[i]);
    free(o->type_names);

    free(o->to_string);

    for (i = 0; i < o->adapted_count; i++) {
        free(o->adapted[i].name);
        psrp_value_free(&o->adapted[i].value);
    }
    free(o->adapted);

    for (i = 0; i < o->extended_count; i++) {
        free(o->extended[i].name);
        psrp_value_free(&o->extended[i].value);
    }
    free(o->extended);

    for (i = 0; i < o->item_count; i++) psrp_value_free(&o->items[i]);
    free(o->items);

    for (i = 0; i < o->entry_count; i++) {
        psrp_value_free(&o->entries[i].key);
        psrp_value_free(&o->entries[i].value);
    }
    free(o->entries);

    psrp_value_free(&o->primitive);
    free(o);
}

void psrp_object_set_ref_id(psrp_object_t *o, int64_t ref_id)
{
    if (o) o->ref_id = ref_id;
}

int64_t psrp_object_ref_id(const psrp_object_t *o)
{
    return o ? o->ref_id : -1;
}

/* Grows an array by one element. Returns the new slot, or NULL on failure. */
static void *grow(void **base, size_t *count, size_t elem_size)
{
    void *p = realloc(*base, (*count + 1) * elem_size);
    if (!p) return NULL;
    *base = p;
    {
        char *slot = (char *)p + (*count) * elem_size;
        memset(slot, 0, elem_size);
        (*count)++;
        return slot;
    }
}

static char *dup_text(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

psrp_result_t psrp_object_add_type_name(psrp_object_t *o, const char *name)
{
    char **slot;
    char *copy;

    if (!o || !name) return PSRP_ERR_INVALID_ARG;
    copy = dup_text(name, strlen(name));
    if (!copy) return PSRP_ERR_NOMEM;

    slot = (char **)grow((void **)&o->type_names, &o->type_name_count,
                         sizeof *o->type_names);
    if (!slot) { free(copy); return PSRP_ERR_NOMEM; }
    *slot = copy;
    return PSRP_OK;
}

size_t psrp_object_type_name_count(const psrp_object_t *o)
{
    return o ? o->type_name_count : 0;
}

const char *psrp_object_type_name(const psrp_object_t *o, size_t index)
{
    if (!o || index >= o->type_name_count) return NULL;
    return o->type_names[index];
}

void psrp_object_set_type_ref_id(psrp_object_t *o, int64_t ref_id)
{
    if (o) o->type_ref_id = ref_id;
}

int64_t psrp_object_type_ref_id(const psrp_object_t *o)
{
    return o ? o->type_ref_id : -1;
}

psrp_result_t psrp_object_set_to_string(psrp_object_t *o, const char *utf8, size_t n)
{
    char *copy;
    if (!o) return PSRP_ERR_INVALID_ARG;
    if (n && !utf8) return PSRP_ERR_INVALID_ARG;
    copy = dup_text(utf8 ? utf8 : "", n);
    if (!copy) return PSRP_ERR_NOMEM;
    free(o->to_string);
    o->to_string = copy;
    o->to_string_len = n;
    return PSRP_OK;
}

const char *psrp_object_to_string(const psrp_object_t *o, size_t *len)
{
    if (!o) { if (len) *len = 0; return NULL; }
    if (len) *len = o->to_string_len;
    return o->to_string;
}

static psrp_result_t add_prop(psrp_property_t **base, size_t *count,
                              const char *name, psrp_value_t *value)
{
    psrp_property_t *slot;
    char *name_copy = NULL;

    if (!value) return PSRP_ERR_INVALID_ARG;
    if (name) {
        name_copy = dup_text(name, strlen(name));
        if (!name_copy) return PSRP_ERR_NOMEM;
    }
    slot = (psrp_property_t *)grow((void **)base, count, sizeof **base);
    if (!slot) { free(name_copy); return PSRP_ERR_NOMEM; }

    slot->name = name_copy;
    slot->value = *value;      /* move */
    psrp_value_init(value);
    return PSRP_OK;
}

psrp_result_t psrp_object_add_adapted(psrp_object_t *o, const char *name,
                                      psrp_value_t *value)
{
    if (!o) return PSRP_ERR_INVALID_ARG;
    return add_prop(&o->adapted, &o->adapted_count, name, value);
}

psrp_result_t psrp_object_add_extended(psrp_object_t *o, const char *name,
                                       psrp_value_t *value)
{
    if (!o) return PSRP_ERR_INVALID_ARG;
    return add_prop(&o->extended, &o->extended_count, name, value);
}

size_t psrp_object_adapted_count(const psrp_object_t *o)
{
    return o ? o->adapted_count : 0;
}

size_t psrp_object_extended_count(const psrp_object_t *o)
{
    return o ? o->extended_count : 0;
}

const psrp_property_t *psrp_object_adapted(const psrp_object_t *o, size_t i)
{
    if (!o || i >= o->adapted_count) return NULL;
    return &o->adapted[i];
}

const psrp_property_t *psrp_object_extended(const psrp_object_t *o, size_t i)
{
    if (!o || i >= o->extended_count) return NULL;
    return &o->extended[i];
}

const psrp_value_t *psrp_object_find(const psrp_object_t *o, const char *name)
{
    size_t i;
    if (!o || !name) return NULL;
    /* Extended properties shadow adapted ones, matching PowerShell's own
     * member lookup order. */
    for (i = 0; i < o->extended_count; i++)
        if (o->extended[i].name && strcmp(o->extended[i].name, name) == 0)
            return &o->extended[i].value;
    for (i = 0; i < o->adapted_count; i++)
        if (o->adapted[i].name && strcmp(o->adapted[i].name, name) == 0)
            return &o->adapted[i].value;
    return NULL;
}

void psrp_object_set_container(psrp_object_t *o, psrp_container_kind_t kind)
{
    if (o) o->container = kind;
}

psrp_container_kind_t psrp_object_container(const psrp_object_t *o)
{
    return o ? o->container : PSRP_CONTAINER_NONE;
}

psrp_result_t psrp_object_add_item(psrp_object_t *o, psrp_value_t *value)
{
    psrp_value_t *slot;
    if (!o || !value) return PSRP_ERR_INVALID_ARG;
    slot = (psrp_value_t *)grow((void **)&o->items, &o->item_count,
                                sizeof *o->items);
    if (!slot) return PSRP_ERR_NOMEM;
    *slot = *value;
    psrp_value_init(value);
    return PSRP_OK;
}

size_t psrp_object_item_count(const psrp_object_t *o)
{
    return o ? o->item_count : 0;
}

const psrp_value_t *psrp_object_item(const psrp_object_t *o, size_t i)
{
    if (!o || i >= o->item_count) return NULL;
    return &o->items[i];
}

psrp_result_t psrp_object_add_entry(psrp_object_t *o, psrp_value_t *key,
                                    psrp_value_t *value)
{
    psrp_dict_entry_t *slot;
    if (!o || !key || !value) return PSRP_ERR_INVALID_ARG;
    slot = (psrp_dict_entry_t *)grow((void **)&o->entries, &o->entry_count,
                                     sizeof *o->entries);
    if (!slot) return PSRP_ERR_NOMEM;
    slot->key = *key;
    slot->value = *value;
    psrp_value_init(key);
    psrp_value_init(value);
    return PSRP_OK;
}

size_t psrp_object_entry_count(const psrp_object_t *o)
{
    return o ? o->entry_count : 0;
}

const psrp_dict_entry_t *psrp_object_entry(const psrp_object_t *o, size_t i)
{
    if (!o || i >= o->entry_count) return NULL;
    return &o->entries[i];
}

psrp_result_t psrp_object_set_primitive(psrp_object_t *o, psrp_value_t *value)
{
    if (!o || !value) return PSRP_ERR_INVALID_ARG;
    psrp_value_free(&o->primitive);
    o->primitive = *value;
    psrp_value_init(value);
    o->has_primitive = true;
    return PSRP_OK;
}

const psrp_value_t *psrp_object_primitive(const psrp_object_t *o)
{
    if (!o || !o->has_primitive) return NULL;
    return &o->primitive;
}
