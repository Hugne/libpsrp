/* Discovering RunspacePools on a server ([MS-PSRP] 3.1.4.10.1).
 *
 * Before a client can connect to someone else's RunspacePool it needs that
 * pool's identifier, and 3.1.4.10.1 gets it with a wxf:Enumerate over the
 * shell resource URI. Each RunspacePool is a WSMan shell, and its ShellId is
 * the pool id.
 *
 * The flat WSMan C API used by the rest of the transport has no enumerate
 * entry point: it covers shells and commands, not WS-Enumerate. The WSMan
 * *automation* API does, through IWSManSession::Enumerate, so that is what
 * this uses. It is the same interface `winrm enumerate` is built on.
 *
 * COM from C is ugly but mechanical: COBJMACROS turns each method into a
 * IFace_Method(this, ...) call through the vtable. The awkwardness is confined
 * to this file; nothing else in the library knows COM exists.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_transport.h"
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

static bool parse_shell_id(const char *v, size_t len, psrp_guid_t *out)
{
    char buf[PSRP_GUID_STR_LEN + 1];

    while (len && is_space(v[0])) { v++; len--; }
    while (len && is_space(v[len - 1])) len--;
    if (len >= 2 && v[0] == '{' && v[len - 1] == '}') { v++; len -= 2; }

    if (len != PSRP_GUID_STR_LEN) return false;
    memcpy(buf, v, len);
    buf[len] = '\0';
    return psrp_guid_parse(buf, out) == PSRP_OK;
}

void psrp_shell_info_free(psrp_shell_info_t *s)
{
    if (!s) return;
    free(s->name);
    free(s->owner);
    free(s->state);
    free(s->resource_uri);
    memset(s, 0, sizeof *s);
}

void psrp_shell_info_free_all(psrp_shell_info_t *list, size_t count)
{
    size_t i;
    if (!list) return;
    for (i = 0; i < count; i++) psrp_shell_info_free(&list[i]);
    free(list);
}

psrp_result_t psrp_wsman_parse_shell(const void *xml, size_t n,
                                     psrp_shell_info_t *out)
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
                /* The ShellId is the RunspacePool id, and it is what makes an
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
        psrp_shell_info_free(out);
        return rc;
    }
    if (!have_id) {
        /* A shell element with no ShellId cannot be connected to, so reporting
         * it would only hand the caller an entry it cannot use. */
        psrp_shell_info_free(out);
        return PSRP_ERR_MALFORMED;
    }
    return PSRP_OK;
}

/* ---------------------------------------------------------- enumerate --- */

static BSTR bstr_of(const wchar_t *s)
{
    return s ? SysAllocString(s) : NULL;
}

/*
 * A discovery session holds the COM objects open across calls.
 *
 * That is not a convenience. Measured on Windows 11: creating a WSMan session,
 * enumerating once and releasing everything leaks about one process handle per
 * call, and CoUninitialize does not reclaim it. Reusing one session for 200
 * enumerations leaks nothing, and the handles come back when the session is
 * finally released. A session that is merely created and never used does not
 * leak either, so it is specifically a used-then-discarded session that costs.
 *
 * The leak is in the WSMan automation layer, not here: it reproduces with a
 * standalone program that touches none of this library's code.
 */
struct psrp_discovery {
    bool com_started;
    IWSManEx *wsman;
    IWSManConnOpt *opts;
    void *session_disp;
    IWSManSession *session;
};

void psrp_wsman_discovery_free(psrp_discovery_t *d)
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

psrp_result_t psrp_wsman_discovery_open(const psrp_wsman_config_t *cfg,
                                        psrp_discovery_t **out)
{
    psrp_discovery_t *d;
    HRESULT hr;
    BSTR connection = NULL;

    if (!cfg || !out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    d = (psrp_discovery_t *)calloc(1, sizeof *d);
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
    psrp_wsman_discovery_free(d);
    return PSRP_ERR_TRANSPORT;
}

psrp_result_t psrp_wsman_discovery_shells(psrp_discovery_t *d,
                                          psrp_shell_info_t **out,
                                          size_t *count)
{
    HRESULT hr;
    BSTR uri = NULL;
    VARIANT uri_var;
    void *result_disp = NULL;
    IWSManEnumerator *en = NULL;
    psrp_shell_info_t *list = NULL;
    size_t used = 0, cap = 0;
    psrp_result_t rc = PSRP_ERR_TRANSPORT;

    if (!d || !out || !count) return PSRP_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;
    VariantInit(&uri_var);

    uri = bstr_of(SHELL_URI);
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
        psrp_shell_info_t info;

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
            if (psrp_wsman_parse_shell(utf8, strlen(utf8), &info) == PSRP_OK) {
                if (used == cap) {
                    size_t next = cap ? cap * 2 : 8;
                    psrp_shell_info_t *grown = (psrp_shell_info_t *)
                        realloc(list, next * sizeof *grown);
                    if (!grown) {
                        psrp_shell_info_free(&info);
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
    psrp_shell_info_free_all(list, used);
    if (en) en->lpVtbl->Release(en);
    if (result_disp)
        ((IUnknown *)result_disp)->lpVtbl->Release((IUnknown *)result_disp);
    SysFreeString(uri);
    return rc;
}

psrp_result_t psrp_wsman_enumerate_shells(const psrp_wsman_config_t *cfg,
                                          psrp_shell_info_t **out,
                                          size_t *count)
{
    psrp_discovery_t *d = NULL;
    psrp_result_t rc;

    if (!cfg || !out || !count) return PSRP_ERR_INVALID_ARG;
    rc = psrp_wsman_discovery_open(cfg, &d);
    if (rc != PSRP_OK) return rc;
    rc = psrp_wsman_discovery_shells(d, out, count);
    psrp_wsman_discovery_free(d);
    return rc;
}
