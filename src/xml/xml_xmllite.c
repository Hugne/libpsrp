/* XmlLite backend for psrp_xml.h.
 *
 * IXmlReader needs an input stream, so this file also provides a tiny
 * read-only ISequentialStream over a caller-supplied memory block. XmlLite
 * accepts either IStream or ISequentialStream, and ISequentialStream is the
 * smaller contract.
 *
 * XmlLite speaks UTF-16; PSRP payloads are UTF-8. Names and values are
 * converted on demand into buffers owned by the reader.
 */

#define COBJMACROS
#include <windows.h>
#include <objidl.h>
#include <xmllite.h>

#include <stdlib.h>
#include <string.h>

#include "internal/psrp_xml.h"
#include "internal/psrp_codec.h"

/* Declared here rather than relying on a lib exporting the symbols, which
 * differs between the Windows SDK and mingw. */
static const IID kIID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const IID kIID_ISequentialStream =
    { 0x0c733a30, 0x2a1c, 0x11ce, { 0xad,0xe5,0x00,0xaa,0x00,0x44,0x77,0x3d } };
static const IID kIID_IXmlReader =
    { 0x7279fc81, 0x709d, 0x4095, { 0xb6,0x3d,0x69,0xfe,0x4b,0x0d,0x90,0x30 } };

/* ------------------------------------------------- memory input stream --- */

typedef struct mem_stream {
    ISequentialStream iface;
    LONG ref;
    const uint8_t *data;
    size_t len;
    size_t pos;
} mem_stream_t;

static mem_stream_t *from_iface(ISequentialStream *s)
{
    return (mem_stream_t *)(void *)s;
}

static HRESULT STDMETHODCALLTYPE ms_QueryInterface(ISequentialStream *This,
                                                   REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &kIID_IUnknown) ||
        IsEqualIID(riid, &kIID_ISequentialStream)) {
        *ppv = This;
        ISequentialStream_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ms_AddRef(ISequentialStream *This)
{
    return (ULONG)InterlockedIncrement(&from_iface(This)->ref);
}

static ULONG STDMETHODCALLTYPE ms_Release(ISequentialStream *This)
{
    mem_stream_t *s = from_iface(This);
    LONG r = InterlockedDecrement(&s->ref);
    if (r == 0) free(s);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE ms_Read(ISequentialStream *This, void *pv,
                                         ULONG cb, ULONG *read)
{
    mem_stream_t *s = from_iface(This);
    size_t avail = s->len - s->pos;
    size_t n = (avail < (size_t)cb) ? avail : (size_t)cb;
    if (!pv) return E_POINTER;
    if (n) memcpy(pv, s->data + s->pos, n);
    s->pos += n;
    if (read) *read = (ULONG)n;
    return (n == (size_t)cb) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ms_Write(ISequentialStream *This,
                                          const void *pv, ULONG cb,
                                          ULONG *written)
{
    (void)This; (void)pv; (void)cb; (void)written;
    return STG_E_ACCESSDENIED;      /* input only */
}

static const ISequentialStreamVtbl kMemStreamVtbl = {
    ms_QueryInterface, ms_AddRef, ms_Release, ms_Read, ms_Write
};

static ISequentialStream *mem_stream_create(const void *data, size_t len)
{
    mem_stream_t *s = (mem_stream_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->iface.lpVtbl = (ISequentialStreamVtbl *)&kMemStreamVtbl;
    s->ref = 1;
    s->data = (const uint8_t *)data;
    s->len = len;
    s->pos = 0;
    return &s->iface;
}

/* --------------------------------------------------------- xml reader ---- */

typedef struct attr {
    char *name;
    char *value;
} attr_t;

struct psrp_xml_reader {
    IXmlReader *reader;
    ISequentialStream *stream;

    char *name;            /* current local name, UTF-8 */
    char *value;           /* current text value, UTF-8 */
    size_t value_len;
    attr_t *attrs;
    size_t attr_count;
    bool empty_element;
};

/* Converts a counted UTF-16 run to a freshly allocated UTF-8 C string. */
static char *w2u(const WCHAR *w, UINT len, size_t *out_len)
{
    psrp_buffer_t b;
    char *result = NULL;
    psrp_buffer_init(&b);
    if (psrp_utf16_to_utf8(w, (size_t)len * sizeof(WCHAR), &b) != PSRP_OK) {
        psrp_buffer_free(&b);
        return NULL;
    }
    if (psrp_buffer_append_u8(&b, 0) != PSRP_OK) {
        psrp_buffer_free(&b);
        return NULL;
    }
    if (out_len) *out_len = b.len - 1;
    result = (char *)psrp_buffer_detach(&b, NULL);
    return result;
}

static void clear_attrs(psrp_xml_reader_t *r)
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

static void clear_node(psrp_xml_reader_t *r)
{
    free(r->name); r->name = NULL;
    free(r->value); r->value = NULL;
    r->value_len = 0;
    r->empty_element = false;
    clear_attrs(r);
}

psrp_result_t psrp_xml_reader_create(const void *utf8, size_t n,
                                     psrp_xml_reader_t **out)
{
    psrp_xml_reader_t *r;
    HRESULT hr;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (n && !utf8) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    r = (psrp_xml_reader_t *)calloc(1, sizeof *r);
    if (!r) return PSRP_ERR_NOMEM;

    r->stream = mem_stream_create(utf8, n);
    if (!r->stream) { free(r); return PSRP_ERR_NOMEM; }

    hr = CreateXmlReader(&kIID_IXmlReader, (void **)&r->reader, NULL);
    if (FAILED(hr) || !r->reader) {
        ISequentialStream_Release(r->stream);
        free(r);
        return PSRP_ERR_XML;
    }

    /* PSRP payloads never carry a DTD; refusing them avoids the whole class
     * of entity-expansion problems. */
    IXmlReader_SetProperty(r->reader, XmlReaderProperty_DtdProcessing,
                           DtdProcessing_Prohibit);

    hr = IXmlReader_SetInput(r->reader, (IUnknown *)r->stream);
    if (FAILED(hr)) {
        IXmlReader_Release(r->reader);
        ISequentialStream_Release(r->stream);
        free(r);
        return PSRP_ERR_XML;
    }

    *out = r;
    return PSRP_OK;
}

void psrp_xml_reader_free(psrp_xml_reader_t *r)
{
    if (!r) return;
    clear_node(r);
    if (r->reader) IXmlReader_Release(r->reader);
    if (r->stream) ISequentialStream_Release(r->stream);
    free(r);
}

static psrp_result_t collect_attributes(psrp_xml_reader_t *r)
{
    HRESULT hr = IXmlReader_MoveToFirstAttribute(r->reader);
    while (hr == S_OK) {
        const WCHAR *wname = NULL, *wvalue = NULL;
        UINT nlen = 0, vlen = 0;
        attr_t *grown;

        if (FAILED(IXmlReader_GetLocalName(r->reader, &wname, &nlen)) ||
            FAILED(IXmlReader_GetValue(r->reader, &wvalue, &vlen)))
            return PSRP_ERR_XML;

        grown = (attr_t *)realloc(r->attrs, (r->attr_count + 1) * sizeof *grown);
        if (!grown) return PSRP_ERR_NOMEM;
        r->attrs = grown;
        r->attrs[r->attr_count].name = w2u(wname, nlen, NULL);
        r->attrs[r->attr_count].value = w2u(wvalue, vlen, NULL);
        if (!r->attrs[r->attr_count].name || !r->attrs[r->attr_count].value) {
            free(r->attrs[r->attr_count].name);
            free(r->attrs[r->attr_count].value);
            return PSRP_ERR_NOMEM;
        }
        r->attr_count++;
        hr = IXmlReader_MoveToNextAttribute(r->reader);
    }
    IXmlReader_MoveToElement(r->reader);
    return PSRP_OK;
}

static bool is_all_whitespace(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            return false;
    return true;
}

psrp_result_t psrp_xml_read(psrp_xml_reader_t *r, psrp_xml_node_t *type)
{
    if (!r || !type) return PSRP_ERR_INVALID_ARG;

    for (;;) {
        XmlNodeType nt = XmlNodeType_None;
        HRESULT hr;

        clear_node(r);
        hr = IXmlReader_Read(r->reader, &nt);
        if (hr == S_FALSE) { *type = PSRP_XML_EOF; return PSRP_OK; }
        if (FAILED(hr)) return PSRP_ERR_XML;

        switch (nt) {
        case XmlNodeType_Element: {
            const WCHAR *w = NULL;
            UINT len = 0;
            psrp_result_t rc;
            if (FAILED(IXmlReader_GetLocalName(r->reader, &w, &len)))
                return PSRP_ERR_XML;
            r->name = w2u(w, len, NULL);
            if (!r->name) return PSRP_ERR_NOMEM;
            r->empty_element = IXmlReader_IsEmptyElement(r->reader) ? true : false;
            rc = collect_attributes(r);
            if (rc != PSRP_OK) return rc;
            *type = PSRP_XML_ELEMENT;
            return PSRP_OK;
        }
        case XmlNodeType_EndElement: {
            const WCHAR *w = NULL;
            UINT len = 0;
            if (FAILED(IXmlReader_GetLocalName(r->reader, &w, &len)))
                return PSRP_ERR_XML;
            r->name = w2u(w, len, NULL);
            if (!r->name) return PSRP_ERR_NOMEM;
            *type = PSRP_XML_END_ELEMENT;
            return PSRP_OK;
        }
        case XmlNodeType_Text:
        case XmlNodeType_CDATA:
        case XmlNodeType_Whitespace: {
            const WCHAR *w = NULL;
            UINT len = 0;
            if (FAILED(IXmlReader_GetValue(r->reader, &w, &len)))
                return PSRP_ERR_XML;
            r->value = w2u(w, len, &r->value_len);
            if (!r->value) return PSRP_ERR_NOMEM;
            /* Pretty-printed CLIXML puts indentation between elements; only
             * real character data is interesting. */
            if (is_all_whitespace(r->value, r->value_len)) continue;
            *type = PSRP_XML_TEXT;
            return PSRP_OK;
        }
        default:
            continue;   /* XML declaration, comments, PIs: not interesting */
        }
    }
}

const char *psrp_xml_local_name(const psrp_xml_reader_t *r)
{
    return r ? r->name : NULL;
}

const char *psrp_xml_value(const psrp_xml_reader_t *r, size_t *len)
{
    if (!r) { if (len) *len = 0; return NULL; }
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
    return r ? r->empty_element : false;
}
