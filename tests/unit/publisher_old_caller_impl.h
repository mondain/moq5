/* Included by four distinct translation units, each with its old prototypes. */
#include "publisher_old_layout.h"
#include "publisher_old_caller_test.h"
#include <string.h>

#ifdef MOQ_PUBLISHER_H
#error Old caller must not see the current publisher declarations
#endif

extern void moq_pub_cfg_init(OLD_CFG *cfg);
extern void moq_pub_cfg_init_sized(OLD_CFG *cfg, size_t size);
extern moq_result_t moq_pub_create(moq_session_t *, const moq_alloc_t *,
                                  const OLD_CFG *, struct moq_publisher **);
extern void moq_pub_destroy(struct moq_publisher *);

typedef struct old_storage {
    uint32_t before;
    OLD_CFG cfg;
    uint8_t after[16];
} old_storage_t;
_Static_assert(offsetof(old_storage_t, cfg) == (_Alignof(OLD_CFG) == 4 ? 4 : 8),
               "old member follows old alignment, not current alignment");

#define CHECK(c) (failures += publisher_old_check((c), __FILE__, __LINE__))
#define FLOOR (offsetof(OLD_CFG, default_publisher_priority) + 1)

static void expected_init(OLD_CFG *cfg, size_t n, bool nested)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)n;
    if (n >= FLOOR) cfg->default_publisher_priority = 128;
#if OLD_GENERATION >= 1
    size_t off = offsetof(OLD_CFG, callbacks);
    if (nested && n >= off + sizeof(uint32_t)) {
        size_t cb_size = n - off;
        if (cb_size > sizeof(cfg->callbacks)) cb_size = sizeof(cfg->callbacks);
        cfg->callbacks.struct_size = (uint32_t)cb_size;
    }
#else
    (void)nested;
#endif
}

static int create_case(old_storage_t *storage, publisher_old_test_context_t *ctx,
                       moq_result_t expected)
{
    int failures = 0;
    unsigned char before[sizeof(*storage)];
    memcpy(before, storage, sizeof(before));
    const size_t calls = *ctx->alloc_calls, live = *ctx->live;
    struct moq_publisher *pub = (struct moq_publisher *)(uintptr_t)1;
    moq_result_t rc = moq_pub_create(ctx->session, ctx->alloc, &storage->cfg, &pub);
    CHECK(rc == expected);
    CHECK(memcmp(before, storage, sizeof(before)) == 0);
    if (expected == MOQ_OK) {
        CHECK(pub != NULL && pub != (struct moq_publisher *)(uintptr_t)1);
        CHECK(*ctx->alloc_calls > calls);
    } else {
        CHECK(pub == NULL);
        CHECK(*ctx->alloc_calls == calls + (expected == MOQ_ERR_NOMEM ? 1u : 0u));
    }
    if (pub && pub != (struct moq_publisher *)(uintptr_t)1) moq_pub_destroy(pub);
    CHECK(*ctx->live == live);
    return failures;
}

int OLD_ENTRY(unsigned operation, publisher_old_test_context_t *ctx)
{
    int failures = 0;
    _Alignas(8) old_storage_t storage;
    const size_t offset = offsetof(old_storage_t, cfg);
    if (_Alignof(OLD_CFG) == 4)
        CHECK(((uintptr_t)&storage.cfg & 7u) == 4);
    memset(&storage, 0xa5, sizeof(storage));

    if (operation == PUB_OLD_INIT || operation == PUB_OLD_SIZED) {
        const size_t first = operation == PUB_OLD_INIT ? FLOOR : 0;
        const size_t last = operation == PUB_OLD_INIT ? FLOOR : sizeof(OLD_CFG);
        moq_pub_cfg_init(NULL);
        moq_pub_cfg_init_sized(NULL, sizeof(OLD_CFG));
        for (size_t n = first; n <= last; ++n) {
            unsigned char want[sizeof(storage)];
            OLD_CFG defaults;
            memset(&storage, 0xa5, sizeof(storage));
            memcpy(want, &storage, sizeof(want));
            if (n >= sizeof(uint32_t)) {
                expected_init(&defaults, n, operation == PUB_OLD_SIZED);
                memcpy(want + offset, &defaults, n);
            }
            if (operation == PUB_OLD_INIT) moq_pub_cfg_init(&storage.cfg);
            else moq_pub_cfg_init_sized(&storage.cfg, n);
            CHECK(memcmp(want, &storage, sizeof(want)) == 0);
        }
        OLD_CFG *exact = ctx->alloc->alloc(sizeof(*exact), ctx->alloc->ctx);
        CHECK(exact != NULL);
        if (exact) {
            unsigned char want[sizeof(*exact)];
            OLD_CFG defaults;
            const size_t n = operation == PUB_OLD_INIT ? FLOOR : sizeof(*exact);
            memset(exact, 0xa5, sizeof(*exact));
            memset(want, 0xa5, sizeof(want));
            expected_init(&defaults, n, operation == PUB_OLD_SIZED);
            memcpy(want, &defaults, n);
            if (operation == PUB_OLD_INIT) moq_pub_cfg_init(exact);
            else moq_pub_cfg_init_sized(exact, n);
            CHECK(memcmp(want, exact, sizeof(want)) == 0);
            ctx->alloc->free(exact, sizeof(*exact), ctx->alloc->ctx);
        }
        return failures;
    }

    expected_init(&storage.cfg, sizeof(storage.cfg), true);
#if OLD_GENERATION == 3
    const size_t field_end = offsetof(OLD_CFG, on_subscribe_v1_ctx) + sizeof(void *);
#elif OLD_GENERATION == 2
    const size_t field_end = offsetof(OLD_CFG, max_fills) + sizeof(uint32_t);
#elif OLD_GENERATION == 1
    const size_t field_end = offsetof(OLD_CFG, callbacks) + sizeof(test_pub_old_callbacks_t);
#else
    const size_t field_end = FLOOR;
#endif
    memset((unsigned char *)&storage.cfg + field_end, 0xa5, sizeof(storage.cfg) - field_end);
    failures += create_case(&storage, ctx, MOQ_OK);
    OLD_CFG *exact = ctx->alloc->alloc(sizeof(*exact), ctx->alloc->ctx);
    CHECK(exact != NULL);
    if (exact) {
        memcpy(exact, &storage.cfg, sizeof(*exact));
        struct moq_publisher *pub = NULL;
        CHECK(moq_pub_create(ctx->session, ctx->alloc, exact, &pub) == MOQ_OK);
        CHECK(pub != NULL);
        CHECK(memcmp(exact, &storage.cfg, sizeof(*exact)) == 0);
        if (pub) moq_pub_destroy(pub);
        ctx->alloc->free(exact, sizeof(*exact), ctx->alloc->ctx);
    }
    storage.cfg.accept_mode = TEST_PUB_OLD_ACCEPT;
    failures += create_case(&storage, ctx, MOQ_OK);
    storage.cfg.struct_size = FLOOR;
    failures += create_case(&storage, ctx, MOQ_OK);
    storage.cfg.struct_size = FLOOR - 1;
    failures += create_case(&storage, ctx, MOQ_ERR_INVAL);
    storage.cfg.struct_size = sizeof(storage.cfg);
    storage.cfg.accept_mode = TEST_PUB_OLD_CALLBACK;
    failures += create_case(&storage, ctx, MOQ_ERR_INVAL);
    storage.cfg.accept_mode = TEST_PUB_OLD_CALLBACK_V1;
    failures += create_case(&storage, ctx, MOQ_ERR_INVAL);
    storage.cfg.accept_mode = TEST_PUB_OLD_REJECT;
    *ctx->fail_alloc = true;
    failures += create_case(&storage, ctx, MOQ_ERR_NOMEM);
    *ctx->fail_alloc = false;

    unsigned char before[sizeof(storage)];
    memcpy(before, &storage, sizeof(before));
    size_t calls = *ctx->alloc_calls;
    struct moq_publisher *sentinel = (struct moq_publisher *)(uintptr_t)1;
    struct moq_publisher *pub = sentinel;
    CHECK(moq_pub_create(NULL, ctx->alloc, &storage.cfg, &pub) == MOQ_ERR_INVAL);
    CHECK(pub == sentinel);
    CHECK(moq_pub_create(ctx->session, NULL, &storage.cfg, &pub) == MOQ_ERR_INVAL);
    CHECK(pub == sentinel);
    CHECK(moq_pub_create(ctx->session, ctx->alloc, NULL, &pub) == MOQ_ERR_INVAL);
    CHECK(pub == sentinel);
    CHECK(moq_pub_create(ctx->session, ctx->alloc, &storage.cfg, NULL) == MOQ_ERR_INVAL);
    CHECK(*ctx->alloc_calls == calls);
    CHECK(memcmp(before, &storage, sizeof(before)) == 0);
    return failures;
}

#undef FLOOR
#undef CHECK
