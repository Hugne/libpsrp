/* [MS-PSRP] 2.2.4 Packet Fragment - encode, decode, and split. */

#include <string.h>

#include "psrp/psrp_fragment.h"

psrp_result_t psrp_fragment_encode(psrp_buffer_t *out, const psrp_fragment_t *f)
{
    psrp_result_t rc;
    uint8_t flags = 0;

    if (!out || !f) return PSRP_ERR_INVALID_ARG;
    /* 2.2.4: ObjectId MUST be greater than 0. */
    if (f->object_id == 0) return PSRP_ERR_INVALID_ARG;
    if (f->blob_len > PSRP_FRAGMENT_MAX_BLOB) return PSRP_ERR_OVERFLOW;
    if (f->blob_len && !f->blob) return PSRP_ERR_INVALID_ARG;
    /* 2.2.4: the Start fragment MUST have a FragmentId of 0. */
    if (f->start && f->fragment_id != 0) return PSRP_ERR_INVALID_ARG;

    if (f->start) flags |= PSRP_FRAGMENT_FLAG_START;
    if (f->end)   flags |= PSRP_FRAGMENT_FLAG_END;

    rc = psrp_buffer_reserve(out, PSRP_FRAGMENT_HEADER_SIZE + f->blob_len);
    if (rc != PSRP_OK) return rc;

    rc = psrp_buffer_append_u64be(out, f->object_id);
    if (rc != PSRP_OK) return rc;
    rc = psrp_buffer_append_u64be(out, f->fragment_id);
    if (rc != PSRP_OK) return rc;
    rc = psrp_buffer_append_u8(out, flags);       /* Reserved bits sent as 0 */
    if (rc != PSRP_OK) return rc;
    rc = psrp_buffer_append_u32be(out, f->blob_len);
    if (rc != PSRP_OK) return rc;
    if (f->blob_len)
        return psrp_buffer_append(out, f->blob, f->blob_len);
    return PSRP_OK;
}

psrp_result_t psrp_fragment_decode(psrp_reader_t *r, psrp_fragment_t *out)
{
    psrp_result_t rc;
    uint64_t object_id, fragment_id;
    uint8_t flags;
    uint32_t blob_len;
    const uint8_t *blob = NULL;
    size_t start_pos;

    if (!r || !out) return PSRP_ERR_INVALID_ARG;
    start_pos = r->pos;

    rc = psrp_read_u64be(r, &object_id);
    if (rc != PSRP_OK) goto rewind;
    rc = psrp_read_u64be(r, &fragment_id);
    if (rc != PSRP_OK) goto rewind;
    rc = psrp_read_u8(r, &flags);
    if (rc != PSRP_OK) goto rewind;
    rc = psrp_read_u32be(r, &blob_len);
    if (rc != PSRP_OK) goto rewind;

    /* Validate before consuming the blob so a bogus length cannot make us
     * wait forever for bytes that will never arrive. */
    if (blob_len > PSRP_FRAGMENT_MAX_BLOB) return PSRP_ERR_MALFORMED;
    if (object_id == 0) return PSRP_ERR_MALFORMED;

    if (blob_len) {
        rc = psrp_read_borrow(r, blob_len, &blob);
        if (rc != PSRP_OK) goto rewind;
    }

    out->object_id = object_id;
    out->fragment_id = fragment_id;
    /* Reserved bits are ignored on receipt, per 2.2.4. */
    out->start = (flags & PSRP_FRAGMENT_FLAG_START) != 0;
    out->end = (flags & PSRP_FRAGMENT_FLAG_END) != 0;
    out->blob = blob;
    out->blob_len = blob_len;
    return PSRP_OK;

rewind:
    /* Leave the reader untouched so the caller can retry once more bytes
     * arrive. Only truncation can land here. */
    r->pos = start_pos;
    return rc;
}

psrp_result_t psrp_fragment_split(psrp_buffer_t *out, uint64_t object_id,
                                  const void *msg, size_t msg_len,
                                  size_t max_blob)
{
    const uint8_t *p = (const uint8_t *)msg;
    size_t offset = 0;
    uint64_t frag_id = 0;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    if (object_id == 0) return PSRP_ERR_INVALID_ARG;
    if (msg_len && !msg) return PSRP_ERR_INVALID_ARG;
    if (max_blob == 0 || max_blob > PSRP_FRAGMENT_MAX_BLOB)
        max_blob = PSRP_FRAGMENT_MAX_BLOB;

    do {
        psrp_fragment_t f;
        size_t chunk = msg_len - offset;
        if (chunk > max_blob) chunk = max_blob;

        f.object_id = object_id;
        f.fragment_id = frag_id;
        f.start = (frag_id == 0);
        f.end = (offset + chunk == msg_len);
        f.blob = p ? p + offset : NULL;
        f.blob_len = (uint32_t)chunk;

        rc = psrp_fragment_encode(out, &f);
        if (rc != PSRP_OK) return rc;

        offset += chunk;
        frag_id++;
    } while (offset < msg_len);
    /* The do/while guarantees a zero-length message still emits one fragment
     * carrying both the Start and End flags. */

    return PSRP_OK;
}
