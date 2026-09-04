#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_fragment.h"
#include "psrp_test.h"

/* ------------------------------------------------------- wire layout ----- */

/* Byte-exact check against [MS-PSRP] 2.2.4: ObjectId(8) FragmentId(8)
 * flags(1) BlobLength(4) Blob. All integers network byte order. */
PSRP_TEST(fragment_encode_wire_layout)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    static const uint8_t blob[] = { 0xAA, 0xBB, 0xCC };
    static const uint8_t want[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,  /* ObjectId   = 1 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* FragmentId = 0 */
        0x03,                                            /* S|E            */
        0x00, 0x00, 0x00, 0x03,                          /* BlobLength = 3 */
        0xAA, 0xBB, 0xCC
    };

    psrp_buffer_init(&b);
    f.object_id = 1; f.fragment_id = 0;
    f.start = true; f.end = true;
    f.blob = blob; f.blob_len = 3;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_EQ_SZ(b.len, PSRP_FRAGMENT_HEADER_SIZE + 3u);
    ASSERT_EQ_MEM(b.data, b.len, want, sizeof want);
    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_flag_bit_values)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    /* S is the low bit (0x01), E the next (0x02). */
    psrp_buffer_init(&b);
    f.object_id = 7; f.fragment_id = 0; f.blob = NULL; f.blob_len = 0;

    f.start = true;  f.end = false;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_EQ_I(b.data[16], 0x01);
    psrp_buffer_reset(&b);

    f.start = false; f.end = true; f.fragment_id = 1;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_EQ_I(b.data[16], 0x02);
    psrp_buffer_reset(&b);

    f.start = false; f.end = false;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_EQ_I(b.data[16], 0x00);
    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_encode_decode_roundtrip)
{
    psrp_buffer_t b;
    psrp_fragment_t f, g;
    psrp_reader_t r;
    static const uint8_t blob[] = "hello fragment";

    psrp_buffer_init(&b);
    f.object_id = 0x0123456789ABCDEFull;
    f.fragment_id = 0x00FF00FF00FF00FFull;
    f.start = false; f.end = true;
    f.blob = blob; f.blob_len = 14;
    ASSERT_OK(psrp_fragment_encode(&b, &f));

    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    ASSERT_EQ_SZ(g.object_id, f.object_id);
    ASSERT_EQ_SZ(g.fragment_id, f.fragment_id);
    ASSERT_FALSE(g.start);
    ASSERT_TRUE(g.end);
    ASSERT_EQ_MEM(g.blob, g.blob_len, blob, 14u);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_empty_blob_roundtrip)
{
    psrp_buffer_t b;
    psrp_fragment_t f, g;
    psrp_reader_t r;

    psrp_buffer_init(&b);
    f.object_id = 5; f.fragment_id = 0;
    f.start = true; f.end = true;
    f.blob = NULL; f.blob_len = 0;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_EQ_SZ(b.len, PSRP_FRAGMENT_HEADER_SIZE);

    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    ASSERT_EQ_SZ(g.blob_len, 0u);
    ASSERT_TRUE(g.start && g.end);
    psrp_buffer_free(&b);
}

/* "Reserved (6 bits): MUST be set to 0 and ignored upon receipt." */
PSRP_TEST(fragment_decode_ignores_reserved_bits)
{
    psrp_buffer_t b;
    psrp_fragment_t f, g;
    psrp_reader_t r;

    psrp_buffer_init(&b);
    f.object_id = 9; f.fragment_id = 0; f.start = true; f.end = true;
    f.blob = NULL; f.blob_len = 0;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    b.data[16] = (uint8_t)(b.data[16] | 0xFC);   /* set all reserved bits */

    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    ASSERT_TRUE(g.start);
    ASSERT_TRUE(g.end);
    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_encode_rejects_invalid)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    static const uint8_t blob[4] = { 0 };

    psrp_buffer_init(&b);
    f.object_id = 0; f.fragment_id = 0; f.start = true; f.end = true;
    f.blob = blob; f.blob_len = 1;
    /* 2.2.4: ObjectId MUST be > 0. */
    ASSERT_ERR(psrp_fragment_encode(&b, &f), PSRP_ERR_INVALID_ARG);

    /* Start fragment must carry FragmentId 0. */
    f.object_id = 1; f.fragment_id = 3; f.start = true;
    ASSERT_ERR(psrp_fragment_encode(&b, &f), PSRP_ERR_INVALID_ARG);

    /* Blob cap. */
    f.fragment_id = 0; f.start = true;
    f.blob_len = PSRP_FRAGMENT_MAX_BLOB + 1;
    ASSERT_ERR(psrp_fragment_encode(&b, &f), PSRP_ERR_OVERFLOW);

    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_decode_truncated_leaves_reader_untouched)
{
    psrp_buffer_t b;
    psrp_fragment_t f, g;
    psrp_reader_t r;
    static const uint8_t blob[] = { 1, 2, 3, 4, 5 };
    size_t i;

    psrp_buffer_init(&b);
    f.object_id = 2; f.fragment_id = 0; f.start = true; f.end = true;
    f.blob = blob; f.blob_len = 5;
    ASSERT_OK(psrp_fragment_encode(&b, &f));

    /* Every prefix shorter than the whole fragment must report TRUNCATED and
     * consume nothing, so a streaming caller can simply retry. */
    for (i = 0; i < b.len; i++) {
        psrp_reader_init(&r, b.data, i);
        ASSERT_ERR(psrp_fragment_decode(&r, &g), PSRP_ERR_TRUNCATED);
        ASSERT_EQ_SZ(r.pos, 0u);
    }
    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    psrp_buffer_free(&b);
}

PSRP_TEST(fragment_decode_rejects_malformed)
{
    psrp_buffer_t b;
    psrp_fragment_t f, g;
    psrp_reader_t r;

    psrp_buffer_init(&b);
    f.object_id = 1; f.fragment_id = 0; f.start = true; f.end = true;
    f.blob = NULL; f.blob_len = 0;
    ASSERT_OK(psrp_fragment_encode(&b, &f));

    /* ObjectId 0 on the wire is malformed, not truncated. */
    memset(b.data, 0, 8);
    psrp_reader_init(&r, b.data, b.len);
    ASSERT_ERR(psrp_fragment_decode(&r, &g), PSRP_ERR_MALFORMED);

    /* BlobLength beyond the 32768 cap is malformed even before the blob
     * arrives, so we never block waiting for bytes that cannot be legal. */
    psrp_buffer_reset(&b);
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    b.data[17] = 0x00; b.data[18] = 0x01; b.data[19] = 0x00; b.data[20] = 0x01;
    psrp_reader_init(&r, b.data, b.len);
    ASSERT_ERR(psrp_fragment_decode(&r, &g), PSRP_ERR_MALFORMED);

    psrp_buffer_free(&b);
}

/* ------------------------------------------------------------- split ----- */

PSRP_TEST(split_small_message_is_one_fragment)
{
    psrp_buffer_t b;
    psrp_fragment_t g;
    psrp_reader_t r;

    psrp_buffer_init(&b);
    ASSERT_OK(psrp_fragment_split(&b, 42, "abc", 3, 0));
    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    ASSERT_EQ_SZ(g.object_id, 42u);
    ASSERT_EQ_SZ(g.fragment_id, 0u);
    /* "If a deserialized object fits into 1 packet, then both the E field and
     * the S field MUST be 1." */
    ASSERT_TRUE(g.start && g.end);
    ASSERT_EQ_MEM(g.blob, g.blob_len, "abc", 3u);
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(split_zero_length_message_still_emits_one_fragment)
{
    psrp_buffer_t b;
    psrp_fragment_t g;
    psrp_reader_t r;

    psrp_buffer_init(&b);
    ASSERT_OK(psrp_fragment_split(&b, 1, NULL, 0, 0));
    ASSERT_EQ_SZ(b.len, PSRP_FRAGMENT_HEADER_SIZE);
    psrp_reader_init(&r, b.data, b.len);
    ASSERT_OK(psrp_fragment_decode(&r, &g));
    ASSERT_TRUE(g.start && g.end);
    ASSERT_EQ_SZ(g.blob_len, 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(split_sets_flags_and_ids_across_fragments)
{
    psrp_buffer_t b;
    psrp_reader_t r;
    uint8_t msg[250];
    size_t i;
    uint64_t expect_id = 0;
    int count = 0;

    for (i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;

    psrp_buffer_init(&b);
    ASSERT_OK(psrp_fragment_split(&b, 3, msg, sizeof msg, 100));

    psrp_reader_init(&r, b.data, b.len);
    for (;;) {
        psrp_fragment_t g;
        psrp_result_t rc = psrp_fragment_decode(&r, &g);
        if (rc == PSRP_ERR_TRUNCATED) break;
        ASSERT_OK(rc);
        ASSERT_EQ_SZ(g.object_id, 3u);
        ASSERT_EQ_SZ(g.fragment_id, expect_id);
        ASSERT_EQ_I(g.start, expect_id == 0);
        count++;
        expect_id++;
        if (g.end) break;
    }
    ASSERT_EQ_I(count, 3);            /* 100 + 100 + 50 */
    ASSERT_EQ_SZ(psrp_reader_remaining(&r), 0u);
    psrp_buffer_free(&b);
}

PSRP_TEST(split_rejects_zero_object_id)
{
    psrp_buffer_t b;
    psrp_buffer_init(&b);
    ASSERT_ERR(psrp_fragment_split(&b, 0, "x", 1, 0), PSRP_ERR_INVALID_ARG);
    psrp_buffer_free(&b);
}

/* ------------------------------------------------------- defragmenter ---- */

static void roundtrip_through_defrag(size_t msg_len, size_t max_blob,
                                     size_t push_chunk)
{
    psrp_buffer_t wire, got;
    psrp_defrag_t *d;
    uint8_t *msg;
    uint64_t oid = 0;
    size_t i;

    msg = (uint8_t *)malloc(msg_len ? msg_len : 1);
    ASSERT_NOT_NULL(msg);
    for (i = 0; i < msg_len; i++) msg[i] = (uint8_t)((i * 31 + 5) & 0xFF);

    psrp_buffer_init(&wire);
    psrp_buffer_init(&got);
    ASSERT_OK(psrp_fragment_split(&wire, 77, msg, msg_len, max_blob));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);

    /* Feed the wire bytes in fixed-size chunks to prove reassembly does not
     * depend on transport read boundaries. */
    for (i = 0; i < wire.len; i += push_chunk) {
        size_t n = wire.len - i;
        if (n > push_chunk) n = push_chunk;
        ASSERT_OK(psrp_defrag_push(d, wire.data + i, n));
    }

    ASSERT_EQ_SZ(psrp_defrag_ready(d), 1u);
    ASSERT_EQ_SZ(psrp_defrag_pending(d), 0u);
    ASSERT_OK(psrp_defrag_next(d, &oid, &got));
    ASSERT_EQ_SZ(oid, 77u);
    ASSERT_EQ_MEM(got.data, got.len, msg, msg_len);
    ASSERT_ERR(psrp_defrag_next(d, &oid, &got), PSRP_ERR_NOT_FOUND);

    psrp_defrag_free(d);
    psrp_buffer_free(&wire);
    psrp_buffer_free(&got);
    free(msg);
}

PSRP_TEST(defrag_roundtrip_single_fragment)
{
    roundtrip_through_defrag(10, 0, 4096);
}

PSRP_TEST(defrag_roundtrip_many_fragments)
{
    roundtrip_through_defrag(1000, 64, 4096);
}

PSRP_TEST(defrag_roundtrip_empty_message)
{
    roundtrip_through_defrag(0, 0, 4096);
}

/* The transport hands us arbitrary chunk sizes; every split point must work,
 * including one byte at a time and splits inside a header. */
PSRP_TEST(defrag_survives_any_chunking)
{
    size_t chunk;
    for (chunk = 1; chunk <= 40; chunk++)
        roundtrip_through_defrag(300, 50, chunk);
}

PSRP_TEST(defrag_exact_blob_boundary)
{
    /* A message that is an exact multiple of the blob size must not emit a
     * spurious trailing empty fragment, and must still reassemble. */
    roundtrip_through_defrag(200, 100, 4096);
    roundtrip_through_defrag(32768, 32768, 8192);
}

PSRP_TEST(defrag_interleaved_object_ids)
{
    psrp_buffer_t a, b, got;
    psrp_defrag_t *d;
    uint64_t oid = 0;
    static const char m1[] = "first message body";
    static const char m2[] = "second message body, longer";

    psrp_buffer_init(&a);
    psrp_buffer_init(&b);
    psrp_buffer_init(&got);
    /* Two messages, each split into several fragments. */
    ASSERT_OK(psrp_fragment_split(&a, 10, m1, sizeof m1 - 1, 7));
    ASSERT_OK(psrp_fragment_split(&b, 20, m2, sizeof m2 - 1, 7));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    /* Feed all of message 10's fragments, then all of 20's. Both must come
     * back intact and in completion order. */
    ASSERT_OK(psrp_defrag_push(d, a.data, a.len));
    ASSERT_OK(psrp_defrag_push(d, b.data, b.len));
    ASSERT_EQ_SZ(psrp_defrag_ready(d), 2u);

    ASSERT_OK(psrp_defrag_next(d, &oid, &got));
    ASSERT_EQ_SZ(oid, 10u);
    ASSERT_EQ_MEM(got.data, got.len, m1, sizeof m1 - 1);

    psrp_buffer_reset(&got);
    ASSERT_OK(psrp_defrag_next(d, &oid, &got));
    ASSERT_EQ_SZ(oid, 20u);
    ASSERT_EQ_MEM(got.data, got.len, m2, sizeof m2 - 1);

    psrp_defrag_free(d);
    psrp_buffer_free(&a);
    psrp_buffer_free(&b);
    psrp_buffer_free(&got);
}

PSRP_TEST(defrag_tracks_pending_until_end_flag)
{
    psrp_buffer_t wire;
    psrp_defrag_t *d;

    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_fragment_split(&wire, 5, "0123456789", 10, 4));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    /* Only the first fragment: reassembly is pending, nothing ready. */
    ASSERT_OK(psrp_defrag_push(d, wire.data, PSRP_FRAGMENT_HEADER_SIZE + 4));
    ASSERT_EQ_SZ(psrp_defrag_pending(d), 1u);
    ASSERT_EQ_SZ(psrp_defrag_ready(d), 0u);

    ASSERT_OK(psrp_defrag_push(d, wire.data + PSRP_FRAGMENT_HEADER_SIZE + 4,
                               wire.len - PSRP_FRAGMENT_HEADER_SIZE - 4));
    ASSERT_EQ_SZ(psrp_defrag_pending(d), 0u);
    ASSERT_EQ_SZ(psrp_defrag_ready(d), 1u);

    psrp_defrag_free(d);
    psrp_buffer_free(&wire);
}

PSRP_TEST(defrag_rejects_continuation_without_start)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    psrp_defrag_t *d;

    psrp_buffer_init(&b);
    f.object_id = 8; f.fragment_id = 1;
    f.start = false; f.end = true;
    f.blob = (const uint8_t *)"x"; f.blob_len = 1;
    ASSERT_OK(psrp_fragment_encode(&b, &f));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    ASSERT_ERR(psrp_defrag_push(d, b.data, b.len), PSRP_ERR_MALFORMED);
    psrp_defrag_free(d);
    psrp_buffer_free(&b);
}

PSRP_TEST(defrag_rejects_out_of_order_fragment)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    psrp_defrag_t *d;

    psrp_buffer_init(&b);
    f.object_id = 8; f.fragment_id = 0; f.start = true; f.end = false;
    f.blob = (const uint8_t *)"a"; f.blob_len = 1;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    /* Skips fragment 1. */
    f.fragment_id = 2; f.start = false; f.end = true;
    f.blob = (const uint8_t *)"b"; f.blob_len = 1;
    ASSERT_OK(psrp_fragment_encode(&b, &f));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    ASSERT_ERR(psrp_defrag_push(d, b.data, b.len), PSRP_ERR_MALFORMED);
    psrp_defrag_free(d);
    psrp_buffer_free(&b);
}

PSRP_TEST(defrag_rejects_duplicate_start)
{
    psrp_buffer_t b;
    psrp_fragment_t f;
    psrp_defrag_t *d;

    psrp_buffer_init(&b);
    f.object_id = 8; f.fragment_id = 0; f.start = true; f.end = false;
    f.blob = (const uint8_t *)"a"; f.blob_len = 1;
    ASSERT_OK(psrp_fragment_encode(&b, &f));
    ASSERT_OK(psrp_fragment_encode(&b, &f));   /* second Start, same ObjectId */

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    ASSERT_ERR(psrp_defrag_push(d, b.data, b.len), PSRP_ERR_MALFORMED);
    psrp_defrag_free(d);
    psrp_buffer_free(&b);
}

PSRP_TEST(defrag_enforces_max_message)
{
    psrp_buffer_t wire;
    psrp_defrag_t *d;

    psrp_buffer_init(&wire);
    ASSERT_OK(psrp_fragment_split(&wire, 4, "0123456789", 10, 4));

    d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    psrp_defrag_set_max_message(d, 5);   /* message is 10 bytes */
    ASSERT_ERR(psrp_defrag_push(d, wire.data, wire.len), PSRP_ERR_OVERFLOW);

    psrp_defrag_free(d);
    psrp_buffer_free(&wire);
}

PSRP_TEST(defrag_handles_null_and_empty_push)
{
    psrp_defrag_t *d = psrp_defrag_new();
    ASSERT_NOT_NULL(d);
    ASSERT_OK(psrp_defrag_push(d, NULL, 0));
    ASSERT_ERR(psrp_defrag_push(d, NULL, 4), PSRP_ERR_INVALID_ARG);
    ASSERT_EQ_SZ(psrp_defrag_ready(d), 0u);
    psrp_defrag_free(d);
    psrp_defrag_free(NULL);          /* must be safe */
}

static const psrp_test_case_t cases[] = {
    PSRP_TEST_CASE(fragment_encode_wire_layout),
    PSRP_TEST_CASE(fragment_flag_bit_values),
    PSRP_TEST_CASE(fragment_encode_decode_roundtrip),
    PSRP_TEST_CASE(fragment_empty_blob_roundtrip),
    PSRP_TEST_CASE(fragment_decode_ignores_reserved_bits),
    PSRP_TEST_CASE(fragment_encode_rejects_invalid),
    PSRP_TEST_CASE(fragment_decode_truncated_leaves_reader_untouched),
    PSRP_TEST_CASE(fragment_decode_rejects_malformed),
    PSRP_TEST_CASE(split_small_message_is_one_fragment),
    PSRP_TEST_CASE(split_zero_length_message_still_emits_one_fragment),
    PSRP_TEST_CASE(split_sets_flags_and_ids_across_fragments),
    PSRP_TEST_CASE(split_rejects_zero_object_id),
    PSRP_TEST_CASE(defrag_roundtrip_single_fragment),
    PSRP_TEST_CASE(defrag_roundtrip_many_fragments),
    PSRP_TEST_CASE(defrag_roundtrip_empty_message),
    PSRP_TEST_CASE(defrag_survives_any_chunking),
    PSRP_TEST_CASE(defrag_exact_blob_boundary),
    PSRP_TEST_CASE(defrag_interleaved_object_ids),
    PSRP_TEST_CASE(defrag_tracks_pending_until_end_flag),
    PSRP_TEST_CASE(defrag_rejects_continuation_without_start),
    PSRP_TEST_CASE(defrag_rejects_out_of_order_fragment),
    PSRP_TEST_CASE(defrag_rejects_duplicate_start),
    PSRP_TEST_CASE(defrag_enforces_max_message),
    PSRP_TEST_CASE(defrag_handles_null_and_empty_push),
};

PSRP_TEST_MAIN(cases)
