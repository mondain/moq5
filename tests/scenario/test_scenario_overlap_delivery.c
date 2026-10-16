/* Deterministic overlap-delivery scenario runner.
 *
 * One track reaches the subscriber on two paths at once: live on the
 * subscription's subgroup stream, and on a response stream for the same
 * groups -- a fill carried on a REQUEST_UPDATE on draft-22, a Joining FETCH
 * on draft-16/18. The last group travels on both paths. The subscriber's
 * event queue is small and is drained only between transport passes, so its
 * session refuses deliveries with WOULD_BLOCK while both streams are in
 * flight; the transport must hold what it has not delivered and resume.
 *
 * Pinned on every profile and every seed:
 *   - every object written surfaces exactly once on the path it was written
 *     on, in order, with its payload (§3.4 fills, §11.3.2 streams: nothing
 *     written and not reset is lost);
 *   - the response stream ends with its terminal (FILL_TERMINAL COMPLETE, or
 *     FETCH_COMPLETE) and the live subgroup with SUBGROUP_FINISHED;
 *   - neither session closes.
 * The deterministic row writes the shape first seen through the relay:
 * paused subscription, the overlapped group ingested, then Forward 1 with a
 * fill over {0,0}..Largest. It also pins that backpressure engaged: the
 * first pass leaves objects undelivered, which the following passes deliver.
 * The seeded rows randomize the track shape, the write interleaving, the
 * subscriber's queue depth and its drain points, under data/control
 * splitting and delivery delay. Each seed runs twice; the trace hash and
 * every counter must match.
 *
 * MOQ_SIM_VERSION selects one profile; without it every profile runs.
 * Replay: MOQ_SIM_VERSION=<v> MOQ_SCENARIO_SEED_START=0x<seed> MOQ_SCENARIO_SEEDS=1
 *         ./test_scenario_overlap_delivery
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t s; } rng_t;
static uint64_t rng_next(rng_t *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#define MIX(h, v) do { (h) ^= (uint64_t)(v); (h) *= 0x100000001B3ULL; } while (0)
#define INIT_HASH 0xCBF29CE484222325ULL

typedef struct {
    uint64_t trace;
    /* Written by the publisher, per path, in write order. */
    size_t   live_sent, resp_sent;
    uint64_t live_sent_hash, resp_sent_hash;
    /* Surfaced to the subscriber, per path, in surfacing order. */
    size_t   live_rcvd, resp_rcvd, resp_gaps;
    uint64_t live_rcvd_hash, resp_rcvd_hash;
    size_t   live_finished, resp_complete, resp_other_terminal;
    size_t   closed;
    /* Objects surfaced (both paths) after the first transport pass. */
    size_t   first_pass;
    size_t   passes;
} run_t;

static void trace_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    run_t *t = (run_t *)ctx;
    uint64_t h = t->trace;
    MIX(h, r->seed); MIX(h, r->step); MIX(h, r->now_us); MIX(h, r->kind);
    MIX(h, r->from); MIX(h, r->to); MIX(h, r->action_kind);
    MIX(h, r->input_kind); MIX(h, r->result); MIX(h, r->code); MIX(h, r->bytes.len);
    for (size_t i = 0; r->bytes.data && i < r->bytes.len; i++) MIX(h, r->bytes.data[i]);
    t->trace = h;
}

static void hash_obj(uint64_t *h, uint64_t g, uint64_t o, const moq_rcbuf_t *pl)
{
    MIX(*h, g);
    MIX(*h, o);
    size_t n = pl ? moq_rcbuf_len(pl) : 0;
    MIX(*h, n);
    const uint8_t *d = pl ? moq_rcbuf_data(pl) : NULL;
    for (size_t i = 0; i < n; i++) MIX(*h, d[i]);
}

/* The publisher-side request a flow serves: a fill child or a fetch. */
typedef struct {
    moq_subscription_t ssub;
    moq_fill_t         sfill;
    moq_fetch_t        sfetch;
    bool               have_fill, have_fetch;
    uint64_t           fetch_end_group, fetch_end_object;
} srv_t;

static void drain_server(moq_session_t *sv, srv_t *srv, run_t *t)
{
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_SUBSCRIBE_REQUEST: srv->ssub = ev.u.subscribe_request.sub; break;
        case MOQ_EVENT_FILL_REQUEST:
            srv->sfill = ev.u.fill_request.fill;
            srv->have_fill = true;
            break;
        case MOQ_EVENT_FETCH_REQUEST:
            srv->sfetch = ev.u.fetch_request.fetch;
            srv->fetch_end_group = ev.u.fetch_request.end_group;
            srv->fetch_end_object = ev.u.fetch_request.end_object;
            srv->have_fetch = true;
            break;
        case MOQ_EVENT_SESSION_CLOSED: t->closed++; break;
        default: break;
        }
        moq_event_cleanup(&ev);
    }
}

/* Returns the number of events polled. */
static size_t drain_client(moq_session_t *c, run_t *t)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        n++;
        switch (ev.kind) {
        case MOQ_EVENT_OBJECT_RECEIVED:
            t->live_rcvd++;
            hash_obj(&t->live_rcvd_hash, ev.u.object_received.group_id,
                     ev.u.object_received.object_id, ev.u.object_received.payload);
            break;
        case MOQ_EVENT_SUBGROUP_FINISHED: t->live_finished++; break;
        case MOQ_EVENT_FILL_OBJECT:
            t->resp_rcvd++;
            hash_obj(&t->resp_rcvd_hash, ev.u.fill_object.group_id,
                     ev.u.fill_object.object_id, ev.u.fill_object.payload);
            break;
        case MOQ_EVENT_FETCH_OBJECT:
            t->resp_rcvd++;
            hash_obj(&t->resp_rcvd_hash, ev.u.fetch_object.group_id,
                     ev.u.fetch_object.object_id, ev.u.fetch_object.payload);
            break;
        case MOQ_EVENT_FILL_GAP: t->resp_gaps++; break;
        case MOQ_EVENT_FILL_TERMINAL:
            if (ev.u.fill_terminal.reason == MOQ_FILL_COMPLETE) t->resp_complete++;
            else t->resp_other_terminal++;
            break;
        case MOQ_EVENT_FETCH_COMPLETE: t->resp_complete++; break;
        case MOQ_EVENT_FETCH_RESET:
        case MOQ_EVENT_FETCH_CANCELLED:
        case MOQ_EVENT_FETCH_ERROR:
            t->resp_other_terminal++;
            break;
        case MOQ_EVENT_SESSION_CLOSED: t->closed++; break;
        default: break;
        }
        moq_event_cleanup(&ev);
    }
    return n;
}

/* Transport passes until nothing moves: run to quiescence, drain both
 * sides, advance to the next deadline when one is pending. */
static void settle(moq_simpair_t *sp, srv_t *srv, run_t *t)
{
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    for (int i = 0; i < 400; i++) {
        size_t steps = 0;
        (void)moq_simpair_run_until_quiescent(sp, 64, &steps);
        size_t polled = drain_client(c, t);
        drain_server(sv, srv, t);
        t->passes++;
        uint64_t dl = moq_simpair_next_deadline_us(sp);
        if (steps <= 1 && polled == 0) {
            if (dl == UINT64_MAX) break;
            if (dl > moq_simpair_now_us(sp))
                (void)moq_simpair_advance_to(sp, dl);
        }
    }
}

/* One write the publisher owes: an object on one of the two paths, or the
 * path's end. */
typedef enum { W_LIVE, W_LIVE_EOG, W_LIVE_END, W_RESP, W_RESP_GAP, W_RESP_END } wkind_t;
typedef struct { wkind_t k; uint64_t g, o; } wr_t;

typedef struct {
    moq_version_t version;
    uint64_t      seed;
    uint32_t      max_events;
    uint32_t      fault_flags;
    uint32_t      fault_per_mille;
    unsigned      groups;        /* groups 0..groups-1; the last is overlapped */
    unsigned      per_group;     /* objects per group */
    unsigned      live_from;     /* first object of the last group sent live */
    bool          interleave;    /* seeded write interleaving vs path-ordered */
    unsigned      payload_max;
} flow_t;

/* Retry a publisher write that met a full action queue after one pass. */
#define WRITE(expr) do {                                                     \
        moq_result_t _rc;                                                    \
        int _n = 0;                                                          \
        while ((_rc = (expr)) == MOQ_ERR_WOULD_BLOCK && _n++ < 64)           \
            (void)moq_simpair_run_until_quiescent(sp, 64, NULL);             \
        if (_rc != MOQ_OK) {                                                 \
            fprintf(stderr, "  %s rc=%d\n", #expr, (int)_rc);                \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static moq_rcbuf_t *make_payload(rng_t *rng, const flow_t *f, int path,
                                 uint64_t g, uint64_t o)
{
    uint8_t body[128];
    size_t n = 4 + (size_t)(rng_next(rng) % (f->payload_max ? f->payload_max : 1));
    if (n > sizeof(body)) n = sizeof(body);
    body[0] = (uint8_t)path; body[1] = (uint8_t)g; body[2] = (uint8_t)o; body[3] = 0xA5;
    for (size_t i = 4; i < n; i++) body[i] = (uint8_t)rng_next(rng);
    moq_rcbuf_t *pl = NULL;
    (void)moq_rcbuf_create(moq_alloc_default(), body, n, &pl);
    return pl;
}

static int run_flow(const flow_t *f, run_t *t)
{
    int failures = 0;
    memset(t, 0, sizeof(*t));
    t->trace = INIT_HASH;
    t->live_sent_hash = t->resp_sent_hash = INIT_HASH;
    t->live_rcvd_hash = t->resp_rcvd_hash = INIT_HASH;

    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = f->seed;
    cfg.version = f->version;
    cfg.max_events = f->max_events;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = t;
    cfg.fault_flags = f->fault_flags;
    cfg.fault_per_mille = f->fault_per_mille;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0 || moq_simpair_start(sp) < 0) {
        fprintf(stderr, "  pair start failed\n");
        if (sp) moq_simpair_destroy(sp);
        return 1;
    }
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    srv_t srv;
    memset(&srv, 0, sizeof(srv));
    settle(sp, &srv, t);
    rng_t rng = { f->seed ^ 0x0E7E41A9ULL };
    uint64_t last = f->groups - 1;
    bool fill = moq_session_supports_fill(c);

    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };
    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    if (fill) {
        /* Paused: the publisher holds the last group while Forward is 0. */
        loc.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
        scfg.location = &loc;
        scfg.has_forward = true;
        scfg.forward = false;
    } else {
        scfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    }
    moq_subscription_t csub;
    moq_result_t rc = moq_session_subscribe(c, &scfg, moq_simpair_now_us(sp), &csub);
    if (rc != MOQ_OK) { fprintf(stderr, "  subscribe rc=%d\n", (int)rc); failures++; }
    settle(sp, &srv, t);
    if (!moq_subscription_is_valid(srv.ssub)) {
        fprintf(stderr, "  no SUBSCRIBE_REQUEST\n");
        moq_simpair_destroy(sp);
        return failures + 1;
    }
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true;
    ac.largest_group = last;
    /* draft-22: the whole last group is published; draft-16/18: the live
     * edge sits just before live_from, where the subscription starts. */
    ac.largest_object = fill ? f->per_group - 1 : f->live_from - 1;
    rc = moq_session_accept_subscribe(sv, srv.ssub, &ac, moq_simpair_now_us(sp));
    if (rc != MOQ_OK) { fprintf(stderr, "  accept rc=%d\n", (int)rc); failures++; }
    settle(sp, &srv, t);

    if (fill) {
        moq_location_filter_cfg_t floc;
        moq_location_filter_cfg_init(&floc);
        floc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;   /* {0,0} .. Largest */
        moq_fill_cfg_t fc;
        moq_fill_cfg_init(&fc);
        fc.location = &floc;
        moq_subscription_update_cfg_t ucfg;
        moq_subscription_update_cfg_init_sized(&ucfg, sizeof(ucfg));
        ucfg.has_forward = true;
        ucfg.forward = true;
        ucfg.fill = &fc;
        rc = moq_session_update_subscription(c, csub, &ucfg, moq_simpair_now_us(sp));
        if (rc != MOQ_OK) { fprintf(stderr, "  update rc=%d\n", (int)rc); failures++; }
    } else {
        moq_fetch_cfg_t fcfg;
        moq_fetch_cfg_init(&fcfg);
        fcfg.is_joining = true;
        fcfg.joining_relative = true;
        fcfg.joining_sub = csub;
        fcfg.joining_start = last;   /* back to group 0 */
        moq_fetch_t cfetch;
        rc = moq_session_fetch(c, &fcfg, moq_simpair_now_us(sp), &cfetch);
        if (rc != MOQ_OK) { fprintf(stderr, "  fetch rc=%d\n", (int)rc); failures++; }
    }
    settle(sp, &srv, t);
    if (fill ? !srv.have_fill : !srv.have_fetch) {
        fprintf(stderr, "  no response request reached the publisher\n");
        moq_simpair_destroy(sp);
        return failures + 1;
    }
    if (!fill) {
        moq_accept_fetch_cfg_t afc;
        moq_accept_fetch_cfg_init(&afc);
        afc.end_group = srv.fetch_end_group;
        afc.end_object = srv.fetch_end_object;
        rc = moq_session_accept_fetch(sv, srv.sfetch, &afc, moq_simpair_now_us(sp));
        if (rc != MOQ_OK) { fprintf(stderr, "  accept_fetch rc=%d\n", (int)rc); failures++; }
    }
    t->passes = 0;
    if (f->fault_per_mille) moq_simpair_enable_faults(sp);

    /* The two write sequences, each in its stream order. */
    wr_t live[64], resp[96];
    size_t nl = 0, nr = 0;
    for (uint64_t o = fill ? 0 : f->live_from; o < f->per_group; o++)
        live[nl++] = (wr_t){ W_LIVE, last, o };
    live[nl++] = (wr_t){ W_LIVE_EOG, last, f->per_group };
    live[nl++] = (wr_t){ W_LIVE_END, last, 0 };
    for (uint64_t g = 0; g <= last; g++) {
        uint64_t end = (fill || g < last) ? f->per_group : f->live_from;
        for (uint64_t o = 0; o < end; o++) resp[nr++] = (wr_t){ W_RESP, g, o };
        /* A fill marks the unknown tail of each group without END_OF_GROUP
         * evidence, as the relay does. */
        if (fill && g < last) resp[nr++] = (wr_t){ W_RESP_GAP, g, UINT64_MAX };
    }
    resp[nr++] = (wr_t){ W_RESP_END, 0, 0 };

    uint64_t now = moq_simpair_now_us(sp);
    moq_subgroup_handle_t sh = { 0 };
    moq_subgroup_cfg_t sg;
    moq_subgroup_cfg_init(&sg);
    sg.group_id = last;
    sg.subgroup_id = 0;
    sg.publisher_priority = 1;
    WRITE(moq_session_open_subgroup(sv, srv.ssub, &sg, now, &sh));
    if (fill) WRITE(moq_session_open_fill(sv, srv.sfill, now));

    size_t il = 0, ir = 0;
    while (il < nl || ir < nr) {
        bool take_live;
        if (il == nl) take_live = false;
        else if (ir == nr) take_live = true;
        else if (f->interleave) take_live = (rng_next(&rng) & 1) != 0;
        else take_live = true;   /* the relay's order: live first */
        const wr_t *w = take_live ? &live[il++] : &resp[ir++];
        now = moq_simpair_now_us(sp);
        switch (w->k) {
        case W_LIVE: {
            moq_rcbuf_t *pl = make_payload(&rng, f, 1, w->g, w->o);
            hash_obj(&t->live_sent_hash, w->g, w->o, pl);
            t->live_sent++;
            WRITE(moq_session_write_object(sv, sh, w->o, pl, now));
            moq_rcbuf_decref(pl);
            break;
        }
        case W_LIVE_EOG:
            hash_obj(&t->live_sent_hash, w->g, w->o, NULL);
            t->live_sent++;
            WRITE(moq_session_write_status_object(sv, sh, w->o,
                                                  MOQ_OBJECT_END_OF_GROUP, now));
            break;
        case W_LIVE_END:
            WRITE(moq_session_close_subgroup(sv, sh, now));
            break;
        case W_RESP: {
            moq_rcbuf_t *pl = make_payload(&rng, f, 2, w->g, w->o);
            hash_obj(&t->resp_sent_hash, w->g, w->o, pl);
            t->resp_sent++;
            moq_fetch_object_cfg_t oc;
            moq_fetch_object_cfg_init(&oc);
            oc.group_id = w->g;
            oc.object_id = w->o;
            oc.publisher_priority = 3;
            oc.payload = pl;
            if (fill) WRITE(moq_session_write_fill_object(sv, srv.sfill, &oc, now));
            else WRITE(moq_session_write_fetch_object(sv, srv.sfetch, &oc, now));
            moq_rcbuf_decref(pl);
            break;
        }
        case W_RESP_GAP:
            WRITE(moq_session_write_fill_range(sv, srv.sfill, MOQ_FETCH_RANGE_UNKNOWN,
                                               w->g, w->o, now));
            break;
        case W_RESP_END:
            if (fill) WRITE(moq_session_end_fill(sv, srv.sfill, now));
            else WRITE(moq_session_end_fetch(sv, srv.sfetch, now));
            break;
        }
        /* Seeded transport passes between writes; the subscriber drains
         * only at some of them. */
        if (f->interleave) {
            uint64_t x = rng_next(&rng);
            if ((x & 3) == 0) {
                (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
                if ((x & 4) != 0) {
                    (void)drain_client(c, t);
                    drain_server(sv, &srv, t);
                }
            }
        }
    }

    /* The first pass delivers what the subscriber's queue admits; the
     * following passes drain and resume. */
    (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    (void)drain_client(c, t);
    drain_server(sv, &srv, t);
    t->first_pass = t->live_rcvd + t->resp_rcvd;
    settle(sp, &srv, t);

    if (t->live_rcvd != t->live_sent || t->live_rcvd_hash != t->live_sent_hash) {
        fprintf(stderr, "  live path: %zu of %zu objects surfaced%s\n", t->live_rcvd,
                t->live_sent, t->live_rcvd == t->live_sent ? " (content differs)" : "");
        failures++;
    }
    if (t->resp_rcvd != t->resp_sent || t->resp_rcvd_hash != t->resp_sent_hash) {
        fprintf(stderr, "  %s path: %zu of %zu objects surfaced%s\n", fill ? "fill" : "fetch",
                t->resp_rcvd, t->resp_sent,
                t->resp_rcvd == t->resp_sent ? " (content differs)" : "");
        failures++;
    }
    if (fill && t->resp_gaps != (size_t)last) {
        fprintf(stderr, "  fill gaps %zu, want %llu\n", t->resp_gaps, (unsigned long long)last);
        failures++;
    }
    if (t->resp_complete != 1 || t->resp_other_terminal != 0) {
        fprintf(stderr, "  %s terminal: complete=%zu other=%zu\n", fill ? "fill" : "fetch",
                t->resp_complete, t->resp_other_terminal);
        failures++;
    }
    if (t->live_finished != 1) {
        fprintf(stderr, "  SUBGROUP_FINISHED x%zu\n", t->live_finished);
        failures++;
    }
    if (t->closed || moq_session_state(c) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(sv) != MOQ_SESS_ESTABLISHED) {
        fprintf(stderr, "  a session closed\n");
        failures++;
    }
    moq_simpair_destroy(sp);
    return failures;
}

static const moq_version_t k_versions[] = {
    MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
};

static bool version_selected(moq_version_t v)
{
    const char *e = getenv("MOQ_SIM_VERSION");
    if (!e || !*e) return true;
    return (moq_version_t)strtoul(e, NULL, 0) == v;
}

/* The reported shape: the last group published in full before the response
 * stream, a queue that cannot hold both paths. */
static int deterministic_row(moq_version_t v)
{
    flow_t f;
    memset(&f, 0, sizeof(f));
    f.version = v;
    f.seed = 0x13A;
    f.max_events = 8;
    f.groups = 3;
    f.per_group = 3;
    f.live_from = 2;
    f.payload_max = 1;
    run_t a, b;
    int fa = run_flow(&f, &a);
    int fb = run_flow(&f, &b);
    int failures = fa + fb;
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        fprintf(stderr, "  deterministic row: runs differ\n");
        failures++;
    }
    /* Backpressure engaged and released: the first pass could not surface
     * every object, the rest arrived after the drains. */
    if (fa == 0 && a.first_pass >= a.live_sent + a.resp_sent) {
        fprintf(stderr, "  deterministic row: the first pass surfaced all %zu objects; "
                "the queue never refused\n", a.first_pass);
        failures++;
    }
    if (failures)
        fprintf(stderr, "FAIL: overlap_deterministic (draft-%d)\n", (int)v);
    else
        printf("PASS: overlap_deterministic (draft-%d): %zu live + %zu %s objects, "
               "%zu after the first pass\n", (int)v, a.live_sent, a.resp_sent,
               v == MOQ_VERSION_DRAFT_22 ? "fill" : "fetch", a.first_pass);
    return failures ? 1 : 0;
}

/* A RESET reaches a stream whose delivery is stalled behind the subscriber's
 * full queue. The reset interrupts the stalled delivery (RFC 9000 §3.2
 * permits abandoning unread data of a reset stream): the subscriber sees
 * a prefix of the objects, in order, then exactly one SUBGROUP_RESET with the
 * publisher's code and no SUBGROUP_FINISHED, and the next subgroup of the
 * track is delivered in full. */
typedef struct {
    size_t   g1_objects, g1_resets, g1_finished, g2_objects, g2_finished, closed;
    uint64_t g1_next, g1_code;
    bool     g1_order;
} reset_tally_t;

static void reset_drain(moq_session_t *c, reset_tally_t *r)
{
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_OBJECT_RECEIVED:
            if (ev.u.object_received.group_id == 1) {
                if (ev.u.object_received.object_id != r->g1_next) r->g1_order = false;
                r->g1_next = ev.u.object_received.object_id + 1;
                r->g1_objects++;
            } else {
                r->g2_objects++;
            }
            break;
        case MOQ_EVENT_SUBGROUP_RESET:
            if (ev.u.subgroup_reset.group_id == 1) {
                r->g1_resets++;
                r->g1_code = ev.u.subgroup_reset.error_code;
            }
            break;
        case MOQ_EVENT_SUBGROUP_FINISHED:
            if (ev.u.subgroup_finished.group_id == 1) r->g1_finished++;
            else r->g2_finished++;
            break;
        case MOQ_EVENT_SESSION_CLOSED: r->closed++; break;
        default: break;
        }
        moq_event_cleanup(&ev);
    }
}

static int reset_row(moq_version_t v)
{
    int failures = 0;
    run_t t;
    memset(&t, 0, sizeof(t));
    t.trace = INIT_HASH;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x13A5;
    cfg.version = v;
    cfg.max_events = 4;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = &t;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0 || moq_simpair_start(sp) < 0) {
        if (sp) moq_simpair_destroy(sp);
        fprintf(stderr, "FAIL: overlap_reset (draft-%d) pair start\n", (int)v);
        return 1;
    }
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    srv_t srv;
    memset(&srv, 0, sizeof(srv));
    settle(sp, &srv, &t);
    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };
    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    scfg.start_group = 1;
    scfg.start_object = 0;
    moq_subscription_t csub;
    if (moq_session_subscribe(c, &scfg, moq_simpair_now_us(sp), &csub) != MOQ_OK) failures++;
    settle(sp, &srv, &t);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    if (moq_session_accept_subscribe(sv, srv.ssub, &ac, moq_simpair_now_us(sp)) != MOQ_OK)
        failures++;
    settle(sp, &srv, &t);

    reset_tally_t r;
    memset(&r, 0, sizeof(r));
    r.g1_order = true;
    rng_t rng = { 0x13A5 };
    flow_t f;
    memset(&f, 0, sizeof(f));
    f.payload_max = 8;
    moq_subgroup_cfg_t sg;
    moq_subgroup_cfg_init(&sg);
    sg.group_id = 1;
    moq_subgroup_handle_t sh;
    uint64_t now = moq_simpair_now_us(sp);
    WRITE(moq_session_open_subgroup(sv, srv.ssub, &sg, now, &sh));
    for (uint64_t o = 0; o < 8; o++) {
        moq_rcbuf_t *pl = make_payload(&rng, &f, 1, 1, o);
        WRITE(moq_session_write_object(sv, sh, o, pl, now));
        moq_rcbuf_decref(pl);
    }
    /* The subscriber's queue fills; the rest of group 1 stalls. */
    (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    WRITE(moq_session_reset_subgroup(sv, sh, 0x77, now));
    (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    for (int i = 0; i < 16; i++) {
        reset_drain(c, &r);
        (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    }
    sg.group_id = 2;
    WRITE(moq_session_open_subgroup(sv, srv.ssub, &sg, now, &sh));
    for (uint64_t o = 0; o < 2; o++) {
        moq_rcbuf_t *pl = make_payload(&rng, &f, 1, 2, o);
        WRITE(moq_session_write_object(sv, sh, o, pl, now));
        moq_rcbuf_decref(pl);
    }
    WRITE(moq_session_close_subgroup(sv, sh, now));
    for (int i = 0; i < 16; i++) {
        (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
        reset_drain(c, &r);
        drain_server(sv, &srv, &t);
    }
    if (!r.g1_order || r.g1_objects >= 8 || r.g1_resets != 1 || r.g1_code != 0x77 ||
        r.g1_finished != 0) {
        fprintf(stderr, "  group 1: %zu objects (in order %d), %zu resets (code 0x%llx), "
                "%zu finished\n", r.g1_objects, (int)r.g1_order, r.g1_resets,
                (unsigned long long)r.g1_code, r.g1_finished);
        failures++;
    }
    if (r.g2_objects != 2 || r.g2_finished != 1) {
        fprintf(stderr, "  group 2: %zu objects, %zu finished\n", r.g2_objects, r.g2_finished);
        failures++;
    }
    if (r.closed || t.closed || moq_session_state(c) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(sv) != MOQ_SESS_ESTABLISHED) {
        fprintf(stderr, "  a session closed\n");
        failures++;
    }
    moq_simpair_destroy(sp);
    if (failures)
        fprintf(stderr, "FAIL: overlap_reset (draft-%d)\n", (int)v);
    else
        printf("PASS: overlap_reset (draft-%d): %zu of 8 objects before the reset\n",
               (int)v, r.g1_objects);
    return failures ? 1 : 0;
}

static int seeded_rows(moq_version_t v, uint64_t start, uint64_t seeds)
{
    int failures = 0;
    for (uint64_t i = 0; i < seeds; i++) {
        uint64_t seed = start + i;
        rng_t r = { seed ^ 0x51CE13AULL };
        flow_t f;
        memset(&f, 0, sizeof(f));
        f.version = v;
        f.seed = seed;
        f.max_events = 3 + (uint32_t)(rng_next(&r) % 14);
        f.groups = 2 + (unsigned)(rng_next(&r) % 3);
        f.per_group = 2 + (unsigned)(rng_next(&r) % 4);
        f.live_from = 1 + (unsigned)(rng_next(&r) % (f.per_group - 1));
        f.interleave = true;
        f.payload_max = 1 + (unsigned)(rng_next(&r) % 48);
        f.fault_per_mille = 300;
        f.fault_flags = MOQ_SIM_FAULT_SPLIT_DATA | MOQ_SIM_FAULT_SPLIT_CONTROL |
                        MOQ_SIM_FAULT_DELAY;
        run_t a, b;
        int fa = run_flow(&f, &a);
        int fb = run_flow(&f, &b);
        if (fa || fb || memcmp(&a, &b, sizeof(a)) != 0) {
            fprintf(stderr, "FAIL seed=0x%llx draft-%d (run0=%d run1=%d, %s)\n",
                    (unsigned long long)seed, (int)v, fa, fb,
                    memcmp(&a, &b, sizeof(a)) ? "runs differ" : "invariant");
            failures++;
        }
    }
    if (failures)
        fprintf(stderr, "FAIL: overlap_seeded (draft-%d, %d of %llu seeds)\n", (int)v,
                failures, (unsigned long long)seeds);
    else
        printf("PASS: overlap_seeded (draft-%d, %llu seeds)\n", (int)v,
               (unsigned long long)seeds);
    return failures ? 1 : 0;
}

int main(void)
{
    uint64_t start = 0, seeds = 64;
    const char *e;
    if ((e = getenv("MOQ_SCENARIO_SEED_START"))) start = strtoull(e, NULL, 0);
    if ((e = getenv("MOQ_SCENARIO_SEEDS"))) seeds = strtoull(e, NULL, 0);
    int failures = 0;
    for (size_t i = 0; i < sizeof(k_versions) / sizeof(k_versions[0]); i++) {
        if (!version_selected(k_versions[i])) continue;
        failures += deterministic_row(k_versions[i]);
        failures += reset_row(k_versions[i]);
        failures += seeded_rows(k_versions[i], start, seeds);
    }
    if (failures) {
        fprintf(stderr, "FAIL: test_scenario_overlap_delivery (%d rows)\n", failures);
        return 1;
    }
    printf("PASS: test_scenario_overlap_delivery\n");
    return 0;
}
