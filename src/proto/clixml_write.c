/* CLIXML serialization ([MS-PSRP] 2.2.5).
 *
 * Element order inside <Obj> follows what real PowerShell emits:
 *   <TN>/<TNRef>, <ToString>, <Props>, <MS>, then container or primitive.
 *
 * Numeric formatting also follows PowerShell, which uses XML-schema lexical
 * forms: shortest round-trippable digits, uppercase exponent ("1E+20"), and
 * "NaN" / "INF" / "-INF" for the specials (not .NET's "Infinity").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_clixml.h"
#include "internal/psrp_clixml.h"
#include "internal/psrp_codec.h"

/* Identifier assignment.
 *
 * 2.2.5.2.1.1 requires an object identifier to be unique for the lifetime of a
 * serializer, and 2.2.5.3.3 makes that lifetime one message. Leaving it to the
 * builders does not work: a nested builder cannot know what the enclosing
 * document has already used, so every one of them numbered from zero and real
 * messages went out with the same RefId on several different objects.
 *
 * So the serializer owns the id space. An object that asked for an identifier
 * gets a fresh one here; an object that asked for none still gets none, since
 * the attribute is optional.
 *
 * Type identifiers are a separate space and, unlike object identifiers, are
 * actually referenced: an object with no type names of its own points at an
 * earlier <TN> with <TNRef>. So those are remapped through a table rather than
 * simply renumbered, or the reference would land on the wrong list.
 */
#define WRITER_TYPE_MAP_MAX 64

typedef struct {
    int64_t next_obj_id;
    int64_t next_type_id;
    struct { int64_t from, to; } type_map[WRITER_TYPE_MAP_MAX];
    size_t type_map_len;
} writer_ctx_t;

/* Records that a <TN> declared with `stored` was written as `assigned`. */
static void type_map_put(writer_ctx_t *w, int64_t stored, int64_t assigned)
{
    size_t i;
    for (i = 0; i < w->type_map_len; i++) {
        if (w->type_map[i].from == stored) {
            w->type_map[i].to = assigned;
            return;
        }
    }
    /* Overflowing simply means a later TNRef falls back to its stored id.
     * That is the behaviour this replaced, so it is no worse than before, and
     * 64 distinct type lists in one message does not occur in practice. */
    if (w->type_map_len < WRITER_TYPE_MAP_MAX) {
        w->type_map[w->type_map_len].from = stored;
        w->type_map[w->type_map_len].to = assigned;
        w->type_map_len++;
    }
}

static int64_t type_map_get(const writer_ctx_t *w, int64_t stored)
{
    size_t i;
    for (i = 0; i < w->type_map_len; i++)
        if (w->type_map[i].from == stored) return w->type_map[i].to;
    return stored;
}

static psrp_result_t write_value(const psrp_value_t *v, const char *name,
                                 psrp_buffer_t *out, writer_ctx_t *w);

/* ------------------------------------------------------- XML escaping ---- */

/* Escapes text content. PowerShell leaves quotes literal in content, so we do
 * too; only the markup-significant characters are escaped. */
static psrp_result_t append_xml_text(psrp_buffer_t *out, const char *s, size_t n)
{
    size_t i, run = 0;
    psrp_result_t rc;

    for (i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        default: run++; continue;
        }
        if (run) {
            rc = psrp_buffer_append(out, s + i - run, run);
            if (rc != PSRP_OK) return rc;
            run = 0;
        }
        rc = psrp_buffer_append_str(out, rep);
        if (rc != PSRP_OK) return rc;
    }
    if (run) return psrp_buffer_append(out, s + n - run, run);
    return PSRP_OK;
}

/* Attribute values are double-quoted, so quotes must be escaped as well. */
static psrp_result_t append_xml_attr(psrp_buffer_t *out, const char *s, size_t n)
{
    size_t i;
    psrp_result_t rc;
    for (i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) rc = psrp_buffer_append_str(out, rep);
        else rc = psrp_buffer_append(out, s + i, 1);
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}

/* Property names are strings and are encoded per 2.2.5.3.2 (2.2.5.3.1). */
static psrp_result_t append_name_attr(psrp_buffer_t *out, const char *name)
{
    psrp_buffer_t enc;
    psrp_result_t rc;

    if (!name) return PSRP_OK;
    psrp_buffer_init(&enc);
    rc = psrp_clixml_encode_string(name, strlen(name), &enc);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, " N=\"");
    if (rc == PSRP_OK) rc = append_xml_attr(out, (const char *)enc.data, enc.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, "\"");
    psrp_buffer_free(&enc);
    return rc;
}

/* ---------------------------------------------------- number formatting -- */

/* Shortest representation that reads back identically, matching .NET's
 * round-trip formatting. %G gives the uppercase exponent PowerShell emits. */
static void format_double(double x, char *buf, size_t buf_size)
{
    int precision;
    if (x != x) { snprintf(buf, buf_size, "NaN"); return; }
    if (x > 1.7976931348623157e308) { snprintf(buf, buf_size, "INF"); return; }
    if (x < -1.7976931348623157e308) { snprintf(buf, buf_size, "-INF"); return; }
    for (precision = 15; precision <= 17; precision++) {
        snprintf(buf, buf_size, "%.*G", precision, x);
        if (strtod(buf, NULL) == x) return;
    }
}

static void format_single(float x, char *buf, size_t buf_size)
{
    int precision;
    if (x != x) { snprintf(buf, buf_size, "NaN"); return; }
    if (x > 3.4028234663852886e38f) { snprintf(buf, buf_size, "INF"); return; }
    if (x < -3.4028234663852886e38f) { snprintf(buf, buf_size, "-INF"); return; }
    for (precision = 6; precision <= 9; precision++) {
        snprintf(buf, buf_size, "%.*G", precision, (double)x);
        if (strtof(buf, NULL) == x) return;
    }
}

/* ------------------------------------------------------------ elements --- */

static psrp_result_t open_tag(psrp_buffer_t *out, const char *elem,
                              const char *name)
{
    psrp_result_t rc = psrp_buffer_append_str(out, "<");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, elem);
    if (rc == PSRP_OK) rc = append_name_attr(out, name);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, ">");
    return rc;
}

static psrp_result_t close_tag(psrp_buffer_t *out, const char *elem)
{
    psrp_result_t rc = psrp_buffer_append_str(out, "</");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, elem);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, ">");
    return rc;
}

/* An element whose content is plain text, optionally _xHHHH_-escaped first. */
static psrp_result_t write_text_element(psrp_buffer_t *out, const char *elem,
                                        const char *name, const char *text,
                                        size_t len, bool escape)
{
    psrp_result_t rc = open_tag(out, elem, name);
    if (rc != PSRP_OK) return rc;

    if (escape) {
        psrp_buffer_t enc;
        psrp_buffer_init(&enc);
        rc = psrp_clixml_encode_string(text, len, &enc);
        if (rc == PSRP_OK)
            rc = append_xml_text(out, (const char *)enc.data, enc.len);
        psrp_buffer_free(&enc);
    } else {
        rc = append_xml_text(out, text, len);
    }
    if (rc != PSRP_OK) return rc;
    return close_tag(out, elem);
}

static psrp_result_t write_ref_attr(psrp_buffer_t *out, const char *elem,
                                    int64_t ref_id, bool self_closing)
{
    char num[32];
    psrp_result_t rc = psrp_buffer_append_str(out, "<");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, elem);
    if (rc == PSRP_OK && ref_id >= 0) {
        snprintf(num, sizeof num, " RefId=\"%lld\"", (long long)ref_id);
        rc = psrp_buffer_append_str(out, num);
    }
    if (rc == PSRP_OK)
        rc = psrp_buffer_append_str(out, self_closing ? " />" : ">");
    return rc;
}

static const char *container_element(psrp_container_kind_t k)
{
    switch (k) {
    case PSRP_CONTAINER_LIST:  return "LST";
    case PSRP_CONTAINER_STACK: return "STK";
    case PSRP_CONTAINER_QUEUE: return "QUE";
    case PSRP_CONTAINER_DICT:  return "DCT";
    default: return NULL;
    }
}

static psrp_result_t write_property_bag(const psrp_object_t *o, bool adapted,
                                        psrp_buffer_t *out, writer_ctx_t *w)
{
    size_t i, count = adapted ? psrp_object_adapted_count(o)
                              : psrp_object_extended_count(o);
    const char *elem = adapted ? "Props" : "MS";
    psrp_result_t rc;

    if (count == 0) return PSRP_OK;
    rc = psrp_buffer_append_str(out, adapted ? "<Props>" : "<MS>");
    if (rc != PSRP_OK) return rc;
    for (i = 0; i < count; i++) {
        const psrp_property_t *p = adapted ? psrp_object_adapted(o, i)
                                           : psrp_object_extended(o, i);
        rc = write_value(&p->value, p->name, out, w);
        if (rc != PSRP_OK) return rc;
    }
    return close_tag(out, elem);
}

static psrp_result_t write_object(const psrp_object_t *o, const char *name,
                                  psrp_buffer_t *out, writer_ctx_t *w)
{
    psrp_result_t rc;
    size_t i, n;
    int64_t ref_id;
    const char *ts;
    size_t ts_len = 0;
    const char *cont;
    const psrp_value_t *prim;

    if (!o) return PSRP_ERR_INVALID_ARG;

    /* <Obj RefId="n" N="name"> */
    rc = psrp_buffer_append_str(out, "<Obj");
    if (rc != PSRP_OK) return rc;
    ref_id = psrp_object_ref_id(o);
    if (ref_id >= 0) {
        char num[32];
        /* The stored value only says "this object wants an identifier"; the
         * value itself comes from the serializer so it cannot collide. */
        snprintf(num, sizeof num, " RefId=\"%lld\"",
                 (long long)w->next_obj_id++);
        rc = psrp_buffer_append_str(out, num);
        if (rc != PSRP_OK) return rc;
    }
    rc = append_name_attr(out, name);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, ">");
    if (rc != PSRP_OK) return rc;

    /* Type names (2.2.5.2.3). The same id names the list either way: an object
     * that carries the names defines it with <TN RefId="n">, and a later
     * object that shares the type refers back with <TNRef RefId="n" />. */
    n = psrp_object_type_name_count(o);
    if (n) {
        int64_t stored = psrp_object_type_ref_id(o);
        int64_t assigned = -1;
        if (stored >= 0) {
            assigned = w->next_type_id++;
            type_map_put(w, stored, assigned);
        }
        rc = write_ref_attr(out, "TN", assigned, false);
        if (rc != PSRP_OK) return rc;
        for (i = 0; i < n; i++) {
            const char *tn = psrp_object_type_name(o, i);
            rc = write_text_element(out, "T", NULL, tn, strlen(tn), true);
            if (rc != PSRP_OK) return rc;
        }
        rc = close_tag(out, "TN");
        if (rc != PSRP_OK) return rc;
    } else if (psrp_object_type_ref_id(o) >= 0) {
        /* Points back at the <TN> that declared this list, under whatever
         * identifier that one actually got. */
        rc = write_ref_attr(out, "TNRef",
                            type_map_get(w, psrp_object_type_ref_id(o)), true);
        if (rc != PSRP_OK) return rc;
    }

    /* <ToString> (2.2.5.2.4) */
    ts = psrp_object_to_string(o, &ts_len);
    if (ts) {
        rc = write_text_element(out, "ToString", NULL, ts, ts_len, true);
        if (rc != PSRP_OK) return rc;
    }

    rc = write_property_bag(o, true, out, w);   /* <Props>, adapted */
    if (rc != PSRP_OK) return rc;
    rc = write_property_bag(o, false, out, w);  /* <MS>, extended */
    if (rc != PSRP_OK) return rc;

    /* Container contents (2.2.5.2.6). */
    cont = container_element(psrp_object_container(o));
    if (cont) {
        rc = open_tag(out, cont, NULL);
        if (rc != PSRP_OK) return rc;
        if (psrp_object_container(o) == PSRP_CONTAINER_DICT) {
            for (i = 0; i < psrp_object_entry_count(o); i++) {
                const psrp_dict_entry_t *e = psrp_object_entry(o, i);
                rc = psrp_buffer_append_str(out, "<En>");
                if (rc == PSRP_OK) rc = write_value(&e->key, "Key", out, w);
                if (rc == PSRP_OK) rc = write_value(&e->value, "Value", out, w);
                if (rc == PSRP_OK) rc = close_tag(out, "En");
                if (rc != PSRP_OK) return rc;
            }
        } else {
            for (i = 0; i < psrp_object_item_count(o); i++) {
                rc = write_value(psrp_object_item(o, i), NULL, out, w);
                if (rc != PSRP_OK) return rc;
            }
        }
        rc = close_tag(out, cont);
        if (rc != PSRP_OK) return rc;
    }

    /* Extended primitive content (2.2.5.2.5). */
    prim = psrp_object_primitive(o);
    if (prim) {
        rc = write_value(prim, NULL, out, w);
        if (rc != PSRP_OK) return rc;
    }

    return close_tag(out, "Obj");
}

static psrp_result_t write_value(const psrp_value_t *v, const char *name,
                                 psrp_buffer_t *out, writer_ctx_t *w)
{
    char num[64];
    const char *elem;

    if (!v || !out) return PSRP_ERR_INVALID_ARG;
    elem = psrp_value_kind_element(v->kind);
    if (!elem) return PSRP_ERR_UNSUPPORTED;

    switch (v->kind) {
    case PSRP_VAL_NULL: {
        psrp_result_t rc = psrp_buffer_append_str(out, "<Nil");
        if (rc == PSRP_OK) rc = append_name_attr(out, name);
        if (rc == PSRP_OK) rc = psrp_buffer_append_str(out, " />");
        return rc;
    }
    case PSRP_VAL_OBJECT:
        return write_object(v->as.obj, name, out, w);

    case PSRP_VAL_BOOL:
        return write_text_element(out, elem, name, v->as.b ? "true" : "false",
                                  v->as.b ? 4u : 5u, false);
    case PSRP_VAL_CHAR:
        snprintf(num, sizeof num, "%u", (unsigned)v->as.ch);
        break;
    case PSRP_VAL_UINT8:
        snprintf(num, sizeof num, "%u", (unsigned)v->as.u8); break;
    case PSRP_VAL_INT8:
        snprintf(num, sizeof num, "%d", (int)v->as.i8); break;
    case PSRP_VAL_UINT16:
        snprintf(num, sizeof num, "%u", (unsigned)v->as.u16); break;
    case PSRP_VAL_INT16:
        snprintf(num, sizeof num, "%d", (int)v->as.i16); break;
    case PSRP_VAL_UINT32:
        snprintf(num, sizeof num, "%lu", (unsigned long)v->as.u32); break;
    case PSRP_VAL_INT32:
        snprintf(num, sizeof num, "%ld", (long)v->as.i32); break;
    case PSRP_VAL_UINT64:
        snprintf(num, sizeof num, "%llu", (unsigned long long)v->as.u64); break;
    case PSRP_VAL_INT64:
        snprintf(num, sizeof num, "%lld", (long long)v->as.i64); break;
    case PSRP_VAL_SINGLE:
        format_single(v->as.f32, num, sizeof num); break;
    case PSRP_VAL_DOUBLE:
        format_double(v->as.f64, num, sizeof num); break;

    case PSRP_VAL_GUID: {
        char guid[PSRP_GUID_BUF_SIZE];
        psrp_result_t rc = psrp_guid_format(&v->as.guid, guid, sizeof guid);
        if (rc != PSRP_OK) return rc;
        return write_text_element(out, elem, name, guid, strlen(guid), false);
    }
    case PSRP_VAL_BYTES: {
        psrp_buffer_t b64;
        psrp_result_t rc;
        psrp_buffer_init(&b64);
        rc = psrp_base64_encode_buf(&b64, v->as.bytes.ptr, v->as.bytes.len);
        if (rc == PSRP_OK)
            rc = write_text_element(out, elem, name, (const char *)b64.data,
                                    b64.len, false);
        psrp_buffer_free(&b64);
        return rc;
    }

    /* Text-valued kinds; only some are escaped (2.2.5.1 cites 2.2.5.3.2). */
    case PSRP_VAL_STRING:
    case PSRP_VAL_DATETIME:
    case PSRP_VAL_DURATION:
    case PSRP_VAL_DECIMAL:
    case PSRP_VAL_URI:
    case PSRP_VAL_VERSION:
    case PSRP_VAL_XMLDOC:
    case PSRP_VAL_SCRIPTBLOCK:
    case PSRP_VAL_SECURESTRING:
        return write_text_element(out, elem, name, v->as.text.ptr,
                                  v->as.text.len,
                                  psrp_value_kind_is_escaped(v->kind));
    default:
        return PSRP_ERR_UNSUPPORTED;
    }

    return write_text_element(out, elem, name, num, strlen(num), false);
}

psrp_result_t psrp_clixml_serialize(const psrp_value_t *v, psrp_buffer_t *out)
{
    /* A fresh context per call, which is exactly the serializer lifetime
     * 2.2.5.3.3 describes: identifiers are unique within one message and
     * restart for the next. */
    writer_ctx_t w;
    memset(&w, 0, sizeof w);
    return write_value(v, NULL, out, &w);
}

psrp_result_t psrp_clixml_serialize_named(const psrp_value_t *v,
                                          const char *name, psrp_buffer_t *out)
{
    writer_ctx_t w;
    memset(&w, 0, sizeof w);
    return write_value(v, name, out, &w);
}
