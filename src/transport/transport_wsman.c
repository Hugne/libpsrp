/* WSMan transport for PSRP ([MS-PSRP] 3.1.5.3), on the Win32 WSMan API.
 *
 * The WSMan API is asynchronous: every operation completes on a worker thread
 * via a callback. Each one-shot operation is wrapped in an event-signalled
 * context; the continuous Receive accumulates into a buffer the caller drains.
 */

#include "internal/psrp_wsman_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_transport.h"
#include "psrp/psrp_fragment.h"
#include "internal/psrp_codec.h"

/* 3.1.5.3.1 and its appendix. Note the capitalisation: the resource URI
 * spells it PowerShell while the creationXml namespace spells it powershell.
 * They differ deliberately. */
static PCWSTR kResourceUri =
    L"http://schemas.microsoft.com/PowerShell/Microsoft.PowerShell";
static PCWSTR kDefaultConnection = L"http://localhost:5985/wsman";
#define PSRP_DEFAULT_TIMEOUT_MS 240000u

/* ------------------------------------------------------- async plumbing -- */

typedef struct async_op {
    HANDLE done;
    DWORD error;
    wchar_t detail[512];
} async_op_t;

static void op_init(async_op_t *op)
{
    memset(op, 0, sizeof *op);
    op->done = CreateEventW(NULL, TRUE, FALSE, NULL);
}

static void op_destroy(async_op_t *op)
{
    if (op->done) CloseHandle(op->done);
}

static void CALLBACK generic_completion(PVOID ctx, DWORD flags,
                                        WSMAN_ERROR *error,
                                        WSMAN_SHELL_HANDLE shell,
                                        WSMAN_COMMAND_HANDLE command,
                                        WSMAN_OPERATION_HANDLE op_handle,
                                        WSMAN_RESPONSE_DATA *data)
{
    async_op_t *op = (async_op_t *)ctx;
    (void)shell; (void)command; (void)op_handle; (void)data;
    if (error && error->code != 0) {
        op->error = error->code;
        if (error->errorDetail) {
            size_t cap = sizeof op->detail / sizeof op->detail[0];
            wcsncpy(op->detail, error->errorDetail, cap - 1);
            op->detail[cap - 1] = L'\0';
        }
    }
    if ((flags & WSMAN_FLAG_CALLBACK_END_OF_OPERATION) || error)
        SetEvent(op->done);
}

/* ------------------------------------------------------------ receive ---- */

typedef struct recv_ctx {
    CRITICAL_SECTION lock;
    HANDLE data_ready;
    psrp_buffer_t buf;      /* raw stdout bytes not yet drained */
    bool done;              /* command reported Done */
    bool op_ended;          /* a Receive operation finished; may need re-post */
    bool expect_abort;      /* we cancelled a Receive on purpose */
    DWORD error;
    wchar_t detail[512];
} recv_ctx_t;

static void CALLBACK receive_completion(PVOID ctx, DWORD flags,
                                        WSMAN_ERROR *error,
                                        WSMAN_SHELL_HANDLE shell,
                                        WSMAN_COMMAND_HANDLE command,
                                        WSMAN_OPERATION_HANDLE op_handle,
                                        WSMAN_RESPONSE_DATA *response)
{
    recv_ctx_t *r = (recv_ctx_t *)ctx;
    WSMAN_RECEIVE_DATA_RESULT *data = response ? &response->receiveData : NULL;
    (void)shell; (void)command; (void)op_handle;

    EnterCriticalSection(&r->lock);
    /* Retargeting Receive from the shell to a command means cancelling the
     * in-flight operation, and the cancellation reports itself as
     * ERROR_OPERATION_ABORTED. That is our own doing, not a server fault. */
    if (error && error->code == ERROR_OPERATION_ABORTED && r->expect_abort) {
        r->expect_abort = false;
        LeaveCriticalSection(&r->lock);
        SetEvent(r->data_ready);
        return;
    }
    if (error && error->code != 0) {
        size_t cap = sizeof r->detail / sizeof r->detail[0];
        r->error = error->code;
        if (error->errorDetail) {
            wcsncpy(r->detail, error->errorDetail, cap - 1);
            r->detail[cap - 1] = L'\0';
        }
        r->done = true;
    } else if (data) {
        /* 3.1.5.3.7: PSRP data arrives on the "stdout" stream. */
        if (data->streamData.type == WSMAN_DATA_TYPE_BINARY &&
            data->streamData.binaryData.dataLength > 0) {
            (void)psrp_buffer_append(&r->buf, data->streamData.binaryData.data,
                                     data->streamData.binaryData.dataLength);
        }
        if (data->commandState && wcsstr(data->commandState, L"Done") != NULL)
            r->done = true;
    }
    if (flags & WSMAN_FLAG_CALLBACK_END_OF_OPERATION) r->op_ended = true;
    LeaveCriticalSection(&r->lock);
    SetEvent(r->data_ready);
}

/* --------------------------------------------------------- transport ----- */

struct psrp_transport {
    WSMAN_API_HANDLE api;
    WSMAN_SESSION_HANDLE session;
    WSMAN_SHELL_HANDLE shell;
    WSMAN_COMMAND_HANDLE command;
    WSMAN_OPERATION_HANDLE recv_op;
    WSMAN_SHELL_ASYNC recv_async;
    WSMAN_AUTHENTICATION_CREDENTIALS auth;
    recv_ctx_t recv;
    DWORD timeout_ms;
    char last_error[640];
};

static void set_error(psrp_transport_t *t, const char *what, DWORD code,
                      const wchar_t *detail)
{
    char narrow[512];
    narrow[0] = '\0';
    if (detail && *detail)
        WideCharToMultiByte(CP_UTF8, 0, detail, -1, narrow, (int)sizeof narrow,
                            NULL, NULL);
    snprintf(t->last_error, sizeof t->last_error, "%s failed: 0x%08lX %s",
             what, (unsigned long)code, narrow);
}

const char *psrp_transport_last_error(const psrp_transport_t *t)
{
    if (!t || t->last_error[0] == '\0') return "no error";
    return t->last_error;
}

bool psrp_transport_command_done(const psrp_transport_t *t)
{
    bool done;
    psrp_transport_t *mut = (psrp_transport_t *)t;
    if (!t) return true;
    EnterCriticalSection(&mut->recv.lock);
    done = mut->recv.done;
    LeaveCriticalSection(&mut->recv.lock);
    return done;
}

psrp_result_t psrp_wsman_transport_create(const psrp_wsman_config_t *cfg,
                                          psrp_transport_t **out)
{
    psrp_transport_t *t;
    PCWSTR connection;
    DWORD rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    t = (psrp_transport_t *)calloc(1, sizeof *t);
    if (!t) return PSRP_ERR_NOMEM;

    InitializeCriticalSection(&t->recv.lock);
    t->recv.data_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    psrp_buffer_init(&t->recv.buf);
    t->timeout_ms = (cfg && cfg->operation_timeout_ms)
                        ? cfg->operation_timeout_ms : PSRP_DEFAULT_TIMEOUT_MS;

    rc = WSManInitialize(WSMAN_FLAG_REQUESTED_API_VERSION_1_1, &t->api);
    if (rc != 0) {
        set_error(t, "WSManInitialize", rc, NULL);
        psrp_transport_free(t);
        return PSRP_ERR_TRANSPORT;
    }

    t->auth.authenticationMechanism = WSMAN_FLAG_AUTH_NEGOTIATE;
    if (cfg && cfg->username && *cfg->username) {
        t->auth.userAccount.username = cfg->username;
        t->auth.userAccount.password = cfg->password ? cfg->password : L"";
    }

    connection = (cfg && cfg->connection) ? cfg->connection : kDefaultConnection;
    rc = WSManCreateSession(t->api, connection, 0, &t->auth, NULL, &t->session);
    if (rc != 0) {
        set_error(t, "WSManCreateSession", rc, NULL);
        psrp_transport_free(t);
        return PSRP_ERR_TRANSPORT;
    }

    *out = t;
    return PSRP_OK;
}

void psrp_transport_free(psrp_transport_t *t)
{
    if (!t) return;
    if (t->recv_op) { WSManCloseOperation(t->recv_op, 0); t->recv_op = NULL; }
    if (t->command) {
        async_op_t op;
        WSMAN_SHELL_ASYNC a;
        op_init(&op);
        a.operationContext = &op;
        a.completionFunction = generic_completion;
        WSManCloseCommand(t->command, 0, &a);
        WaitForSingleObject(op.done, 5000);
        op_destroy(&op);
        t->command = NULL;
    }
    if (t->shell) {
        async_op_t op;
        WSMAN_SHELL_ASYNC a;
        op_init(&op);
        a.operationContext = &op;
        a.completionFunction = generic_completion;
        WSManCloseShell(t->shell, 0, &a);
        WaitForSingleObject(op.done, 5000);
        op_destroy(&op);
        t->shell = NULL;
    }
    if (t->session) { WSManCloseSession(t->session, 0); t->session = NULL; }
    if (t->api) { WSManDeinitialize(t->api, 0); t->api = NULL; }

    if (t->recv.data_ready) CloseHandle(t->recv.data_ready);
    psrp_buffer_free(&t->recv.buf);
    DeleteCriticalSection(&t->recv.lock);
    free(t);
}

/* Formats a GUID as the wide hyphenated string WSMan wants for its ids. */
static psrp_result_t guid_to_wide(const psrp_guid_t *g, wchar_t out[64])
{
    char narrow[PSRP_GUID_BUF_SIZE];
    psrp_result_t rc = psrp_guid_format(g, narrow, sizeof narrow);
    if (rc != PSRP_OK) return rc;
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, out, 64);
    return PSRP_OK;
}

/* Builds <creationXml xmlns="...powershell">base64</creationXml> as UTF-16. */
static psrp_result_t build_creation_xml(const void *payload, size_t len,
                                        wchar_t **out)
{
    psrp_buffer_t b64, utf8, utf16;
    psrp_result_t rc;
    wchar_t *result = NULL;

    psrp_buffer_init(&b64);
    psrp_buffer_init(&utf8);
    psrp_buffer_init(&utf16);

    rc = psrp_base64_encode_buf(&b64, payload, len);
    if (rc == PSRP_OK)
        rc = psrp_buffer_append_str(&utf8,
            "<creationXml xmlns=\"http://schemas.microsoft.com/powershell\">");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&utf8, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, "</creationXml>");
    if (rc == PSRP_OK) rc = psrp_utf8_to_utf16(utf8.data, utf8.len, &utf16);
    /* UTF-16 NUL terminator. */
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&utf16, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&utf16, 0);
    if (rc == PSRP_OK) {
        result = (wchar_t *)psrp_buffer_detach(&utf16, NULL);
        if (!result) rc = PSRP_ERR_NOMEM;
    }

    psrp_buffer_free(&b64);
    psrp_buffer_free(&utf8);
    psrp_buffer_free(&utf16);
    *out = result;
    return rc;
}

/* Posts the continuous Receive that pumps stdout into recv.buf. */
static void post_receive(psrp_transport_t *t)
{
    static PCWSTR streams[1] = { L"stdout" };
    WSMAN_STREAM_ID_SET desired;
    desired.streamIDsCount = 1;
    desired.streamIDs = streams;

    t->recv_async.operationContext = &t->recv;
    t->recv_async.completionFunction = receive_completion;
    WSManReceiveShellOutput(t->shell, t->command, 0, &desired, &t->recv_async,
                            &t->recv_op);
}

psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *shell_id,
                                  const void *payload, size_t len)
{
    wchar_t shell_id_w[64];
    wchar_t *creation = NULL;
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    psrp_result_t rc;
    /* 3.1.5.3.1: stdin carries data, pr carries host responses. */
    static PCWSTR in_streams[2] = { L"stdin", L"pr" };
    static PCWSTR out_streams[1] = { L"stdout" };
    WSMAN_STREAM_ID_SET in_set, out_set;
    WSMAN_SHELL_STARTUP_INFO startup;
    WSMAN_DATA create_xml;
    WSMAN_OPTION protocol_option;
    WSMAN_OPTION_SET options;

    if (!t || !shell_id || (len && !payload)) return PSRP_ERR_INVALID_ARG;

    rc = guid_to_wide(shell_id, shell_id_w);
    if (rc != PSRP_OK) return rc;
    rc = build_creation_xml(payload, len, &creation);
    if (rc != PSRP_OK) return rc;

    in_set.streamIDsCount = 2;  in_set.streamIDs = in_streams;
    out_set.streamIDsCount = 1; out_set.streamIDs = out_streams;

    memset(&startup, 0, sizeof startup);
    startup.inputStreamSet = &in_set;
    startup.outputStreamSet = &out_set;

    memset(&create_xml, 0, sizeof create_xml);
    create_xml.type = WSMAN_DATA_TYPE_TEXT;
    create_xml.text.buffer = creation;
    create_xml.text.bufferLength = (DWORD)wcslen(creation);

    /* 3.1.5.3.1 requires this option; the server faults when it is missing or
     * its major version is not 2. The spec's Create table capitalises the name
     * while its Connect rules spell it lowercase; PowerShell sends lowercase. */
    memset(&protocol_option, 0, sizeof protocol_option);
    protocol_option.name = L"protocolversion";
    protocol_option.value = L"2.2";
    protocol_option.mustComply = TRUE;
    memset(&options, 0, sizeof options);
    options.optionsCount = 1;
    options.options = &protocol_option;
    options.optionsMustUnderstand = TRUE;

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    WSManCreateShellEx(t->session, 0, kResourceUri, shell_id_w, &startup,
                       &options, &create_xml, &async, &t->shell);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManCreateShellEx", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManCreateShellEx", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    op_destroy(&op);
    free(creation);

    if (rc == PSRP_OK) post_receive(t);   /* start pumping stdout */
    return rc;
}

/* Length of the first fragment in `payload`, or 0 if undecodable. 3.1.5.3.3
 * allows only that fragment in the Command's Arguments. */
static size_t first_fragment_len(const void *payload, size_t len)
{
    psrp_reader_t r;
    psrp_fragment_t f;
    psrp_reader_init(&r, payload, len);
    if (psrp_fragment_decode(&r, &f) != PSRP_OK) return 0;
    return r.pos;
}

psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *command_id,
                                         const void *payload, size_t len)
{
    wchar_t command_id_w[64];
    psrp_buffer_t b64, utf16;
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    PCWSTR args[1];
    WSMAN_COMMAND_ARG_SET arg_set;
    psrp_result_t rc;
    size_t first;

    if (!t || !command_id || !payload || len == 0) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;

    rc = guid_to_wide(command_id, command_id_w);
    if (rc != PSRP_OK) return rc;

    first = first_fragment_len(payload, len);
    if (first == 0) return PSRP_ERR_MALFORMED;

    psrp_buffer_init(&b64);
    psrp_buffer_init(&utf16);
    rc = psrp_base64_encode_buf(&b64, payload, first);
    if (rc == PSRP_OK) rc = psrp_utf8_to_utf16(b64.data, b64.len, &utf16);
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&utf16, 0);
    if (rc == PSRP_OK) rc = psrp_buffer_append_u8(&utf16, 0);
    if (rc != PSRP_OK) {
        psrp_buffer_free(&b64);
        psrp_buffer_free(&utf16);
        return rc;
    }

    /* Retarget Receive at the command so its output is delivered. */
    if (t->recv_op) {
        EnterCriticalSection(&t->recv.lock);
        t->recv.expect_abort = true;
        LeaveCriticalSection(&t->recv.lock);
        WSManCloseOperation(t->recv_op, 0);
        t->recv_op = NULL;
    }

    op_init(&op);
    args[0] = (PCWSTR)utf16.data;
    arg_set.argsCount = 1;
    arg_set.args = args;
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    /* 3.1.5.3.3 says the Command element MUST be empty, with the first
     * fragment in Arguments. The Win32 WSMan client will not let us do that
     * literally: an empty command line is rejected client-side with
     * 0x80338180 ("one of the parameters ... is null or zero"), by both
     * WSManRunShellCommand and its Ex form. A single space is the closest we
     * can get, and a real PowerShell endpoint accepts it -- verified by the
     * live interop test, which runs a pipeline and gets its output back. */
    WSManRunShellCommandEx(t->shell, 0, command_id_w, L" ", &arg_set, NULL,
                           &async, &t->command);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManRunShellCommandEx", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManRunShellCommandEx", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    op_destroy(&op);
    psrp_buffer_free(&b64);
    psrp_buffer_free(&utf16);

    if (rc == PSRP_OK) {
        /* The cancelled shell-level Receive may have left state behind. */
        EnterCriticalSection(&t->recv.lock);
        t->recv.done = false;
        t->recv.op_ended = false;
        t->recv.expect_abort = false;
        t->recv.error = 0;
        LeaveCriticalSection(&t->recv.lock);
        post_receive(t);
        /* Fragments beyond the first travel by Send, per 3.1.5.3.3. */
        if (len > first)
            rc = psrp_transport_send(t, (const uint8_t *)payload + first,
                                     len - first);
    }
    return rc;
}

/* Shared by both streams; PSRP uses "stdin" for data and "pr" for host
 * responses (3.1.5.3.5). */
static psrp_result_t send_on_stream(psrp_transport_t *t, PCWSTR stream,
                                    const void *data, size_t len)
{
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    WSMAN_DATA payload;
    WSMAN_OPERATION_HANDLE send_op = NULL;
    psrp_result_t rc = PSRP_OK;

    if (!t || !data || len == 0) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;

    memset(&payload, 0, sizeof payload);
    payload.type = WSMAN_DATA_TYPE_BINARY;
    payload.binaryData.dataLength = (DWORD)len;
    payload.binaryData.data = (BYTE *)data;

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    WSManSendShellInput(t->shell, t->command, 0, stream, &payload, FALSE,
                        &async, &send_op);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManSendShellInput", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManSendShellInput", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    if (send_op) WSManCloseOperation(send_op, 0);
    op_destroy(&op);
    return rc;
}

psrp_result_t psrp_transport_send(psrp_transport_t *t, const void *data,
                                  size_t len)
{
    return send_on_stream(t, L"stdin", data, len);
}

psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len)
{
    return send_on_stream(t, L"pr", data, len);
}

/* 3.1.5.3.9 fixes this string, and it is misspelled: "crtl_c", not "ctrl_c".
 * Both occurrences in the spec agree, and PowerShell expects it verbatim.
 * Tidying the typo would silently break pipeline cancellation. */
static PCWSTR kSignalTerminate = L"powershell/signal/crtl_c";

psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t)
{
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    WSMAN_OPERATION_HANDLE sig = NULL;
    psrp_result_t rc = PSRP_OK;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->shell || !t->command) return PSRP_ERR_STATE;

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    WSManSignalShell(t->shell, t->command, 0, kSignalTerminate, &async, &sig);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManSignalShell", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManSignalShell", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    if (sig) WSManCloseOperation(sig, 0);
    op_destroy(&op);
    return rc;
}

psrp_result_t psrp_transport_close_shell(psrp_transport_t *t)
{
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    psrp_result_t rc = PSRP_OK;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;

    /* Cancel the receive first; otherwise it reports the shell closing under
     * it as a transport error. */
    if (t->recv_op) {
        EnterCriticalSection(&t->recv.lock);
        t->recv.expect_abort = true;
        LeaveCriticalSection(&t->recv.lock);
        WSManCloseOperation(t->recv_op, 0);
        t->recv_op = NULL;
    }

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    WSManCloseShell(t->shell, 0, &async);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManCloseShell", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManCloseShell", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    op_destroy(&op);
    t->shell = NULL;   /* closed exactly once */
    return rc;
}

psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms)
{
    DWORD waited = 0;
    const DWORD slice = 50;

    if (!t || !out) return PSRP_ERR_INVALID_ARG;

    for (;;) {
        bool have = false;
        bool finished;
        bool needs_repost = false;
        DWORD err;

        EnterCriticalSection(&t->recv.lock);
        if (t->recv.buf.len) {
            if (psrp_buffer_append(out, t->recv.buf.data, t->recv.buf.len)
                == PSRP_OK) {
                psrp_buffer_reset(&t->recv.buf);
                have = true;
            }
        }
        finished = t->recv.done;
        err = t->recv.error;
        /* A Receive operation can end before the command does; re-post so the
         * remaining output still arrives. */
        if (t->recv.op_ended && !t->recv.done) {
            t->recv.op_ended = false;
            needs_repost = true;
        }
        LeaveCriticalSection(&t->recv.lock);

        if (needs_repost) {
            if (t->recv_op) { WSManCloseOperation(t->recv_op, 0); t->recv_op = NULL; }
            post_receive(t);
        }

        if (have) return PSRP_OK;
        if (err != 0) {
            set_error(t, "WSManReceiveShellOutput", err, t->recv.detail);
            return PSRP_ERR_TRANSPORT;
        }
        /* Nothing buffered and nothing more coming, or the caller's patience
         * ran out: both are "no data", not a failure. */
        if (finished) return PSRP_ERR_TRUNCATED;
        if (waited >= timeout_ms) return PSRP_ERR_TRUNCATED;

        WaitForSingleObject(t->recv.data_ready, slice);
        waited += slice;
    }
}
