/* Shell records from a WS-Management enumeration, and their parsing.
 *
 * Split out of the enumeration backends because it is the half that has
 * nothing to do with either: whether the XML arrived through the WSMan COM
 * automation interface or over libcurl, a shell record is the same record.
 *
 * What a caller does with the shells it finds is its own business. PowerShell
 * puts a RunspacePool id in the ShellId, but that is PSRP's convention and
 * nothing here relies on it: an identifier is an opaque string.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/winrm.h"
#include "internal/psrp_xml.h"

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
