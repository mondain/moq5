/* Malformed Tracks at the receiving endpoint (draft-22 §12.1) and the Prior
 * Group/Object ID Gap Object Properties (§10.8, §10.9).
 *
 * Pinned, on draft-18 and draft-22:
 * - each Track-level Object Property violation (§10.7 nested or repeated
 *   Immutable Properties and an unparseable pair, §3.7 a Mandatory Track
 *   Property, §10.8/§10.9 a repeated gap) fails the subscription, not the
 *   session: the request stream is reset and stopped with MALFORMED_TRACK,
 *   UNSUBSCRIBED carries MOQ_REQUEST_TERMINAL_MALFORMED_TRACK and 0x12, and
 *   the object is not delivered;
 * - the session-level property failures stay PROTOCOL_VIOLATION closes
 *   (§8.3 Delta Type overflow and value Length above 2^16-1);
 * - §12.1 conditions 1-5 across streams of one Track, and the §10.8/§10.9 gap
 *   conditions, fail the subscription; a consistent Track is delivered;
 * - a datagram and a PUBLISH-initiated Track fail the same way (the latter
 *   through PUBLISH_FINISHED); a FETCH response fails the fetch through
 *   FETCH_RESET, including a Location that does not ascend in its Group;
 * - a failure that cannot queue its terminal at once is kept and completes
 *   when the queue drains, without the stream being driven again;
 * - the gaps are surfaced on OBJECT_RECEIVED, OBJECT_CHUNK and FETCH_OBJECT
 *   (draft-18 and draft-22), and an object written with gaps carries them in
 *   its properties; the object cfg's symbol initializer keeps the original
 *   extent and its sized form the full one.
 * Draft-18 shares the draft-22 data plane and fails a malformed Track the same
 * way (its §2.4.2); draft-16 cancels with UNSUBSCRIBE (its §2.4.2) for the
 * violations its own Extension Headers define (§11.2: "A Key-Value-Pair
 * cannot be parsed") and for the conditions tracked across streams.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <moq/subscriber.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/spi/core_spi.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define ALIAS 7u

/* -- Fixture ------------------------------------------------------------ */

typedef struct fx {
    moq_simpair_t      *sp;
    moq_session_t      *client;   /* subscriber */
    moq_session_t      *server;   /* publisher  */
    moq_subscription_t  csub;
    moq_subscription_t  ssub;
    moq_publication_t   cpub;     /* PUBLISH fixture */
    uint64_t            next_stream;
} fx_t;

static bool fx_open_cfg(fx_t *f, moq_version_t v, uint32_t max_events,
                        bool streaming)
{
    memset(f, 0, sizeof(*f));
    moq_simpair_cfg_t c = MOQ_SIMPAIR_CFG_INIT;
    c.alloc = moq_alloc_default();
    c.version = v;
    c.max_events = max_events;
    c.client_streaming_objects = streaming;
    if (v == MOQ_VERSION_DRAFT_16) {          /* MAX_REQUEST_ID grants */
        c.server_send_request_capacity = true;
        c.server_initial_request_capacity = 32;
        c.client_send_request_capacity = true;
        c.client_initial_request_capacity = 32;
    }
    if (moq_simpair_create(&c, &f->sp) < 0) return false;
    f->client = moq_simpair_client(f->sp);
    f->server = moq_simpair_server(f->sp);
    if (moq_simpair_start(f->sp) < 0) return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    f->next_stream = 0x7000;
    return moq_session_state(f->client) == MOQ_SESS_ESTABLISHED;
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

static bool fx_subscribe(fx_t *f)
{
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    if (moq_session_subscribe(f->client, &cfg, moq_simpair_now_us(f->sp), &f->csub) < 0)
        return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    moq_event_t ev; bool got = false;
    while (moq_session_poll_events(f->server, &ev, 1) > 0) {
        if (!got && ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            f->ssub = ev.u.subscribe_request.sub; got = true;
        }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    acc.has_track_alias = true;
    acc.track_alias = ALIAS;
    if (moq_session_accept_subscribe(f->server, f->ssub, &acc,
                                     moq_simpair_now_us(f->sp)) < 0)
        return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    drain(f->client);
    return sub_resolve_handle(f->client, f->csub) >= 0;
}

static bool fx_open(fx_t *f, moq_version_t v)
{
    return fx_open_cfg(f, v, 0, false) && fx_subscribe(f);
}

static bool fx_open_publish(fx_t *f, moq_version_t v)
{
    if (!fx_open_cfg(f, v, 0, false)) return false;
    moq_publish_cfg_t pc;
    moq_publish_cfg_init(&pc);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    pc.track_namespace = (moq_namespace_t){ parts, 1 };
    pc.track_name = MOQ_BYTES_LITERAL("p");
    pc.has_track_alias = true;
    pc.track_alias = ALIAS;
    moq_publication_t spub;
    if (moq_session_publish(f->server, &pc, moq_simpair_now_us(f->sp), &spub) < 0)
        return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    moq_event_t ev; bool got = false;
    while (moq_session_poll_events(f->client, &ev, 1) > 0) {
        if (!got && ev.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            f->cpub = ev.u.publish_request.pub; got = true;
        }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_publish_cfg_t ac;
    moq_accept_publish_cfg_init(&ac);
    if (moq_session_accept_publish(f->client, f->cpub, &ac,
                                   moq_simpair_now_us(f->sp)) < 0)
        return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    drain(f->client);
    int ps = pub_resolve_handle(f->client, f->cpub);
    return ps >= 0 && f->client->publishes[ps].state == MOQ_PUB_ESTABLISHED;
}

static void fx_close(fx_t *f)
{
    if (f->sp) moq_simpair_destroy(f->sp);
    memset(f, 0, sizeof(*f));
}

/* -- Wire builders --------------------------------------------------------- */

typedef struct wire { uint8_t b[256]; size_t n; } wire_t;

static void put(wire_t *w, uint8_t v) { if (w->n < sizeof(w->b)) w->b[w->n++] = v; }

/* Subgroup header: PROPERTIES, explicit Subgroup ID and Priority, the
 * END_OF_GROUP bit on request. */
static void sg_header(wire_t *w, uint64_t group, uint64_t subgroup, uint8_t prio,
                      bool eog)
{
    put(w, eog ? 0x1D : 0x15);
    put(w, ALIAS); put(w, (uint8_t)group); put(w, (uint8_t)subgroup); put(w, prio);
}

/* One object on a PROPERTIES subgroup: Object ID Delta, the property block,
 * then a one-byte payload, or a zero length and `status` when status != 0. */
static void sg_object(wire_t *w, uint8_t delta, const uint8_t *props, size_t plen,
                      uint8_t status)
{
    put(w, delta);
    put(w, (uint8_t)plen);
    for (size_t i = 0; i < plen; i++) put(w, props[i]);
    if (status) { put(w, 0x00); put(w, status); }
    else { put(w, 0x01); put(w, 'A'); }
}

/* -- Observation ------------------------------------------------------------ */

typedef struct seen {
    size_t   objects;
    bool     unsubscribed;
    uint32_t cause;
    uint64_t stream_error_code;
    bool     publish_finished;
    bool     fetch_reset;
    bool     abort_malformed;     /* ABORT_BIDI_STREAM with MALFORMED_TRACK */
    bool     stop_data;           /* a STOP_SENDING on a data stream */
    uint64_t close_code;          /* UINT64_MAX when the session stays open */
    bool     has_gaps;
    bool     has_group_gap, has_object_gap;
    uint64_t group_gap, object_gap;
} seen_t;

static void note_gaps(seen_t *o, bool hg, uint64_t g, bool ho, uint64_t ob)
{
    o->has_gaps = true;
    o->has_group_gap = hg; o->group_gap = g;
    o->has_object_gap = ho; o->object_gap = ob;
}

/* Events, then (when `actions`) the queued actions, which are consumed:
 * a caller that needs them carried to the peer passes false. */
static seen_t observe_ex(moq_session_t *s, bool actions)
{
    seen_t o; memset(&o, 0, sizeof(o));
    o.close_code = UINT64_MAX;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_OBJECT_RECEIVED: {
            const moq_object_received_event_t *x = &ev.u.object_received;
            o.objects++;
            if (ev.detail_size >= offsetof(moq_object_received_event_t, prior_object_gap) + 8)
                note_gaps(&o, x->has_prior_group_gap, x->prior_group_gap,
                          x->has_prior_object_gap, x->prior_object_gap);
            break;
        }
        case MOQ_EVENT_OBJECT_CHUNK: {
            const moq_object_chunk_event_t *x = &ev.u.object_chunk;
            if (x->begin) {
                o.objects++;
                note_gaps(&o, x->has_prior_group_gap, x->prior_group_gap,
                          x->has_prior_object_gap, x->prior_object_gap);
            }
            break;
        }
        case MOQ_EVENT_FETCH_OBJECT: {
            const moq_fetch_object_event_t *x = &ev.u.fetch_object;
            o.objects++;
            note_gaps(&o, x->has_prior_group_gap, x->prior_group_gap,
                      x->has_prior_object_gap, x->prior_object_gap);
            break;
        }
        case MOQ_EVENT_UNSUBSCRIBED:
            o.unsubscribed = true;
            o.cause = ev.u.unsubscribed.cause;
            o.stream_error_code = ev.u.unsubscribed.stream_error_code;
            break;
        case MOQ_EVENT_PUBLISH_FINISHED:
            o.publish_finished = true;
            o.cause = ev.u.publish_finished.cause;
            o.stream_error_code = ev.u.publish_finished.stream_error_code;
            break;
        case MOQ_EVENT_FETCH_RESET:
            o.fetch_reset = true;
            o.cause = ev.u.fetch_reset.cause;
            o.stream_error_code = ev.u.fetch_reset.error_code;
            break;
        default:
            break;
        }
        moq_event_cleanup(&ev);
    }
    moq_action_t a;
    while (actions && moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_ABORT_BIDI_STREAM &&
            a.u.abort_bidi_stream.error_code == MOQ_STREAM_ERROR_MALFORMED_TRACK)
            o.abort_malformed = true;
        if (a.kind == MOQ_ACTION_STOP_DATA) o.stop_data = true;
        if (a.kind == MOQ_ACTION_CLOSE_SESSION && o.close_code == UINT64_MAX)
            o.close_code = a.u.close_session.code;
        moq_action_cleanup(&a);
    }
    return o;
}

static seen_t observe(moq_session_t *s) { return observe_ex(s, true); }

static moq_result_t feed(fx_t *f, uint64_t stream, const wire_t *w, bool fin)
{
    return moq_session_on_data_bytes(f->client, moq_stream_ref_from_u64(stream),
                                      w->b, w->n, fin, moq_simpair_now_us(f->sp));
}

static uint64_t new_stream(fx_t *f) { uint64_t r = f->next_stream; f->next_stream += 4; return r; }

/* The subscription failed as a malformed Track, with the session open. */
static void expect_failed(const char *what, fx_t *f, const seen_t *o, size_t objects)
{
    bool ok = o->unsubscribed && o->cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK &&
              o->stream_error_code == MOQ_STREAM_ERROR_MALFORMED_TRACK &&
              o->abort_malformed && o->close_code == UINT64_MAX &&
              o->objects == objects &&
              moq_session_state(f->client) == MOQ_SESS_ESTABLISHED &&
              sub_resolve_handle(f->client, f->csub) < 0;
    if (!ok) {
        fprintf(stderr, "FAIL %s: unsub=%d cause=%u code=0x%llx abort=%d close=0x%llx "
                "objects=%zu (want %zu) state=%d\n", what, o->unsubscribed,
                (unsigned)o->cause, (unsigned long long)o->stream_error_code, o->abort_malformed,
                (unsigned long long)o->close_code, o->objects, objects,
                (int)moq_session_state(f->client));
        failures++;
    }
}

/* Every object delivered, nothing failed. */
static void expect_delivered(const char *what, fx_t *f, const seen_t *o, size_t objects)
{
    bool ok = !o->unsubscribed && !o->abort_malformed && o->close_code == UINT64_MAX &&
              o->objects == objects && sub_resolve_handle(f->client, f->csub) >= 0;
    if (!ok) {
        fprintf(stderr, "FAIL %s: unsub=%d abort=%d close=0x%llx objects=%zu (want %zu)\n",
                what, o->unsubscribed, o->abort_malformed,
                (unsigned long long)o->close_code, o->objects, objects);
        failures++;
    }
}

/* -- Object Property violations ---------------------------------------------- */

typedef struct prop_row {
    const char *what;
    uint8_t     props[12];
    size_t      len;
    bool        track_level;   /* fails the Track; else the session */
} prop_row_t;

static const prop_row_t prop_rows[] = {
    /* 0x0B { 0x0B { } } */
    { "nested Immutable Properties", { 0x0B, 0x02, 0x0B, 0x00 }, 4, true },
    /* 0x0B { } then delta 0 -> 0x0B { } again */
    { "two Immutable Properties", { 0x0B, 0x00, 0x00, 0x00 }, 4, true },
    /* 0x4000 (vi64 3-byte form c0 40 00) with value 1 */
    { "Mandatory Track Property", { 0xC0, 0x40, 0x00, 0x01 }, 4, true },
    /* odd type 0x21 declaring 5 bytes with 1 present */
    { "unparseable pair", { 0x21, 0x05, 0x00 }, 3, true },
    /* 0x3E = 1 twice (delta 0) */
    { "two Prior Object ID Gaps", { 0x3E, 0x01, 0x00, 0x01 }, 4, true },
    /* 0x3C = 1 twice */
    { "two Prior Group ID Gaps", { 0x3C, 0x01, 0x00, 0x01 }, 4, true },
    /* Prior Object ID Gap inside Immutable Properties and in the mutable
     * list: one Object, two instances (§10.7: search both lists). */
    { "gap in both lists", { 0x0B, 0x02, 0x3E, 0x01, 0x33, 0x01 }, 6, true },
    /* Delta Type past 2^64-1: 0x3E then delta 2^64-2 (ff ff ff ff ff ff ff ff fe) */
    { "Delta Type overflow", { 0x3E, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE }, 11, false },
    /* odd type 0x21 with Length 0x10000 (vi64 3-byte form c1 00 00) */
    { "value Length above 2^16-1", { 0x21, 0xC1, 0x00, 0x00 }, 4, false },
};

static void test_property_violations(moq_version_t v)
{
    for (size_t i = 0; i < sizeof(prop_rows) / sizeof(prop_rows[0]); i++) {
        const prop_row_t *r = &prop_rows[i];
        fx_t f;
        if (!fx_open(&f, v)) { fprintf(stderr, "FAIL fixture %s\n", r->what); failures++; fx_close(&f); continue; }
        wire_t w = {0};
        sg_header(&w, 3, 0, 0, false);
        sg_object(&w, 2, r->props, r->len, 0);   /* Object 2 */
        (void)feed(&f, new_stream(&f), &w, false);
        seen_t o = observe(f.client);
        char what[96];
        snprintf(what, sizeof(what), "d%u %s", (unsigned)v, r->what);
        if (r->track_level) {
            expect_failed(what, &f, &o, 0);
        } else if (o.close_code != 0x3 || o.unsubscribed || o.objects != 0) {
            fprintf(stderr, "FAIL %s: expected PROTOCOL_VIOLATION close, got close=0x%llx unsub=%d objects=%zu\n",
                    what, (unsigned long long)o.close_code, o.unsubscribed, o.objects);
            failures++;
        }
        fx_close(&f);
    }
}

/* -- Conditions across one Track -------------------------------------------- */

/* Gap property bytes: Prior Group ID Gap and/or Prior Object ID Gap. */
static size_t gap_props(uint8_t *p, bool hg, uint8_t g, bool ho, uint8_t o)
{
    size_t n = 0;
    if (hg) { p[n++] = 0x3C; p[n++] = g; }
    if (ho) { p[n++] = hg ? 0x02 : 0x3E; p[n++] = o; }
    return n;
}

typedef struct step {
    uint8_t group, subgroup, prio;
    bool    eog_bit;
    uint8_t first_object;       /* first Object ID on the stream */
    uint8_t count;              /* objects on the stream */
    uint8_t last_status;        /* wire status of the last object, 0 = Normal */
    bool    fin;
    bool    gap_group;  uint8_t group_gap;   /* on the first object */
    bool    gap_object; uint8_t object_gap;
} step_t;

typedef struct cond_row {
    const char *what;
    step_t      steps[3];
    size_t      nsteps;
    bool        malformed;
    size_t      objects;        /* delivered before the failure / in all */
} cond_row_t;

#define ST(g, sg, pr, eog, first, cnt, last, fin) \
    { (g), (sg), (pr), (eog), (first), (cnt), (last), (fin), false, 0, false, 0 }
#define STG(g, sg, first, hg, gv, ho, ov) \
    { (g), (sg), 0, false, (first), 1, 0, false, (hg), (gv), (ho), (ov) }

static const cond_row_t cond_rows[] = {
    { "consistent Track", { ST(1, 0, 0, false, 0, 2, 0, true), ST(1, 1, 0, false, 2, 1, 0, true),
                            ST(2, 0, 0, false, 0, 1, 0, false) }, 3, false, 4 },
    { "object past End of Group", { ST(1, 0, 0, false, 0, 3, 0x3, false),
                            ST(1, 1, 0, false, 3, 1, 0, false) }, 2, true, 3 },
    { "object past End of Track", { ST(1, 0, 0, false, 0, 1, 0x4, false),
                            ST(2, 0, 0, false, 0, 1, 0, false) }, 2, true, 1 },
    { "End of Track below a received object", { ST(1, 0, 0, false, 0, 3, 0, false),
                            ST(1, 1, 0, false, 1, 1, 0x4, false) }, 2, true, 3 },
    { "two Ends of Group", { ST(1, 0, 0, false, 0, 2, 0x3, false),
                            ST(1, 1, 0, false, 0, 1, 0x3, false) }, 2, true, 2 },
    { "object past a header END_OF_GROUP final", { ST(1, 0, 0, true, 0, 2, 0, true),
                            ST(1, 1, 0, false, 2, 1, 0, false) }, 2, true, 2 },
    { "Publisher Priority changes in a Subgroup", { ST(1, 0, 0, false, 0, 1, 0, false),
                            ST(1, 0, 5, false, 1, 1, 0, false) }, 2, true, 1 },
    { "object past a Subgroup's final", { ST(1, 0, 0, false, 0, 2, 0, true),
                            ST(1, 0, 0, false, 2, 1, 0, false) }, 2, true, 2 },
    { "a Subgroup FIN'd at two finals", { ST(1, 0, 0, false, 0, 2, 0, true),
                            ST(1, 0, 0, false, 0, 1, 0, true) }, 2, true, 3 },
    { "Prior Group ID Gap above the Group ID", { STG(1, 0, 0, true, 2, false, 0) }, 1, true, 0 },
    { "Prior Object ID Gap above the Object ID", { STG(1, 0, 1, false, 0, true, 2) }, 1, true, 0 },
    { "object inside a Prior Group ID Gap", { STG(5, 0, 0, true, 2, false, 0),
                            STG(4, 0, 0, false, 0, false, 0) }, 2, true, 1 },
    { "Prior Group ID Gap covering a received Group", { STG(3, 0, 0, false, 0, false, 0),
                            STG(5, 0, 0, true, 2, false, 0) }, 2, true, 1 },
    { "one Group, two Prior Group ID Gaps", { STG(5, 0, 0, true, 2, false, 0),
                            STG(5, 1, 1, true, 1, false, 0) }, 2, true, 1 },
    { "object inside a Prior Object ID Gap", { STG(1, 0, 5, false, 0, true, 2),
                            STG(1, 1, 4, false, 0, false, 0) }, 2, true, 1 },
    { "Prior Object ID Gap covering a received Object", { STG(1, 1, 3, false, 0, false, 0),
                            STG(1, 0, 5, false, 0, true, 2) }, 2, true, 1 },
    { "consistent gaps", { STG(5, 0, 5, true, 2, true, 2),
                            STG(5, 1, 6, true, 2, false, 0), STG(6, 0, 0, false, 0, false, 0) }, 3, false, 3 },
};

static void run_steps(fx_t *f, const step_t *steps, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        const step_t *st = &steps[k];
        wire_t w = {0};
        sg_header(&w, st->group, st->subgroup, st->prio, st->eog_bit);
        for (uint8_t j = 0; j < st->count; j++) {
            uint8_t props[8]; size_t pl = 0;
            if (j == 0) pl = gap_props(props, st->gap_group, st->group_gap,
                                       st->gap_object, st->object_gap);
            uint8_t status = (j + 1 == st->count) ? st->last_status : 0;
            sg_object(&w, j == 0 ? st->first_object : 0, props, pl, status);
        }
        (void)feed(f, new_stream(f), &w, st->fin);
    }
}

static void test_conditions(moq_version_t v)
{
    for (size_t i = 0; i < sizeof(cond_rows) / sizeof(cond_rows[0]); i++) {
        const cond_row_t *r = &cond_rows[i];
        fx_t f;
        if (!fx_open(&f, v)) { fprintf(stderr, "FAIL fixture %s\n", r->what); failures++; fx_close(&f); continue; }
        run_steps(&f, r->steps, r->nsteps);
        seen_t o = observe(f.client);
        char what[112];
        snprintf(what, sizeof(what), "d%u %s", (unsigned)v, r->what);
        if (r->malformed) expect_failed(what, &f, &o, r->objects);
        else expect_delivered(what, &f, &o, r->objects);
        fx_close(&f);
    }
}

/* -- Streaming receive and a full event queue -------------------------------- */

static void test_streaming(void)
{
    fx_t f;
    if (!fx_open_cfg(&f, MOQ_VERSION_DRAFT_22, 0, true) || !fx_subscribe(&f)) {
        fprintf(stderr, "FAIL streaming fixture\n"); failures++; fx_close(&f); return;
    }
    wire_t w = {0};
    static const uint8_t nested[] = { 0x0B, 0x02, 0x0B, 0x00 };
    sg_header(&w, 1, 0, 0, false);
    sg_object(&w, 0, NULL, 0, 0);
    sg_object(&w, 0, nested, sizeof(nested), 0);
    (void)feed(&f, new_stream(&f), &w, false);
    seen_t o = observe(f.client);
    expect_failed("d22 streaming receive", &f, &o, 1);
    fx_close(&f);
}

static void test_event_queue_full(void)
{
    fx_t f;
    if (!fx_open_cfg(&f, MOQ_VERSION_DRAFT_22, 4, false) || !fx_subscribe(&f)) {
        fprintf(stderr, "FAIL queue fixture\n"); failures++; fx_close(&f); return;
    }
    /* Four objects fill the event queue; the fifth is malformed. */
    wire_t w = {0};
    static const uint8_t nested[] = { 0x0B, 0x02, 0x0B, 0x00 };
    sg_header(&w, 1, 0, 0, false);
    for (int i = 0; i < 4; i++) sg_object(&w, 0, NULL, 0, 0);
    sg_object(&w, 0, nested, sizeof(nested), 0);
    uint64_t stream = new_stream(&f);
    /* The verdict is taken with the input: nothing has to be fed again. */
    MOQ_TEST_CHECK_EQ_INT((int)feed(&f, stream, &w, false), (int)MOQ_OK);
    MOQ_TEST_CHECK(event_queue_full(f.client));
    MOQ_TEST_CHECK(sub_resolve_handle(f.client, f.csub) >= 0);
    /* Draining the queue lets the owed terminal through, once. */
    seen_t o = observe(f.client);
    expect_failed("d22 failure after a full event queue", &f, &o, 4);
    wire_t none = {0};
    (void)feed(&f, stream, &none, false);
    o = observe(f.client);
    MOQ_TEST_CHECK(!o.unsubscribed && o.objects == 0);
    fx_close(&f);
}

/* -- Datagrams and PUBLISH ---------------------------------------------------- */

static void test_datagrams(moq_version_t v)
{
    /* PROPERTIES datagram carrying nested Immutable Properties. */
    static const uint8_t nested_dg[] = { 0x01, ALIAS, 0x02, 0x05, 0x03, 0x04,
                                         0x0B, 0x02, 0x0B, 0x00, 'h', 'i' };
    fx_t f;
    if (fx_open(&f, v)) {
        (void)moq_session_on_datagram(f.client, nested_dg, sizeof(nested_dg),
                                      moq_simpair_now_us(f.sp));
        seen_t o = observe(f.client);
        expect_failed("datagram property violation", &f, &o, 0);
    } else { failures++; }
    fx_close(&f);

    /* An End of Track status datagram at {2,5}, then a datagram at {3,0}. */
    static const uint8_t eot_dg[] = { 0x20, ALIAS, 0x02, 0x05, 0x03, 0x04 };
    static const uint8_t late_dg[] = { 0x00, ALIAS, 0x03, 0x00, 0x03, 'h' };
    if (fx_open(&f, v)) {
        (void)moq_session_on_datagram(f.client, eot_dg, sizeof(eot_dg), moq_simpair_now_us(f.sp));
        (void)moq_session_on_datagram(f.client, late_dg, sizeof(late_dg), moq_simpair_now_us(f.sp));
        seen_t o = observe(f.client);
        expect_failed("datagram past End of Track", &f, &o, 1);
    } else { failures++; }
    fx_close(&f);

    /* Gaps on a datagram are surfaced (draft-18 and draft-22). */
    static const uint8_t gap_dg[] = { 0x01, ALIAS, 0x02, 0x05, 0x03, 0x04,
                                      0x3C, 0x01, 0x02, 0x02, 'h', 'i' };
    if (fx_open(&f, v)) {
        (void)moq_session_on_datagram(f.client, gap_dg, sizeof(gap_dg), moq_simpair_now_us(f.sp));
        seen_t o = observe(f.client);
        expect_delivered("datagram with gaps", &f, &o, 1);
        MOQ_TEST_CHECK(o.has_gaps && o.has_group_gap && o.group_gap == 1 &&
                       o.has_object_gap && o.object_gap == 2);
    } else { failures++; }
    fx_close(&f);
}

static void test_publish(void)
{
    fx_t f;
    if (!fx_open_publish(&f, MOQ_VERSION_DRAFT_22)) {
        fprintf(stderr, "FAIL publish fixture\n"); failures++; fx_close(&f); return;
    }
    wire_t w = {0};
    static const uint8_t mandatory[] = { 0xC0, 0x40, 0x00, 0x01 };
    sg_header(&w, 1, 0, 0, false);
    sg_object(&w, 0, NULL, 0, 0);
    sg_object(&w, 0, mandatory, sizeof(mandatory), 0);
    (void)feed(&f, new_stream(&f), &w, false);
    seen_t o = observe(f.client);
    bool ok = o.publish_finished && o.cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK &&
              o.stream_error_code == MOQ_STREAM_ERROR_MALFORMED_TRACK &&
              o.abort_malformed && o.objects == 1 && o.close_code == UINT64_MAX &&
              pub_resolve_handle(f.client, f.cpub) < 0;
    if (!ok) {
        fprintf(stderr, "FAIL d22 PUBLISH malformed: finished=%d cause=%u abort=%d objects=%zu close=0x%llx\n",
                o.publish_finished, (unsigned)o.cause, o.abort_malformed, o.objects,
                (unsigned long long)o.close_code);
        failures++;
    }
    /* The publisher sees its request stream cancelled with MALFORMED_TRACK. */
    moq_simpair_run_until_quiescent(f.sp, 32, NULL);
    MOQ_TEST_CHECK(moq_session_state(f.server) == MOQ_SESS_ESTABLISHED);
    fx_close(&f);
}

/* -- FETCH -------------------------------------------------------------------- */

typedef struct fetch_fx {
    fx_t        f;
    moq_fetch_t cfetch;
    int         slot;
} fetch_fx_t;

static bool fetch_open(fetch_fx_t *x, moq_version_t v)
{
    memset(x, 0, sizeof(*x));
    if (!fx_open_cfg(&x->f, v, 0, false)) return false;
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    const moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    fc.track_namespace = (moq_namespace_t){ parts, 1 };
    fc.track_name = MOQ_BYTES_LITERAL("v");
    fc.start_group = 0;
    fc.end_group = 9;
    if (moq_session_fetch(x->f.client, &fc, moq_simpair_now_us(x->f.sp), &x->cfetch) < 0)
        return false;
    moq_simpair_run_until_quiescent(x->f.sp, 32, NULL);
    moq_fetch_t sfetch = {0};
    bool got = false;
    moq_event_t ev;
    while (moq_session_poll_events(x->f.server, &ev, 1) > 0) {
        if (!got && ev.kind == MOQ_EVENT_FETCH_REQUEST) { sfetch = ev.u.fetch_request.fetch; got = true; }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_fetch_cfg_t acc;
    moq_accept_fetch_cfg_init(&acc);
    acc.end_group = 9;
    if (moq_session_accept_fetch(x->f.server, sfetch, &acc, moq_simpair_now_us(x->f.sp)) < 0)
        return false;
    moq_simpair_run_until_quiescent(x->f.sp, 32, NULL);
    drain(x->f.client);
    x->slot = fetch_resolve_handle(x->f.client, x->cfetch);
    return x->slot >= 0 && x->f.client->fetches[x->slot].data_stream_started;
}

static seen_t fetch_feed(fetch_fx_t *x, const uint8_t *b, size_t n)
{
    (void)moq_session_on_data_bytes(x->f.client, x->f.client->fetches[x->slot].data_stream_ref,
                                    b, n, false, moq_simpair_now_us(x->f.sp));
    return observe(x->f.client);
}

static void expect_fetch_failed(const char *what, fetch_fx_t *x, const seen_t *o, size_t objects)
{
    /* The response stream is stopped; the request stream is reset only while
     * it is still open (§3.2.4). */
    bool ok = o->fetch_reset && o->cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK &&
              o->stream_error_code == MOQ_STREAM_ERROR_MALFORMED_TRACK &&
              o->stop_data && o->objects == objects &&
              o->close_code == UINT64_MAX &&
              fetch_resolve_handle(x->f.client, x->cfetch) < 0;
    if (!ok) {
        fprintf(stderr, "FAIL %s: reset=%d cause=%u code=0x%llx abort=%d objects=%zu close=0x%llx\n",
                what, o->fetch_reset, (unsigned)o->cause, (unsigned long long)o->stream_error_code,
                o->abort_malformed, o->objects, (unsigned long long)o->close_code);
        failures++;
    }
}

static void test_fetch(moq_version_t v)
{
    /* First object {1,4} with properties (flags 0x3c), then a second object
     * whose Object ID Delta is zero in the same Group: {1,4} again. */
    static const uint8_t first_gap[] = { 0x3c, 0x01, 0x04, 0x00, 0x04, 0x3C, 0x01, 0x02, 0x02, 0x01, 'A' };
    static const uint8_t same_again[] = { 0x14, 0x00, 0x00, 0x01, 'B' };
    static const uint8_t nested[] = { 0x3c, 0x01, 0x04, 0x00, 0x04, 0x0B, 0x02, 0x0B, 0x00, 0x01, 'A' };
    fetch_fx_t x;
    if (fetch_open(&x, v)) {
        seen_t o = fetch_feed(&x, first_gap, sizeof(first_gap));
        MOQ_TEST_CHECK_EQ_SIZE(o.objects, 1);
        MOQ_TEST_CHECK(o.has_gaps && o.has_group_gap && o.group_gap == 1 &&
                       o.has_object_gap && o.object_gap == 2);
        o = fetch_feed(&x, same_again, sizeof(same_again));
        expect_fetch_failed("FETCH Location repeats", &x, &o, 0);
    } else { fprintf(stderr, "FAIL fetch fixture\n"); failures++; }
    fx_close(&x.f);

    if (fetch_open(&x, v)) {
        seen_t o = fetch_feed(&x, nested, sizeof(nested));
        expect_fetch_failed("FETCH property violation", &x, &o, 0);
    } else { fprintf(stderr, "FAIL fetch fixture\n"); failures++; }
    fx_close(&x.f);
}

/* -- Gaps surfaced and sent ------------------------------------------------------ */

static void test_gaps_roundtrip(moq_version_t v)
{
    fx_t f;
    if (!fx_open(&f, v)) { fprintf(stderr, "FAIL gaps fixture\n"); failures++; fx_close(&f); return; }
    moq_subgroup_cfg_t sc;
    moq_subgroup_cfg_init(&sc);
    sc.group_id = 4;
    sc.subgroup_id = 0;
    sc.object_properties = true;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                          moq_simpair_now_us(f.sp), &sg),
                          (int)MOQ_OK);
    moq_rcbuf_t *pl = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl), (int)MOQ_OK);
    /* An application property of its own, merged with the gaps. */
    static const uint8_t own[] = { 0x40, 0x01 };   /* type 0x40, value 1 */
    moq_rcbuf_t *props = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_rcbuf_create(moq_alloc_default(), own, sizeof(own), &props), (int)MOQ_OK);

    moq_object_cfg_t oc;
    moq_object_cfg_init(&oc);   /* the sized macro */
    MOQ_TEST_CHECK_EQ_SIZE(oc.struct_size, sizeof(moq_object_cfg_t));
    oc.object_id = 3;
    oc.payload = pl;
    oc.properties = props;
    /* Refusals first: a gap above its ID, and a conflict with properties. */
    oc.has_prior_group_gap = true; oc.prior_group_gap = 5;     /* > Group 4 */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, &oc, moq_simpair_now_us(f.sp)),
                          (int)MOQ_ERR_INVAL);
    oc.prior_group_gap = 1;
    oc.has_prior_object_gap = true; oc.prior_object_gap = 4;   /* > Object 3 */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, &oc, moq_simpair_now_us(f.sp)),
                          (int)MOQ_ERR_INVAL);
    oc.prior_object_gap = 2;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, &oc, moq_simpair_now_us(f.sp)),
                          (int)MOQ_OK);
    moq_simpair_run_until_quiescent(f.sp, 32, NULL);
    seen_t o = observe(f.client);
    expect_delivered("gaps sent", &f, &o, 1);
    MOQ_TEST_CHECK(o.has_gaps && o.has_group_gap && o.group_gap == 1 &&
                   o.has_object_gap && o.object_gap == 2);
    moq_rcbuf_decref(props);
    moq_rcbuf_decref(pl);

    /* A subgroup without object properties cannot carry gaps. */
    moq_subgroup_cfg_init(&sc);
    sc.group_id = 5;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                          moq_simpair_now_us(f.sp), &sg),
                          (int)MOQ_OK);
    moq_object_cfg_init(&oc);
    oc.object_id = 1;
    oc.has_prior_object_gap = true; oc.prior_object_gap = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, &oc, moq_simpair_now_us(f.sp)),
                          (int)MOQ_ERR_INVAL);
    fx_close(&f);
}

/* A gap already stated inside Immutable Properties is one instance of that
 * property (§10.7 searches both lists; §10.8/§10.9: at most one instance per
 * Object). An equal requested gap leaves the properties as they are; a
 * different one, or properties already holding two instances, is refused
 * before anything is queued. The receiver delivers what was sent without
 * calling the Track malformed. */
static size_t queued_actions_total(const moq_session_t *s)
{
    return s->action_tail - s->action_head;
}

static void test_gaps_immutable_merge(moq_version_t v)
{
    static const struct {
        const char *what;
        uint8_t     props[8];
        size_t      len;
        bool        object_gap;   /* else the group gap */
        uint64_t    gap;
        bool        ok;
    } rows[] = {
        { "equal Prior Object ID Gap in Immutable Properties",
          { 0x0B, 0x02, 0x3E, 0x02 }, 4, true, 2, true },
        { "different Prior Object ID Gap in Immutable Properties",
          { 0x0B, 0x02, 0x3E, 0x02 }, 4, true, 1, false },
        { "equal Prior Group ID Gap in Immutable Properties",
          { 0x0B, 0x02, 0x3C, 0x01 }, 4, false, 1, true },
        { "different Prior Group ID Gap in Immutable Properties",
          { 0x0B, 0x02, 0x3C, 0x01 }, 4, false, 2, false },
        { "Prior Object ID Gap already in both lists",
          { 0x0B, 0x02, 0x3E, 0x02, 0x33, 0x02 }, 6, true, 2, false },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fx_t f;
        if (!fx_open(&f, v)) { fprintf(stderr, "FAIL gap merge fixture\n"); failures++; fx_close(&f); continue; }
        moq_subgroup_cfg_t sc;
        moq_subgroup_cfg_init(&sc);
        sc.group_id = 4;
        sc.object_properties = true;
        moq_subgroup_handle_t sg;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                              moq_simpair_now_us(f.sp), &sg),
                              (int)MOQ_OK);
        moq_rcbuf_t *pl = NULL, *props = NULL;
        (void)moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
        (void)moq_rcbuf_create(moq_alloc_default(), rows[i].props, rows[i].len, &props);
        moq_object_cfg_t oc;
        moq_object_cfg_init(&oc);
        oc.object_id = 5;
        oc.payload = pl;
        oc.properties = props;
        if (rows[i].object_gap) { oc.has_prior_object_gap = true; oc.prior_object_gap = rows[i].gap; }
        else { oc.has_prior_group_gap = true; oc.prior_group_gap = rows[i].gap; }
        size_t before = queued_actions_total(f.server);
        moq_result_t rc = moq_session_write_object_ex(f.server, sg, &oc,
                                                      moq_simpair_now_us(f.sp));
        moq_rcbuf_decref(props);
        moq_rcbuf_decref(pl);
        if (rows[i].ok ? rc != MOQ_OK : (rc != MOQ_ERR_INVAL ||
                                         queued_actions_total(f.server) != before)) {
            fprintf(stderr, "FAIL d%u %s: rc=%d, %zu action(s) queued\n", (unsigned)v,
                    rows[i].what, (int)rc, queued_actions_total(f.server) - before);
            failures++;
        } else if (rows[i].ok) {
            moq_simpair_run_until_quiescent(f.sp, 32, NULL);
            moq_event_t ev;
            bool got = false, ended = false;
            while (moq_session_poll_events(f.client, &ev, 1) > 0) {
                if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
                    const moq_rcbuf_t *rp = ev.u.object_received.properties;
                    got = rp && moq_rcbuf_len(rp) == rows[i].len &&
                          memcmp(moq_rcbuf_data(rp), rows[i].props, rows[i].len) == 0;
                }
                if (ev.kind == MOQ_EVENT_UNSUBSCRIBED) ended = true;
                moq_event_cleanup(&ev);
            }
            if (!got || ended || sub_resolve_handle(f.client, f.csub) < 0) {
                fprintf(stderr, "FAIL d%u %s: received unchanged %d, subscription ended %d\n",
                        (unsigned)v, rows[i].what, got, ended);
                failures++;
            }
        }
        fx_close(&f);
    }
}

static void test_gaps_unsupported_d16(void)
{
    fx_t f;
    if (!fx_open(&f, MOQ_VERSION_DRAFT_16)) { fprintf(stderr, "FAIL d16 fixture\n"); failures++; fx_close(&f); return; }
    moq_subgroup_cfg_t sc;
    moq_subgroup_cfg_init(&sc);
    sc.group_id = 1;
    sc.object_properties = true;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                          moq_simpair_now_us(f.sp), &sg),
                          (int)MOQ_OK);
    moq_object_cfg_t oc;
    moq_object_cfg_init(&oc);
    oc.object_id = 1;
    oc.has_prior_object_gap = true; oc.prior_object_gap = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, &oc, moq_simpair_now_us(f.sp)),
                          (int)MOQ_ERR_UNSUPPORTED);
    fx_close(&f);
}

/* The symbol initializer keeps the original extent: a caller built before the
 * gap fields never has them read, whatever its storage holds past it. */
static void test_object_cfg_initializers(void)
{
    const size_t v0 = offsetof(moq_object_cfg_t, properties) + sizeof(moq_rcbuf_t *);
    unsigned char block[sizeof(moq_object_cfg_t)];
    memset(block, 0xA5, sizeof(block));
    moq_object_cfg_t *c = (moq_object_cfg_t *)(void *)block;
    (moq_object_cfg_init)(c);
    MOQ_TEST_CHECK_EQ_SIZE(c->struct_size, v0);
    for (size_t i = v0; i < sizeof(block); i++)
        if (block[i] != 0xA5) { MOQ_TEST_CHECK(!"symbol init wrote past the original extent"); break; }

    /* Written through a session, the poisoned gap fields are not read. */
    fx_t f;
    if (!fx_open(&f, MOQ_VERSION_DRAFT_22)) { failures++; fx_close(&f); return; }
    moq_subgroup_cfg_t sc;
    moq_subgroup_cfg_init(&sc);
    sc.group_id = 1;
    sc.object_properties = true;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                          moq_simpair_now_us(f.sp), &sg),
                          (int)MOQ_OK);
    c->object_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(f.server, sg, c, moq_simpair_now_us(f.sp)),
                          (int)MOQ_OK);
    moq_simpair_run_until_quiescent(f.sp, 32, NULL);
    seen_t o = observe(f.client);
    expect_delivered("old-extent object cfg", &f, &o, 1);
    MOQ_TEST_CHECK(o.has_gaps && !o.has_group_gap && !o.has_object_gap);
    fx_close(&f);

    moq_object_cfg_t full;
    memset(&full, 0xA5, sizeof(full));
    moq_object_cfg_init_sized(&full, sizeof(full));
    MOQ_TEST_CHECK_EQ_SIZE(full.struct_size, sizeof(full));
    MOQ_TEST_CHECK(!full.has_prior_group_gap && !full.has_prior_object_gap &&
                   full.prior_group_gap == 0 && full.prior_object_gap == 0);
}

/* -- Subscriber facade ------------------------------------------------------- */

typedef struct done_seen { int count; uint64_t status; moq_sub_track_t *track; } done_seen_t;

static void on_done(void *ctx, moq_sub_track_t *track, uint64_t status)
{
    done_seen_t *d = (done_seen_t *)ctx;
    d->count++;
    d->status = status;
    d->track = track;
}

static void test_facade(void)
{
    fx_t f;
    if (!fx_open_cfg(&f, MOQ_VERSION_DRAFT_22, 0, false)) {
        fprintf(stderr, "FAIL facade fixture\n"); failures++; fx_close(&f); return;
    }
    done_seen_t seen = {0};
    moq_sub_cfg_t cfg;
    moq_sub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.callbacks.ctx = &seen;
    cfg.on_subscribe_done = on_done;
    moq_subscriber_t *sub = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_sub_create(f.client, moq_alloc_default(), &cfg, &sub), (int)MOQ_OK);
    if (!sub) { fx_close(&f); return; }
    moq_sub_track_cfg_t tc;
    moq_sub_track_cfg_init(&tc);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    tc.track_namespace = (moq_namespace_t){ parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("v");
    moq_sub_track_t *track = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_sub_subscribe(sub, &tc, moq_simpair_now_us(f.sp), &track), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(f.sp, 16, NULL);
    moq_event_t ev;
    while (moq_session_poll_events(f.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) f.ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    acc.has_track_alias = true;
    acc.track_alias = ALIAS;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(f.server, f.ssub, &acc,
                                                             moq_simpair_now_us(f.sp)), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(f.sp, 16, NULL);
    (void)moq_sub_tick(sub, moq_simpair_now_us(f.sp));
    /* The publisher reopens Subgroup 0 of Group 1 at another priority. */
    for (int k = 0; k < 2; k++) {
        moq_subgroup_cfg_t sc;
        moq_subgroup_cfg_init(&sc);
        sc.group_id = 1;
        sc.subgroup_id = 0;
        sc.publisher_priority = (uint8_t)(k * 4);
        moq_subgroup_handle_t sg;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(f.server, f.ssub, &sc,
                                                              moq_simpair_now_us(f.sp), &sg), (int)MOQ_OK);
        moq_rcbuf_t *pl = NULL;
        (void)moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(f.server, sg, (uint64_t)k, pl,
                                                            moq_simpair_now_us(f.sp)), (int)MOQ_OK);
        moq_rcbuf_decref(pl);
        if (k == 0)
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(f.server, sg,
                                                                   moq_simpair_now_us(f.sp)),
                                  (int)MOQ_OK);
        moq_simpair_run_until_quiescent(f.sp, 32, NULL);
        (void)moq_sub_tick(sub, moq_simpair_now_us(f.sp));
    }
    moq_sub_object_t obj;
    int objects = 0;
    while (moq_sub_poll_object(sub, &obj) == MOQ_OK) { objects++; moq_sub_object_cleanup(&obj); }
    moq_sub_stats_t st;
    memset(&st, 0, sizeof(st));
    MOQ_TEST_CHECK_EQ_INT((int)moq_sub_get_stats(sub, &st, sizeof(st)), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(objects, 1);
    MOQ_TEST_CHECK_EQ_INT(seen.count, 1);
    MOQ_TEST_CHECK_EQ_HEX(seen.status, MOQ_STREAM_ERROR_MALFORMED_TRACK);
    MOQ_TEST_CHECK(seen.track == track);
    MOQ_TEST_CHECK_EQ_U64(st.malformed_tracks, 1);
    MOQ_TEST_CHECK(moq_session_state(f.client) == MOQ_SESS_ESTABLISHED);
    moq_sub_destroy(sub);
    fx_close(&f);
}

/* Draft-16 (its §2.4.2: "MUST UNSUBSCRIBE any subscription"): an Extension
 * Header block that cannot be parsed (§11.2) and a Subgroup reopened at
 * another Publisher Priority each end the subscription with UNSUBSCRIBE —
 * UNSUBSCRIBED with the MALFORMED_TRACK cause and no code — and the session
 * stays open. */
static void test_draft16(void)
{
    for (int row = 0; row < 2; row++) {
        fx_t f;
        if (!fx_open(&f, MOQ_VERSION_DRAFT_16)) {
            fprintf(stderr, "FAIL d16 fixture\n"); failures++; fx_close(&f); continue;
        }
        size_t delivered = 1;
        if (row == 0) {
            static const uint8_t bad[] = { 0x21, 0x05, 0x00 };
            wire_t w = {0};
            sg_header(&w, 3, 0, 0, false);
            sg_object(&w, 2, bad, sizeof(bad), 0);
            (void)feed(&f, new_stream(&f), &w, false);
            delivered = 0;
        } else {
            for (int k = 0; k < 2; k++) {
                moq_subgroup_cfg_t sc;
                moq_subgroup_cfg_init(&sc);
                sc.group_id = 1;
                sc.publisher_priority = (uint8_t)(10 + k);
                moq_subgroup_handle_t sg;
                MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(
                    f.server, f.ssub, &sc, moq_simpair_now_us(f.sp), &sg), (int)MOQ_OK);
                moq_rcbuf_t *pl = NULL;
                (void)moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
                MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(
                    f.server, sg, (uint64_t)k, pl, moq_simpair_now_us(f.sp)), (int)MOQ_OK);
                moq_rcbuf_decref(pl);
                if (k == 0)
                    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(
                        f.server, sg, moq_simpair_now_us(f.sp)), (int)MOQ_OK);
                moq_simpair_run_until_quiescent(f.sp, 32, NULL);
            }
        }
        seen_t o = observe(f.client);
        bool ok = o.unsubscribed && o.cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK &&
                  o.stream_error_code == 0 && o.close_code == UINT64_MAX &&
                  o.objects == delivered &&
                  moq_session_state(f.client) == MOQ_SESS_ESTABLISHED &&
                  sub_resolve_handle(f.client, f.csub) < 0;
        if (!ok) {
            fprintf(stderr, "FAIL d16 row %d: unsub=%d cause=%u code=0x%llx close=0x%llx objects=%zu\n",
                    row, o.unsubscribed, (unsigned)o.cause,
                    (unsigned long long)o.stream_error_code,
                    (unsigned long long)o.close_code, o.objects);
            failures++;
        }
        fx_close(&f);
    }
}

/* Draft-16 PUBLISH-initiated subscription: the subscriber ends it with
 * UNSUBSCRIBE by the PUBLISH's Request ID (its §2.4.2 "MUST UNSUBSCRIBE any
 * subscription"); the application sees PUBLISH_FINISHED with the
 * MALFORMED_TRACK cause and no code, the publisher sees its publication
 * unsubscribed, and both sessions stay open. */
static void test_draft16_publish(void)
{
    fx_t f;
    if (!fx_open_publish(&f, MOQ_VERSION_DRAFT_16)) {
        fprintf(stderr, "FAIL d16 publish fixture\n"); failures++; fx_close(&f); return;
    }
    static const uint8_t bad[] = { 0x21, 0x05, 0x00 };
    wire_t w = {0};
    sg_header(&w, 1, 0, 0, false);
    sg_object(&w, 0, NULL, 0, 0);
    sg_object(&w, 0, bad, sizeof(bad), 0);
    (void)feed(&f, new_stream(&f), &w, false);
    seen_t o = observe_ex(f.client, false);
    bool ok = o.publish_finished && o.cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK &&
              o.stream_error_code == 0 && o.objects == 1 &&
              o.close_code == UINT64_MAX &&
              pub_resolve_handle(f.client, f.cpub) < 0;
    if (!ok) {
        fprintf(stderr, "FAIL d16 PUBLISH malformed: finished=%d cause=%u code=0x%llx objects=%zu close=0x%llx\n",
                o.publish_finished, (unsigned)o.cause,
                (unsigned long long)o.stream_error_code, o.objects,
                (unsigned long long)o.close_code);
        failures++;
    }
    moq_simpair_run_until_quiescent(f.sp, 32, NULL);
    bool unsubscribed = false;
    moq_event_t ev;
    while (moq_session_poll_events(f.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_UNSUBSCRIBED) unsubscribed = true;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(unsubscribed);
    MOQ_TEST_CHECK(moq_session_state(f.server) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(f.client) == MOQ_SESS_ESTABLISHED);
    fx_close(&f);
}

/* The relay's MAX_CACHE_DURATION query (core_spi.h): draft-18 and draft-22
 * find the property inside Immutable Properties; draft-16 reads its
 * top-level Track Extensions only, so a value only inside its Immutable
 * Extensions is not applied. */
static void test_max_cache_query(void)
{
    static const uint8_t d16_top[] = { 0x04, 0x01 };
    static const uint8_t d16_immutable[] = { 0x0b, 0x02, 0x04, 0x01 };
    static const uint8_t immutable[] = { 0x0b, 0x02, 0x04, 0x05 };
    uint64_t ms = 0;
    MOQ_TEST_CHECK(moq_core_internal_track_max_cache_duration(MOQ_VERSION_DRAFT_16,
                   d16_top, sizeof(d16_top), &ms) && ms == 1);
    ms = 0;
    MOQ_TEST_CHECK(!moq_core_internal_track_max_cache_duration(MOQ_VERSION_DRAFT_16,
                   d16_immutable, sizeof(d16_immutable), &ms) && ms == 0);
    const moq_version_t later[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        ms = 0;
        MOQ_TEST_CHECK(moq_core_internal_track_max_cache_duration(later[i], immutable,
                       sizeof(immutable), &ms) && ms == 5);
    }
}

int main(void)
{
    test_max_cache_query();
    const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        test_property_violations(versions[i]);
        test_conditions(versions[i]);
        test_datagrams(versions[i]);
        test_fetch(versions[i]);
        test_gaps_roundtrip(versions[i]);
        test_gaps_immutable_merge(versions[i]);
    }
    test_streaming();
    test_event_queue_full();
    test_publish();
    test_gaps_unsupported_d16();
    test_object_cfg_initializers();
    test_facade();
    test_draft16();
    test_draft16_publish();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: d22_malformed_track\n");
    return 0;
}
