#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psrp_test.h"

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

    printf("%zu/%zu passed\n", ran - (size_t)failures, ran);
    fflush(stdout);
    return failures ? 1 : 0;
}
