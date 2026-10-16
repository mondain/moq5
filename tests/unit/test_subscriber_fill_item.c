/* Sized FILL observation through public facade/session/simpair APIs only. */
#include <moq/subscriber.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>

static int failures;
static bool check(bool ok, const char *expr, int line)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, line, expr);
        failures++;
    }
    return ok;
}
#define CHECK(x) check((x), #x, __LINE__)
#define REQUIRE(x) do { if (!CHECK(x)) return false; } while (0)
#define NEED(x) do { if (!CHECK(x)) goto done; } while (0)

static const uint64_t group = (UINT64_C(1) << 40) + 7;
static const uint64_t object = (UINT64_C(1) << 36) + 2;
static const uint64_t subgroup = (UINT64_C(1) << 42) + 9;
static const uint8_t body[] = {0, 0x42, 0xff};
static const uint8_t props[] = {2, 0x41};
enum { MAX_CHILDREN = 4, MAX_EVENTS = 64 };

typedef struct fixture {
    test_alloc_state_t state;
    moq_alloc_t alloc;
    moq_simpair_t *pair;
    moq_subscriber_t *sub;
    moq_sub_track_t *track;
    moq_subscription_t peer_sub;
    moq_fill_t peer_fill[MAX_CHILDREN];
    uint64_t initiator[MAX_CHILDREN];
    moq_sub_fill_t child[MAX_CHILDREN];
    unsigned peer_count;
} fixture_t;

static uint64_t now(fixture_t *f) { return moq_simpair_now_us(f->pair); }
static moq_session_t *server(fixture_t *f) { return moq_simpair_server(f->pair); }
static bool pump(fixture_t *f)
{
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 64, NULL) == MOQ_OK);
    return true;
}

static bool collect(fixture_t *f)
{
    for (unsigned i = 0; i < MAX_EVENTS; i++) {
        moq_event_t ev;
        if (moq_session_poll_events(server(f), &ev, 1) == 0) return true;
        moq_result_t rc = MOQ_OK;
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            f->peer_sub = ev.u.subscribe_request.sub;
            moq_accept_subscribe_cfg_t ac;
            moq_accept_subscribe_cfg_init(&ac);
            ac.has_largest = true;
            ac.largest_group = group + 1;
            ac.largest_object = 0;
            rc = moq_session_accept_subscribe(server(f), f->peer_sub, &ac, now(f));
        } else if (ev.kind == MOQ_EVENT_FILL_REQUEST) {
            if (f->peer_count >= MAX_CHILDREN) rc = MOQ_ERR_LIMIT;
            else {
                unsigned n = f->peer_count++;
                f->peer_fill[n] = ev.u.fill_request.fill;
                f->initiator[n] = ev.u.fill_request.initiating_request_id;
            }
        }
        moq_event_cleanup(&ev);
        REQUIRE(rc == MOQ_OK);
    }
    REQUIRE(false);
    return false;
}

static bool start(fixture_t *f, uint32_t fill_items)
{
    memset(f, 0, sizeof(*f));
    f->alloc = test_allocator(&f->state);
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = &f->alloc;
    pc.version = MOQ_VERSION_DRAFT_22;
    pc.seed = 7;
    pc.initial_now_us = 1000;
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
    moq_sub_cfg_t cfg;
    moq_sub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.max_tracks = 1;
    cfg.max_fills = MAX_CHILDREN;
    cfg.max_fill_items = fill_items;
    cfg.max_fetches = 1;
    cfg.max_fetch_items = 2;
    cfg.max_queued_object_bytes = 1024;
    REQUIRE(moq_sub_create(moq_simpair_client(f->pair), &f->alloc, &cfg, &f->sub) == MOQ_OK);
    return true;
}

static void destroy(fixture_t *f)
{
    if (f->sub) moq_sub_destroy(f->sub);
    if (f->pair) moq_simpair_destroy(f->pair);
    f->sub = NULL;
    f->pair = NULL;
}

static void finish(fixture_t *f)
{
    destroy(f);
    CHECK(f->state.balance == 0);
}

static bool request(fixture_t *f, unsigned n)
{
    REQUIRE(n < MAX_CHILDREN && n == f->peer_count);
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = group;
    loc.start_object = object;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;
    if (n == 0) {
        moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("sized-fill")};
        moq_sub_track_cfg_t cfg;
        moq_sub_track_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = (moq_namespace_t){ns, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.fill = &fill;
        REQUIRE(moq_sub_subscribe(f->sub, &cfg, now(f), &f->track) == MOQ_OK);
        REQUIRE(f->track != NULL);
    } else {
        moq_sub_update_cfg_t cfg;
        moq_sub_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.fill = &fill;
        REQUIRE(moq_sub_update_subscription(f->sub, f->track, &cfg, now(f)) == MOQ_OK);
    }
    f->child[n] = moq_sub_track_fill(f->track);
    REQUIRE(moq_sub_fill_is_valid(f->child[n]));
    REQUIRE(pump(f) && collect(f) && pump(f) && collect(f));
    REQUIRE(f->peer_count == n + 1);
    REQUIRE(moq_fill_is_valid(f->peer_fill[n]));
    REQUIRE(f->initiator[n] == 2 * n);
    return true;
}

static bool write_object(fixture_t *f, unsigned child, uint64_t id, bool datagram)
{
    moq_fetch_object_cfg_t cfg;
    moq_fetch_object_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.subgroup_id = subgroup;
    cfg.object_id = id;
    cfg.publisher_priority = 213;
    cfg.datagram = datagram;
    REQUIRE(moq_rcbuf_create(&f->alloc, body, sizeof(body), &cfg.payload) == MOQ_OK);
    moq_result_t rc = moq_rcbuf_create(&f->alloc, props, sizeof(props), &cfg.properties);
    if (rc == MOQ_OK)
        rc = moq_session_write_fill_object(server(f), f->peer_fill[child], &cfg, now(f));
    moq_rcbuf_decref(cfg.payload);
    moq_rcbuf_decref(cfg.properties);
    REQUIRE(rc == MOQ_OK);
    REQUIRE(pump(f));
    return true;
}

static bool common(const fixture_t *f, const moq_sub_fill_item_t *it,
                   unsigned child, uint32_t kind)
{
    REQUIRE(it->struct_size == sizeof(*it));
    REQUIRE(it->kind == kind && it->track == f->track);
    REQUIRE(moq_sub_fill_eq(it->request, f->child[child]));
    REQUIRE(it->initiating_request_id == f->initiator[child]);
    return true;
}

static bool object_fields(const moq_sub_fill_item_t *it, uint64_t id, bool datagram)
{
    REQUIRE(it->u.object.group_id == group && it->u.object.object_id == id);
    REQUIRE(it->u.object.subgroup_id == (datagram ? 0 : subgroup));
    REQUIRE(it->u.object.datagram == datagram && it->u.object.publisher_priority == 213);
    REQUIRE(it->u.object.payload && moq_rcbuf_len(it->u.object.payload) == sizeof(body));
    REQUIRE(it->u.object.properties && moq_rcbuf_len(it->u.object.properties) == sizeof(props));
    REQUIRE(memcmp(moq_rcbuf_data(it->u.object.payload), body, sizeof(body)) == 0);
    REQUIRE(memcmp(moq_rcbuf_data(it->u.object.properties), props, sizeof(props)) == 0);
    return true;
}

typedef struct future_item {
    moq_sub_fill_item_t item;
    uint8_t tail[17];
} future_item_t;

static bool refusals(fixture_t *f)
{
    future_item_t output, before;
    moq_sub_stats_t a, b;
    REQUIRE(moq_sub_get_stats(f->sub, &a, sizeof(a)) == MOQ_OK);
    moq_sub_fill_t latest = moq_sub_track_fill(f->track);
    const size_t sizes[] = {0, 4, 7, offsetof(moq_sub_fill_item_t, u.object.payload),
        offsetof(moq_sub_fill_item_t, u.object.properties), MOQ_SUB_FILL_ITEM_V0_SIZE - 1};
    memset(&output, 0xa5, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        REQUIRE(moq_sub_poll_fill(f->sub, &output.item, sizes[i]) == MOQ_ERR_INVAL);
        REQUIRE(memcmp(&output, &before, sizeof(output)) == 0);
    }
    REQUIRE(moq_sub_poll_fill(NULL, &output.item, sizeof(output)) == MOQ_ERR_INVAL);
    REQUIRE(memcmp(&output, &before, sizeof(output)) == 0);
    REQUIRE(moq_sub_poll_fill(f->sub, NULL, sizeof(output)) == MOQ_ERR_INVAL);
    REQUIRE(moq_sub_get_stats(f->sub, &b, sizeof(b)) == MOQ_OK);
    REQUIRE(a.fill_items_queued == b.fill_items_queued && a.fill_items_polled == b.fill_items_polled);
    REQUIRE(a.bytes_queued == b.bytes_queued);
    REQUIRE(moq_sub_fill_eq(latest, moq_sub_track_fill(f->track)));
    return true;
}

static bool poll(fixture_t *f, moq_sub_fill_item_t *out, unsigned child, uint32_t kind,
                 size_t size)
{
    REQUIRE(refusals(f));
    REQUIRE(moq_sub_poll_fill(f->sub, out, size) == MOQ_OK);
    REQUIRE(common(f, out, child, kind));
    return true;
}

static void all_kinds_and_owned_lifetime(void)
{
    fixture_t f;
    moq_sub_fill_item_t held = {0}, it = {0};
    future_item_t future = {0};
    NEED(start(&f, 8));
    NEED(request(&f, 0));
    NEED(moq_session_open_fill(server(&f), f.peer_fill[0], now(&f)) == MOQ_OK);
    NEED(write_object(&f, 0, object, false));
    NEED(write_object(&f, 0, object + 1, true));
    NEED(moq_session_write_fill_range(server(&f), f.peer_fill[0],
        MOQ_FETCH_RANGE_NON_EXISTENT, group, object + 2, now(&f)) == MOQ_OK);
    NEED(moq_session_end_fill(server(&f), f.peer_fill[0], now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &held, 0, MOQ_SUB_FILL_OBJECT, MOQ_SUB_FILL_ITEM_V0_SIZE));
    NEED(object_fields(&held, object, false));
    uint32_t payload_refs = moq_rcbuf_refcount(held.u.object.payload);
    uint32_t property_refs = moq_rcbuf_refcount(held.u.object.properties);
    NEED(payload_refs > 0 && property_refs > 0);
    NEED(refusals(&f));
    NEED(moq_rcbuf_refcount(held.u.object.payload) == payload_refs);
    NEED(moq_rcbuf_refcount(held.u.object.properties) == property_refs);
    memset(&future, 0xa5, sizeof(future));
    /* Clear the record so an early failure never cleans sentinel pointers. */
    memset(&future.item, 0, sizeof(future.item));
    NEED(poll(&f, &future.item, 0, MOQ_SUB_FILL_OBJECT, sizeof(future)));
    NEED(object_fields(&future.item, object + 1, true));
    for (size_t i = sizeof(future.item); i < sizeof(future); i++)
        NEED(((const uint8_t *)&future)[i] == 0xa5);
    moq_sub_fill_item_cleanup(&future.item);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_GAP, sizeof(it)));
    NEED(it.u.gap.range_kind == MOQ_FETCH_RANGE_NON_EXISTENT &&
         it.u.gap.group_id == group && it.u.gap.object_id == object + 2);
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_TERMINAL, sizeof(it)));
    NEED(it.u.terminal.reason == MOQ_FILL_COMPLETE &&
         it.u.terminal.error_domain == MOQ_FILL_ERROR_NONE && it.u.terminal.error_code == 0);
    NEED(moq_sub_cancel_fill(f.sub, f.child[0], now(&f)) == MOQ_ERR_STALE_HANDLE);
    moq_sub_fill_item_cleanup(&it);
    future_item_t empty, before;
    memset(&empty, 0xa5, sizeof(empty));
    memcpy(&before, &empty, sizeof(before));
    NEED(moq_sub_poll_fill(f.sub, &empty.item, sizeof(empty)) == MOQ_DONE);
    NEED(memcmp(&empty, &before, sizeof(empty)) == 0);
    moq_sub_stats_t stats;
    NEED(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    NEED(stats.fill_objects == 2 && stats.fill_gaps == 1 && stats.fill_terminals == 1 &&
         stats.fill_items_polled == 5 && stats.fill_items_queued == 0 && stats.bytes_queued == 0);
    NEED(moq_sub_unsubscribe(f.sub, f.track, now(&f)) == MOQ_OK);
    NEED(moq_sub_release_track(f.sub, f.track) == MOQ_OK);
    destroy(&f);
    NEED(object_fields(&held, object, false));
done:
    moq_sub_fill_item_cleanup(&held);
    moq_sub_fill_item_cleanup(&held);
    moq_sub_fill_item_cleanup(&it);
    moq_sub_fill_item_cleanup(&future.item);
    finish(&f);
}

static void multiple_children_and_cancel(void)
{
    fixture_t f;
    moq_sub_fill_item_t it = {0};
    NEED(start(&f, 16));
    NEED(request(&f, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_track_is_active(f.track));
    NEED(request(&f, 1));
    NEED(!moq_sub_fill_eq(f.child[0], f.child[1]));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_fill_eq(moq_sub_track_fill(f.track), f.child[1]));
    for (unsigned n = 0; n < 2; n++) {
        NEED(moq_session_open_fill(server(&f), f.peer_fill[n], now(&f)) == MOQ_OK);
        NEED(write_object(&f, n, object, false));
    }
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &it, 1, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    for (unsigned n = 0; n < 2; n++) {
        NEED(poll(&f, &it, n, MOQ_SUB_FILL_OBJECT, sizeof(it)));
        NEED(object_fields(&it, object, false));
        moq_sub_fill_item_cleanup(&it);
    }
    NEED(moq_sub_cancel_fill(f.sub, f.child[0], now(&f)) == MOQ_OK);
    NEED(moq_sub_cancel_fill(f.sub, f.child[0], now(&f)) == MOQ_ERR_WRONG_STATE);
    NEED(pump(&f) && collect(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_TERMINAL, sizeof(it)));
    NEED(it.u.terminal.reason == MOQ_FILL_LOCAL_CANCELLED &&
         it.u.terminal.error_domain == MOQ_FILL_ERROR_NONE && it.u.terminal.error_code == 0);
    moq_sub_fill_item_cleanup(&it);
    NEED(moq_sub_cancel_fill(f.sub, f.child[0], now(&f)) == MOQ_ERR_STALE_HANDLE);
    NEED(moq_sub_fill_eq(moq_sub_track_fill(f.track), f.child[1]));
    NEED(write_object(&f, 1, object + 1, false));
    NEED(moq_session_write_fill_range(server(&f), f.peer_fill[1],
        MOQ_FETCH_RANGE_NON_EXISTENT, group, object + 2, now(&f)) == MOQ_OK);
    NEED(moq_session_end_fill(server(&f), f.peer_fill[1], now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 1, MOQ_SUB_FILL_OBJECT, sizeof(it)));
    NEED(object_fields(&it, object + 1, false));
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &it, 1, MOQ_SUB_FILL_GAP, sizeof(it)));
    NEED(it.u.gap.range_kind == MOQ_FETCH_RANGE_NON_EXISTENT &&
         it.u.gap.group_id == group && it.u.gap.object_id == object + 2);
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &it, 1, MOQ_SUB_FILL_TERMINAL, sizeof(it)));
    NEED(it.u.terminal.reason == MOQ_FILL_COMPLETE &&
         it.u.terminal.error_domain == MOQ_FILL_ERROR_NONE && it.u.terminal.error_code == 0);
    moq_sub_fill_item_cleanup(&it);
    NEED(moq_sub_track_is_active(f.track));
    NEED(request(&f, 2));
    NEED(!moq_sub_fill_eq(f.child[2], f.child[0]) && !moq_sub_fill_eq(f.child[2], f.child[1]));
    NEED(moq_sub_cancel_fill(f.sub, f.child[0], now(&f)) == MOQ_ERR_STALE_HANDLE);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 2, MOQ_SUB_FILL_CREATED, sizeof(it)));
done:
    moq_sub_fill_item_cleanup(&it);
    finish(&f);
}

static void reset_code(uint64_t code)
{
    fixture_t f;
    moq_sub_fill_item_t it = {0};
    NEED(start(&f, 4));
    NEED(request(&f, 0));
    NEED(moq_session_abort_fill(server(&f), f.peer_fill[0], code, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_TERMINAL, sizeof(it)));
    NEED(it.u.terminal.reason == MOQ_FILL_PEER_CANCELLED_OR_RESET &&
         it.u.terminal.error_domain == MOQ_FILL_ERROR_STREAM && it.u.terminal.error_code == code);
done:
    moq_sub_fill_item_cleanup(&it);
    finish(&f);
}

static void zero_stream_code(void) { reset_code(0); }
static void wide_stream_code(void) { reset_code((UINT64_C(1) << 54) + 13); }

static void created_quota_and_pending(void)
{
    fixture_t f;
    moq_sub_fill_item_t it = {0};
    NEED(start(&f, 1));
    NEED(request(&f, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(request(&f, 1));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_sub_stats_t stats;
    NEED(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    NEED(stats.fill_items_queued == 2 && stats.bytes_queued == 0 && stats.fill_items_polled == 0);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(moq_sub_poll_fill(f.sub, &it, sizeof(it)) == MOQ_DONE);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(poll(&f, &it, 1, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    NEED(stats.fill_items_queued == 0 && stats.bytes_queued == 0 && stats.fill_items_polled == 2);
done:
    moq_sub_fill_item_cleanup(&it);
    finish(&f);
}

static void created_pins_retired_track(void)
{
    fixture_t f;
    moq_sub_fill_item_t it = {0};
    NEED(start(&f, 1));
    NEED(request(&f, 0));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_session_on_transport_close(moq_simpair_client(f.pair), 0, now(&f)) == MOQ_OK);
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(!moq_sub_fill_is_valid(moq_sub_track_fill(f.track)));
    NEED(moq_sub_track_get_state(f.track) == MOQ_SUB_TRACK_DONE);
    NEED(moq_sub_release_track(f.sub, f.track) == MOQ_ERR_WRONG_STATE);
    NEED(poll(&f, &it, 0, MOQ_SUB_FILL_CREATED, sizeof(it)));
    moq_sub_fill_item_cleanup(&it);
    NEED(moq_sub_release_track(f.sub, f.track) == MOQ_OK);
done:
    moq_sub_fill_item_cleanup(&it);
    finish(&f);
}

static void unpolled_pending_destruction(void)
{
    fixture_t f;
    NEED(start(&f, 1));
    NEED(request(&f, 0));
    NEED(moq_session_open_fill(server(&f), f.peer_fill[0], now(&f)) == MOQ_OK);
    NEED(write_object(&f, 0, object, false));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_ERR_WOULD_BLOCK);
    moq_sub_stats_t stats;
    NEED(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    NEED(stats.fill_items_queued == 2 && stats.bytes_queued == sizeof(body) + sizeof(props));
done:
    finish(&f);
}

static void unchanged_fetch_canary(void)
{
    fixture_t f;
    struct {
        moq_sub_fetch_item_t item;
        uint8_t tail[17];
    } out;
    memset(&out, 0xa5, sizeof(out));
    memset(&out.item, 0, sizeof(out.item));
    NEED(start(&f, 1));
    moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("sized-fill")};
    moq_sub_fetch_cfg_t cfg;
    moq_sub_fetch_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ns, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.end_group = 0;
    cfg.end_object = 2;
    moq_sub_fetch_req_t *request = NULL;
    NEED(moq_sub_fetch(f.sub, &cfg, now(&f), &request) == MOQ_OK);
    NEED(request != NULL && pump(&f));
    moq_fetch_t peer = MOQ_FETCH_INVALID;
    for (unsigned n = 0; n < MAX_EVENTS; n++) {
        moq_event_t ev;
        if (moq_session_poll_events(server(&f), &ev, 1) == 0) break;
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) peer = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    NEED(moq_fetch_is_valid(peer));
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_object = 2;
    NEED(moq_session_accept_fetch(server(&f), peer, &ac, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_poll_fetch(f.sub, &out.item) == MOQ_OK);
    NEED(out.item.kind == MOQ_SUB_FETCH_OK && out.item.request == request &&
         out.item.u.ok.end_group == 0 && out.item.u.ok.end_object == 2);
    moq_sub_fetch_item_cleanup(&out.item);
    moq_fetch_object_cfg_t oc;
    moq_fetch_object_cfg_init(&oc);
    NEED(moq_rcbuf_create(&f.alloc, body, sizeof(body), &oc.payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_fetch_object(server(&f), peer, &oc, now(&f));
    moq_rcbuf_decref(oc.payload);
    NEED(rc == MOQ_OK);
    NEED(moq_session_end_fetch(server(&f), peer, now(&f)) == MOQ_OK);
    NEED(pump(&f));
    NEED(moq_sub_tick(f.sub, now(&f)) == MOQ_OK);
    NEED(moq_sub_poll_fetch(f.sub, &out.item) == MOQ_OK);
    NEED(out.item.kind == MOQ_SUB_FETCH_OBJECT && out.item.request == request &&
         out.item.u.object.group_id == 0 && out.item.u.object.object_id == 0);
    NEED(out.item.u.object.payload && moq_rcbuf_len(out.item.u.object.payload) == sizeof(body));
    NEED(memcmp(moq_rcbuf_data(out.item.u.object.payload), body, sizeof(body)) == 0);
    moq_sub_fetch_item_cleanup(&out.item);
    NEED(moq_sub_poll_fetch(f.sub, &out.item) == MOQ_OK);
    NEED(out.item.kind == MOQ_SUB_FETCH_COMPLETE && out.item.request == request);
    moq_sub_fetch_item_cleanup(&out.item);
    for (size_t i = sizeof(out.item); i < sizeof(out); i++)
        NEED(((const uint8_t *)&out)[i] == 0xa5);
done:
    moq_sub_fetch_item_cleanup(&out.item);
    finish(&f);
}

static void stamped_cleanup_prefixes(void)
{
    const size_t payload_end = offsetof(moq_sub_fill_item_t, u.object.payload) + sizeof(moq_rcbuf_t *);
    const size_t props_end = offsetof(moq_sub_fill_item_t, u.object.properties) + sizeof(moq_rcbuf_t *);
    const size_t sizes[] = {0, 4, 7, 8, payload_end - 1, payload_end,
                            props_end - 1, props_end, sizeof(future_item_t)};
    moq_sub_fill_item_cleanup(NULL);
    for (size_t n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        test_alloc_state_t state = {0};
        moq_alloc_t alloc = test_allocator(&state);
        moq_rcbuf_t *a = NULL, *b = NULL;
        moq_sub_fill_item_t source = {0};
        size_t extent = sizes[n] < sizeof(uint32_t) ? sizeof(uint32_t) : sizes[n];
        void *storage = malloc(extent);
        if (!CHECK(storage != NULL)) return;
        bool made = moq_rcbuf_create(&alloc, body, sizeof(body), &a) == MOQ_OK &&
                    moq_rcbuf_create(&alloc, props, sizeof(props), &b) == MOQ_OK;
        if (!CHECK(made)) {
            moq_rcbuf_decref(a);
            moq_rcbuf_decref(b);
            free(storage);
            return;
        }
        source.struct_size = (uint32_t)sizes[n];
        source.kind = MOQ_SUB_FILL_OBJECT;
        source.u.object.payload = moq_rcbuf_incref(a);
        source.u.object.properties = moq_rcbuf_incref(b);
        memset(storage, 0xa5, extent);
        memcpy(storage, &source, extent < sizeof(source) ? extent : sizeof(source));
        moq_sub_fill_item_cleanup(storage);
        moq_sub_fill_item_cleanup(storage);
        CHECK(moq_rcbuf_refcount(a) == (sizes[n] >= payload_end ? 1u : 2u));
        CHECK(moq_rcbuf_refcount(b) == (sizes[n] >= props_end ? 1u : 2u));
        if (sizes[n] < payload_end) moq_rcbuf_decref(a);
        if (sizes[n] < props_end) moq_rcbuf_decref(b);
        if (sizes[n] >= payload_end) CHECK(((moq_sub_fill_item_t *)storage)->u.object.payload == NULL);
        if (sizes[n] >= props_end) CHECK(((moq_sub_fill_item_t *)storage)->u.object.properties == NULL);
        if (extent > sizeof(source))
            for (size_t i = sizeof(source); i < extent; i++) CHECK(((uint8_t *)storage)[i] == 0xa5);
        moq_rcbuf_decref(a);
        moq_rcbuf_decref(b);
        free(storage);
        CHECK(state.balance == 0);
    }
    const uint32_t kinds[] = {MOQ_SUB_FILL_CREATED, MOQ_SUB_FILL_GAP, MOQ_SUB_FILL_TERMINAL};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        moq_sub_fill_item_t it, before;
        memset(&it, 0xa5, sizeof(it));
        it.struct_size = sizeof(it);
        it.kind = kinds[i];
        memcpy(&before, &it, sizeof(before));
        moq_sub_fill_item_cleanup(&it);
        moq_sub_fill_item_cleanup(&it);
        CHECK(memcmp(&it, &before, sizeof(it)) == 0);
    }
}

static void run(const char *name, void (*test)(void))
{
    int before = failures;
    test();
    printf("%s: %s\n", failures == before ? "PASS" : "FAIL", name);
}

int main(void)
{
    run("all_kinds_sizes_and_owned_lifetime", all_kinds_and_owned_lifetime);
    run("multiple_children_initiators_cancel_and_reuse", multiple_children_and_cancel);
    run("zero_stream_error_code", zero_stream_code);
    run("wide_stream_error_code", wide_stream_code);
    run("created_quota_and_pending", created_quota_and_pending);
    run("created_pins_retired_track", created_pins_retired_track);
    run("unpolled_pending_destruction", unpolled_pending_destruction);
    run("unchanged_two_argument_fetch_and_canary", unchanged_fetch_canary);
    run("stamped_cleanup_exact_allocated_prefixes", stamped_cleanup_prefixes);
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
