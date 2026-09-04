/* psrp_wsman_api.h - the subset of the Win32 WSMan API we use, declared for C.
 *
 * Why not just include <wsman.h>? Because the Windows SDK's wsman.h is not
 * valid C. It unconditionally declares
 *
 *     typedef struct _WSMAN_SHELL_STARTUP_INFO_V11 : _WSMAN_SHELL_STARTUP_INFO_V10
 *
 * using C++ struct inheritance, outside any __cplusplus or API-version guard,
 * so the header fails to compile in a C translation unit no matter which
 * WSMAN_API_VERSION macro is defined.
 *
 * The alternatives were to compile this one file as C++ -- dragging C++
 * linkage into an otherwise pure C static library -- or to declare what we
 * need ourselves. We do the latter, which is also what Microsoft's own
 * psl-omi-provider does: it ships its own wsman.h rather than the SDK's.
 *
 * Layouts below are transcribed from the SDK header; the ABI is plain C
 * structs, so this is a declaration difference, not a reimplementation.
 *
 * We request API version 1.1, because PSRP needs WSManRunShellCommandEx: the
 * spec (3.1.5.3.3) requires the rsp:Command element to be EMPTY with the
 * payload in rsp:Arguments, and the non-Ex WSManRunShellCommand rejects an
 * empty command line with 0x80338180. Version 1.1 also means the completion
 * callback receives WSMAN_RESPONSE_DATA rather than the receive result
 * directly, and the startup info gains a trailing `name` field.
 */
#ifndef PSRP_WSMAN_API_H
#define PSRP_WSMAN_API_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSMAN_API *WSMAN_API_HANDLE;
typedef struct WSMAN_SESSION *WSMAN_SESSION_HANDLE;
typedef struct WSMAN_OPERATION *WSMAN_OPERATION_HANDLE;
typedef struct WSMAN_SHELL *WSMAN_SHELL_HANDLE;
typedef struct WSMAN_COMMAND *WSMAN_COMMAND_HANDLE;

#define WSMAN_FLAG_REQUESTED_API_VERSION_1_0 0x0
#define WSMAN_FLAG_REQUESTED_API_VERSION_1_1 0x1
#define WSMAN_FLAG_AUTH_NEGOTIATE 0x4
#define WSMAN_FLAG_CALLBACK_END_OF_OPERATION 0x1

typedef enum WSManDataType {
    WSMAN_DATA_NONE = 0,
    WSMAN_DATA_TYPE_TEXT = 1,
    WSMAN_DATA_TYPE_BINARY = 2,
    WSMAN_DATA_TYPE_DWORD = 4
} WSManDataType;

typedef struct _WSMAN_DATA_TEXT {
    DWORD bufferLength;
    PCWSTR buffer;
} WSMAN_DATA_TEXT;

typedef struct _WSMAN_DATA_BINARY {
    DWORD dataLength;
    BYTE *data;
} WSMAN_DATA_BINARY;

typedef struct _WSMAN_DATA {
    WSManDataType type;
    union {
        WSMAN_DATA_TEXT text;
        WSMAN_DATA_BINARY binaryData;
        DWORD number;
    };
} WSMAN_DATA;

typedef struct _WSMAN_ERROR {
    DWORD code;
    PCWSTR errorDetail;
    PCWSTR language;
    PCWSTR machineName;
    PCWSTR pluginName;
} WSMAN_ERROR;

typedef struct _WSMAN_USERNAME_PASSWORD_CREDS {
    PCWSTR username;
    PCWSTR password;
} WSMAN_USERNAME_PASSWORD_CREDS;

typedef struct _WSMAN_AUTHENTICATION_CREDENTIALS {
    DWORD authenticationMechanism;
    union {
        WSMAN_USERNAME_PASSWORD_CREDS userAccount;
        PCWSTR certificateThumbprint;
    };
} WSMAN_AUTHENTICATION_CREDENTIALS;

typedef struct _WSMAN_OPTION {
    PCWSTR name;
    PCWSTR value;
    BOOL mustComply;
} WSMAN_OPTION;

typedef struct _WSMAN_OPTION_SET {
    DWORD optionsCount;
    WSMAN_OPTION *options;
    BOOL optionsMustUnderstand;
} WSMAN_OPTION_SET;

typedef struct _WSMAN_STREAM_ID_SET {
    DWORD streamIDsCount;
    PCWSTR *streamIDs;
} WSMAN_STREAM_ID_SET;

typedef struct _WSMAN_ENVIRONMENT_VARIABLE {
    PCWSTR name;
    PCWSTR value;
} WSMAN_ENVIRONMENT_VARIABLE;

typedef struct _WSMAN_ENVIRONMENT_VARIABLE_SET {
    DWORD varsCount;
    WSMAN_ENVIRONMENT_VARIABLE *vars;
} WSMAN_ENVIRONMENT_VARIABLE_SET;

typedef struct _WSMAN_COMMAND_ARG_SET {
    DWORD argsCount;
    PCWSTR *args;
} WSMAN_COMMAND_ARG_SET;

/* V11 is V10 plus a trailing `name`. The SDK expresses that with C++ struct
 * inheritance, which is exactly what makes its header unusable from C; the
 * flattened layout below is ABI-identical. */
typedef struct _WSMAN_SHELL_STARTUP_INFO_V11 {
    WSMAN_STREAM_ID_SET *inputStreamSet;
    WSMAN_STREAM_ID_SET *outputStreamSet;
    DWORD idleTimeoutMs;
    PCWSTR workingDirectory;
    WSMAN_ENVIRONMENT_VARIABLE_SET *variableSet;
    PCWSTR name;
} WSMAN_SHELL_STARTUP_INFO;

typedef struct _WSMAN_RECEIVE_DATA_RESULT {
    PCWSTR streamId;
    WSMAN_DATA streamData;
    PCWSTR commandState;
    DWORD exitCode;
} WSMAN_RECEIVE_DATA_RESULT;

typedef struct _WSMAN_PROXY_INFO WSMAN_PROXY_INFO;

typedef struct _WSMAN_CONNECT_DATA { WSMAN_DATA data; } WSMAN_CONNECT_DATA;
typedef struct _WSMAN_CREATE_SHELL_DATA { WSMAN_DATA data; } WSMAN_CREATE_SHELL_DATA;

typedef union _WSMAN_RESPONSE_DATA {
    WSMAN_RECEIVE_DATA_RESULT receiveData;
    WSMAN_CONNECT_DATA connectData;
    WSMAN_CREATE_SHELL_DATA createData;
} WSMAN_RESPONSE_DATA;

/* API 1.1 callback. */
typedef void (CALLBACK *WSMAN_SHELL_COMPLETION_FUNCTION)(
    PVOID operationContext,
    DWORD flags,
    WSMAN_ERROR *error,
    WSMAN_SHELL_HANDLE shell,
    WSMAN_COMMAND_HANDLE command,
    WSMAN_OPERATION_HANDLE operationHandle,
    WSMAN_RESPONSE_DATA *data);

typedef struct _WSMAN_SHELL_ASYNC {
    PVOID operationContext;
    WSMAN_SHELL_COMPLETION_FUNCTION completionFunction;
} WSMAN_SHELL_ASYNC;

DWORD WINAPI WSManInitialize(DWORD flags, WSMAN_API_HANDLE *apiHandle);
DWORD WINAPI WSManDeinitialize(WSMAN_API_HANDLE apiHandle, DWORD flags);

DWORD WINAPI WSManCreateSession(
    WSMAN_API_HANDLE apiHandle, PCWSTR connection, DWORD flags,
    WSMAN_AUTHENTICATION_CREDENTIALS *serverAuthenticationCredentials,
    WSMAN_PROXY_INFO *proxyInfo, WSMAN_SESSION_HANDLE *session);
DWORD WINAPI WSManCloseSession(WSMAN_SESSION_HANDLE session, DWORD flags);

void WINAPI WSManCreateShell(
    WSMAN_SESSION_HANDLE session, DWORD flags, PCWSTR resourceUri,
    WSMAN_SHELL_STARTUP_INFO *startupInfo, WSMAN_OPTION_SET *options,
    WSMAN_DATA *createXml, WSMAN_SHELL_ASYNC *async,
    WSMAN_SHELL_HANDLE *shell);
/* Ex variant: lets the caller choose the WSMan shell id. */
void WINAPI WSManCreateShellEx(
    WSMAN_SESSION_HANDLE session, DWORD flags, PCWSTR resourceUri,
    PCWSTR shellId, WSMAN_SHELL_STARTUP_INFO *startupInfo,
    WSMAN_OPTION_SET *options, WSMAN_DATA *createXml,
    WSMAN_SHELL_ASYNC *async, WSMAN_SHELL_HANDLE *shell);

void WINAPI WSManCloseShell(WSMAN_SHELL_HANDLE shellHandle, DWORD flags,
                            WSMAN_SHELL_ASYNC *async);

void WINAPI WSManRunShellCommand(
    WSMAN_SHELL_HANDLE shell, DWORD flags, PCWSTR commandLine,
    WSMAN_COMMAND_ARG_SET *args, WSMAN_OPTION_SET *options,
    WSMAN_SHELL_ASYNC *async, WSMAN_COMMAND_HANDLE *command);
/* Ex variant: takes an explicit command id and, unlike the non-Ex form,
 * accepts the empty command line that PSRP requires. */
void WINAPI WSManRunShellCommandEx(
    WSMAN_SHELL_HANDLE shell, DWORD flags, PCWSTR commandId,
    PCWSTR commandLine, WSMAN_COMMAND_ARG_SET *args,
    WSMAN_OPTION_SET *options, WSMAN_SHELL_ASYNC *async,
    WSMAN_COMMAND_HANDLE *command);

void WINAPI WSManCloseCommand(WSMAN_COMMAND_HANDLE commandHandle, DWORD flags,
                              WSMAN_SHELL_ASYNC *async);

void WINAPI WSManSendShellInput(
    WSMAN_SHELL_HANDLE shell, WSMAN_COMMAND_HANDLE command, DWORD flags,
    PCWSTR streamId, WSMAN_DATA *streamData, BOOL endOfStream,
    WSMAN_SHELL_ASYNC *async, WSMAN_OPERATION_HANDLE *sendOperation);

void WINAPI WSManReceiveShellOutput(
    WSMAN_SHELL_HANDLE shell, WSMAN_COMMAND_HANDLE command, DWORD flags,
    WSMAN_STREAM_ID_SET *desiredStreamSet, WSMAN_SHELL_ASYNC *async,
    WSMAN_OPERATION_HANDLE *receiveOperation);

void WINAPI WSManCloseOperation(WSMAN_OPERATION_HANDLE operationHandle,
                                DWORD flags);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_WSMAN_API_H */
