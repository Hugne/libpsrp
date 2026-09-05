/** @file
 * psrp_object.h - the PowerShell object model used by CLIXML ([MS-PSRP] 2.2.5).
 *
 * A psrp_value_t is either one of the primitive types of 2.2.5.1 or a complex
 * object (2.2.5.2). Values own their storage; psrp_value_free releases it.
 *
 * Design notes:
 *
 * - Decimal, DateTime and Duration are held as text rather than parsed into
 *   numeric or calendar types. The protocol only ever moves them through, and
 *   parsing them would lose precision (decimal) or drag in calendar handling
 *   we do not need. Callers that care can parse the text themselves.
 * - Strings are UTF-8 and carry an explicit length, because PowerShell strings
 *   may contain U+0000.
 */
#ifndef PSRP_OBJECT_H
#define PSRP_OBJECT_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_object psrp_object_t;

typedef enum psrp_value_kind {
    PSRP_VAL_NULL = 0,      /**< `<Nil>`      2.2.5.1.20 */
    PSRP_VAL_STRING,        /**< `<S>`        2.2.5.1.1  (escaped) */
    PSRP_VAL_CHAR,          /**< `<C>`        2.2.5.1.2  UTF-16 code unit */
    PSRP_VAL_BOOL,          /**< `<B>`        2.2.5.1.3  */
    PSRP_VAL_DATETIME,      /**< `<DT>`       2.2.5.1.4  text */
    PSRP_VAL_DURATION,      /**< `<TS>`       2.2.5.1.5  text */
    PSRP_VAL_UINT8,         /**< `<By>`       2.2.5.1.6  */
    PSRP_VAL_INT8,          /**< `<SB>`       2.2.5.1.7  */
    PSRP_VAL_UINT16,        /**< `<U16>`      2.2.5.1.8  */
    PSRP_VAL_INT16,         /**< `<I16>`      2.2.5.1.9  */
    PSRP_VAL_UINT32,        /**< `<U32>`      2.2.5.1.10 */
    PSRP_VAL_INT32,         /**< `<I32>`      2.2.5.1.11 */
    PSRP_VAL_UINT64,        /**< `<U64>`      2.2.5.1.12 */
    PSRP_VAL_INT64,         /**< `<I64>`      2.2.5.1.13 */
    PSRP_VAL_SINGLE,        /**< `<Sg>`       2.2.5.1.14 */
    PSRP_VAL_DOUBLE,        /**< `<Db>`       2.2.5.1.15 */
    PSRP_VAL_DECIMAL,       /**< `<D>`        2.2.5.1.16 text */
    PSRP_VAL_BYTES,         /**< `<BA>`       2.2.5.1.17 base64 */
    PSRP_VAL_GUID,          /**< `<G>`        2.2.5.1.18 */
    PSRP_VAL_URI,           /**< `<URI>`      2.2.5.1.19 (escaped) */
    PSRP_VAL_VERSION,       /**< `<Version>`  2.2.5.1.21 text */
    PSRP_VAL_XMLDOC,        /**< `<XD>`       2.2.5.1.22 (escaped) */
    PSRP_VAL_SCRIPTBLOCK,   /**< `<SBK>`      2.2.5.1.23 (escaped) */
    PSRP_VAL_SECURESTRING,  /**< `<SS>`       2.2.5.1.24 base64 ciphertext */
    PSRP_VAL_OBJECT         /**< `<Obj>`      2.2.5.2 */
} psrp_value_kind_t;

typedef struct psrp_value {
    psrp_value_kind_t kind;
    union {
        bool b;
        uint16_t ch;
        uint8_t u8;
        int8_t i8;
        uint16_t u16;
        int16_t i16;
        uint32_t u32;
        int32_t i32;
        uint64_t u64;
        int64_t i64;
        float f32;
        double f64;
        psrp_guid_t guid;
        struct { char *ptr; size_t len; } text;    /**< owned, UTF-8 */
        struct { uint8_t *ptr; size_t len; } bytes;/* owned */
        psrp_object_t *obj;                        /**< owned */
    } as;
} psrp_value_t;

/** The element name for a kind, e.g. "S", "I32", "Nil". NULL if unknown. */
const char *psrp_value_kind_element(psrp_value_kind_t kind);
/** Reverse lookup; false if `element` is not a primitive element name. */
bool psrp_value_kind_from_element(const char *element, psrp_value_kind_t *out);
/** True if the kind's text content is escaped per 2.2.5.3.2. */
bool psrp_value_kind_is_escaped(psrp_value_kind_t kind);

/** Leaves the value as PSRP_VAL_NULL. Always safe to call before use. */
void psrp_value_init(psrp_value_t *v);
/** Releases owned storage and resets to PSRP_VAL_NULL. Idempotent. */
void psrp_value_free(psrp_value_t *v);

/** Scalar setters. */
void psrp_value_set_null(psrp_value_t *v);
void psrp_value_set_bool(psrp_value_t *v, bool x);
void psrp_value_set_char(psrp_value_t *v, uint16_t code_unit);
void psrp_value_set_uint8(psrp_value_t *v, uint8_t x);
void psrp_value_set_int8(psrp_value_t *v, int8_t x);
void psrp_value_set_uint16(psrp_value_t *v, uint16_t x);
void psrp_value_set_int16(psrp_value_t *v, int16_t x);
void psrp_value_set_uint32(psrp_value_t *v, uint32_t x);
void psrp_value_set_int32(psrp_value_t *v, int32_t x);
void psrp_value_set_uint64(psrp_value_t *v, uint64_t x);
void psrp_value_set_int64(psrp_value_t *v, int64_t x);
void psrp_value_set_single(psrp_value_t *v, float x);
void psrp_value_set_double(psrp_value_t *v, double x);
void psrp_value_set_guid(psrp_value_t *v, const psrp_guid_t *g);

/** Text-valued kinds. `n` may be 0; the text is copied. Passing a kind that is
 * not text-valued returns PSRP_ERR_INVALID_ARG. */
psrp_result_t psrp_value_set_text(psrp_value_t *v, psrp_value_kind_t kind,
                                  const char *utf8, size_t n);
/** Convenience for PSRP_VAL_STRING from a NUL-terminated C string. */
psrp_result_t psrp_value_set_string(psrp_value_t *v, const char *utf8);

/** Byte-valued kinds (PSRP_VAL_BYTES). The data is copied. */
psrp_result_t psrp_value_set_bytes(psrp_value_t *v, const void *data, size_t n);

/** Takes ownership of `obj`; on failure the caller keeps ownership. */
psrp_result_t psrp_value_set_object(psrp_value_t *v, psrp_object_t *obj);

/* ------------------------------------ 2.2.5.2 Complex Objects (`<Obj>`) ---- */

/** Known containers, 2.2.5.2.6. */
typedef enum psrp_container_kind {
    PSRP_CONTAINER_NONE = 0,
    PSRP_CONTAINER_LIST,    /**< `<LST>` 2.2.5.2.6.3 */
    PSRP_CONTAINER_STACK,   /**< `<STK>` 2.2.5.2.6.1 */
    PSRP_CONTAINER_QUEUE,   /**< `<QUE>` 2.2.5.2.6.2 */
    PSRP_CONTAINER_DICT     /**< `<DCT>` 2.2.5.2.6.4 */
} psrp_container_kind_t;

/** A name/value pair. `name` is the N="..." attribute (2.2.5.3.1). */
typedef struct psrp_property {
    char *name;             /**< owned; NULL for unnamed values */
    psrp_value_t value;
} psrp_property_t;

typedef struct psrp_dict_entry {
    psrp_value_t key;
    psrp_value_t value;
} psrp_dict_entry_t;

psrp_object_t *psrp_object_new(void);
void psrp_object_free(psrp_object_t *o);

/** RefId (2.2.5.2.1.1). Negative means "no RefId attribute". */
void psrp_object_set_ref_id(psrp_object_t *o, int64_t ref_id);
int64_t psrp_object_ref_id(const psrp_object_t *o);

/** Type names (2.2.5.2.3). The ref id names the type list in a single id
 * space: an object holding names serializes as `<TN RefId="n">...</TN>`, and a
 * later object with no names but the same id serializes as
 * `<TNRef RefId="n" />`. */
psrp_result_t psrp_object_add_type_name(psrp_object_t *o, const char *name);
size_t psrp_object_type_name_count(const psrp_object_t *o);
const char *psrp_object_type_name(const psrp_object_t *o, size_t index);
void psrp_object_set_type_ref_id(psrp_object_t *o, int64_t ref_id);
int64_t psrp_object_type_ref_id(const psrp_object_t *o);

/** ToString (2.2.5.2.4). */
psrp_result_t psrp_object_set_to_string(psrp_object_t *o, const char *utf8, size_t n);
const char *psrp_object_to_string(const psrp_object_t *o, size_t *len);

/** Properties. Adapted go in `<Props>` (2.2.5.2.8), extended in `<MS>`
 * (2.2.5.2.9). Both take ownership of *value on success and reset it. */
psrp_result_t psrp_object_add_adapted(psrp_object_t *o, const char *name,
                                      psrp_value_t *value);
psrp_result_t psrp_object_add_extended(psrp_object_t *o, const char *name,
                                       psrp_value_t *value);
size_t psrp_object_adapted_count(const psrp_object_t *o);
size_t psrp_object_extended_count(const psrp_object_t *o);
const psrp_property_t *psrp_object_adapted(const psrp_object_t *o, size_t i);
const psrp_property_t *psrp_object_extended(const psrp_object_t *o, size_t i);
/** Finds an extended (then adapted) property by name; NULL if absent. */
const psrp_value_t *psrp_object_find(const psrp_object_t *o, const char *name);

/** Containers. Items are used by LST/STK/QUE, entries by DCT. */
void psrp_object_set_container(psrp_object_t *o, psrp_container_kind_t kind);
psrp_container_kind_t psrp_object_container(const psrp_object_t *o);
psrp_result_t psrp_object_add_item(psrp_object_t *o, psrp_value_t *value);
size_t psrp_object_item_count(const psrp_object_t *o);
const psrp_value_t *psrp_object_item(const psrp_object_t *o, size_t i);
psrp_result_t psrp_object_add_entry(psrp_object_t *o, psrp_value_t *key,
                                    psrp_value_t *value);
size_t psrp_object_entry_count(const psrp_object_t *o);
const psrp_dict_entry_t *psrp_object_entry(const psrp_object_t *o, size_t i);

/** Deep copies. Needed because every add_* entry point takes ownership of the
 * value it is given, so handing one value to two messages requires a copy.
 * RefIds are preserved verbatim; the writer assigns its own when serializing. */
psrp_result_t psrp_value_clone(const psrp_value_t *src, psrp_value_t *dst);
psrp_result_t psrp_object_clone(const psrp_object_t *src, psrp_object_t **out);

/** True when the kind stores its content as text rather than in the union. */
bool psrp_value_kind_has_text(psrp_value_kind_t kind);

/** Extended primitive object (2.2.5.2.5): an `<Obj>` whose content is a
 * primitive value alongside ToString / extended properties. */
psrp_result_t psrp_object_set_primitive(psrp_object_t *o, psrp_value_t *value);
const psrp_value_t *psrp_object_primitive(const psrp_object_t *o);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_OBJECT_H */
