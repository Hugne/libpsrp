/* psrp_fragment.h - [MS-PSRP] 2.2.4 Packet Fragment.
 *
 * A WS-MAN packet carries a limited payload, so PSRP messages are split into
 * fragments. One fragment must fit in one WS-MAN packet, but one packet may
 * carry several fragments.
 *
 * Wire layout (all integers network byte order):
 *
 *   ObjectId    8 bytes   id of the PSRP message this fragment belongs to (>0)
 *   FragmentId  8 bytes   sequence within that message, starting at 0
 *   flags       1 byte    Reserved:6 | E:1 | S:1   (S = 0x01, E = 0x02)
 *   BlobLength  4 bytes   0 .. 32768
 *   Blob        variable
 *
 * A message that fits in one fragment has both S and E set. The Start fragment
 * always has FragmentId 0. Reserved bits MUST be sent as 0 and ignored on
 * receipt.
 */
#ifndef PSRP_FRAGMENT_H
#define PSRP_FRAGMENT_H

#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSRP_FRAGMENT_HEADER_SIZE 21u
/* [MS-PSRP] 2.2.4: BlobLength MUST be <= 32768. */
#define PSRP_FRAGMENT_MAX_BLOB 32768u

#define PSRP_FRAGMENT_FLAG_START 0x01u
#define PSRP_FRAGMENT_FLAG_END   0x02u

typedef struct psrp_fragment {
    uint64_t object_id;
    uint64_t fragment_id;
    bool start;
    bool end;
    const uint8_t *blob;   /* borrowed on decode; not owned */
    uint32_t blob_len;
} psrp_fragment_t;

/* Appends one encoded fragment (header + blob) to `out`.
 * Rejects object_id == 0 and blob_len > PSRP_FRAGMENT_MAX_BLOB. */
psrp_result_t psrp_fragment_encode(psrp_buffer_t *out, const psrp_fragment_t *f);

/* Decodes one fragment. `out->blob` borrows from the reader's buffer and stays
 * valid only as long as that memory does.
 * Returns PSRP_ERR_TRUNCATED if the reader holds an incomplete fragment, so a
 * caller streaming bytes can simply wait for more. */
psrp_result_t psrp_fragment_decode(psrp_reader_t *r, psrp_fragment_t *out);

/* Splits one complete PSRP message into fragments, appending them all to
 * `out`. `max_blob` is the per-fragment blob cap; pass 0 for the protocol
 * maximum. A zero-length message still produces exactly one fragment with both
 * S and E set. */
psrp_result_t psrp_fragment_split(psrp_buffer_t *out, uint64_t object_id,
                                  const void *msg, size_t msg_len,
                                  size_t max_blob);

/* ---------------------------------------------------------- defragmenter -- */

/* Reassembles fragments back into whole messages. Handles fragments arriving
 * in arbitrarily sized chunks and messages interleaved by ObjectId. */
typedef struct psrp_defrag psrp_defrag_t;

psrp_defrag_t *psrp_defrag_new(void);
void psrp_defrag_free(psrp_defrag_t *d);

/* Guards against a peer streaming an unbounded message. Default 64 MiB. */
void psrp_defrag_set_max_message(psrp_defrag_t *d, size_t max_bytes);

/* Feeds received bytes. Complete messages are queued for psrp_defrag_next.
 * Returns PSRP_ERR_MALFORMED on a protocol violation (bad ordering, unknown
 * ObjectId, oversized message); the defragmenter should be discarded then. */
psrp_result_t psrp_defrag_push(psrp_defrag_t *d, const void *data, size_t len);

/* Moves the next completed message into `out` (appending to it) and reports
 * its ObjectId. Returns PSRP_ERR_NOT_FOUND when nothing is ready. */
psrp_result_t psrp_defrag_next(psrp_defrag_t *d, uint64_t *object_id,
                               psrp_buffer_t *out);

/* Number of completed messages waiting in the queue. */
size_t psrp_defrag_ready(const psrp_defrag_t *d);
/* Number of messages partially reassembled (fragments seen, no End yet). */
size_t psrp_defrag_pending(const psrp_defrag_t *d);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_FRAGMENT_H */
