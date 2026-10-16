/*
 * A request's migration target retained by a facade: what a REDIRECT
 * refusal or a per-request GOAWAY named, copied out of the event (whose
 * spans the next advancing call invalidates) into one facade-owned block.
 */
#ifndef MOQ_FACADE_MIGRATION_INTERNAL_H
#define MOQ_FACADE_MIGRATION_INTERNAL_H

#include <moq/session.h>
#include <stddef.h>
#include <string.h>

typedef struct facade_migration {
    bool                    present;   /* the request ended by migration */
    moq_result_t            error;     /* MOQ_ERR_NOMEM: the copy failed */
    moq_request_migration_t value;     /* spans point into block */
    void                   *block;
    size_t                  block_len;
} facade_migration_t;

static inline void facade_migration_clear(facade_migration_t *m,
                                          const moq_alloc_t *alloc)
{
    if (m->block) alloc->free(m->block, m->block_len, alloc->ctx);
    memset(m, 0, sizeof(*m));
}

/* Copy one migration. `redirect` selects the REDIRECT fields, otherwise the
 * GOAWAY ones; a NULL namespace/name means empty. On allocation failure the
 * migration is still recorded (present), its target unavailable (NOMEM). */
static inline void facade_migration_set(facade_migration_t *m,
    const moq_alloc_t *alloc, bool redirect, bool can_retry,
    uint64_t retry_after_ms, uint64_t timeout_ms, moq_bytes_t uri,
    const moq_namespace_t *ns, moq_bytes_t name)
{
    facade_migration_clear(m, alloc);
    m->present = true;
    m->value.struct_size = (uint32_t)sizeof(m->value);
    m->value.redirect = redirect;
    m->value.goaway = !redirect;
    m->value.can_retry = redirect && can_retry;
    m->value.retry_after_ms = redirect ? retry_after_ms : 0;
    m->value.timeout_ms = redirect ? 0 : timeout_ms;
    size_t parts = ns ? ns->count : 0;
    size_t bytes = parts * sizeof(moq_bytes_t) + uri.len + name.len;
    for (size_t i = 0; i < parts; i++) bytes += ns->parts[i].len;
    if (bytes == 0) return;
    uint8_t *b = (uint8_t *)alloc->alloc(bytes, alloc->ctx);
    if (!b) {
        m->error = MOQ_ERR_NOMEM;
        return;
    }
    m->block = b;
    m->block_len = bytes;
    moq_bytes_t *pv = (moq_bytes_t *)(void *)b;
    uint8_t *p = b + parts * sizeof(moq_bytes_t);
    for (size_t i = 0; i < parts; i++) {
        pv[i].data = ns->parts[i].len ? p : NULL;
        pv[i].len = ns->parts[i].len;
        if (ns->parts[i].len) memcpy(p, ns->parts[i].data, ns->parts[i].len);
        p += ns->parts[i].len;
    }
    m->value.target.track_namespace.parts = parts ? pv : NULL;
    m->value.target.track_namespace.count = parts;
    m->value.target.connect_uri.data = uri.len ? p : NULL;
    m->value.target.connect_uri.len = uri.len;
    if (uri.len) memcpy(p, uri.data, uri.len);
    p += uri.len;
    m->value.target.track_name.data = name.len ? p : NULL;
    m->value.target.track_name.len = name.len;
    if (name.len) memcpy(p, name.data, name.len);
}

/* The public query: MOQ_DONE when the request did not end by migration,
 * the retained copy's failure, or min(out_size, sizeof) bytes written. */
static inline moq_result_t facade_migration_get(const facade_migration_t *m,
    moq_request_migration_t *out, size_t out_size)
{
    if (!out || out_size < offsetof(moq_request_migration_t, target) +
                           sizeof(out->target))
        return MOQ_ERR_INVAL;
    if (!m->present) return MOQ_DONE;
    if (m->error != MOQ_OK) return m->error;
    size_t n = out_size < sizeof(m->value) ? out_size : sizeof(m->value);
    memcpy(out, &m->value, n);
    out->struct_size = (uint32_t)n;
    return MOQ_OK;
}

#endif /* MOQ_FACADE_MIGRATION_INTERNAL_H */
