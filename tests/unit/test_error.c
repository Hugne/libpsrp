#include <string.h>

#include "psrp/psrp.h"
#include "psrp_test.h"

static const psrp_result_t kAllCodes[] = {
    PSRP_OK,
    PSRP_ERR_INVALID_ARG, PSRP_ERR_NOMEM, PSRP_ERR_TOO_SMALL,
    PSRP_ERR_STATE, PSRP_ERR_NOT_FOUND, PSRP_ERR_MALFORMED,
    PSRP_ERR_TRUNCATED, PSRP_ERR_OVERFLOW, PSRP_ERR_UNSUPPORTED,
    PSRP_ERR_TRANSPORT, PSRP_ERR_CRYPTO, PSRP_ERR_XML, PSRP_ERR_INTERNAL
};
#define N_CODES (sizeof kAllCodes / sizeof kAllCodes[0])

PSRP_TEST(strerror_never_null_and_nonempty)
{
    size_t i;
    for (i = 0; i < N_CODES; i++) {
        const char *s = psrp_strerror(kAllCodes[i]);
        ASSERT_NOT_NULL(s);
        ASSERT_TRUE(strlen(s) > 0);
    }
}

/* A duplicated string would make a failure message ambiguous. */
PSRP_TEST(strerror_messages_are_distinct)
{
    size_t i, j;
    for (i = 0; i < N_CODES; i++)
        for (j = i + 1; j < N_CODES; j++)
            if (strcmp(psrp_strerror(kAllCodes[i]),
                       psrp_strerror(kAllCodes[j])) == 0)
                PSRP_FAIL("codes %d and %d share the message \"%s\"",
                          (int)kAllCodes[i], (int)kAllCodes[j],
                          psrp_strerror(kAllCodes[i]));
}

PSRP_TEST(strerror_handles_unknown_code)
{
    const char *s = psrp_strerror((psrp_result_t)-12345);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR(s, "unknown error");
}

PSRP_TEST(ok_is_zero_and_errors_are_negative)
{
    size_t i;
    ASSERT_EQ_I(PSRP_OK, 0);
    for (i = 0; i < N_CODES; i++)
        if (kAllCodes[i] != PSRP_OK)
            ASSERT_TRUE(kAllCodes[i] < 0);
}

PSRP_TEST(version_string_present)
{
    const char *v = psrp_version_string();
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(strlen(v) > 0);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(strerror_never_null_and_nonempty),
    PSRP_TEST_CASE(strerror_messages_are_distinct),
    PSRP_TEST_CASE(strerror_handles_unknown_code),
    PSRP_TEST_CASE(ok_is_zero_and_errors_are_negative),
    PSRP_TEST_CASE(version_string_present),
};

PSRP_TEST_MAIN(cases)
