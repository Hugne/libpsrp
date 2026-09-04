/* CLIXML deserialization ([MS-PSRP] 2.2.5).
 *
 * Recursive descent over the psrp_xml.h pull parser. The XML backend has
 * already resolved XML entities, so the only decoding left here is the
 * _xHHHH_ escaping of 2.2.5.3.2, applied to the kinds whose spec sections
 * cite it.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_clixml.h"
#include "internal/psrp_clixml.h"
#include "internal/psrp_codec.h"
#include "internal/psrp_xml.h"

static psrp_result_t parse_value(psrp_xml_reader_t *r, psrp_value_t *out);

static char *dup_cstr(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* Collects character data up to the element's end tag. */
static psrp_result_t gather_text(psrp_xml_reader_t *r, bool empty,
                                 psrp_buffer_t *text)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;

    if (empty) return PSRP_OK;   /* <Foo /> has no end tag and no content */

    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_TEXT) {
            size_t len = 0;
            const char *v = psrp_xml_value(r, &len);
            if (text) {          /* a NULL sink means "consume and discard" */
                rc = psrp_buffer_append(text, v, len);
                if (rc != PSRP_OK) return rc;
            }
            continue;
        }
        /* A primitive element must not contain child elements. */
        return PSRP_ERR_MALFORMED;
    }
}

/* ------------------------------------------------------ number parsing --- */

static psrp_result_t parse_i64(const char *s, int64_t lo, int64_t hi, int64_t *out)
{
    char *end = NULL;
    long long v;
    if (!s || !*s) return PSRP_ERR_MALFORMED;
    v = strtoll(s, &end, 10);
    if (end == s || (end && *end != '\0')) return PSRP_ERR_MALFORMED;
    if ((int64_t)v < lo || (int64_t)v > hi) return PSRP_ERR_MALFORMED;
    *out = (int64_t)v;
    return PSRP_OK;
}

static psrp_result_t parse_u64(const char *s, uint64_t hi, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !*s) return PSRP_ERR_MALFORMED;
    /* strtoull silently wraps a leading '-'; reject it explicitly. */
    if (s[0] == '-') return PSRP_ERR_MALFORMED;
    v = strtoull(s, &end, 10);
    if (end == s || (end && *end != '\0')) return PSRP_ERR_MALFORMED;
    if ((uint64_t)v > hi) return PSRP_ERR_MALFORMED;
    *out = (uint64_t)v;
    return PSRP_OK;
}

static bool text_is(const char *s, const char *lit)
{
    return s && strcmp(s, lit) == 0;
}

/* Applies the kind's text to the value. */
static psrp_result_t value_from_text(psrp_value_kind_t kind, const char *text,
                                     size_t len, psrp_value_t *out)
{
    psrp_result_t rc;
    int64_t sv;
    uint64_t uv;

    switch (kind) {
    case PSRP_VAL_BOOL:
        /* XML schema boolean also permits 1 and 0. */
        if (text_is(text, "true") || text_is(text, "1")) psrp_value_set_bool(out, true);
        else if (text_is(text, "false") || text_is(text, "0")) psrp_value_set_bool(out, false);
        else return PSRP_ERR_MALFORMED;
        return PSRP_OK;

    case PSRP_VAL_CHAR:
        rc = parse_u64(text, 0xFFFFu, &uv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_char(out, (uint16_t)uv);
        return PSRP_OK;

    case PSRP_VAL_UINT8:
        rc = parse_u64(text, 0xFFu, &uv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_uint8(out, (uint8_t)uv);
        return PSRP_OK;
    case PSRP_VAL_UINT16:
        rc = parse_u64(text, 0xFFFFu, &uv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_uint16(out, (uint16_t)uv);
        return PSRP_OK;
    case PSRP_VAL_UINT32:
        rc = parse_u64(text, 0xFFFFFFFFu, &uv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_uint32(out, (uint32_t)uv);
        return PSRP_OK;
    case PSRP_VAL_UINT64:
        rc = parse_u64(text, 0xFFFFFFFFFFFFFFFFull, &uv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_uint64(out, uv);
        return PSRP_OK;

    case PSRP_VAL_INT8:
        rc = parse_i64(text, -128, 127, &sv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_int8(out, (int8_t)sv);
        return PSRP_OK;
    case PSRP_VAL_INT16:
        rc = parse_i64(text, -32768, 32767, &sv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_int16(out, (int16_t)sv);
        return PSRP_OK;
    case PSRP_VAL_INT32:
        rc = parse_i64(text, -2147483647LL - 1, 2147483647LL, &sv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_int32(out, (int32_t)sv);
        return PSRP_OK;
    case PSRP_VAL_INT64:
        rc = parse_i64(text, (-9223372036854775807LL - 1),
                       9223372036854775807LL, &sv);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_int64(out, sv);
        return PSRP_OK;

    case PSRP_VAL_SINGLE:
    case PSRP_VAL_DOUBLE: {
        double d;
        char *end = NULL;
        if (!text || !*text) return PSRP_ERR_MALFORMED;
        /* XML schema spellings, which strtod does not know. */
        if (text_is(text, "INF")) d = 1e308 * 10.0;
        else if (text_is(text, "-INF")) d = -(1e308 * 10.0);
        else if (text_is(text, "NaN")) { d = 1e308 * 10.0; d = d - d; }
        else {
            d = strtod(text, &end);
            if (end == text || (end && *end != '\0')) return PSRP_ERR_MALFORMED;
        }
        if (kind == PSRP_VAL_SINGLE) psrp_value_set_single(out, (float)d);
        else psrp_value_set_double(out, d);
        return PSRP_OK;
    }

    case PSRP_VAL_GUID: {
        psrp_guid_t g;
        rc = psrp_guid_parse(text ? text : "", &g);
        if (rc != PSRP_OK) return rc;
        psrp_value_set_guid(out, &g);
        return PSRP_OK;
    }

    case PSRP_VAL_BYTES: {
        psrp_buffer_t raw;
        psrp_buffer_init(&raw);
        rc = psrp_base64_decode(text ? text : "", len, &raw);
        if (rc == PSRP_OK) rc = psrp_value_set_bytes(out, raw.data, raw.len);
        psrp_buffer_free(&raw);
        return rc;
    }

    default:
        break;
    }

    /* Remaining kinds are text-valued; some are _xHHHH_ escaped. */
    if (psrp_value_kind_is_escaped(kind)) {
        psrp_buffer_t dec;
        psrp_buffer_init(&dec);
        rc = psrp_clixml_decode_string(text, len, &dec);
        if (rc == PSRP_OK)
            rc = psrp_value_set_text(out, kind, (const char *)dec.data, dec.len);
        psrp_buffer_free(&dec);
        return rc;
    }
    return psrp_value_set_text(out, kind, text, len);
}

/* ---------------------------------------------------- complex objects ---- */

static psrp_result_t parse_type_names(psrp_xml_reader_t *r, psrp_object_t *o,
                                      bool empty)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;
    const char *ref = psrp_xml_attr(r, "RefId");

    if (ref) {
        int64_t id;
        if (parse_i64(ref, 0, 0x7FFFFFFFLL, &id) == PSRP_OK)
            psrp_object_set_type_ref_id(o, id);
    }
    if (empty) return PSRP_OK;

    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_ELEMENT) {
            bool t_empty = psrp_xml_is_empty_element(r);
            psrp_buffer_t text;
            bool is_t = strcmp(psrp_xml_local_name(r), "T") == 0;
            psrp_buffer_init(&text);
            rc = gather_text(r, t_empty, &text);
            if (rc == PSRP_OK && is_t) {
                psrp_buffer_t dec;
                psrp_buffer_init(&dec);
                rc = psrp_clixml_decode_string(text.data, text.len, &dec);
                if (rc == PSRP_OK) {
                    rc = psrp_buffer_append_u8(&dec, 0);
                    if (rc == PSRP_OK)
                        rc = psrp_object_add_type_name(o, (const char *)dec.data);
                }
                psrp_buffer_free(&dec);
            }
            psrp_buffer_free(&text);
            if (rc != PSRP_OK) return rc;
            continue;
        }
        if (nt == PSRP_XML_TEXT) continue;
        return PSRP_ERR_MALFORMED;
    }
}

/* Parses the children of <Props> or <MS> into the object. */
static psrp_result_t parse_property_bag(psrp_xml_reader_t *r, psrp_object_t *o,
                                        bool adapted, bool empty)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;

    if (empty) return PSRP_OK;
    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_TEXT) continue;
        if (nt != PSRP_XML_ELEMENT) return PSRP_ERR_MALFORMED;
        {
            /* The N attribute must be copied before parse_value advances. */
            char *name = dup_cstr(psrp_xml_attr(r, "N"));
            psrp_value_t v;
            psrp_value_init(&v);
            rc = parse_value(r, &v);
            if (rc == PSRP_OK)
                rc = adapted ? psrp_object_add_adapted(o, name, &v)
                             : psrp_object_add_extended(o, name, &v);
            free(name);
            psrp_value_free(&v);
            if (rc != PSRP_OK) return rc;
        }
    }
}

static psrp_result_t parse_dict(psrp_xml_reader_t *r, psrp_object_t *o, bool empty)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;

    if (empty) return PSRP_OK;
    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_TEXT) continue;
        if (nt != PSRP_XML_ELEMENT) return PSRP_ERR_MALFORMED;

        /* Each entry is <En> with children named Key and Value. */
        if (strcmp(psrp_xml_local_name(r), "En") != 0) return PSRP_ERR_MALFORMED;
        if (psrp_xml_is_empty_element(r)) continue;
        {
            psrp_value_t key, val;
            bool have_key = false, have_val = false;
            psrp_value_init(&key);
            psrp_value_init(&val);
            for (;;) {
                rc = psrp_xml_read(r, &nt);
                if (rc != PSRP_OK) break;
                if (nt == PSRP_XML_END_ELEMENT) break;
                if (nt == PSRP_XML_TEXT) continue;
                if (nt != PSRP_XML_ELEMENT) { rc = PSRP_ERR_MALFORMED; break; }
                {
                    const char *n = psrp_xml_attr(r, "N");
                    bool is_key = n && strcmp(n, "Key") == 0;
                    bool is_val = n && strcmp(n, "Value") == 0;
                    psrp_value_t tmp;
                    psrp_value_init(&tmp);
                    rc = parse_value(r, &tmp);
                    if (rc != PSRP_OK) { psrp_value_free(&tmp); break; }
                    if (is_key) { psrp_value_free(&key); key = tmp; have_key = true; }
                    else if (is_val) { psrp_value_free(&val); val = tmp; have_val = true; }
                    else psrp_value_free(&tmp);
                }
            }
            if (rc == PSRP_OK && have_key && have_val)
                rc = psrp_object_add_entry(o, &key, &val);
            psrp_value_free(&key);
            psrp_value_free(&val);
            if (rc != PSRP_OK) return rc;
        }
    }
}

static psrp_result_t parse_item_list(psrp_xml_reader_t *r, psrp_object_t *o,
                                     bool empty)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;

    if (empty) return PSRP_OK;
    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_TEXT) continue;
        if (nt != PSRP_XML_ELEMENT) return PSRP_ERR_MALFORMED;
        {
            psrp_value_t v;
            psrp_value_init(&v);
            rc = parse_value(r, &v);
            if (rc == PSRP_OK) rc = psrp_object_add_item(o, &v);
            psrp_value_free(&v);
            if (rc != PSRP_OK) return rc;
        }
    }
}

static psrp_result_t parse_object(psrp_xml_reader_t *r, psrp_object_t *o)
{
    psrp_xml_node_t nt;
    psrp_result_t rc;
    const char *ref = psrp_xml_attr(r, "RefId");

    if (ref) {
        int64_t id;
        if (parse_i64(ref, 0, 0x7FFFFFFFLL, &id) == PSRP_OK)
            psrp_object_set_ref_id(o, id);
    }
    if (psrp_xml_is_empty_element(r)) return PSRP_OK;

    for (;;) {
        const char *name;
        bool empty;

        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) return rc;
        if (nt == PSRP_XML_END_ELEMENT) return PSRP_OK;
        if (nt == PSRP_XML_TEXT) continue;
        if (nt != PSRP_XML_ELEMENT) return PSRP_ERR_MALFORMED;

        name = psrp_xml_local_name(r);
        empty = psrp_xml_is_empty_element(r);

        if (strcmp(name, "TN") == 0) {
            rc = parse_type_names(r, o, empty);
        } else if (strcmp(name, "TNRef") == 0) {
            const char *tref = psrp_xml_attr(r, "RefId");
            int64_t id;
            if (tref && parse_i64(tref, 0, 0x7FFFFFFFLL, &id) == PSRP_OK)
                psrp_object_set_type_ref_id(o, id);
            /* <TNRef> is normally written empty; if not, consume to its
             * end tag rather than mis-reading the next sibling. */
            rc = gather_text(r, empty, NULL);
        } else if (strcmp(name, "ToString") == 0) {
            psrp_buffer_t text, dec;
            psrp_buffer_init(&text);
            psrp_buffer_init(&dec);
            rc = gather_text(r, empty, &text);
            if (rc == PSRP_OK)
                rc = psrp_clixml_decode_string(text.data, text.len, &dec);
            if (rc == PSRP_OK)
                rc = psrp_object_set_to_string(o, (const char *)dec.data, dec.len);
            psrp_buffer_free(&text);
            psrp_buffer_free(&dec);
        } else if (strcmp(name, "Props") == 0) {
            rc = parse_property_bag(r, o, true, empty);
        } else if (strcmp(name, "MS") == 0) {
            rc = parse_property_bag(r, o, false, empty);
        } else if (strcmp(name, "LST") == 0 || strcmp(name, "STK") == 0 ||
                   strcmp(name, "QUE") == 0) {
            psrp_object_set_container(o, strcmp(name, "LST") == 0
                                          ? PSRP_CONTAINER_LIST
                                          : (strcmp(name, "STK") == 0
                                             ? PSRP_CONTAINER_STACK
                                             : PSRP_CONTAINER_QUEUE));
            rc = parse_item_list(r, o, empty);
        } else if (strcmp(name, "DCT") == 0) {
            psrp_object_set_container(o, PSRP_CONTAINER_DICT);
            rc = parse_dict(r, o, empty);
        } else {
            /* Anything else is extended-primitive content (2.2.5.2.5). */
            psrp_value_t v;
            psrp_value_init(&v);
            rc = parse_value(r, &v);
            if (rc == PSRP_OK) rc = psrp_object_set_primitive(o, &v);
            psrp_value_free(&v);
        }
        if (rc != PSRP_OK) return rc;
    }
}

/* Precondition: the reader sits on an ELEMENT node. */
static psrp_result_t parse_value(psrp_xml_reader_t *r, psrp_value_t *out)
{
    const char *name = psrp_xml_local_name(r);
    bool empty = psrp_xml_is_empty_element(r);
    psrp_value_kind_t kind;
    psrp_buffer_t text;
    psrp_result_t rc;

    if (!name) return PSRP_ERR_MALFORMED;

    if (strcmp(name, "Obj") == 0) {
        psrp_object_t *o = psrp_object_new();
        if (!o) return PSRP_ERR_NOMEM;
        rc = parse_object(r, o);
        if (rc != PSRP_OK) { psrp_object_free(o); return rc; }
        rc = psrp_value_set_object(out, o);
        if (rc != PSRP_OK) psrp_object_free(o);
        return rc;
    }

    if (!psrp_value_kind_from_element(name, &kind))
        return PSRP_ERR_UNSUPPORTED;

    if (kind == PSRP_VAL_NULL) {
        psrp_value_set_null(out);
        return gather_text(r, empty, NULL);
    }

    psrp_buffer_init(&text);
    rc = gather_text(r, empty, &text);
    if (rc == PSRP_OK) {
        /* NUL-terminate for the strtol family, keeping the explicit length. */
        size_t len = text.len;
        rc = psrp_buffer_append_u8(&text, 0);
        if (rc == PSRP_OK)
            rc = value_from_text(kind, (const char *)text.data, len, out);
    }
    psrp_buffer_free(&text);
    return rc;
}

psrp_result_t psrp_clixml_deserialize(const void *utf8, size_t n,
                                      psrp_value_t *out)
{
    psrp_xml_reader_t *r = NULL;
    psrp_xml_node_t nt;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    psrp_value_init(out);

    rc = psrp_xml_reader_create(utf8, n, &r);
    if (rc != PSRP_OK) return rc;

    for (;;) {
        rc = psrp_xml_read(r, &nt);
        if (rc != PSRP_OK) goto done;
        if (nt == PSRP_XML_EOF) { rc = PSRP_ERR_MALFORMED; goto done; }
        if (nt != PSRP_XML_ELEMENT) continue;

        /* PSRP message payloads are a bare value. PSSerializer wraps its
         * output in <Objs>; accepting that lets us read PowerShell's own
         * output directly, which the golden-vector tests rely on. */
        if (strcmp(psrp_xml_local_name(r), "Objs") == 0) continue;
        break;
    }

    rc = parse_value(r, out);

done:
    psrp_xml_reader_free(r);
    if (rc != PSRP_OK) psrp_value_free(out);
    return rc;
}
