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
    psrp_shell_info_t s;
    char id[PSRP_GUID_STR_LEN + 1];

    ASSERT_OK(psrp_wsman_parse_shell(kShellXml, sizeof kShellXml - 1, &s));
    ASSERT_OK(psrp_guid_format(&s.shell_id, id, sizeof id));
    ASSERT_EQ_STR(id, "4358d2a2-8a0b-4b5d-9c43-1d2e3f405162");
    ASSERT_EQ_STR(s.name, "Runspace1");
    ASSERT_EQ_STR(s.state, "Disconnected");
    ASSERT_EQ_STR(s.owner, "CLAUDE\\Administrator");
    ASSERT_EQ_STR(s.resource_uri,
                  "http://schemas.microsoft.com/powershell/Microsoft.PowerShell");
    psrp_shell_info_free(&s);
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
    psrp_shell_info_t s;

    ASSERT_OK(psrp_wsman_parse_shell(xml, sizeof xml - 1, &s));
    ASSERT_EQ_STR(s.state, "Connected");
    psrp_shell_info_free(&s);
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
    psrp_shell_info_t a, b;

    ASSERT_OK(psrp_wsman_parse_shell(braced, sizeof braced - 1, &a));
    ASSERT_OK(psrp_wsman_parse_shell(padded, sizeof padded - 1, &b));
    ASSERT_TRUE(psrp_guid_equal(&a.shell_id, &b.shell_id));
    psrp_shell_info_free(&a);
    psrp_shell_info_free(&b);
}

PSRP_TEST(a_shell_without_an_id_is_rejected)
{
    /* Reporting it would hand the caller an entry it cannot connect to. */
    static const char xml[] =
        "<Shell><Name>nameless</Name><State>Connected</State></Shell>";
    psrp_shell_info_t s;

    ASSERT_ERR(psrp_wsman_parse_shell(xml, sizeof xml - 1, &s),
               PSRP_ERR_MALFORMED);
    ASSERT_NULL(s.name);      /* nothing is left owned on the failure path */
}

PSRP_TEST(a_malformed_id_is_rejected)
{
    static const char xml[] =
        "<Shell><ShellId>not-a-guid</ShellId></Shell>";
    psrp_shell_info_t s;

    ASSERT_ERR(psrp_wsman_parse_shell(xml, sizeof xml - 1, &s),
               PSRP_ERR_MALFORMED);
}

PSRP_TEST(rejects_bad_arguments_and_junk)
{
    psrp_shell_info_t s;

    ASSERT_ERR(psrp_wsman_parse_shell(NULL, 0, &s), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_wsman_parse_shell("<Shell/>", 8, NULL),
               PSRP_ERR_INVALID_ARG);
    /* Not XML at all: rejected, never read as an empty shell. */
    ASSERT_TRUE(psrp_wsman_parse_shell("<<<<", 4, &s) != PSRP_OK);
}

PSRP_TEST(freeing_is_idempotent)
{
    psrp_shell_info_t s;

    ASSERT_OK(psrp_wsman_parse_shell(kShellXml, sizeof kShellXml - 1, &s));
    psrp_shell_info_free(&s);
    psrp_shell_info_free(&s);
    psrp_shell_info_free(NULL);
    ASSERT_NULL(s.name);

    psrp_shell_info_free_all(NULL, 0);
}

PSRP_TEST(enumerate_rejects_bad_arguments)
{
    psrp_wsman_config_t cfg;
    psrp_shell_info_t *list = NULL;
    size_t count = 99;

    memset(&cfg, 0, sizeof cfg);
    ASSERT_ERR(psrp_wsman_enumerate_shells(NULL, &list, &count),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_wsman_enumerate_shells(&cfg, NULL, &count),
               PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_wsman_enumerate_shells(&cfg, &list, NULL),
               PSRP_ERR_INVALID_ARG);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(parses_a_shell_element),
    PSRP_TEST_CASE(the_namespace_prefix_does_not_matter),
    PSRP_TEST_CASE(a_braced_or_padded_shell_id_is_accepted),
    PSRP_TEST_CASE(a_shell_without_an_id_is_rejected),
    PSRP_TEST_CASE(a_malformed_id_is_rejected),
    PSRP_TEST_CASE(rejects_bad_arguments_and_junk),
    PSRP_TEST_CASE(freeing_is_idempotent),
    PSRP_TEST_CASE(enumerate_rejects_bad_arguments),
};

PSRP_TEST_MAIN(cases)
