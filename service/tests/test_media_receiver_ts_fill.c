/*
 * Fills of an MPEG2TS media track through the receiver's fill intake: the
 * production item handler, queue and poll paths, fed facade fill items
 * directly (no wire). Each fill is its own delivery domain.
 *
 * Pinned:
 * - FILL_TERMINAL of a TS fill follows every Object of the fill: it is
 *   pollable once each of them is polled or dropped, after every event
 *   queued before that, and wait() reports it;
 * - its loss summary: an Unknown or Timed-Out range the fill ends with, and
 *   local drops after the last Object the application received, including
 *   drops after the fill's own end; causes are ORed and the Location is the
 *   contribution earliest in delivery order, even when discovered later, and
 *   never the numerically smallest one of a descending fill;
 * - a fill's loss is reported on the next queued Object of the same fill
 *   first, and never on another fill's Object; Non-Existent ranges are no
 *   loss;
 * - the record pool gates the facade's fill queue and frees by application
 *   polling alone; a one-slot event ring takes no reservation and does not
 *   turn fatal;
 * - LIVE loss stays within its subscription generation;
 * - a fill Object's payload gets the LIVE transform: 192-octet prefixes
 *   removed, and the join PSI on the subscription's first delivered Object,
 *   fill or LIVE, and on no other -- never for a fill of an earlier
 *   subscription; every buffer is released;
 * - a caller whose struct_size does not cover the whole summary reads the
 *   earlier fields only; the catalog fill and other tracks keep the
 *   immediate terminal and take no record.
 */
#include <moq/media_receiver.h>
#include <moq/subscriber.h>
#define MOQ_MEDIA_RECEIVER_TESTING 1
#include "../src/media_receiver.c"
#include "test_support.h"
#include "test_alloc_support.h"
#include "ts_stream.h"

#include <stdio.h>
#include <string.h>

static int failures;

static const char ts_catalog[] =
    "{\"version\":1,\"tracks\":["
    "{\"name\":\"ts\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
    "\"mimeType\":\"video/mp2t\",\"mpeg2tsMode\":\"unmodified-program\","
    "\"mpeg2tsPacketSize\":188,\"mpeg2tsProgramNumber\":1},"
    "{\"name\":\"loc\",\"packaging\":\"loc\",\"isLive\":true,"
    "\"role\":\"video\",\"codec\":\"avc1.42e01e\"}]}";

/* Facade generations stand in for the subscriptions; never dereferenced. */
static char gen_ts, gen_loc, gen_catalog;
#define SUB_TS      ((moq_sub_track_t *)(void *)&gen_ts)
#define SUB_LOC     ((moq_sub_track_t *)(void *)&gen_loc)
#define SUB_CATALOG ((moq_sub_track_t *)(void *)&gen_catalog)

typedef struct fx {
    test_alloc_state_t state;
    moq_alloc_t alloc;
    moq_media_receiver_t *r;
    moq_media_track_t *ts, *loc;
    ts_gen_t gen;
} fx_t;

/* A receiver over `catalog`, its tracks known; m2ts selects 192-octet
 * packets with the prefix stripped and the join PSI. */
static bool fx_open_catalog(fx_t *f, uint32_t max_objects, const char *catalog,
                            bool m2ts)
{
    memset(f, 0, sizeof(*f));
    f->alloc = test_allocator(&f->state);
    moq_media_receiver_cfg_t cfg;
    moq_media_receiver_cfg_init_live_sized(&cfg, sizeof(cfg));
    static moq_bytes_t ns[] = { { (const uint8_t *)"tsfill", 6 } };
    cfg.namespace_ = (moq_namespace_t){ ns, 1 };
    cfg.auto_subscribe = true;
    cfg.overflow.policy = MOQ_MEDIA_OVERFLOW_DROP_GROUP;
    cfg.overflow.max_objects = max_objects;
    cfg.mpeg2ts_strip_prefix = m2ts;
    cfg.mpeg2ts_join_psi = m2ts;
    /* The draft is set below without a session latch, so the companion that
     * latch would resolve is pinned here. */
    cfg.companion_profile = MOQ_MEDIA_COMPANION_LEGACY_MSF01_LOC01;
    moq_media_receiver_test_set_alloc(&f->alloc);
    f->r = moq_media_receiver_test_new_cfg(&cfg);
    moq_media_receiver_test_set_alloc(NULL);
    MOQ_TEST_CHECK(f->r != NULL);
    if (!f->r) return false;
    f->r->transport_version = MOQ_VERSION_DRAFT_18;
    moq_media_receiver_test_ingest(f->r, 0, 0, catalog, strlen(catalog));
    moq_media_track_event_t ev;
    while (moq_media_receiver_poll_track(f->r, &ev, sizeof(ev)) == MOQ_OK) {
        if (ev.kind != MOQ_MEDIA_TRACK_ADDED) continue;
        if (ev.desc->name.len == 2) f->ts = ev.track;
        if (ev.desc->name.len == 3) f->loc = ev.track;
    }
    MOQ_TEST_CHECK(f->ts && f->loc);
    if (!f->ts || !f->loc) return false;
    f->ts->sub_track = SUB_TS;
    f->loc->sub_track = SUB_LOC;
    f->r->catalog_sub = SUB_CATALOG;
    MOQ_TEST_CHECK(f->ts->desired_subscribed && f->loc->desired_subscribed);
    ts_gen_init(&f->gen, m2ts ? 192 : 188, 1, false);
    return true;
}

static bool fx_open(fx_t *f, uint32_t max_objects)
{
    return fx_open_catalog(f, max_objects, ts_catalog, false);
}

static void fx_close(fx_t *f)
{
    moq_media_track_event_t ev;
    moq_media_object_t o;
    if (!f->r) return;
    while (moq_media_receiver_poll_object(f->r, &o, sizeof(o)) == MOQ_OK)
        moq_media_object_cleanup(&o);
    while (moq_media_receiver_poll_track(f->r, &ev, sizeof(ev)) == MOQ_OK) {}
    moq_media_receiver_test_free(f->r);
    f->r = NULL;
    MOQ_TEST_CHECK(f->state.balance == 0);
}

static moq_sub_fill_t tok(uint64_t n) { return (moq_sub_fill_t){ n }; }

static void feed(fx_t *f, moq_sub_fill_item_t *it)
{
    it->struct_size = sizeof(*it);
    receiver_take_fill_item(f->r, it, 0);
}

static void created(fx_t *f, uint64_t token, moq_sub_track_t *track, uint64_t id)
{
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_CREATED;
    it.request = tok(token);
    it.track = track;
    it.initiating_request_id = id;
    feed(f, &it);
}

static void object(fx_t *f, uint64_t token, uint64_t g, uint64_t o)
{
    uint8_t buf[8 * 188];
    size_t n = ts_gen_gop(&f->gen, buf, 1);
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_OBJECT;
    it.request = tok(token);
    it.track = SUB_TS;
    it.u.object.group_id = g;
    it.u.object.object_id = o;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f->r->alloc, buf, n, &it.u.object.payload) == MOQ_OK);
    feed(f, &it);
}

/* An Object of the fill whose payload breaks the packet-size rule: it is
 * dropped at intake, a local drop at its own Location. */
static void bad_object(fx_t *f, uint64_t token, uint64_t g, uint64_t o)
{
    static const uint8_t junk[7] = { 0x47, 1, 2, 3, 4, 5, 6 };
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_OBJECT;
    it.request = tok(token);
    it.track = SUB_TS;
    it.u.object.group_id = g;
    it.u.object.object_id = o;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f->r->alloc, junk, sizeof(junk),
                                    &it.u.object.payload) == MOQ_OK);
    feed(f, &it);
}

static void range(fx_t *f, uint64_t token, uint32_t kind, uint64_t g, uint64_t o)
{
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_GAP;
    it.request = tok(token);
    it.track = SUB_TS;
    it.u.gap.range_kind = kind;
    it.u.gap.group_id = g;
    it.u.gap.object_id = o;
    feed(f, &it);
}

static void terminal(fx_t *f, uint64_t token, moq_sub_track_t *track,
                     uint64_t id, uint32_t reason)
{
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_TERMINAL;
    it.request = tok(token);
    it.track = track;
    it.initiating_request_id = id;
    it.u.terminal.reason = reason;
    feed(f, &it);
}

/* The next object, which must exist; its loss report is returned in *o. */
static bool next_object(fx_t *f, moq_media_object_t *o)
{
    moq_result_t rc = moq_media_receiver_poll_object(f->r, o, sizeof(*o));
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    return rc == MOQ_OK;
}

/* The next event must be this fill's terminal, with this summary. */
static void expect_terminal(fx_t *f, uint64_t id, uint32_t causes,
                            uint64_t g, uint64_t o)
{
    moq_media_track_event_t ev;
    memset(&ev, 0xa5, sizeof(ev));
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_receiver_poll_track(f->r, &ev, sizeof(ev)),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)ev.kind, (int)MOQ_MEDIA_TRACK_FILL_TERMINAL);
    MOQ_TEST_CHECK(ev.track == f->ts);
    MOQ_TEST_CHECK_EQ_U64(ev.initiating_request_id, id);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_reason, MOQ_FILL_COMPLETE);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_causes, causes);
    MOQ_TEST_CHECK_EQ_U64(ev._reserved_fill_loss, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_group, causes ? g : 0);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_object, causes ? o : 0);
}

/* Nothing to poll (no endpoint is attached, so an empty receiver reads as
 * closed). */
static void expect_no_event(fx_t *f)
{
    moq_media_track_event_t ev;
    moq_result_t rc = moq_media_receiver_poll_track(f->r, &ev, sizeof(ev));
    MOQ_TEST_CHECK(rc == MOQ_DONE || rc == MOQ_ERR_CLOSED);
}

/* A fill that ends with a Timed-Out range and delivered no Object: the
 * range is reported on the terminal, at the range's end Location since
 * nothing preceded it. After an Object the application received, an
 * Unknown tail starts right after that Object. */
static void test_tail_without_objects(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 11);
    range(&f, 1, MOQ_FETCH_RANGE_TIMED_OUT, 5, 3);
    terminal(&f, 1, SUB_TS, 11, MOQ_FILL_COMPLETE);
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_receiver_wait(f.r, 0), (int)MOQ_OK);
    expect_terminal(&f, 11, MOQ_MEDIA_FILL_LOSS_TIMED_OUT, 5, 3);
    expect_no_event(&f);

    created(&f, 2, SUB_TS, 12);
    object(&f, 2, 6, 0);
    range(&f, 2, MOQ_FETCH_RANGE_NON_EXISTENT, 6, 2);
    range(&f, 2, MOQ_FETCH_RANGE_UNKNOWN, 6, 9);
    moq_media_object_t o;
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK(o.has_loss_report && !o.gap);
        MOQ_TEST_CHECK_EQ_INT((int)o.delivery_origin, (int)MOQ_MEDIA_DELIVERY_FILL);
        moq_media_object_cleanup(&o);
    }
    terminal(&f, 2, SUB_TS, 12, MOQ_FILL_COMPLETE);
    expect_terminal(&f, 12, MOQ_MEDIA_FILL_LOSS_UNKNOWN, 6, 3);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_tail_without_objects");
}

/* The terminal waits for the fill's queued Objects. They are dropped after
 * it (another fill's Objects evict their Group): the drops are the summary,
 * at the first dropped Object, and the other fill's Object reports none. */
static void test_drop_after_terminal(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 2)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 21);
    created(&f, 2, SUB_TS, 22);
    object(&f, 1, 7, 0);
    object(&f, 1, 7, 1);
    terminal(&f, 1, SUB_TS, 21, MOQ_FILL_COMPLETE);
    expect_no_event(&f);
    object(&f, 2, 9, 0);
    expect_terminal(&f, 21, MOQ_MEDIA_FILL_LOSS_LOCAL_DROP, 7, 0);
    moq_media_object_t o;
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 9);
        MOQ_TEST_CHECK(o.has_loss_report && !o.gap);
        moq_media_object_cleanup(&o);
    }
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_drop_after_terminal");
}

/* An Unknown tail is recorded first; an Object queued before it is dropped
 * later. The earlier delivery position wins and both causes are reported.
 * Descending fills: the summary keeps the Location earliest in delivery
 * order, not the numerically smaller one -- when a later, smaller drop is
 * recorded before the earlier Object is dropped, and when the first Object's
 * drop is carried by the next one. */
static void test_earliest_position(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 1)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 31);
    created(&f, 2, SUB_TS, 32);
    object(&f, 1, 7, 0);
    range(&f, 1, MOQ_FETCH_RANGE_UNKNOWN, 7, 5);
    terminal(&f, 1, SUB_TS, 31, MOQ_FILL_COMPLETE);
    expect_no_event(&f);
    object(&f, 2, 9, 0);
    expect_terminal(&f, 31, MOQ_MEDIA_FILL_LOSS_UNKNOWN | MOQ_MEDIA_FILL_LOSS_LOCAL_DROP,
                    7, 0);
    fx_close(&f);

    if (!fx_open(&f, 1)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 35);
    created(&f, 2, SUB_TS, 36);
    object(&f, 1, 9, 0);
    bad_object(&f, 1, 8, 0);
    terminal(&f, 1, SUB_TS, 35, MOQ_FILL_COMPLETE);
    object(&f, 2, 20, 0);   /* evicts Group 9 */
    expect_terminal(&f, 35, MOQ_MEDIA_FILL_LOSS_LOCAL_DROP, 9, 0);
    fx_close(&f);

    if (!fx_open(&f, 2)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 33);
    created(&f, 2, SUB_TS, 34);
    object(&f, 1, 9, 0);
    object(&f, 1, 8, 0);
    terminal(&f, 1, SUB_TS, 33, MOQ_FILL_COMPLETE);
    object(&f, 2, 20, 0);   /* evicts Group 9: Group 8 now carries it */
    expect_no_event(&f);
    object(&f, 2, 21, 0);   /* evicts Group 8 */
    expect_terminal(&f, 33, MOQ_MEDIA_FILL_LOSS_LOCAL_DROP, 9, 0);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_earliest_position");
}

/* Loss is reported on the fill's next Object when there is one: a range
 * before it, or a drop of an earlier queued Object. Nothing is left for the
 * terminal, and the other fill's Object reports nothing. */
static void test_carry_to_next_object(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 2)) { fx_close(&f); return; }
    created(&f, 1, SUB_TS, 41);
    created(&f, 2, SUB_TS, 42);
    object(&f, 1, 4, 0);
    range(&f, 1, MOQ_FETCH_RANGE_UNKNOWN, 4, 3);
    object(&f, 1, 5, 0);
    object(&f, 2, 9, 0);    /* evicts Group 4 */
    moq_media_object_t o;
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 5);
        MOQ_TEST_CHECK(o.has_loss_report && o.gap && !o.gap_tail_unknown);
        MOQ_TEST_CHECK_EQ_U64(o.gap_group, 4);
        MOQ_TEST_CHECK_EQ_U64(o.gap_object, 0);
        MOQ_TEST_CHECK(!o.gap_duration_known);
        moq_media_object_cleanup(&o);
    }
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 9);
        MOQ_TEST_CHECK(o.has_loss_report && !o.gap);
        moq_media_object_cleanup(&o);
    }
    terminal(&f, 1, SUB_TS, 41, MOQ_FILL_COMPLETE);
    expect_terminal(&f, 41, 0, 0, 0);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_carry_to_next_object");
}

/* A terminal with no Object queued is ready at once, after the events
 * queued before it and before later ones. */
static void test_zero_object_terminal_order(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    MOQ_TEST_CHECK(rq_push(f.r, MOQ_MEDIA_TRACK_ENDED, f.loc));
    created(&f, 1, SUB_TS, 51);
    terminal(&f, 1, SUB_TS, 51, MOQ_FILL_COMPLETE);
    MOQ_TEST_CHECK(rq_push(f.r, MOQ_MEDIA_TRACK_REMOVED, f.loc));
    moq_media_track_event_t ev;
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK &&
                   ev.kind == MOQ_MEDIA_TRACK_ENDED);
    expect_terminal(&f, 51, 0, 0, 0);
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK &&
                   ev.kind == MOQ_MEDIA_TRACK_REMOVED);
    expect_no_event(&f);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_zero_object_terminal_order");
}

/* Every record in use: the facade's fill queue is not read. Polling an
 * Object makes its record ready, and polling that terminal frees it: the
 * gate opens again with no facade item read. */
static void test_pool_full_progress(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 64)) { fx_close(&f); return; }
    for (uint64_t i = 1; i <= RECEIVER_FILL_DOMS; i++) {
        created(&f, i, SUB_TS, 100 + i);
        object(&f, i, i, 0);
        terminal(&f, i, SUB_TS, 100 + i, MOQ_FILL_COMPLETE);
    }
    MOQ_TEST_CHECK(!receiver_fill_items_ready(f.r));
    MOQ_TEST_CHECK(f.r->fill_blocked);
    expect_no_event(&f);
    moq_media_object_t o;
    if (next_object(&f, &o)) moq_media_object_cleanup(&o);
    MOQ_TEST_CHECK(!receiver_fill_items_ready(f.r));
    expect_terminal(&f, 101, 0, 0, 0);
    MOQ_TEST_CHECK(receiver_fill_items_ready(f.r));
    MOQ_TEST_CHECK(!f.r->fill_blocked);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_pool_full_progress");
}

/* A one-slot event ring: a waiting terminal holds no slot, the ring can be
 * filled by another event and drained, the terminal follows it, and the
 * receiver never turns fatal. */
static void test_one_slot_ring(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    /* The catalog's own events are drained; the ring now holds one. */
    uint32_t cap = f.r->ev_cap;
    f.r->ev_cap = 1;
    created(&f, 1, SUB_TS, 61);
    object(&f, 1, 3, 0);
    terminal(&f, 1, SUB_TS, 61, MOQ_FILL_COMPLETE);
    MOQ_TEST_CHECK_EQ_U64(f.r->ev_tail - f.r->ev_head, 0);
    MOQ_TEST_CHECK(receiver_fill_items_ready(f.r));
    MOQ_TEST_CHECK(rq_push(f.r, MOQ_MEDIA_TRACK_ENDED, f.loc));
    moq_media_object_t o;
    if (next_object(&f, &o)) moq_media_object_cleanup(&o);
    moq_media_track_event_t ev;
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK &&
                   ev.kind == MOQ_MEDIA_TRACK_ENDED);
    expect_terminal(&f, 61, 0, 0, 0);
    MOQ_TEST_CHECK(rq_push(f.r, MOQ_MEDIA_TRACK_REMOVED, f.loc));
    MOQ_TEST_CHECK(!moq_media_receiver_is_fatal(f.r));
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK &&
                   ev.kind == MOQ_MEDIA_TRACK_REMOVED);
    f.r->ev_head = f.r->ev_tail = 0;
    f.r->ev_cap = cap;
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_one_slot_ring");
}

/* A caller whose struct_size ends before the summary gets every earlier
 * field and nothing past its size; one covering only part of the summary
 * reads it as absent. */
static void test_short_caller(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    const size_t old = offsetof(moq_media_track_event_t, fill_unreported_loss_causes);
    const size_t part = offsetof(moq_media_track_event_t, fill_unreported_loss_group);
    for (int round = 0; round < 2; round++) {
        created(&f, 1, SUB_TS, 71);
        range(&f, 1, MOQ_FETCH_RANGE_TIMED_OUT, 2, 2);
        terminal(&f, 1, SUB_TS, 71, MOQ_FILL_COMPLETE);
        size_t size = round ? part : old;
        moq_media_track_event_t ev;
        memset(&ev, 0xa5, sizeof(ev));
        MOQ_TEST_CHECK_EQ_INT((int)moq_media_receiver_poll_track(f.r, &ev, size),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(ev.struct_size, size);
        MOQ_TEST_CHECK_EQ_INT((int)ev.kind, (int)MOQ_MEDIA_TRACK_FILL_TERMINAL);
        MOQ_TEST_CHECK_EQ_U64(ev.initiating_request_id, 71);
        MOQ_TEST_CHECK_EQ_U64(ev.fill_reason, MOQ_FILL_COMPLETE);
        const uint8_t *b = (const uint8_t *)&ev;
        for (size_t i = size; i < sizeof(ev); i++)
            MOQ_TEST_CHECK(b[i] == 0xa5);
        if (round) MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_causes, 0);
    }
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_short_caller");
}

/* A fill of another track and the catalog fill take no record: the other
 * track's terminal is queued at once, the catalog's stays private. */
static void test_other_fills_unchanged(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    created(&f, 1, SUB_LOC, 81);
    created(&f, 2, SUB_CATALOG, 82);
    for (size_t i = 0; i < RECEIVER_FILL_DOMS; i++)
        MOQ_TEST_CHECK_EQ_INT(f.r->fill_doms[i].state, DOM_FREE);
    terminal(&f, 2, SUB_CATALOG, 82, MOQ_FILL_COMPLETE);
    expect_no_event(&f);
    terminal(&f, 1, SUB_LOC, 81, MOQ_FILL_COMPLETE);
    MOQ_TEST_CHECK_EQ_U64(f.r->ev_tail - f.r->ev_head, 1);
    moq_media_track_event_t ev;
    memset(&ev, 0xa5, sizeof(ev));
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)ev.kind, (int)MOQ_MEDIA_TRACK_FILL_TERMINAL);
    MOQ_TEST_CHECK(ev.track == f.loc);
    MOQ_TEST_CHECK_EQ_U64(ev.initiating_request_id, 81);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_causes, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_object, 0);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_other_fills_unchanged");
}

/* A LIVE Object of the TS track, as the reorder window releases it with no
 * gap of its own. */
static void live_object(fx_t *f, uint64_t g)
{
    uint8_t buf[8 * 188];
    size_t n = ts_gen_gop(&f->gen, buf, 1);
    moq_sub_object_t so;
    memset(&so, 0, sizeof(so));
    so.track = SUB_TS;
    so.group_id = g;
    so.status = MOQ_OBJECT_NORMAL;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f->r->alloc, buf, n, &so.payload) == MOQ_OK);
    receiver_loss_t loss;
    memset(&loss, 0, sizeof(loss));
    receiver_deliver_object(f->r, f->ts, &so, &loss, NULL, MOQ_MEDIA_DELIVERY_LIVE, 0,
                            NULL, NULL);
}

static void live_loss(fx_t *f, uint32_t gen, uint64_t g)
{
    pthread_mutex_lock(&f->r->mu);
    ts_note_loss_locked(f->r, f->ts, gen, g, 0, f->r->obj_head);
    pthread_mutex_unlock(&f->r->mu);
}

/* LIVE loss belongs to its subscription generation. A loss of the next
 * generation is not reported on a queued Object of the previous one, but on
 * the next generation's Object; a loss left from the previous generation is
 * not reported on the next generation's first Object. */
static void test_live_generation_scope(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    f.ts->ts_gen = 1;
    live_object(&f, 1);           /* generation 1, queued */
    f.ts->ts_gen = 2;             /* a new subscription's window */
    live_loss(&f, 2, 7);
    live_object(&f, 8);           /* generation 2 */
    moq_media_object_t o;
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 1);
        MOQ_TEST_CHECK(o.has_loss_report && !o.gap);
        moq_media_object_cleanup(&o);
    }
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 8);
        MOQ_TEST_CHECK(o.has_loss_report && o.gap);
        MOQ_TEST_CHECK_EQ_U64(o.gap_group, 7);
        moq_media_object_cleanup(&o);
    }
    live_loss(&f, 2, 9);          /* nothing of generation 2 queued */
    f.ts->ts_gen = 3;
    live_object(&f, 1);           /* generation 3's first Object */
    if (next_object(&f, &o)) {
        MOQ_TEST_CHECK(o.has_loss_report && !o.gap);
        moq_media_object_cleanup(&o);
    }
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_live_generation_scope");
}

/* -- Payload transform of fill Objects -------------------------------- */

static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* The m2ts receiver: a 192-octet track with PSI init data, the prefix
 * stripped and the join PSI on, and a subscription's window started. */
static size_t g_psi188;   /* the PSI's length once stripped */

static bool fx_open_m2ts_max(fx_t *f, uint32_t max_objects)
{
    ts_gen_t g;
    ts_gen_init(&g, 192, 1, false);
    uint8_t psi[2 * 192];
    size_t psi_len = ts_gen_psi(&g, psi);
    g_psi188 = psi_len / 192 * 188;
    static char psi_b64[2 * 192 * 2];
    b64(psi, psi_len, psi_b64);
    static char json[2048];
    snprintf(json, sizeof(json),
        "{\"version\":1,"
        "\"initDataList\":[{\"id\":\"psi\",\"type\":\"inline\",\"data\":\"%s\"}],"
        "\"tracks\":["
        "{\"name\":\"ts\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
        "\"mimeType\":\"video/mp2t\",\"mpeg2tsMode\":\"unmodified-program\","
        "\"mpeg2tsPacketSize\":192,\"mpeg2tsProgramNumber\":1,"
        "\"mpeg2tsTimestampMode\":\"arrival-time\",\"initRef\":\"psi\"},"
        "{\"name\":\"loc\",\"packaging\":\"loc\",\"isLive\":true,"
        "\"role\":\"video\",\"codec\":\"avc1.42e01e\"}]}", psi_b64);
    if (!fx_open_catalog(f, max_objects, json, true)) return false;
    MOQ_TEST_CHECK(f->ts->desc.init_data.len == psi_len);
    MOQ_TEST_CHECK(receiver_ts_start(f->r, f->ts, 0));
    return f->ts->ts != NULL;
}

static bool fx_open_m2ts(fx_t *f)
{
    return fx_open_m2ts_max(f, 0);
}

/* One 192-octet GOP Object; *stripped receives its 188-octet packets. */
static size_t m2ts_payload(fx_t *f, uint8_t *raw, uint8_t *stripped, size_t *slen)
{
    size_t n = ts_gen_gop(&f->gen, raw, 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 192) {
        memcpy(stripped + o, raw + i + 4, 188);
        o += 188;
    }
    *slen = o;
    return n;
}

static void m2ts_fill_object(fx_t *f, uint64_t token, uint64_t g, uint64_t o,
                             uint8_t *stripped, size_t *slen)
{
    uint8_t raw[8 * 192];
    size_t n = m2ts_payload(f, raw, stripped, slen);
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_OBJECT;
    it.request = tok(token);
    it.track = SUB_TS;
    it.u.object.group_id = g;
    it.u.object.object_id = o;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f->r->alloc, raw, n, &it.u.object.payload) == MOQ_OK);
    feed(f, &it);
}

/* A LIVE Object offered to the window and released by its deadline. */
static void m2ts_live_object(fx_t *f, uint64_t g, uint8_t *stripped, size_t *slen)
{
    uint8_t raw[8 * 192];
    size_t n = m2ts_payload(f, raw, stripped, slen);
    moq_sub_object_t so;
    memset(&so, 0, sizeof(so));
    so.track = SUB_TS;
    so.group_id = g;
    so.status = MOQ_OBJECT_NORMAL;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f->r->alloc, raw, n, &so.payload) == MOQ_OK);
    receiver_on_ts_object(f->r, f->ts, &so, 1000);
    receiver_ts_drain(f->r, f->ts, UINT64_C(1) << 40);
}

/* The next Object is (g, o) of `origin` and its payload is 188-octet
 * packets: the join PSI when `psi`, then exactly `stripped`. */
static void expect_m2ts(fx_t *f, uint64_t g, uint32_t origin, bool psi,
                        const uint8_t *stripped, size_t slen)
{
    moq_media_object_t o;
    if (!next_object(f, &o)) return;
    MOQ_TEST_CHECK_EQ_U64(o.group_id, g);
    MOQ_TEST_CHECK_EQ_INT((int)o.delivery_origin, (int)origin);
    size_t head = psi ? g_psi188 : 0;
    MOQ_TEST_CHECK_EQ_U64(o.payload.len, head + slen);
    MOQ_TEST_CHECK(o.payload.len % 188 == 0);
    if (o.payload.len == head + slen) {
        for (size_t i = 0; i < o.payload.len; i += 188)
            MOQ_TEST_CHECK(o.payload.data[i] == 0x47);
        if (psi) {
            /* The PAT, then the PMT. */
            MOQ_TEST_CHECK((((o.payload.data[1] & 0x1F) << 8) | o.payload.data[2]) == 0);
            MOQ_TEST_CHECK((((o.payload.data[189] & 0x1F) << 8) | o.payload.data[190]) ==
                           TS_PMT_PID);
        }
        MOQ_TEST_CHECK(memcmp(o.payload.data + head, stripped, slen) == 0);
    }
    moq_media_object_cleanup(&o);
}

/* The subscription's join fill delivers first: its first Object carries
 * the PSI, nothing after it does, and every payload -- the join fill's, a
 * historical fill's and the LIVE one -- is stripped to 188 octets. */
static void test_m2ts_fill_first(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188], s3[8 * 188], s4[8 * 188];
    size_t l1, l2, l3, l4;
    created(&f, 1, SUB_TS, 91);
    created(&f, 2, SUB_TS, 92);
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    m2ts_fill_object(&f, 1, 5, 1, s2, &l2);
    m2ts_fill_object(&f, 2, 2, 0, s3, &l3);
    m2ts_live_object(&f, 6, s4, &l4);
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, true, s1, l1);
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, false, s2, l2);
    expect_m2ts(&f, 2, MOQ_MEDIA_DELIVERY_FILL, false, s3, l3);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, false, s4, l4);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_fill_first");
}

/* A LIVE Object delivered first carries the PSI; the fill after it does
 * not, and is stripped. */
static void test_m2ts_live_first(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 93);
    m2ts_live_object(&f, 6, s1, &l1);
    m2ts_fill_object(&f, 1, 5, 0, s2, &l2);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s1, l1);
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, false, s2, l2);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_live_first");
}

/* A fill of an earlier subscription never takes the current one's PSI:
 * it is stripped only, and the current subscription's first LIVE Object
 * still carries the PSI. */
static void test_m2ts_earlier_subscription_fill(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 94);
    MOQ_TEST_CHECK(receiver_ts_start(f.r, f.ts, 0));   /* the next subscription */
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    m2ts_live_object(&f, 6, s2, &l2);
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, false, s1, l1);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s2, l2);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_earlier_subscription_fill");
}

/* A transformed fill payload refused at admission (a straggler of an
 * evicted Group) is released, and the refusal is a local drop of the fill.
 * It was never delivered, so the subscription's PSI goes on the Object that
 * is: the LIVE one after it. */
static void test_m2ts_refused_releases(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    f.ts->has_drop_filter = true;
    f.ts->drop_below_group = 5;
    created(&f, 1, SUB_TS, 95);
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    MOQ_TEST_CHECK_EQ_U64(f.r->obj_tail - f.r->obj_head, 0);
    m2ts_live_object(&f, 6, s2, &l2);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s2, l2);
    terminal(&f, 1, SUB_TS, 95, MOQ_FILL_COMPLETE);
    moq_media_track_event_t ev;
    memset(&ev, 0, sizeof(ev));
    MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)ev.kind, (int)MOQ_MEDIA_TRACK_FILL_TERMINAL);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_causes, MOQ_MEDIA_FILL_LOSS_LOCAL_DROP);
    MOQ_TEST_CHECK_EQ_U64(ev.fill_unreported_loss_group, 5);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_refused_releases");
}

/* A fill item read the way a failed catalog's retirement reads it: observed
 * only (its Objects are discarded), gated by event-ring room alone. */
static void retire_observe(fx_t *f, uint32_t kind, uint64_t token, uint64_t id)
{
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.struct_size = sizeof(it);
    it.kind = kind;
    it.request = tok(token);
    it.track = SUB_TS;
    it.initiating_request_id = id;
    MOQ_TEST_CHECK(receiver_fill_poll_ready(f->r));
    receiver_fill_observe(f->r, &it);
    moq_sub_fill_item_cleanup(&it);
}

static int doms_in_use(const fx_t *f)
{
    int n = 0;
    for (size_t i = 0; i < RECEIVER_FILL_DOMS; i++)
        n += f->r->fill_doms[i].state != DOM_FREE;
    return n;
}

/* A failed catalog's retirement takes no fill record: with every record in
 * use, or with records free, a TS fill CREATED there is observed without
 * one and its terminal is queued at once. With every record in use and no
 * failed catalog, a CREATED that reached here anyway also takes none. */
static void test_retire_takes_no_record(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open(&f, 64)) { fx_close(&f); return; }
    for (uint64_t i = 1; i <= RECEIVER_FILL_DOMS; i++) {
        created(&f, i, SUB_TS, 100 + i);
        object(&f, i, i, 0);
        terminal(&f, i, SUB_TS, 100 + i, MOQ_FILL_COMPLETE);
    }
    MOQ_TEST_CHECK_EQ_INT(doms_in_use(&f), (int)RECEIVER_FILL_DOMS);
    for (int failed = 1; failed >= 0; failed--) {
        uint64_t token = 200 + (uint64_t)failed, id = 300 + (uint64_t)failed;
        f.r->catalog_ack_failed = failed != 0;
        retire_observe(&f, MOQ_SUB_FILL_CREATED, token, id);
        MOQ_TEST_CHECK_EQ_INT(doms_in_use(&f), (int)RECEIVER_FILL_DOMS);
        retire_observe(&f, MOQ_SUB_FILL_TERMINAL, token, id);
        MOQ_TEST_CHECK_EQ_U64(f.r->ev_tail - f.r->ev_head, 1);
        moq_media_track_event_t ev;
        memset(&ev, 0, sizeof(ev));
        MOQ_TEST_CHECK(moq_media_receiver_poll_track(f.r, &ev, sizeof(ev)) == MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)ev.kind, (int)MOQ_MEDIA_TRACK_FILL_TERMINAL);
        MOQ_TEST_CHECK_EQ_U64(ev.initiating_request_id, id);
    }
    f.r->catalog_ack_failed = false;
    fx_close(&f);

    if (!fx_open(&f, 0)) { fx_close(&f); return; }
    f.r->catalog_ack_failed = true;
    retire_observe(&f, MOQ_SUB_FILL_CREATED, 1, 401);
    MOQ_TEST_CHECK_EQ_INT(doms_in_use(&f), 0);
    retire_observe(&f, MOQ_SUB_FILL_TERMINAL, 1, 401);
    MOQ_TEST_CHECK_EQ_U64(f.r->ev_tail - f.r->ev_head, 1);
    f.r->catalog_ack_failed = false;
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_fill_retire_takes_no_record");
}

/* An allocator that refuses while `refuse` is set, else counts as the
 * test allocator does. */
static bool g_refuse;
static void *refusing_alloc(size_t size, void *ctx)
{
    return g_refuse ? NULL : test_alloc(size, ctx);
}

/* The first fill Object's payload transform cannot allocate: it is dropped,
 * never delivered, and the PSI goes on the next delivered Object. */
static void test_m2ts_join_after_transform_failure(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188], raw[8 * 192];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 96);
    size_t n = m2ts_payload(&f, raw, s1, &l1);
    moq_sub_fill_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = MOQ_SUB_FILL_OBJECT;
    it.request = tok(1);
    it.track = SUB_TS;
    it.u.object.group_id = 5;
    MOQ_TEST_CHECK(moq_rcbuf_create(&f.r->alloc, raw, n, &it.u.object.payload) == MOQ_OK);
    void *(*saved)(size_t, void *) = f.r->alloc.alloc;
    f.r->alloc.alloc = refusing_alloc;
    g_refuse = true;
    feed(&f, &it);
    g_refuse = false;
    f.r->alloc.alloc = saved;
    MOQ_TEST_CHECK_EQ_U64(f.r->obj_tail - f.r->obj_head, 0);
    m2ts_live_object(&f, 6, s2, &l2);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s2, l2);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_join_after_transform_failure");
}

/* The first Object is queued and then evicted before the application polls
 * it: it was never delivered, so the PSI goes on the Object that is. */
static void test_m2ts_join_after_eviction(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts_max(&f, 1)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 97);
    created(&f, 2, SUB_TS, 98);
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    MOQ_TEST_CHECK_EQ_U64(f.r->obj_tail - f.r->obj_head, 1);
    m2ts_fill_object(&f, 2, 9, 0, s2, &l2);   /* evicts Group 5 */
    expect_m2ts(&f, 9, MOQ_MEDIA_DELIVERY_FILL, true, s2, l2);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_join_after_eviction");
}

/* An Object queued in one subscription and delivered after the next has
 * started is of the earlier one: it never takes the current PSI, which goes
 * on the current subscription's first delivered Object. */
static void test_m2ts_join_skips_earlier_queued(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 99);
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    MOQ_TEST_CHECK(receiver_ts_start(f.r, f.ts, 0));   /* the next subscription */
    m2ts_live_object(&f, 6, s2, &l2);
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, false, s1, l1);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s2, l2);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_join_skips_earlier_queued");
}

/* A current-subscription Object prepared with no PSI octets is delivered
 * first: the join is not spent on it, and the next delivered Object carries
 * the PSI. No intake path is known to prepare such an Object (an empty TS
 * Object is refused, and malformed packets are refused by the parser), so
 * the queued entry is set to that state directly. */
static void test_m2ts_join_not_spent_without_psi(void)
{
    int before = failures;
    fx_t f;
    if (!fx_open_m2ts(&f)) { fx_close(&f); return; }
    uint8_t s1[8 * 188], s2[8 * 188];
    size_t l1, l2;
    created(&f, 1, SUB_TS, 100);
    m2ts_fill_object(&f, 1, 5, 0, s1, &l1);
    MOQ_TEST_CHECK_EQ_U64(f.r->obj_tail - f.r->obj_head, 1);
    receiver_obj_entry_t *e = &f.r->objs[f.r->obj_head % f.r->ring_cap];
    MOQ_TEST_CHECK(e->join_candidate && e->join_psi_len == g_psi188);
    e->obj.payload.data += e->join_psi_len;
    e->obj.payload.len -= e->join_psi_len;
    e->join_psi_len = 0;
    expect_m2ts(&f, 5, MOQ_MEDIA_DELIVERY_FILL, false, s1, l1);
    MOQ_TEST_CHECK(!f.ts->ts_join_delivered);
    m2ts_live_object(&f, 6, s2, &l2);
    expect_m2ts(&f, 6, MOQ_MEDIA_DELIVERY_LIVE, true, s2, l2);
    MOQ_TEST_CHECK(f.ts->ts_join_delivered);
    fx_close(&f);
    if (failures == before) MOQ_TEST_PASS("ts_m2ts_join_not_spent_without_psi");
}

int main(void)
{
#ifdef MOQ_SERVICE_HAVE_MPEG2TS
    test_tail_without_objects();
    test_drop_after_terminal();
    test_earliest_position();
    test_carry_to_next_object();
    test_zero_object_terminal_order();
    test_pool_full_progress();
    test_one_slot_ring();
    test_short_caller();
    test_other_fills_unchanged();
    test_live_generation_scope();
    test_m2ts_fill_first();
    test_m2ts_live_first();
    test_m2ts_earlier_subscription_fill();
    test_m2ts_refused_releases();
    test_m2ts_join_after_transform_failure();
    test_m2ts_join_after_eviction();
    test_m2ts_join_skips_earlier_queued();
    test_m2ts_join_not_spent_without_psi();
    test_retire_takes_no_record();
#endif
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
