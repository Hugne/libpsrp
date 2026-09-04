/* psrp_metadata.h - command metadata and user events
 * ([MS-PSRP] 2.2.2.12, 2.2.2.14, 2.2.3.19-23).
 *
 * GET_COMMAND_METADATA asks the server which commands exist. Its results come
 * back on the pipeline's normal output stream: first a CommandMetadataCount
 * object saying how many follow, then one CommandMetadata object per command.
 */
#ifndef PSRP_METADATA_H
#define PSRP_METADATA_H

#include "psrp/psrp_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2.2.3.19 CommandType: a 32-bit set of flags. The spec says PSRP does not
 * interpret this and that the categories are defined by the higher layer, so
 * these are the well-known System.Management.Automation.CommandTypes values
 * offered as a convenience rather than as protocol constants. */
#define PSRP_COMMAND_TYPE_ALIAS            0x0001
#define PSRP_COMMAND_TYPE_FUNCTION         0x0002
#define PSRP_COMMAND_TYPE_FILTER           0x0004
#define PSRP_COMMAND_TYPE_CMDLET           0x0008
#define PSRP_COMMAND_TYPE_EXTERNAL_SCRIPT  0x0010
#define PSRP_COMMAND_TYPE_APPLICATION      0x0020
#define PSRP_COMMAND_TYPE_SCRIPT           0x0040
#define PSRP_COMMAND_TYPE_CONFIGURATION    0x0100
#define PSRP_COMMAND_TYPE_ALL              0x017F

/* Name of a single-flag command type, e.g. "Cmdlet". Returns NULL when the
 * value is not exactly one known flag, since a set has no single name. */
const char *psrp_command_type_name(int32_t command_type);

/* 2.2.2.14 GET_COMMAND_METADATA.
 *
 * `name_patterns` are Wildcards (2.2.3.20): ordinary strings where the escape
 * character is a backtick rather than a backslash. Passing none sends Null,
 * which the spec defines as meaning a single "*".
 *
 * Namespace is sent as Null; the spec gives Null the same meaning as a list
 * with one empty string, so there is nothing to lose.
 *
 * `argument_list` is the optional 2.2.3.24 ArgumentList: a list of arbitrary
 * objects the server's higher layer may use to shape the parameter metadata
 * it returns. Pass NULL to send Null, which means no arguments. It is copied,
 * not consumed. Passing a value that is not a list is rejected, since the
 * spec says ArgumentList MUST be one. */
psrp_result_t psrp_build_get_command_metadata(const char *const *name_patterns,
                                              size_t pattern_count,
                                              int32_t command_type,
                                              const psrp_value_t *argument_list,
                                              psrp_buffer_t *out);

/* 2.2.3.21 CommandMetadataCount: the first object of the result stream. */
psrp_result_t psrp_parse_command_metadata_count(const void *xml, size_t n,
                                                int32_t *count);

/* 2.2.3.23 ParameterMetadata, one per parameter a command accepts. */
typedef struct psrp_parameter_metadata {
    char *name;            /* a non-empty String per the spec */
    char *parameter_type;  /* a .NET type name; NULL when absent */
    char **aliases;        /* alternative names */
    size_t alias_count;
    bool is_switch;        /* SwitchParameter */
    bool is_dynamic;       /* IsDynamic */
} psrp_parameter_metadata_t;

/* 2.2.3.22 CommandMetadata, one per command. */
typedef struct psrp_command_metadata {
    char *name;
    char *command_namespace;   /* Namespace; NULL when absent */
    char *help_uri;            /* NULL when absent or Null */
    int32_t command_type;      /* -1 when absent */
    char **parameter_names;    /* keys of the Parameters dictionary */
    size_t parameter_count;
    /* One entry per parameter_names entry, in the same order. A parameter
     * whose metadata object was missing or unreadable still gets an entry,
     * with only its name filled in, so the two arrays stay aligned. */
    psrp_parameter_metadata_t *parameters;
} psrp_command_metadata_t;

psrp_result_t psrp_parse_command_metadata(const void *xml, size_t n,
                                          psrp_command_metadata_t *out);
void psrp_command_metadata_free(psrp_command_metadata_t *m);

/* 2.2.2.12 USER_EVENT. The property names really do contain dots, e.g.
 * "PSEventArgs.EventIdentifier". Sender, SourceArgs and MessageData are
 * arbitrary higher-layer objects and are not surfaced here. */
typedef struct psrp_user_event {
    int32_t event_id;          /* -1 when absent */
    char *source_identifier;
    char *time_generated;      /* raw DateTime text */
    char *computer_name;       /* NULL when absent or Null */
} psrp_user_event_t;

psrp_result_t psrp_parse_user_event(const void *xml, size_t n,
                                    psrp_user_event_t *out);
void psrp_user_event_free(psrp_user_event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_METADATA_H */
