/** @file
 * psrp_records.h - the records a pipeline emits on its output streams.
 *
 * Covers ERROR_RECORD (2.2.2.20 / 2.2.3.15), the informational records shared
 * by DEBUG / VERBOSE / WARNING (2.2.2.22-24 / 2.2.3.16), PROGRESS_RECORD
 * (2.2.2.25 / 2.2.5.1.25), INFORMATION_RECORD (2.2.2.26), and PIPELINE_OUTPUT
 * (2.2.2.19), whose payload is simply a serialized object of any type.
 */
#ifndef PSRP_RECORDS_H
#define PSRP_RECORDS_H

#include "psrp/psrp_object.h"
#include "psrp/psrp_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------- 2.2.3.9 ------------ */

/** The spec's table runs 0..22 and 25; 23 and 24 are not defined. */
const char *psrp_error_category_name(int32_t category);

/* ------------------------------------- 2.2.2.20 / 2.2.3.15 ErrorRecord -- */

/** The fields a caller actually needs. Exception, TargetObject and
 * InvocationInfo are deliberately not surfaced: the spec states PSRP MUST NOT
 * interpret them, and they are arbitrary higher-layer objects. Use
 * psrp_parse_pipeline_output on the raw payload if you need them. */
typedef struct psrp_error_record {
    char *message;                   /**< the record's ToString */
    char *fully_qualified_error_id;  /**< FullyQualifiedErrorId */
    char *category_message;          /**< ErrorCategory_Message */
    char *category_reason;           /**< ErrorCategory_Reason */
    char *category_activity;         /**< ErrorCategory_Activity */
    char *target_name;               /**< ErrorCategory_TargetName */
    int32_t category;                /**< ErrorCategory_Category, -1 if absent */
} psrp_error_record_t;

psrp_result_t psrp_parse_error_record(const void *xml, size_t n,
                                      psrp_error_record_t *out);
void psrp_error_record_free(psrp_error_record_t *r);

/* ------------------------------------------------------- 2.2.3.15.1 ----- */
/**
 * InvocationInfo describes the higher-layer command that produced a record.
 * Error records and informational records may both carry it, and every field
 * is optional: PowerShell only fills it in when the record was asked to
 * serialize it.
 *
 * The spec is explicit that PSRP implementations MUST NOT interpret this data,
 * so it is surfaced as it arrives and never acted on. The bound parameters and
 * unbound arguments hold arbitrary objects, so they stay as values rather than
 * being flattened into strings that would lose their types.
 */
typedef struct psrp_invocation_info {
    char *invocation_name;      /**< InvocationInfo_InvocationName */
    char *line;                 /**< InvocationInfo_Line */
    char *position_message;     /**< InvocationInfo_PositionMessage */
    char *script_name;          /**< InvocationInfo_ScriptName */

    int32_t command_origin;     /**< psrp_command_origin_t, -1 if absent */
    int32_t offset_in_line;     /**< -1 if absent */
    int32_t script_line_number; /**< -1 if absent */
    int32_t pipeline_length;    /**< -1 if absent */
    int32_t pipeline_position;  /**< -1 if absent */
    int64_t history_id;         /**< -1 if absent */

    bool expecting_input;
    bool has_expecting_input;

    /** Owned. Null values when the property was absent. */
    psrp_value_t bound_parameters;   /**< dictionary of name -> value */
    psrp_value_t unbound_arguments;  /**< list of values */
    psrp_value_t pipeline_iteration_info;  /**< list of Signed Int */
} psrp_invocation_info_t;

/** Reads the InvocationInfo properties out of a record's XML. Every field is
 * optional, so a record carrying none of them parses successfully with
 * everything empty; only malformed XML fails. */
psrp_result_t psrp_parse_invocation_info(const void *xml, size_t n,
                                         psrp_invocation_info_t *out);
void psrp_invocation_info_free(psrp_invocation_info_t *i);

/** True when any InvocationInfo property was present. */
bool psrp_invocation_info_present(const psrp_invocation_info_t *i);

/** --------------------------------- 2.2.3.16 Debug/Verbose/Warning ------- */

typedef struct psrp_informational_record {
    char *message;          /**< InformationalRecord_Message */
    bool has_invocation_info;/* InformationalRecord_SerializeInvocationInfo */
} psrp_informational_record_t;

psrp_result_t psrp_parse_informational_record(const void *xml, size_t n,
                                              psrp_informational_record_t *out);
void psrp_informational_record_free(psrp_informational_record_t *r);

/** --------------------------------------------- 2.2.5.1.25 Progress ------ */

typedef struct psrp_progress_record {
    char *activity;
    char *status_description;
    char *current_operation;
    int32_t activity_id;
    int32_t parent_activity_id;
    int32_t percent_complete;
    int32_t seconds_remaining;
} psrp_progress_record_t;

psrp_result_t psrp_parse_progress_record(const void *xml, size_t n,
                                         psrp_progress_record_t *out);
void psrp_progress_record_free(psrp_progress_record_t *r);

/* ------------------------------------------ 2.2.2.26 Information -------- */

/** Note these arrive as adapted properties (`<Props>`), not extended ones. */
typedef struct psrp_information_record {
    char *message_data;
    char *source;
    char *time_generated;   /**< raw DateTime text */
} psrp_information_record_t;

psrp_result_t psrp_parse_information_record(const void *xml, size_t n,
                                            psrp_information_record_t *out);
void psrp_information_record_free(psrp_information_record_t *r);

/* ------------------------------------- 2.2.2.19 / 2.2.2.17 pipeline I/O - */

/** PIPELINE_OUTPUT and PIPELINE_INPUT payloads are just a serialized object. */
psrp_result_t psrp_parse_pipeline_output(const void *xml, size_t n,
                                         psrp_value_t *out);
psrp_result_t psrp_build_pipeline_input(const psrp_value_t *v,
                                        psrp_buffer_t *out);

/** Renders a value the way a caller usually wants to see it: the scalar's text
 * for primitives, and ToString for a complex object (falling back to a
 * property named ToString, then to nothing). This is what turns a
 * PIPELINE_OUTPUT payload into the line a user sees. */
psrp_result_t psrp_value_to_text(const psrp_value_t *v, psrp_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_RECORDS_H */
