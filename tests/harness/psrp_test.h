/* psrp_test.h - the in-repo unit test harness. No dependencies.
 *
 * A test file declares tests with PSRP_TEST, lists them in a table, and calls
 * PSRP_TEST_MAIN. Each executable is registered with CTest; failures print the
 * test name, file, line, and a diff for memory/string comparisons.
 *
 *   PSRP_TEST(buffer_append_grows) {
 *       psrp_buffer_t b; psrp_buffer_init(&b);
 *       ASSERT_OK(psrp_buffer_append_str(&b, "hi"));
 *       ASSERT_EQ_SZ(b.len, 2u);
 *       psrp_buffer_free(&b);
 *   }
 *
 *   static const psrp_test_case_t cases[] = {
 *       PSRP_TEST_CASE(buffer_append_grows),
 *   };
 *   PSRP_TEST_MAIN(cases)
 *
 * Run all tests, or one by name:  test_buffer buffer_append_grows
 * List them:                      test_buffer --list
 */
#ifndef PSRP_TEST_H
#define PSRP_TEST_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psrp_test_fn)(void);

typedef struct psrp_test_case {
    const char *name;
    psrp_test_fn fn;
} psrp_test_case_t;

/* Internal state used by the assertion macros. */
extern jmp_buf psrp_test_jmp_;

/* Records a failure for the running test, then longjmps out of it. */
void psrp_test_fail_(const char *file, int line, const char *fmt, ...);
/* Renders two byte ranges side by side when a memory comparison fails. */
void psrp_test_dump_mem_(const void *a, size_t alen, const void *b, size_t blen);

int psrp_test_run(const psrp_test_case_t *cases, size_t count, int argc, char **argv);

/* Turns on allocation tracking for the rest of the process; leaks are reported
 * to stderr at exit. psrp_test_run calls this itself. It is exposed because the
 * fuzzer has its own main and error paths are where leaks hide. A no-op unless
 * building with the MSVC debug CRT: AddressSanitizer on Windows has no leak
 * detector, so this is the only one available. */
void psrp_test_enable_leak_check(void);

#define PSRP_TEST(name) static void name(void)
#define PSRP_TEST_CASE(fn) { #fn, fn }

#define PSRP_TEST_MAIN(cases)                                                  \
    int main(int argc, char **argv)                                            \
    {                                                                          \
        return psrp_test_run((cases), sizeof(cases) / sizeof((cases)[0]),       \
                             argc, argv);                                      \
    }

#define PSRP_FAIL(...)                                                         \
    do {                                                                       \
        psrp_test_fail_(__FILE__, __LINE__, __VA_ARGS__);                      \
        longjmp(psrp_test_jmp_, 1);                                            \
    } while (0)

#define ASSERT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) PSRP_FAIL("expected true: %s", #cond);                    \
    } while (0)

#define ASSERT_FALSE(cond)                                                     \
    do {                                                                       \
        if ((cond)) PSRP_FAIL("expected false: %s", #cond);                    \
    } while (0)

#define ASSERT_NOT_NULL(p)                                                     \
    do {                                                                       \
        if ((p) == NULL) PSRP_FAIL("expected non-NULL: %s", #p);               \
    } while (0)

#define ASSERT_NULL(p)                                                         \
    do {                                                                       \
        if ((p) != NULL) PSRP_FAIL("expected NULL: %s", #p);                   \
    } while (0)

/* Signed integer comparison. */
#define ASSERT_EQ_I(a, b)                                                      \
    do {                                                                       \
        long long a_ = (long long)(a), b_ = (long long)(b);                    \
        if (a_ != b_)                                                          \
            PSRP_FAIL("%s == %s: got %lld, want %lld", #a, #b, a_, b_);        \
    } while (0)

/* Unsigned / size_t comparison. */
#define ASSERT_EQ_SZ(a, b)                                                     \
    do {                                                                       \
        unsigned long long a_ = (unsigned long long)(a);                       \
        unsigned long long b_ = (unsigned long long)(b);                       \
        if (a_ != b_)                                                          \
            PSRP_FAIL("%s == %s: got %llu, want %llu", #a, #b, a_, b_);        \
    } while (0)

/* psrp_result_t helpers: print the symbolic name, not just the number. */
#define ASSERT_OK(expr)                                                        \
    do {                                                                       \
        psrp_result_t rc_ = (expr);                                            \
        if (rc_ != PSRP_OK)                                                    \
            PSRP_FAIL("%s: expected ok, got %d (%s)", #expr, (int)rc_,         \
                      psrp_strerror(rc_));                                     \
    } while (0)

#define ASSERT_ERR(expr, want)                                                 \
    do {                                                                       \
        psrp_result_t rc_ = (expr);                                            \
        psrp_result_t want_ = (want);                                          \
        if (rc_ != want_)                                                      \
            PSRP_FAIL("%s: got %d (%s), want %d (%s)", #expr, (int)rc_,        \
                      psrp_strerror(rc_), (int)want_, psrp_strerror(want_));   \
    } while (0)

#define ASSERT_EQ_STR(a, b)                                                    \
    do {                                                                       \
        const char *a_ = (a), *b_ = (b);                                       \
        if (a_ == NULL || b_ == NULL || strcmp(a_, b_) != 0)                   \
            PSRP_FAIL("%s == %s:\n  got  \"%s\"\n  want \"%s\"", #a, #b,       \
                      a_ ? a_ : "(null)", b_ ? b_ : "(null)");                 \
    } while (0)

#define ASSERT_EQ_MEM(a, alen, b, blen)                                        \
    do {                                                                       \
        const void *a_ = (a); const void *b_ = (b);                            \
        size_t al_ = (size_t)(alen), bl_ = (size_t)(blen);                     \
        if (al_ != bl_ || (al_ && memcmp(a_, b_, al_) != 0)) {                 \
            psrp_test_dump_mem_(a_, al_, b_, bl_);                             \
            PSRP_FAIL("%s != %s (%zu vs %zu bytes)", #a, #b, al_, bl_);        \
        }                                                                      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* PSRP_TEST_H */
