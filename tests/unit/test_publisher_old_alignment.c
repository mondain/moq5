#include <moq/publisher.h>
#include <moq/sim.h>
#include "publisher_old_caller_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks, alloc_calls, live;
static bool fail_alloc;

int publisher_old_check(bool condition, const char *file, int line)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL %s:%d\n", file, line);
    return condition ? 0 : 1;
}
#define CHECK(c) (failures += publisher_old_check((c), __FILE__, __LINE__))

static void *allocate(size_t size, void *ctx)
{
    (void)ctx;
    ++alloc_calls;
    if (fail_alloc) return NULL;
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
static void *resize(void *p, size_t old_size, size_t size, void *ctx)
{
    (void)old_size;
    if (!p) return allocate(size, ctx);
    ++alloc_calls;
    if (fail_alloc) return NULL;
    return realloc(p, size);
}
static void release(void *p, size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    if (p) --live;
    free(p);
}

static int current_guards(moq_session_t *session, const moq_alloc_t *alloc)
{
    int failures = 0;
    struct {
        uint64_t before;
        moq_pub_cfg_t cfg;
        uint8_t future[16];
    } storage;
    memset(&storage, 0xa5, sizeof(storage));
    moq_pub_cfg_init_sized(&storage.cfg, sizeof(storage.cfg) + sizeof(storage.future));
    CHECK(storage.before == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(storage.cfg.struct_size == sizeof(storage.cfg));
    CHECK(storage.cfg.default_publisher_priority == 128);
    CHECK(storage.cfg.callbacks.struct_size == sizeof(storage.cfg.callbacks));
    for (size_t i = 0; i < sizeof(storage.future); ++i) CHECK(storage.future[i] == 0xa5);
    storage.cfg.struct_size = sizeof(storage.cfg) + sizeof(storage.future);
    unsigned char before[sizeof(storage)];
    memcpy(before, &storage, sizeof(before));
    moq_publisher_t *pub = NULL;
    CHECK(moq_pub_create(session, alloc, &storage.cfg, &pub) == MOQ_OK);
    CHECK(pub != NULL);
    CHECK(memcmp(before, &storage, sizeof(before)) == 0);
    if (pub) moq_pub_destroy(pub);
    CHECK(live == 0);
    return failures;
}

static void on_closed(void *ctx, uint64_t code)
{
    unsigned *count = ctx;
    *count += code == 17 ? 1u : 100u;
}

static int nested_prefixes(moq_session_t *session, const moq_alloc_t *alloc)
{
    int failures = 0;
    const size_t end = offsetof(moq_pub_callbacks_t, on_closed) + sizeof(void *);
    const size_t extents[] = {0, 3, 4, end - 1, end, sizeof(moq_pub_callbacks_t)};
    for (size_t i = 0; i < sizeof(extents) / sizeof(extents[0]); ++i) {
        for (size_t j = 0; j < sizeof(extents) / sizeof(extents[0]); ++j) {
            unsigned count = 0;
            moq_pub_cfg_t cfg;
            moq_pub_cfg_init_sized(&cfg, sizeof(cfg));
            cfg.struct_size = (uint32_t)(offsetof(moq_pub_cfg_t, callbacks) + extents[i]);
            cfg.callbacks.struct_size = (uint32_t)extents[j];
            cfg.callbacks.ctx = &count;
            cfg.callbacks.on_closed = on_closed;
            unsigned char before[sizeof(cfg)];
            memcpy(before, &cfg, sizeof(before));
            moq_publisher_t *pub = NULL;
            CHECK(moq_pub_create(session, alloc, &cfg, &pub) == MOQ_OK);
            CHECK(pub != NULL);
            CHECK(memcmp(before, &cfg, sizeof(before)) == 0);
            if (pub) {
                moq_event_t event;
                memset(&event, 0, sizeof(event));
                event.kind = MOQ_EVENT_SESSION_CLOSED;
                event.u.closed.code = 17;
                moq_pub_event_result_t result = MOQ_PUB_EVENT_IGNORED;
                CHECK(moq_pub_handle_event(pub, &event, 0, &result) == MOQ_OK);
                CHECK(count == (extents[i] >= end && extents[j] >= end ? 1u : 0u));
                moq_pub_destroy(pub);
            }
            CHECK(live == 0);
        }
    }
    return failures;
}

static int refusal_order(const moq_alloc_t *alloc)
{
    int failures = 0;
    moq_simpair_cfg_t pair_cfg = MOQ_SIMPAIR_CFG_INIT;
    pair_cfg.alloc = moq_alloc_default();
    pair_cfg.version = MOQ_VERSION_DRAFT_18;
    moq_simpair_t *pair = NULL;
    CHECK(moq_simpair_create(&pair_cfg, &pair) == MOQ_OK);
    if (!pair) return failures;
    moq_pub_cfg_t cfg;
    moq_pub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.max_fill_snapshot_bytes = 1;
    cfg.accept_mode = MOQ_PUB_CALLBACK_V1;
    fail_alloc = true;
    size_t calls = alloc_calls;
    unsigned char before[sizeof(cfg)];
    memcpy(before, &cfg, sizeof(before));
    moq_publisher_t *pub = (moq_publisher_t *)(uintptr_t)1;
    CHECK(moq_pub_create(moq_simpair_server(pair), alloc, &cfg, &pub) == MOQ_ERR_INVAL);
    CHECK(pub == NULL && alloc_calls == calls);
    CHECK(memcmp(before, &cfg, sizeof(before)) == 0);
    cfg.accept_mode = MOQ_PUB_REJECT_ALL;
    memcpy(before, &cfg, sizeof(before));
    CHECK(moq_pub_create(moq_simpair_server(pair), alloc, &cfg, &pub) == MOQ_ERR_UNSUPPORTED);
    CHECK(pub == NULL && alloc_calls == calls);
    CHECK(memcmp(before, &cfg, sizeof(before)) == 0);
    cfg.max_fill_snapshot_bytes = 0;
    CHECK(moq_pub_create(moq_simpair_server(pair), alloc, &cfg, &pub) == MOQ_ERR_NOMEM);
    CHECK(pub == NULL && alloc_calls == calls + 1);
    fail_alloc = false;
    CHECK(live == 0);
    moq_simpair_destroy(pair);
    return failures;
}

int main(int argc, char **argv)
{
    int failures = 0;
    const char *names[] = {"v0", "callbacks", "pre_v1", "n12"};
    const char *operations[] = {"init", "sized", "create"};
    int (*const callers[])(unsigned, publisher_old_test_context_t *) = {
        publisher_old_v0, publisher_old_callbacks, publisher_old_pre_v1, publisher_old_v1
    };
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s [v0|callbacks|pre_v1|n12 init|sized|create]\n", argv[0]);
        return 2;
    }
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    moq_simpair_t *pair = NULL;
    CHECK(moq_simpair_create(&cfg, &pair) == MOQ_OK);
    if (!pair) return 1;
    CHECK(moq_simpair_start(pair) == MOQ_OK);
    moq_simpair_run_until_quiescent(pair, 32, NULL);
    moq_alloc_t alloc = {.ctx = NULL, .alloc = allocate, .realloc = resize, .free = release};
    publisher_old_test_context_t ctx = {
        .session = moq_simpair_server(pair), .alloc = &alloc,
        .alloc_calls = &alloc_calls, .live = &live, .fail_alloc = &fail_alloc
    };
    unsigned cases = 0;
    for (unsigned i = 0; i < sizeof(callers) / sizeof(callers[0]); ++i) {
        for (unsigned op = 0; op < sizeof(operations) / sizeof(operations[0]); ++op) {
            if (argc == 3 && (strcmp(argv[1], names[i]) || strcmp(argv[2], operations[op])))
                continue;
            ++cases;
            failures += callers[i](op, &ctx);
        }
    }
    if (argc == 1) {
        failures += current_guards(ctx.session, &alloc);
        failures += nested_prefixes(ctx.session, &alloc);
        failures += refusal_order(&alloc);
    }
    CHECK(cases > 0);
    CHECK(live == 0);
    moq_simpair_destroy(pair);
    printf("publisher old alignment: %u cases, %zu checks, %d failures\n",
           cases, checks, failures);
    return failures ? 1 : 0;
}
