/* Parsing shell enumeration results (3.1.4.10.1).
 *
 * The COM call itself needs a server, so the live test covers that. What is
 * testable here is the parsing, which is where the format assumptions live.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_transport.h"
#include "psrp_test.h"

/* One item as WinRM returns it: namespaced, with the shell namespace bound to
 * the customary rsp prefix. */
static const char kShellXml[] =
    "<rsp:Shell xmlns:rsp=\"http://schemas.microsoft.com/wbem/wsman/1/"
    "windows/shell\">"
    "<rsp:ShellId>4358D2A2-8A0B-4B5D-9C43-1D2E3F405162</rsp:ShellId>"
    "<rsp:Name>Runspace1</rsp:Name>"
    "<rsp:ResourceUri>http://schemas.microsoft.com/powershell/Microsoft."
    "PowerShell</rsp:ResourceUri>"
    "<rsp:Owner>CLAUDE\\Administrator</rsp:Owner>"
    "<rsp:ClientIP>127.0.0.1</rsp:ClientIP>"
    "<rsp:ProcessId>4242</rsp:ProcessId>"
    "<rsp:State>Disconnected</rsp:State>"
    "<rsp:ShellRunTime>PT1M30S</rsp:ShellRunTime>"
    "</rsp:Shell>";

PSRP_TEST(parses_a_shell_element)
{
    winrm_shell_info_t s;

    ASSERT_OK(winrm_parse_shell(kShellXml, sizeof kShellXml - 1, &s));
    /* Verbatim, in the case the server sent it. Round-tripping it through
     * a GUID used to normalise the case; an opaque identifier should come
     * back as it arrived. */
    ASSERT_EQ_STR(s.shell_id, "4358D2A2-8A0B-4B5D-9C43-1D2E3F405162");
    ASSERT_EQ_STR(s.name, "Runspace1");
    ASSERT_EQ_STR(s.state, "Disconnected");
    ASSERT_EQ_STR(s.owner, "CLAUDE\\Administrator");
    ASSERT_EQ_STR(s.resource_uri,
                  "http://schemas.microsoft.com/powershell/Microsoft.PowerShell");
    winrm_shell_info_free(&s);
}

PSRP_TEST(the_namespace_prefix_does_not_matter)
{
    /* A server may bind the shell namespace to any prefix, so the parser works
     * on local names. Keying on "rsp:ShellId" would break against a server
     * that spelled it differently. */
    static const char xml[] =
        "<w:Shell xmlns:w=\"http://schemas.microsoft.com/wbem/wsman/1/"
        "windows/shell\">"
        "<w:ShellId>4358D2A2-8A0B-4B5D-9C43-1D2E3F405162</w:ShellId>"
        "<w:State>Connected</w:State></w:Shell>";
    winrm_shell_info_t s;

    ASSERT_OK(winrm_parse_shell(xml, sizeof xml - 1, &s));
    ASSERT_EQ_STR(s.state, "Connected");
    winrm_shell_info_free(&s);
}

PSRP_TEST(a_braced_or_padded_shell_id_is_accepted)
{
    /* Nothing forbids a server from wrapping the id in braces or padding it,
     * and refusing those would drop a pool that is perfectly connectable. */
    static const char braced[] =
        "<Shell><ShellId>{4358D2A2-8A0B-4B5D-9C43-1D2E3F405162}</ShellId>"
        "</Shell>";
    static const char padded[] =
        "<Shell><ShellId>  4358D2A2-8A0B-4B5D-9C43-1D2E3F405162  </ShellId>"
        "</Shell>";
    winrm_shell_info_t a, b;

    ASSERT_OK(winrm_parse_shell(braced, sizeof braced - 1, &a));
    ASSERT_OK(winrm_parse_shell(padded, sizeof padded - 1, &b));
    ASSERT_EQ_STR(a.shell_id, b.shell_id);
    winrm_shell_info_free(&a);
    winrm_shell_info_free(&b);
}

PSRP_TEST(a_shell_without_an_id_is_rejected)
{
    /* Reporting it would hand the caller an entry it cannot connect to. */
    static const char xml[] =
        "<Shell><Name>nameless</Name><State>Connected</State></Shell>";
    winrm_shell_info_t s;

    ASSERT_ERR(winrm_parse_shell(xml, sizeof xml - 1, &s),
               PSRP_ERR_MALFORMED);
    ASSERT_NULL(s.name);      /* nothing is left owned on the failure path */
}

/* A WS-Management ShellId is an opaque string, so a non-GUID one is valid
 * here. It used to be rejected, back when this parser produced a GUID -- but
 * that was PSRP's requirement wearing WinRM's clothes. A caller that means to
 * adopt the pool converts the string itself and finds out then; a caller
 * merely listing shells has no reason to care. What is still rejected is a
 * shell with no id at all, which cannot be addressed by anyone. */
PSRP_TEST(a_non_guid_id_is_a_valid_shell_id)
{
    static const char xml[] =
        "<Shell><ShellId>not-a-guid</ShellId></Shell>";
    winrm_shell_info_t s;

    ASSERT_OK(winrm_parse_shell(xml, sizeof xml - 1, &s));
    ASSERT_EQ_STR(s.shell_id, "not-a-guid");
    winrm_shell_info_free(&s);
}

PSRP_TEST(rejects_bad_arguments_and_junk)
{
    winrm_shell_info_t s;

    ASSERT_ERR(winrm_parse_shell(NULL, 0, &s), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(winrm_parse_shell("<Shell/>", 8, NULL),
               PSRP_ERR_INVALID_ARG);
    /* Not XML at all: rejected, never read as an empty shell. */
    ASSERT_TRUE(winrm_parse_shell("<<<<", 4, &s) != PSRP_OK);
}

PSRP_TEST(freeing_is_idempotent)
{
    winrm_shell_info_t s;

    ASSERT_OK(winrm_parse_shell(kShellXml, sizeof kShellXml - 1, &s));
    winrm_shell_info_free(&s);
    winrm_shell_info_free(&s);
    winrm_shell_info_free(NULL);
    ASSERT_NULL(s.name);

    winrm_shell_info_free_all(NULL, 0);
}

PSRP_TEST(enumerate_rejects_bad_arguments)
{
    winrm_config_t cfg;
    winrm_shell_info_t *list = NULL;
    size_t count = 99;

    memset(&cfg, 0, sizeof cfg);
    ASSERT_ERR(winrm_enumerate_shells(NULL, &list, &count),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(winrm_enumerate_shells(&cfg, NULL, &count),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(winrm_enumerate_shells(&cfg, &list, NULL),
               PSRP_ERR_INVALID_ARG);
}

PSRP_TEST(discovery_rejects_bad_arguments)
{
    winrm_config_t cfg;
    winrm_enumerator_t *d = NULL;
    winrm_shell_info_t *list = NULL;
    size_t count = 99;

    memset(&cfg, 0, sizeof cfg);
    ASSERT_ERR(winrm_enumerator_open(NULL, &d), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(winrm_enumerator_open(&cfg, NULL), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(winrm_enumerator_shells(NULL, &list, &count),
               PSRP_ERR_INVALID_ARG);
    /* Freeing nothing is allowed, so a caller can clean up unconditionally. */
    winrm_enumerator_free(NULL);
}

PSRP_TEST(a_discovery_handle_can_list_repeatedly)
{
    /* The reason this API exists: each discarded WSMan session leaves a
     * WinHTTP connection Event behind for about a minute (TODO PSRP-14), so
     * listing in a loop through one-shot calls holds handles it need not.
     * Measured: 100 one-shot calls held ~112 handles, 100 through one handle
     * held 1.
     *
     * Enumeration needs a live WinRM service, so this only asserts the shape
     * when the machine has one; on a machine without, opening fails and there
     * is nothing to check. */
    winrm_config_t cfg;
    winrm_enumerator_t *d = NULL;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.operation_timeout_ms = 30000;

    if (winrm_enumerator_open(&cfg, &d) != PSRP_OK) return;
    ASSERT_NOT_NULL(d);

    for (i = 0; i < 3; i++) {
        winrm_shell_info_t *list = NULL;
        size_t count = 12345;
        ASSERT_OK(winrm_enumerator_shells(d, &list, &count));
        /* Count is always set, even when the server reports nothing. */
        ASSERT_TRUE(count != 12345);
        if (count == 0) ASSERT_NULL(list);
        winrm_shell_info_free_all(list, count);
    }

    winrm_enumerator_free(d);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(parses_a_shell_element),
    PSRP_TEST_CASE(the_namespace_prefix_does_not_matter),
    PSRP_TEST_CASE(a_braced_or_padded_shell_id_is_accepted),
    PSRP_TEST_CASE(a_shell_without_an_id_is_rejected),
    PSRP_TEST_CASE(a_non_guid_id_is_a_valid_shell_id),
    PSRP_TEST_CASE(rejects_bad_arguments_and_junk),
    PSRP_TEST_CASE(freeing_is_idempotent),
    PSRP_TEST_CASE(enumerate_rejects_bad_arguments),
    PSRP_TEST_CASE(discovery_rejects_bad_arguments),
    PSRP_TEST_CASE(a_discovery_handle_can_list_repeatedly),
};

PSRP_TEST_MAIN(cases)
