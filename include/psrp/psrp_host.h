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

/* ------------------------------ host value types (2.2.3.1-3) ----------- */
/*
 * Coordinates, Size and Color all serialize as a "typed value" wrapper: an
 * object with a T property naming the .NET type and a V property holding the
 * value. Color's V is a plain int; the other two nest another object.
 */

/* 2.2.3.3, whose underlying type is System.ConsoleColor.
 *
 * The spec's table lists 1..15 and omits 0 entirely, but System.ConsoleColor
 * defines 0 as Black and a server can legitimately send it -- a black
 * foreground is not exotic. We accept and name it rather than rejecting a
 * value the named type clearly permits. */
typedef enum psrp_console_color {
    PSRP_COLOR_BLACK = 0,
    PSRP_COLOR_DARK_BLUE = 1,
    PSRP_COLOR_DARK_GREEN = 2,
    PSRP_COLOR_DARK_CYAN = 3,
    PSRP_COLOR_DARK_RED = 4,
    PSRP_COLOR_DARK_MAGENTA = 5,
    PSRP_COLOR_DARK_YELLOW = 6,
    PSRP_COLOR_GRAY = 7,
    PSRP_COLOR_DARK_GRAY = 8,
    PSRP_COLOR_BLUE = 9,
    PSRP_COLOR_GREEN = 10,
    PSRP_COLOR_CYAN = 11,
    PSRP_COLOR_RED = 12,
    PSRP_COLOR_MAGENTA = 13,
    PSRP_COLOR_YELLOW = 14,
    PSRP_COLOR_WHITE = 15
} psrp_console_color_t;

const char *psrp_console_color_name(int32_t color);

/* Build the T/V wrappers. Each sets *out to a complex object value.
 *
 * These are the shape the _hostDefaultData dictionary (2.2.3.14) wants. They
 * are NOT the shape of a host method's return value: 2.2.6.1.1 sends a plainly
 * serializable type unencoded, so a ReadLine reply is a bare string set with
 * psrp_value_set_string, and answering with the wrapped form makes the server
 * fail its cast to System.String. Verified against a live server. */
psrp_result_t psrp_host_make_coordinates(int32_t x, int32_t y,
                                         psrp_value_t *out);
psrp_result_t psrp_host_make_size(int32_t width, int32_t height,
                                  psrp_value_t *out);
psrp_result_t psrp_host_make_color(int32_t color, psrp_value_t *out);
psrp_result_t psrp_host_make_string(const char *utf8, psrp_value_t *out);

/* Read them back. Return PSRP_ERR_MALFORMED if the shape is wrong. */
psrp_result_t psrp_host_read_coordinates(const psrp_value_t *v, int32_t *x,
                                         int32_t *y);
psrp_result_t psrp_host_read_size(const psrp_value_t *v, int32_t *width,
                                  int32_t *height);
psrp_result_t psrp_host_read_color(const psrp_value_t *v, int32_t *color);

/* -------------------------- _hostDefaultData (2.2.3.14) ---------------- */

/* The ten entries 2.2.3.14 requires when _hostDefaultData is present, keyed
 * by the integers in its table. A client that supplies a real host fills this
 * in; one that does not omits the whole dictionary. */
typedef struct psrp_host_default_data {
    int32_t foreground_color;              /* key 0, Color */
    int32_t background_color;              /* key 1, Color */
    int32_t cursor_position_x;             /* key 2, Coordinates */
    int32_t cursor_position_y;
    int32_t window_position_x;             /* key 3, Coordinates */
    int32_t window_position_y;
    int32_t cursor_size;                   /* key 4, Int32 */
    int32_t buffer_width;                  /* key 5, Size */
    int32_t buffer_height;
    int32_t window_width;                  /* key 6, Size */
    int32_t window_height;
    int32_t max_window_width;              /* key 7, Size */
    int32_t max_window_height;
    int32_t max_physical_window_width;     /* key 8, Size */
    int32_t max_physical_window_height;
    const char *window_title;              /* key 9, String; not owned */
} psrp_host_default_data_t;

/* Plausible values for a headless 120x50 console. */
void psrp_host_default_data_defaults(psrp_host_default_data_t *out);

/* Builds the <Obj N="_hostDefaultData"> value, whose `data` property is a
 * hashtable from those integer keys to typed-value wrappers. */
psrp_result_t psrp_host_build_default_data(const psrp_host_default_data_t *d,
                                           psrp_value_t *out);

/* ------------------- keyboard, buffer and credential types ------------- */
/*
 * Note which property bag each type uses: KeyInfo carries extended
 * properties (<MS>) while BufferCell and PSCredential carry adapted ones
 * (<Props>). Reading from the wrong bag simply finds nothing, so the
 * distinction is load-bearing rather than cosmetic.
 */

/* 2.2.3.27 ControlKeyStates: bit flags in a signed int. */
#define PSRP_CONTROL_KEY_RIGHT_ALT   0x0001
#define PSRP_CONTROL_KEY_LEFT_ALT    0x0002
#define PSRP_CONTROL_KEY_RIGHT_CTRL  0x0004
#define PSRP_CONTROL_KEY_LEFT_CTRL   0x0008
#define PSRP_CONTROL_KEY_SHIFT       0x0010
#define PSRP_CONTROL_KEY_NUM_LOCK    0x0020
#define PSRP_CONTROL_KEY_SCROLL_LOCK 0x0040
#define PSRP_CONTROL_KEY_CAPS_LOCK   0x0080
#define PSRP_CONTROL_KEY_ENHANCED    0x0100

/* 2.2.3.29 BufferCellType. */
typedef enum psrp_buffer_cell_type {
    PSRP_BUFFER_CELL_COMPLETE = 0,
    PSRP_BUFFER_CELL_LEADING = 1,
    PSRP_BUFFER_CELL_TRAILING = 2
} psrp_buffer_cell_type_t;

/* 2.2.3.30 CommandOrigin. PSRP MUST NOT interpret this. */
typedef enum psrp_command_origin {
    PSRP_COMMAND_ORIGIN_RUNSPACE = 0,
    PSRP_COMMAND_ORIGIN_INTERNAL = 1
} psrp_command_origin_t;

const char *psrp_buffer_cell_type_name(int32_t type);
const char *psrp_command_origin_name(int32_t origin);

/* 2.2.3.26 KeyInfo, in extended properties. */
typedef struct psrp_key_info {
    int32_t virtual_key_code;
    uint16_t character;        /* a UTF-16 code unit */
    int32_t control_key_state; /* PSRP_CONTROL_KEY_* flags */
    bool key_down;
} psrp_key_info_t;

psrp_result_t psrp_host_make_key_info(const psrp_key_info_t *k,
                                      psrp_value_t *out);
psrp_result_t psrp_host_read_key_info(const psrp_value_t *v,
                                      psrp_key_info_t *out);

/* 2.2.3.28 BufferCell, in adapted properties. The colours are 2.2.3.3
 * Color wrappers, not bare ints. */
typedef struct psrp_buffer_cell {
    uint16_t character;
    int32_t foreground_color;
    int32_t background_color;
    int32_t cell_type;         /* psrp_buffer_cell_type_t */
} psrp_buffer_cell_t;

psrp_result_t psrp_host_make_buffer_cell(const psrp_buffer_cell_t *c,
                                         psrp_value_t *out);
psrp_result_t psrp_host_read_buffer_cell(const psrp_value_t *v,
                                         psrp_buffer_cell_t *out);

/* 2.2.3.25 PSCredential, in adapted properties, and it MUST carry its type
 * names. The password is a SecureString: pass the base64 ciphertext produced
 * by psrp_crypto_encrypt_string, since a credential can only be sent after
 * the session key exchange. */
psrp_result_t psrp_host_make_credential(const char *username,
                                        const char *password_ciphertext_b64,
                                        psrp_value_t *out);
/* Both outputs are allocated and owned by the caller; either may be NULL. */
psrp_result_t psrp_host_read_credential(const psrp_value_t *v, char **username,
                                        char **password_ciphertext_b64);

/* -------------------- host method parameters (2.2.6) ------------------- */
/*
 * A host call's `mp` is a list of parameters. How each element is encoded
 * depends on its type (2.2.6.1):
 *
 *   - A plainly serializable value is NOT encoded at all; it appears as
 *     itself (2.2.6.1.1). This is the common case.
 *   - Lists and collections become a T/V wrapper whose V is a list
 *     (2.2.6.1.3, 2.2.6.1.5).
 *   - Arrays become an object with `mae` (elements flattened deepest-first)
 *     and `mal` (the size of each dimension) (2.2.6.1.4).
 *   - CultureInfo is reduced to its ToString() (2.2.6.1.2).
 *
 * Because an unencoded value and a wrapped one are both possible, callers
 * should read a parameter through psrp_host_param_unwrap rather than
 * assuming either shape.
 */

/* Number of parameters in a host call's `mp` value; 0 if it is absent. */
size_t psrp_host_param_count(const psrp_value_t *mp);

/* The parameter at `index`, or NULL when out of range. */
const psrp_value_t *psrp_host_param(const psrp_value_t *mp, size_t index);

/* Returns the value a parameter carries. If it is a T/V wrapper the inner V
 * is returned and *type_name (when given) is set to the T string; otherwise
 * the value is returned unchanged with *type_name set to NULL, which is what
 * 2.2.6.1.1 describes. Never returns NULL for a non-NULL input. */
const psrp_value_t *psrp_host_param_unwrap(const psrp_value_t *v,
                                           const char **type_name);

/* 2.2.6.1.4 array parameter. `elements` receives the flattened `mae` list
 * object and `dimensions` the `mal` list of sizes; either may be NULL.
 * Returns PSRP_ERR_MALFORMED when the value is not an encoded array. */
psrp_result_t psrp_host_param_array(const psrp_value_t *v,
                                    const psrp_value_t **elements,
                                    const psrp_value_t **dimensions);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_HOST_H */
