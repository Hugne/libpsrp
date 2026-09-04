/* Reassembly of [MS-PSRP] 2.2.4 fragments back into whole messages.
 *
 * Fragments arrive in arbitrarily sized transport reads, so incoming bytes are
 * buffered until whole fragments can be parsed. Messages may interleave by
 * ObjectId, so partial reassemblies are kept in a small list.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_fragment.h"

#define PSRP_DEFRAG_DEFAULT_MAX_MESSAGE ((size_t)64 * 1024 * 1024)

typedef struct partial {
    struct partial *next;
    uint64_t object_id;
    uint64_t next_fragment_id;  /* fragment id we require next */
    psrp_buffer_t data;
} partial_t;

typedef struct ready {
    struct ready *next;
    uint64_t object_id;
    psrp_buffer_t data;
} ready_t;

struct psrp_defrag {
    psrp_buffer_t in;      /* unparsed transport bytes */
    partial_t *partials;   /* reassembly in progress */
    ready_t *ready_head;   /* completed, oldest first */
    ready_t *ready_tail;
    size_t max_message;
};

psrp_defrag_t *psrp_defrag_new(void)
{
    psrp_defrag_t *d = (psrp_defrag_t *)calloc(1, sizeof *d);
    if (!d) return NULL;
    psrp_buffer_init(&d->in);
    d->max_message = PSRP_DEFRAG_DEFAULT_MAX_MESSAGE;
    return d;
}

void psrp_defrag_free(psrp_defrag_t *d)
{
    partial_t *p;
    ready_t *r;
    if (!d) return;
    psrp_buffer_free(&d->in);
    p = d->partials;
    while (p) {
        partial_t *next = p->next;
        psrp_buffer_free(&p->data);
        free(p);
        p = next;
    }
    r = d->ready_head;
    while (r) {
        ready_t *next = r->next;
        psrp_buffer_free(&r->data);
        free(r);
        r = next;
    }
    free(d);
}

void psrp_defrag_set_max_message(psrp_defrag_t *d, size_t max_bytes)
{
    if (d && max_bytes) d->max_message = max_bytes;
}

static partial_t *partial_find(psrp_defrag_t *d, uint64_t object_id)
{
    partial_t *p;
    for (p = d->partials; p; p = p->next)
        if (p->object_id == object_id) return p;
    return NULL;
}

static void partial_remove(psrp_defrag_t *d, partial_t *target)
{
    partial_t **link = &d->partials;
    while (*link) {
        if (*link == target) {
            *link = target->next;
            psrp_buffer_free(&target->data);
            free(target);
            return;
        }
        link = &(*link)->next;
    }
}

static psrp_result_t ready_push(psrp_defrag_t *d, uint64_t object_id,
                                psrp_buffer_t *take)
{
    ready_t *r = (ready_t *)calloc(1, sizeof *r);
    if (!r) return PSRP_ERR_NOMEM;
    r->object_id = object_id;
    r->data = *take;          /* move ownership */
    psrp_buffer_init(take);
    if (d->ready_tail) d->ready_tail->next = r;
    else d->ready_head = r;
    d->ready_tail = r;
    return PSRP_OK;
}

static psrp_result_t handle_fragment(psrp_defrag_t *d, const psrp_fragment_t *f)
{
    partial_t *p = partial_find(d, f->object_id);
    psrp_result_t rc;

    if (f->start) {
        /* 2.2.4: the Start fragment MUST have a FragmentId of 0. */
        if (f->fragment_id != 0) return PSRP_ERR_MALFORMED;
        /* A second Start for an ObjectId still being reassembled is a
         * protocol violation, not something to silently paper over. */
        if (p) return PSRP_ERR_MALFORMED;

        p = (partial_t *)calloc(1, sizeof *p);
        if (!p) return PSRP_ERR_NOMEM;
        p->object_id = f->object_id;
        p->next_fragment_id = 0;
        psrp_buffer_init(&p->data);
        p->next = d->partials;
        d->partials = p;
    } else {
        if (!p) return PSRP_ERR_MALFORMED;   /* continuation of nothing */
    }

    if (f->fragment_id != p->next_fragment_id) return PSRP_ERR_MALFORMED;

    if (f->blob_len) {
        if (p->data.len + f->blob_len > d->max_message) {
            partial_remove(d, p);
            return PSRP_ERR_OVERFLOW;
        }
        rc = psrp_buffer_append(&p->data, f->blob, f->blob_len);
        if (rc != PSRP_OK) return rc;
    }
    p->next_fragment_id++;

    if (f->end) {
        psrp_buffer_t done = p->data;
        psrp_buffer_init(&p->data);   /* detach before removing the node */
        rc = ready_push(d, p->object_id, &done);
        partial_remove(d, p);
        if (rc != PSRP_OK) {
            psrp_buffer_free(&done);
            return rc;
        }
    }
    return PSRP_OK;
}

psrp_result_t psrp_defrag_push(psrp_defrag_t *d, const void *data, size_t len)
{
    psrp_result_t rc;
    psrp_reader_t r;
    size_t consumed = 0;

    if (!d) return PSRP_ERR_INVALID_ARG;
    if (len && !data) return PSRP_ERR_INVALID_ARG;

    if (len) {
        rc = psrp_buffer_append(&d->in, data, len);
        if (rc != PSRP_OK) return rc;
    }

    psrp_reader_init(&r, d->in.data, d->in.len);
    for (;;) {
        psrp_fragment_t f;
        rc = psrp_fragment_decode(&r, &f);
        if (rc == PSRP_ERR_TRUNCATED) break;      /* wait for more bytes */
        if (rc != PSRP_OK) return rc;

        rc = handle_fragment(d, &f);
        if (rc != PSRP_OK) return rc;
        consumed = r.pos;
    }

    if (consumed) {
        rc = psrp_buffer_consume(&d->in, consumed);
        if (rc != PSRP_OK) return rc;
    }
    return PSRP_OK;
}

psrp_result_t psrp_defrag_next(psrp_defrag_t *d, uint64_t *object_id,
                               psrp_buffer_t *out)
{
    ready_t *r;
    psrp_result_t rc;

    if (!d || !out) return PSRP_ERR_INVALID_ARG;
    r = d->ready_head;
    if (!r) return PSRP_ERR_NOT_FOUND;

    rc = psrp_buffer_append(out, r->data.data, r->data.len);
    if (rc != PSRP_OK) return rc;
    if (object_id) *object_id = r->object_id;

    d->ready_head = r->next;
    if (!d->ready_head) d->ready_tail = NULL;
    psrp_buffer_free(&r->data);
    free(r);
    return PSRP_OK;
}

size_t psrp_defrag_ready(const psrp_defrag_t *d)
{
    size_t n = 0;
    const ready_t *r;
    if (!d) return 0;
    for (r = d->ready_head; r; r = r->next) n++;
    return n;
}

size_t psrp_defrag_pending(const psrp_defrag_t *d)
{
    size_t n = 0;
    const partial_t *p;
    if (!d) return 0;
    for (p = d->partials; p; p = p->next) n++;
    return n;
}
