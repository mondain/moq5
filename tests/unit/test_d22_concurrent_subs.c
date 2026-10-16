/*
 * Concurrent subscriptions to one track (draft-22 §3.1, §3.1.3).
 *
 * "An endpoint MAY have multiple concurrent subscriptions to the same Track,
 * each identified by a unique Request ID. A publisher MAY assign the same or
 * different Track Aliases to these subscriptions. When an Object matches the
 * filters of multiple subscriptions to the same Track, the publisher MUST
 * send the Object once for each matching subscription, even when those
 * subscriptions share the same Track Alias. Because subscriptions can share a
 * Track Alias, the subscriber re-applies each subscription's filter to
 * determine which subscription a received Object belongs to."
 *
 * Pinned on draft-22, through a simulated pair:
 * - several subscriptions to one track coexist, each with its own filter,
 *   under distinct aliases and under one shared alias;
 * - on a shared alias every received Object (stream or datagram, whole or
 *   chunked) is surfaced once to each subscription whose filter (Location
 *   window AND Forward) admits it, and discarded when none does;
 * - each subscription's subgroup closure and Stream Count are its own: a
 *   stream attributable to exactly one subscription counts toward it, so its
 *   PUBLISH_DONE completes; ending one subscription leaves the others intact;
 * - a filter update or a Forward change on one subscription changes only its
 *   own attribution; a fill on one is that subscription's alone;
 * - a shared alias naming a different track is still DUPLICATE_TRACK_ALIAS.
 * Drafts 16 and 18 keep their own rule: a second subscription to a track is
 * refused locally.
 */
#include <moq/moq.h>
#include <moq/publisher.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_bytes_t g_ns_parts[] = { { (const uint8_t *)"live", 4 } };

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    uint64_t now;
} rig_t;

#define MAX_LOCS 64
typedef struct {
    moq_subscription_t csub, ssub;
    moq_publication_t cpub, spub;     /* a PUBLISH-initiated subscription */
    uint64_t alias;
    size_t objs;
    uint64_t locs[MAX_LOCS];     /* (group << 16) | object, in arrival order */
    size_t datagrams;
    size_t finished, resets;
    size_t chunk_begin, chunk_end, chunk_bytes, chunk_reset;
    size_t done;
    uint64_t done_count;
    size_t fill_objs;
    size_t update_ok;
} subrec_t;

static bool rig_open(rig_t *r, moq_version_t v, bool streaming, uint32_t max_events)
{
    memset(r, 0, sizeof(*r));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = v;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    scfg.client_streaming_objects = streaming;
    scfg.max_events = max_events;
    if (moq_simpair_create(&scfg, &r->sp) < 0) return false;
    if (moq_simpair_start(r->sp) < 0) { moq_simpair_destroy(r->sp); return false; }
    moq_simpair_run_until_quiescent(r->sp, 32, NULL);
    r->c = moq_simpair_client(r->sp);
    r->sv = moq_simpair_server(r->sp);
    r->now = 10;
    moq_event_t ev;
    while (moq_session_poll_events(r->c, &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(r->sv, &ev, 1) > 0) moq_event_cleanup(&ev);
    moq_simpair_run_until_quiescent(r->sp, 32, NULL);
    return true;
}

/* The simulated transport does not re-deliver to a stream that answered
 * WOULD_BLOCK; a real bridge re-drives it once the application has polled.
 * Do that here for every client stream parked on an owed event. */
static void redrive(rig_t *r)
{
    for (size_t i = 0; i < r->c->rx_cap; i++) {
        moq_rx_stream_t *x = &r->c->rx_streams[i];
        if (!x->active) continue;
        if (x->parse_state == MOQ_RX_PENDING_EMIT ||
            x->parse_state == MOQ_RX_PENDING_CHUNK ||
            x->parse_state == MOQ_RX_PENDING_FINISHED ||
            x->parse_state == MOQ_RX_PENDING_RESET)
            (void)moq_session_on_data_bytes(r->c, x->stream_ref, NULL, 0, false,
                                             ++r->now);
    }
}

static void pump(rig_t *r)
{
    moq_simpair_run_until_quiescent(r->sp, 256, NULL);
}

/* Poll `s` (pumping and re-driving in between, for small event queues) until
 * an event of `kind` naming `sub` (any subscription when invalid) arrives;
 * every other event is discarded. */
static bool take(rig_t *r, moq_session_t *s, moq_event_kind_t kind,
                 moq_subscription_t sub, moq_event_t *out)
{
    for (int round = 0; round < 64; round++) {
        moq_event_t ev;
        while (moq_session_poll_events(s, &ev, 1) > 0) {
            moq_subscription_t h = ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST
                ? ev.u.subscribe_request.sub
                : ev.kind == MOQ_EVENT_SUBSCRIBE_OK ? ev.u.subscribe_ok.sub
                : MOQ_SUBSCRIPTION_INVALID;
            if (ev.kind == kind &&
                (!moq_subscription_is_valid(sub) || moq_subscription_eq(h, sub))) {
                *out = ev;
                return true;
            }
            moq_event_cleanup(&ev);
        }
        pump(r);
        redrive(r);
    }
    return false;
}

static subrec_t *find_rec2(subrec_t **recs, size_t n, moq_subscription_t h,
                           moq_publication_t p)
{
    for (size_t i = 0; i < n; i++) {
        if (!recs[i]) continue;
        if (moq_subscription_is_valid(h) && moq_subscription_eq(recs[i]->csub, h))
            return recs[i];
        if (moq_publication_is_valid(p) && moq_publication_is_valid(recs[i]->cpub) &&
            moq_publication_eq(recs[i]->cpub, p))
            return recs[i];
    }
    return NULL;
}

static subrec_t *find_rec(subrec_t **recs, size_t n, moq_subscription_t h)
{
    return find_rec2(recs, n, h, MOQ_PUBLICATION_INVALID);
}

/* Drain the client's events into the records; returns objects that named no
 * known subscription. Pumps between polls so a small event queue refills. */
static size_t drain_client(rig_t *r, subrec_t **recs, size_t n)
{
    size_t stray = 0;
    int idle = 0;
    for (int round = 0; round < 64 && idle < 2; round++) {
        moq_event_t ev;
        size_t got = 0;
        while (moq_session_poll_events(r->c, &ev, 1) > 0) {
            got++;
            subrec_t *rec = NULL;
            switch (ev.kind) {
            case MOQ_EVENT_OBJECT_RECEIVED:
                rec = find_rec2(recs, n, ev.u.object_received.sub, ev.u.object_received.pub);
                if (!rec) { stray++; break; }
                if (rec->objs < MAX_LOCS)
                    rec->locs[rec->objs] = (ev.u.object_received.group_id << 16) |
                                           ev.u.object_received.object_id;
                rec->objs++;
                if (ev.u.object_received.datagram) rec->datagrams++;
                break;
            case MOQ_EVENT_OBJECT_CHUNK:
                rec = find_rec2(recs, n, ev.u.object_chunk.sub, ev.u.object_chunk.pub);
                if (!rec) { stray++; break; }
                if (ev.u.object_chunk.begin) rec->chunk_begin++;
                if (ev.u.object_chunk.chunk)
                    rec->chunk_bytes += moq_rcbuf_len(ev.u.object_chunk.chunk);
                if (ev.u.object_chunk.terminal == MOQ_OBJECT_TERMINAL_RESET) {
                    rec->chunk_reset++;
                } else if (ev.u.object_chunk.end) {
                    rec->chunk_end++;
                    if (rec->objs < MAX_LOCS)
                        rec->locs[rec->objs] = (ev.u.object_chunk.group_id << 16) |
                                               ev.u.object_chunk.object_id;
                    rec->objs++;
                }
                break;
            case MOQ_EVENT_SUBGROUP_FINISHED:
                rec = find_rec2(recs, n, ev.u.subgroup_finished.sub, ev.u.subgroup_finished.pub);
                if (rec) rec->finished++;
                break;
            case MOQ_EVENT_SUBGROUP_RESET:
                rec = find_rec2(recs, n, ev.u.subgroup_reset.sub, ev.u.subgroup_reset.pub);
                if (rec) rec->resets++;
                break;
            case MOQ_EVENT_SUBSCRIBE_DONE:
                rec = find_rec(recs, n, ev.u.subscribe_done.sub);
                if (rec) { rec->done++; rec->done_count = ev.u.subscribe_done.stream_count; }
                break;
            case MOQ_EVENT_FILL_OBJECT:
                rec = find_rec(recs, n, ev.u.fill_object.sub);
                if (rec) rec->fill_objs++;
                break;
            case MOQ_EVENT_SUBSCRIPTION_UPDATE_OK:
                rec = find_rec(recs, n, ev.u.subscription_update_ok.sub);
                if (rec) rec->update_ok++;
                break;
            default:
                break;
            }
            moq_event_cleanup(&ev);
        }
        pump(r);
        redrive(r);
        idle = got == 0 ? idle + 1 : 0;
    }
    return stray;
}

static void drain_server(rig_t *r)
{
    moq_event_t ev;
    while (moq_session_poll_events(r->sv, &ev, 1) > 0) moq_event_cleanup(&ev);
}

/* Subscribe to `name` with the given legacy filter and have the server
 * accept, optionally with an explicit alias. Fills rec->csub/ssub/alias. */
static bool sub_accept_ex(rig_t *r, subrec_t *rec, const char *name,
                          moq_subscribe_filter_t filter, uint64_t sg, uint64_t eg,
                          const moq_location_filter_cfg_t *loc,
                          bool explicit_alias, uint64_t alias,
                          const moq_fill_cfg_t *fill)
{
    memset(rec, 0, sizeof(*rec));
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    cfg.filter = filter;
    cfg.start_group = sg;
    cfg.start_object = 0;
    cfg.end_group = eg;
    cfg.location = loc;
    cfg.fill = fill;
    if (moq_session_subscribe(r->c, &cfg, ++r->now, &rec->csub) != MOQ_OK) return false;
    pump(r);
    moq_event_t ev;
    if (!take(r, r->sv, MOQ_EVENT_SUBSCRIBE_REQUEST, MOQ_SUBSCRIPTION_INVALID, &ev))
        return false;
    rec->ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_track_alias = explicit_alias;
    ac.track_alias = alias;
    ac.has_largest = true;
    ac.largest_group = 0;
    ac.largest_object = 0;
    if (moq_session_accept_subscribe(r->sv, rec->ssub, &ac, ++r->now) != MOQ_OK) return false;
    pump(r);
    if (!take(r, r->c, MOQ_EVENT_SUBSCRIBE_OK, rec->csub, &ev)) return false;
    rec->alias = ev.u.subscribe_ok.track_alias;
    moq_event_cleanup(&ev);
    return true;
}

static bool sub_accept(rig_t *r, subrec_t *rec, const char *name,
                       moq_subscribe_filter_t filter, uint64_t sg, uint64_t eg,
                       bool explicit_alias, uint64_t alias,
                       const moq_fill_cfg_t *fill)
{
    return sub_accept_ex(r, rec, name, filter, sg, eg, NULL, explicit_alias,
                         alias, fill);
}

static moq_rcbuf_t *mk_payload(const char *s)
{
    moq_rcbuf_t *pl = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)s, strlen(s), &pl);
    return pl;
}

/* The publisher opens one subgroup stream of `rec`'s subscription in `group`
 * carrying objects first..first+n-1; closed unless out_sg is given. */
static bool send_stream(rig_t *r, const subrec_t *rec, uint64_t group,
                        uint64_t first, uint64_t n, moq_subgroup_handle_t *out_sg)
{
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    sc.group_id = group;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(r->sv, rec->ssub, &sc, ++r->now, &sg) != MOQ_OK)
        return false;
    for (uint64_t i = 0; i < n; i++) {
        moq_rcbuf_t *pl = mk_payload("payload-bytes");
        moq_result_t rc = moq_session_write_object(r->sv, sg, first + i, pl, ++r->now);
        moq_rcbuf_decref(pl);
        if (rc != MOQ_OK) return false;
    }
    if (out_sg) { *out_sg = sg; pump(r); return true; }
    bool ok = moq_session_close_subgroup(r->sv, sg, ++r->now) == MOQ_OK;
    pump(r);
    return ok;
}

/* As send_stream, but the stream's bytes are handed to the client directly,
 * the way a transport bridge does: a WOULD_BLOCK answer means the session
 * retained the bytes, and the stream is re-driven after the application
 * polls (drain_client). Used with event queues smaller than the number of
 * subscriptions an object reaches, which the simulated transport does not
 * re-drive. */
static bool send_stream_direct(rig_t *r, const subrec_t *rec, uint64_t group,
                               uint64_t first, uint64_t n, uint64_t ref)
{
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    sc.group_id = group;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(r->sv, rec->ssub, &sc, ++r->now, &sg) != MOQ_OK)
        return false;
    for (uint64_t i = 0; i < n; i++) {
        moq_rcbuf_t *pl = mk_payload("payload-bytes");
        moq_result_t rc = moq_session_write_object(r->sv, sg, first + i, pl, ++r->now);
        moq_rcbuf_decref(pl);
        if (rc != MOQ_OK) return false;
    }
    if (moq_session_close_subgroup(r->sv, sg, ++r->now) != MOQ_OK) return false;
    moq_action_t act;
    bool ok = true;
    while (moq_session_poll_actions(r->sv, &act, 1) > 0) {
        if (act.kind == MOQ_ACTION_SEND_DATA) {
            const moq_send_data_action_t *d = &act.u.send_data;
            uint8_t buf[256];
            size_t len = d->header_len;
            memcpy(buf, d->header, len);
            if (d->payload) {
                size_t pl = moq_rcbuf_len(d->payload);
                if (pl > sizeof(buf) - len) ok = false;
                else { memcpy(buf + len, moq_rcbuf_data(d->payload), pl); len += pl; }
            }
            moq_result_t rc = moq_session_on_data_bytes(r->c, moq_stream_ref_from_u64(ref),
                                                        buf, len, d->fin, ++r->now);
            if (rc < 0 && rc != MOQ_ERR_WOULD_BLOCK) ok = false;
        }
        moq_action_cleanup(&act);
    }
    return ok;
}

static bool send_dgram(rig_t *r, const subrec_t *rec, uint64_t group, uint64_t object)
{
    moq_rcbuf_t *pl = mk_payload("dg");
    moq_result_t rc = moq_session_send_object_datagram(r->sv, rec->ssub, group, object,
                                                       3, false, pl, NULL, 0, ++r->now);
    moq_rcbuf_decref(pl);
    pump(r);
    return rc == MOQ_OK;
}

static bool has_loc(const subrec_t *rec, uint64_t g, uint64_t o)
{
    for (size_t i = 0; i < rec->objs && i < MAX_LOCS; i++)
        if (rec->locs[i] == ((g << 16) | o)) return true;
    return false;
}

static size_t count_loc(const subrec_t *rec, uint64_t g, uint64_t o)
{
    size_t n = 0;
    for (size_t i = 0; i < rec->objs && i < MAX_LOCS; i++)
        if (rec->locs[i] == ((g << 16) | o)) n++;
    return n;
}

static bool established(rig_t *r)
{
    return moq_session_state(r->c) == MOQ_SESS_ESTABLISHED &&
           moq_session_state(r->sv) == MOQ_SESS_ESTABLISHED;
}

#define RANGE MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE
#define START MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START

/* Two subscriptions with disjoint filters, distinct aliases (the server
 * numbers them itself): each receives exactly its own objects. */
static int test_disjoint_distinct(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    MOQ_TEST_CHECK(moq_session_supports_concurrent_subscriptions(r.c));
    MOQ_TEST_CHECK(!moq_session_supports_concurrent_subscriptions(NULL));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 2, 0, false, 0, NULL));
    MOQ_TEST_CHECK(a.alias != b.alias);
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 2, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 3, NULL));
    subrec_t *recs[] = { &a, &b };
    MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 2), 0);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 3);
    MOQ_TEST_CHECK(has_loc(&a, 1, 1) && !has_loc(&a, 2, 0));
    MOQ_TEST_CHECK(has_loc(&b, 2, 2) && !has_loc(&b, 1, 0));
    MOQ_TEST_CHECK_EQ_SIZE(a.finished, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.finished, 1);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Two subscriptions with disjoint filters under ONE alias: the subscriber
 * re-applies each filter, so each still receives exactly its own objects,
 * and each stream's closure reaches the one subscription it belongs to. */
static int test_disjoint_shared(bool streaming)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, streaming, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 2, 0, true, a.alias, NULL));
    MOQ_TEST_CHECK_EQ_U64(b.alias, a.alias);
    MOQ_TEST_CHECK(established(&r));
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 2, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 3, NULL));
    subrec_t *recs[] = { &a, &b };
    MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 2), 0);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 3);
    MOQ_TEST_CHECK(has_loc(&a, 1, 0) && has_loc(&a, 1, 1) && !has_loc(&a, 2, 0));
    MOQ_TEST_CHECK(has_loc(&b, 2, 0) && has_loc(&b, 2, 2) && !has_loc(&b, 1, 0));
    if (streaming) {
        MOQ_TEST_CHECK_EQ_SIZE(a.chunk_begin, 2);
        MOQ_TEST_CHECK_EQ_SIZE(b.chunk_begin, 3);
        MOQ_TEST_CHECK_EQ_SIZE(b.chunk_bytes, 3 * strlen("payload-bytes"));
    }
    MOQ_TEST_CHECK_EQ_SIZE(a.finished, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.finished, 1);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Overlapping filters. Distinct aliases: each subscription receives the copy
 * sent for it, exactly once. A shared alias: two copies arrive and neither
 * can be told apart, so every copy is offered to every subscription whose
 * filter admits it (deduplication is the subscriber application's, §3.1).
 * In streaming receive one exception keeps chunk routing by (group,
 * subgroup, object) sound: a copy that begins while the same object is still
 * being surfaced from the other stream is consumed without events, so the
 * two runs never interleave. Every subscription still receives the object
 * once, through the copy already in flight. */
static int test_overlap(bool shared, bool streaming, uint32_t max_events)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, streaming, max_events));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, shared, a.alias, NULL));
    MOQ_TEST_CHECK(shared ? b.alias == a.alias : b.alias != a.alias);
    subrec_t *recs[] = { &a, &b };
    if (max_events == 0) {
        MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 2, NULL));
        /* Queued and not yet polled: every copy is charged to the budget. */
        if (!streaming)
            MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes,
                                   (shared ? 4 : 2) * strlen("payload-bytes"));
        MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 2), 0);
        MOQ_TEST_CHECK(send_stream(&r, &b, 1, 0, 2, NULL));
    } else {
        MOQ_TEST_CHECK(send_stream_direct(&r, &a, 1, 0, 2, 0x7001));
        MOQ_TEST_CHECK(send_stream_direct(&r, &b, 1, 0, 2, 0x7002));
    }
    MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 2), 0);
    /* With one event slot the second stream's copies begin while the
     * first's are in flight. */
    size_t copies = shared && !(streaming && max_events != 0) ? 2 : 1;
    MOQ_TEST_CHECK_EQ_SIZE(count_loc(&a, 1, 0), copies);
    MOQ_TEST_CHECK_EQ_SIZE(count_loc(&a, 1, 1), copies);
    MOQ_TEST_CHECK_EQ_SIZE(count_loc(&b, 1, 0), copies);
    MOQ_TEST_CHECK_EQ_SIZE(count_loc(&b, 1, 1), copies);
    /* Each stream still ends once, whatever it surfaced. */
    MOQ_TEST_CHECK_EQ_SIZE(a.finished, shared ? 2u : 1u);
    MOQ_TEST_CHECK_EQ_SIZE(b.finished, shared ? 2u : 1u);
    if (streaming) {
        MOQ_TEST_CHECK_EQ_SIZE(a.chunk_begin, 2 * copies);
        MOQ_TEST_CHECK_EQ_SIZE(a.chunk_end, 2 * copies);
        MOQ_TEST_CHECK_EQ_SIZE(b.chunk_bytes, 2 * copies * strlen("payload-bytes"));
    }
    /* Every duplicated buffer was released through the events: the receive
     * budget is back to what open streams hold. */
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Three subscriptions under one alias plus one under its own: every object
 * reaches exactly the subscriptions whose window admits it; an object no
 * window admits is discarded and the session stays up. */
static int test_three_and_mixed(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b, c, d;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", RANGE, 2, 2, true, a.alias, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &c, "t", START, 3, 0, true, a.alias, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &d, "t", START, 2, 0, false, 0, NULL));
    MOQ_TEST_CHECK(c.alias == a.alias && d.alias != a.alias);
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 1, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &c, 3, 0, 1, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &d, 2, 0, 1, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &d, 3, 0, 1, NULL));
    /* Group 0 is outside every window sharing the alias. */
    MOQ_TEST_CHECK(send_stream(&r, &a, 0, 0, 1, NULL));
    subrec_t *recs[] = { &a, &b, &c, &d };
    MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 4), 0);
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 1, 0));
    MOQ_TEST_CHECK(b.objs == 1 && has_loc(&b, 2, 0));
    MOQ_TEST_CHECK(c.objs == 1 && has_loc(&c, 3, 0));
    MOQ_TEST_CHECK(d.objs == 2 && has_loc(&d, 2, 0) && has_loc(&d, 3, 0));
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Datagrams on a shared alias are attributed the same way. */
static int test_datagrams_shared(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, true, a.alias, NULL));
    MOQ_TEST_CHECK(send_dgram(&r, &b, 2, 0));       /* B only */
    MOQ_TEST_CHECK(send_dgram(&r, &a, 1, 4));       /* both windows */
    /* Each queued copy holds its own share of the receive budget, which
     * the poll releases per event: 2 bytes for B's, 2 x 2 for the shared. */
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 6);
    subrec_t *recs[] = { &a, &b };
    MOQ_TEST_CHECK_EQ_SIZE(drain_client(&r, recs, 2), 0);
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 1, 4) && a.datagrams == 1);
    MOQ_TEST_CHECK(b.objs == 2 && has_loc(&b, 2, 0) && has_loc(&b, 1, 4));
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Ending one subscription stops only the streams that belong to it alone;
 * the others keep receiving, on open and on new streams. */
static int test_unsubscribe_one(bool shared)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 2, 0, shared, a.alias, NULL));
    moq_subgroup_handle_t sga, sgb;
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, &sga));
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 1, &sgb));
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && b.objs == 1);
    MOQ_TEST_CHECK(moq_session_unsubscribe(r.c, a.csub, ++r.now) == MOQ_OK);
    /* A's open stream is stopped (one STOP_DATA, CANCELLED); B's is not.
     * The actions are taken here, so the publisher keeps A's subscription:
     * only the subscriber's attribution is under test. */
    size_t stops = 0;
    moq_action_t act;
    while (moq_session_poll_actions(r.c, &act, 1) > 0) {
        if (act.kind == MOQ_ACTION_STOP_DATA) {
            stops++;
            MOQ_TEST_CHECK_EQ_U64(act.u.stop_data.error_code, 0x1);
        }
        moq_action_cleanup(&act);
    }
    MOQ_TEST_CHECK_EQ_SIZE(stops, 1);
    pump(&r);
    drain_server(&r);
    moq_rcbuf_t *pl = mk_payload("more");
    MOQ_TEST_CHECK(moq_session_write_object(r.sv, sgb, 1, pl, ++r.now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    MOQ_TEST_CHECK(moq_session_close_subgroup(r.sv, sgb, ++r.now) == MOQ_OK);
    pump(&r);
    MOQ_TEST_CHECK(send_stream(&r, &b, 3, 0, 1, NULL));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 3);
    MOQ_TEST_CHECK(has_loc(&b, 2, 1) && has_loc(&b, 3, 0));
    MOQ_TEST_CHECK_EQ_SIZE(b.finished, 2);
    MOQ_TEST_CHECK(sub_resolve_handle(r.c, b.csub) >= 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* A subgroup reset on a shared stream closes it once for each subscription
 * it reached: B (object {1,0} only) and A (from {1,0}). Whole objects: one
 * SUBGROUP_RESET each. Streaming, reset inside object 1 (A's alone): A's
 * chunk sequence ends with the reset terminal, B gets SUBGROUP_RESET. */
static int test_reset_shared(bool streaming)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, streaming, 0));
    moq_location_filter_cfg_t loc; moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_OBJECT_END;
    loc.start_group = 1; loc.start_object = 0;
    loc.end_group = 1; loc.end_object = 0;
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept_ex(&r, &b, "t", START, 0, 0, &loc, true, a.alias, NULL));
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, &sg));
    if (streaming) {
        MOQ_TEST_CHECK(moq_session_begin_object(r.sv, sg, 1, 10, ++r.now) == MOQ_OK);
        moq_rcbuf_t *pl = mk_payload("part");
        MOQ_TEST_CHECK(moq_session_write_object_data(r.sv, sg, pl, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
    } else {
        moq_rcbuf_t *pl = mk_payload("whole");
        MOQ_TEST_CHECK(moq_session_write_object(r.sv, sg, 1, pl, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
    }
    pump(&r);
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(moq_session_reset_subgroup(r.sv, sg, 0x2, ++r.now) == MOQ_OK);
    pump(&r);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.resets, 1);
    if (streaming) {
        MOQ_TEST_CHECK_EQ_SIZE(a.chunk_begin, 2);
        MOQ_TEST_CHECK_EQ_SIZE(a.chunk_reset, 1);
        MOQ_TEST_CHECK_EQ_SIZE(a.resets, 0);
        MOQ_TEST_CHECK_EQ_SIZE(b.chunk_reset, 0);
    } else {
        MOQ_TEST_CHECK_EQ_SIZE(a.objs, 2);
        MOQ_TEST_CHECK_EQ_SIZE(a.resets, 1);
    }
    MOQ_TEST_CHECK_EQ_SIZE(a.finished + b.finished, 0);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Overlapping filters on one alias: a stream that reached both stays open
 * while A ends (B still uses it) and is stopped once B ends too. */
static int test_unsubscribe_both_shared(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, true, a.alias, NULL));
    moq_subgroup_handle_t sgb;
    MOQ_TEST_CHECK(send_stream(&r, &b, 1, 0, 1, &sgb));
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && b.objs == 1);
    size_t stops[2] = { 0, 0 };
    for (int round = 0; round < 2; round++) {
        MOQ_TEST_CHECK(moq_session_unsubscribe(r.c, round ? b.csub : a.csub,
                                               ++r.now) == MOQ_OK);
        moq_action_t act;
        while (moq_session_poll_actions(r.c, &act, 1) > 0) {
            if (act.kind == MOQ_ACTION_STOP_DATA) stops[round]++;
            moq_action_cleanup(&act);
        }
    }
    MOQ_TEST_CHECK_EQ_SIZE(stops[0], 0);
    MOQ_TEST_CHECK_EQ_SIZE(stops[1], 1);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

static bool done_sub(rig_t *r, const subrec_t *rec, uint64_t count)
{
    moq_done_subscribe_cfg_t dc; moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x2;
    dc.stream_count = count;
    moq_result_t rc = moq_session_done_subscribe(r->sv, rec->ssub, &dc, ++r->now);
    pump(r);
    return rc == MOQ_OK;
}

/* PUBLISH_DONE's Stream Count is per subscription. With disjoint filters
 * each stream is attributable to one subscription, so each subscription's
 * count is met by its own streams, shared alias or not, and ending one
 * leaves the other established. */
static int test_stream_count(bool shared)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 2, 0, shared, a.alias, NULL));
    /* An empty subgroup of A's in group 1 counts too: its group lies only in
     * A's window. */
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 2, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 5, 0, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 1, NULL));
    MOQ_TEST_CHECK(done_sub(&r, &a, 2));
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.done, 1);
    MOQ_TEST_CHECK_EQ_U64(a.done_count, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.done, 0);
    MOQ_TEST_CHECK(sub_resolve_handle(r.c, b.csub) >= 0);
    MOQ_TEST_CHECK(send_stream(&r, &b, 3, 0, 1, NULL));
    MOQ_TEST_CHECK(done_sub(&r, &b, 2));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.done, 1);
    MOQ_TEST_CHECK_EQ_U64(b.done_count, 2);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Overlapping filters under one alias: no stream is attributable to one
 * subscription, so neither count is met by counting; each PUBLISH_DONE is
 * then completed by the subscriber's terminal timer, not early. */
static int test_stream_count_ambiguous(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, true, a.alias, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &b, 1, 0, 1, NULL));
    MOQ_TEST_CHECK(done_sub(&r, &a, 1));
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.done, 0);
    int slot = sub_resolve_handle(r.c, a.csub);
    MOQ_TEST_CHECK(slot >= 0 && r.c->subs[slot].done_pending);
    MOQ_TEST_CHECK(slot >= 0 && r.c->subs[slot].processed_stream_count == 0);
    /* Past the terminal wait the retained done surfaces. */
    MOQ_TEST_CHECK(moq_session_tick(r.c, UINT64_C(1) << 50) == MOQ_OK);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.done, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.done, 0);
    MOQ_TEST_CHECK(sub_resolve_handle(r.c, b.csub) >= 0);
    MOQ_TEST_CHECK(moq_session_state(r.c) == MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* A stream that reached two subscriptions is never counted toward one of
 * them, even after the other ended: it may have been the other's, and
 * counting it would complete B before B's own stream arrived. */
static int test_stream_count_after_unsubscribe(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, true, a.alias, NULL));
    moq_subgroup_handle_t sga;
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, &sga));
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && b.objs == 1);
    /* A ends on the subscriber only (its actions are taken here), so the
     * publisher still finishes A's stream normally. */
    MOQ_TEST_CHECK(moq_session_unsubscribe(r.c, a.csub, ++r.now) == MOQ_OK);
    moq_action_t act;
    while (moq_session_poll_actions(r.c, &act, 1) > 0) moq_action_cleanup(&act);
    MOQ_TEST_CHECK(moq_session_close_subgroup(r.sv, sga, ++r.now) == MOQ_OK);
    pump(&r);
    drain_client(&r, recs, 2);
    int slot = sub_resolve_handle(r.c, b.csub);
    MOQ_TEST_CHECK(slot >= 0 && r.c->subs[slot].processed_stream_count == 0);
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 1, NULL));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(slot >= 0 && r.c->subs[slot].processed_stream_count == 1);
    MOQ_TEST_CHECK(done_sub(&r, &b, 1));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.done, 1);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* A filter update and a Forward change on one subscription change only its
 * own attribution. */
static int test_update_one(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 1, 0, true, a.alias, NULL));
    subrec_t *recs[] = { &a, &b };
    /* Narrow A to groups >= 5. */
    moq_subscription_update_cfg_t up;
    moq_subscription_update_cfg_init_sized(&up, sizeof(up));
    up.has_filter = true;
    up.filter = START;
    up.start_group = 5;
    MOQ_TEST_CHECK(moq_session_update_subscription(r.c, a.csub, &up, ++r.now) == MOQ_OK);
    pump(&r);
    drain_server(&r);
    pump(&r);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.update_ok, 1);
    MOQ_TEST_CHECK(send_stream(&r, &b, 2, 0, 1, NULL));   /* B only now */
    MOQ_TEST_CHECK(send_stream(&r, &a, 5, 0, 1, NULL));   /* both */
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 5, 0));
    MOQ_TEST_CHECK(b.objs == 2 && has_loc(&b, 2, 0) && has_loc(&b, 5, 0));
    /* Pause B: what it would match now reaches A only. */
    moq_subscription_update_cfg_init_sized(&up, sizeof(up));
    up.has_forward = true;
    up.forward = false;
    MOQ_TEST_CHECK(moq_session_update_subscription(r.c, b.csub, &up, ++r.now) == MOQ_OK);
    pump(&r);
    drain_server(&r);
    pump(&r);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(b.update_ok, 1);
    MOQ_TEST_CHECK(send_stream(&r, &a, 6, 0, 1, NULL));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 2 && has_loc(&a, 6, 0));
    MOQ_TEST_CHECK_EQ_SIZE(b.objs, 2);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* A fill requested on one of two concurrent subscriptions is delivered to
 * that subscription only (fill streams carry the initiating Request ID). */
static int test_fill_one(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    moq_location_filter_cfg_t loc; moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    moq_fill_cfg_t fill; moq_fill_cfg_init(&fill);
    fill.location = &loc;
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &b, "t", START, 0, 0, false, 0, NULL));
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 0, 0, true, b.alias, &fill));
    /* The server's FILL_REQUEST for A's fill. */
    moq_fill_t sfill = { 0 };
    moq_event_t ev;
    while (moq_session_poll_events(r.sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_REQUEST) sfill = ev.u.fill_request.fill;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_fill_is_valid(sfill));
    if (moq_fill_is_valid(sfill)) {
        MOQ_TEST_CHECK(moq_session_open_fill(r.sv, sfill, ++r.now) == MOQ_OK);
        moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
        oc.group_id = 0; oc.object_id = 0; oc.publisher_priority = 3;
        oc.payload = mk_payload("fill");
        MOQ_TEST_CHECK(moq_session_write_fill_object(r.sv, sfill, &oc, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(oc.payload);
        MOQ_TEST_CHECK(moq_session_end_fill(r.sv, sfill, ++r.now) == MOQ_OK);
        pump(&r);
    }
    subrec_t *recs[] = { &a, &b };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.fill_objs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(b.fill_objs, 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* An alias shared with a DIFFERENT track is still a protocol violation
 * (§3.1.3: DUPLICATE_TRACK_ALIAS); the publisher side refuses to assign it. */
static int test_other_track_alias(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, b;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "x", START, 0, 0, false, 0, NULL));
    /* Our publisher refuses to share an alias across tracks. */
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("y");
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &b.csub) == MOQ_OK);
    pump(&r);
    moq_event_t ev;
    while (moq_session_poll_events(r.sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) b.ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_track_alias = true;
    ac.track_alias = a.alias;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, b.ssub, &ac, ++r.now) == MOQ_ERR_INVAL);
    /* Forced past that check, the subscriber closes the session. */
    int slot = -1;
    for (size_t i = 0; i < r.sv->sub_cap; i++)
        if (r.sv->subs[i].state == MOQ_SUB_ESTABLISHED) slot = (int)i;
    MOQ_TEST_CHECK(slot >= 0);
    if (slot >= 0) r.sv->subs[slot].track_alias = a.alias + 100;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, b.ssub, &ac, ++r.now) == MOQ_OK);
    pump(&r);
    MOQ_TEST_CHECK(moq_session_state(r.c) == MOQ_SESS_CLOSED);
    uint64_t code = 0;
    while (moq_session_poll_events(r.c, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SESSION_CLOSED) code = ev.u.closed.code;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_U64(code, 0x5);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* The publisher facade serves every concurrent subscription to its track
 * from one write, each through its own window: "the publisher MUST send the
 * Object once for each matching subscription". A (group 1 only), B (from
 * group 2) and C (from group 1, overlapping both). */
static int test_facade_publisher(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    moq_pub_cfg_t pc;
    moq_pub_cfg_init_sized(&pc, sizeof(pc));
    pc.accept_mode = MOQ_PUB_ACCEPT_ALL;
    moq_publisher_t *pub = NULL;
    MOQ_TEST_CHECK(moq_pub_create(r.sv, moq_alloc_default(), &pc, &pub) == MOQ_OK);
    moq_pub_track_cfg_t tc;
    moq_pub_track_cfg_init(&tc);
    tc.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("t");
    moq_pub_track_t *track = NULL;
    MOQ_TEST_CHECK(moq_pub_add_track(pub, &tc, ++r.now, &track) == MOQ_OK);
    subrec_t a, b, c;
    subrec_t *all[] = { &a, &b, &c };
    const moq_subscribe_filter_t f[] = { RANGE, START, START };
    const uint64_t sg[] = { 1, 2, 1 }, eg[] = { 1, 0, 0 };
    for (int i = 0; i < 3; i++) {
        memset(all[i], 0, sizeof(subrec_t));
        moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        cfg.filter = f[i];
        cfg.start_group = sg[i];
        cfg.end_group = eg[i];
        MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &all[i]->csub) == MOQ_OK);
        pump(&r);
        MOQ_TEST_CHECK(moq_pub_tick(pub, ++r.now) == MOQ_OK);
        pump(&r);
        moq_event_t ev;
        MOQ_TEST_CHECK(take(&r, r.c, MOQ_EVENT_SUBSCRIBE_OK, all[i]->csub, &ev));
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_SIZE(moq_pub_active_subscriptions(pub, track), 3);
    for (uint64_t g = 1; g <= 2; g++) {
        moq_rcbuf_t *pl = mk_payload("facade");
        MOQ_TEST_CHECK(moq_pub_write_object(pub, track, g, 0, pl, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
        pump(&r);
        MOQ_TEST_CHECK(moq_pub_tick(pub, ++r.now) == MOQ_OK);
        drain_client(&r, all, 3);
    }
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 1, 0));
    MOQ_TEST_CHECK(b.objs == 1 && has_loc(&b, 2, 0));
    MOQ_TEST_CHECK(c.objs == 2 && has_loc(&c, 1, 0) && has_loc(&c, 2, 0));
    /* Ending one leaves the others served. */
    MOQ_TEST_CHECK(moq_session_unsubscribe(r.c, c.csub, ++r.now) == MOQ_OK);
    pump(&r);
    MOQ_TEST_CHECK(moq_pub_tick(pub, ++r.now) == MOQ_OK);
    pump(&r);
    MOQ_TEST_CHECK_EQ_SIZE(moq_pub_active_subscriptions(pub, track), 2);
    moq_rcbuf_t *pl = mk_payload("facade");
    MOQ_TEST_CHECK(moq_pub_write_object(pub, track, 2, 1, pl, ++r.now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    pump(&r);
    drain_client(&r, all, 3);
    MOQ_TEST_CHECK(b.objs == 2 && has_loc(&b, 2, 1));
    MOQ_TEST_CHECK_EQ_SIZE(c.objs, 2);
    MOQ_TEST_CHECK(established(&r));
    moq_pub_destroy(pub);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* -- A PUBLISH-initiated subscription sharing the alias ------------ */

/* The server publishes track `name` to the client, optionally under an
 * explicit alias; the client accepts. */
static bool publish_accept(rig_t *r, subrec_t *rec, const char *name,
                           bool explicit_alias, uint64_t alias)
{
    memset(rec, 0, sizeof(*rec));
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    cfg.has_track_alias = explicit_alias;
    cfg.track_alias = alias;
    if (moq_session_publish(r->sv, &cfg, ++r->now, &rec->spub) != MOQ_OK) return false;
    pump(r);
    moq_event_t ev;
    bool got = false;
    for (int round = 0; round < 8 && !got; round++) {
        while (moq_session_poll_events(r->c, &ev, 1) > 0) {
            if (!got && ev.kind == MOQ_EVENT_PUBLISH_REQUEST) {
                rec->cpub = ev.u.publish_request.pub;
                rec->alias = ev.u.publish_request.track_alias;
                got = true;
            }
            moq_event_cleanup(&ev);
        }
        if (!got) pump(r);
    }
    if (!got) return false;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    if (moq_session_accept_publish(r->c, rec->cpub, &ac, ++r->now) != MOQ_OK) return false;
    pump(r);
    drain_server(r);
    return true;
}

static bool send_pub_stream(rig_t *r, const subrec_t *rec, uint64_t group,
                            uint64_t first, uint64_t n)
{
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    sc.group_id = group;
    moq_subgroup_handle_t sg;
    if (moq_session_open_pub_subgroup(r->sv, rec->spub, &sc, ++r->now, &sg) != MOQ_OK)
        return false;
    for (uint64_t i = 0; i < n; i++) {
        moq_rcbuf_t *pl = mk_payload("payload-bytes");
        moq_result_t rc = moq_session_write_object(r->sv, sg, first + i, pl, ++r->now);
        moq_rcbuf_decref(pl);
        if (rc != MOQ_OK) return false;
    }
    bool ok = moq_session_close_subgroup(r->sv, sg, ++r->now) == MOQ_OK;
    pump(r);
    return ok;
}

/* Draft-22 §3.1.3 closes only on an alias naming "a different Track": a
 * PUBLISH-initiated subscription and a SUBSCRIBE-initiated one to the same
 * track may share an alias, in either order. Objects are attributed across
 * both kinds by filter (the publication here is unfiltered, A starts at
 * group 2) and the stream reaching only the publication counts toward it. */
static int test_publication_shares_alias(bool publish_first)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, p;
    if (publish_first) {
        MOQ_TEST_CHECK(publish_accept(&r, &p, "t", false, 0));
        MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 2, 0, true, p.alias, NULL));
    } else {
        MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 2, 0, false, 0, NULL));
        MOQ_TEST_CHECK(publish_accept(&r, &p, "t", true, a.alias));
    }
    MOQ_TEST_CHECK_EQ_U64(p.alias, a.alias);
    MOQ_TEST_CHECK(established(&r));
    subrec_t *recs[] = { &a, &p };
    MOQ_TEST_CHECK(send_pub_stream(&r, &p, 0, 0, 2));    /* the publication only */
    drain_client(&r, recs, 2);
    int ps = pub_resolve_handle(r.c, p.cpub);
    MOQ_TEST_CHECK(ps >= 0 && r.c->publishes[ps].processed_stream_count == 1);
    MOQ_TEST_CHECK(send_stream(&r, &a, 2, 0, 1, NULL));  /* both windows */
    moq_rcbuf_t *pl = mk_payload("dg");
    MOQ_TEST_CHECK(moq_session_send_pub_object_datagram(r.sv, p.spub, 1, 3, 3, false, pl,
                                                        NULL, 0, ++r.now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    pump(&r);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 2, 0));
    MOQ_TEST_CHECK(p.objs == 4 && has_loc(&p, 0, 1) && has_loc(&p, 2, 0) && has_loc(&p, 1, 3));
    MOQ_TEST_CHECK_EQ_SIZE(p.finished, 2);
    MOQ_TEST_CHECK_EQ_SIZE(a.finished, 1);
    /* The stream that reached both counts for neither. */
    int as = sub_resolve_handle(r.c, a.csub);
    MOQ_TEST_CHECK(as >= 0 && r.c->subs[as].processed_stream_count == 0);
    MOQ_TEST_CHECK(ps >= 0 && r.c->publishes[ps].processed_stream_count == 1);
    /* Ending the subscription leaves the publication receiving. */
    MOQ_TEST_CHECK(moq_session_unsubscribe(r.c, a.csub, ++r.now) == MOQ_OK);
    pump(&r);
    drain_server(&r);
    MOQ_TEST_CHECK(send_pub_stream(&r, &p, 3, 0, 1));
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(p.objs == 5 && has_loc(&p, 3, 0));
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 0);
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Drafts 16 and 18 allow one subscription per track and role (§5.1), so our
 * publisher keeps refusing to give a publication the alias of a
 * subscription. */
static int test_publication_alias_single_drafts(moq_version_t v)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, false, 0));
    subrec_t a;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 0, 0, false, 0, NULL));
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.has_track_alias = true;
    cfg.track_alias = a.alias;
    moq_publication_t pub;
    MOQ_TEST_CHECK(moq_session_publish(r.sv, &cfg, ++r.now, &pub) == MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(!moq_session_supports_concurrent_subscriptions(r.c));
    moq_simpair_destroy(r.sp);
    return failures;
}

/* -- Objects ahead of a shared alias (§3.1.3) ----------------------- */

/* A server action held back and delivered by hand, so the test controls
 * whether the subscriber sees data before the control message. */
typedef struct {
    uint32_t kind;
    uint64_t ref;
    uint8_t bytes[512];
    size_t len;
    bool fin;
} held_t;

static size_t hold_server_actions(rig_t *r, held_t *h, size_t cap)
{
    size_t n = 0;
    moq_action_t act;
    while (moq_session_poll_actions(r->sv, &act, 1) > 0) {
        if (n < cap && (act.kind == MOQ_ACTION_SEND_DATA ||
                        act.kind == MOQ_ACTION_SEND_BIDI_STREAM ||
                        act.kind == MOQ_ACTION_SEND_DATAGRAM)) {
            held_t *x = &h[n++];
            memset(x, 0, sizeof(*x));
            x->kind = act.kind;
            if (act.kind == MOQ_ACTION_SEND_DATA) {
                const moq_send_data_action_t *d = &act.u.send_data;
                x->ref = d->stream_ref._v;
                memcpy(x->bytes, d->header, d->header_len);
                x->len = d->header_len;
                if (d->payload) {
                    memcpy(x->bytes + x->len, moq_rcbuf_data(d->payload),
                           moq_rcbuf_len(d->payload));
                    x->len += moq_rcbuf_len(d->payload);
                }
                x->fin = d->fin;
            } else if (act.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
                x->ref = act.u.send_bidi_stream.stream_ref._v;
                memcpy(x->bytes, act.u.send_bidi_stream.data, act.u.send_bidi_stream.len);
                x->len = act.u.send_bidi_stream.len;
                x->fin = act.u.send_bidi_stream.fin;
            } else {
                memcpy(x->bytes, act.u.send_datagram.data, act.u.send_datagram.len);
                x->len = act.u.send_datagram.len;
            }
        }
        moq_action_cleanup(&act);
    }
    return n;
}

/* Deliver the held actions of `kinds` to the client: data streams under a
 * fresh client-side ref each, request-stream bytes on `req_ref`. */
static void deliver_held(rig_t *r, held_t *h, size_t n, bool data, bool control,
                         uint64_t req_ref)
{
    for (size_t i = 0; i < n; i++) {
        if (h[i].kind == MOQ_ACTION_SEND_DATA && data)
            (void)moq_session_on_data_bytes(r->c,
                moq_stream_ref_from_u64(0x9000 + h[i].ref), h[i].bytes, h[i].len,
                h[i].fin, ++r->now);
        else if (h[i].kind == MOQ_ACTION_SEND_DATAGRAM && data)
            (void)moq_session_on_datagram(r->c, h[i].bytes, h[i].len, ++r->now);
        else if (h[i].kind == MOQ_ACTION_SEND_BIDI_STREAM && control)
            (void)moq_session_on_bidi_stream_bytes(r->c, moq_stream_ref_from_u64(req_ref),
                h[i].bytes, h[i].len, h[i].fin, ++r->now);
    }
}

typedef enum { PEND_SAME_ALIAS, PEND_OTHER_ALIAS, PEND_REJECTED } pend_outcome_t;

/* A (alias X, group 1 only) is established; P (from group 5) is pending. The publisher answers P and sends P's objects -- one stream
 * object in group `g` and one datagram in group `g + 1` -- but the
 * subscriber sees the objects before the answer. Under the shared alias P
 * receives them after its SUBSCRIBE_OK; under another alias, or when P is
 * refused, they are discarded and never reach A outside its window. */
static int test_pending_retention(pend_outcome_t outcome)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, p;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    memset(&p, 0, sizeof(p));
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.filter = START;
    cfg.start_group = 5;
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &p.csub) == MOQ_OK);
    pump(&r);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(&r, r.sv, MOQ_EVENT_SUBSCRIBE_REQUEST, MOQ_SUBSCRIPTION_INVALID, &ev));
    p.ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int pslot = sub_resolve_handle(r.c, p.csub);
    MOQ_TEST_CHECK(pslot >= 0);
    uint64_t req_ref = pslot >= 0 ? r.c->subs[pslot].rs.ref._v : 0;
    /* Another subscription stays pending throughout, so nothing but P's own
     * answer can release what was held for P. */
    moq_subscription_t q;
    cfg.track_name = MOQ_BYTES_LITERAL("other");
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &q) == MOQ_OK);
    { moq_action_t act;
      while (moq_session_poll_actions(r.c, &act, 1) > 0) moq_action_cleanup(&act); }
    held_t held[16];
    size_t nh = 0;
    subrec_t *recs[] = { &a, &p };
    if (outcome == PEND_REJECTED) {
        /* A's own subscription carries the objects; P is then refused. */
        moq_reject_subscribe_cfg_t rj; moq_reject_subscribe_cfg_init(&rj);
        rj.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        MOQ_TEST_CHECK(moq_session_reject_subscribe(r.sv, p.ssub, &rj, ++r.now) == MOQ_OK);
        nh = hold_server_actions(&r, held, 16);
        MOQ_TEST_CHECK(send_stream_direct(&r, &a, 5, 0, 1, 0x7105));
    } else {
        moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
        ac.has_largest = true;
        ac.has_track_alias = outcome == PEND_SAME_ALIAS;
        ac.track_alias = a.alias;
        MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, p.ssub, &ac, ++r.now) == MOQ_OK);
        const subrec_t *carrier = outcome == PEND_SAME_ALIAS ? &p : &a;
        moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
        sc.group_id = 5;
        moq_subgroup_handle_t sg;
        MOQ_TEST_CHECK(moq_session_open_subgroup(r.sv, carrier->ssub, &sc, ++r.now, &sg) == MOQ_OK);
        moq_rcbuf_t *pl = mk_payload("early");
        MOQ_TEST_CHECK(moq_session_write_object(r.sv, sg, 0, pl, ++r.now) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_close_subgroup(r.sv, sg, ++r.now) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_send_object_datagram(r.sv, carrier->ssub, 6, 0, 3, false,
                                                        pl, NULL, 0, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
        nh = hold_server_actions(&r, held, 16);
    }
    /* Data first, then the answer. */
    deliver_held(&r, held, nh, true, false, req_ref);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(p.objs, 0);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 0);   /* groups 5 and 6 lie outside A */
    MOQ_TEST_CHECK(r.c->staged_count > 0);
    deliver_held(&r, held, nh, false, true, req_ref);
    drain_client(&r, recs, 2);
    if (outcome == PEND_SAME_ALIAS) {
        MOQ_TEST_CHECK(p.objs == 2 && has_loc(&p, 5, 0) && has_loc(&p, 6, 0));
    } else {
        MOQ_TEST_CHECK_EQ_SIZE(p.objs, 0);
    }
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_count, 0);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->recv_payload_bytes, 0);
    MOQ_TEST_CHECK(moq_session_state(r.c) == MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* What is held is surfaced through the window the answer establishes: P
 * asks for the next object; before its answer the newest Largest known here
 * is {1,0}, the publisher's is {5,0}. An object at group 3 is held (it may
 * be P's) and dropped once the answer shows it is not; group 6 arrives. */
static int test_pending_retention_window(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, p;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    MOQ_TEST_CHECK(send_stream(&r, &a, 1, 0, 1, NULL));
    subrec_t *recs[] = { &a, &p };
    memset(&p, 0, sizeof(p));
    drain_client(&r, recs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 1);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &p.csub) == MOQ_OK);
    pump(&r);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(&r, r.sv, MOQ_EVENT_SUBSCRIBE_REQUEST, MOQ_SUBSCRIPTION_INVALID, &ev));
    p.ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int pslot = sub_resolve_handle(r.c, p.csub);
    uint64_t req_ref = pslot >= 0 ? r.c->subs[pslot].rs.ref._v : 0;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true;
    ac.largest_group = 5;
    ac.largest_object = 0;
    ac.has_track_alias = true;
    ac.track_alias = a.alias;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, p.ssub, &ac, ++r.now) == MOQ_OK);
    for (uint64_t g = 3; g <= 6; g += 3) {
        moq_rcbuf_t *pl = mk_payload("w");
        MOQ_TEST_CHECK(moq_session_send_object_datagram(r.sv, p.ssub, g, 0, 3, false, pl,
                                                        NULL, 0, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
    }
    held_t held[16];
    size_t nh = hold_server_actions(&r, held, 16);
    deliver_held(&r, held, nh, true, false, req_ref);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_count, 2);
    deliver_held(&r, held, nh, false, true, req_ref);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(p.objs == 1 && has_loc(&p, 6, 0));
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_count, 0);
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 1);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Overlapping windows: the object reaches A at once and P after its
 * SUBSCRIBE_OK names the shared alias. */
static int test_pending_retention_overlap(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, p;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 1, 0, false, 0, NULL));
    memset(&p, 0, sizeof(p));
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.filter = START;
    cfg.start_group = 1;
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &p.csub) == MOQ_OK);
    pump(&r);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(&r, r.sv, MOQ_EVENT_SUBSCRIBE_REQUEST, MOQ_SUBSCRIPTION_INVALID, &ev));
    p.ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int pslot = sub_resolve_handle(r.c, p.csub);
    uint64_t req_ref = pslot >= 0 ? r.c->subs[pslot].rs.ref._v : 0;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true;
    ac.has_track_alias = true;
    ac.track_alias = a.alias;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, p.ssub, &ac, ++r.now) == MOQ_OK);
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    sc.group_id = 2;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK(moq_session_open_subgroup(r.sv, p.ssub, &sc, ++r.now, &sg) == MOQ_OK);
    moq_rcbuf_t *pl = mk_payload("early");
    MOQ_TEST_CHECK(moq_session_write_object(r.sv, sg, 0, pl, ++r.now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    MOQ_TEST_CHECK(moq_session_close_subgroup(r.sv, sg, ++r.now) == MOQ_OK);
    held_t held[16];
    size_t nh = hold_server_actions(&r, held, 16);
    subrec_t *recs[] = { &a, &p };
    deliver_held(&r, held, nh, true, false, req_ref);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(a.objs == 1 && has_loc(&a, 2, 0));
    MOQ_TEST_CHECK_EQ_SIZE(p.objs, 0);
    deliver_held(&r, held, nh, false, true, req_ref);
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK(p.objs == 1 && has_loc(&p, 2, 0));
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 1);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_count, 0);
    MOQ_TEST_CHECK(moq_session_state(r.c) == MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* The retained objects live in the existing held-object ring: beyond its
 * slots the oldest is dropped and counted, nothing grows. */
static int test_pending_retention_bounded(void)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, MOQ_VERSION_DRAFT_22, false, 0));
    subrec_t a, p;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", RANGE, 1, 1, false, 0, NULL));
    memset(&p, 0, sizeof(p));
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.filter = START;
    cfg.start_group = 5;
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &p.csub) == MOQ_OK);
    pump(&r);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(&r, r.sv, MOQ_EVENT_SUBSCRIBE_REQUEST, MOQ_SUBSCRIPTION_INVALID, &ev));
    p.ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int pslot = sub_resolve_handle(r.c, p.csub);
    uint64_t req_ref = pslot >= 0 ? r.c->subs[pslot].rs.ref._v : 0;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true;
    ac.has_track_alias = true;
    ac.track_alias = a.alias;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(r.sv, p.ssub, &ac, ++r.now) == MOQ_OK);
    held_t ok[4];
    size_t nok = hold_server_actions(&r, ok, 4);
    size_t cap = r.c->staged_cap;
    size_t dropped0 = r.c->staged_dropped;
    for (uint64_t i = 0; i < cap + 4; i++) {
        moq_rcbuf_t *pl = mk_payload("d");
        MOQ_TEST_CHECK(moq_session_send_object_datagram(r.sv, p.ssub, 5, i, 3, false, pl,
                                                        NULL, 0, ++r.now) == MOQ_OK);
        moq_rcbuf_decref(pl);
        held_t d[2];
        size_t nd = hold_server_actions(&r, d, 2);
        deliver_held(&r, d, nd, true, false, req_ref);
    }
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_count, cap);
    MOQ_TEST_CHECK_EQ_SIZE(r.c->staged_dropped - dropped0, 4);
    deliver_held(&r, ok, nok, false, true, req_ref);
    subrec_t *recs[] = { &a, &p };
    drain_client(&r, recs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(p.objs, cap);
    MOQ_TEST_CHECK(has_loc(&p, 5, cap + 3) && !has_loc(&p, 5, 0));
    MOQ_TEST_CHECK_EQ_SIZE(a.objs, 0);
    moq_simpair_destroy(r.sp);
    return failures;
}

/* Drafts 16 and 18: "at most one subscription to a Track acting as a
 * subscriber"; a second local subscribe is refused before anything is sent. */
static int test_single_subscription_drafts(moq_version_t v)
{
    int failures = 0;
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, false, 0));
    subrec_t a;
    MOQ_TEST_CHECK(sub_accept(&r, &a, "t", START, 0, 0, false, 0, NULL));
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    moq_subscription_t second;
    MOQ_TEST_CHECK(moq_session_subscribe(r.c, &cfg, ++r.now, &second) == MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(!r.c->profile->allows_concurrent_subscriptions);
    MOQ_TEST_CHECK(!moq_session_supports_concurrent_subscriptions(r.c));
    MOQ_TEST_CHECK(established(&r));
    moq_simpair_destroy(r.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    int before;
#define RUN(call) do { before = failures; failures += (call); \
    if (failures != before) fprintf(stderr, "  in %s\n", #call); } while (0)
    RUN(test_disjoint_distinct());
    RUN(test_disjoint_shared(false));
    RUN(test_disjoint_shared(true));
    RUN(test_overlap(false, false, 0));
    RUN(test_overlap(true, false, 0));
    RUN(test_overlap(true, true, 0));
    /* An event queue of one: every copy still arrives, one at a time. */
    RUN(test_overlap(true, false, 1));
    RUN(test_overlap(true, true, 1));
    RUN(test_three_and_mixed());
    RUN(test_datagrams_shared());
    RUN(test_reset_shared(false));
    RUN(test_reset_shared(true));
    RUN(test_unsubscribe_one(false));
    RUN(test_unsubscribe_one(true));
    RUN(test_stream_count(false));
    RUN(test_stream_count(true));
    RUN(test_stream_count_ambiguous());
    RUN(test_stream_count_after_unsubscribe());
    RUN(test_unsubscribe_both_shared());
    RUN(test_update_one());
    RUN(test_fill_one());
    RUN(test_other_track_alias());
    RUN(test_facade_publisher());
    RUN(test_publication_shares_alias(false));
    RUN(test_publication_shares_alias(true));
    RUN(test_publication_alias_single_drafts(MOQ_VERSION_DRAFT_16));
    RUN(test_publication_alias_single_drafts(MOQ_VERSION_DRAFT_18));
    RUN(test_pending_retention(PEND_SAME_ALIAS));
    RUN(test_pending_retention(PEND_OTHER_ALIAS));
    RUN(test_pending_retention(PEND_REJECTED));
    RUN(test_pending_retention_overlap());
    RUN(test_pending_retention_window());
    RUN(test_pending_retention_bounded());
    RUN(test_single_subscription_drafts(MOQ_VERSION_DRAFT_16));
    RUN(test_single_subscription_drafts(MOQ_VERSION_DRAFT_18));
#undef RUN
    if (failures == 0) MOQ_TEST_PASS("d22_concurrent_subs");
    return failures != 0;
}
