/* psrp_xml.h - internal pull-parser interface over an XML backend.
 *
 * The only backend today is XmlLite (Microsoft's standard native XML reader,
 * present in the Windows SDK, in llvm-mingw, and as a system DLL, so it needs
 * no vendoring). Everything above this header is backend-agnostic, so a
 * portable parser can be dropped in later without touching the CLIXML code.
 * See TODO PSRP-05.
 *
 * Names and values are returned as UTF-8 and remain valid until the next call
 * to psrp_xml_read on the same reader.
 */
#ifndef PSRP_XML_H
#define PSRP_XML_H

#include "psrp/psrp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psrp_xml_reader psrp_xml_reader_t;

typedef enum psrp_xml_node {
    PSRP_XML_EOF = 0,
    PSRP_XML_ELEMENT,       /* <Foo ...>  or <Foo ... />  */
    PSRP_XML_END_ELEMENT,   /* </Foo>                     */
    PSRP_XML_TEXT           /* character data             */
} psrp_xml_node_t;

/* Parses `utf8`/`n` held by the caller; the reader does not copy the input,
 * so it must outlive the reader. */
psrp_result_t psrp_xml_reader_create(const void *utf8, size_t n,
                                     psrp_xml_reader_t **out);
void psrp_xml_reader_free(psrp_xml_reader_t *r);

/* Advances to the next significant node. Whitespace-only text between
 * elements is skipped, so pretty-printed and compact CLIXML parse alike.
 * Returns PSRP_ERR_XML if the document is not well formed. */
psrp_result_t psrp_xml_read(psrp_xml_reader_t *r, psrp_xml_node_t *type);

/* Local name of the current element (element / end-element nodes). */
const char *psrp_xml_local_name(const psrp_xml_reader_t *r);

/* Text of the current TEXT node. `len` may be NULL. */
const char *psrp_xml_value(const psrp_xml_reader_t *r, size_t *len);

/* Attribute of the current element by local name, or NULL. */
const char *psrp_xml_attr(const psrp_xml_reader_t *r, const char *name);

/* True when the current element was written as <Foo />, which produces no
 * matching END_ELEMENT node. */
bool psrp_xml_is_empty_element(const psrp_xml_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_XML_H */
