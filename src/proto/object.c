/* The PowerShell value model backing CLIXML ([MS-PSRP] 2.2.5). */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_object.h"

/* Element names from 2.2.5.1. Order must match psrp_value_kind_t. */
static const struct {
    psrp_value_kind_t kind;
    const char *element;
    bool escaped;         /* content encoded per 2.2.5.3.2 */
} kKinds[] = {
    { PSRP_VAL_NULL,         "Nil",     false },
    { PSRP_VAL_STRING,       "S",       true  },
    { PSRP_VAL_CHAR,         "C",       false },
    { PSRP_VAL_BOOL,         "B",       false },
    { PSRP_VAL_DATETIME,     "DT",      false },
    { PSRP_VAL_DURATION,     "TS",      false },
    { PSRP_VAL_UINT8,        "By",      false },
    { PSRP_VAL_INT8,         "SB",      false },
    { PSRP_VAL_UINT16,       "U16",     false },
    { PSRP_VAL_INT16,        "I16",     false },
    { PSRP_VAL_UINT32,       "U32",     false },
    { PSRP_VAL_INT32,        "I32",     false },
    { PSRP_VAL_UINT64,       "U64",     false },
    { PSRP_VAL_INT64,        "I64",     false },
    { PSRP_VAL_SINGLE,       "Sg",      false },
    { PSRP_VAL_DOUBLE,       "Db",      false },
    { PSRP_VAL_DECIMAL,      "D",       false },
    { PSRP_VAL_BYTES,        "BA",      false },
    { PSRP_VAL_GUID,         "G",       false },
    /* 2.2.5.1.19 and 2.2.5.1.22/23 explicitly cite the encoding section. */
    { PSRP_VAL_URI,          "URI",     true  },
    { PSRP_VAL_VERSION,      "Version", false },
    { PSRP_VAL_XMLDOC,       "XD",      true  },
    { PSRP_VAL_SCRIPTBLOCK,  "SBK",     true  },
    { PSRP_VAL_SECURESTRING, "SS",      false },
    { PSRP_VAL_OBJECT,       "Obj",     false }
};
#define KKIND_COUNT (sizeof kKinds / sizeof kKinds[0])

const char *psrp_value_kind_element(psrp_value_kind_t kind)
{
    size_t i;
    for (i = 0; i < KKIND_COUNT; i++)
        if (kKinds[i].kind == kind) return kKinds[i].element;
    return NULL;
}

bool psrp_value_kind_from_element(const char *element, psrp_value_kind_t *out)
{
    size_t i;
    if (!element) return false;
    for (i = 0; i < KKIND_COUNT; i++) {
        if (strcmp(kKinds[i].element, element) == 0) {
            if (out) *out = kKinds[i].kind;
            return true;
        }
    }
    return false;
}

bool psrp_value_kind_is_escaped(psrp_value_kind_t kind)
{
    size_t i;
    for (i = 0; i < KKIND_COUNT; i++)
        if (kKinds[i].kind == kind) return kKinds[i].escaped;
    return false;
}

/* Which kinds carry owned text / bytes. */
static bool kind_is_text(psrp_value_kind_t k)
{
    switch (k) {
    case PSRP_VAL_STRING: case PSRP_VAL_DATETIME: case PSRP_VAL_DURATION:
    case PSRP_VAL_DECIMAL: case PSRP_VAL_URI: case PSRP_VAL_VERSION:
    case PSRP_VAL_XMLDOC: case PSRP_VAL_SCRIPTBLOCK: case PSRP_VAL_SECURESTRING:
        return true;
    default:
        return false;
    }
}

void psrp_value_init(psrp_value_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof *v);
    v->kind = PSRP_VAL_NULL;
}

bool psrp_value_kind_has_text(psrp_value_kind_t kind)
{
    return kind_is_text(kind);
}

void psrp_value_free(psrp_value_t *v)
{
    if (!v) return;
    if (kind_is_text(v->kind)) {
        free(v->as.text.ptr);
    } else if (v->kind == PSRP_VAL_BYTES) {
        free(v->as.bytes.ptr);
    } else if (v->kind == PSRP_VAL_OBJECT) {
        psrp_object_free(v->as.obj);
    }
    psrp_value_init(v);
}

#define SETTER(fn, field, type, kindval)                                      \
    void fn(psrp_value_t *v, type x)                                          \
    {                                                                         \
        if (!v) return;                                                       \
        psrp_value_free(v);                                                   \
        v->kind = kindval;                                                    \
        v->as.field = x;                                                      \
    }

void psrp_value_set_null(psrp_value_t *v)
{
    if (!v) return;
    psrp_value_free(v);
    v->kind = PSRP_VAL_NULL;
}

SETTER(psrp_value_set_bool,   b,   bool,     PSRP_VAL_BOOL)
SETTER(psrp_value_set_char,   ch,  uint16_t, PSRP_VAL_CHAR)
SETTER(psrp_value_set_uint8,  u8,  uint8_t,  PSRP_VAL_UINT8)
SETTER(psrp_value_set_int8,   i8,  int8_t,   PSRP_VAL_INT8)
SETTER(psrp_value_set_uint16, u16, uint16_t, PSRP_VAL_UINT16)
SETTER(psrp_value_set_int16,  i16, int16_t,  PSRP_VAL_INT16)
SETTER(psrp_value_set_uint32, u32, uint32_t, PSRP_VAL_UINT32)
SETTER(psrp_value_set_int32,  i32, int32_t,  PSRP_VAL_INT32)
SETTER(psrp_value_set_uint64, u64, uint64_t, PSRP_VAL_UINT64)
SETTER(psrp_value_set_int64,  i64, int64_t,  PSRP_VAL_INT64)
SETTER(psrp_value_set_single, f32, float,    PSRP_VAL_SINGLE)
SETTER(psrp_value_set_double, f64, double,   PSRP_VAL_DOUBLE)

void psrp_value_set_guid(psrp_value_t *v, const psrp_guid_t *g)
{
    if (!v || !g) return;
    psrp_value_free(v);
    v->kind = PSRP_VAL_GUID;
    v->as.guid = *g;
}

psrp_result_t psrp_value_set_text(psrp_value_t *v, psrp_value_kind_t kind,
                                  const char *utf8, size_t n)
{
    char *copy;

    if (!v) return PSRP_ERR_INVALID_ARG;
    if (!kind_is_text(kind)) return PSRP_ERR_INVALID_ARG;
    if (n && !utf8) return PSRP_ERR_INVALID_ARG;

    /* Always NUL-terminate for callers that want a C string, while keeping the
     * explicit length for strings containing U+0000. */
    copy = (char *)malloc(n + 1);
    if (!copy) return PSRP_ERR_NOMEM;
    if (n) memcpy(copy, utf8, n);
    copy[n] = '\0';

    psrp_value_free(v);
    v->kind = kind;
    v->as.text.ptr = copy;
    v->as.text.len = n;
    return PSRP_OK;
}

psrp_result_t psrp_value_set_string(psrp_value_t *v, const char *utf8)
{
    if (!utf8) return PSRP_ERR_INVALID_ARG;
    return psrp_value_set_text(v, PSRP_VAL_STRING, utf8, strlen(utf8));
}

psrp_result_t psrp_value_set_bytes(psrp_value_t *v, const void *data, size_t n)
{
    uint8_t *copy;

    if (!v) return PSRP_ERR_INVALID_ARG;
    if (n && !data) return PSRP_ERR_INVALID_ARG;

    copy = (uint8_t *)malloc(n ? n : 1);
    if (!copy) return PSRP_ERR_NOMEM;
    if (n) memcpy(copy, data, n);

    psrp_value_free(v);
    v->kind = PSRP_VAL_BYTES;
    v->as.bytes.ptr = copy;
    v->as.bytes.len = n;
    return PSRP_OK;
}

psrp_result_t psrp_value_set_object(psrp_value_t *v, psrp_object_t *obj)
{
    if (!v || !obj) return PSRP_ERR_INVALID_ARG;
    psrp_value_free(v);
    v->kind = PSRP_VAL_OBJECT;
    v->as.obj = obj;
    return PSRP_OK;
}
