/* Independent public contract checks; no private symbols or wire injection. */
#include <moq/session.h>
#include <stddef.h>

typedef struct original_update_options {
    uint32_t struct_size, flags;
    uint64_t initiating_request_id;
    moq_location_filter_cfg_t location;
    uint64_t object_delivery_timeout_ms, subgroup_delivery_timeout_ms;
} original_update_options_t;

_Static_assert(MOQ_UPDATE_RESOLUTION_V0_SIZE == 80, "frozen resolution floor");
_Static_assert(sizeof(moq_update_resolution_t) == 80, "resolution extent");
_Static_assert(offsetof(moq_update_resolution_t, flags) == 4, "flags offset");
_Static_assert(offsetof(moq_update_resolution_t, update_request_id) == 8, "ID offset");
_Static_assert(offsetof(moq_update_resolution_t, largest_group) == 16, "group offset");
_Static_assert(offsetof(moq_update_resolution_t, largest_object) == 24, "object offset");
_Static_assert(offsetof(moq_update_resolution_t, location) == 32, "location offset");
_Static_assert(sizeof(original_update_options_t) == 80, "old options extent");
_Static_assert(offsetof(moq_subscription_update_options_t, resolution) == 80,
               "tail beyond complete old options");
#define OLD_FIELD(field) _Static_assert(offsetof(moq_subscription_update_options_t, field) == \
    offsetof(original_update_options_t, field), "old " #field " offset")
OLD_FIELD(struct_size);
OLD_FIELD(flags);
OLD_FIELD(initiating_request_id);
OLD_FIELD(location);
OLD_FIELD(object_delivery_timeout_ms);
OLD_FIELD(subgroup_delivery_timeout_ms);
#undef OLD_FIELD

#ifndef MOQ_UPDATE_CONTRACT_LAYOUT_ONLY
#include <moq/sim.h>
#include <moq/rcbuf.h>
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
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

typedef struct memory {
    test_alloc_state_t state;
    size_t allocations, frees;
} memory_t;

static void *count_alloc(size_t n, void *ctx)
{
    memory_t *m = ctx;
    m->allocations++;
    return test_alloc(n, &m->state);
}
static void *count_realloc(void *p, size_t old, size_t n, void *ctx)
{
    memory_t *m = ctx;
    m->allocations++;
    return test_realloc(p, old, n, &m->state);
}
static void count_free(void *p, size_t n, void *ctx)
{
    memory_t *m = ctx;
    m->frees++;
    test_free(p, n, &m->state);
}

typedef struct fixture {
    memory_t memory;
    moq_alloc_t alloc;
    moq_simpair_t *pair;
    moq_session_t *publisher, *subscriber;
    moq_subscription_t ps, ss;
    moq_publication_t pp, sp;
    moq_version_t version;
    bool publication, capture;
    uint8_t ack[4][64];
    size_t ack_len[4], ack_count;
} fixture_t;

static uint64_t now(fixture_t *f) { return moq_simpair_now_us(f->pair); }

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (!f->capture || r->kind != MOQ_SIM_TRACE_ACTION ||
        r->from != MOQ_PERSPECTIVE_SERVER || r->action_kind != MOQ_ACTION_SEND_BIDI_STREAM ||
        r->detail_bytes.len == 0 || r->detail_bytes.data[0] != 7) return;
    size_t i = f->ack_count++;
    if (!CHECK(i < 4 && r->detail_bytes.len <= sizeof(f->ack[0]))) return;
    f->ack_len[i] = r->detail_bytes.len;
    memcpy(f->ack[i], r->detail_bytes.data, r->detail_bytes.len);
}

static bool pump(fixture_t *f)
{
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 64, NULL) == MOQ_OK);
    return true;
}

static bool drain(moq_session_t *s)
{
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t ev;
        if (!moq_session_poll_events(s, &ev, 1)) return true;
        moq_event_cleanup(&ev);
    }
    REQUIRE(false);
    return false;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t ev;
        if (!moq_session_poll_events(s, &ev, 1)) break;
        if (ev.kind == kind) { *out = ev; return true; }
        moq_event_cleanup(&ev);
    }
    REQUIRE(false);
    return false;
}

static bool start(fixture_t *f, bool publication, moq_version_t version, bool defer)
{
    memset(f, 0, sizeof(*f));
    f->publication = publication;
    f->version = version;
    f->alloc = (moq_alloc_t){.ctx = &f->memory, .alloc = count_alloc,
        .realloc = count_realloc, .free = count_free};
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = &f->alloc;
    pc.version = version;
    pc.initial_now_us = 1000;
    pc.seed = 11;
    pc.max_actions = 8;
    pc.client_send_request_capacity = pc.server_send_request_capacity = true;
    pc.client_initial_request_capacity = pc.server_initial_request_capacity = 32;
    pc.server_defer_subscription_updates = defer;
    pc.trace_fn = trace;
    pc.trace_ctx = f;
    REQUIRE(moq_simpair_create(&pc, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK && pump(f));
    f->publisher = moq_simpair_server(f->pair);
    f->subscriber = moq_simpair_client(f->pair);
    REQUIRE(moq_session_state(f->publisher) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(f->subscriber) == MOQ_SESS_ESTABLISHED);
    REQUIRE(drain(f->publisher) && drain(f->subscriber));
    moq_bytes_t part = MOQ_BYTES_LITERAL("upd-contract");
    moq_namespace_t ns = {&part, 1};
    moq_event_t ev;
    if (publication) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        REQUIRE(moq_session_publish(f->publisher, &cfg, now(f), &f->pp) == MOQ_OK);
        REQUIRE(pump(f) && take(f->subscriber, MOQ_EVENT_PUBLISH_REQUEST, &ev));
        f->sp = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init(&ac);
        REQUIRE(moq_session_accept_publish(f->subscriber, f->sp, &ac, now(f)) == MOQ_OK);
    } else {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        REQUIRE(moq_session_subscribe(f->subscriber, &cfg, now(f), &f->ss) == MOQ_OK);
        REQUIRE(pump(f) && take(f->publisher, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
        f->ps = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        REQUIRE(moq_session_accept_subscribe(f->publisher, f->ps, &ac, now(f)) == MOQ_OK);
    }
    REQUIRE(pump(f) && drain(f->publisher) && drain(f->subscriber));
    f->capture = version == MOQ_VERSION_DRAFT_22;
    return true;
}

static void finish(fixture_t *f)
{
    if (f->pair) moq_simpair_destroy(f->pair);
    CHECK(f->memory.state.balance == 0);
}

static bool note(fixture_t *f, uint64_t group, uint64_t object)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("upd-contract");
    moq_namespace_t ns = {&part, 1};
    REQUIRE(moq_session_note_object_published(f->publisher, &ns,
        MOQ_BYTES_LITERAL("v"), group, object) == MOQ_OK);
    return true;
}

static bool update(fixture_t *f, const moq_location_filter_cfg_t *loc,
                    const moq_fill_cfg_t *fill, bool legacy_next)
{
    moq_result_t rc;
    if (f->publication) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 37;
        cfg.location = loc;
        cfg.fill = fill;
        rc = moq_session_update_publication(f->subscriber, f->sp, &cfg, now(f));
    } else {
        moq_subscription_update_cfg_t cfg;
        moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 37;
        cfg.location = loc;
        cfg.fill = fill;
        cfg.has_filter = legacy_next;
        cfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        rc = moq_session_update_subscription(f->subscriber, f->ss, &cfg, now(f));
    }
    REQUIRE(rc == MOQ_OK);
    REQUIRE(pump(f));
    return true;
}

static moq_result_t query(fixture_t *f, moq_session_t *s, uint64_t handle,
                          uint64_t id, moq_update_resolution_t *out, size_t size)
{
    return f->publication
        ? moq_session_get_publication_update_resolution(s, (moq_publication_t){handle}, id, out, size)
        : moq_session_get_subscription_update_resolution(s, (moq_subscription_t){handle}, id, out, size);
}
static uint64_t publisher_handle(fixture_t *f) { return f->publication ? f->pp._opaque : f->ps._opaque; }
static uint64_t subscriber_handle(fixture_t *f) { return f->publication ? f->sp._opaque : f->ss._opaque; }

static bool owed(fixture_t *f, uint64_t *id, bool *has, moq_location_filter_cfg_t *raw)
{
    moq_result_t rc = f->publication
        ? moq_session_publication_owed_update(f->publisher, f->pp, id, has, raw)
        : moq_session_subscription_owed_update(f->publisher, f->ps, id, has, raw);
    REQUIRE(rc == MOQ_OK);
    return true;
}

static moq_result_t accept(fixture_t *f, uint64_t id)
{
    moq_accept_subscription_update_cfg_t cfg;
    moq_accept_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = id;
    return f->publication
        ? moq_session_accept_publication_update(f->publisher, f->pp, &cfg, now(f))
        : moq_session_accept_subscription_update(f->publisher, f->ps, &cfg, now(f));
}

static bool take_update(fixture_t *f, moq_subscription_update_options_t *copy,
                        const moq_subscription_update_options_t **borrowed,
                        uint64_t *epoch)
{
    moq_event_t ev;
    REQUIRE(take(f->publisher, f->publication ? MOQ_EVENT_PUBLISH_UPDATED :
                 MOQ_EVENT_SUBSCRIBE_UPDATED, &ev));
    const moq_subscription_update_options_t *options = f->publication
        ? ev.u.publish_updated.options : ev.u.subscribe_updated.options;
    bool valid = options && options->struct_size >= 160;
    if (valid) {
        if (copy) *copy = *options;
        if (borrowed) *borrowed = options;
        if (epoch) *epoch = ev.borrow_epoch;
    }
    moq_event_cleanup(&ev);
    REQUIRE(valid);
    return true;
}

static bool ack_largest(fixture_t *f, uint64_t group, uint64_t object)
{
    moq_event_t ev;
    REQUIRE(take(f->subscriber, f->publication ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                 MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &ev));
    bool ok = f->publication
        ? ev.u.publication_update_ok.has_largest &&
          ev.u.publication_update_ok.largest_group == group && ev.u.publication_update_ok.largest_object == object
        : ev.u.subscription_update_ok.has_largest &&
          ev.u.subscription_update_ok.largest_group == group && ev.u.subscription_update_ok.largest_object == object;
    moq_event_cleanup(&ev);
    REQUIRE(ok);
    return true;
}

static bool refusal(fixture_t *f, moq_session_t *s, uint64_t handle,
                     uint64_t id, size_t size, moq_result_t expected)
{
    struct { moq_update_resolution_t value; uint8_t tail[16]; } out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    REQUIRE(query(f, s, handle, id, &out.value, size) == expected);
    REQUIRE(memcmp(&out, &before, sizeof(out)) == 0);
    return true;
}

static moq_update_resolution_t next_at(uint64_t id, uint64_t group, uint64_t object)
{
    moq_update_resolution_t value = {
        .struct_size = 80, .flags = 3, .update_request_id = id,
        .largest_group = group, .largest_object = object,
        .location = {.struct_size = 48, .kind = MOQ_LOCATION_FILTER_ABSOLUTE_START,
                     .start_group = group, .start_object = object + 1}
    };
    return value;
}

static void test_query_contract(bool publication)
{
    fixture_t f, other = {0};
    NEED(start(&f, publication, MOQ_VERSION_DRAFT_22, true));
    uint64_t handle = publisher_handle(&f);
    NEED(refusal(&f, f.publisher, handle, UINT64_MAX, 80, MOQ_ERR_WRONG_STATE));
    NEED(note(&f, 1, 2));
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    NEED(update(&f, &loc, NULL, false));
    uint64_t id;
    bool has;
    moq_location_filter_cfg_t raw;
    NEED(owed(&f, &id, &has, &raw));
    NEED(has && raw.kind == MOQ_LOCATION_FILTER_NEXT_OBJECT);
    moq_update_resolution_t expected = next_at(id, 1, 2), before_poll;
    NEED(query(&f, f.publisher, handle, id, &before_poll, 80) == MOQ_OK);
    NEED(memcmp(&expected, &before_poll, 80) == 0);
    moq_subscription_update_options_t copy;
    const moq_subscription_update_options_t *borrowed = NULL;
    uint64_t epoch;
    NEED(take_update(&f, &copy, &borrowed, &epoch));
    NEED(memcmp(&copy.resolution, &expected, 80) == 0);
    NEED(copy.location.kind == MOQ_LOCATION_FILTER_NEXT_OBJECT);
    NEED(copy.initiating_request_id == id && copy.flags == MOQ_SUBSCRIPTION_UPDATE_OPTIONS_LOCATION);
    NEED(note(&f, 3, 0));
    size_t allocations = f.memory.allocations, frees = f.memory.frees;
    size_t actions = moq_session_action_capacity(f.publisher);
    uint64_t deadline = moq_session_next_deadline_us(f.publisher);
    for (size_t size = 0; size < 80; size++) {
        NEED(refusal(&f, f.publisher, handle, id, size, MOQ_ERR_INVAL));
        NEED(refusal(&f, f.publisher, 0, id, size, MOQ_ERR_INVAL));
    }
    for (size_t size = 80; size <= 96; size++) {
        struct { moq_update_resolution_t value; uint8_t tail[16]; } out;
        memset(&out, 0xa5, sizeof(out));
        NEED(query(&f, f.publisher, handle, id, &out.value, size) == MOQ_OK);
        NEED(memcmp(&out.value, &expected, 80) == 0);
        for (unsigned i = 0; i < sizeof(out.tail); i++) NEED(out.tail[i] == 0xa5);
    }
    struct { moq_update_resolution_t value; uint8_t tail[16]; } future;
    memset(&future, 0xa5, sizeof(future));
    NEED(query(&f, f.publisher, handle, id, &future.value, SIZE_MAX) == MOQ_OK);
    NEED(memcmp(&future.value, &expected, 80) == 0);
    for (unsigned i = 0; i < sizeof(future.tail); i++) NEED(future.tail[i] == 0xa5);
    NEED(refusal(&f, NULL, 0, id, 80, MOQ_ERR_INVAL));
    NEED(query(&f, f.publisher, 0, id, NULL, 80) == MOQ_ERR_INVAL);
    NEED(refusal(&f, f.publisher, 0, id, 80, MOQ_ERR_STALE_HANDLE));
    NEED(refusal(&f, f.subscriber, subscriber_handle(&f), id + 1, 80, MOQ_ERR_WRONG_STATE));
    NEED(refusal(&f, f.publisher, handle, id + 1, 80, MOQ_ERR_STALE_HANDLE));
    moq_update_resolution_t wrong_pool, saved;
    memset(&wrong_pool, 0xa5, sizeof(wrong_pool));
    saved = wrong_pool;
    moq_result_t rc = publication
        ? moq_session_get_subscription_update_resolution(f.publisher, (moq_subscription_t){handle}, id, &wrong_pool, 80)
        : moq_session_get_publication_update_resolution(f.publisher, (moq_publication_t){handle}, id, &wrong_pool, 80);
    NEED(rc == MOQ_ERR_STALE_HANDLE && memcmp(&wrong_pool, &saved, 80) == 0);
    NEED(moq_session_borrow_valid(f.publisher, epoch));
    NEED(memcmp(borrowed, &copy, sizeof(copy)) == 0);
    NEED(f.memory.allocations == allocations && f.memory.frees == frees);
    NEED(moq_session_action_capacity(f.publisher) == actions);
    NEED(moq_session_next_deadline_us(f.publisher) == deadline);
    NEED(start(&other, publication, MOQ_VERSION_DRAFT_22, true));
    NEED(refusal(&f, other.publisher, handle, id, 80, MOQ_ERR_STALE_HANDLE));
    NEED(accept(&f, id) == MOQ_OK && pump(&f));
    NEED(ack_largest(&f, 1, 2));
    NEED(refusal(&f, f.publisher, handle, id + 1, 80, MOQ_ERR_WRONG_STATE));
    static const uint8_t literal_ack[] = {7, 0, 4, 1, 9, 1, 2};
    NEED(f.ack_count == 1 && f.ack_len[0] == sizeof(literal_ack));
    NEED(memcmp(f.ack[0], literal_ack, sizeof(literal_ack)) == 0);
    NEED(moq_simpair_advance_to(f.pair, now(&f) + 10) == MOQ_OK);
    NEED(memcmp(&copy.resolution, &expected, 80) == 0);
    NEED(update(&f, NULL, NULL, false));
    uint64_t next_id;
    NEED(owed(&f, &next_id, &has, &raw));
    NEED(next_id != id && !has);
    NEED(refusal(&f, f.publisher, handle, id, 80, MOQ_ERR_STALE_HANDLE));
    moq_reject_subscription_update_cfg_t reject;
    moq_reject_subscription_update_cfg_init(&reject);
    reject.update_request_id = next_id;
    reject.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
    rc = publication
        ? moq_session_reject_publication_update(f.publisher, f.pp, &reject, now(&f))
        : moq_session_reject_subscription_update(f.publisher, f.ps, &reject, now(&f));
    NEED(rc == MOQ_OK);
    NEED(refusal(&f, f.publisher, handle, next_id, 80, MOQ_ERR_STALE_HANDLE));
done:
    finish(&other);
    finish(&f);
}

static void test_relative_zero(bool publication)
{
    fixture_t f;
    NEED(start(&f, publication, MOQ_VERSION_DRAFT_22, true));
    NEED(note(&f, UINT64_MAX, UINT64_MAX));
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    NEED(update(&f, &loc, &fill, false));
    uint64_t id;
    NEED(owed(&f, &id, NULL, NULL));
    moq_update_resolution_t value;
    NEED(query(&f, f.publisher, publisher_handle(&f), id, &value, sizeof(value)) == MOQ_OK);
    NEED(value.location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START);
    NEED(value.location.start_group == UINT64_MAX && value.location.start_object == 0);
    NEED(accept(&f, id) == MOQ_OK && pump(&f));
    static const uint8_t literal_ack[] = {
        7, 0, 20, 1, 9,
        255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255
    };
    NEED(f.ack_count == 1 && f.ack_len[0] == sizeof(literal_ack));
    NEED(memcmp(f.ack[0], literal_ack, sizeof(literal_ack)) == 0);
    NEED(ack_largest(&f, UINT64_MAX, UINT64_MAX));
    moq_event_t ev;
    NEED(take(f.publisher, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_request_event_t request = ev.u.fill_request;
    moq_event_cleanup(&ev);
    NEED(request.initiating_request_id == id);
    NEED(request.start_group == UINT64_MAX && request.start_object == 0);
    NEED(request.end_group == UINT64_MAX && request.end_object == UINT64_MAX);
done:
    finish(&f);
}

static void test_legacy(bool publication, moq_version_t version)
{
    fixture_t f;
    NEED(start(&f, publication, version, true));
    uint64_t handle = publisher_handle(&f);
    NEED(refusal(&f, f.publisher, handle, UINT64_MAX, 80, MOQ_ERR_UNSUPPORTED));
    NEED(refusal(&f, f.publisher, 0, 0, 80, MOQ_ERR_STALE_HANDLE));
    NEED(refusal(&f, f.subscriber, subscriber_handle(&f), 0, 80, MOQ_ERR_WRONG_STATE));
    NEED(refusal(&f, f.publisher, handle, 0, 79, MOQ_ERR_INVAL));
    NEED(note(&f, 1, 2));
    NEED(update(&f, NULL, NULL, !publication));
    uint64_t id;
    bool has;
    moq_location_filter_cfg_t raw;
    NEED(owed(&f, &id, &has, &raw));
    NEED(has == !publication);
    if (!publication) NEED(raw.kind == MOQ_LOCATION_FILTER_RELATIVE_GROUP && raw.relative_group_offset == 0);
    NEED(refusal(&f, f.publisher, handle, id + 1, 80, MOQ_ERR_UNSUPPORTED));
    NEED(note(&f, 3, 0));
    NEED(accept(&f, id) == MOQ_OK && pump(&f));
    NEED(ack_largest(&f, 3, 0));
done:
    finish(&f);
}

static void test_automatic_copies(bool publication)
{
    fixture_t f;
    NEED(start(&f, publication, MOQ_VERSION_DRAFT_22, false));
    NEED(note(&f, 1, 2));
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    NEED(update(&f, &loc, NULL, false));
    NEED(ack_largest(&f, 1, 2));
    NEED(note(&f, 3, 0));
    /* First answer arrived, but its publisher notification remains unpolled. */
    NEED(update(&f, NULL, NULL, false));
    NEED(ack_largest(&f, 3, 0));
    NEED(refusal(&f, f.publisher, publisher_handle(&f), 0, 80, MOQ_ERR_WRONG_STATE));
    moq_subscription_update_options_t first, second;
    NEED(take_update(&f, &first, NULL, NULL));
    NEED(take_update(&f, &second, NULL, NULL));
    NEED(first.initiating_request_id != second.initiating_request_id);
    moq_update_resolution_t expected = next_at(first.initiating_request_id, 1, 2);
    NEED(memcmp(&first.resolution, &expected, 80) == 0);
    moq_location_filter_cfg_t zero = {0};
    NEED(second.flags == 0 && second.resolution.flags == MOQ_UPDATE_RESOLUTION_HAS_LARGEST);
    NEED(second.resolution.update_request_id == second.initiating_request_id);
    NEED(second.resolution.largest_group == 3 && second.resolution.largest_object == 0);
    NEED(memcmp(&second.location, &zero, sizeof(zero)) == 0);
    NEED(memcmp(&second.resolution.location, &zero, sizeof(zero)) == 0);
    NEED(moq_simpair_advance_to(f.pair, now(&f) + 10) == MOQ_OK);
    NEED(memcmp(&first.resolution, &expected, 80) == 0);
    static const uint8_t first_ack[] = {7, 0, 4, 1, 9, 1, 2};
    static const uint8_t second_ack[] = {7, 0, 4, 1, 9, 3, 0};
    NEED(f.ack_count == 2 && f.ack_len[0] == sizeof(first_ack) && f.ack_len[1] == sizeof(second_ack));
    NEED(memcmp(f.ack[0], first_ack, sizeof(first_ack)) == 0);
    NEED(memcmp(f.ack[1], second_ack, sizeof(second_ack)) == 0);
done:
    finish(&f);
}

static bool occupy_actions(fixture_t *f, moq_subgroup_handle_t *handle)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = 6;
    moq_result_t rc = f->publication
        ? moq_session_open_pub_subgroup(f->publisher, f->pp, &cfg, now(f), handle)
        : moq_session_open_subgroup(f->publisher, f->ps, &cfg, now(f), handle);
    REQUIRE(rc == MOQ_OK);
    moq_rcbuf_t *body = NULL;
    REQUIRE(moq_rcbuf_create(&f->alloc, (const uint8_t *)"q", 1, &body) == MOQ_OK);
    for (uint64_t i = 0; i < 8 && moq_session_action_capacity(f->publisher) > 0; i++) {
        rc = moq_session_write_object(f->publisher, *handle, i, body, now(f));
        if (rc != MOQ_OK) break;
    }
    moq_rcbuf_decref(body);
    REQUIRE(rc == MOQ_OK && moq_session_action_capacity(f->publisher) == 0);
    return true;
}

static void test_fill_basis(bool publication, bool defer)
{
    fixture_t f;
    NEED(start(&f, publication, MOQ_VERSION_DRAFT_22, defer));
    NEED(note(&f, 1, 2));
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    loc.relative_group_offset = 1;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.has_group_order = true;
    fill.group_order = MOQ_GROUP_ORDER_ASCENDING;
    fill.has_fill_timeout = true;
    fill.fill_timeout_ms = 27;
    uint64_t processed = now(&f);
    NEED(update(&f, &loc, &fill, false));
    NEED(note(&f, 3, 0));
    moq_subscription_update_options_t options;
    NEED(take_update(&f, &options, NULL, NULL));
    uint64_t id = options.initiating_request_id;
    NEED(options.location.kind == MOQ_LOCATION_FILTER_RELATIVE_GROUP);
    NEED(options.resolution.largest_group == 1 && options.resolution.largest_object == 2);
    NEED(options.resolution.location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START);
    NEED(options.resolution.location.start_group == 1 && options.resolution.location.start_object == 0);
    if (defer) {
        uint64_t owed_id;
        NEED(owed(&f, &owed_id, NULL, NULL) && owed_id == id);
        NEED(moq_simpair_advance_to(f.pair, now(&f) + 100) == MOQ_OK);
        NEED(moq_session_action_capacity(f.publisher) == 8);
        moq_subgroup_handle_t pressure_stream;
        NEED(occupy_actions(&f, &pressure_stream));
        NEED(accept(&f, id) == MOQ_ERR_WOULD_BLOCK);
        NEED(moq_session_action_capacity(f.publisher) == 0 && f.ack_count == 0);
        moq_update_resolution_t value;
        NEED(query(&f, f.publisher, publisher_handle(&f), id, &value, 80) == MOQ_OK);
        NEED(memcmp(&value, &options.resolution, 80) == 0);
        NEED(note(&f, 5, 0));
        NEED(pump(&f));
        NEED(moq_session_close_subgroup(f.publisher, pressure_stream, now(&f)) == MOQ_OK);
        NEED(pump(&f));
        NEED(accept(&f, id) == MOQ_OK && pump(&f));
    }
    NEED(ack_largest(&f, 1, 2));
    static const uint8_t literal_ack[] = {7, 0, 4, 1, 9, 1, 2};
    NEED(f.ack_count == 1 && f.ack_len[0] == sizeof(literal_ack));
    NEED(memcmp(f.ack[0], literal_ack, sizeof(literal_ack)) == 0);
    moq_event_t ev;
    NEED(take(f.publisher, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_request_event_t request = ev.u.fill_request;
    moq_event_cleanup(&ev);
    NEED(request.initiating_request_id == id && request.processed_at_us == processed);
    NEED(request.start_group == 1 && request.start_object == 0 && request.end_group == 1 && request.end_object == 2);
    NEED(request.subscriber_priority == 37 && request.group_order == MOQ_GROUP_ORDER_ASCENDING);
    NEED(request.has_fill_timeout && request.fill_timeout_ms == 27);
    NEED(moq_session_open_fill(f.publisher, request.fill, now(&f)) == MOQ_OK);
    for (uint64_t i = 0; i < 3; i++) {
        moq_fetch_object_cfg_t cfg;
        moq_fetch_object_cfg_init(&cfg);
        cfg.group_id = 1;
        cfg.object_id = i;
        NEED(moq_rcbuf_create(&f.alloc, (const uint8_t *)"x", 1, &cfg.payload) == MOQ_OK);
        moq_result_t rc = moq_session_write_fill_object(f.publisher, request.fill, &cfg, now(&f));
        moq_rcbuf_decref(cfg.payload);
        NEED(rc == MOQ_OK);
    }
    NEED(moq_session_end_fill(f.publisher, request.fill, now(&f)) == MOQ_OK && pump(&f));
    unsigned objects = 0, terminals = 0, n;
    for (n = 0; n < 64; n++) {
        if (!moq_session_poll_events(f.subscriber, &ev, 1)) break;
        if (ev.kind == MOQ_EVENT_FILL_OBJECT) {
            CHECK(ev.u.fill_object.initiating_request_id == id);
            CHECK(ev.u.fill_object.group_id == 1 && ev.u.fill_object.object_id == objects);
            objects++;
        } else if (ev.kind == MOQ_EVENT_FILL_TERMINAL) {
            CHECK(ev.u.fill_terminal.initiating_request_id == id);
            CHECK(ev.u.fill_terminal.reason == MOQ_FILL_COMPLETE);
            CHECK(ev.u.fill_terminal.error_domain == MOQ_FILL_ERROR_NONE);
            CHECK(ev.u.fill_terminal.error_code == 0);
            terminals++;
        }
        moq_event_cleanup(&ev);
    }
    NEED(n < 64);
    NEED(objects == 3 && terminals == 1);
done:
    finish(&f);
}

static void sub_contract(void) { test_query_contract(false); }
static void pub_contract(void) { test_query_contract(true); }
static void sub_relative(void) { test_relative_zero(false); }
static void pub_relative(void) { test_relative_zero(true); }
static void legacy16_sub(void) { test_legacy(false, MOQ_VERSION_DRAFT_16); }
static void legacy16_pub(void) { test_legacy(true, MOQ_VERSION_DRAFT_16); }
static void legacy18_sub(void) { test_legacy(false, MOQ_VERSION_DRAFT_18); }
static void legacy18_pub(void) { test_legacy(true, MOQ_VERSION_DRAFT_18); }
static void sub_automatic(void) { test_automatic_copies(false); }
static void pub_automatic(void) { test_automatic_copies(true); }
static void sub_fill_retry(void) { test_fill_basis(false, true); }
static void pub_fill_retry(void) { test_fill_basis(true, true); }
static void sub_fill_auto(void) { test_fill_basis(false, false); }
static void pub_fill_auto(void) { test_fill_basis(true, false); }

int main(void)
{
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"subscription_query_contract", sub_contract},
        {"publication_query_contract", pub_contract},
        {"subscription_relative_zero_ack_fill", sub_relative},
        {"publication_relative_zero_ack_fill", pub_relative},
        {"d16_subscription_accept_time", legacy16_sub},
        {"d16_publication_accept_time", legacy16_pub},
        {"d18_subscription_accept_time", legacy18_sub},
        {"d18_publication_accept_time", legacy18_pub},
        {"subscription_automatic_event_copies", sub_automatic},
        {"publication_automatic_event_copies", pub_automatic},
        {"subscription_fill_retry_basis", sub_fill_retry},
        {"publication_fill_retry_basis", pub_fill_retry},
        {"subscription_fill_automatic_basis", sub_fill_auto},
        {"publication_fill_automatic_basis", pub_fill_auto},
    };
    unsigned passed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned before = failures;
        cases[i].run();
        bool ok = before == failures;
        printf("%s: %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        if (ok) passed++;
    }
    printf("%u/%zu cases passed; %u failed assertions\n", passed,
           sizeof(cases) / sizeof(cases[0]), failures);
    return failures ? 1 : 0;
}
#endif
