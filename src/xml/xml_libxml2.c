/* xml_libxml2.c - the psrp_xml.h pull-parser seam, on libxml2.
 *
 * The counterpart to xml_xmllite.c. libxml2's xmlTextReader is already a pull
 * parser, so this is mostly a mapping exercise; what deserves attention is
 * where the two backends must agree, because everything above this file is
 * shared and the CLIXML tests are identical on both platforms:
 *
 *   - whitespace-only text between elements is skipped, so pretty-printed and
 *     compact CLIXML parse alike;
 *   - CDATA counts as text, since a server may legally use it;
 *   - `<Foo />` reports no END_ELEMENT, matching XmlLite, which is the whole
 *     reason psrp_xml_is_empty_element exists;
 *   - every attribute of the current element is collected when that element
 *     is read, so several psrp_xml_attr results are valid at once. The CLIXML
 *     reader relies on that -- it holds `N` and `RefId` together -- and a
 *     backend handing out one reusable buffer would corrupt one of them;
 *   - names and values are UTF-8 copies, so they survive until the next read
 *     no matter what libxml2 does with its internal buffers.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/xmlreader.h>

#include "internal/psrp_xml.h"

typedef struct {
    char *name;
    char *value;
} attr_t;

struct psrp_xml_reader {
    xmlTextReaderPtr r;
    char *name;          /* current element's local name, owned */
    char *value;         /* current text node's value, owned */
    size_t value_len;
    attr_t *attrs;       /* current element's attributes, owned */
    size_t attr_count;
    bool empty;
};

static char *dup_xml(const xmlChar *s, size_t *len_out)
{
    size_t n;
    char *copy;

    if (!s) { if (len_out) *len_out = 0; return NULL; }

    n = strlen((const char *)s);
    copy = (char *)malloc(n + 1);
    if (!copy) { if (len_out) *len_out = 0; return NULL; }
    memcpy(copy, s, n + 1);
    if (len_out) *len_out = n;
    return copy;
}

static void drop_attrs(psrp_xml_reader_t *r)
{
    size_t i;
    for (i = 0; i < r->attr_count; i++) {
        free(r->attrs[i].name);
        free(r->attrs[i].value);
    }
    free(r->attrs);
    r->attrs = NULL;
    r->attr_count = 0;
}

/* Collects every attribute of the element the reader is positioned on, then
 * restores that position. */
static psrp_result_t collect_attrs(psrp_xml_reader_t *r)
{
    int more;

    drop_attrs(r);

    more = xmlTextReaderMoveToFirstAttribute(r->r);
    while (more == 1) {
        attr_t *grown = (attr_t *)realloc(r->attrs,
                                          (r->attr_count + 1) * sizeof *grown);
        if (!grown) return PSRP_ERR_NOMEM;
        r->attrs = grown;

        r->attrs[r->attr_count].name =
            dup_xml(xmlTextReaderConstLocalName(r->r), NULL);
        r->attrs[r->attr_count].value =
            dup_xml(xmlTextReaderConstValue(r->r), NULL);
        if (!r->attrs[r->attr_count].name) {
            free(r->attrs[r->attr_count].value);
            return PSRP_ERR_NOMEM;
        }
        r->attr_count++;
        more = xmlTextReaderMoveToNextAttribute(r->r);
    }
    if (more < 0) return PSRP_ERR_XML;

    xmlTextReaderMoveToElement(r->r);
    return PSRP_OK;
}

static bool is_blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
    return true;
}

/* libxml2 prints to stderr when no handler is installed. This one exists to
 * make sure one always is; the caller is told about malformed input through
 * the return code.
 *
 * Deliberately the READER's error handler rather than the structured one:
 * xmlStructuredErrorFunc takes xmlErrorPtr on libxml2 2.9 and const xmlError*
 * from 2.12, so a callback written for either fails to compile against the
 * other under -Werror. This signature has not changed. */
static void swallow_error(void *arg, const char *msg,
                          xmlParserSeverities severity,
                          xmlTextReaderLocatorPtr locator)
{
    (void)arg; (void)msg; (void)severity; (void)locator;
}

psrp_result_t psrp_xml_reader_create(const void *utf8, size_t n,
                                     psrp_xml_reader_t **out)
{
    psrp_xml_reader_t *r;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;
    if (!utf8 && n) return PSRP_ERR_INVALID_ARG;
    if (n > (size_t)INT_MAX) return PSRP_ERR_OVERFLOW;

    r = (psrp_xml_reader_t *)calloc(1, sizeof *r);
    if (!r) return PSRP_ERR_NOMEM;

    /* The input is borrowed rather than copied, as the seam specifies.
     *
     * XML_PARSE_NONET because resolving anything over the network would be a
     * bug at best: CLIXML has no legitimate external references.
     *
     * NOERROR and NOWARNING because libxml2 otherwise writes parse diagnostics
     * straight to stderr. Malformed input is an ordinary, tested outcome here
     * -- it is what a hostile or broken server sends -- and the caller learns
     * about it from PSRP_ERR_XML, not from text appearing on their terminal.
     * XmlLite is silent, and the two backends have to behave alike. */
    r->r = xmlReaderForMemory((const char *)utf8, (int)n, NULL, "UTF-8",
                              XML_PARSE_NONET | XML_PARSE_NOERROR |
                              XML_PARSE_NOWARNING);
    if (!r->r) { free(r); return PSRP_ERR_XML; }

    /* The flags above are not enough on their own, which took a fuzz target
     * feeding NUL bytes on an older libxml2 to show: an ENCODING error is
     * reported through the error handler rather than as a parse diagnostic,
     * so NOERROR does not cover it and "input conversion failed due to input
     * error, bytes 0x00 0x00 0x00 0x00" lands on the caller's stderr anyway.
     *
     * Silencing the handler covers every diagnostic libxml2 has, present and
     * future, rather than the subset a flag happens to name. It is set on
     * this reader rather than globally: xmlSetGenericErrorFunc would replace
     * whatever the embedding application installed, and a library has no
     * business doing that to its host. */
    xmlTextReaderSetErrorHandler(r->r, swallow_error, NULL);

    *out = r;
    return PSRP_OK;
}

void psrp_xml_reader_free(psrp_xml_reader_t *r)
{
    if (!r) return;
    if (r->r) xmlFreeTextReader(r->r);
    drop_attrs(r);
    free(r->name);
    free(r->value);
    free(r);
}

psrp_result_t psrp_xml_read(psrp_xml_reader_t *r, psrp_xml_node_t *type)
{
    if (!r || !type) return PSRP_ERR_INVALID_ARG;

    for (;;) {
        int rc = xmlTextReaderRead(r->r);
        int node;

        if (rc == 0) { *type = PSRP_XML_EOF; return PSRP_OK; }
        if (rc < 0) return PSRP_ERR_XML;

        node = xmlTextReaderNodeType(r->r);
        switch (node) {
        case XML_READER_TYPE_ELEMENT: {
            psrp_result_t arc;
            char *nm = dup_xml(xmlTextReaderConstLocalName(r->r), NULL);
            if (!nm) return PSRP_ERR_NOMEM;
            free(r->name);
            r->name = nm;
            r->empty = xmlTextReaderIsEmptyElement(r->r) == 1;
            arc = collect_attrs(r);
            if (arc != PSRP_OK) return arc;
            *type = PSRP_XML_ELEMENT;
            return PSRP_OK;
        }

        case XML_READER_TYPE_END_ELEMENT: {
            char *nm = dup_xml(xmlTextReaderConstLocalName(r->r), NULL);
            if (!nm) return PSRP_ERR_NOMEM;
            free(r->name);
            r->name = nm;
            r->empty = false;
            drop_attrs(r);
            *type = PSRP_XML_END_ELEMENT;
            return PSRP_OK;
        }

        case XML_READER_TYPE_TEXT:
        case XML_READER_TYPE_CDATA: {
            const xmlChar *v = xmlTextReaderConstValue(r->r);
            char *copy;
            size_t len;
            /* Whitespace between elements is layout, not content. libxml2
             * only sometimes classifies it as WHITESPACE, so judge the text
             * itself rather than trusting the node type. */
            if (v && is_blank((const char *)v)) continue;
            copy = dup_xml(v, &len);
            if (v && !copy) return PSRP_ERR_NOMEM;
            free(r->value);
            r->value = copy;
            r->value_len = len;
            *type = PSRP_XML_TEXT;
            return PSRP_OK;
        }

        default:
            continue;   /* comments, PIs, declarations: not significant here */
        }
    }
}

const char *psrp_xml_local_name(const psrp_xml_reader_t *r)
{
    return r && r->name ? r->name : "";
}

const char *psrp_xml_value(const psrp_xml_reader_t *r, size_t *len)
{
    if (!r || !r->value) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = r->value_len;
    return r->value;
}

const char *psrp_xml_attr(const psrp_xml_reader_t *r, const char *name)
{
    size_t i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->attr_count; i++)
        if (r->attrs[i].name && strcmp(r->attrs[i].name, name) == 0)
            return r->attrs[i].value;
    return NULL;
}

bool psrp_xml_is_empty_element(const psrp_xml_reader_t *r)
{
    return r ? r->empty : false;
}
