/* Ordinary-use controls drive only the public facade/session/simpair APIs.
 * Consume and assert CREATED before the existing OBJECT/GAP/TERMINAL quota
 * controls; the sized-fill fixture separately exercises CREATED pressure.
 * The admission-internals companion additionally observes the private event
 * transaction without changing how requests or data enter the core. */
#include <moq/subscriber.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>
#ifdef MOQ_TEST_ADMISSION_INTERNALS
#include "../../core/src/session/session_internal.h"
#endif

static int failures;

static bool check(bool ok, const char *expr, int line)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, line, expr);
        failures++;
    }
    return ok;
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define REQUIRE(expr) do { if (!CHECK(expr)) return false; } while (0)
#define NEED(expr) do { if (!CHECK(expr)) goto done; } while (0)

enum { MAX_ALLOCATIONS = 32, MAX_EVENTS = 64, PAYLOAD_BYTES = 4 };
static const uint8_t properties[] = {0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb};
static const uint8_t payload[] = {'b', 'o', 'd', 'y'};

typedef struct ledger {
    test_alloc_state_t state;
    size_t calls, bytes, fail_at;
    size_t sizes[MAX_ALLOCATIONS];
} ledger_t;

static void *record_alloc(size_t size, void *ctx)
{
    ledger_t *l = ctx;
    if (l->calls < MAX_ALLOCATIONS) l->sizes[l->calls] = size;
    l->calls++;
    if (l->fail_at && l->calls == l->fail_at) return NULL;
    void *p = test_alloc(size, &l->state);
    if (p) l->bytes += size;
    return p;
}

static void record_free(void *p, size_t size, void *ctx)
{
    ledger_t *l = ctx;
    if (p && CHECK(l->bytes >= size)) l->bytes -= size;
    test_free(p, size, &l->state);
}

typedef struct fixture {
    test_alloc_state_t pair_state;
    moq_alloc_t pair_alloc;
    ledger_t ledger;
    moq_simpair_t *pair;
    moq_subscriber_t *sub;
    moq_subscription_t peer_sub;
    moq_fill_t peer_fill;
    moq_fetch_t peer_fetch;
    uint64_t fill_initiating_request_id;
} fixture_t;

static uint64_t now(fixture_t *f) { return moq_simpair_now_us(f->pair); }
static moq_session_t *server(fixture_t *f) { return moq_simpair_server(f->pair); }

static bool pump(fixture_t *f)
{
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 64, NULL) == MOQ_OK);
    return true;
}

static bool collect_peer(fixture_t *f)
{
    for (unsigned i = 0; i < MAX_EVENTS; i++) {
        moq_event_t ev;
        if (moq_session_poll_events(server(f), &ev, 1) == 0) return true;
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            f->peer_sub = ev.u.subscribe_request.sub;
        else if (ev.kind == MOQ_EVENT_FILL_REQUEST)
            f->peer_fill = ev.u.fill_request.fill;
        else if (ev.kind == MOQ_EVENT_FETCH_REQUEST)
            f->peer_fetch = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    REQUIRE(false); /* A fixture never intentionally produces this many events. */
    return false;
}

static bool start_limits(fixture_t *f, moq_version_t version, uint32_t fetch_items,
                     uint32_t fill_items, uint32_t objects, uint64_t bytes,
                     bool streaming, uint32_t actions)
{
    memset(f, 0, sizeof(*f));
    f->pair_alloc = test_allocator(&f->pair_state);
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = &f->pair_alloc;
    pc.version = version;
    pc.seed = 7;
    pc.initial_now_us = 1000;
    pc.client_streaming_objects = streaming;
    pc.max_actions = actions;
    pc.client_send_request_capacity = pc.server_send_request_capacity = true;
    pc.client_initial_request_capacity = pc.server_initial_request_capacity = 16;
    REQUIRE(moq_simpair_create(&pc, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK);
    REQUIRE(pump(f));
    moq_session_t *ends[] = {moq_simpair_client(f->pair), server(f)};
    for (unsigned end = 0; end < 2; end++) {
        REQUIRE(moq_session_state(ends[end]) == MOQ_SESS_ESTABLISHED);
        unsigned n;
        for (n = 0; n < MAX_EVENTS; n++) {
            moq_event_t ev;
            if (moq_session_poll_events(ends[end], &ev, 1) == 0) break;
            moq_event_cleanup(&ev);
        }
        REQUIRE(n < MAX_EVENTS);
    }
    moq_alloc_t alloc = {.ctx = &f->ledger, .alloc = record_alloc, .free = record_free};
    moq_sub_cfg_t cfg;
    moq_sub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.max_tracks = 2;
    cfg.max_objects = objects;
    cfg.max_fetches = 2;
    cfg.max_fetch_items = fetch_items;
    cfg.max_fills = 2;
    cfg.max_fill_items = fill_items;
    cfg.streaming_objects = streaming;
    cfg.max_chunks = streaming ? 1 : 2;
    cfg.max_queued_object_bytes = bytes;
    REQUIRE(moq_sub_create(moq_simpair_client(f->pair), &alloc, &cfg, &f->sub) == MOQ_OK);
    return true;
}

static bool start_ex(fixture_t *f, moq_version_t version, uint32_t fetch_items,
                     uint32_t fill_items, uint32_t objects, uint64_t bytes,
                     bool streaming)
{
    return start_limits(f, version, fetch_items, fill_items, objects, bytes, streaming, 0);
}

static bool start(fixture_t *f, moq_version_t version, uint32_t fetch_items,
                  uint32_t fill_items, uint32_t objects, uint64_t bytes)
{
    return start_ex(f, version, fetch_items, fill_items, objects, bytes, false);
}

static void finish(fixture_t *f)
{
    if (f->sub) moq_sub_destroy(f->sub);
    if (f->pair) moq_simpair_destroy(f->pair);
    CHECK(f->ledger.state.balance == 0);
    CHECK(f->ledger.bytes == 0);
    CHECK(f->pair_state.balance == 0);
}

static bool stats(fixture_t *f, moq_sub_stats_t *out)
{
    REQUIRE(moq_sub_get_stats(f->sub, out, sizeof(*out)) == MOQ_OK);
    return true;
}

static bool request_track(fixture_t *f, bool fill, moq_sub_track_t **out)
{
    moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("history")};
    moq_location_filter_cfg_t location, fill_location;
    moq_location_filter_cfg_init(&location);
    moq_location_filter_cfg_init(&fill_location);
    location.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    fill_location.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    moq_fill_cfg_t fc;
    moq_fill_cfg_init(&fc);
    fc.location = &fill_location;
    moq_sub_track_cfg_t cfg;
    moq_sub_track_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){ns, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    if (fill) { cfg.location = &location; cfg.fill = &fc; }
    f->peer_sub = MOQ_SUBSCRIPTION_INVALID;
    f->peer_fill = MOQ_FILL_INVALID;
    REQUIRE(moq_sub_subscribe(f->sub, &cfg, now(f), out) == MOQ_OK);
    REQUIRE(*out != NULL);
    REQUIRE(pump(f) && collect_peer(f));
    REQUIRE(moq_subscription_is_valid(f->peer_sub));
    if (fill) {
        REQUIRE(moq_sub_tick(f->sub, now(f)) == MOQ_OK);
        moq_sub_fill_item_t created;
        REQUIRE(moq_sub_poll_fill(f->sub, &created, sizeof(created)) == MOQ_OK);
        bool ok = created.kind == MOQ_SUB_FILL_CREATED && created.track == *out &&
            moq_sub_fill_eq(created.request, moq_sub_track_fill(*out));
        f->fill_initiating_request_id = created.initiating_request_id;
        moq_sub_fill_item_cleanup(&created);
        REQUIRE(ok);
    }
    return true;
}

static bool subscribe_track(fixture_t *f, bool fill, bool ack_properties,
                            moq_sub_track_t **out)
{
    REQUIRE(request_track(f, fill, out));
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    if (fill) { ac.has_largest = true; ac.largest_group = 0; ac.largest_object = 2; }
    if (ack_properties) ac.track_properties = (moq_bytes_t){properties, sizeof(properties)};
    REQUIRE(moq_session_accept_subscribe(server(f), f->peer_sub, &ac, now(f)) == MOQ_OK);
    REQUIRE(pump(f) && collect_peer(f));
    REQUIRE(moq_sub_tick(f->sub, now(f)) == MOQ_OK);
    REQUIRE(moq_sub_track_is_active(*out));
    if (fill) {
        REQUIRE(moq_fill_is_valid(f->peer_fill));
        REQUIRE(moq_sub_fill_is_valid(moq_sub_track_fill(*out)));
    }
    return true;
}

static bool send_live_bytes(fixture_t *f, uint64_t group, size_t length)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    moq_subgroup_handle_t handle;
    REQUIRE(moq_session_open_subgroup(server(f), f->peer_sub, &cfg, now(f), &handle) == MOQ_OK);
    moq_rcbuf_t *body = NULL;
    REQUIRE(length <= sizeof(payload));
    REQUIRE(moq_rcbuf_create(&f->pair_alloc, payload, length, &body) == MOQ_OK);
    moq_result_t rc = moq_session_write_object(server(f), handle, 0, body, now(f));
    moq_rcbuf_decref(body);
    REQUIRE(rc == MOQ_OK);
    REQUIRE(moq_session_close_subgroup(server(f), handle, now(f)) == MOQ_OK);
    REQUIRE(pump(f));
    return true;
}

static bool send_live(fixture_t *f, uint64_t group)
{
    return send_live_bytes(f, group, sizeof(payload));
}

static bool send_fill_object(fixture_t *f, uint64_t object)
{
    moq_fetch_object_cfg_t cfg;
    moq_fetch_object_cfg_init(&cfg);
    cfg.group_id = 0;
    cfg.object_id = object;
    REQUIRE(moq_rcbuf_create(&f->pair_alloc, payload, sizeof(payload), &cfg.payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_fill_object(server(f), f->peer_fill, &cfg, now(f));
    moq_rcbuf_decref(cfg.payload);
    REQUIRE(rc == MOQ_OK);
    REQUIRE(pump(f));
    return true;
}

static bool poll_live(fixture_t *f, moq_sub_track_t *track, uint64_t group)
{
    moq_sub_object_t item;
    REQUIRE(moq_sub_poll_object(f->sub, &item) == MOQ_OK);
    bool ok = item.track == track && item.group_id == group && item.object_id == 0 &&
        item.payload && moq_rcbuf_len(item.payload) == sizeof(payload) &&
        memcmp(moq_rcbuf_data(item.payload), payload, sizeof(payload)) == 0;
    moq_sub_object_cleanup(&item);
    REQUIRE(ok);
    return true;
}

static bool poll_fill_object(fixture_t *f, moq_sub_track_t *track,
                             moq_sub_fill_t child, uint64_t object)
{
    moq_sub_fill_item_t item;
    REQUIRE(moq_sub_poll_fill(f->sub, &item, sizeof(item)) == MOQ_OK);
    bool ok = item.kind == MOQ_SUB_FILL_OBJECT && item.track == track &&
        moq_sub_fill_eq(item.request, child) && item.u.object.group_id == 0 &&
        item.u.object.object_id == object && item.u.object.payload &&
        moq_rcbuf_len(item.u.object.payload) == sizeof(payload) &&
        memcmp(moq_rcbuf_data(item.u.object.payload), payload, sizeof(payload)) == 0;
    moq_sub_fill_item_cleanup(&item);
    REQUIRE(ok);
    return true;
}

/* On an early successful release, go straight to destruction, without using
 * the track again. 3750 pins the existing WRONG_STATE result. */
static bool release_refused(fixture_t *f, moq_sub_track_t *track)
{
    moq_result_t rc = moq_sub_release_track(f->sub, track);
    REQUIRE(rc == MOQ_ERR_WRONG_STATE);
    REQUIRE(moq_sub_track_get_state(track) == MOQ_SUB_TRACK_DONE);
    return true;
}

static void test_live_retirement(moq_version_t version, bool pending_only)
{
    fixture_t f;
    NEED(start(&f, version, 2, 2, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, false, &track));
    NEED(send_live(&f, 1));
    if (pending_only) NEED(send_live(&f, 2));
    NEED(moq_sub_tick(f.sub, now(&f)) == (pending_only ? MOQ_ERR_WOULD_BLOCK : MOQ_OK));
    if (pending_only) NEED(poll_live(&f, track, 1));
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.objects_queued == 1 && snapshot.bytes_queued == PAYLOAD_BYTES);
    NEED(moq_sub_unsubscribe(f.sub, track, now(&f)) == MOQ_OK);
    if (version != MOQ_VERSION_DRAFT_22) {
        /* Legacy release accepts queued/pending items. Do not use track again. */
        NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
        goto done;
    }
    NEED(release_refused(&f, track));
    if (pending_only) NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll_live(&f, track, pending_only ? 2 : 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(stats(&f, &snapshot));
    NEED(snapshot.objects_queued == 0 && snapshot.bytes_queued == 0);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static void test_fill_retirement(unsigned objects)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 1, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, true, false, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    for (unsigned i = 0; i < objects; i++) NEED(send_fill_object(&f, i));
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == (objects == 2 ? MOQ_ERR_WOULD_BLOCK : MOQ_OK));
    if (objects == 2) NEED(poll_fill_object(&f, track, child, 0));
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fill_items_queued == (objects ? 1u : 0u));
    NEED(snapshot.bytes_queued == (objects ? PAYLOAD_BYTES : 0u));
    NEED(moq_sub_unsubscribe(f.sub, track, now(&f)) == MOQ_OK);
    NEED(release_refused(&f, track));
    if (objects) {
        moq_result_t rc = moq_sub_tick(f.sub, now(&f));
        NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        NEED(poll_fill_object(&f, track, child, objects - 1));
    }
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(release_refused(&f, track)); /* The terminal metadata still owns credit. */
    moq_sub_fill_item_t terminal;
    NEED(moq_sub_poll_fill(f.sub, &terminal, sizeof(terminal)) == MOQ_OK);
    bool ok = terminal.kind == MOQ_SUB_FILL_TERMINAL && terminal.track == track &&
        moq_sub_fill_eq(terminal.request, child) &&
        terminal.u.terminal.reason == MOQ_FILL_PARENT_TERMINATED;
    moq_sub_fill_item_cleanup(&terminal);
    NEED(ok);
    NEED(!moq_sub_fill_is_valid(moq_sub_track_fill(track)));
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fill_items_queued == 0 && snapshot.bytes_queued == 0);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static void test_chunk_retirement(moq_version_t version, bool pending_only)
{
    fixture_t f;
    NEED(start_ex(&f, version, 2, 2, 1, 64, true));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, false, &track));
    NEED(send_live(&f, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_sub_chunk_t item;
    NEED(moq_sub_poll_chunk(f.sub, &item) == MOQ_OK);
    bool ok = item.track == track && item.begin && !item.end;
    moq_sub_chunk_cleanup(&item);
    NEED(ok);
    if (!pending_only) NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.chunks_queued == 1 && snapshot.bytes_queued == PAYLOAD_BYTES);
    NEED(moq_sub_unsubscribe(f.sub, track, now(&f)) == MOQ_OK);
    if (version != MOQ_VERSION_DRAFT_22) {
        NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
        goto done;
    }
    NEED(release_refused(&f, track));
    if (pending_only) NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_poll_chunk(f.sub, &item) == MOQ_OK);
    ok = item.track == track && item.end && item.chunk &&
        item.terminal == MOQ_OBJECT_TERMINAL_NORMAL &&
        moq_rcbuf_len(item.chunk) == sizeof(payload) &&
        memcmp(moq_rcbuf_data(item.chunk), payload, sizeof(payload)) == 0;
    moq_sub_chunk_cleanup(&item);
    NEED(ok);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static void test_closed_child(unsigned objects, bool terminal_queued)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 1, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, true, false, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    for (unsigned i = 0; i < objects; i++) NEED(send_fill_object(&f, i));
    if (terminal_queued) {
        NEED(moq_session_end_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
        NEED(pump(&f));
    }
    NEED(moq_sub_tick(f.sub, now(&f)) == (objects == 2 ? MOQ_ERR_WOULD_BLOCK : MOQ_OK));
    if (objects == 2) NEED(poll_fill_object(&f, track, child, 0));
    NEED(moq_session_on_transport_close(moq_simpair_client(f.pair), 0, now(&f)) == MOQ_OK);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.closed);
    if (objects || terminal_queued) NEED(release_refused(&f, track));
    if (objects) NEED(poll_fill_object(&f, track, child, objects - 1));
    if (terminal_queued) {
        moq_sub_fill_item_t item;
        NEED(moq_sub_poll_fill(f.sub, &item, sizeof(item)) == MOQ_OK);
        bool ok = item.track == track && item.kind == MOQ_SUB_FILL_TERMINAL &&
            moq_sub_fill_eq(item.request, child) && item.u.terminal.reason == MOQ_FILL_COMPLETE;
        moq_sub_fill_item_cleanup(&item);
        NEED(ok);
    }
    NEED(!moq_sub_fill_is_valid(moq_sub_track_fill(track)));
    moq_sub_fill_item_t empty;
    NEED(moq_sub_poll_fill(f.sub, &empty, sizeof(empty)) == MOQ_DONE); /* No synthetic close terminal. */
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static void test_rejected_parent(void)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 1, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(request_track(&f, true, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    moq_reject_subscribe_cfg_t cfg;
    moq_reject_subscribe_cfg_init(&cfg);
    cfg.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
    NEED(moq_session_reject_subscribe(server(&f), f.peer_sub, &cfg, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_track_get_state(track) == MOQ_SUB_TRACK_ERROR);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_ERR_WRONG_STATE);
    moq_sub_fill_item_t item;
    NEED(moq_sub_poll_fill(f.sub, &item, sizeof(item)) == MOQ_OK);
    bool ok = item.kind == MOQ_SUB_FILL_TERMINAL && item.track == track &&
        moq_sub_fill_eq(item.request, child) && item.u.terminal.reason == MOQ_FILL_PARENT_REJECTED &&
        item.u.terminal.error_domain == MOQ_FILL_ERROR_PARENT_REQUEST &&
        item.u.terminal.error_code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
    moq_sub_fill_item_cleanup(&item);
    NEED(ok);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

typedef struct allocation_sample {
    size_t calls, bytes;
    size_t sizes[MAX_ALLOCATIONS];
} allocation_sample_t;

static bool allocation_sample(uint32_t fetch, uint32_t fill, allocation_sample_t *out)
{
    fixture_t f;
    bool ok = start(&f, MOQ_VERSION_DRAFT_22, fetch, fill, 1, 64);
    if (ok) ok = CHECK(f.ledger.calls <= MAX_ALLOCATIONS);
    if (ok) {
        out->calls = f.ledger.calls;
        out->bytes = f.ledger.bytes;
        memcpy(out->sizes, f.ledger.sizes, sizeof(out->sizes));
    }
    finish(&f);
    return ok;
}

static void test_inherited_allocation(void)
{
    allocation_sample_t inherited, explicit, defaulted;
    NEED(allocation_sample(3, 0, &inherited));
    NEED(allocation_sample(3, 3, &explicit));
    NEED(inherited.calls == explicit.calls);
    CHECK(inherited.bytes == explicit.bytes);
    CHECK(memcmp(inherited.sizes, explicit.sizes, inherited.calls * sizeof(size_t)) == 0);
    NEED(allocation_sample(0, 0, &defaulted));
    NEED(allocation_sample(256, 256, &explicit));
    NEED(defaulted.calls == explicit.calls);
    CHECK(defaulted.bytes == explicit.bytes);
    CHECK(memcmp(defaulted.sizes, explicit.sizes, defaulted.calls * sizeof(size_t)) == 0);
done:
    return;
}

static void test_one_history_arena(void)
{
    allocation_sample_t base, more_fetch, more_fill, both;
    NEED(allocation_sample(2, 3, &base));
    NEED(allocation_sample(3, 3, &more_fetch));
    NEED(allocation_sample(2, 4, &more_fill));
    NEED(allocation_sample(3, 4, &both));
    NEED(base.calls == more_fetch.calls && base.calls == more_fill.calls && base.calls == both.calls);
    /* Observe exactly one allocation growing by one equal-sized slot for
     * either quota, and by two for both. No private slot type/size is assumed. */
    size_t changed = 0;
    for (size_t i = 0; i < base.calls; i++) {
        if (base.sizes[i] == more_fetch.sizes[i] && base.sizes[i] == more_fill.sizes[i] &&
            base.sizes[i] == both.sizes[i]) continue;
        changed++;
        NEED(more_fetch.sizes[i] > base.sizes[i]);
        size_t slot = more_fetch.sizes[i] - base.sizes[i];
        NEED(more_fill.sizes[i] == base.sizes[i] + slot);
        NEED(both.sizes[i] == base.sizes[i] + 2 * slot);
    }
    CHECK(changed == 1);
done:
    return;
}

static void test_create_failures(void)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 3, 1, 64));
    size_t allocations = f.ledger.calls;
    NEED(allocations > 0 && allocations < MAX_ALLOCATIONS);
    for (size_t fail_at = 1; fail_at <= allocations + 1; fail_at++) {
        ledger_t ledger = {0};
        ledger.fail_at = fail_at;
        moq_alloc_t alloc = {.ctx = &ledger, .alloc = record_alloc, .free = record_free};
        moq_sub_cfg_t cfg;
        moq_sub_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.max_tracks = cfg.max_objects = cfg.max_chunks = 1;
        cfg.max_fetches = cfg.max_fills = 2;
        cfg.max_fetch_items = 2;
        cfg.max_fill_items = 3;
        moq_subscriber_t *sub = NULL;
        moq_result_t rc = moq_sub_create(moq_simpair_client(f.pair), &alloc, &cfg, &sub);
        bool ok = fail_at <= allocations ? rc == MOQ_ERR_NOMEM && sub == NULL :
            rc == MOQ_OK && sub != NULL;
        moq_sub_destroy(sub);
        NEED(ledger.bytes == 0 && ledger.state.balance == 0);
        NEED(ok);
    }
done:
    finish(&f);
}

static bool start_fetch(fixture_t *f, moq_sub_fetch_req_t **request)
{
    moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("history")};
    moq_sub_fetch_cfg_t cfg;
    moq_sub_fetch_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ns, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.end_group = 0;
    cfg.end_object = 2; /* Exclusive: objects 0 and 1. */
    cfg.group_order = MOQ_GROUP_ORDER_ASCENDING;
    f->peer_fetch = MOQ_FETCH_INVALID;
    REQUIRE(moq_sub_fetch(f->sub, &cfg, now(f), request) == MOQ_OK);
    REQUIRE(*request != NULL);
    REQUIRE(pump(f) && collect_peer(f));
    REQUIRE(moq_fetch_is_valid(f->peer_fetch));
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 0;
    ac.end_object = 2;
    REQUIRE(moq_session_accept_fetch(server(f), f->peer_fetch, &ac, now(f)) == MOQ_OK);
    REQUIRE(pump(f));
    REQUIRE(moq_sub_tick(f->sub, now(f)) == MOQ_OK);
    return true;
}

static bool send_fetch_bytes(fixture_t *f, moq_fetch_t fetch, uint64_t object,
                             size_t length)
{
    moq_fetch_object_cfg_t cfg;
    moq_fetch_object_cfg_init(&cfg);
    cfg.object_id = object;
    REQUIRE(length <= sizeof(payload));
    REQUIRE(moq_rcbuf_create(&f->pair_alloc, payload, length, &cfg.payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_fetch_object(server(f), fetch, &cfg, now(f));
    moq_rcbuf_decref(cfg.payload);
    REQUIRE(rc == MOQ_OK);
    REQUIRE(pump(f));
    return true;
}

static bool send_fetch_object(fixture_t *f, moq_fetch_t fetch, uint64_t object)
{
    return send_fetch_bytes(f, fetch, object, sizeof(payload));
}

static void test_mixed_cancel(bool destroy_pending)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 4, 2, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, true, false, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    NEED(send_fill_object(&f, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_fetch_req_t *a = NULL, *b = NULL, *c = NULL;
    NEED(start_fetch(&f, &a));
    moq_fetch_t peer_a = f.peer_fetch;
    NEED(start_fetch(&f, &b));
    NEED(send_fetch_object(&f, peer_a, 0));
    NEED(send_fetch_object(&f, f.peer_fetch, 0));
    NEED(send_fetch_object(&f, peer_a, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fetch_items_queued == 5 && snapshot.fill_items_queued == 1 &&
        snapshot.bytes_queued == 4 * PAYLOAD_BYTES);
    if (destroy_pending) goto done;
    /* Remove this request from the head, middle and pending slot. The other
     * request and FILL family must keep both FIFO order and retained refs. */
    NEED(moq_sub_cancel_fetch(f.sub, a, now(&f)) == MOQ_OK);
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fetch_items_queued == 2 && snapshot.fill_items_queued == 1 &&
        snapshot.bytes_queued == 2 * PAYLOAD_BYTES);
    NEED(start_fetch(&f, &c));
    NEED(c == a);
    NEED(send_fetch_object(&f, f.peer_fetch, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    for (unsigned i = 0; i < 4; i++) {
        moq_sub_fetch_item_t item;
        NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
        bool ok = item.request == (i < 2 ? b : c);
        if (i % 2 == 0) ok = ok && item.kind == MOQ_SUB_FETCH_OK;
        else ok = ok && item.kind == MOQ_SUB_FETCH_OBJECT && item.u.object.object_id == 0 &&
            item.u.object.payload && moq_rcbuf_len(item.u.object.payload) == sizeof(payload) &&
            memcmp(moq_rcbuf_data(item.u.object.payload), payload, sizeof(payload)) == 0;
        moq_sub_fetch_item_cleanup(&item);
        NEED(ok);
    }
    NEED(poll_fill_object(&f, track, child, 0));
    NEED(moq_sub_cancel_fetch(f.sub, b, now(&f)) == MOQ_OK);
    NEED(moq_sub_cancel_fetch(f.sub, c, now(&f)) == MOQ_OK);
    /* Refill and empty the same small arena repeatedly, including tail-only
     * cancellation and both families' terminal metadata. */
    for (unsigned round = 0; round < 4; round++) {
        NEED(start_fetch(&f, &a));
        NEED(moq_sub_cancel_fetch(f.sub, a, now(&f)) == MOQ_OK);
        moq_sub_fetch_item_t empty;
        NEED(moq_sub_poll_fetch(f.sub, &empty) == MOQ_DONE);
    }
    NEED(moq_sub_cancel_fill(f.sub, child, now(&f)) == MOQ_OK);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_fill_item_t terminal;
    NEED(moq_sub_poll_fill(f.sub, &terminal, sizeof(terminal)) == MOQ_OK);
    bool ok = terminal.kind == MOQ_SUB_FILL_TERMINAL && terminal.track == track &&
        moq_sub_fill_eq(terminal.request, child) && terminal.u.terminal.reason == MOQ_FILL_LOCAL_CANCELLED;
    moq_sub_fill_item_cleanup(&terminal);
    NEED(ok);
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fetch_items_queued == 0 && snapshot.fill_items_queued == 0 && snapshot.bytes_queued == 0);
done:
    finish(&f);
}

static void test_peer_done(void)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, 1, 64));
    moq_sub_track_t *track = NULL, *other = NULL;
    NEED(subscribe_track(&f, false, false, &track));
    moq_subscription_t peer = f.peer_sub;
    NEED(send_live(&f, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(subscribe_track(&f, false, false, &other));
    NEED(moq_sub_unsubscribe(f.sub, other, now(&f)) == MOQ_OK);
    NEED(moq_sub_release_track(f.sub, other) == MOQ_OK); /* Another track's item cannot pin it. */
    moq_done_subscribe_cfg_t cfg;
    moq_done_subscribe_cfg_init(&cfg);
    cfg.stream_count = 1;
    NEED(moq_session_done_subscribe(server(&f), peer, &cfg, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(release_refused(&f, track));
    NEED(poll_live(&f, track, 1));
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static void test_ack_history_credit(bool fill, bool fits)
{
    fixture_t f;
    uint64_t budget = sizeof(properties) + PAYLOAD_BYTES - (fits ? 0u : 1u);
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, 1, budget));
    moq_sub_track_t *owner = NULL, *track = NULL;
    if (fill) {
        NEED(subscribe_track(&f, true, false, &owner));
        NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
        NEED(send_fill_object(&f, 0));
    } else {
        moq_sub_fetch_req_t *request = NULL;
        NEED(start_fetch(&f, &request));
        NEED(send_fetch_object(&f, f.peer_fetch, 0));
    }
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.bytes_queued == PAYLOAD_BYTES);
    NEED(request_track(&f, false, &track));
    moq_accept_subscribe_cfg_t cfg;
    moq_accept_subscribe_cfg_init(&cfg);
    cfg.track_properties = (moq_bytes_t){properties, sizeof(properties)};
    NEED(moq_session_accept_subscribe(server(&f), f.peer_sub, &cfg, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    moq_result_t expected = fits ? MOQ_OK : MOQ_ERR_BUFFER;
    NEED(moq_sub_tick(f.sub, now(&f)) == expected);
    moq_sub_track_result_t ack;
    NEED(moq_sub_get_track_result(f.sub, track, &ack, sizeof(ack)) == expected);
    if (fits) {
        NEED(ack.track_properties.data && ack.track_properties.len == sizeof(properties));
        NEED(memcmp(ack.track_properties.data, properties, sizeof(properties)) == 0);
    }
    NEED(stats(&f, &snapshot));
    NEED(snapshot.bytes_queued == PAYLOAD_BYTES); /* ACK is not a queue gauge. */
    NEED(moq_sub_unsubscribe(f.sub, track, now(&f)) == MOQ_OK);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static bool poll_metadata(fixture_t *f, bool fill, unsigned index,
                          moq_sub_track_t *track, moq_sub_fill_t child,
                          moq_sub_fetch_req_t *request, bool *present)
{
    *present = false;
    bool ok;
    if (fill) {
        moq_sub_fill_item_t item;
        moq_result_t rc = moq_sub_poll_fill(f->sub, &item, sizeof(item));
        if (rc == MOQ_DONE) return true;
        REQUIRE(rc == MOQ_OK);
        ok = item.track == track && moq_sub_fill_eq(item.request, child);
        if (index < 2)
            ok = ok && item.kind == MOQ_SUB_FILL_GAP && item.u.gap.group_id == 0 &&
                item.u.gap.object_id == index + 1 && item.u.gap.range_kind == MOQ_FETCH_RANGE_NON_EXISTENT;
        else
            ok = ok && index == 2 && item.kind == MOQ_SUB_FILL_TERMINAL &&
                item.u.terminal.reason == MOQ_FILL_COMPLETE &&
                item.u.terminal.error_domain == MOQ_FILL_ERROR_NONE && item.u.terminal.error_code == 0;
        moq_sub_fill_item_cleanup(&item);
    } else {
        moq_sub_fetch_item_t item;
        moq_result_t rc = moq_sub_poll_fetch(f->sub, &item);
        if (rc == MOQ_DONE) return true;
        REQUIRE(rc == MOQ_OK);
        ok = item.request == request;
        if (index < 2)
            ok = ok && item.kind == MOQ_SUB_FETCH_GAP && item.u.gap.group_id == 0 &&
                item.u.gap.object_id == index && item.u.gap.range_kind == MOQ_FETCH_RANGE_NON_EXISTENT;
        else
            ok = ok && index == 2 && item.kind == MOQ_SUB_FETCH_COMPLETE;
        moq_sub_fetch_item_cleanup(&item);
    }
    REQUIRE(ok);
    *present = true;
    return true;
}

static bool drain_metadata(fixture_t *f, bool fill, unsigned quota,
                           moq_sub_track_t *track, moq_sub_fill_t child,
                           moq_sub_fetch_req_t *request)
{
    unsigned seen = 0;
    for (unsigned round = 0; round < 4; round++) {
        moq_result_t rc = moq_sub_tick(f->sub, now(f));
        REQUIRE(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        moq_sub_stats_t snapshot;
        REQUIRE(stats(f, &snapshot));
        REQUIRE(snapshot.bytes_queued == 0);
        uint64_t retained = fill ? snapshot.fill_items_queued : snapshot.fetch_items_queued;
        REQUIRE(retained <= quota + 1); /* The documented one-deep pending slot. */
        unsigned count = 0;
        for (unsigned n = 0; n < 4; n++) {
            bool present;
            REQUIRE(poll_metadata(f, fill, seen, track, child, request, &present));
            if (!present) break;
            seen++;
            count++;
        }
        if (round == 0) REQUIRE(count == (quota < 3 ? quota : 3));
        REQUIRE(count <= quota);
        if (seen == 3) {
            REQUIRE(moq_sub_tick(f->sub, now(f)) == MOQ_OK);
            REQUIRE(stats(f, &snapshot));
            REQUIRE(snapshot.fill_items_queued == 0 && snapshot.fetch_items_queued == 0);
            return true;
        }
    }
    REQUIRE(false);
    return false;
}

static bool poll_opening_metadata(fixture_t *f, bool fill,
                                  moq_sub_track_t *track, moq_sub_fill_t child,
                                  moq_sub_fetch_req_t *request)
{
    bool ok;
    if (fill) {
        moq_sub_fill_item_t gap;
        REQUIRE(moq_sub_poll_fill(f->sub, &gap, sizeof(gap)) == MOQ_OK);
        ok = gap.kind == MOQ_SUB_FILL_GAP && gap.track == track &&
            moq_sub_fill_eq(gap.request, child) && gap.u.gap.group_id == 0 &&
            gap.u.gap.object_id == 0 && gap.u.gap.range_kind == MOQ_FETCH_RANGE_NON_EXISTENT;
        moq_sub_fill_item_cleanup(&gap);
    } else {
        moq_sub_fetch_item_t accepted;
        REQUIRE(moq_sub_poll_fetch(f->sub, &accepted) == MOQ_OK);
        ok = accepted.kind == MOQ_SUB_FETCH_OK && accepted.request == request;
        moq_sub_fetch_item_cleanup(&accepted);
    }
    REQUIRE(ok);
    return true;
}

static void test_metadata_quotas(uint32_t fetch_quota, uint32_t fill_override,
                                bool fill_first)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, fetch_quota, fill_override, 1, 64));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, true, false, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    moq_sub_fetch_req_t *request = NULL;
    if (!fill_first) NEED(start_fetch(&f, &request));
    NEED(moq_session_write_fill_range(server(&f), f.peer_fill,
        MOQ_FETCH_RANGE_NON_EXISTENT, 0, 0, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    if (fill_first) NEED(start_fetch(&f, &request));
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fetch_items_queued == 1 && snapshot.fill_items_queued == 1);
    /* Poll the newer family first; the other family's older item must remain. */
    NEED(poll_opening_metadata(&f, !fill_first, track, child, request));
    NEED(stats(&f, &snapshot));
    NEED(snapshot.fetch_items_queued == (fill_first ? 0u : 1u));
    NEED(snapshot.fill_items_queued == (fill_first ? 1u : 0u));
    NEED(poll_opening_metadata(&f, fill_first, track, child, request));
    for (unsigned i = 1; i <= 2; i++)
        NEED(moq_session_write_fill_range(server(&f), f.peer_fill,
            MOQ_FETCH_RANGE_NON_EXISTENT, 0, i, now(&f)) == MOQ_OK);
    NEED(moq_session_end_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(drain_metadata(&f, true, fill_override ? fill_override : fetch_quota,
                         track, child, request));
    for (unsigned i = 0; i < 2; i++)
        NEED(moq_session_write_fetch_range(server(&f), f.peer_fetch,
            MOQ_FETCH_RANGE_NON_EXISTENT, 0, i, now(&f)) == MOQ_OK);
    NEED(moq_session_end_fetch(server(&f), f.peer_fetch, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(drain_metadata(&f, false, fetch_quota, track, child, request));
    NEED(moq_sub_track_is_active(track));
    NEED(!moq_sub_fill_is_valid(moq_sub_track_fill(track)));
done:
    finish(&f);
}

static void test_retained_bytes(bool pending_control)
{
    fixture_t f;
    uint64_t budget = sizeof(properties) + (pending_control ? 2u : 1u) * PAYLOAD_BYTES;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, pending_control ? 1 : 4, budget));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, true, &track));
    moq_sub_track_result_t ack;
    NEED(moq_sub_get_track_result(f.sub, track, &ack, sizeof(ack)) == MOQ_OK);
    NEED(ack.track_properties.len == sizeof(properties) && ack.track_properties.data != NULL);
    NEED(memcmp(ack.track_properties.data, properties, sizeof(properties)) == 0);
    const uint8_t *snapshot_data = ack.track_properties.data;
    moq_sub_stats_t snapshot;
    NEED(stats(&f, &snapshot));
    NEED(snapshot.bytes_queued == 0); /* ACK bytes never become queued bytes. */
    NEED(send_live(&f, 1) && send_live(&f, 2));
    moq_result_t rc = moq_sub_tick(f.sub, now(&f));
    NEED(rc == MOQ_ERR_WOULD_BLOCK);
    NEED(stats(&f, &snapshot));
    NEED(snapshot.bytes_queued + sizeof(properties) <= budget);
    if (pending_control)
        NEED(snapshot.objects_queued == 2 && snapshot.bytes_queued == 2 * PAYLOAD_BYTES);
    for (uint64_t group = 1; group <= 2; group++) {
        NEED(poll_live(&f, track, group));
        NEED(stats(&f, &snapshot));
        NEED(snapshot.bytes_queued + sizeof(properties) <= budget);
        rc = moq_sub_tick(f.sub, now(&f));
        NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        NEED(moq_sub_get_track_result(f.sub, track, &ack, sizeof(ack)) == MOQ_OK);
        NEED(ack.track_properties.data == snapshot_data && ack.track_properties.len == sizeof(properties));
        NEED(memcmp(ack.track_properties.data, properties, sizeof(properties)) == 0);
    }
    NEED(stats(&f, &snapshot));
    NEED(snapshot.bytes_queued == 0 && snapshot.objects_queued == 0);
    NEED(moq_sub_unsubscribe(f.sub, track, now(&f)) == MOQ_OK);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
done:
    finish(&f);
}

static bool no_local_failure(fixture_t *f)
{
    moq_sub_local_failure_t out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    REQUIRE(moq_sub_poll_local_failure(f->sub, &out, sizeof(out)) == MOQ_DONE);
    REQUIRE(memcmp(&out, &before, sizeof(out)) == 0);
    return true;
}

static bool local_failure(fixture_t *f, uint32_t kind, moq_sub_track_t *track,
                          moq_sub_fetch_req_t *fetch, moq_sub_fill_t fill,
                          uint64_t initiating_request_id)
{
    struct { moq_sub_local_failure_t value; uint8_t guard[16]; } out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    const size_t short_sizes[] = {0, 4, MOQ_SUB_LOCAL_FAILURE_V0_SIZE - 1};
    size_t calls = f->ledger.calls;
    REQUIRE(moq_sub_poll_local_failure(NULL, &out.value, sizeof(out)) == MOQ_ERR_INVAL);
    REQUIRE(moq_sub_poll_local_failure(f->sub, NULL, sizeof(out)) == MOQ_ERR_INVAL);
    for (size_t i = 0; i < sizeof(short_sizes) / sizeof(short_sizes[0]); i++) {
        REQUIRE(moq_sub_poll_local_failure(f->sub, &out.value, short_sizes[i]) == MOQ_ERR_INVAL);
        REQUIRE(memcmp(&out, &before, sizeof(out)) == 0);
    }
    REQUIRE(moq_sub_poll_local_failure(f->sub, &out.value, sizeof(out)) == MOQ_OK);
    REQUIRE(out.value.struct_size == sizeof(out.value));
    REQUIRE(out.value.owner_kind == kind && out.value.local_result == MOQ_ERR_BUFFER);
    REQUIRE(out.value.reserved == 0 && out.value.track == track && out.value.fetch == fetch);
    REQUIRE(moq_sub_fill_eq(out.value.fill, fill));
    REQUIRE(out.value.initiating_request_id == initiating_request_id);
    REQUIRE(memcmp(out.guard, before.guard, sizeof(out.guard)) == 0);
    REQUIRE(f->ledger.calls == calls);
    return no_local_failure(f);
}

static bool poll_small_live(fixture_t *f, moq_sub_track_t *track, uint64_t group)
{
    moq_sub_object_t item;
    REQUIRE(moq_sub_poll_object(f->sub, &item) == MOQ_OK);
    bool ok = item.track == track && item.group_id == group && item.payload &&
        moq_rcbuf_len(item.payload) == 2 && memcmp(moq_rcbuf_data(item.payload), payload, 2) == 0;
    moq_sub_object_cleanup(&item);
    REQUIRE(ok);
    return true;
}

static void test_live_capacity_failure(bool close_before_notice, bool polled_tail)
{
    fixture_t f;
    moq_sub_object_t held = {0};
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, 4, sizeof(properties) + 3));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, true, &track));
    moq_sub_track_result_t ack;
    NEED(moq_sub_get_track_result(f.sub, track, &ack, sizeof(ack)) == MOQ_OK);
    const uint8_t *ack_bytes = ack.track_properties.data;
    NEED(send_live_bytes(&f, 1, 2));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    if (polled_tail) NEED(moq_sub_poll_object(f.sub, &held) == MOQ_OK);
    NEED(send_live(&f, 2));
    size_t calls = f.ledger.calls;
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_BUFFER);
    NEED(f.ledger.calls == calls);
    NEED(moq_sub_track_get_state(track) == MOQ_SUB_TRACK_DONE);
    NEED(release_refused(&f, track));
    NEED(moq_sub_get_track_result(f.sub, track, &ack, sizeof(ack)) == MOQ_OK);
    NEED(ack.track_properties.data == ack_bytes && ack.track_properties.len == sizeof(properties));
    NEED(memcmp(ack.track_properties.data, properties, sizeof(properties)) == 0);
    moq_request_error_t peer_error = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
    NEED(!moq_sub_track_get_error(track, &peer_error));
    NEED(peer_error == MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    if (close_before_notice)
        NEED(moq_session_on_transport_close(moq_simpair_client(f.pair), 0, now(&f)) == MOQ_OK);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(release_refused(&f, track)); /* Native cancellation alone cannot erase the notice. */
    NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_TRACK, track, NULL,
                       (moq_sub_fill_t){0}, 0));
    if (!polled_tail) {
        NEED(release_refused(&f, track));
        NEED(poll_small_live(&f, track, 1));
    } else {
        NEED(held.track == track && held.payload && moq_rcbuf_len(held.payload) == 2);
        NEED(memcmp(moq_rcbuf_data(held.payload), payload, 2) == 0);
    }
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
    if (!close_before_notice) {
        NEED(moq_session_state(moq_simpair_client(f.pair)) == MOQ_SESS_ESTABLISHED);
        NEED(subscribe_track(&f, false, false, &track));
        NEED(send_live(&f, 3));
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
        NEED(poll_live(&f, track, 3));
        NEED(no_local_failure(&f));
    }
done:
    moq_sub_object_cleanup(&held);
    finish(&f);
}

static void test_fill_capacity_failure(bool terminal_first, bool peer_complete)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, 2, sizeof(properties) + 3));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, true, true, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    NEED(send_fill_object(&f, 0));
    if (peer_complete) {
        NEED(moq_session_end_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
        NEED(pump(&f));
    }
    size_t calls = f.ledger.calls;
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_BUFFER);
    NEED(f.ledger.calls == calls && moq_sub_track_is_active(track));
    if (!terminal_first)
        NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_FILL, track, NULL, child,
                           f.fill_initiating_request_id));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_fill_item_t terminal;
    NEED(moq_sub_poll_fill(f.sub, &terminal, sizeof(terminal)) == MOQ_OK);
    bool ok = terminal.kind == MOQ_SUB_FILL_TERMINAL && terminal.track == track &&
        moq_sub_fill_eq(terminal.request, child) && terminal.u.terminal.reason ==
        (peer_complete ? MOQ_FILL_COMPLETE : MOQ_FILL_LOCAL_CANCELLED);
    moq_sub_fill_item_cleanup(&terminal);
    NEED(ok);
    if (terminal_first) {
        NEED(moq_sub_fill_eq(moq_sub_track_fill(track), child));
        NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_FILL, track, NULL, child,
                           f.fill_initiating_request_id));
    }
    NEED(!moq_sub_fill_is_valid(moq_sub_track_fill(track)));
    NEED(moq_sub_track_is_active(track));
    NEED(send_live_bytes(&f, 1, 2));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll_small_live(&f, track, 1));
    NEED(moq_session_state(moq_simpair_client(f.pair)) == MOQ_SESS_ESTABLISHED);
    NEED(no_local_failure(&f));
done:
    finish(&f);
}

static void test_fetch_capacity_failure(bool peer_complete)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 4, 2, 2, 3));
    moq_sub_fetch_req_t *request = NULL;
    NEED(start_fetch(&f, &request));
    NEED(send_fetch_bytes(&f, f.peer_fetch, 0, 2));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(send_fetch_object(&f, f.peer_fetch, 1));
    if (peer_complete) {
        NEED(moq_session_end_fetch(server(&f), f.peer_fetch, now(&f)) == MOQ_OK);
        NEED(pump(&f));
    }
    size_t calls = f.ledger.calls;
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_BUFFER);
    NEED(f.ledger.calls == calls);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    moq_sub_fetch_req_t *other = NULL;
    NEED(start_fetch(&f, &other));
    NEED(other != request);
    NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_FETCH, NULL, request,
                       (moq_sub_fill_t){0}, 0));
    moq_sub_fetch_item_t item;
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
    bool ok = item.request == request && item.kind == MOQ_SUB_FETCH_OK;
    moq_sub_fetch_item_cleanup(&item);
    NEED(ok);
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
    ok = item.request == request && item.kind == MOQ_SUB_FETCH_OBJECT &&
        item.u.object.object_id == 0 && item.u.object.payload &&
        moq_rcbuf_len(item.u.object.payload) == 2 &&
        memcmp(moq_rcbuf_data(item.u.object.payload), payload, 2) == 0;
    moq_sub_fetch_item_cleanup(&item);
    NEED(ok);
    if (peer_complete) {
        NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
        ok = item.request == request && item.kind == MOQ_SUB_FETCH_COMPLETE;
        moq_sub_fetch_item_cleanup(&item);
        NEED(ok);
    }
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
    ok = item.request == other && item.kind == MOQ_SUB_FETCH_OK;
    moq_sub_fetch_item_cleanup(&item);
    NEED(ok);
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_DONE); /* No invented terminal on local cancel. */
    moq_sub_fetch_req_t *reused = NULL;
    NEED(start_fetch(&f, &reused));
    NEED(reused == request && reused != other);
    NEED(no_local_failure(&f));
    NEED(moq_session_state(moq_simpair_client(f.pair)) == MOQ_SESS_ESTABLISHED);
done:
    finish(&f);
}

static void test_chunk_capacity_failure(void)
{
    fixture_t f;
    NEED(start_ex(&f, MOQ_VERSION_DRAFT_22, 2, 2, 2, sizeof(properties) + 3, true));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, true, &track));
    NEED(send_live(&f, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_BUFFER);
    NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_TRACK, track, NULL,
                       (moq_sub_fill_t){0}, 0));
    NEED(release_refused(&f, track));
    moq_sub_chunk_t item;
    NEED(moq_sub_poll_chunk(f.sub, &item) == MOQ_OK);
    bool ok = item.track == track && item.begin && !item.end && !item.chunk;
    moq_sub_chunk_cleanup(&item);
    NEED(ok); /* Zero-byte BEGIN survives the later oversized retained chunk. */
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_poll_chunk(f.sub, &item) == MOQ_DONE);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
    NEED(moq_session_state(moq_simpair_client(f.pair)) == MOQ_SESS_ESTABLISHED);
done:
    finish(&f);
}

static void test_cancel_action_pressure(void)
{
    fixture_t f;
    NEED(start_limits(&f, MOQ_VERSION_DRAFT_22, 2, 2, 2, sizeof(properties) + 3, false, 4));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, true, &track));
    NEED(send_live(&f, 1));
    moq_session_t *client = moq_simpair_client(f.pair);
    moq_result_t rc = MOQ_OK;
    unsigned queued;
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("pressure")};
    cfg.track_namespace = (moq_namespace_t){ns, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    for (queued = 0; queued < 8; queued++) {
        moq_subscription_t ignored;
        rc = moq_session_subscribe(client, &cfg, now(&f), &ignored);
        if (rc == MOQ_ERR_WOULD_BLOCK) break;
        NEED(rc == MOQ_OK);
    }
    NEED(queued > 0 && queued < 8 && rc == MOQ_ERR_WOULD_BLOCK);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_BUFFER);
    NEED(local_failure(&f, MOQ_SUB_LOCAL_FAILURE_TRACK, track, NULL,
                       (moq_sub_fill_t){0}, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(release_refused(&f, track)); /* Notice consumed, but real cancellation cannot queue. */
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_release_track(f.sub, track) == MOQ_OK);
    NEED(no_local_failure(&f));
done:
    finish(&f);
}

static void test_mixed_capacity_recovery(bool streaming)
{
    fixture_t f;
    NEED(start_ex(&f, MOQ_VERSION_DRAFT_22, 2, 2, 2,
                  sizeof(properties) + PAYLOAD_BYTES, streaming));
    moq_sub_track_t *track = NULL;
    moq_sub_fetch_req_t *fetch = NULL;
    NEED(subscribe_track(&f, true, true, &track));
    moq_sub_fill_t child = moq_sub_track_fill(track);
    NEED(start_fetch(&f, &fetch));
    moq_sub_fetch_item_t item;
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
    bool ok = item.kind == MOQ_SUB_FETCH_OK && item.request == fetch;
    moq_sub_fetch_item_cleanup(&item);
    NEED(ok);
    NEED(moq_session_open_fill(server(&f), f.peer_fill, now(&f)) == MOQ_OK);
    NEED(send_fetch_object(&f, f.peer_fetch, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(send_fill_object(&f, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_sub_stats_t before, after;
    NEED(stats(&f, &before));
    NEED(before.bytes_queued == PAYLOAD_BYTES && before.fetch_items_queued == 1 &&
         before.fill_items_queued == 0);
    for (unsigned i = 0; i < 3; i++) {
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
        NEED(stats(&f, &after));
        NEED(after.bytes_queued == before.bytes_queued &&
             after.fetch_items_queued == before.fetch_items_queued &&
             after.fill_items_queued == before.fill_items_queued);
        NEED(no_local_failure(&f));
    }
    NEED(moq_sub_poll_fetch(f.sub, &item) == MOQ_OK);
    ok = item.kind == MOQ_SUB_FETCH_OBJECT && item.request == fetch &&
        item.u.object.payload && moq_rcbuf_len(item.u.object.payload) == PAYLOAD_BYTES;
    moq_sub_fetch_item_cleanup(&item);
    NEED(ok);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK); /* No input, timer, or pump. */
    NEED(stats(&f, &after));
    NEED(after.bytes_queued == PAYLOAD_BYTES && after.fill_items_queued == 1 &&
         after.fetch_items_queued == 0);
    NEED(send_live(&f, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    NEED(stats(&f, &after));
    NEED(after.bytes_queued == PAYLOAD_BYTES && after.objects_queued == 0);
    NEED(poll_fill_object(&f, track, child, 0));
    if (streaming) {
        moq_sub_chunk_t chunk;
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
        NEED(stats(&f, &after));
        NEED(after.bytes_queued == PAYLOAD_BYTES && after.chunks_queued == 2);
        NEED(moq_sub_poll_chunk(f.sub, &chunk) == MOQ_OK);
        ok = chunk.track == track && chunk.begin && !chunk.end && !chunk.chunk;
        moq_sub_chunk_cleanup(&chunk);
        NEED(ok);
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
        NEED(moq_sub_poll_chunk(f.sub, &chunk) == MOQ_OK);
        ok = chunk.track == track && chunk.end && chunk.chunk &&
            moq_rcbuf_len(chunk.chunk) == PAYLOAD_BYTES;
        moq_sub_chunk_cleanup(&chunk);
        NEED(ok);
    } else {
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
        NEED(poll_live(&f, track, 1));
    }
    NEED(stats(&f, &after));
    NEED(after.bytes_queued == 0);
    NEED(no_local_failure(&f));
done:
    finish(&f);
}

#ifdef MOQ_TEST_ADMISSION_INTERNALS
static void test_leave_transaction(void)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, 2, 2, 4, sizeof(properties) + PAYLOAD_BYTES));
    moq_sub_track_t *track = NULL;
    NEED(subscribe_track(&f, false, true, &track));
    NEED(send_live(&f, 1) && send_live(&f, 2));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_session_t *client = moq_simpair_client(f.pair);
    NEED(client->event_head < client->event_tail);
    moq_event_t head;
    memcpy(&head, &client->events[client->event_head % client->event_cap], sizeof(head));
    NEED(head.kind == MOQ_EVENT_OBJECT_RECEIVED && head.u.object_received.group_id == 2);
    size_t event_head = client->event_head, event_tail = client->event_tail;
    uint64_t progress = client->event_progress_token;
    size_t receive_bytes = client->recv_payload_bytes;
    int64_t balance = f.pair_state.balance;
    NEED(receive_bytes == PAYLOAD_BYTES);
    for (unsigned i = 0; i < 3; i++) {
        NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
        NEED(client->event_head == event_head && client->event_tail == event_tail);
        NEED(client->event_progress_token == progress && client->recv_payload_bytes == receive_bytes);
        NEED(memcmp(&head, &client->events[event_head % client->event_cap], sizeof(head)) == 0);
        NEED(f.pair_state.balance == balance);
    }
    NEED(poll_live(&f, track, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(client->event_head > event_head && client->event_progress_token > progress);
    NEED(client->recv_payload_bytes == 0);
    NEED(poll_live(&f, track, 2));
done:
    finish(&f);
}
#endif

static void capacity_live(void) { test_live_capacity_failure(false, false); }
static void capacity_polled(void) { test_live_capacity_failure(false, true); }
static void capacity_closed(void) { test_live_capacity_failure(true, false); }
static void capacity_fill_notice_first(void) { test_fill_capacity_failure(false, false); }
static void capacity_fill_terminal_first(void) { test_fill_capacity_failure(true, false); }
static void capacity_fill_peer_terminal(void) { test_fill_capacity_failure(true, true); }
static void capacity_fetch_cancel(void) { test_fetch_capacity_failure(false); }
static void capacity_fetch_peer_terminal(void) { test_fetch_capacity_failure(true); }
static void capacity_mixed_objects(void) { test_mixed_capacity_recovery(false); }
static void capacity_mixed_chunks(void) { test_mixed_capacity_recovery(true); }

static void live_queued(void) { test_live_retirement(MOQ_VERSION_DRAFT_22, false); }
static void live_pending(void) { test_live_retirement(MOQ_VERSION_DRAFT_22, true); }
static void legacy16_queued(void) { test_live_retirement(MOQ_VERSION_DRAFT_16, false); }
static void legacy16_pending(void) { test_live_retirement(MOQ_VERSION_DRAFT_16, true); }
static void legacy18_queued(void) { test_live_retirement(MOQ_VERSION_DRAFT_18, false); }
static void legacy18_pending(void) { test_live_retirement(MOQ_VERSION_DRAFT_18, true); }
static void child_only(void) { test_fill_retirement(0); }
static void fill_queued(void) { test_fill_retirement(1); }
static void fill_pending(void) { test_fill_retirement(2); }
static void inherited_quota(void) { test_metadata_quotas(1, 0, false); }
static void small_fill_quota(void) { test_metadata_quotas(3, 1, true); }
static void small_fetch_quota(void) { test_metadata_quotas(1, 3, false); }
static void pending_bytes(void) { test_retained_bytes(true); }
static void ack_credit(void) { test_retained_bytes(false); }
static void chunk_queued(void) { test_chunk_retirement(MOQ_VERSION_DRAFT_22, false); }
static void chunk_pending(void) { test_chunk_retirement(MOQ_VERSION_DRAFT_22, true); }
static void legacy16_chunk(void) { test_chunk_retirement(MOQ_VERSION_DRAFT_16, false); }
static void legacy18_chunk(void) { test_chunk_retirement(MOQ_VERSION_DRAFT_18, true); }
static void close_child(void) { test_closed_child(0, false); }
static void close_queued(void) { test_closed_child(1, false); }
static void close_pending(void) { test_closed_child(2, false); }
static void close_terminal(void) { test_closed_child(0, true); }
static void mixed_cancel(void) { test_mixed_cancel(false); }
static void mixed_destroy(void) { test_mixed_cancel(true); }
static void ack_fetch_fits(void) { test_ack_history_credit(false, true); }
static void ack_fetch_full(void) { test_ack_history_credit(false, false); }
static void ack_fill_fits(void) { test_ack_history_credit(true, true); }
static void ack_fill_full(void) { test_ack_history_credit(true, false); }

int main(int argc, char **argv)
{
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"d22_live_queued_retirement", live_queued},
        {"d22_live_pending_retirement", live_pending},
        {"d22_initial_child_retirement", child_only},
        {"d22_fill_queued_retirement", fill_queued},
        {"d22_fill_pending_retirement", fill_pending},
        {"d16_queued_release_control", legacy16_queued},
        {"d16_pending_release_control", legacy16_pending},
        {"d18_queued_release_control", legacy18_queued},
        {"d18_pending_release_control", legacy18_pending},
        {"configured_fill_default_allocation", test_inherited_allocation},
        {"one_tagged_history_arena", test_one_history_arena},
        {"configured_fill_default_delivery", inherited_quota},
        {"explicit_small_fill_quota", small_fill_quota},
        {"explicit_small_fetch_quota", small_fetch_quota},
        {"queued_pending_ack_gauge_control", pending_bytes},
        {"ack_debits_live_admission", ack_credit},
        {"d22_chunk_queued_retirement", chunk_queued},
        {"d22_chunk_pending_retirement", chunk_pending},
        {"d16_chunk_release_control", legacy16_chunk},
        {"d18_chunk_release_control", legacy18_chunk},
        {"closed_child_retirement", close_child},
        {"closed_queued_fill_retirement", close_queued},
        {"closed_pending_fill_retirement", close_pending},
        {"closed_preserves_queued_terminal", close_terminal},
        {"rejected_parent_retirement", test_rejected_parent},
        {"history_allocation_failure_cleanup", test_create_failures},
        {"mixed_family_cancel_and_reuse", mixed_cancel},
        {"mixed_family_pending_destroy", mixed_destroy},
        {"peer_done_and_other_track_release", test_peer_done},
        {"ack_counts_fetch_arena_fits", ack_fetch_fits},
        {"ack_counts_fetch_arena_full", ack_fetch_full},
        {"ack_counts_fill_arena_fits", ack_fill_fits},
        {"ack_counts_fill_arena_full", ack_fill_full},
        {"capacity_live_preserves_queued_and_ack", capacity_live},
        {"capacity_live_preserves_polled", capacity_polled},
        {"capacity_close_preserves_notice_and_tail", capacity_closed},
        {"capacity_fill_notice_before_terminal", capacity_fill_notice_first},
        {"capacity_fill_terminal_before_notice", capacity_fill_terminal_first},
        {"capacity_fill_preserves_peer_terminal", capacity_fill_peer_terminal},
        {"capacity_fetch_cancel_preserves_tail_and_reuse", capacity_fetch_cancel},
        {"capacity_fetch_preserves_peer_terminal", capacity_fetch_peer_terminal},
        {"capacity_chunk_preserves_begin", test_chunk_capacity_failure},
        {"capacity_cancel_retries_real_action_pressure", test_cancel_action_pressure},
        {"capacity_mixed_fetch_fill_live_poll_recovery", capacity_mixed_objects},
        {"capacity_mixed_fetch_fill_chunk_poll_recovery", capacity_mixed_chunks},
#ifdef MOQ_TEST_ADMISSION_INTERNALS
        {"capacity_leave_preserves_core_ownership_credit_progress", test_leave_transaction},
#endif
    };
    unsigned ran = 0, passed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc == 2 && strcmp(argv[1], cases[i].name) != 0) continue;
        int before = failures;
        cases[i].run();
        bool ok = failures == before;
        printf("%s: %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        ran++;
        if (ok) passed++;
    }
    if (argc > 2 || ran == 0) {
        fprintf(stderr, "usage: %s [case-name]\n", argv[0]);
        return 2;
    }
    printf("%u/%u cases passed; %d failed assertions\n", passed, ran, failures);
    return failures ? 1 : 0;
}
