/* psrp_over_winrm.c - PSRP's use of a WS-Management session.
 *
 * The whole of the mapping between the two layers, and the only file that is
 * allowed to know both vocabularies. Everything PowerShell-specific about
 * carrying PSRP over WinRM is here:
 *
 *   - a RunspacePool id becomes the ShellId, and a pipeline id the CommandId,
 *     formatted the way the server expects;
 *   - a pipeline payload is a sequence of PSRP fragments, and 3.1.5.3.3 lets
 *     only the first ride in the Command request, so the payload is split on
 *     its first fragment boundary;
 *   - the Command element is empty, which is what 3.1.5.3.3 requires;
 *   - host responses travel on the "pr" stream and pipeline input on "stdin"
 *     (3.1.5.3.5).
 *
 * None of that is visible to winrm.h, which creates shells and moves bytes on
 * named streams and has no idea what is in them. The split in particular used
 * to happen down there, which meant the WinRM client parsing a PSRP fragment
 * header -- the lower layer reading the upper layer's wire format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_transport.h"
#include "psrp/psrp_fragment.h"

struct psrp_transport {
    winrm_session_t *s;
    char last_error[64];
};

/* WinRM matches identifiers case-sensitively and PowerShell sends them upper
 * case; psrp_guid_format produces lower. Getting this wrong creates the shell
 * happily and then reports "the shell was not found on the server" on the
 * next request, which reads as the shell having died. */
static psrp_result_t guid_upper(const psrp_guid_t *g, char *out, size_t cap)
{
    psrp_result_t rc = psrp_guid_format(g, out, cap);
    size_t i;
    if (rc != PSRP_OK) return rc;
    for (i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'f') out[i] = (char)(out[i] - 'a' + 'A');
    return PSRP_OK;
}

/* Where the first fragment ends. Decoding one fragment header is the only way
 * to know, and it is PSRP's format, so it is done here. */
static size_t first_fragment_len(const void *payload, size_t len)
{
    psrp_reader_t r;
    psrp_fragment_t f;
    psrp_reader_init(&r, payload, len);
    if (psrp_fragment_decode(&r, &f) != PSRP_OK) return 0;
    return r.pos;
}

psrp_result_t psrp_transport_over_winrm(const winrm_config_t *cfg,
                                        psrp_transport_t **out)
{
    psrp_transport_t *t;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    *out = NULL;

    t = (psrp_transport_t *)calloc(1, sizeof *t);
    if (!t) return PSRP_ERR_NOMEM;

    rc = winrm_session_open(cfg, &t->s);
    if (rc != PSRP_OK) {
        /* The session may exist even when opening failed, and it holds the
         * only description of why. */
        snprintf(t->last_error, sizeof t->last_error, "%s",
                 winrm_last_error(t->s));
        winrm_session_free(t->s);
        free(t);
        return rc;
    }

    *out = t;
    return PSRP_OK;
}

winrm_session_t *psrp_transport_session(psrp_transport_t *t)
{
    return t ? t->s : NULL;
}

void psrp_transport_free(psrp_transport_t *t)
{
    if (!t) return;
    winrm_session_free(t->s);
    free(t);
}

psrp_result_t psrp_transport_open(psrp_transport_t *t,
                                  const psrp_guid_t *pool_id,
                                  const void *payload, size_t len)
{
    char id[PSRP_GUID_BUF_SIZE];
    psrp_result_t rc;

    if (!t || !pool_id) return PSRP_ERR_INVALID_ARG;
    rc = guid_upper(pool_id, id, sizeof id);
    if (rc != PSRP_OK) return rc;

    /* PowerShell makes the ShellId the RunspacePool id so the two identifier
     * spaces line up. WinRM does not require that; PSRP chooses it. */
    return winrm_shell_create(t->s, id, payload, len);
}

psrp_result_t psrp_transport_run_command(psrp_transport_t *t,
                                         const psrp_guid_t *pipeline_id,
                                         const void *payload, size_t len)
{
    char id[PSRP_GUID_BUF_SIZE];
    size_t first;
    psrp_result_t rc;

    if (!t || !pipeline_id || !payload || len == 0) return PSRP_ERR_INVALID_ARG;
    rc = guid_upper(pipeline_id, id, sizeof id);
    if (rc != PSRP_OK) return rc;

    first = first_fragment_len(payload, len);
    if (first == 0) return PSRP_ERR_MALFORMED;

    /* An empty Command element is what 3.1.5.3.3 requires. The Win32 WSMan
     * client refuses to send one and substitutes a space (TODO PSRP-08); the
     * curl client sends it literally. Either way the choice is PSRP's, so it
     * is expressed here. */
    rc = winrm_command(t->s, id, "", payload, first);
    if (rc != PSRP_OK) return rc;

    if (len > first)
        rc = winrm_send(t->s, "stdin", (const uint8_t *)payload + first,
                        len - first);
    return rc;
}

psrp_result_t psrp_transport_send(psrp_transport_t *t, const void *data,
                                  size_t len)
{
    if (!t) return PSRP_ERR_INVALID_ARG;
    return winrm_send(t->s, "stdin", data, len);
}

psrp_result_t psrp_transport_send_priority(psrp_transport_t *t,
                                           const void *data, size_t len)
{
    if (!t) return PSRP_ERR_INVALID_ARG;
    /* 3.1.5.3.5 reserves "pr" for host responses, so that a reply the server
     * is waiting on cannot queue behind pipeline input. */
    return winrm_send(t->s, "pr", data, len);
}

psrp_result_t psrp_transport_receive(psrp_transport_t *t, psrp_buffer_t *out,
                                     uint32_t timeout_ms)
{
    if (!t) return PSRP_ERR_INVALID_ARG;
    return winrm_receive(t->s, out, timeout_ms);
}

psrp_result_t psrp_transport_stop_pipeline(psrp_transport_t *t)
{
    if (!t) return PSRP_ERR_INVALID_ARG;
    return winrm_signal(t->s, WINRM_SIGNAL_TERMINATE);
}

psrp_result_t psrp_transport_close_shell(psrp_transport_t *t)
{
    if (!t) return PSRP_ERR_INVALID_ARG;
    return winrm_shell_delete(t->s);
}

bool psrp_transport_command_done(const psrp_transport_t *t)
{
    return t && winrm_command_done(t->s);
}

const char *psrp_transport_last_error(const psrp_transport_t *t)
{
    if (!t) return "no transport";
    if (!t->s) return t->last_error[0] ? t->last_error : "no error";
    return winrm_last_error(t->s);
}
