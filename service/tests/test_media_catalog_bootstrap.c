/* D22 catalog readiness is a transaction, not receipt of the first object. */
#include <moq/media_receiver.h>
#include <moq/sim.h>
#include "test_alloc_support.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void moq_media_receiver_test_set_alloc(const moq_alloc_t *);
moq_endpoint_t *moq_endpoint_test_make_bare(void);
void moq_endpoint_test_free_bare(moq_endpoint_t *);
void moq_endpoint_test_pump_cycle(moq_endpoint_t *, moq_session_t *, uint64_t);
void moq_endpoint_test_drain_posted(moq_endpoint_t *, moq_session_t *, uint64_t);
size_t moq_endpoint_test_task_count(moq_endpoint_t *);

static unsigned failures;
static const char *case_name;
static bool check(bool value, const char *expr, int line)
{
    if (!value) {
        fprintf(stderr, "FAIL %s:%d: %s\n", case_name, line, expr);
        failures++;
    }
    return value;
}
#define CHECK(e) check((e), #e, __LINE__)
#define REQUIRE(e) do { if (!CHECK(e)) return false; } while (0)
#define STEP(e) do { if (!(e)) goto done; } while (0)

static const char base[] = "{\"version\":1,\"generatedAt\":1,\"tracks\":["
    "{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true,\"role\":\"video\",\"codec\":\"av01\"}]}";
static const char delta[] = "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":["
    "{\"name\":\"a\",\"packaging\":\"loc\",\"isLive\":true,\"role\":\"audio\",\"codec\":\"opus\"}]}]}";
static const char live_delta[] = "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":["
    "{\"name\":\"v2\",\"packaging\":\"loc\",\"isLive\":true,\"role\":\"video\",\"codec\":\"av01\"}]}]}";

typedef struct fixture {
    test_alloc_state_t allocations;
    moq_alloc_t alloc;
    moq_simpair_t *pair;
    moq_endpoint_t *endpoint;
    moq_media_receiver_t *receiver;
    moq_subscription_t subscription;
    moq_fill_t fill;
    unsigned ready, added;
} fixture_t;

static moq_session_t *peer(fixture_t *f) { return moq_simpair_server(f->pair); }
static moq_session_t *client(fixture_t *f) { return moq_simpair_client(f->pair); }
static uint64_t now(fixture_t *f) { return moq_simpair_now_us(f->pair); }

static bool network(fixture_t *f)
{
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 64, NULL) == MOQ_OK);
    return true;
}

static bool pump(fixture_t *f)
{
    REQUIRE(network(f));
    moq_endpoint_test_pump_cycle(f->endpoint, client(f), now(f));
    return true;
}

static void observations(fixture_t *f)
{
    moq_media_track_event_t event;
    for (unsigned n = 0; n < 64; n++) {
        moq_result_t rc = moq_media_receiver_poll_track(f->receiver, &event, sizeof(event));
        if (rc != MOQ_OK) return;
        if (event.kind == MOQ_MEDIA_CATALOG_READY) f->ready++;
        if (event.kind == MOQ_MEDIA_TRACK_ADDED) f->added++;
    }
    CHECK(false);
}

static bool start_limits(fixture_t *f, bool has_largest, uint64_t last_object,
                         uint32_t object_cap, uint64_t byte_cap)
{
    memset(f, 0, sizeof(*f));
    f->alloc = test_allocator(&f->allocations);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &f->alloc;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.seed = 37;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    REQUIRE(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK);
    REQUIRE(network(f));
    moq_event_t event;
    while (moq_session_poll_events(peer(f), &event, 1)) moq_event_cleanup(&event);
    while (moq_session_poll_events(client(f), &event, 1)) moq_event_cleanup(&event);

    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live_sized(&rcfg, sizeof(rcfg));
    const moq_bytes_t part = MOQ_BYTES_LITERAL("bootstrap");
    rcfg.namespace_ = (moq_namespace_t){ &part, 1 };
    rcfg.auto_subscribe = false;
    rcfg.overflow.max_objects = object_cap;
    rcfg.overflow.max_bytes = byte_cap;
    rcfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
    f->endpoint = moq_endpoint_test_make_bare();
    REQUIRE(f->endpoint != NULL);
    moq_media_receiver_test_set_alloc(&f->alloc);
    moq_result_t rc = moq_media_receiver_attach(f->endpoint, &rcfg, &f->receiver);
    moq_media_receiver_test_set_alloc(NULL);
    REQUIRE(rc == MOQ_OK && f->receiver != NULL);
    moq_endpoint_test_pump_cycle(f->endpoint, client(f), now(f));
    REQUIRE(network(f));
    bool subscribed = false;
    for (unsigned n = 0; n < 64; n++) {
        if (!moq_session_poll_events(peer(f), &event, 1)) break;
        if (event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            f->subscription = event.u.subscribe_request.sub;
            subscribed = true;
        }
        moq_event_cleanup(&event);
    }
    REQUIRE(subscribed);
    moq_accept_subscribe_cfg_t accept;
    moq_accept_subscribe_cfg_init(&accept);
    accept.has_largest = has_largest;
    accept.largest_group = 7;
    accept.largest_object = last_object;
    REQUIRE(moq_session_accept_subscribe(peer(f), f->subscription, &accept, now(f)) == MOQ_OK);
    for (unsigned n = 0; n < 64; n++) {
        if (!moq_session_poll_events(peer(f), &event, 1)) break;
        if (event.kind == MOQ_EVENT_FILL_REQUEST) f->fill = event.u.fill_request.fill;
        moq_event_cleanup(&event);
    }
    REQUIRE(!has_largest || moq_fill_is_valid(f->fill));
    REQUIRE(pump(f));
    observations(f);
    REQUIRE(f->ready == 0 && f->added == 0);
    REQUIRE(!moq_media_receiver_is_fatal(f->receiver));
    return true;
}

static bool start(fixture_t *f, bool has_largest, uint64_t last_object)
{
    return start_limits(f, has_largest, last_object, 0, 0);
}

static bool write_fill(fixture_t *f, uint64_t object, const char *json)
{
    moq_fetch_object_cfg_t cfg;
    moq_fetch_object_cfg_init(&cfg);
    cfg.group_id = 7;
    cfg.subgroup_id = 0;
    cfg.object_id = object;
    REQUIRE(moq_rcbuf_create(&f->alloc, (const uint8_t *)json, strlen(json), &cfg.payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_fill_object(peer(f), f->fill, &cfg, now(f));
    moq_rcbuf_decref(cfg.payload);
    REQUIRE(rc == MOQ_OK);
    return pump(f);
}

static bool write_live_ex(fixture_t *f, uint64_t group, uint64_t object,
                          const char *json, bool end_of_group)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.subgroup_id = 0;
    moq_subgroup_handle_t stream;
    REQUIRE(moq_session_open_subgroup(peer(f), f->subscription, &cfg, now(f), &stream) == MOQ_OK);
    moq_object_cfg_t obj;
    moq_object_cfg_init(&obj);
    obj.object_id = object;
    REQUIRE(moq_rcbuf_create(&f->alloc, (const uint8_t *)json, strlen(json), &obj.payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_object_ex(peer(f), stream, &obj, now(f));
    moq_rcbuf_decref(obj.payload);
    REQUIRE(rc == MOQ_OK);
    if (end_of_group)
        REQUIRE(moq_session_write_status_object(peer(f), stream, object + 1,
            MOQ_OBJECT_END_OF_GROUP, now(f)) == MOQ_OK);
    REQUIRE(moq_session_close_subgroup(peer(f), stream, now(f)) == MOQ_OK);
    return pump(f);
}

static bool write_live(fixture_t *f, uint64_t group, uint64_t object, const char *json)
{
    return write_live_ex(f, group, object, json, false);
}

static void finish(fixture_t *f)
{
    if (f->receiver) moq_media_receiver_destroy(f->receiver);
    if (f->endpoint) {
        for (unsigned i = 0; f->pair && i < 16 && moq_endpoint_test_task_count(f->endpoint); i++) {
            moq_endpoint_test_drain_posted(f->endpoint, client(f), now(f));
            (void)network(f);
        }
        CHECK(moq_endpoint_test_task_count(f->endpoint) == 0);
        moq_endpoint_test_free_bare(f->endpoint);
    }
    if (f->pair) moq_simpair_destroy(f->pair);
    CHECK(f->allocations.balance == 0);
}

static void completed_fill(bool early_live)
{
    fixture_t f;
    case_name = early_live ? "early_live_delta" : "complete_before_ready";
    STEP(start(&f, true, 1));
    if (early_live) {
        STEP(write_live(&f, 7, 2, live_delta));
        CHECK(!moq_media_receiver_is_fatal(f.receiver));
        moq_media_object_t output;
        moq_result_t rc = moq_media_receiver_poll_object(f.receiver, &output, sizeof(output));
        CHECK(rc == MOQ_DONE);
        if (rc == MOQ_OK) moq_media_object_cleanup(&output);
    }
    STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(write_fill(&f, 0, base));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    STEP(write_fill(&f, 1, delta));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    STEP(CHECK(moq_session_end_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(pump(&f));
    observations(&f);
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
    CHECK(f.ready == 1 && f.added == (early_live ? 3u : 2u));
    STEP(pump(&f));
    observations(&f);
    CHECK(f.ready == 1);
done:
    finish(&f);
}

static void failed_fill(bool staged_generation)
{
    fixture_t f;
    case_name = staged_generation ? "newer_live_cannot_certify_retained" : "failed_retained_no_live_rescue";
    STEP(start(&f, true, 1));
    STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(write_fill(&f, 0, base));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    if (staged_generation) {
        STEP(write_live(&f, 8, 0, base));
        observations(&f);
        CHECK(f.ready == 0 && f.added == 0);
    }
    STEP(CHECK(moq_session_abort_fill(peer(&f), f.fill, 6, now(&f)) == MOQ_OK));
    STEP(pump(&f));
    observations(&f);
    CHECK(moq_media_receiver_is_fatal(f.receiver));
    CHECK(moq_media_receiver_fatal_code(f.receiver) == MOQ_MEDIA_RECEIVER_FATAL_CATALOG_UNUSABLE);
    CHECK(moq_session_state(client(&f)) == MOQ_SESS_ESTABLISHED);
    STEP(write_live(&f, 8, 0, base));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    CHECK(moq_media_receiver_is_fatal(f.receiver));
done:
    finish(&f);
}

static void initially_empty(void)
{
    fixture_t f;
    case_name = "initially_empty_live_control";
    STEP(start(&f, false, 0));
    CHECK(!moq_fill_is_valid(f.fill));
    STEP(write_live(&f, 0, 0, base));
    observations(&f);
    CHECK(f.ready == 1 && f.added == 1);
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
done:
    finish(&f);
}

static void incomplete_fill(bool gap)
{
    fixture_t f;
    case_name = gap ? "required_gap_refuses_ready" : "incomplete_fill_refuses_ready";
    STEP(start(&f, true, 1));
    STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(write_fill(&f, 0, base));
    if (gap)
        STEP(CHECK(moq_session_write_fill_range(peer(&f), f.fill,
            MOQ_FETCH_RANGE_UNKNOWN, 7, 1, now(&f)) == MOQ_OK));
    STEP(CHECK(moq_session_end_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(pump(&f));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    CHECK(moq_media_receiver_is_fatal(f.receiver));
    CHECK(moq_media_receiver_fatal_code(f.receiver) == MOQ_MEDIA_RECEIVER_FATAL_CATALOG_UNUSABLE);
    CHECK(moq_session_state(client(&f)) == MOQ_SESS_ESTABLISHED);
done:
    finish(&f);
}

static void empty_catalog(void)
{
    fixture_t f;
    case_name = "valid_empty_catalog_is_ready";
    STEP(start(&f, true, 0));
    STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(write_fill(&f, 0, "{\"version\":1,\"generatedAt\":1,\"tracks\":[]}"));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    STEP(CHECK(moq_session_end_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(pump(&f));
    observations(&f);
    CHECK(f.ready == 1 && f.added == 0);
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
done:
    finish(&f);
}

static void staging_capacity(bool bytes)
{
    fixture_t f;
    case_name = bytes ? "live_staging_byte_bound" : "live_staging_count_bound";
    STEP(start_limits(&f, true, 1, bytes ? 0 : 1, bytes ? strlen(live_delta) - 1 : 0));
    STEP(write_live(&f, 7, 2, live_delta));
    if (!bytes) {
        CHECK(!moq_media_receiver_is_fatal(f.receiver));
        moq_media_receiver_stats_t stats;
        STEP(CHECK(moq_media_receiver_get_stats(f.receiver, &stats, sizeof(stats)) == MOQ_OK));
        CHECK(stats.objects_queued == 0 && stats.bytes_queued == 0);
        STEP(write_live(&f, 8, 0, base));
    }
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    CHECK(moq_media_receiver_is_fatal(f.receiver));
    CHECK(moq_media_receiver_fatal_code(f.receiver) == MOQ_MEDIA_RECEIVER_FATAL_CATALOG_UNUSABLE);
    CHECK(moq_session_state(client(&f)) == MOQ_SESS_ESTABLISHED);
done:
    finish(&f);
}

static void destroy_pending(bool live)
{
    fixture_t f;
    case_name = live ? "destroy_with_catalog_and_live_stage" : "destroy_with_catalog_stage";
    STEP(start(&f, true, 1));
    STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
    STEP(write_fill(&f, 0, base));
    if (live) STEP(write_live(&f, 7, 2, live_delta));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
done:
    finish(&f);
}

static void live_end_of_group(bool pending_fill)
{
    fixture_t f;
    case_name = pending_fill ? "live_end_group_during_pending_fill" : "live_end_group_after_commit";
    /* The live JSON fills both staging limits; its status marker must use neither. */
    STEP(start_limits(&f, true, 0, 1, strlen(base)));
    if (!pending_fill) {
        STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
        STEP(write_fill(&f, 0, base));
        STEP(CHECK(moq_session_end_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
        STEP(pump(&f));
        observations(&f);
        CHECK(f.ready == 1 && f.added == 1);
    }
    STEP(write_live_ex(&f, 8, 0, base, true));
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
    moq_media_receiver_stats_t stats;
    STEP(CHECK(moq_media_receiver_get_stats(f.receiver, &stats, sizeof(stats)) == MOQ_OK));
    CHECK(stats.objects_queued == 0 && stats.bytes_queued == 0);
    CHECK(stats.objects_dropped == 0 && stats.catalog_drops == 0);
    if (pending_fill) {
        observations(&f);
        CHECK(f.ready == 0 && f.added == 0);
        moq_media_object_t object;
        moq_result_t rc = moq_media_receiver_poll_object(f.receiver, &object, sizeof(object));
        CHECK(rc == MOQ_DONE);
        if (rc == MOQ_OK) moq_media_object_cleanup(&object);
        STEP(CHECK(moq_session_open_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
        STEP(write_fill(&f, 0, base));
        observations(&f);
        CHECK(f.ready == 0 && f.added == 0);
        STEP(CHECK(moq_session_end_fill(peer(&f), f.fill, now(&f)) == MOQ_OK));
        STEP(pump(&f));
    }
    observations(&f);
    CHECK(f.ready == 1 && f.added == 1);
    CHECK(!moq_media_receiver_is_fatal(f.receiver));
    CHECK(moq_session_state(client(&f)) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(peer(&f)) == MOQ_SESS_ESTABLISHED);
    STEP(pump(&f));
    observations(&f);
    CHECK(f.ready == 1);
done:
    finish(&f);
}

static void empty_normal_live(void)
{
    fixture_t f;
    case_name = "empty_normal_live_is_unusable";
    STEP(start(&f, true, 0));
    STEP(write_live(&f, 8, 0, ""));
    observations(&f);
    CHECK(f.ready == 0 && f.added == 0);
    CHECK(moq_media_receiver_is_fatal(f.receiver));
    CHECK(moq_media_receiver_fatal_code(f.receiver) == MOQ_MEDIA_RECEIVER_FATAL_CATALOG_UNUSABLE);
    CHECK(moq_session_state(client(&f)) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(peer(&f)) == MOQ_SESS_ESTABLISHED);
    moq_media_receiver_stats_t stats;
    STEP(CHECK(moq_media_receiver_get_stats(f.receiver, &stats, sizeof(stats)) == MOQ_OK));
    CHECK(stats.objects_queued == 0 && stats.bytes_queued == 0);
done:
    finish(&f);
}

int main(void)
{
    completed_fill(false);
    completed_fill(true);
    failed_fill(false);
    failed_fill(true);
    initially_empty();
    incomplete_fill(false);
    incomplete_fill(true);
    empty_catalog();
    staging_capacity(false);
    staging_capacity(true);
    destroy_pending(false);
    destroy_pending(true);
    live_end_of_group(true);
    live_end_of_group(false);
    empty_normal_live();
    printf("catalog bootstrap: 15 cases, %u failures\n", failures);
    return failures ? 1 : 0;
}
