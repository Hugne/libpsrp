/* Enumerating shells on a WS-Management server ([MS-WSMV]).
 *
 * WS-Enumerate over the shell resource, which is how a client finds shells it
 * did not create. The flat WSMan C API the rest of this client uses covers
 * shells and commands but not enumeration, so this goes through the WSMan COM
 * automation interface -- the same one `winrm enumerate` uses.
 *
 * What a caller does with the shells it finds is its own business. PowerShell
 * puts a RunspacePool id in the ShellId, but that is PSRP's convention and
 * nothing here relies on it: an identifier is an opaque string.
 */


#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <stdlib.h>
#include <string.h>

#include "psrp/winrm.h"
#include "internal/psrp_xml.h"

/* From wsmandisp.idl. Declared here rather than including wsmandisp.h so this
 * file keeps the same "declare the subset we use" approach the rest of the
 * transport takes, and so the build needs no extra generated GUID object. */
static const CLSID kCLSID_WSMan = {
    0xBCED617B, 0xEC03, 0x420B,
    { 0x85, 0x08, 0x97, 0x7D, 0xC7, 0xA6, 0x86, 0xBD } };
static const IID kIID_IWSManEx = {
    0x2D53BDAA, 0x798E, 0x49E6,
    { 0xA1, 0xAA, 0x74, 0xD0, 0x12, 0x56, 0xF4, 0x11 } };
static const IID kIID_IWSManSession = {
    0xFC84FC58, 0x1286, 0x40C4,
    { 0x9D, 0xA0, 0xC8, 0xEF, 0x6E, 0xC2, 0x41, 0xE0 } };
static const IID kIID_IWSManEnumerator = {
    0xF3457CA9, 0xABB9, 0x4FA5,
    { 0xB8, 0x50, 0x90, 0xE8, 0xCA, 0x30, 0x0E, 0x7F } };
static const IID kIID_IWSManConnectionOptions = {
    0xF704E861, 0x9E52, 0x464F,
    { 0xB7, 0x86, 0xDA, 0x5E, 0xB2, 0x32, 0x0F, 0xDD } };

/* The shell resource URI. Note the capitalisation: the spec's Connect rules
 * spell PowerShell lowercase in creationXml but the resource URI carries it
 * capitalised, and the server matches the URI exactly. */
#define SHELL_URI L"http://schemas.microsoft.com/wbem/wsman/1/windows/shell"

/* WSManSessionFlags. Only the one this needs. */
#define WSMAN_FLAG_CRED_USERNAME_PASSWORD 0x1000

/* ---------------------------------------------------- interface subsets -- */
/*
 * Only the methods used here are named; the rest of each vtable is padded so
 * the slots line up. Getting a slot count wrong would call the wrong function
 * with the wrong arguments, so each pad is counted against the IDL.
 */

typedef struct IWSManExVtbl IWSManExVtbl;
typedef struct { IWSManExVtbl *lpVtbl; } IWSManEx;

typedef struct IWSManSessionVtbl IWSManSessionVtbl;
typedef struct { IWSManSessionVtbl *lpVtbl; } IWSManSession;

typedef struct IWSManEnumeratorVtbl IWSManEnumeratorVtbl;
typedef struct { IWSManEnumeratorVtbl *lpVtbl; } IWSManEnumerator;

typedef struct IWSManConnOptVtbl IWSManConnOptVtbl;
typedef struct { IWSManConnOptVtbl *lpVtbl; } IWSManConnOpt;

/* IDispatch is 7 slots: 3 IUnknown + GetTypeInfoCount, GetTypeInfo,
 * GetIDsOfNames, Invoke. */
#define IDISPATCH_SLOTS                                                       \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);     \
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);                              \
    ULONG   (STDMETHODCALLTYPE *Release)(void *);                             \
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(void *, UINT *);            \
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(void *, UINT, LCID, void **);    \
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(void *, REFIID, LPOLESTR *,    \
                                               UINT, LCID, DISPID *);         \
    HRESULT (STDMETHODCALLTYPE *Invoke)(void *, DISPID, REFIID, LCID, WORD,   \
                                        DISPPARAMS *, VARIANT *, EXCEPINFO *,\
                                        UINT *)

struct IWSManExVtbl {
    IDISPATCH_SLOTS;
    /* IWSMan */
    HRESULT (STDMETHODCALLTYPE *CreateSession)(void *, BSTR, long, void *,
                                               void **);
    HRESULT (STDMETHODCALLTYPE *CreateConnectionOptions)(void *, void **);
    /* the remaining IWSMan and IWSManEx members are unused */
};

struct IWSManConnOptVtbl {
    IDISPATCH_SLOTS;
    HRESULT (STDMETHODCALLTYPE *get_UserName)(void *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *put_UserName)(void *, BSTR);
    HRESULT (STDMETHODCALLTYPE *put_Password)(void *, BSTR);
};

struct IWSManSessionVtbl {
    IDISPATCH_SLOTS;
    HRESULT (STDMETHODCALLTYPE *Get)(void *, VARIANT, long, BSTR *);
    HRESULT (STDMETHODCALLTYPE *Put)(void *, VARIANT, BSTR, long, BSTR *);
    HRESULT (STDMETHODCALLTYPE *Create)(void *, VARIANT, BSTR, long, BSTR *);
    HRESULT (STDMETHODCALLTYPE *Delete)(void *, VARIANT, long);
    HRESULT (STDMETHODCALLTYPE *InvokeMethod)(void *, BSTR, VARIANT, BSTR,
                                              long, BSTR *);
    HRESULT (STDMETHODCALLTYPE *Enumerate)(void *, VARIANT, BSTR, BSTR, long,
                                           void **);
};

struct IWSManEnumeratorVtbl {
    IDISPATCH_SLOTS;
    HRESULT (STDMETHODCALLTYPE *ReadItem)(void *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *get_AtEndOfStream)(void *, VARIANT_BOOL *);
    HRESULT (STDMETHODCALLTYPE *get_Error)(void *, BSTR *);
};

/* ------------------------------------------------------------ parsing --- */

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ShellId text to a GUID. psrp_guid_parse wants exactly the bare 36-character
 * form, but a server is free to wrap the value in braces or pad it with
 * whitespace, and rejecting those would drop a perfectly connectable pool. */
static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* A WS-Management ShellId is an opaque string. The server may return it
 * braced or padded, so it is normalised here -- but not parsed: that it
 * happens to hold a GUID is PowerShell's convention, and this layer has no
 * business assuming it. */
static bool parse_shell_id(const char *v, size_t len, char **out)
{
    char *copy;

    while (len && is_space(v[0])) { v++; len--; }
    while (len && is_space(v[len - 1])) len--;
    if (len >= 2 && v[0] == '{' && v[len - 1] == '}') { v++; len -= 2; }
    if (!len) return false;

    copy = (char *)malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, v, len);
    copy[len] = '\0';

    free(*out);
    *out = copy;
    return true;
}

void winrm_shell_info_free(winrm_shell_info_t *s)
{
    if (!s) return;
    free(s->shell_id);
    free(s->name);
    free(s->owner);
    free(s->state);
    free(s->resource_uri);
    memset(s, 0, sizeof *s);
}

void winrm_shell_info_free_all(winrm_shell_info_t *list, size_t count)
{
    size_t i;
    if (!list) return;
    for (i = 0; i < count; i++) winrm_shell_info_free(&list[i]);
    free(list);
}

psrp_result_t winrm_parse_shell(const void *xml, size_t n,
                                     winrm_shell_info_t *out)
{
    psrp_xml_reader_t *r = NULL;
    psrp_result_t rc;
    char *pending = NULL;     /* local name of the element we are inside */
    bool have_id = false;

    if (!xml || !out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    rc = psrp_xml_reader_create(xml, n, &r);
    if (rc != PSRP_OK) return rc;

    for (;;) {
        psrp_xml_node_t node;
        rc = psrp_xml_read(r, &node);
        if (rc != PSRP_OK) break;
        if (node == PSRP_XML_EOF) break;

        if (node == PSRP_XML_ELEMENT) {
            /* Local names, so the rsp: prefix does not matter; a server is
             * free to bind the shell namespace to any prefix it likes. */
            const char *name = psrp_xml_local_name(r);
            free(pending);
            pending = name ? dup_n(name, strlen(name)) : NULL;
        } else if (node == PSRP_XML_TEXT && pending) {
            size_t len = 0;
            const char *v = psrp_xml_value(r, &len);
            if (!v) continue;

            if (strcmp(pending, "ShellId") == 0) {
                /* A shell with no identifier cannot be addressed, which makes an
                 * entry useful; anything else is decoration. */
                if (parse_shell_id(v, len, &out->shell_id)) have_id = true;
            } else if (strcmp(pending, "Name") == 0 && !out->name) {
                out->name = dup_n(v, len);
            } else if (strcmp(pending, "Owner") == 0 && !out->owner) {
                out->owner = dup_n(v, len);
            } else if (strcmp(pending, "State") == 0 && !out->state) {
                out->state = dup_n(v, len);
            } else if (strcmp(pending, "ResourceUri") == 0 &&
                       !out->resource_uri) {
                out->resource_uri = dup_n(v, len);
            }
        } else if (node == PSRP_XML_END_ELEMENT) {
            free(pending);
            pending = NULL;
        }
    }

    free(pending);
    psrp_xml_reader_free(r);

    if (rc != PSRP_OK && rc != PSRP_ERR_XML) {
        winrm_shell_info_free(out);
        return rc;
    }
    if (!have_id) {
        /* A shell element with no ShellId cannot be connected to, so reporting
         * it would only hand the caller an entry it cannot use. */
        winrm_shell_info_free(out);
        return PSRP_ERR_MALFORMED;
    }
    return PSRP_OK;
}

/* ---------------------------------------------------------- enumerate --- */

/* For the fixed wide constants in this file, which are already UTF-16. */
static BSTR bstr_of_wide(const wchar_t *s)
{
    return s ? SysAllocString(s) : NULL;
}

/* UTF-8 to BSTR. The COM automation interface wants UTF-16, and the config is
 * UTF-8 like the rest of the library, so the conversion happens here rather
 * than being pushed onto the caller. */
static BSTR bstr_of(const char *utf8)
{
    int n;
    BSTR b;

    if (!utf8) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;

    /* SysAllocStringLen wants a length excluding the terminator. */
    b = SysAllocStringLen(NULL, (UINT)(n - 1));
    if (!b) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, b, n) <= 0) {
        SysFreeString(b);
        return NULL;
    }
    return b;
}

/*
 * A discovery session holds the COM objects open across calls.
 *
 * Measured on Windows 11: a WSMan session that enumerates once and is then
 * released leaves about one process handle behind for roughly a minute. The
 * handle is a WinHTTP connection Event, not anything of WSMan's or ours -- the
 * same pattern reproduces with plain WinHTTP and no WSMan at all -- and
 * WinHTTP's scavenger reclaims it, so nothing is leaked. A session that is
 * created and never used costs nothing, and a reused one costs nothing after
 * the first few.
 *
 * So this is an efficiency measure, not a workaround: listing in a loop through
 * one-shot calls holds handles for no reason. See TODO PSRP-14.
 */
struct winrm_enumerator {
    bool com_started;
    IWSManEx *wsman;
    IWSManConnOpt *opts;
    void *session_disp;
    IWSManSession *session;
};

void winrm_enumerator_free(winrm_enumerator_t *d)
{
    if (!d) return;
    if (d->session) d->session->lpVtbl->Release(d->session);
    if (d->session_disp)
        ((IUnknown *)d->session_disp)->lpVtbl->Release(
            (IUnknown *)d->session_disp);
    if (d->opts) d->opts->lpVtbl->Release(d->opts);
    if (d->wsman) d->wsman->lpVtbl->Release(d->wsman);
    if (d->com_started) CoUninitialize();
    free(d);
}

psrp_result_t winrm_enumerator_open(const winrm_config_t *cfg,
                                        winrm_enumerator_t **out)
{
    winrm_enumerator_t *d;
    HRESULT hr;
    BSTR connection = NULL;

    if (!cfg || !out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    d = (winrm_enumerator_t *)calloc(1, sizeof *d);
    if (!d) return PSRP_ERR_NOMEM;

    /* Apartment-threaded, and tolerate an apartment the caller already set up:
     * RPC_E_CHANGED_MODE means COM is live in another mode, which is fine for
     * an in-proc call like this one. */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) d->com_started = true;
    else if (hr != RPC_E_CHANGED_MODE) { free(d); return PSRP_ERR_TRANSPORT; }

    hr = CoCreateInstance(&kCLSID_WSMan, NULL, CLSCTX_INPROC_SERVER,
                          &kIID_IWSManEx, (void **)&d->wsman);
    if (FAILED(hr)) goto fail;

    if (cfg->username && cfg->password) {
        void *opts_disp = NULL;
        BSTR user = bstr_of(cfg->username);
        BSTR pass = bstr_of(cfg->password);

        hr = d->wsman->lpVtbl->CreateConnectionOptions(d->wsman, &opts_disp);
        if (SUCCEEDED(hr) && opts_disp) {
            /* CreateConnectionOptions hands back an IDispatch. For a
             * single-inheritance COM object that pointer happens to equal the
             * IWSManConnectionOptions one, but relying on that is relying on
             * layout, so ask properly. */
            hr = ((IUnknown *)opts_disp)->lpVtbl->QueryInterface(
                (IUnknown *)opts_disp, &kIID_IWSManConnectionOptions,
                (void **)&d->opts);
            ((IUnknown *)opts_disp)->lpVtbl->Release((IUnknown *)opts_disp);
        }
        if (SUCCEEDED(hr) && d->opts) {
            if (user) (void)d->opts->lpVtbl->put_UserName(d->opts, user);
            if (pass) (void)d->opts->lpVtbl->put_Password(d->opts, pass);
        }
        /* put_* copies, so these are ours to release either way. */
        if (user) SysFreeString(user);
        if (pass) SysFreeString(pass);
        if (FAILED(hr)) goto fail;
    }

    connection = bstr_of(cfg->connection);   /* NULL means the local machine */
    /* Supplying credentials without this flag is refused outright: "Requests
     * with credentials must include the following flag:
     * WSManFlagCredUsernamePassword". The flat WSMan C API infers it from the
     * authentication struct; the automation API makes you say it. */
    hr = d->wsman->lpVtbl->CreateSession(
        d->wsman, connection,
        d->opts ? WSMAN_FLAG_CRED_USERNAME_PASSWORD : 0,
        d->opts, &d->session_disp);
    if (connection) SysFreeString(connection);
    if (FAILED(hr) || !d->session_disp) goto fail;

    hr = ((IUnknown *)d->session_disp)->lpVtbl->QueryInterface(
        (IUnknown *)d->session_disp, &kIID_IWSManSession, (void **)&d->session);
    if (FAILED(hr)) goto fail;

    *out = d;
    return PSRP_OK;

fail:
    winrm_enumerator_free(d);
    return PSRP_ERR_TRANSPORT;
}

psrp_result_t winrm_enumerator_shells(winrm_enumerator_t *d,
                                          winrm_shell_info_t **out,
                                          size_t *count)
{
    HRESULT hr;
    BSTR uri = NULL;
    VARIANT uri_var;
    void *result_disp = NULL;
    IWSManEnumerator *en = NULL;
    winrm_shell_info_t *list = NULL;
    size_t used = 0, cap = 0;
    psrp_result_t rc = PSRP_ERR_TRANSPORT;

    if (!d || !out || !count) return PSRP_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;
    VariantInit(&uri_var);

    uri = bstr_of_wide(SHELL_URI);
    if (!uri) return PSRP_ERR_NOMEM;
    /* The VARIANT only borrows the BSTR: it is an [in] parameter, so the
     * callee does not take ownership and this must not be VariantClear'd or
     * the string would be freed twice. */
    uri_var.vt = VT_BSTR;
    uri_var.bstrVal = uri;

    hr = d->session->lpVtbl->Enumerate(d->session, uri_var, NULL, NULL, 0,
                                       &result_disp);
    if (FAILED(hr) || !result_disp) goto done;

    hr = ((IUnknown *)result_disp)->lpVtbl->QueryInterface(
        (IUnknown *)result_disp, &kIID_IWSManEnumerator, (void **)&en);
    if (FAILED(hr)) goto done;

    for (;;) {
        VARIANT_BOOL eos = VARIANT_FALSE;
        BSTR item = NULL;
        int utf8_len;
        char *utf8;
        winrm_shell_info_t info;

        hr = en->lpVtbl->get_AtEndOfStream(en, &eos);
        if (FAILED(hr) || eos != VARIANT_FALSE) break;

        hr = en->lpVtbl->ReadItem(en, &item);
        if (FAILED(hr) || !item) break;

        utf8_len = WideCharToMultiByte(CP_UTF8, 0, item, -1, NULL, 0, NULL,
                                       NULL);
        utf8 = utf8_len > 0 ? (char *)malloc((size_t)utf8_len) : NULL;
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, item, -1, utf8, utf8_len, NULL,
                                NULL);
            if (winrm_parse_shell(utf8, strlen(utf8), &info) == PSRP_OK) {
                if (used == cap) {
                    size_t next = cap ? cap * 2 : 8;
                    winrm_shell_info_t *grown = (winrm_shell_info_t *)
                        realloc(list, next * sizeof *grown);
                    if (!grown) {
                        winrm_shell_info_free(&info);
                        free(utf8);
                        SysFreeString(item);
                        rc = PSRP_ERR_NOMEM;
                        goto done;
                    }
                    list = grown;
                    cap = next;
                }
                list[used++] = info;
            }
            free(utf8);
        }
        SysFreeString(item);
    }

    *out = list;
    *count = used;
    list = NULL;
    rc = PSRP_OK;

done:
    winrm_shell_info_free_all(list, used);
    if (en) en->lpVtbl->Release(en);
    if (result_disp)
        ((IUnknown *)result_disp)->lpVtbl->Release((IUnknown *)result_disp);
    SysFreeString(uri);
    return rc;
}

psrp_result_t winrm_enumerate_shells(const winrm_config_t *cfg,
                                          winrm_shell_info_t **out,
                                          size_t *count)
{
    winrm_enumerator_t *d = NULL;
    psrp_result_t rc;

    if (!cfg || !out || !count) return PSRP_ERR_INVALID_ARG;
    rc = winrm_enumerator_open(cfg, &d);
    if (rc != PSRP_OK) return rc;
    rc = winrm_enumerator_shells(d, out, count);
    winrm_enumerator_free(d);
    return rc;
}
