/* Public observation must agree with the immutable REQUEST_OK basis. */
#include <moq/session.h>
#include <stddef.h>

#ifdef MOQ_UPDATE_RESOLUTION_HAS_LARGEST
typedef struct old_options {
    uint32_t struct_size, flags;
    uint64_t initiating_request_id;
    moq_location_filter_cfg_t location;
    uint64_t object_delivery_timeout_ms, subgroup_delivery_timeout_ms;
} old_options_t;

_Static_assert(sizeof(moq_update_resolution_t) == 80, "frozen resolution floor");
_Static_assert(offsetof(moq_subscription_update_options_t, resolution) ==
               sizeof(old_options_t), "complete old options prefix");
_Static_assert(sizeof(moq_event_t) == 144, "unchanged event envelope");
#endif

#if defined(MOQ_UPDATE_RESOLUTION_LAYOUT_ONLY)
#ifndef MOQ_UPDATE_RESOLUTION_HAS_LARGEST
#error "update resolution layout is absent"
#endif
#elif !defined(MOQ_UPDATE_RESOLUTION_HAS_LARGEST)
#include <stdio.h>
int main(void)
{
    fprintf(stderr, "update_resolution.api_missing\n");
    return 1;
}
#else
#include <moq/sim.h>
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>

static unsigned failures, checks, cases;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define REQUIRE(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "%s:%d: prerequisite %s\n", __FILE__, __LINE__, #c); \
    goto done; } } while (0)

typedef struct pair {
    moq_simpair_t *sp;
    moq_session_t *publisher, *subscriber;
    moq_subscription_t ps, ss;
    moq_publication_t pp, spub;
    bool publication, observe_ack, ack_overflow;
    uint8_t ack[64];
    size_t ack_len, ack_count;
    test_alloc_state_t memory;
    moq_alloc_t alloc;
} pair_t;

static void trace(void *ctx, const moq_sim_trace_record_t *record)
{
    pair_t *p = ctx;
    if (!p->observe_ack || record->kind != MOQ_SIM_TRACE_ACTION ||
        record->from != MOQ_PERSPECTIVE_SERVER ||
        record->action_kind != MOQ_ACTION_SEND_BIDI_STREAM ||
        record->detail_bytes.len == 0 || record->detail_bytes.data[0] != 7)
        return;
    p->ack_count++;
    if (record->detail_bytes.len > sizeof(p->ack)) {
        p->ack_overflow = true;
        return;
    }
    p->ack_len = record->detail_bytes.len;
    memcpy(p->ack, record->detail_bytes.data, p->ack_len);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1) moq_event_cleanup(&ev);
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1) {
        if (ev.kind == kind) { *out = ev; return true; }
        moq_event_cleanup(&ev);
    }
    return false;
}

static bool pump(pair_t *p)
{
    return moq_simpair_run_until_quiescent(p->sp, 32, NULL) >= 0;
}

static moq_result_t note(pair_t *p, uint64_t group, uint64_t object)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("resolution");
    moq_namespace_t ns = { &part, 1 };
    return moq_session_note_object_published(p->publisher, &ns,
        MOQ_BYTES_LITERAL("track"), group, object);
}

static bool open_pair(pair_t *p, bool publication, bool defer, moq_version_t version)
{
    memset(p, 0, sizeof(*p));
    p->publication = publication;
    p->alloc = test_allocator(&p->memory);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &p->alloc;
    cfg.version = version;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 16;
    cfg.server_defer_subscription_updates = defer;
    cfg.trace_fn = trace;
    cfg.trace_ctx = p;
    if (moq_simpair_create(&cfg, &p->sp) != MOQ_OK ||
        moq_simpair_start(p->sp) != MOQ_OK || !pump(p)) return false;
    p->publisher = moq_simpair_server(p->sp);
    p->subscriber = moq_simpair_client(p->sp);
    drain(p->publisher);
    drain(p->subscriber);
    moq_bytes_t part = MOQ_BYTES_LITERAL("resolution");
    moq_namespace_t ns = { &part, 1 };
    moq_event_t ev;
    if (publication) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init_sized(&pc, sizeof(pc));
        pc.track_namespace = ns;
        pc.track_name = MOQ_BYTES_LITERAL("track");
        if (moq_session_publish(p->publisher, &pc, 1, &p->pp) != MOQ_OK ||
            !pump(p) || !take(p->subscriber, MOQ_EVENT_PUBLISH_REQUEST, &ev))
            return false;
        p->spub = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init(&ac);
        if (moq_session_accept_publish(p->subscriber, p->spub, &ac, 2) != MOQ_OK)
            return false;
    } else {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = ns;
        sc.track_name = MOQ_BYTES_LITERAL("track");
        if (moq_session_subscribe(p->subscriber, &sc, 1, &p->ss) != MOQ_OK ||
            !pump(p) || !take(p->publisher, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev))
            return false;
        p->ps = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        if (moq_session_accept_subscribe(p->publisher, p->ps, &ac, 2) != MOQ_OK)
            return false;
    }
    if (!pump(p)) return false;
    drain(p->publisher);
    drain(p->subscriber);
    return true;
}

static void close_pair(pair_t *p)
{
    if (p->sp) moq_simpair_destroy(p->sp);
    CHECK(p->memory.balance == 0);
}

static moq_result_t send_update(pair_t *p, const moq_location_filter_cfg_t *location)
{
    if (p->publication) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.location = location;
        return moq_session_update_publication(p->subscriber, p->spub, &cfg, 3);
    }
    moq_subscription_update_cfg_t cfg;
    moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.location = location;
    return moq_session_update_subscription(p->subscriber, p->ss, &cfg, 3);
}

static moq_result_t owed(pair_t *p, uint64_t *id)
{
    bool has;
    moq_location_filter_cfg_t filter;
    return p->publication
        ? moq_session_publication_owed_update(p->publisher, p->pp, id, &has, &filter)
        : moq_session_subscription_owed_update(p->publisher, p->ps, id, &has, &filter);
}

static moq_result_t query(pair_t *p, uint64_t id, moq_update_resolution_t *out,
                           size_t size)
{
    return p->publication
        ? moq_session_get_publication_update_resolution(p->publisher, p->pp, id, out, size)
        : moq_session_get_subscription_update_resolution(p->publisher, p->ps, id, out, size);
}

static moq_result_t accept(pair_t *p, uint64_t id)
{
    moq_accept_subscription_update_cfg_t cfg;
    moq_accept_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = id;
    if (p->publication) {
        return moq_session_accept_publication_update(p->publisher, p->pp, &cfg, 4);
    }
    return moq_session_accept_subscription_update(p->publisher, p->ps, &cfg, 4);
}

static void expect_resolution(const moq_update_resolution_t *r, uint64_t id,
                              bool has_largest, unsigned kind)
{
    CHECK(r->struct_size == sizeof(*r));
    CHECK(r->update_request_id == id);
    CHECK((r->flags & MOQ_UPDATE_RESOLUTION_HAS_LARGEST) ==
          (has_largest ? MOQ_UPDATE_RESOLUTION_HAS_LARGEST : 0));
    CHECK(r->largest_group == (has_largest ? 1u : 0u));
    CHECK(r->largest_object == (has_largest ? 2u : 0u));
    CHECK((r->flags & MOQ_UPDATE_RESOLUTION_LOCATION_REPLACEMENT) ==
          (kind ? MOQ_UPDATE_RESOLUTION_LOCATION_REPLACEMENT : 0));
    if (!kind) {
        moq_location_filter_cfg_t zero = {0};
        CHECK(memcmp(&r->location, &zero, sizeof(zero)) == 0);
    } else {
        CHECK(r->location.struct_size == sizeof(r->location));
        CHECK(r->location.kind == (kind == 3 ? MOQ_LOCATION_FILTER_NONE :
                                               MOQ_LOCATION_FILTER_ABSOLUTE_START));
        if (kind != 3) {
            CHECK(r->location.start_group == (has_largest ? (kind == 1 ? 1u : 2u) : 0u));
            CHECK(r->location.start_object == (has_largest && kind == 1 ? 3u : 0u));
        }
    }
}

static void test_snapshot(bool publication, bool defer, bool has_largest, unsigned kind)
{
    pair_t p;
    cases++;
    REQUIRE(open_pair(&p, publication, defer, MOQ_VERSION_DRAFT_22));
    if (has_largest) REQUIRE(note(&p, 1, 2) == MOQ_OK);
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = kind == 1 ? MOQ_LOCATION_FILTER_NEXT_OBJECT :
                    kind == 2 ? MOQ_LOCATION_FILTER_RELATIVE_GROUP : MOQ_LOCATION_FILTER_NONE;
    p.observe_ack = true;
    REQUIRE(send_update(&p, kind ? &location : NULL) == MOQ_OK);
    REQUIRE(pump(&p));
    uint64_t id = 0;
    moq_update_resolution_t before;
    if (defer) {
        REQUIRE(owed(&p, &id) == MOQ_OK);
        REQUIRE(query(&p, id, &before, sizeof(before)) == MOQ_OK);
        expect_resolution(&before, id, has_largest, kind);
        uint8_t untouched[sizeof(before)];
        memset(&before, 0xa5, sizeof(before));
        memcpy(untouched, &before, sizeof(before));
        CHECK(query(&p, id, &before, sizeof(before) - 1) == MOQ_ERR_INVAL);
        CHECK(memcmp(&before, untouched, sizeof(before)) == 0);
        CHECK(query(&p, id + 2, &before, sizeof(before)) == MOQ_ERR_STALE_HANDLE);
        CHECK(memcmp(&before, untouched, sizeof(before)) == 0);
    }
    REQUIRE(note(&p, 3, 0) == MOQ_OK);
    moq_event_t ev;
    REQUIRE(take(p.publisher, publication ? MOQ_EVENT_PUBLISH_UPDATED :
                                          MOQ_EVENT_SUBSCRIBE_UPDATED, &ev));
    const moq_subscription_update_options_t *options = publication
        ? ev.u.publish_updated.options : ev.u.subscribe_updated.options;
    REQUIRE(options != NULL && options->struct_size >=
        offsetof(moq_subscription_update_options_t, resolution) + sizeof(before));
    moq_update_resolution_t observed = options->resolution;
    id = options->initiating_request_id;
    expect_resolution(&observed, id, has_largest, kind);
    moq_event_cleanup(&ev);
    if (defer) {
        struct { moq_update_resolution_t value; uint8_t future[16]; } extended;
        memset(&extended, 0x5a, sizeof(extended));
        REQUIRE(query(&p, id, &extended.value, sizeof(extended)) == MOQ_OK);
        CHECK(memcmp(&extended.value, &observed, sizeof(observed)) == 0);
        for (size_t i = 0; i < sizeof(extended.future); i++) CHECK(extended.future[i] == 0x5a);
        REQUIRE(accept(&p, id) == MOQ_OK);
        REQUIRE(pump(&p));
        CHECK(query(&p, id, &before, sizeof(before)) == MOQ_ERR_WRONG_STATE);
    }
    static const uint8_t with_largest[] = {7, 0, 4, 1, 9, 1, 2};
    static const uint8_t without_largest[] = {7, 0, 1, 0};
    const uint8_t *expected = has_largest ? with_largest : without_largest;
    size_t length = has_largest ? sizeof(with_largest) : sizeof(without_largest);
    CHECK(!p.ack_overflow && p.ack_count == 1);
    CHECK(p.ack_len == length && memcmp(p.ack, expected, length) == 0);
done:
    close_pair(&p);
}

static void test_full_width(bool publication, unsigned kind)
{
    pair_t p;
    cases++;
    REQUIRE(open_pair(&p, publication, true, MOQ_VERSION_DRAFT_22));
    REQUIRE(note(&p, UINT64_MAX, UINT64_MAX) == MOQ_OK);
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = kind == 0 ? MOQ_LOCATION_FILTER_NEXT_OBJECT :
                    kind == 3 ? MOQ_LOCATION_FILTER_OBJECT_END :
                                MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    if (kind == 2) location.relative_group_offset = 1;
    if (kind == 3) {
        location.start_group = location.end_group = UINT64_MAX;
        location.start_object = 7;
        location.end_object = 6;
    }
    p.observe_ack = true;
    REQUIRE(send_update(&p, &location) == MOQ_OK);
    REQUIRE(pump(&p));
    uint64_t id;
    REQUIRE(owed(&p, &id) == MOQ_OK);
    moq_update_resolution_t value;
    REQUIRE(query(&p, id, &value, sizeof(value)) == MOQ_OK);
    CHECK(value.flags == (MOQ_UPDATE_RESOLUTION_HAS_LARGEST |
                          MOQ_UPDATE_RESOLUTION_LOCATION_REPLACEMENT));
    CHECK(value.largest_group == UINT64_MAX && value.largest_object == UINT64_MAX);
    if (kind == 0 || kind == 3) {
        CHECK(value.location.kind == MOQ_LOCATION_FILTER_OBJECT_END);
        CHECK(value.location.start_group == value.location.end_group);
        CHECK(value.location.start_object > value.location.end_object);
    } else {
        CHECK(value.location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START);
        CHECK(value.location.start_group == UINT64_MAX && value.location.start_object == 0);
    }
    REQUIRE(accept(&p, id) == MOQ_OK);
    REQUIRE(pump(&p));
    static const uint8_t full_ack[] = {
        7, 0, 20, 1, 9,
        255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255
    };
    CHECK(p.ack_count == 1 && !p.ack_overflow);
    CHECK(p.ack_len == sizeof(full_ack) && memcmp(p.ack, full_ack, sizeof(full_ack)) == 0);
done:
    close_pair(&p);
}

static void test_successive(bool publication, bool defer)
{
    pair_t p;
    cases++;
    REQUIRE(open_pair(&p, publication, defer, MOQ_VERSION_DRAFT_22));
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    REQUIRE(note(&p, 1, 2) == MOQ_OK);
    REQUIRE(send_update(&p, &location) == MOQ_OK);
    REQUIRE(pump(&p));
    uint64_t first_id = 0;
    if (defer) {
        REQUIRE(owed(&p, &first_id) == MOQ_OK);
        REQUIRE(accept(&p, first_id) == MOQ_OK);
        REQUIRE(pump(&p));
    }
    /* The first event remains queued while another legal UPDATE is processed. */
    REQUIRE(note(&p, 3, 0) == MOQ_OK);
    location.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    REQUIRE(send_update(&p, &location) == MOQ_OK);
    REQUIRE(pump(&p));
    if (defer) {
        moq_update_resolution_t value, untouched;
        memset(&value, 0xa5, sizeof(value));
        untouched = value;
        CHECK(query(&p, first_id, &value, sizeof(value)) == MOQ_ERR_STALE_HANDLE);
        CHECK(memcmp(&value, &untouched, sizeof(value)) == 0);
    }
    moq_update_resolution_t values[2];
    for (size_t i = 0; i < 2; i++) {
        moq_event_t ev;
        REQUIRE(take(p.publisher, publication ? MOQ_EVENT_PUBLISH_UPDATED :
                                               MOQ_EVENT_SUBSCRIBE_UPDATED, &ev));
        const moq_subscription_update_options_t *options = publication
            ? ev.u.publish_updated.options : ev.u.subscribe_updated.options;
        REQUIRE(options && options->struct_size >= sizeof(*options));
        values[i] = options->resolution;
        CHECK(moq_session_borrow_valid(p.publisher, ev.borrow_epoch));
        moq_event_cleanup(&ev);
    }
    CHECK(values[0].update_request_id != values[1].update_request_id);
    CHECK(values[0].largest_group == 1 && values[0].largest_object == 2);
    CHECK(values[0].location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START &&
          values[0].location.start_group == 1 && values[0].location.start_object == 3);
    CHECK(values[1].largest_group == 3 && values[1].largest_object == 0);
    CHECK(values[1].location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START &&
          values[1].location.start_group == 4 && values[1].location.start_object == 0);
done:
    close_pair(&p);
}

int main(void)
{
    for (unsigned publication = 0; publication < 2; publication++)
        for (unsigned defer = 0; defer < 2; defer++)
            for (unsigned largest = 0; largest < 2; largest++)
                for (unsigned kind = 0; kind < 4; kind++)
                    test_snapshot(publication != 0, defer != 0, largest != 0, kind);
    for (unsigned publication = 0; publication < 2; publication++)
        for (unsigned kind = 0; kind < 4; kind++)
            test_full_width(publication != 0, kind);
    for (unsigned publication = 0; publication < 2; publication++)
        for (unsigned defer = 0; defer < 2; defer++)
            test_successive(publication != 0, defer != 0);
    printf("update_resolution: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
#endif
