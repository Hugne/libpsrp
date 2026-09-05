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
    /* A wxf:ConnectResponse carries open content (3.1.5.3.15), and the winrm
     * stack owns that buffer only for the duration of the callback, so it is
     * copied out here. Unused by every other operation. */
    char *response_utf8;
    size_t response_len;
} async_op_t;

/* Copies WSMAN_DATA out as UTF-8, whichever form it arrived in. */
static void capture_response(async_op_t *op, const WSMAN_DATA *d)
{
    if (!d) return;
    if (d->type == WSMAN_DATA_TYPE_TEXT && d->text.buffer && d->text.bufferLength) {
        int n = WideCharToMultiByte(CP_UTF8, 0, d->text.buffer,
                                    (int)d->text.bufferLength, NULL, 0, NULL,
                                    NULL);
        if (n <= 0) return;
        op->response_utf8 = (char *)malloc((size_t)n + 1);
        if (!op->response_utf8) return;
        WideCharToMultiByte(CP_UTF8, 0, d->text.buffer,
                            (int)d->text.bufferLength, op->response_utf8, n,
                            NULL, NULL);
        op->response_utf8[n] = '\0';
        op->response_len = (size_t)n;
    } else if (d->type == WSMAN_DATA_TYPE_BINARY &&
               d->binaryData.data && d->binaryData.dataLength) {
        op->response_utf8 = (char *)malloc(d->binaryData.dataLength + 1);
        if (!op->response_utf8) return;
        memcpy(op->response_utf8, d->binaryData.data, d->binaryData.dataLength);
        op->response_utf8[d->binaryData.dataLength] = '\0';
        op->response_len = d->binaryData.dataLength;
    }
}

static void op_init(async_op_t *op)
{
    memset(op, 0, sizeof *op);
    op->done = CreateEventW(NULL, TRUE, FALSE, NULL);
}

static void op_destroy(async_op_t *op)
{
    if (op->done) CloseHandle(op->done);
    free(op->response_utf8);
    op->response_utf8 = NULL;
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

/* Completion for WSManConnectShell. Identical to the generic one except that
 * the response's open content is kept: 3.1.5.3.15 puts the server's
 * SESSION_CAPABILITY there, in a <connectResponseXml> element, and nowhere
 * else. A connect that ignored it, as this transport did, could never
 * negotiate. */
static void CALLBACK connect_completion(PVOID ctx, DWORD flags,
                                        WSMAN_ERROR *error,
                                        WSMAN_SHELL_HANDLE shell,
                                        WSMAN_COMMAND_HANDLE command,
                                        WSMAN_OPERATION_HANDLE op_handle,
                                        WSMAN_RESPONSE_DATA *data)
{
    async_op_t *op = (async_op_t *)ctx;
    (void)shell; (void)command; (void)op_handle;
    if (data && !op->response_utf8) capture_response(op, &data->connectData.data);
    generic_completion(ctx, flags, error, shell, command, op_handle, NULL);
}

/* ------------------------------------------------------------ receive ---- */

/* Callers copy this buffer out under the lock, so both sides must agree on
 * its size. */
#define PSRP_RECV_DETAIL_CHARS 512

/*
 * One of these per receive channel. There are two, and that is the point:
 *
 *   shell_rx  targets the shell and lives as long as it does. Everything
 *             addressed to the RunspacePool arrives here: availability
 *             replies, USER_EVENT, RUNSPACEPOOL_HOST_CALL, key exchange.
 *   cmd_rx    targets the current command and lives as long as it does.
 *             Pipeline output, records and PIPELINE_HOST_CALL arrive here.
 *
 * A single Receive retargeted from the shell to each command in turn was the
 * previous design, and it lost every pool-level message after the first
 * pipeline started: once retargeted it was never returned to the shell, so
 * a RUNSPACE_AVAILABILITY asked for after a command had run never arrived,
 * and neither did any forwarded event. PowerShell's own client keeps a shell
 * receive open alongside each command's, which is what this now does.
 */
typedef struct recv_ctx {
    CRITICAL_SECTION lock;
    HANDLE data_ready;      /* shared with the other channel; not owned */
    WSMAN_OPERATION_HANDLE op;
    WSMAN_SHELL_ASYNC async;
    psrp_buffer_t buf;      /* raw stdout bytes not yet drained */
    bool done;              /* command reported Done (cmd_rx only) */
    bool op_ended;          /* a Receive operation finished; may need re-post */
    /* Receives we cancelled ourselves and whose completion has not arrived
     * yet. A count rather than a flag, because cancellation is asynchronous:
     * the callback for a cancelled operation can land after the next one has
     * already been posted, and a flag cleared in between turns our own
     * cancellation into a reported fault. That was an intermittent failure
     * roughly one run in a hundred cycles. */
    unsigned pending_aborts;
    DWORD error;
    wchar_t detail[PSRP_RECV_DETAIL_CHARS];
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
    if (error && error->code == ERROR_OPERATION_ABORTED &&
        r->pending_aborts > 0) {
        r->pending_aborts--;
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

    /* Disconnect state (3.1.4.9). The idle timeout has nowhere to ride in
     * WSMAN_SHELL_ASYNC, so it waits here for the one call that reads it. */
    bool disconnected;
    DWORD pending_idle_timeout_ms;
    WSMAN_AUTHENTICATION_CREDENTIALS auth;
    HANDLE data_ready;      /* signalled by either channel */
    recv_ctx_t shell_rx;    /* pool-level traffic */
    recv_ctx_t cmd_rx;      /* the current pipeline's traffic */
    DWORD timeout_ms;
    char last_error[640];
};

static void rx_init(recv_ctx_t *r, HANDLE data_ready)
{
    memset(r, 0, sizeof *r);
    InitializeCriticalSection(&r->lock);
    r->data_ready = data_ready;
    psrp_buffer_init(&r->buf);
}

static void rx_destroy(recv_ctx_t *r)
{
    psrp_buffer_free(&r->buf);
    DeleteCriticalSection(&r->lock);
}

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
    EnterCriticalSection(&mut->cmd_rx.lock);
    done = mut->cmd_rx.done;
    LeaveCriticalSection(&mut->cmd_rx.lock);
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

    t->data_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    rx_init(&t->shell_rx, t->data_ready);
    rx_init(&t->cmd_rx, t->data_ready);
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

/* Cancels the standing Receive. The server tearing it down underneath us looks
 * exactly like a fault, so it is always cancelled deliberately first. */
static void cancel_receive(recv_ctx_t *r)
{
    if (!r->op) return;
    EnterCriticalSection(&r->lock);
    /* Counted, not flagged: the completion for this operation may arrive at
     * any point afterwards, including after the replacement Receive has been
     * posted, and it must still be recognised as ours when it does. */
    r->pending_aborts++;
    LeaveCriticalSection(&r->lock);
    WSManCloseOperation(r->op, 0);
    r->op = NULL;
}

static void cancel_both(psrp_transport_t *t)
{
    cancel_receive(&t->cmd_rx);
    cancel_receive(&t->shell_rx);
}

/* Clears everything the previous stream left behind. A stale `done` is the
 * dangerous one: the receive loop reads it as "nothing more is coming" and
 * never re-posts, so the next shell or command produces no output at all. */
static void reset_receive(recv_ctx_t *r)
{
    EnterCriticalSection(&r->lock);
    psrp_buffer_reset(&r->buf);
    r->done = false;
    r->op_ended = false;
    r->error = 0;
    /* pending_aborts is deliberately left alone. It tracks completions still
     * to arrive for operations we already cancelled, and clearing it here is
     * exactly the bug this replaced: the late completion would then look like
     * a server fault. Only the callback that consumes one may decrement it. */
    LeaveCriticalSection(&r->lock);
}

/* Closes the command handle, if there is one.
 *
 * A command belongs to a shell, so this has to happen before the shell goes
 * away. Closing the shell first and the command afterwards leaves the second
 * call operating on a handle whose parent no longer exists, which is what this
 * code used to do: only psrp_transport_free closed the command, and
 * psrp_transport_close_shell ran before it. */
static void close_command(psrp_transport_t *t)
{
    async_op_t op;
    WSMAN_SHELL_ASYNC a;

    if (!t->command) return;
    op_init(&op);
    a.operationContext = &op;
    a.completionFunction = generic_completion;
    WSManCloseCommand(t->command, 0, &a);
    WaitForSingleObject(op.done, 5000);
    op_destroy(&op);
    t->command = NULL;
}

void psrp_transport_free(psrp_transport_t *t)
{
    if (!t) return;
    cancel_both(t);
    close_command(t);
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

    rx_destroy(&t->cmd_rx);
    rx_destroy(&t->shell_rx);
    if (t->data_ready) CloseHandle(t->data_ready);
    free(t);
}

/* Formats a GUID as the wide hyphenated string WSMan wants for its ids.
 *
 * Upper case, deliberately. psrp_guid_format writes lower case, and a shell
 * created under a lower-case id works fine, but the server reports that same
 * id back in upper case, and WSManConnectShell matches the ShellId selector
 * case-sensitively: connecting with the lower-case form fails with 0x8033805B,
 * "shell was not found", for a shell enumeration had just listed. PowerShell's
 * own client sends ids in upper case throughout, so this does the same. */
static psrp_result_t guid_to_wide(const psrp_guid_t *g, wchar_t out[64])
{
    char narrow[PSRP_GUID_BUF_SIZE];
    size_t i;
    psrp_result_t rc = psrp_guid_format(g, narrow, sizeof narrow);
    if (rc != PSRP_OK) return rc;
    for (i = 0; narrow[i]; i++)
        if (narrow[i] >= 'a' && narrow[i] <= 'f') narrow[i] = (char)(narrow[i] - 32);
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, out, 64);
    return PSRP_OK;
}

/* Builds <element xmlns="...powershell">base64</element> as UTF-16: the open
 * content of a wxf:Create or wxf:Connect.
 *
 * The element name is not decoration. 3.1.5.3.1 names it creationXml for a
 * Create and 3.1.5.3.14 names it connectXml for a Connect, and the PowerShell
 * plugin looks for the one it expects: a Connect carrying a creationXml
 * element reaches the plugin and comes back as a .NET exception
 * (0xE0434352), because the payload it went looking for is not there. */
static psrp_result_t build_open_content(const char *element,
                                        const void *payload, size_t len,
                                        wchar_t **out)
{
    psrp_buffer_t b64, utf8, utf16;
    psrp_result_t rc;
    wchar_t *result = NULL;

    psrp_buffer_init(&b64);
    psrp_buffer_init(&utf8);
    psrp_buffer_init(&utf16);

    rc = psrp_base64_encode_buf(&b64, payload, len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, "<");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, element);
    if (rc == PSRP_OK)
        rc = psrp_buffer_append_str(&utf8,
            " xmlns=\"http://schemas.microsoft.com/powershell\">");
    if (rc == PSRP_OK) rc = psrp_buffer_append(&utf8, b64.data, b64.len);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, "</");
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, element);
    if (rc == PSRP_OK) rc = psrp_buffer_append_str(&utf8, ">");
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

/* Posts a continuous Receive on one channel. `command` is NULL for the
 * shell-level channel and the command handle for the pipeline's. */
static void post_receive(psrp_transport_t *t, recv_ctx_t *r,
                         WSMAN_COMMAND_HANDLE command)
{
    static PCWSTR streams[1] = { L"stdout" };
    WSMAN_STREAM_ID_SET desired;
    desired.streamIDsCount = 1;
    desired.streamIDs = streams;

    r->async.operationContext = r;
    r->async.completionFunction = receive_completion;
    WSManReceiveShellOutput(t->shell, command, 0, &desired, &r->async, &r->op);
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
    rc = build_open_content("creationXml", payload, len, &creation);
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

    if (rc == PSRP_OK) {
        /* A transport can be reused for another shell, and the previous one
         * leaves its stream state behind. Clearing it before posting is what
         * makes reuse work; without this the new shell inherits the old
         * stream's `done` and never delivers anything. */
        reset_receive(&t->shell_rx);
        reset_receive(&t->cmd_rx);
        post_receive(t, &t->shell_rx, NULL);   /* pool-level traffic */
    }
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

    /* The previous pipeline's receive and handle go first. The shell-level
     * channel is deliberately untouched: pool-level traffic keeps flowing
     * while the new command runs. */
    cancel_receive(&t->cmd_rx);
    close_command(t);

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
        reset_receive(&t->cmd_rx);
        post_receive(t, &t->cmd_rx, t->command);
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

/* ---------------- disconnect and reconnect (3.1.4.9, 3.1.4.10) --------- */

/* Runs one shell-level async call and reports its outcome. */
static psrp_result_t run_shell_op(psrp_transport_t *t, const char *what,
                                  void (*start)(psrp_transport_t *,
                                                WSMAN_SHELL_ASYNC *))
{
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    psrp_result_t rc = PSRP_OK;

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = generic_completion;
    start(t, &async);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, what, 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, what, op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    }
    op_destroy(&op);
    return rc;
}

/* The idle timeout has to reach the callback somehow and WSMAN_SHELL_ASYNC
 * carries no room for it, so it rides on the transport for the one call. */
static void start_disconnect(psrp_transport_t *t, WSMAN_SHELL_ASYNC *async)
{
    WSMAN_SHELL_DISCONNECT_INFO info;
    info.idleTimeoutMs = t->pending_idle_timeout_ms;
    WSManDisconnectShell(t->shell, 0,
                         info.idleTimeoutMs ? &info : NULL, async);
}

static void start_reconnect(psrp_transport_t *t, WSMAN_SHELL_ASYNC *async)
{
    WSManReconnectShell(t->shell, 0, async);
}

/* Reconnecting the shell does not reattach its commands. A pipeline that ran
 * on while we were away has its output buffered against the command, and
 * that stream stays detached until the command itself is reconnected. Without
 * this, a reconnect after a busy disconnect came back to silence: the pool
 * was fine and the pipeline's output was simply never delivered. PowerShell's
 * client reconnects each command after the shell for exactly this reason. */
static void start_reconnect_command(psrp_transport_t *t,
                                    WSMAN_SHELL_ASYNC *async)
{
    WSManReconnectShellCommand(t->command, 0, async);
}

psrp_result_t psrp_transport_disconnect(psrp_transport_t *t,
                                        uint32_t idle_timeout_ms)
{
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;
    if (t->disconnected) return PSRP_ERR_STATE;

    /* 3.1.4.9 step 1 waits for sends to finish. Sends here are synchronous, so
     * by the time control returns there is nothing outstanding. The receive is
     * different: it is a standing operation, so cancel it deliberately.
     *
     * The command handle is deliberately kept: a disconnected shell goes on
     * running its pipeline server-side, which is the whole point, and the
     * handle is what a reconnect resumes against. */
    cancel_both(t);

    t->pending_idle_timeout_ms = idle_timeout_ms;
    rc = run_shell_op(t, "WSManDisconnectShell", start_disconnect);
    if (rc == PSRP_OK) t->disconnected = true;
    return rc;
}

psrp_result_t psrp_transport_reconnect(psrp_transport_t *t)
{
    psrp_result_t rc;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;
    if (!t->disconnected) return PSRP_ERR_STATE;

    rc = run_shell_op(t, "WSManReconnectShell", start_reconnect);
    if (rc == PSRP_OK) {
        t->disconnected = false;
        /* 3.1.4.10.2 step 3: a reconnect MUST be followed by a Receive to
         * start data flowing again. Post them explicitly. The old code only
         * cleared flags and relied on the next run_command to post one, so
         * pool-level traffic after a reconnect was never listened for unless
         * a new pipeline happened to follow. */
        reset_receive(&t->shell_rx);
        post_receive(t, &t->shell_rx, NULL);
        if (t->command) {
            /* The command has to be reattached before its Receive means
             * anything; see start_reconnect_command. A failure here is
             * reported but does not undo the shell reconnect, which stands. */
            rc = run_shell_op(t, "WSManReconnectShellCommand",
                              start_reconnect_command);
            reset_receive(&t->cmd_rx);
            post_receive(t, &t->cmd_rx, t->command);
        }
    }
    return rc;
}

/* Pulls the base64 out of <connectResponseXml ...>...</connectResponseXml>
 * and decodes it. Whitespace inside the element is skipped, since the spec's
 * own example wraps the encoded text across lines. */
static psrp_result_t decode_connect_response(const char *xml, size_t n,
                                             psrp_buffer_t *out)
{
    const char *open_tag, *start, *end;
    psrp_buffer_t compact;
    psrp_result_t rc;
    size_t i;

    open_tag = strstr(xml, "<connectResponseXml");
    if (!open_tag) return PSRP_ERR_MALFORMED;
    start = strchr(open_tag, '>');
    if (!start) return PSRP_ERR_MALFORMED;
    start++;
    end = strstr(start, "</connectResponseXml>");
    if (!end) return PSRP_ERR_MALFORMED;
    (void)n;

    psrp_buffer_init(&compact);
    rc = PSRP_OK;
    for (i = 0; rc == PSRP_OK && start + i < end; i++) {
        char c = start[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        rc = psrp_buffer_append_u8(&compact, (uint8_t)c);
    }
    if (rc == PSRP_OK)
        rc = psrp_base64_decode((const char *)compact.data, compact.len, out);
    psrp_buffer_free(&compact);
    return rc;
}

psrp_result_t psrp_transport_connect(psrp_transport_t *t,
                                     const psrp_guid_t *shell_id,
                                     const void *payload, size_t len,
                                     psrp_buffer_t *response_payload)
{
    wchar_t shell_id_w[64];
    wchar_t *connect_xml = NULL;
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    WSMAN_DATA data;
    psrp_result_t rc;

    if (!t || !shell_id || (len && !payload)) return PSRP_ERR_INVALID_ARG;
    if (t->shell) return PSRP_ERR_STATE;

    rc = guid_to_wide(shell_id, shell_id_w);
    if (rc != PSRP_OK) return rc;
    /* Same wrapper shape as a Create's open content, different element name:
     * 3.1.5.3.14 requires connectXml here, and the plugin will not find the
     * payload under any other. */
    rc = build_open_content("connectXml", payload, len, &connect_xml);
    if (rc != PSRP_OK) return rc;

    memset(&data, 0, sizeof data);
    data.type = WSMAN_DATA_TYPE_TEXT;
    data.text.buffer = connect_xml;
    data.text.bufferLength = (DWORD)wcslen(connect_xml);

    op_init(&op);
    async.operationContext = &op;
    async.completionFunction = connect_completion;
    /* No OptionSet on a Connect, unlike a Create. The WSMan client keeps the
     * options given to WSManConnectShell on the shell handle and re-sends
     * them with every later operation, and the server then rejects the very
     * next Receive with 0x80338039, "invalid or duplicated OptionSet".
     * 3.1.5.3.14 lists protocolversion for the Connect, but the pool already
     * exists with a version negotiated at its creation and the server does not
     * need it repeated: a Connect without it succeeds, negotiates, and goes on
     * to run pipelines. Verified against a live server. */
    WSManConnectShell(t->session, 0, kResourceUri, shell_id_w, NULL, &data,
                      &async, &t->shell);
    if (WaitForSingleObject(op.done, t->timeout_ms) != WAIT_OBJECT_0) {
        set_error(t, "WSManConnectShell", 0, L"timed out");
        rc = PSRP_ERR_TRANSPORT;
    } else if (op.error != 0) {
        set_error(t, "WSManConnectShell", op.error, op.detail);
        rc = PSRP_ERR_TRANSPORT;
    } else {
        rc = PSRP_OK;
    }

    if (rc == PSRP_OK && response_payload) {
        /* 3.1.5.3.15: the server's SESSION_CAPABILITY is in the response's
         * open content and nowhere else. A response without it is a failed
         * connect as far as the spec is concerned, so it is an error here. */
        if (!op.response_utf8) {
            set_error(t, "WSManConnectShell", 0,
                      L"no connectResponseXml in the response");
            rc = PSRP_ERR_TRANSPORT;
        } else {
            rc = decode_connect_response(op.response_utf8, op.response_len,
                                         response_payload);
            if (rc != PSRP_OK)
                set_error(t, "WSManConnectShell", 0,
                          L"connectResponseXml could not be decoded");
        }
    }

    free(connect_xml);
    if (rc != PSRP_OK) {
        /* WSManConnectShell may have produced a handle even though the
         * operation as a whole failed -- the connectResponseXml checks above
         * run after a successful connect. Nulling it without closing would
         * strand the shell: psrp_transport_free would no longer see it, and
         * the server would hold the pool until its idle timeout.
         *
         * `op` is deliberately still alive here. On the timeout branch the
         * connect completion is still outstanding and holds &op; closing the
         * shell aborts it, and it will SetEvent(op.done) on its way out. Had
         * op been destroyed first, that handle would be closed and the new
         * event below would very likely be handed the same value, so the
         * aborted connect would signal the close wait instead -- releasing it
         * early and leaving the real close completion to write into a stack
         * frame we had already returned from. */
        if (t->shell) {
            async_op_t close_op;
            WSMAN_SHELL_ASYNC close_async;
            op_init(&close_op);
            close_async.operationContext = &close_op;
            close_async.completionFunction = generic_completion;
            WSManCloseShell(t->shell, 0, &close_async);
            WaitForSingleObject(close_op.done, t->timeout_ms);
            op_destroy(&close_op);
            t->shell = NULL;
        }
        op_destroy(&op);
        return rc;
    }
    op_destroy(&op);

    /* 3.1.4.10.3 step 6: a Receive follows the connect. The old code never
     * posted one, so even traffic that did arrive on the stream had nowhere
     * to land. */
    reset_receive(&t->shell_rx);
    reset_receive(&t->cmd_rx);
    post_receive(t, &t->shell_rx, NULL);
    return PSRP_OK;
}

bool psrp_transport_is_disconnected(const psrp_transport_t *t)
{
    return t && t->disconnected;
}

psrp_result_t psrp_transport_close_shell(psrp_transport_t *t)
{
    async_op_t op;
    WSMAN_SHELL_ASYNC async;
    psrp_result_t rc = PSRP_OK;

    if (!t) return PSRP_ERR_INVALID_ARG;
    if (!t->shell) return PSRP_ERR_STATE;

    /* Order matters: cancel both standing Receives, then release the
     * command, then close the shell. A command belongs to its shell, so
     * closing the shell first would leave the command handle parented to
     * nothing. */
    cancel_both(t);
    close_command(t);

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

/* Drains one channel under its lock. Reports whether data was taken, whether
 * the operation needs re-posting, and any error. */
static void drain_channel(recv_ctx_t *r, psrp_buffer_t *out, bool *have,
                          bool *repost, DWORD *err, wchar_t *detail,
                          bool *done)
{
    EnterCriticalSection(&r->lock);
    if (r->buf.len) {
        if (psrp_buffer_append(out, r->buf.data, r->buf.len) == PSRP_OK) {
            psrp_buffer_reset(&r->buf);
            *have = true;
        }
    }
    if (r->error && !*err) {
        *err = r->error;
        /* Copied out under the lock rather than read afterwards. The callback
         * fills this in with wcsncpy and terminates it on the next line, so a
         * reader racing that pair can see an unterminated buffer and run off
         * the end of it. */
        memcpy(detail, r->detail, PSRP_RECV_DETAIL_CHARS * sizeof *detail);
    }
    if (done) *done = r->done;
    /* A Receive operation can end before what it watches does; re-post so the
     * rest still arrives. The command channel stops once its command is done;
     * the shell channel stops only when the shell does. */
    if (r->op_ended && !r->done) {
        r->op_ended = false;
        *repost = true;
    }
    LeaveCriticalSection(&r->lock);
}

psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms)
{
    DWORD waited = 0;
    const DWORD slice = 50;

    if (!t || !out) return PSRP_ERR_INVALID_ARG;

    for (;;) {
        bool have = false;
        bool cmd_done = false;
        bool repost_shell = false, repost_cmd = false;
        DWORD err = 0;
        wchar_t detail[PSRP_RECV_DETAIL_CHARS];

        detail[0] = L'\0';
        drain_channel(&t->shell_rx, out, &have, &repost_shell, &err, detail,
                      NULL);
        drain_channel(&t->cmd_rx, out, &have, &repost_cmd, &err, detail,
                      &cmd_done);

        /* Deliberately not cancel_receive for either: an operation that has
         * reported END_OF_OPERATION produces no further completion, and
         * counting an abort would leave a phantom that swallowed the next
         * genuine one. */
        if (repost_shell && t->shell && !t->disconnected) {
            if (t->shell_rx.op) { WSManCloseOperation(t->shell_rx.op, 0); t->shell_rx.op = NULL; }
            post_receive(t, &t->shell_rx, NULL);
        }
        if (repost_cmd && t->command && !t->disconnected) {
            if (t->cmd_rx.op) { WSManCloseOperation(t->cmd_rx.op, 0); t->cmd_rx.op = NULL; }
            post_receive(t, &t->cmd_rx, t->command);
        }

        if (have) return PSRP_OK;
        if (err != 0) {
            set_error(t, "WSManReceiveShellOutput", err, detail);
            return PSRP_ERR_TRANSPORT;
        }
        /* Nothing buffered and the pipeline has finished, or the caller's
         * patience ran out: both are "no data", not a failure. The shell
         * channel never finishes on its own, so with no command in flight
         * this waits out the timeout, which is the right answer for a caller
         * polling for pool-level traffic. */
        if (cmd_done && t->command) return PSRP_ERR_TRUNCATED;
        if (waited >= timeout_ms) return PSRP_ERR_TRUNCATED;

        WaitForSingleObject(t->data_ready, slice);
        waited += slice;
    }
}
