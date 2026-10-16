/* Legal local requests and ACKs; private reads prove saved-window semantics,
 * not delivered-media behavior at full-width locations. */
#include <moq/session.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "../../core/src/session/session_internal.h"
#include <stdio.h>
#include <string.h>

static unsigned failures, checks, cases;
static const char *case_name;
static bool check(bool ok, const char *expression, int line)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL %s:%d: %s\n", case_name, line, expression);
        failures++;
    }
    return ok;
}
#define CHECK(expr) check((expr), #expr, __LINE__)
#define REQUIRE(expr) do { if (!CHECK(expr)) return false; } while (0)
#define NEED(expr) do { if (!CHECK(expr)) goto done; } while (0)

typedef enum phase { INITIAL, UPDATE, NOTIFY } phase_t;
typedef struct fixture {
    test_alloc_state_t memory;
    moq_alloc_t alloc;
    moq_simpair_t *pair;
    moq_session_t *publisher, *subscriber;
    moq_subscription_t ps, ss;
    moq_publication_t pp, sp;
    moq_version_t version;
    bool publication, capture;
    uint8_t ack[64];
    size_t ack_len, ack_count;
} fixture_t;

static uint64_t now(fixture_t *f) { return moq_simpair_now_us(f->pair); }
static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (!f->capture || r->kind != MOQ_SIM_TRACE_ACTION ||
        r->from != MOQ_PERSPECTIVE_SERVER ||
        r->action_kind != MOQ_ACTION_SEND_BIDI_STREAM ||
        !r->detail_bytes.len || r->detail_bytes.data[0] != 7) return;
    f->ack_count++;
    if (!CHECK(r->detail_bytes.len <= sizeof(f->ack))) return;
    f->ack_len = r->detail_bytes.len;
    memcpy(f->ack, r->detail_bytes.data, f->ack_len);
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

static bool start(fixture_t *f, moq_version_t version, bool publication,
                  bool has_largest, uint64_t group, uint64_t object)
{
    memset(f, 0, sizeof(*f));
    f->version = version;
    f->publication = publication;
    f->alloc = test_allocator(&f->memory);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &f->alloc;
    cfg.version = version;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    cfg.trace_fn = trace;
    cfg.trace_ctx = f;
    REQUIRE(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK && pump(f));
    f->publisher = moq_simpair_server(f->pair);
    f->subscriber = moq_simpair_client(f->pair);
    REQUIRE(drain(f->publisher) && drain(f->subscriber));
    if (has_largest) {
        moq_bytes_t part = MOQ_BYTES_LITERAL("local-relative");
        moq_namespace_t ns = {&part, 1};
        REQUIRE(moq_session_note_object_published(f->publisher, &ns,
            MOQ_BYTES_LITERAL("v"), group, object) == MOQ_OK);
    }
    return true;
}
static void finish(fixture_t *f)
{
    if (f->pair) moq_simpair_destroy(f->pair);
    CHECK(f->memory.balance == 0);
}

static bool establish(fixture_t *f, const moq_location_filter_cfg_t *location,
                      bool raw_next)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("local-relative");
    moq_namespace_t ns = {&part, 1};
    moq_event_t ev;
    if (f->publication) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.location = location;
        REQUIRE(moq_session_publish(f->publisher, &cfg, now(f), &f->pp) == MOQ_OK);
        REQUIRE(pump(f) && take(f->subscriber, MOQ_EVENT_PUBLISH_REQUEST, &ev));
        f->sp = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init_sized(&ac, sizeof(ac));
        if (raw_next) ac.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        REQUIRE(moq_session_accept_publish(f->subscriber, f->sp, &ac, now(f)) == MOQ_OK);
    } else {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.filter = raw_next ? MOQ_SUBSCRIBE_FILTER_NEXT_GROUP : MOQ_SUBSCRIBE_FILTER_NONE;
        /* These fields are ignored for the legacy NEXT_GROUP spelling. */
        cfg.start_group = 123;
        cfg.start_object = 77;
        cfg.end_group = 99;
        cfg.location = location;
        REQUIRE(moq_session_subscribe(f->subscriber, &cfg, now(f), &f->ss) == MOQ_OK);
        REQUIRE(pump(f) && take(f->publisher, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
        f->ps = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        REQUIRE(moq_session_accept_subscribe(f->publisher, f->ps, &ac, now(f)) == MOQ_OK);
    }
    REQUIRE(pump(f) && drain(f->publisher) && drain(f->subscriber));
    return true;
}

static moq_result_t update(fixture_t *f, const moq_location_filter_cfg_t *location,
                           bool raw_next, const moq_fill_cfg_t *fill)
{
    if (f->publication) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.location = location;
        cfg.fill = fill;
        return moq_session_update_publication(f->subscriber, f->sp, &cfg, now(f));
    }
    moq_subscription_update_cfg_t cfg;
    moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.location = location;
    cfg.fill = fill;
    cfg.has_filter = raw_next;
    cfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
    cfg.start_group = 123;
    cfg.start_object = 77;
    cfg.end_group = 99;
    return moq_session_update_subscription(f->subscriber, f->ss, &cfg, now(f));
}

static size_t known_handle_slot(const fixture_t *f, bool publisher)
{
    const moq_session_t *s = publisher ? f->publisher : f->subscriber;
    if (!s) return SIZE_MAX;
    if (f->publication) {
        uint64_t handle = publisher ? f->pp._opaque : f->sp._opaque;
        if (!handle || !s->publishes) return SIZE_MAX;
        for (size_t i = 0; i < s->pub_cap; i++) {
            const moq_pub_entry_t *entry = &s->publishes[i];
            if (entry->state != MOQ_PUB_FREE && entry->handle._opaque == handle)
                return i;
        }
    } else {
        uint64_t handle = publisher ? f->ps._opaque : f->ss._opaque;
        if (!handle || !s->subs) return SIZE_MAX;
        for (size_t i = 0; i < s->sub_cap; i++) {
            const moq_sub_entry_t *entry = &s->subs[i];
            if (entry->state != MOQ_SUB_FREE && entry->handle._opaque == handle)
                return i;
        }
    }
    return SIZE_MAX;
}

static void window_check(fixture_t *f, bool publisher, bool empty, uint64_t start)
{
    const moq_session_t *s = publisher ? f->publisher : f->subscriber;
    size_t slot = known_handle_slot(f, publisher);
    if (!CHECK(slot != SIZE_MAX)) return;
    const moq_resolved_window_t *w = f->publication
        ? &s->publishes[slot].window : &s->subs[slot].window;
    CHECK(w->has_window && w->unsatisfiable == empty);
    if (!empty) {
        CHECK(w->start_group == start && w->start_object == 0);
        CHECK(!w->has_end && !w->has_end_object);
    }
}

static void filter_check(fixture_t *f, bool publisher, uint64_t offset)
{
    moq_session_t *s = publisher ? f->publisher : f->subscriber;
    moq_location_filter_cfg_t cfg;
    moq_result_t rc = f->publication
        ? moq_session_publication_filter(s, publisher ? f->pp : f->sp, &cfg)
        : moq_session_subscription_filter(s, publisher ? f->ps : f->ss, &cfg);
    if (!CHECK(rc == MOQ_OK)) return;
    CHECK(cfg.kind == MOQ_LOCATION_FILTER_RELATIVE_GROUP);
    CHECK(cfg.relative_group_offset == offset);
    size_t slot = known_handle_slot(f, publisher);
    if (!CHECK(slot != SIZE_MAX)) return;
    uint32_t stored = f->publication ? s->publishes[slot].filter_type : s->subs[slot].filter_type;
    CHECK(stored == (f->version == MOQ_VERSION_DRAFT_22
        ? MOQ_SUBSCRIBE_FILTER_RELATIVE_GROUP_INTERNAL : MOQ_SUBSCRIBE_FILTER_NEXT_GROUP));
}

static void literal_ack_check(fixture_t *f, bool has_largest, uint64_t group)
{
    static const uint8_t full[] = {7, 0, 20, 1, 9,
        255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255};
    static const uint8_t ordinary[] = {7, 0, 4, 1, 9, 5, 2};
    static const uint8_t absent[] = {7, 0, 1, 0};
    const uint8_t *expected = !has_largest ? absent : group == UINT64_MAX ? full : ordinary;
    size_t n = !has_largest ? sizeof(absent) : group == UINT64_MAX ? sizeof(full) : sizeof(ordinary);
    CHECK(f->ack_count == 1 && f->ack_len == n);
    CHECK(memcmp(f->ack, expected, n) == 0);
}

static void test_local(moq_version_t version, bool publication, phase_t phase,
                       bool raw_next, bool has_largest, uint64_t group,
                       uint64_t offset)
{
    fixture_t f;
    uint64_t ceiling = version == MOQ_VERSION_DRAFT_16 ? (UINT64_C(1) << 62) - 1 : UINT64_MAX;
    uint64_t object = group == ceiling ? ceiling : 2;
    NEED(start(&f, version, publication, has_largest, group, object));
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    location.relative_group_offset = offset;
    const moq_location_filter_cfg_t *typed = raw_next ? NULL : &location;
    NEED(establish(&f, phase == INITIAL ? typed : NULL, phase == INITIAL && raw_next));
    if (phase == UPDATE) {
        f.capture = version == MOQ_VERSION_DRAFT_22;
        NEED(update(&f, typed, raw_next, NULL) == MOQ_OK && pump(&f));
        moq_event_t ev;
        NEED(take(f.subscriber, publication ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                  MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &ev));
        bool seen = publication ? ev.u.publication_update_ok.has_largest :
                                  ev.u.subscription_update_ok.has_largest;
        CHECK(seen == has_largest);
        moq_event_cleanup(&ev);
        if (version == MOQ_VERSION_DRAFT_22) literal_ack_check(&f, has_largest, group);
    } else if (phase == NOTIFY) {
        if (version == MOQ_VERSION_DRAFT_22) {
            NEED(update(&f, &location, false, NULL) == MOQ_OK && pump(&f));
            moq_event_t ack;
            NEED(take(f.subscriber, publication ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                      MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &ack));
            bool seen = publication ? ack.u.publication_update_ok.has_largest :
                                      ack.u.subscription_update_ok.has_largest;
            CHECK(seen == has_largest);
            moq_event_cleanup(&ack);
            NEED(drain(f.publisher) && drain(f.subscriber));
        }
        moq_publish_state_cfg_t cfg;
        moq_publish_state_cfg_init(&cfg);
        cfg.filter = &location;
        moq_result_t rc = publication
            ? moq_session_notify_publication_state(f.publisher, f.pp, &cfg, now(&f))
            : moq_session_notify_subscription_state(f.publisher, f.ps, &cfg, now(&f));
        if (version != MOQ_VERSION_DRAFT_22) {
            CHECK(rc == MOQ_ERR_UNSUPPORTED);
            goto done;
        }
        NEED(rc == MOQ_OK && pump(&f));
        moq_event_t ev;
        NEED(take(f.subscriber, MOQ_EVENT_PUBLISH_STATE, &ev));
        CHECK(ev.u.publish_state.has_filter && ev.u.publish_state.has_largest == has_largest);
        moq_event_cleanup(&ev);
    }
    bool empty = version != MOQ_VERSION_DRAFT_22 && has_largest && group == ceiling;
    uint64_t expected = !has_largest ? 0 : offset == 0
        ? (group == UINT64_MAX ? UINT64_MAX : group + 1)
        : offset - 1 > group ? 0 : group - (offset - 1);
    window_check(&f, true, empty, expected);
    /* UPDATE ACKs (including the one authorizing NOTIFY) and initial
     * SUBSCRIBE have stored requester windows. */
    if (phase == UPDATE || phase == NOTIFY || (!publication && phase == INITIAL))
        window_check(&f, false, empty, expected);
    filter_check(&f, true, offset);
    filter_check(&f, false, offset);
    CHECK(moq_session_state(f.publisher) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(f.subscriber) == MOQ_SESS_ESTABLISHED);
done:
    finish(&f);
}

static void test_explicit_fill(bool publication)
{
    fixture_t f;
    NEED(start(&f, MOQ_VERSION_DRAFT_22, publication, true, UINT64_MAX, UINT64_MAX));
    NEED(establish(&f, NULL, false));
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &location;
    f.capture = true;
    NEED(update(&f, &location, false, &fill) == MOQ_OK && pump(&f));
    literal_ack_check(&f, true, UINT64_MAX);
    moq_event_t ev;
    NEED(take(f.publisher, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_request_event_t request = ev.u.fill_request;
    moq_event_cleanup(&ev);
    CHECK(request.start_group == UINT64_MAX && request.start_object == 0);
    CHECK(request.end_group == UINT64_MAX && request.end_object == UINT64_MAX);
    window_check(&f, true, false, UINT64_MAX);
    window_check(&f, false, false, UINT64_MAX);
done:
    finish(&f);
}

static void test_fetch(moq_version_t version)
{
    fixture_t f;
    uint64_t ceiling = version == MOQ_VERSION_DRAFT_16 ? (UINT64_C(1) << 62) - 1 : UINT64_MAX;
    NEED(start(&f, version, false, true, ceiling, ceiling));
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init_sized(&cfg, sizeof(cfg));
    moq_bytes_t part = MOQ_BYTES_LITERAL("local-relative");
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.location = &location;
    moq_fetch_t fetch;
    moq_result_t rc = moq_session_fetch(f.subscriber, &cfg, now(&f), &fetch);
    if (version != MOQ_VERSION_DRAFT_22) {
        CHECK(rc == MOQ_ERR_UNSUPPORTED);
        goto done;
    }
    NEED(rc == MOQ_OK && pump(&f));
    moq_event_t ev;
    NEED(take(f.publisher, MOQ_EVENT_FETCH_REQUEST, &ev));
    CHECK(ev.u.fetch_request.start_group == UINT64_MAX && ev.u.fetch_request.start_object == 0);
    /* FETCH exposes an exclusive end; zero includes the entire last group. */
    CHECK(ev.u.fetch_request.end_group == UINT64_MAX && ev.u.fetch_request.end_object == 0);
    moq_event_cleanup(&ev);
done:
    finish(&f);
}

int main(void)
{
    unsigned passed = 0;
    char name[128];
    case_name = name;
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_22, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_16};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        bool modern = v == 0;
        uint64_t ceiling = v == 2 ? (UINT64_C(1) << 62) - 1 : UINT64_MAX;
        for (unsigned phase = INITIAL; phase <= NOTIFY; phase++) {
            for (unsigned form = 0; form < 3; form++) {
                bool publication = form == 2;
                bool raw = form == 1 || (!modern && publication && phase == INITIAL);
                if (phase == NOTIFY && form == 1) continue;
                if (!modern && phase == UPDATE && form != 1) continue;
                for (unsigned basis = 0; basis < (modern ? 3u : 2u); basis++) {
                    uint64_t group = basis == 0 ? 5 : ceiling;
                    bool has_largest = basis != 2;
                    snprintf(name, sizeof(name), "d%u_%s_%s_%s_basis%u", v == 0 ? 22 : v == 1 ? 18 : 16,
                        publication ? "publish" : "subscribe", phase == INITIAL ? "initial" :
                        phase == UPDATE ? "update" : "notify", raw ? "raw" : "typed", basis);
                    unsigned before = failures;
                    test_local(versions[v], publication, (phase_t)phase, raw, has_largest, group, 0);
                    cases++;
                    if (failures == before) { passed++; printf("PASS %s\n", name); }
                }
            }
        }
        snprintf(name, sizeof(name), "d%u_fetch_relative_zero", v == 0 ? 22 : v == 1 ? 18 : 16);
        unsigned before = failures;
        test_fetch(versions[v]);
        cases++;
        if (failures == before) { passed++; printf("PASS %s\n", name); }
    }
    for (unsigned publication = 0; publication < 2; publication++) {
        snprintf(name, sizeof(name), "%s_explicit_fill", publication ? "publish" : "subscribe");
        unsigned before = failures;
        test_explicit_fill(publication != 0);
        cases++;
        if (failures == before) { passed++; printf("PASS %s\n", name); }
        for (unsigned offset = 1; offset <= 2; offset++) {
            snprintf(name, sizeof(name), "%s_relative_%u", publication ? "publish" : "subscribe", offset);
            before = failures;
            test_local(MOQ_VERSION_DRAFT_22, publication != 0, INITIAL, false, true,
                offset == 1 ? UINT64_MAX : 0, offset);
            cases++;
            if (failures == before) { passed++; printf("PASS %s\n", name); }
        }
    }
    printf("%u/%u cases passed; %u checks; %u failures\n", passed, cases, checks, failures);
    return failures ? 1 : 0;
}
