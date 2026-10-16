#include "session_internal.h"
#include <string.h>

/* -- Malformed Track detection (draft-22 §12.1) --------------------- *
 *
 * A receiver that detects a Malformed Track MUST cancel the subscriptions and
 * fetches carrying it and SHOULD tell the application. The conditions it can
 * detect cheaply are kept per received Track:
 *
 *   1  a Subgroup's Publisher Priority changes,
 *   2  an Object past a Subgroup's final Object (the last one before a FIN),
 *   3  a Subgroup ended by FIN at two different final Objects,
 *   4  an Object past a Group's final Object (END_OF_GROUP status, or the last
 *      Object before a FIN on a subgroup with the END_OF_GROUP bit), and two
 *      different finals for one Group,
 *   5  an Object past the Track's final Object (END_OF_TRACK status), and two
 *      different Track finals,
 *
 * plus the Prior Group ID Gap and Prior Object ID Gap conditions (§10.8,
 * §10.9). Condition 6 (the same Object twice with a different Payload) needs
 * the Objects themselves and condition 7 (a Delivery Mode change) needs every
 * Object's mode; a receiver that does not keep them cannot detect either.
 *
 * Memory is bounded: a record keeps the most recent MT_GROUPS groups and
 * MT_SUBGROUPS subgroups, replacing the least recently used. A condition about
 * a Group or Subgroup no longer kept is not detected; nothing is reported
 * that the Track did not say. */

#define MT_GROUPS    8u
#define MT_SUBGROUPS 16u

#define MT_KIND_ALIAS 1u
#define MT_KIND_FETCH 2u

typedef struct mt_group {
    bool     used;
    bool     has_final;
    bool     has_seen;
    bool     has_group_gap;
    bool     has_object_gap;
    uint32_t stamp;
    uint64_t group_id;
    uint64_t final_object;
    uint64_t min_seen;
    uint64_t max_seen;
    uint64_t group_gap;
    uint64_t object_gap_lo;      /* the latest Prior Object ID Gap range, */
    uint64_t object_gap_hi;      /* inclusive                              */
} mt_group_t;

typedef struct mt_subgroup {
    bool     used;
    bool     has_final;
    bool     inherited;          /* priority omitted on the wire */
    uint8_t  priority;
    uint32_t stamp;
    uint64_t group_id;
    uint64_t subgroup_id;
    uint64_t final_object;
} mt_subgroup_t;

struct moq_mt_track {
    struct moq_mt_track *next;
    uint8_t  kind;
    uint64_t key;                /* alias, or the fetch handle */
    /* An alias record holds the history of one Track: the canonical key of
     * the Track its alias named when the record was made, stored after the
     * record. Requests for that Track come and go on the alias without
     * touching it; the alias naming another Track starts a new record. */
    size_t   track_key_len;
    bool     has_track_end;
    uint64_t end_group;
    uint64_t end_object;
    uint32_t clock;
    mt_group_t    groups[MT_GROUPS];
    mt_subgroup_t subgroups[MT_SUBGROUPS];
};

static const uint8_t *mt_track_key(const moq_mt_track_t *t)
{
    return (const uint8_t *)(t + 1);
}

static bool key_eq(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    return a && b && al == bl && al > 0 && memcmp(a, b, al) == 0;
}

/* The canonical key of the Track a request names, or false when it has none. */
static bool sub_key(const moq_sub_entry_t *e, const uint8_t **k, size_t *l)
{
    if (e->track_id_buf && e->track_id_len > 0) {
        *k = e->track_id_buf; *l = e->track_id_len; return true;
    }
    if (e->hist && e->hist->key_len > 0) {
        *k = e->hist->key; *l = e->hist->key_len; return true;
    }
    return false;
}

static bool pub_key(const moq_pub_entry_t *e, const uint8_t **k, size_t *l)
{
    if (e->hist && e->hist->key_len > 0) {
        *k = e->hist->key; *l = e->hist->key_len; return true;
    }
    return false;
}

/* The Track the peer currently delivers on `alias`: the key of a request
 * bound to it. False when no request holds the alias. */
static bool alias_key(moq_session_t *s, uint64_t alias, const uint8_t **k, size_t *l,
                      bool *has_owner)
{
    *has_owner = false;
    int ss = sub_find_by_alias_subscriber(s, alias);
    if (ss >= 0) {
        *has_owner = true;
        return sub_key(&s->subs[ss], k, l);
    }
    int ps = pub_find_by_alias_subscriber(s, alias);
    if (ps >= 0) {
        *has_owner = true;
        return pub_key(&s->publishes[ps], k, l);
    }
    return false;
}

static bool mt_alive(moq_session_t *s, const moq_mt_track_t *t)
{
    if (t->kind == MT_KIND_FETCH)
        return fetch_resolve_handle(s, (moq_fetch_t){ ._opaque = t->key }) >= 0;
    const uint8_t *k = NULL;
    size_t l = 0;
    bool owner;
    return alias_key(s, t->key, &k, &l, &owner) &&
           key_eq(k, l, mt_track_key(t), t->track_key_len);
}

static void mt_free(moq_session_t *s, moq_mt_track_t *t)
{
    s->alloc.free(t, sizeof(*t) + t->track_key_len, s->alloc.ctx);
    s->mt_count--;
}

/* Drop the records whose Track no request holds any more. */
static void mt_collect(moq_session_t *s)
{
    moq_mt_track_t **pp = &s->mt_head;
    while (*pp) {
        moq_mt_track_t *t = *pp;
        if (!mt_alive(s, t)) {
            *pp = t->next;
            mt_free(s, t);
        } else {
            pp = &t->next;
        }
    }
}

static moq_mt_track_t *mt_new(moq_session_t *s, uint8_t kind, uint64_t key,
                              const uint8_t *track_key, size_t track_key_len,
                              moq_result_t *rc)
{
    size_t cap = s->sub_cap + s->pub_cap + s->fetch_cap;
    if (s->mt_count >= cap) mt_collect(s);
    if (s->mt_count >= cap) return NULL;
    moq_mt_track_t *t = (moq_mt_track_t *)s->alloc.alloc(sizeof(*t) + track_key_len,
                                                          s->alloc.ctx);
    if (!t) {
        *rc = MOQ_ERR_NOMEM;
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->key = key;
    t->track_key_len = track_key_len;
    if (track_key_len) memcpy(t + 1, track_key, track_key_len);
    t->next = s->mt_head;
    s->mt_head = t;
    s->mt_count++;
    return t;
}

moq_mt_track_t *mt_track_for_alias(moq_session_t *s, uint64_t alias,
                                   moq_result_t *rc)
{
    *rc = MOQ_OK;
    if (!s->profile->malformed_track_local) return NULL;
    const uint8_t *k = NULL;
    size_t l = 0;
    bool owner;
    if (!alias_key(s, alias, &k, &l, &owner)) return NULL;
    for (moq_mt_track_t **pp = &s->mt_head; *pp; pp = &(*pp)->next) {
        moq_mt_track_t *t = *pp;
        if (t->kind != MT_KIND_ALIAS || t->key != alias) continue;
        if (key_eq(k, l, mt_track_key(t), t->track_key_len)) return t;
        /* The alias names another Track now: its history does not apply. */
        *pp = t->next;
        mt_free(s, t);
        break;
    }
    return mt_new(s, MT_KIND_ALIAS, alias, k, l, rc);
}

moq_mt_track_t *mt_track_for_fetch(moq_session_t *s, moq_fetch_t fetch,
                                   moq_result_t *rc)
{
    *rc = MOQ_OK;
    if (!s->profile->malformed_track_local) return NULL;
    if (fetch_resolve_handle(s, fetch) < 0) return NULL;
    for (moq_mt_track_t *t = s->mt_head; t; t = t->next)
        if (t->kind == MT_KIND_FETCH && t->key == fetch._opaque) return t;
    return mt_new(s, MT_KIND_FETCH, fetch._opaque, NULL, 0, rc);
}

void mt_destroy_all(moq_session_t *s)
{
    while (s->mt_head) {
        moq_mt_track_t *t = s->mt_head;
        s->mt_head = t->next;
        mt_free(s, t);
    }
}

/* -- Lookup ----------------------------------------------------------- */

static mt_group_t *mt_group_find(moq_mt_track_t *t, uint64_t group_id)
{
    for (size_t i = 0; i < MT_GROUPS; i++)
        if (t->groups[i].used && t->groups[i].group_id == group_id)
            return &t->groups[i];
    return NULL;
}

static mt_subgroup_t *mt_subgroup_find(moq_mt_track_t *t, uint64_t group_id,
                                       uint64_t subgroup_id)
{
    for (size_t i = 0; i < MT_SUBGROUPS; i++)
        if (t->subgroups[i].used && t->subgroups[i].group_id == group_id &&
            t->subgroups[i].subgroup_id == subgroup_id)
            return &t->subgroups[i];
    return NULL;
}

static mt_group_t *mt_group_take(moq_mt_track_t *t, uint64_t group_id)
{
    mt_group_t *g = mt_group_find(t, group_id);
    if (!g) {
        g = &t->groups[0];
        for (size_t i = 0; i < MT_GROUPS; i++) {
            if (!t->groups[i].used) { g = &t->groups[i]; break; }
            if (t->groups[i].stamp < g->stamp) g = &t->groups[i];
        }
        memset(g, 0, sizeof(*g));
        g->used = true;
        g->group_id = group_id;
    }
    g->stamp = ++t->clock;
    return g;
}

static mt_subgroup_t *mt_subgroup_take(moq_mt_track_t *t, uint64_t group_id,
                                       uint64_t subgroup_id, uint8_t priority,
                                       bool inherited)
{
    mt_subgroup_t *sg = mt_subgroup_find(t, group_id, subgroup_id);
    if (!sg) {
        sg = &t->subgroups[0];
        for (size_t i = 0; i < MT_SUBGROUPS; i++) {
            if (!t->subgroups[i].used) { sg = &t->subgroups[i]; break; }
            if (t->subgroups[i].stamp < sg->stamp) sg = &t->subgroups[i];
        }
        memset(sg, 0, sizeof(*sg));
        sg->used = true;
        sg->group_id = group_id;
        sg->subgroup_id = subgroup_id;
        sg->priority = priority;
        sg->inherited = inherited;
    }
    sg->stamp = ++t->clock;
    return sg;
}

static bool loc_after(uint64_t g, uint64_t o, uint64_t eg, uint64_t eo)
{
    return g > eg || (g == eg && o > eo);
}

/* -- Checks ----------------------------------------------------------- */

static uint8_t mt_check_gaps(moq_mt_track_t *t, const moq_mt_object_t *o,
                             const mt_group_t *g)
{
    /* The object falls inside a range an earlier object said does not
     * exist. */
    for (size_t i = 0; i < MT_GROUPS; i++) {
        const mt_group_t *r = &t->groups[i];
        if (!r->used || !r->has_group_gap || r->group_gap == 0) continue;
        if (o->group_id < r->group_id &&
            o->group_id >= r->group_id - r->group_gap)
            return MOQ_MALFORMED_GAP;
    }
    if (g && g->has_object_gap && o->object_id >= g->object_gap_lo &&
        o->object_id <= g->object_gap_hi)
        return MOQ_MALFORMED_GAP;
    if (!o->gaps) return MOQ_MALFORMED_NONE;
    if (o->gaps->has_group_gap) {
        uint64_t n = o->gaps->group_gap;
        if (n > o->group_id) return MOQ_MALFORMED_GAP;
        /* A Group contains Objects with different Prior Group ID Gaps. */
        if (g && g->has_group_gap && g->group_gap != n) return MOQ_MALFORMED_GAP;
        if (n > 0) {
            /* The gap covers a Group an Object was received in. */
            for (size_t i = 0; i < MT_GROUPS; i++) {
                const mt_group_t *r = &t->groups[i];
                if (r->used && r->has_seen && r->group_id < o->group_id &&
                    r->group_id >= o->group_id - n)
                    return MOQ_MALFORMED_GAP;
            }
        }
    }
    if (o->gaps->has_object_gap) {
        uint64_t n = o->gaps->object_gap;
        if (n > o->object_id) return MOQ_MALFORMED_GAP;
        if (n > 0 && g && g->has_seen) {
            /* The gap covers an Object received in this Group. */
            uint64_t lo = o->object_id - n, hi = o->object_id - 1;
            if ((g->min_seen >= lo && g->min_seen <= hi) ||
                (g->max_seen >= lo && g->max_seen <= hi))
                return MOQ_MALFORMED_GAP;
        }
    }
    return MOQ_MALFORMED_NONE;
}

uint8_t mt_object(moq_mt_track_t *t, const moq_mt_object_t *o)
{
    if (!t) return MOQ_MALFORMED_NONE;
    bool eog = o->end_of_group || o->status == MOQ_OBJECT_END_OF_GROUP;
    bool eot = o->status == MOQ_OBJECT_END_OF_TRACK;

    if (t->has_track_end) {
        if (eot && (o->group_id != t->end_group || o->object_id != t->end_object))
            return MOQ_MALFORMED_TRACK_END;
        if (loc_after(o->group_id, o->object_id, t->end_group, t->end_object))
            return MOQ_MALFORMED_PAST_TRACK_END;
    }
    mt_group_t *g = mt_group_find(t, o->group_id);
    if (g && g->has_final) {
        if (eog && o->object_id != g->final_object) return MOQ_MALFORMED_GROUP_END;
        if (o->object_id > g->final_object) return MOQ_MALFORMED_PAST_GROUP_END;
    }
    /* A final below an Object already received in the Group. */
    if ((eog || eot) && g && g->has_seen && g->max_seen > o->object_id)
        return eog ? MOQ_MALFORMED_GROUP_END : MOQ_MALFORMED_TRACK_END;
    if (eot) {
        for (size_t i = 0; i < MT_GROUPS; i++) {
            const mt_group_t *r = &t->groups[i];
            if (r->used && r->has_seen && r->group_id > o->group_id)
                return MOQ_MALFORMED_TRACK_END;
            if (r->used && r->has_final && r->group_id == o->group_id &&
                r->final_object > o->object_id)
                return MOQ_MALFORMED_TRACK_END;
        }
    }
    mt_subgroup_t *sg = NULL;
    if (o->in_subgroup) {
        sg = mt_subgroup_find(t, o->group_id, o->subgroup_id);
        /* Condition 1 compares what the publisher stated: a priority
         * omitted twice is the same statement even when the requests that
         * received the two objects resolve different defaults (draft-22
         * §10.4: the default is per subscription). */
        if (sg && !(sg->inherited && o->priority_inherited) &&
            sg->priority != o->priority)
            return MOQ_MALFORMED_PRIORITY;
        if (sg && sg->has_final && o->object_id > sg->final_object)
            return MOQ_MALFORMED_PAST_SUBGROUP_END;
    }
    uint8_t gr = mt_check_gaps(t, o, g);
    if (gr != MOQ_MALFORMED_NONE) return gr;

    /* Consistent: record it. */
    g = mt_group_take(t, o->group_id);
    if (!g->has_seen) {
        g->has_seen = true;
        g->min_seen = g->max_seen = o->object_id;
    } else {
        if (o->object_id < g->min_seen) g->min_seen = o->object_id;
        if (o->object_id > g->max_seen) g->max_seen = o->object_id;
    }
    if (eog || eot) {
        g->has_final = true;
        g->final_object = o->object_id;
    }
    if (eot) {
        t->has_track_end = true;
        t->end_group = o->group_id;
        t->end_object = o->object_id;
    }
    if (o->gaps && o->gaps->has_group_gap) {
        g->has_group_gap = true;
        g->group_gap = o->gaps->group_gap;
    }
    if (o->gaps && o->gaps->has_object_gap && o->gaps->object_gap > 0) {
        g->has_object_gap = true;
        g->object_gap_lo = o->object_id - o->gaps->object_gap;
        g->object_gap_hi = o->object_id - 1;
    }
    if (o->in_subgroup) {
        /* Condition 1 compares with the previous Object of the Subgroup, so
         * the record follows the latest one: after an explicit priority and
         * then an omitted one, a further omission is compared with the
         * omission, not with the explicit value. */
        sg = mt_subgroup_take(t, o->group_id, o->subgroup_id, o->priority,
                              o->priority_inherited);
        sg->priority = o->priority;
        sg->inherited = o->priority_inherited;
    }
    return MOQ_MALFORMED_NONE;
}

uint8_t mt_subgroup_fin(moq_mt_track_t *t, uint64_t group_id, uint64_t subgroup_id,
                        bool has_last, uint64_t last, bool end_of_group)
{
    if (!t || !has_last) return MOQ_MALFORMED_NONE;
    mt_subgroup_t *sg = mt_subgroup_find(t, group_id, subgroup_id);
    if (sg && sg->has_final && sg->final_object != last)
        return MOQ_MALFORMED_SUBGROUP_END;
    mt_group_t *g = mt_group_find(t, group_id);
    if (end_of_group && g) {
        if (g->has_final && g->final_object != last) return MOQ_MALFORMED_GROUP_END;
        if (g->has_seen && g->max_seen > last) return MOQ_MALFORMED_GROUP_END;
    }
    if (end_of_group && t->has_track_end && t->end_group == group_id &&
        t->end_object != last)
        return MOQ_MALFORMED_GROUP_END;
    if (sg) {
        sg->has_final = true;
        sg->final_object = last;
    }
    if (end_of_group) {
        g = mt_group_take(t, group_id);
        g->has_final = true;
        g->final_object = last;
    }
    return MOQ_MALFORMED_NONE;
}

/* -- Failure ---------------------------------------------------------- */

/* Mark one request: its Track has a malformed verdict and its cancellation
 * is owed until it commits or the request ends otherwise. */
static void mark_sub(moq_session_t *s, moq_sub_entry_t *e)
{
    if (e->mt_failing || e->role != MOQ_SUB_ROLE_SUBSCRIBER || e->done_pending) return;
    if (e->state != MOQ_SUB_PENDING_SUBSCRIBER && e->state != MOQ_SUB_ESTABLISHED) return;
    e->mt_failing = true;
    s->mt_failing_count++;
}

static void mark_pub(moq_session_t *s, moq_pub_entry_t *e)
{
    if (e->mt_failing || e->role != MOQ_PUB_ROLE_SUBSCRIBER || e->done_pending) return;
    if (e->state != MOQ_PUB_PENDING_SUBSCRIBER && e->state != MOQ_PUB_ESTABLISHED) return;
    e->mt_failing = true;
    s->mt_failing_count++;
}

static void mark_fetch(moq_session_t *s, moq_fetch_entry_t *e)
{
    if (e->mt_failing || e->role != MOQ_FETCH_ROLE_FETCHER ||
        e->state != MOQ_FETCH_PENDING_FETCHER)
        return;
    e->mt_failing = true;
    s->mt_failing_count++;
}

void session_fail_malformed_track(moq_session_t *s, const uint8_t *key, size_t key_len)
{
    if (key && key_len > 0) {
        /* Latch first: every request for the Track is marked before any
         * cancellation retires one, so a cancellation that cannot be queued
         * now does not lose the others, and a request that ends meanwhile
         * cannot carry the Track's identity away. The key may live in one of
         * these requests; it is not read once marking is done. */
        for (int32_t i = s->sub_occ_head; i >= 0; i = s->subs[i].occ_next) {
            const uint8_t *k;
            size_t l;
            if (sub_key(&s->subs[i], &k, &l) && key_eq(k, l, key, key_len))
                mark_sub(s, &s->subs[i]);
        }
        for (size_t i = 0; i < s->pub_cap; i++) {
            const uint8_t *k;
            size_t l;
            if (pub_key(&s->publishes[i], &k, &l) && key_eq(k, l, key, key_len))
                mark_pub(s, &s->publishes[i]);
        }
        for (int32_t i = s->fetch_occ_head; i >= 0; i = s->fetches[i].occ_next) {
            const moq_fetch_entry_t *e = &s->fetches[i];
            if (key_eq(e->track_key, e->track_key_len, key, key_len))
                mark_fetch(s, &s->fetches[i]);
        }
    }
    mt_retry_pending(s);
}

void session_fail_malformed_alias(moq_session_t *s, uint64_t alias)
{
    const uint8_t *k = NULL;
    size_t l = 0;
    bool owner;
    if (!alias_key(s, alias, &k, &l, &owner)) {
        /* No key to name the Track by: the requests on the alias carry it. */
        for (int32_t i = s->sub_occ_head; i >= 0; i = s->subs[i].occ_next)
            if (s->subs[i].state == MOQ_SUB_ESTABLISHED && s->subs[i].track_alias == alias)
                mark_sub(s, &s->subs[i]);
        for (size_t i = 0; i < s->pub_cap; i++)
            if (s->publishes[i].state == MOQ_PUB_ESTABLISHED &&
                s->publishes[i].track_alias == alias)
                mark_pub(s, &s->publishes[i]);
    }
    session_fail_malformed_track(s, k, l);
}

void session_fail_malformed_fetch(moq_session_t *s, int fetch_slot)
{
    moq_fetch_entry_t *e = &s->fetches[fetch_slot];
    if (!e->track_key) mark_fetch(s, e);
    session_fail_malformed_track(s, e->track_key, e->track_key_len);
}

void session_fail_malformed_owner(moq_session_t *s, moq_subscription_t sub,
                                  moq_publication_t pub)
{
    const uint8_t *k = NULL;
    size_t l = 0;
    int ss = sub_resolve_handle(s, sub);
    if (ss >= 0) {
        if (!sub_key(&s->subs[ss], &k, &l)) mark_sub(s, &s->subs[ss]);
    } else {
        int ps = pub_resolve_handle(s, pub);
        if (ps >= 0 && !pub_key(&s->publishes[ps], &k, &l))
            mark_pub(s, &s->publishes[ps]);
    }
    session_fail_malformed_track(s, k, l);
}

bool session_alias_failing(const moq_session_t *s, uint64_t alias)
{
    if (s->mt_failing_count == 0) return false;
    for (int32_t i = s->sub_occ_head; i >= 0; i = s->subs[i].occ_next) {
        const moq_sub_entry_t *e = &s->subs[i];
        if (e->mt_failing && e->state == MOQ_SUB_ESTABLISHED && e->track_alias == alias)
            return true;
    }
    for (size_t i = 0; i < s->pub_cap; i++) {
        const moq_pub_entry_t *e = &s->publishes[i];
        if (e->mt_failing && e->state != MOQ_PUB_FREE && e->track_alias == alias)
            return true;
    }
    return false;
}

/* After one owed cancellation was attempted: false when the queues could not
 * take it (a full action or event queue, or a send buffer still holding
 * earlier control bytes): the request stays marked and the drive stops until
 * capacity returns. Any other outcome settles the mark; a request that no
 * longer owed a terminal is left as it is, and a cancellation that cannot be
 * made at all closes the session rather than leave the Track running. A
 * send-buffer refusal is temporary only while the buffer holds queued bytes,
 * which polling the actions releases; with the buffer empty the message can
 * never fit, and that cancellation cannot be made at all. */
static bool drive_settled(moq_session_t *s, moq_result_t rc, bool still_marked,
                          bool *mark)
{
    if (rc == MOQ_ERR_WOULD_BLOCK) return false;
    if (rc == MOQ_ERR_BUFFER && s->send_len > 0) return false;
    if (s->state == MOQ_SESS_CLOSED) return false;
    if (rc < 0) {
        (void)close_with_error(s, 0x1, "malformed Track cancellation failed");
        return false;
    }
    if (still_marked) {
        *mark = false;
        s->mt_failing_count--;
    }
    return true;
}

void mt_retry_pending(moq_session_t *s)
{
    if (s->mt_failing_count == 0 || !session_is_active(s)) return;
    for (int32_t i = s->sub_occ_head, nxt; i >= 0; i = nxt) {
        nxt = s->subs[i].occ_next;
        if (!s->subs[i].mt_failing) continue;
        moq_subscription_t h = s->subs[i].handle;
        moq_result_t rc = session_sub_fail_malformed(s, (int)i);
        int still = sub_resolve_handle(s, h);
        bool dummy = false;
        if (!drive_settled(s, rc, still >= 0 && s->subs[still].mt_failing,
                           still >= 0 ? &s->subs[still].mt_failing : &dummy))
            return;
    }
    for (size_t i = 0; i < s->pub_cap; i++) {
        if (!s->publishes[i].mt_failing) continue;
        moq_publication_t h = s->publishes[i].handle;
        moq_result_t rc = s->publishes[i].state == MOQ_PUB_PENDING_SUBSCRIBER
            ? session_pub_reject_pending_malformed(s, (int)i)
            : session_pub_fail_malformed(s, (int)i);
        int still = pub_resolve_handle(s, h);
        bool dummy = false;
        if (!drive_settled(s, rc, still >= 0 && s->publishes[still].mt_failing,
                           still >= 0 ? &s->publishes[still].mt_failing : &dummy))
            return;
    }
    for (int32_t i = s->fetch_occ_head, nxt; i >= 0; i = nxt) {
        nxt = s->fetches[i].occ_next;
        if (!s->fetches[i].mt_failing) continue;
        moq_fetch_t h = s->fetches[i].handle;
        moq_result_t rc = session_fetch_fail_malformed(s, (int)i);
        int still = fetch_resolve_handle(s, h);
        bool dummy = false;
        if (!drive_settled(s, rc, still >= 0 && s->fetches[still].mt_failing,
                           still >= 0 ? &s->fetches[still].mt_failing : &dummy))
            return;
    }
}
