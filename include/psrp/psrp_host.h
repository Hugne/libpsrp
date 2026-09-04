/* psrp_host.h - host method calls ([MS-PSRP] 2.2.2.15/16, 2.2.2.27/28,
 * 2.2.3.17, 2.2.6).
 *
 * The server can ask the client's host to do something: read a line, write
 * text, report the window size. Each call carries a call id (ci), a method
 * identifier (mi) and encoded parameters (mp).
 *
 * The response rule is strict and easy to get wrong in both directions:
 *
 *   - If the method returns a value, the client MUST send a response.
 *     Not answering leaves the server waiting, which stalls the pipeline.
 *   - If the method returns nothing, the client MUST NOT send a response.
 *     Answering anyway is a protocol violation.
 *
 * psrp_host_method_returns_value() encodes that table so a caller does not
 * have to memorise 56 method ids.
 */
#ifndef PSRP_HOST_H
#define PSRP_HOST_H

#include "psrp/psrp_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2.2.3.17. The names match PowerShell's RemoteHostMethodId enum. */
typedef enum psrp_host_method {
    PSRP_HOST_GET_NAME = 1,
    PSRP_HOST_GET_VERSION = 2,
    PSRP_HOST_GET_INSTANCE_ID = 3,
    PSRP_HOST_GET_CURRENT_CULTURE = 4,
    PSRP_HOST_GET_CURRENT_UI_CULTURE = 5,
    PSRP_HOST_SET_SHOULD_EXIT = 6,
    PSRP_HOST_ENTER_NESTED_PROMPT = 7,
    PSRP_HOST_EXIT_NESTED_PROMPT = 8,
    PSRP_HOST_NOTIFY_BEGIN_APPLICATION = 9,
    PSRP_HOST_NOTIFY_END_APPLICATION = 10,
    PSRP_HOST_READ_LINE = 11,
    PSRP_HOST_READ_LINE_AS_SECURE_STRING = 12,
    PSRP_HOST_WRITE1 = 13,
    PSRP_HOST_WRITE2 = 14,
    PSRP_HOST_WRITE_LINE1 = 15,
    PSRP_HOST_WRITE_LINE2 = 16,
    PSRP_HOST_WRITE_LINE3 = 17,
    PSRP_HOST_WRITE_ERROR_LINE = 18,
    PSRP_HOST_WRITE_DEBUG_LINE = 19,
    PSRP_HOST_WRITE_PROGRESS = 20,
    PSRP_HOST_WRITE_VERBOSE_LINE = 21,
    PSRP_HOST_WRITE_WARNING_LINE = 22,
    PSRP_HOST_PROMPT = 23,
    PSRP_HOST_PROMPT_FOR_CREDENTIAL1 = 24,
    PSRP_HOST_PROMPT_FOR_CREDENTIAL2 = 25,
    PSRP_HOST_PROMPT_FOR_CHOICE = 26,
    PSRP_HOST_GET_FOREGROUND_COLOR = 27,
    PSRP_HOST_SET_FOREGROUND_COLOR = 28,
    PSRP_HOST_GET_BACKGROUND_COLOR = 29,
    PSRP_HOST_SET_BACKGROUND_COLOR = 30,
    PSRP_HOST_GET_CURSOR_POSITION = 31,
    PSRP_HOST_SET_CURSOR_POSITION = 32,
    PSRP_HOST_GET_WINDOW_POSITION = 33,
    PSRP_HOST_SET_WINDOW_POSITION = 34,
    PSRP_HOST_GET_CURSOR_SIZE = 35,
    PSRP_HOST_SET_CURSOR_SIZE = 36,
    PSRP_HOST_GET_BUFFER_SIZE = 37,
    PSRP_HOST_SET_BUFFER_SIZE = 38,
    PSRP_HOST_GET_WINDOW_SIZE = 39,
    PSRP_HOST_SET_WINDOW_SIZE = 40,
    PSRP_HOST_GET_WINDOW_TITLE = 41,
    PSRP_HOST_SET_WINDOW_TITLE = 42,
    PSRP_HOST_GET_MAX_WINDOW_SIZE = 43,
    PSRP_HOST_GET_MAX_PHYSICAL_WINDOW_SIZE = 44,
    PSRP_HOST_GET_KEY_AVAILABLE = 45,
    PSRP_HOST_READ_KEY = 46,
    PSRP_HOST_FLUSH_INPUT_BUFFER = 47,
    PSRP_HOST_SET_BUFFER_CONTENTS1 = 48,
    PSRP_HOST_SET_BUFFER_CONTENTS2 = 49,
    PSRP_HOST_GET_BUFFER_CONTENTS = 50,
    PSRP_HOST_SCROLL_BUFFER_CONTENTS = 51,
    PSRP_HOST_PUSH_RUNSPACE = 52,
    PSRP_HOST_POP_RUNSPACE = 53,
    PSRP_HOST_GET_IS_RUNSPACE_PUSHED = 54,
    PSRP_HOST_GET_RUNSPACE = 55,
    PSRP_HOST_PROMPT_FOR_CHOICE_MULTIPLE_SELECTION = 56
} psrp_host_method_t;

/* Method name, e.g. "ReadLine"; "Unknown" outside the table. */
const char *psrp_host_method_name(int32_t method_id);

/* True when the method has a return value, which is exactly when a response
 * is required. See the header comment. */
bool psrp_host_method_returns_value(int32_t method_id);

/* A parsed RUNSPACEPOOL_HOST_CALL or PIPELINE_HOST_CALL. */
typedef struct psrp_host_call {
    int64_t call_id;          /* ci */
    int32_t method_id;        /* mi */
    psrp_value_t parameters;  /* mp; owned. Usually a list object. */
} psrp_host_call_t;

psrp_result_t psrp_parse_host_call(const void *xml, size_t n,
                                   psrp_host_call_t *out);
void psrp_host_call_free(psrp_host_call_t *c);

/* Builds a response carrying the method's return value (the `mr` property).
 * `return_value` may be NULL to send an explicit null. */
psrp_result_t psrp_build_host_response(int64_t call_id, int32_t method_id,
                                       const psrp_value_t *return_value,
                                       psrp_buffer_t *out);

/* Builds a response reporting that the host could not perform the call (the
 * `me` property). Use this when the method is unsupported: it is the correct
 * answer, and far better than silence, which would stall the server. */
psrp_result_t psrp_build_host_response_error(int64_t call_id, int32_t method_id,
                                             const char *message,
                                             psrp_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_HOST_H */
