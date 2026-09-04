#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psrp_test.h"

/* Leak checking.
 *
 * AddressSanitizer on Windows has no leak detector: LeakSanitizer is not
 * supported there, so an ASan build catches out-of-bounds accesses but would
 * miss a parser that forgets to free an object graph on an error path, which
 * is exactly the kind of bug this library is prone to. The MSVC debug CRT does
 * track allocations, so use that instead.
 *
 * The check runs once at exit over the whole executable rather than per test,
 * because a per-test snapshot would flag anything the CRT itself allocates
 * lazily on first use. A leaking test therefore fails the executable, and the
 * report names the allocation.
 */
#if defined(_MSC_VER) && defined(_DEBUG)
#  define PSRP_TEST_LEAK_CHECK 1
#  define _CRTDBG_MAP_ALLOC
#  include <crtdbg.h>
#  include <stdlib.h>

void psrp_test_enable_leak_check(void)
{
    int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flags |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF;
    _CrtSetDbgFlag(flags);
    /* Send the report to stderr instead of a message box, which would hang a
     * test run with nobody there to dismiss it. */
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}
#else
#  define PSRP_TEST_LEAK_CHECK 0
void psrp_test_enable_leak_check(void) { }
#endif

jmp_buf psrp_test_jmp_;

static int failed_current_;
static char fail_msg_[1024];

void psrp_test_fail_(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    int n;

    failed_current_ = 1;
    n = snprintf(fail_msg_, sizeof fail_msg_, "%s:%d: ", file, line);
    if (n < 0 || (size_t)n >= sizeof fail_msg_) return;

    va_start(ap, fmt);
    vsnprintf(fail_msg_ + n, sizeof fail_msg_ - (size_t)n, fmt, ap);
    va_end(ap);
}

static void dump_one(const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    printf("      %s (%zu bytes):", label, n);
    for (i = 0; i < n && i < 64; i++) {
        if (i % 16 == 0) printf("\n        ");
        printf("%02x ", p[i]);
    }
    if (n > 64) printf("\n        ... (%zu more)", n - 64);
    printf("\n");
}

void psrp_test_dump_mem_(const void *a, size_t alen, const void *b, size_t blen)
{
    dump_one("got ", (const uint8_t *)a, alen);
    dump_one("want", (const uint8_t *)b, blen);
}

static int run_one(const psrp_test_case_t *tc)
{
    failed_current_ = 0;
    fail_msg_[0] = '\0';

    if (setjmp(psrp_test_jmp_) == 0) {
        tc->fn();
    }

    if (failed_current_) {
        printf("  FAIL %s\n    %s\n", tc->name, fail_msg_);
        return 1;
    }
    printf("  ok   %s\n", tc->name);
    return 0;
}

int psrp_test_run(const psrp_test_case_t *cases, size_t count, int argc, char **argv)
{
    size_t i;
    int failures = 0;
    size_t ran = 0;

    psrp_test_enable_leak_check();

    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (i = 0; i < count; i++) printf("%s\n", cases[i].name);
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        failures += run_one(&cases[i]);
        ran++;
    }

    if (argc > 1 && ran == 0) {
        printf("no such test: %s\n", argv[1]);
        return 2;
    }

    printf("%zu/%zu passed%s\n", ran - (size_t)failures, ran,
           PSRP_TEST_LEAK_CHECK ? " (leak-checked)" : "");
    fflush(stdout);
    return failures ? 1 : 0;
}
