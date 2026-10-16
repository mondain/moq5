#include "publisher_track_layout_asserts.h"
#include "publisher_track_old_caller_test.h"
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks, alloc_calls, live, fail_at = SIZE_MAX;
static uint64_t next_group = 10;

int publisher_track_old_check(bool condition, const char *file, int line)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL %s:%d\n", file, line);
    return condition ? 0 : 1;
}
#define CHECK(c) (failures += publisher_track_old_check((c), __FILE__, __LINE__))

static void *allocate(size_t size, void *ctx)
{
    (void)ctx;
    if (++alloc_calls == fail_at) return NULL;
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
static void *resize(void *p, size_t old_size, size_t size, void *ctx)
{
    (void)old_size;
    if (!p) return allocate(size, ctx);
    if (++alloc_calls == fail_at) return NULL;
    return realloc(p, size);
}
static void release(void *p, size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    if (p) --live;
    free(p);
}

static void discard_events(moq_session_t *session)
{
    moq_event_t event;
    while (moq_session_poll_events(session, &event, 1)) moq_event_cleanup(&event);
}

static int open_pair(publisher_track_old_context_t *ctx, const moq_pub_cfg_t *pcfg,
                     moq_version_t version)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.seed = 42;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    CHECK(moq_simpair_create(&cfg, &ctx->pair) == MOQ_OK);
    if (!ctx->pair) return failures;
    CHECK(moq_simpair_start(ctx->pair) == MOQ_OK);
    CHECK(moq_simpair_run_until_quiescent(ctx->pair, 8, NULL) == MOQ_OK);
    discard_events(moq_simpair_client(ctx->pair));
    discard_events(moq_simpair_server(ctx->pair));
    CHECK(moq_pub_create(moq_simpair_server(ctx->pair), ctx->alloc, pcfg, &ctx->pub) == MOQ_OK);
    return failures;
}

static int close_pair(publisher_track_old_context_t *ctx)
{
    int failures = 0;
    moq_pub_destroy(ctx->pub);
    ctx->pub = NULL;
    moq_simpair_destroy(ctx->pair);
    ctx->pair = NULL;
    CHECK(live == 0);
    return failures;
}

static int settle(publisher_track_old_context_t *ctx)
{
    int failures = 0;
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(moq_simpair_run_until_quiescent(ctx->pair, 8, NULL) == MOQ_OK);
        moq_result_t rc = moq_pub_tick(ctx->pub, moq_simpair_now_us(ctx->pair));
        CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    }
    CHECK(moq_simpair_run_until_quiescent(ctx->pair, 8, NULL) == MOQ_OK);
    discard_events(moq_simpair_client(ctx->pair));
    return failures;
}

int publisher_track_old_behavior(publisher_track_old_context_t *ctx,
    moq_pub_track_t *track, uint64_t budget, uint8_t priority, bool monotonic)
{
    int failures = 0;
    uint64_t group = next_group;
    next_group += 10;
    uint64_t now = moq_simpair_now_us(ctx->pair);
    uint8_t *data = malloc((size_t)budget + 1);
    CHECK(data != NULL);
    if (!data) return failures;
    memset(data, 0x37, (size_t)budget + 1);
    for (unsigned over = 0; over < 2; ++over) {
        moq_rcbuf_t *payload = NULL;
        CHECK(moq_rcbuf_create(ctx->alloc, data, (size_t)budget + over, &payload) == MOQ_OK);
        if (!payload) continue;
        moq_pub_retained_object_t object = {
            .object_id = 0, .payload = payload, .end_of_group = true
        };
        moq_pub_retained_group_cfg_t retained;
        moq_pub_retained_group_cfg_init(&retained);
        retained.group_id = group;
        retained.objects = &object;
        retained.object_count = 1;
        CHECK(moq_pub_set_retained_group(ctx->pub, track, &retained) ==
              (over ? MOQ_ERR_INVAL : MOQ_OK));
        moq_rcbuf_decref(payload);
    }
    free(data);
    CHECK(moq_pub_clear_retained_group(ctx->pub, track) == MOQ_OK);

    uint8_t byte = 1;
    moq_rcbuf_t *payload = NULL;
    CHECK(moq_rcbuf_create(ctx->alloc, &byte, 1, &payload) == MOQ_OK);
    if (!payload) return failures;
    moq_pub_object_cfg_t object;
    moq_pub_object_cfg_init_sized(&object, sizeof(object));
    object.payload = payload;
    object.datagram = true;
    object.group_id = group + 3;
    CHECK(moq_pub_write_object_ex(ctx->pub, track, &object, now) == MOQ_OK);
    object.group_id = group + 2;
    CHECK(moq_pub_write_object_ex(ctx->pub, track, &object, now) ==
          (monotonic ? MOQ_ERR_WRONG_STATE : MOQ_OK));

    moq_bytes_t ns = MOQ_BYTES_LITERAL("track-compat");
    moq_subscribe_cfg_t subscribe;
    moq_subscribe_cfg_init(&subscribe);
    subscribe.track_namespace = (moq_namespace_t){&ns, 1};
    subscribe.track_name = MOQ_BYTES_LITERAL("video");
    moq_subscription_t sub = MOQ_SUBSCRIPTION_INVALID;
    CHECK(moq_session_subscribe(moq_simpair_client(ctx->pair), &subscribe, now, &sub) == MOQ_OK);
    failures += settle(ctx);
    CHECK(moq_pub_write_object(ctx->pub, track, group + 4, 0, payload, now) == MOQ_OK);
    moq_rcbuf_decref(payload);
    moq_action_t action;
    bool found = false;
    while (moq_session_poll_actions(moq_simpair_server(ctx->pair), &action, 1)) {
        if (!found && action.kind == MOQ_ACTION_SEND_DATA && action.u.send_data.header_len > 0) {
            found = true;
            CHECK(action.u.send_data.header[action.u.send_data.header_len - 1] == priority);
        }
        moq_action_cleanup(&action);
    }
    CHECK(found);
    CHECK(moq_session_unsubscribe(moq_simpair_client(ctx->pair), sub, now) == MOQ_OK);
    failures += settle(ctx);
    return failures;
}

static int current_cases(publisher_track_old_context_t *ctx, const moq_pub_cfg_t *pcfg)
{
    int failures = 0;
    const size_t budget_end = offsetof(moq_pub_track_cfg_t, max_retained_bytes) + 8;
    const size_t priority_end = offsetof(moq_pub_track_cfg_t, has_publisher_priority) + 1;
    const size_t monotonic_end = offsetof(moq_pub_track_cfg_t, monotonic_groups) + 1;
    const size_t extents[] = {sizeof(test_pub_track_original_t), budget_end - 1, budget_end,
                             priority_end, monotonic_end - 1, monotonic_end,
                             sizeof(moq_pub_track_cfg_t) + 16};
    for (size_t i = 0; i < sizeof(extents) / sizeof(extents[0]); ++i) {
        failures += open_pair(ctx, pcfg, MOQ_VERSION_DRAFT_18);
        if (!ctx->pub) { failures += close_pair(ctx); continue; }
        struct {
            uint64_t before;
            moq_pub_track_cfg_t cfg;
            uint8_t after[16];
        } storage;
        memset(&storage, 0xa5, sizeof(storage));
        if (i == 0) moq_pub_track_cfg_init(&storage.cfg);
        else moq_pub_track_cfg_init_sized(&storage.cfg, extents[i]);
        const size_t written = extents[i] < sizeof(storage.cfg) ? extents[i] : sizeof(storage.cfg);
        moq_pub_track_cfg_t defaults;
        memset(&defaults, 0, sizeof(defaults));
        defaults.struct_size = (uint32_t)written;
        defaults.publisher_priority = 128;
        CHECK(memcmp(&defaults, &storage.cfg, written) == 0);
        CHECK(storage.cfg.struct_size == written);
        CHECK(storage.cfg.publisher_priority == 128);
        CHECK(storage.before == UINT64_C(0xa5a5a5a5a5a5a5a5));
        for (size_t j = written; j < sizeof(storage.cfg); ++j)
            CHECK(((unsigned char *)&storage.cfg)[j] == 0xa5);
        for (size_t j = 0; j < sizeof(storage.after); ++j) CHECK(storage.after[j] == 0xa5);
        storage.cfg.struct_size = (uint32_t)extents[i];
        moq_bytes_t ns = MOQ_BYTES_LITERAL("track-compat");
        storage.cfg.track_namespace = (moq_namespace_t){&ns, 1};
        storage.cfg.track_name = MOQ_BYTES_LITERAL("video");
        storage.cfg.publisher_priority = 0;
        storage.cfg.max_retained_bytes = 32;
        storage.cfg.has_publisher_priority = true;
        storage.cfg.monotonic_groups = true;
        unsigned char before[sizeof(storage)];
        memcpy(before, &storage, sizeof(before));
        moq_pub_track_t *track = NULL;
        size_t calls = alloc_calls;
        CHECK(moq_pub_add_track(ctx->pub, &storage.cfg, 1000, &track) == MOQ_OK);
        CHECK(track != NULL);
        CHECK(alloc_calls == calls + 5);
        CHECK(memcmp(before, &storage, sizeof(before)) == 0);
        if (track) {
            failures += publisher_track_old_behavior(ctx, track,
                extents[i] >= budget_end ? 32 : 1024u * 1024u,
                extents[i] >= priority_end ? 0 : 150, extents[i] >= monotonic_end);
            CHECK(moq_pub_remove_track(ctx->pub, track, 1000) == MOQ_OK);
        }
        failures += close_pair(ctx);
    }
    return failures;
}

typedef struct fill_probe {
    publisher_track_old_context_t *ctx;
    unsigned calls;
    int failures;
} fill_probe_t;

static moq_pub_fill_decision_t fill_request(void *context, moq_pub_track_t *track,
                                           const moq_pub_fill_request_t *request)
{
    fill_probe_t *probe = context;
    (void)track;
    (void)request;
    probe->calls++;
    probe->ctx->refusal = MOQ_ERR_WRONG_STATE;
    probe->failures += publisher_track_old_original(PUB_TRACK_OLD_REFUSAL, probe->ctx);
    probe->failures += publisher_track_old_pre_monotonic(PUB_TRACK_OLD_REFUSAL, probe->ctx);
    return MOQ_PUB_FILL_DEFER;
}

static int fill_refusal(publisher_track_old_context_t *ctx, moq_pub_cfg_t cfg)
{
    int failures = 0;
    fill_probe_t probe = {.ctx = ctx};
    cfg.on_fill_request = fill_request;
    cfg.on_fill_request_ctx = &probe;
    failures += open_pair(ctx, &cfg, MOQ_VERSION_DRAFT_22);
    if (!ctx->pub) { failures += close_pair(ctx); return failures; }
    moq_bytes_t ns = MOQ_BYTES_LITERAL("track-compat");
    moq_pub_track_cfg_t track_cfg;
    moq_pub_track_cfg_init_sized(&track_cfg, sizeof(track_cfg));
    track_cfg.track_namespace = (moq_namespace_t){&ns, 1};
    track_cfg.track_name = MOQ_BYTES_LITERAL("video");
    moq_pub_track_t *track = NULL;
    CHECK(moq_pub_add_track(ctx->pub, &track_cfg, 1000, &track) == MOQ_OK);
    uint8_t byte = 1;
    moq_rcbuf_t *payload = NULL;
    CHECK(moq_rcbuf_create(ctx->alloc, &byte, 1, &payload) == MOQ_OK);
    moq_pub_retained_object_t object = {
        .object_id = 0, .payload = payload, .end_of_group = true
    };
    moq_pub_retained_group_cfg_t retained;
    moq_pub_retained_group_cfg_init(&retained);
    retained.group_id = 3;
    retained.objects = &object;
    retained.object_count = 1;
    CHECK(moq_pub_set_retained_group(ctx->pub, track, &retained) == MOQ_OK);
    moq_rcbuf_decref(payload);

    moq_location_filter_cfg_t location, history;
    moq_location_filter_cfg_init(&location);
    moq_location_filter_cfg_init(&history);
    location.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    history.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    history.relative_group_offset = 1;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &history;
    moq_subscribe_cfg_t subscribe;
    moq_subscribe_cfg_init_sized(&subscribe, sizeof(subscribe));
    subscribe.track_namespace = track_cfg.track_namespace;
    subscribe.track_name = track_cfg.track_name;
    subscribe.location = &location;
    subscribe.fill = &fill;
    subscribe.forward = true;
    moq_subscription_t sub = MOQ_SUBSCRIPTION_INVALID;
    CHECK(moq_session_subscribe(moq_simpair_client(ctx->pair), &subscribe, 1000, &sub) == MOQ_OK);
    failures += settle(ctx);
    CHECK(probe.calls == 1);
    failures += probe.failures;
    failures += close_pair(ctx);
    return failures;
}

int main(void)
{
    int failures = 0;
    moq_alloc_t alloc = {.ctx = NULL, .alloc = allocate, .realloc = resize, .free = release};
    publisher_track_old_context_t ctx = {
        .alloc = &alloc, .alloc_calls = &alloc_calls, .live = &live, .fail_at = &fail_at
    };
    moq_pub_cfg_t cfg;
    moq_pub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.accept_mode = MOQ_PUB_ACCEPT_ALL;
    cfg.default_publisher_priority = 150;
    int (*const callers[])(unsigned, publisher_track_old_context_t *) = {
        publisher_track_old_original, publisher_track_old_pre_monotonic
    };
    for (size_t i = 0; i < sizeof(callers) / sizeof(callers[0]); ++i) {
        for (unsigned operation = PUB_TRACK_OLD_INIT; operation <= PUB_TRACK_OLD_REFUSAL; ++operation) {
            failures += open_pair(&ctx, &cfg, MOQ_VERSION_DRAFT_18);
            if (ctx.pub) {
                if (operation == PUB_TRACK_OLD_REFUSAL) {
                    moq_event_t event;
                    memset(&event, 0, sizeof(event));
                    event.kind = MOQ_EVENT_SESSION_CLOSED;
                    moq_pub_event_result_t result;
                    CHECK(moq_pub_handle_event(ctx.pub, &event, 1000, &result) == MOQ_OK);
                    ctx.refusal = MOQ_ERR_CLOSED;
                }
                failures += callers[i](operation, &ctx);
            }
            failures += close_pair(&ctx);
        }
    }
    failures += current_cases(&ctx, &cfg);
    failures += fill_refusal(&ctx, cfg);
    CHECK(live == 0 && fail_at == SIZE_MAX);
    printf("publisher track compatibility: %zu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
