#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp_test.h"

/* ---------------------------------------------------------------- writer -- */

PSRP_TEST(buffer_zeroed_is_valid)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_NULL(b.data);
    ASSERT_EQ_SZ(b.len, 0u);
    ASSERT_EQ_SZ(b.cap, 0u);
    psrp_buffer_free(&b);      /* free of empty must be safe */
    psrp_buffer_free(&b);      /* and idempotent */
}

PSRP_TEST(buffer_append_and_grow)
{
    psrp_buffer_t b;
    size_t i;
    psrp_buffer_init(&b);
    for (i = 0; i < 1000; i++)
        ASSERT_OK(psrp_buffer_append_u8(&b, (uint8_t)(i & 0xFF)));
    ASSERT_EQ_SZ(b.len, 1000u);
    ASSERT_TRUE(b.cap >= b.len);
    for (i = 0; i < 1000; i++)
        ASSERT_EQ_I(b.data[i], (uint8_t)(i & 0xFF));
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_append_zero_length_is_noop)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append(&b, NULL, 0));  /* NULL ok when n == 0 */
    ASSERT_EQ_SZ(b.len, 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_big_endian_integers)
{
    psrp_buffer_t b;
    static const uint8_t want[] = {
        0xAB,                                            /* u8   */
        0x12, 0x34,                                      /* u16  */
        0xDE, 0xAD, 0xBE, 0xEF,                          /* u32  */
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF   /* u64  */
    };
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_u8(&b, 0xAB));
    ASSERT_OK(psrp_buffer_append_u16be(&b, 0x1234));
    ASSERT_OK(psrp_buffer_append_u32be(&b, 0xDEADBEEFu));
    ASSERT_OK(psrp_buffer_append_u64be(&b, 0x0123456789ABCDEFull));
    ASSERT_EQ_MEM(b.data, b.len, want, sizeof want);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_little_endian_u32)
{
    psrp_buffer_t b;
    static const uint8_t want[] = { 0xEF, 0xBE, 0xAD, 0xDE };
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_u32le(&b, 0xDEADBEEFu));
    ASSERT_EQ_MEM(b.data, b.len, want, sizeof want);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_append_str_excludes_terminator)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_str(&b, "abc"));
    ASSERT_EQ_SZ(b.len, 3u);
    ASSERT_EQ_MEM(b.data, b.len, "abc", 3u);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_reset_keeps_capacity)
{
    psrp_buffer_t b;
    size_t cap;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_str(&b, "hello world"));
    cap = b.cap;
    psrp_buffer_reset(&b);
    ASSERT_EQ_SZ(b.len, 0u);
    ASSERT_EQ_SZ(b.cap, cap);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_consume_front)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_str(&b, "0123456789"));
    ASSERT_OK(psrp_buffer_consume(&b, 4));
    ASSERT_EQ_SZ(b.len, 6u);
    ASSERT_EQ_MEM(b.data, b.len, "456789", 6u);

    ASSERT_OK(psrp_buffer_consume(&b, 0));      /* no-op */
    ASSERT_EQ_SZ(b.len, 6u);

    ASSERT_OK(psrp_buffer_consume(&b, 6));      /* exact */
    ASSERT_EQ_SZ(b.len, 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_consume_past_end_rejected)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_str(&b, "abc"));
    ASSERT_ERR(psrp_buffer_consume(&b, 4), PSRP_ERR_INVALID_ARG);
    ASSERT_EQ_SZ(b.len, 3u);   /* unchanged on failure */
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_detach_transfers_ownership)
{
    psrp_buffer_t b;
    uint8_t *p;
    size_t n = 0;
    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_str(&b, "xyz"));
    p = psrp_buffer_detach(&b, &n);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_SZ(n, 3u);
    ASSERT_EQ_MEM(p, n, "xyz", 3u);
    ASSERT_NULL(b.data);
    ASSERT_EQ_SZ(b.len, 0u);
    free(p);
    psrp_buffer_free(&b);
}

PSRP_TEST(buffer_rejects_null_and_overflow)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_ERR(psrp_buffer_append(NULL, "a", 1), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_buffer_append(&b, NULL, 1), PSRP_ERR_INVALID_ARG);
    ASSERT_ERR(psrp_buffer_append_str(&b, NULL), PSRP_ERR_INVALID_ARG);
    /* A reservation beyond the internal cap must fail, not wrap. */
    ASSERT_ERR(psrp_buffer_reserve(&b, (size_t)-1), PSRP_ERR_OVERFLOW);
    psrp_buffer_free(&b);
}

/* ---------------------------------------------------------------- reader -- */

PSRP_TEST(reader_reads_big_endian)
{
    static const uint8_t src[] = {
        0xAB, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
    };
    psrp_reader_t r;
    uint8_t v8; uint16_t v16; uint32_t v32; uint64_t v64;

    psrp_reader_init(&r, src, sizeof src);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), sizeof src);
    ASSERT_OK(psrp_read_u8(&r, &v8));      ASSERT_EQ_I(v8, 0xAB);
    ASSERT_OK(psrp_read_u16be(&r, &v16));  ASSERT_EQ_I(v16, 0x1234);
    ASSERT_OK(psrp_read_u32be(&r, &v32));  ASSERT_EQ_SZ(v32, 0xDEADBEEFu);
    ASSERT_OK(psrp_read_u64be(&r, &v64));  ASSERT_EQ_SZ(v64, 0x0123456789ABCDEFull);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
}

PSRP_TEST(reader_reads_little_endian_u32)
{
    static const uint8_t src[] = { 0xEF, 0xBE, 0xAD, 0xDE };
    psrp_reader_t r;
    uint32_t v = 0;
    psrp_reader_init(&r, src, sizeof src);
    ASSERT_OK(psrp_read_u32le(&r, &v));
    ASSERT_EQ_SZ(v, 0xDEADBEEFu);
}

PSRP_TEST(reader_truncation_is_distinct_from_malformed)
{
    static const uint8_t src[] = { 0x01, 0x02, 0x03 };
    psrp_reader_t r;
    uint32_t v32; uint64_t v64; uint16_t v16;

    psrp_reader_init(&r, src, sizeof src);
    ASSERT_ERR(psrp_read_u32be(&r, &v32), PSRP_ERR_TRUNCATED);
    ASSERT_ERR(psrp_read_u64be(&r, &v64), PSRP_ERR_TRUNCATED);
    /* A failed read must not consume anything. */
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 3u);
    ASSERT_OK(psrp_read_u16be(&r, &v16));
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 1u);
}

PSRP_TEST(reader_bytes_borrow_and_skip)
{
    static const uint8_t src[] = "abcdefgh";
    psrp_reader_t r;
    uint8_t tmp[4];
    const uint8_t *p = NULL;

    psrp_reader_init(&r, src, 8);
    ASSERT_OK(psrp_read_bytes(&r, tmp, 4));
    ASSERT_EQ_MEM(tmp, 4u, "abcd", 4u);

    ASSERT_OK(psrp_read_borrow(&r, 2, &p));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_MEM(p, 2u, "ef", 2u);

    ASSERT_OK(psrp_reader_skip(&r, 2));
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    ASSERT_ERR(psrp_reader_skip(&r, 1), PSRP_ERR_TRUNCATED);
}

/* An empty reader is valid and simply has nothing yet: reads must report
 * TRUNCATED so a streaming decoder can retry, not INVALID_ARG. */
PSRP_TEST(reader_empty_input)
{
    psrp_reader_t r;
    uint8_t v;
    psrp_reader_init(&r, NULL, 0);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    ASSERT_ERR(psrp_read_u8(&r, &v), PSRP_ERR_TRUNCATED);

    /* Same for a non-NULL but fully consumed buffer. */
    psrp_reader_init(&r, "a", 1);
    ASSERT_OK(psrp_read_u8(&r, &v));
    ASSERT_ERR(psrp_read_u8(&r, &v), PSRP_ERR_TRUNCATED);

    /* A NULL reader is still a caller error. */
    ASSERT_ERR(psrp_read_u8(NULL, &v), PSRP_ERR_INVALID_ARG);
}

PSRP_TEST(reader_zero_byte_read_ok)
{
    static const uint8_t src[] = { 1 };
    psrp_reader_t r;
    psrp_reader_init(&r, src, 1);
    ASSERT_OK(psrp_read_bytes(&r, NULL, 0));
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 1u);
}

/* Writer then reader must round-trip every integer width. */
PSRP_TEST(buffer_reader_roundtrip)
{
    psrp_buffer_t b;
    psrp_reader_t r;
    uint8_t a8; uint16_t a16; uint32_t a32, ale; uint64_t a64;

    psrp_buffer_init(&b);
    ASSERT_OK(psrp_buffer_append_u8(&b, 0x7F));
    ASSERT_OK(psrp_buffer_append_u16be(&b, 0xFFFE));
    ASSERT_OK(psrp_buffer_append_u32be(&b, 0x80000001u));
    ASSERT_OK(psrp_buffer_append_u64be(&b, 0xFFFFFFFFFFFFFFFFull));
    ASSERT_OK(psrp_buffer_append_u32le(&b, 0x0A0B0C0Du));

    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_read_u8(&r, &a8));     ASSERT_EQ_I(a8, 0x7F);
    ASSERT_OK(psrp_read_u16be(&r, &a16)); ASSERT_EQ_I(a16, 0xFFFE);
    ASSERT_OK(psrp_read_u32be(&r, &a32)); ASSERT_EQ_SZ(a32, 0x80000001u);
    ASSERT_OK(psrp_read_u64be(&r, &a64)); ASSERT_EQ_SZ(a64, 0xFFFFFFFFFFFFFFFFull);
    ASSERT_OK(psrp_read_u32le(&r, &ale)); ASSERT_EQ_SZ(ale, 0x0A0B0C0Du);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    psrp_buffer_free(&b);
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(buffer_zeroed_is_valid),
    PSRP_TEST_CASE(buffer_append_and_grow),
    PSRP_TEST_CASE(buffer_append_zero_length_is_noop),
    PSRP_TEST_CASE(buffer_big_endian_integers),
    PSRP_TEST_CASE(buffer_little_endian_u32),
    PSRP_TEST_CASE(buffer_append_str_excludes_terminator),
    PSRP_TEST_CASE(buffer_reset_keeps_capacity),
    PSRP_TEST_CASE(buffer_consume_front),
    PSRP_TEST_CASE(buffer_consume_past_end_rejected),
    PSRP_TEST_CASE(buffer_detach_transfers_ownership),
    PSRP_TEST_CASE(buffer_rejects_null_and_overflow),
    PSRP_TEST_CASE(reader_reads_big_endian),
    PSRP_TEST_CASE(reader_reads_little_endian_u32),
    PSRP_TEST_CASE(reader_truncation_is_distinct_from_malformed),
    PSRP_TEST_CASE(reader_bytes_borrow_and_skip),
    PSRP_TEST_CASE(reader_empty_input),
    PSRP_TEST_CASE(reader_zero_byte_read_ok),
    PSRP_TEST_CASE(buffer_reader_roundtrip),
};

PSRP_TEST_MAIN(cases)
