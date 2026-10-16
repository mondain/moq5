#include <moq/relay/relay.h>
#include <moq/relay/trace.h>
#ifdef TEST_RUNTIME
#include <moq/relay/moqr_shards.h>
#endif
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "sdk:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

/* Check every rejected prefix and the complete output plus caller-owned tail. */
#define CHECK_STATS(type, getter, object) do { \
    struct { type value; unsigned char tail[23]; } out; \
    unsigned char before[sizeof(out)]; \
    memset(&out, 0xa5, sizeof(out)); \
    memcpy(before, &out, sizeof(out)); \
    for (size_t n = 0; n < sizeof(type); ++n) { \
        CHECK(getter(object, &out.value, n) == MOQR_ERR_INVAL); \
        CHECK(memcmp(before, &out, sizeof(out)) == 0); \
    } \
    CHECK(getter(NULL, &out.value, sizeof(out)) == MOQR_ERR_INVAL); \
    CHECK(memcmp(before, &out, sizeof(out)) == 0); \
    CHECK(getter(object, NULL, sizeof(out)) == MOQR_ERR_INVAL); \
    CHECK(getter(object, &out.value, sizeof(out)) == MOQR_OK); \
    CHECK(memcmp(before + sizeof(type), (unsigned char *)&out + sizeof(type), \
                 sizeof(out) - sizeof(type)) == 0); \
    type exact; \
    memset(&exact, 0x5a, sizeof(exact)); \
    CHECK(getter(object, &exact, sizeof(exact)) == MOQR_OK); \
    CHECK(memcmp(&exact, &out.value, sizeof(exact)) == 0); \
} while (0)

#ifdef TEST_RUNTIME
static moqr_result_t shard_stats(moqr_shards_t *s, moqr_shards_stats_t *out,
                                 size_t size)
{
    return moqr_shards_get_stats_sized(s, 0, out, size);
}
#endif

typedef struct sdk_alloc {
    moq_alloc_t vt;
    size_t calls, live;
} sdk_alloc_t;

static void *sdk_alloc_new(size_t n, void *ctx)
{
    sdk_alloc_t *a = ctx;
    const moq_alloc_t *base = moq_alloc_default();
    a->calls++;
    void *p = base->alloc(n, base->ctx);
    if (p != NULL) a->live += n;
    return p;
}

static void *sdk_alloc_resize(void *p, size_t old, size_t n, void *ctx)
{
    sdk_alloc_t *a = ctx;
    const moq_alloc_t *base = moq_alloc_default();
    a->calls++;
    void *next = base->realloc(p, old, n, base->ctx);
    if (next != NULL) a->live = a->live - old + n;
    return next;
}

static void sdk_alloc_free(void *p, size_t n, void *ctx)
{
    sdk_alloc_t *a = ctx;
    const moq_alloc_t *base = moq_alloc_default();
    a->calls++;
    if (p != NULL) a->live -= n;
    base->free(p, n, base->ctx);
}

static void sdk_alloc_init(sdk_alloc_t *a)
{
    memset(a, 0, sizeof(*a));
    a->vt.alloc = sdk_alloc_new;
    a->vt.realloc = sdk_alloc_resize;
    a->vt.free = sdk_alloc_free;
    a->vt.ctx = a;
}

static moq_bytes_t sdk_bytes(const char *s)
{
    return (moq_bytes_t){ .data = (const uint8_t *)s, .len = strlen(s) };
}

static int sdk_generation(moqr_core_t *core, moqr_trace_t *trace, sdk_alloc_t *a,
                          moqr_track_t track, moqr_result_t result, uint64_t gen)
{
    struct { uint64_t before, value, after; } out = {17, UINT64_MAX, 19};
    moqr_core_stats_t before = {0}, after = {0};
    moqr_core_get_stats(core, &before);
    uint64_t hash = moqr_trace_hash(trace);
    size_t calls = a->calls, live = a->live;
    for (unsigned i = 0; i < 8; i++) {
        CHECK(moqr_core_track_current_generation(core, track, &out.value) == result);
        CHECK(out.value == (result == MOQR_OK ? gen : UINT64_MAX));
        CHECK(out.before == 17 && out.after == 19);
    }
    CHECK(moqr_core_track_current_generation(core, track, NULL) == MOQR_ERR_INVAL);
    moqr_core_get_stats(core, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(moqr_trace_hash(trace) == hash && a->calls == calls && a->live == live);
    return 0;
}

static int sdk_track_lifetime(void)
{
    sdk_alloc_t a;
    sdk_alloc_init(&a);
    moqr_trace_t *trace = NULL;
    moqr_core_t *core = NULL;
    moqr_core_relay_cfg_t cfg;
    moqr_binding_t source = MOQR_BINDING_INVALID, down = MOQR_BINDING_INVALID;
    moqr_sub_t sub = MOQR_SUB_INVALID;
    moqr_track_t track = MOQR_TRACK_INVALID, replaced = MOQR_TRACK_INVALID;
    moqr_intent_t intents[3];
    uint64_t gen = 0, untouched = UINT64_MAX;
    int failed = 0;
#define SDK_REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "sdk-lifetime:%d: %s\n", __LINE__, #expr); \
    failed = 1; goto cleanup; \
} } while (0)
    SDK_REQUIRE(moqr_core_track_current_generation(NULL, track, &untouched) == MOQR_ERR_INVAL);
    SDK_REQUIRE(untouched == UINT64_MAX);
    SDK_REQUIRE(moqr_trace_create(&a.vt, 64, &trace) == MOQR_OK);
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a.vt);
    cfg.trace = trace;
    cfg.max_tracks = 1;
    cfg.linger_us = 10;
    SDK_REQUIRE(moqr_core_create(&cfg, &core) == MOQR_OK);
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(moqr_core_binding_open(core, 900, &source) == MOQR_OK);
    SDK_REQUIRE(moqr_core_binding_open(core, 901, &down) == MOQR_OK);
    moq_bytes_t part = sdk_bytes("sdk");
    moqr_ns_t ns = { .parts = &part, .count = 1 };
    SDK_REQUIRE(moqr_core_announce(core, source, ns) == MOQR_OK);
    moqr_subscribe_req_t req;
    moqr_subscribe_req_init(&req);
    req.ns = ns;
    req.name = sdk_bytes("lifetime");
    req.cookie = 902;
    SDK_REQUIRE(moqr_core_subscribe(core, down, &req, &sub) == MOQR_OK);
    SDK_REQUIRE(moqr_core_sub_track(core, sub, &track, &gen) == MOQR_OK);
    moqr_sub_state_t state;
    SDK_REQUIRE(moqr_core_sub_state(core, sub, &state) == MOQR_OK && state == MOQR_SUB_PARKED);
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_OK, gen) == 0);
    moqr_track_t wrong_pool = { ._opaque = source._opaque };
    moqr_track_t foreign = { ._opaque = moq_handle_pack(MOQR_HANDLE_POOL_TRACK,
        (uint16_t)(moq_handle_session_tag(track._opaque) + 1u),
        moq_handle_generation(track._opaque), moq_handle_slot(track._opaque)) };
    moqr_track_t out_of_pool = { ._opaque = moq_handle_pack(MOQR_HANDLE_POOL_TRACK,
        moq_handle_session_tag(track._opaque), moq_handle_generation(track._opaque), 1) };
    SDK_REQUIRE(sdk_generation(core, trace, &a, wrong_pool, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(sdk_generation(core, trace, &a, foreign, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(sdk_generation(core, trace, &a, out_of_pool, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1);
    SDK_REQUIRE(intents[0].kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE &&
                intents[0].binding_cookie == 900 && intents[0].track_gen == gen &&
                moqr_track_eq(intents[0].track, track));
    SDK_REQUIRE(moqr_core_upstream_ok(core, track, gen, 903, false, 0, 0) == MOQR_OK);
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_OK, gen) == 0);
    SDK_REQUIRE(moqr_core_sub_state(core, sub, &state) == MOQR_OK && state == MOQR_SUB_ACTIVE);
    SDK_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1 &&
                intents[0].kind == MOQR_INTENT_ACCEPT_SUB && intents[0].cookie == 902);
    SDK_REQUIRE(moqr_core_upstream_lost(core, track, gen) == MOQR_OK);
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_OK, gen + 1) == 0);
    SDK_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 0);
    SDK_REQUIRE(moqr_core_publish_open(core, source, ns, req.name, 904, &replaced) == MOQR_OK);
    SDK_REQUIRE(!moqr_track_eq(replaced, track) &&
                moq_handle_slot(replaced._opaque) == moq_handle_slot(track._opaque));
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(sdk_generation(core, trace, &a, replaced, MOQR_OK, gen + 2) == 0);
    SDK_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 2 &&
                intents[0].kind == MOQR_INTENT_SUB_DONE &&
                intents[1].kind == MOQR_INTENT_ACCEPT_PUBLISH &&
                intents[1].track_gen == gen + 2);
    moqr_pd_desc_t done;
    SDK_REQUIRE(moqr_pd_desc_local(MOQR_PD_TRACK_ENDED, &done) == MOQR_OK);
    SDK_REQUIRE(moqr_core_source_done(core, replaced, gen + 2, done, 1) == MOQR_OK);
    SDK_REQUIRE(sdk_generation(core, trace, &a, replaced, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(moqr_core_publish_open(core, source, ns, sdk_bytes("reuse"), 905, &track) == MOQR_OK);
    SDK_REQUIRE(!moqr_track_eq(replaced, track) &&
                moq_handle_slot(replaced._opaque) == moq_handle_slot(track._opaque));
    SDK_REQUIRE(sdk_generation(core, trace, &a, replaced, MOQR_ERR_STALE_HANDLE, 0) == 0);
    SDK_REQUIRE(sdk_generation(core, trace, &a, track, MOQR_OK, 1) == 0);
    SDK_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1 &&
                intents[0].kind == MOQR_INTENT_ACCEPT_PUBLISH && intents[0].cookie == 905);
cleanup:
    moqr_core_destroy(core);
    moqr_trace_destroy(trace);
    if (a.live != 0) {
        fprintf(stderr, "sdk-lifetime: leaked bytes=%zu\n", a.live);
        failed = 1;
    }
#undef SDK_REQUIRE
    return failed;
}

static int sdk_metadata_query(moqr_core_t *core, moqr_trace_t *trace, sdk_alloc_t *a,
                              moqr_track_t track, moqr_result_t result, bool managed)
{
    moqr_core_stats_t before = {0}, after = {0};
    moqr_core_get_stats(core, &before);
    uint64_t hash = moqr_trace_hash(trace);
    size_t calls = a->calls, live = a->live;
    for (unsigned initial = 0; initial < 2; initial++) {
        struct { unsigned char before; bool value; unsigned char after; } out = {
            0xa5, initial != 0, 0x5a
        };
        CHECK(moqr_core_track_metadata_managed(core, track, &out.value) == result);
        CHECK(out.value == (result == MOQR_OK ? managed : initial != 0));
        CHECK(out.before == 0xa5 && out.after == 0x5a);
    }
    CHECK(moqr_core_track_metadata_managed(core, track, NULL) == MOQR_ERR_INVAL);
    moqr_core_get_stats(core, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(moqr_trace_hash(trace) == hash && a->calls == calls && a->live == live);
    return 0;
}

static int sdk_binding_options(void)
{
    sdk_alloc_t a;
    sdk_alloc_init(&a);
    moqr_core_t *core = NULL;
    moqr_trace_t *trace = NULL;
    void *short_input = NULL;
    size_t short_size = 0;
    int failed = 0;
#define SDK_N33_REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "sdk-options:%d: %s\n", __LINE__, #expr); \
    failed = 1; goto cleanup; \
} } while (0)
    struct options_canary { uint64_t before; moqr_binding_options_t value; uint64_t after; } out;
    unsigned char expected[sizeof(out)];
    for (size_t n = 0; n <= sizeof(out.value) + sizeof(out.after); n++) {
        memset(&out, 0xa5, sizeof(out));
        memcpy(expected, &out, sizeof(out));
        if (n >= sizeof(uint32_t)) {
            size_t initialized = n < sizeof(out.value) ? n : sizeof(out.value);
            uint32_t stamp = (uint32_t)initialized;
            memset(expected + offsetof(struct options_canary, value), 0, initialized);
            memcpy(expected + offsetof(struct options_canary, value), &stamp, sizeof(stamp));
        }
        moqr_binding_options_init_sized(&out.value, n);
        SDK_N33_REQUIRE(memcmp(expected, &out, sizeof(out)) == 0);
    }
    moqr_binding_options_init_sized(NULL, 8);
    SDK_N33_REQUIRE(moqr_trace_create(&a.vt, 64, &trace) == MOQR_OK);
    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a.vt);
    cfg.trace = trace;
    cfg.max_bindings = 2;
    SDK_N33_REQUIRE(moqr_core_create(&cfg, &core) == MOQR_OK);
    moqr_binding_options_t options;
    moqr_binding_options_init_sized(&options, sizeof(options));
    moqr_binding_t binding = { ._opaque = UINT64_MAX };
    moqr_core_stats_t before = {0}, after = {0};
    moqr_core_get_stats(core, &before);
    uint64_t hash = moqr_trace_hash(trace);
    size_t calls = a.calls;
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(NULL, 1, &options, &binding) == MOQR_ERR_INVAL);
    SDK_N33_REQUIRE(binding._opaque == UINT64_MAX);
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 1, &options, NULL) == MOQR_ERR_INVAL);
    for (unsigned bit = 1; bit < 32; bit++) {
        options.flags = UINT32_C(1) << bit;
        SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 1, &options, &binding) == MOQR_ERR_INVAL);
        SDK_N33_REQUIRE(binding._opaque == UINT64_MAX);
    }
    SDK_N33_REQUIRE(a.calls == calls && moqr_trace_hash(trace) == hash);
    for (uint32_t n = 0; n < 8; n++) {
        short_size = n < sizeof(uint32_t) ? sizeof(uint32_t) : n;
        short_input = a.vt.alloc(short_size, a.vt.ctx);
        SDK_N33_REQUIRE(short_input != NULL);
        memset(short_input, 0xff, short_size);
        memcpy(short_input, &n, sizeof(n));
        calls = a.calls;
        SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 1,
            (const moqr_binding_options_t *)short_input, &binding) == MOQR_ERR_INVAL);
        SDK_N33_REQUIRE(binding._opaque == UINT64_MAX && a.calls == calls &&
                        moqr_trace_hash(trace) == hash);
        a.vt.free(short_input, short_size, a.vt.ctx);
        short_input = NULL;
    }
    moqr_core_get_stats(core, &after);
    SDK_N33_REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
    moqr_intent_t intent;
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, &intent, 1) == 0);
    bool managed = true;
    SDK_N33_REQUIRE(moqr_core_track_metadata_managed(NULL, MOQR_TRACK_INVALID, &managed) == MOQR_ERR_INVAL && managed);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, MOQR_TRACK_INVALID,
                                      MOQR_ERR_STALE_HANDLE, false) == 0);
    moqr_binding_t direct[2];
    moqr_binding_options_init_sized(&options, sizeof(options));
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 10, NULL, &direct[0]) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 11, &options, &direct[1]) == MOQR_OK);
    moq_bytes_t part = sdk_bytes("options");
    moqr_ns_t ns = { .parts = &part, .count = 1 };
    for (unsigned i = 0; i < 2; i++) {
        moqr_track_t track;
        SDK_N33_REQUIRE(moqr_core_publish_open(core, direct[i], ns,
            sdk_bytes(i == 0 ? "null" : "default"), 12 + i, &track) == MOQR_OK);
        SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, false) == 0);
        SDK_N33_REQUIRE(moqr_core_poll_intents(core, &intent, 1) == 1 &&
                        intent.kind == MOQR_INTENT_ACCEPT_PUBLISH);
    }
cleanup:
    if (short_input != NULL) a.vt.free(short_input, short_size, a.vt.ctx);
    moqr_core_destroy(core);
    moqr_trace_destroy(trace);
    if (a.live != 0) {
        fprintf(stderr, "sdk-options: leaked bytes=%zu\n", a.live);
        failed = 1;
    }
#undef SDK_N33_REQUIRE
    return failed;
}

static int sdk_metadata_source(bool push, bool source_managed, bool downstream_managed)
{
    sdk_alloc_t a;
    sdk_alloc_init(&a);
    moqr_core_t *core = NULL;
    moqr_trace_t *trace = NULL;
    int failed = 0;
#define SDK_N33_REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "sdk-source(%u/%u/%u):%d: %s\n", (unsigned)push, \
        (unsigned)source_managed, (unsigned)downstream_managed, __LINE__, #expr); \
    failed = 1; goto cleanup; \
} } while (0)
    SDK_N33_REQUIRE(moqr_trace_create(&a.vt, 128, &trace) == MOQR_OK);
    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a.vt);
    cfg.trace = trace;
    cfg.max_tracks = 1;
    SDK_N33_REQUIRE(moqr_core_create(&cfg, &core) == MOQR_OK);
    moqr_binding_options_t options;
    moqr_binding_options_init_sized(&options, sizeof(options));
    options.flags = source_managed ? MOQR_BINDING_TRACK_METADATA_MANAGED : 0;
    moqr_binding_t source, down, alternate, reopened;
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 100, &options, &source) == MOQR_OK);
    options.flags = downstream_managed ? MOQR_BINDING_TRACK_METADATA_MANAGED : 0;
    SDK_N33_REQUIRE(moqr_core_binding_open_with_options(core, 101, &options, &down) == MOQR_OK);
    moq_bytes_t parts[] = { sdk_bytes("sdk"), sdk_bytes("provenance") };
    moqr_ns_t ns = { .parts = parts, .count = 2 };
    moqr_ns_t prefix = { .parts = parts, .count = 1 };
    SDK_N33_REQUIRE(moqr_core_announce(core, source, ns) == MOQR_OK);
    moqr_track_t track = MOQR_TRACK_INVALID, replacement = MOQR_TRACK_INVALID;
    moqr_sub_t sub, rejoin;
    moqr_intent_t intents[3];
    uint64_t generation = 0;
    moqr_subscribe_req_t req;
    moqr_subscribe_req_init(&req);
    req.ns = ns;
    req.name = sdk_bytes("video");
    req.cookie = 102;
    if (push) {
        SDK_N33_REQUIRE(moqr_core_publish_open(core, source, ns, req.name, 103, &track) == MOQR_OK);
        SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1 &&
                        intents[0].kind == MOQR_INTENT_ACCEPT_PUBLISH &&
                        intents[0].binding_cookie == 100);
        SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, source_managed) == 0);
    }
    SDK_N33_REQUIRE(moqr_core_subscribe(core, down, &req, &sub) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_sub_track(core, sub, &track, &generation) == MOQR_OK);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, source_managed) == 0);
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1);
    if (!push) {
        SDK_N33_REQUIRE(intents[0].kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE);
        moqr_sub_state_t state;
        SDK_N33_REQUIRE(moqr_core_sub_state(core, sub, &state) == MOQR_OK &&
                        state == MOQR_SUB_PARKED);
        SDK_N33_REQUIRE(moqr_core_upstream_ok(core, track, generation, 104, false, 0, 0) == MOQR_OK);
        SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1);
    }
    SDK_N33_REQUIRE(intents[0].kind == MOQR_INTENT_ACCEPT_SUB &&
                    intents[0].binding_cookie == 101 && intents[0].cookie == 102);
    moqr_track_t wrong = { ._opaque = down._opaque };
    moqr_track_t foreign = { ._opaque = moq_handle_pack(MOQR_HANDLE_POOL_TRACK,
        (uint16_t)(moq_handle_session_tag(track._opaque) + 1u),
        moq_handle_generation(track._opaque), moq_handle_slot(track._opaque)) };
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, wrong, MOQR_ERR_STALE_HANDLE, false) == 0);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, foreign, MOQR_ERR_STALE_HANDLE, false) == 0);

    /* An alternate direct source cannot erase the original managed obligation. */
    SDK_N33_REQUIRE(moqr_core_binding_open(core, 105, &alternate) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_announce(core, alternate, prefix) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_binding_close(core, source, 1) == MOQR_OK);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, source_managed) == 0);
    SDK_N33_REQUIRE(moqr_core_sub_track(core, sub, &replacement, &generation) == MOQR_OK &&
                    moqr_track_eq(track, replacement));
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 1 &&
                    intents[0].kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE &&
                    intents[0].binding_cookie == 105 && intents[0].track_gen == generation);
    SDK_N33_REQUIRE(moqr_core_upstream_ok(core, track, generation, 106, false, 0, 0) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 0);
    SDK_N33_REQUIRE(moqr_core_upstream_lost(core, track, generation) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_unannounce(core, alternate, prefix) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_binding_close(core, alternate, 2) == MOQR_OK);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, source_managed) == 0);
    SDK_N33_REQUIRE(moqr_core_binding_open(core, 107, &reopened) == MOQR_OK &&
                    reopened._opaque != source._opaque &&
                    moq_handle_slot(reopened._opaque) == moq_handle_slot(source._opaque));
    SDK_N33_REQUIRE(moqr_core_announce(core, reopened, ns) == MOQR_OK);
    req.cookie = 108;
    SDK_N33_REQUIRE(moqr_core_subscribe(core, down, &req, &rejoin) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_sub_track(core, rejoin, &replacement, &generation) == MOQR_OK &&
                    moqr_track_eq(track, replacement));
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, source_managed) == 0);
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 2 &&
                    intents[0].kind == MOQR_INTENT_ACCEPT_SUB && intents[0].cookie == 108 &&
                    intents[1].kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE);
    SDK_N33_REQUIRE(moqr_core_upstream_ok(core, track, generation, 109, false, 0, 0) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 0);
    SDK_N33_REQUIRE(moqr_core_upstream_lost(core, track, generation) == MOQR_OK);
    SDK_N33_REQUIRE(moqr_core_publish_open(core, reopened, ns, req.name, 110, &replacement) == MOQR_OK);
    SDK_N33_REQUIRE(!moqr_track_eq(track, replacement) &&
                    moq_handle_slot(track._opaque) == moq_handle_slot(replacement._opaque));
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_ERR_STALE_HANDLE, false) == 0);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, replacement, MOQR_OK, false) == 0);
    SDK_N33_REQUIRE(moqr_core_poll_intents(core, intents, 3) == 3 &&
                    intents[0].kind == MOQR_INTENT_SUB_DONE &&
                    intents[1].kind == MOQR_INTENT_SUB_DONE &&
                    intents[2].kind == MOQR_INTENT_ACCEPT_PUBLISH);
    generation = intents[2].track_gen;
    SDK_N33_REQUIRE(moqr_core_upstream_lost(core, replacement, generation) == MOQR_OK);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, replacement, MOQR_ERR_STALE_HANDLE, false) == 0);
    SDK_N33_REQUIRE(moqr_core_publish_open(core, reopened, ns, sdk_bytes("reuse"), 111, &track) == MOQR_OK);
    SDK_N33_REQUIRE(!moqr_track_eq(track, replacement) &&
                    moq_handle_slot(track._opaque) == moq_handle_slot(replacement._opaque));
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, track, MOQR_OK, false) == 0);
    SDK_N33_REQUIRE(sdk_metadata_query(core, trace, &a, replacement, MOQR_ERR_STALE_HANDLE, false) == 0);
cleanup:
    moqr_core_destroy(core);
    moqr_trace_destroy(trace);
    if (a.live != 0) {
        fprintf(stderr, "sdk-source: leaked bytes=%zu\n", a.live);
        failed = 1;
    }
#undef SDK_N33_REQUIRE
    return failed;
}

int main(void)
{
    CHECK(sdk_track_lifetime() == 0);
    CHECK(sdk_binding_options() == 0);
    for (unsigned push = 0; push < 2; push++) {
        for (unsigned source = 0; source < 2; source++) {
            for (unsigned downstream = 0; downstream < 2; downstream++) {
                CHECK(sdk_metadata_source(push != 0, source != 0, downstream != 0) == 0);
            }
        }
    }
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_core_relay_cfg_t cfg;
    moqr_core_t *core = NULL;
    moqr_log_cfg_t lcfg;
    moqr_log_t *log = NULL;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), alloc);
    CHECK(moqr_core_create(&cfg, &core) == MOQR_OK);
    CHECK_STATS(moqr_core_stats_t, moqr_core_get_stats_sized, core);
    moqr_log_cfg_init_sized(&lcfg, sizeof(lcfg), alloc);
    CHECK(moqr_log_create(&lcfg, &log) == MOQR_OK);
    CHECK_STATS(moqr_log_stats_t, moqr_log_get_stats_sized, log);
    moqr_log_destroy(log);
    moqr_core_destroy(core);
#ifdef TEST_RUNTIME
    moqr_shards_cfg_t scfg;
    moqr_shards_t *a = NULL, *b = NULL;
    moqr_binding_t binding;
    moqr_core_stats_t ac, bc;
    moqr_shards_cfg_init_sized(&scfg, sizeof(scfg), alloc);
    scfg.shards = 2;
    CHECK(moqr_shards_create(&scfg, &a) == MOQR_OK);
    CHECK(moqr_shards_create(&scfg, &b) == MOQR_OK);
    CHECK_STATS(moqr_shards_stats_t, shard_stats, a);
    CHECK_STATS(moqr_bind_stats_t, moqr_bind_get_stats_sized,
                moqr_shards_bind(a, 0));
    moqr_shards_stats_t rejected, before;
    memset(&rejected, 0xa5, sizeof(rejected));
    memcpy(&before, &rejected, sizeof(before));
    CHECK(moqr_shards_get_stats_sized(a, 2, &rejected, sizeof(rejected)) == MOQR_ERR_INVAL);
    CHECK(memcmp(&before, &rejected, sizeof(before)) == 0);
    CHECK(moqr_shards_core(a, 2) == NULL);
    CHECK(moqr_shards_bind(a, 2) == NULL);
    CHECK(moqr_shards_trace(a, 2) == NULL);
    CHECK(moqr_core_get_stats_sized(moqr_shards_core(a, 0), &ac, sizeof(ac)) == MOQR_OK);
    CHECK(moqr_core_get_stats_sized(moqr_shards_core(b, 0), &bc, sizeof(bc)) == MOQR_OK);
    CHECK(ac.bindings == bc.bindings);
    uint64_t base_bindings = bc.bindings;
    CHECK(moqr_core_binding_open(moqr_shards_core(a, 0), 91, &binding) == MOQR_OK);
    CHECK(moqr_core_get_stats_sized(moqr_shards_core(a, 0), &ac, sizeof(ac)) == MOQR_OK);
    CHECK(moqr_core_get_stats_sized(moqr_shards_core(b, 0), &bc, sizeof(bc)) == MOQR_OK);
    CHECK(ac.bindings == base_bindings + 1 && bc.bindings == base_bindings);
    CHECK(moqr_core_binding_close(moqr_shards_core(a, 0), binding, 0) == MOQR_OK);
    moqr_shards_destroy(a);
    CHECK(moqr_shards_step(b, 1) == MOQR_OK);
    CHECK(moqr_core_get_stats_sized(moqr_shards_core(b, 0), &bc, sizeof(bc)) == MOQR_OK);
    CHECK(bc.bindings == base_bindings);
    moqr_shards_destroy(b);
#endif
    return 0;
}
