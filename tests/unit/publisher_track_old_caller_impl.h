/* Included by two distinct old-prototype TUs, never by the current driver. */
#include "publisher_track_old_layout.h"
#include "publisher_track_old_caller_test.h"
#include <string.h>

#ifdef MOQ_PUBLISHER_H
#error Old track caller must not see current publisher declarations
#endif

extern void moq_pub_track_cfg_init(OLD_TRACK_CFG *cfg);
extern void moq_pub_track_cfg_init_sized(OLD_TRACK_CFG *cfg, size_t size);
extern moq_result_t moq_pub_add_track(struct moq_publisher *, const OLD_TRACK_CFG *,
                                     uint64_t, struct moq_pub_track **);
extern moq_result_t moq_pub_remove_track(struct moq_publisher *, struct moq_pub_track *, uint64_t);

typedef struct old_track_storage {
    uint32_t before;
    OLD_TRACK_CFG cfg;
    uint8_t after[16];
} old_track_storage_t;
_Static_assert(offsetof(old_track_storage_t, cfg) == (_Alignof(OLD_TRACK_CFG) == 4 ? 4 : 8),
               "genuine old member alignment, not current alignment");

#define CHECK(c) (failures += publisher_track_old_check((c), __FILE__, __LINE__))
#define ORIGINAL_SIZE sizeof(test_pub_track_original_t)

static void init_expected(OLD_TRACK_CFG *cfg, size_t n)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)n;
    if (n >= offsetof(OLD_TRACK_CFG, publisher_priority) + 1)
        cfg->publisher_priority = 128;
}

static int add_case(OLD_TRACK_CFG *cfg, publisher_track_old_context_t *ctx,
                    moq_result_t expected, uint64_t budget, uint8_t priority)
{
    int failures = 0;
    unsigned char before[sizeof(*cfg)];
    memcpy(before, cfg, sizeof(before));
    size_t calls = *ctx->alloc_calls, live = *ctx->live;
    struct moq_pub_track *sentinel = (struct moq_pub_track *)(uintptr_t)1;
    struct moq_pub_track *track = sentinel;
    moq_result_t rc = moq_pub_add_track(ctx->pub, cfg, 1000, &track);
    CHECK(rc == expected);
    CHECK(memcmp(before, cfg, sizeof(before)) == 0);
    if (rc == MOQ_OK && track && track != sentinel) {
        CHECK(*ctx->alloc_calls == calls + 5); /* existing track/identity ownership only */
        struct moq_pub_track *duplicate = sentinel;
        calls = *ctx->alloc_calls;
        CHECK(moq_pub_add_track(ctx->pub, cfg, 1000, &duplicate) == MOQ_ERR_INVAL);
        CHECK(duplicate == NULL && *ctx->alloc_calls == calls);
        CHECK(memcmp(before, cfg, sizeof(before)) == 0);
        /* The accepted identity must be independent of the caller's buffers. */
        ((uint8_t *)cfg->track_namespace.parts[0].data)[0] = 'X';
        ((uint8_t *)cfg->track_name.data)[0] = 'X';
        failures += publisher_track_old_behavior(ctx, track, budget, priority, false);
        ((uint8_t *)cfg->track_namespace.parts[0].data)[0] = 't';
        ((uint8_t *)cfg->track_name.data)[0] = 'v';
        CHECK(moq_pub_remove_track(ctx->pub, track, 1000) == MOQ_OK);
    } else {
        CHECK(track == NULL);
        CHECK(*ctx->alloc_calls == calls);
    }
    CHECK(*ctx->live == live);
    return failures;
}

int OLD_TRACK_ENTRY(unsigned operation, publisher_track_old_context_t *ctx)
{
    int failures = 0;
    _Alignas(8) old_track_storage_t storage;
    if (_Alignof(OLD_TRACK_CFG) == 4)
        CHECK(((uintptr_t)&storage.cfg & 7u) == 4);
    memset(&storage, 0xa5, sizeof(storage));

    if (operation == PUB_TRACK_OLD_INIT || operation == PUB_TRACK_OLD_SIZED) {
        const size_t first = operation == PUB_TRACK_OLD_INIT ? ORIGINAL_SIZE : 0;
        const size_t last = operation == PUB_TRACK_OLD_INIT ? ORIGINAL_SIZE : sizeof(OLD_TRACK_CFG);
        moq_pub_track_cfg_init(NULL);
        moq_pub_track_cfg_init_sized(NULL, sizeof(OLD_TRACK_CFG));
        for (size_t n = first; n <= last; ++n) {
            unsigned char want[sizeof(storage)];
            OLD_TRACK_CFG defaults;
            memset(&storage, 0xa5, sizeof(storage));
            memcpy(want, &storage, sizeof(want));
            if (n >= sizeof(uint32_t)) {
                init_expected(&defaults, n);
                memcpy(want + offsetof(old_track_storage_t, cfg), &defaults, n);
            }
            if (operation == PUB_TRACK_OLD_INIT) moq_pub_track_cfg_init(&storage.cfg);
            else moq_pub_track_cfg_init_sized(&storage.cfg, n);
            CHECK(memcmp(want, &storage, sizeof(want)) == 0);
        }
        OLD_TRACK_CFG *exact = ctx->alloc->alloc(sizeof(*exact), ctx->alloc->ctx);
        CHECK(exact != NULL);
        if (exact) {
            unsigned char want[sizeof(*exact)];
            OLD_TRACK_CFG defaults;
            const size_t n = operation == PUB_TRACK_OLD_INIT ? ORIGINAL_SIZE : sizeof(*exact);
            memset(exact, 0xa5, sizeof(*exact));
            memset(want, 0xa5, sizeof(want));
            init_expected(&defaults, n);
            memcpy(want, &defaults, n);
            if (operation == PUB_TRACK_OLD_INIT) moq_pub_track_cfg_init(exact);
            else moq_pub_track_cfg_init_sized(exact, n);
            CHECK(memcmp(want, exact, sizeof(want)) == 0);
            ctx->alloc->free(exact, sizeof(*exact), ctx->alloc->ctx);
        }
        return failures;
    }

    uint8_t ns_data[] = "track-compat", name_data[] = "video";
    moq_bytes_t part = {ns_data, sizeof(ns_data) - 1};
    init_expected(&storage.cfg, sizeof(storage.cfg));
    storage.cfg.track_namespace = (moq_namespace_t){&part, 1};
    storage.cfg.track_name = (moq_bytes_t){name_data, sizeof(name_data) - 1};
    storage.cfg.publisher_priority = 0;
#if OLD_TRACK_GENERATION == 1
    const size_t tail = offsetof(OLD_TRACK_CFG, has_publisher_priority) + 1;
#else
    const size_t tail = offsetof(OLD_TRACK_CFG, publisher_priority) + 1;
#endif
    memset((unsigned char *)&storage.cfg + tail, 0xa5, sizeof(storage.cfg) - tail);

    struct moq_pub_track *sentinel = (struct moq_pub_track *)(uintptr_t)1, *out = sentinel;
    size_t calls = *ctx->alloc_calls;
    CHECK(moq_pub_add_track(NULL, &storage.cfg, 1000, &out) == MOQ_ERR_INVAL);
    CHECK(out == sentinel);
    CHECK(moq_pub_add_track(ctx->pub, NULL, 1000, &out) == MOQ_ERR_INVAL);
    CHECK(out == sentinel);
    CHECK(moq_pub_add_track(ctx->pub, &storage.cfg, 1000, NULL) == MOQ_ERR_INVAL);
    CHECK(*ctx->alloc_calls == calls);

    if (operation == PUB_TRACK_OLD_REFUSAL) {
        for (unsigned invalid = 0; invalid < 2; ++invalid) {
            if (invalid) storage.cfg.struct_size = 0;
            unsigned char before[sizeof(storage)];
            memcpy(before, &storage, sizeof(before));
            out = sentinel;
            CHECK(moq_pub_add_track(ctx->pub, &storage.cfg, 1000, &out) == ctx->refusal);
            CHECK(out == (ctx->refusal == MOQ_ERR_WRONG_STATE ? sentinel : NULL));
            CHECK(*ctx->alloc_calls == calls);
            CHECK(memcmp(before, &storage, sizeof(before)) == 0);
        }
        return failures;
    }

    storage.cfg.struct_size = ORIGINAL_SIZE - 1;
    failures += add_case(&storage.cfg, ctx, MOQ_ERR_INVAL, 0, 0);
    storage.cfg.struct_size = sizeof(storage.cfg);
    storage.cfg.track_name.data = NULL;
    failures += add_case(&storage.cfg, ctx, MOQ_ERR_INVAL, 0, 0);
    storage.cfg.track_name.data = name_data;
    failures += add_case(&storage.cfg, ctx, MOQ_OK, 1024u * 1024u, 150);

    OLD_TRACK_CFG *exact = ctx->alloc->alloc(sizeof(*exact), ctx->alloc->ctx);
    CHECK(exact != NULL);
    if (exact) {
        memcpy(exact, &storage.cfg, sizeof(*exact));
#if OLD_TRACK_GENERATION == 1
        exact->max_retained_bytes = 32;
        exact->has_publisher_priority = true;
        failures += add_case(exact, ctx, MOQ_OK, 32, 0);
#else
        failures += add_case(exact, ctx, MOQ_OK, 1024u * 1024u, 150);
#endif
        ctx->alloc->free(exact, sizeof(*exact), ctx->alloc->ctx);
    }

    /* Fail each existing publisher allocation in order; each refusal must
     * release everything and leave the same identity available for retry. */
    bool succeeded = false;
    for (size_t nth = 1; nth <= 6; ++nth) {
        calls = *ctx->alloc_calls;
        size_t live = *ctx->live;
        unsigned char before[sizeof(storage)];
        memcpy(before, &storage, sizeof(before));
        *ctx->fail_at = calls + nth;
        out = sentinel;
        moq_result_t rc = moq_pub_add_track(ctx->pub, &storage.cfg, 1000, &out);
        *ctx->fail_at = SIZE_MAX;
        CHECK(memcmp(before, &storage, sizeof(before)) == 0);
        if (rc == MOQ_OK && out && out != sentinel) {
            CHECK(nth == 6 && *ctx->alloc_calls == calls + 5);
            CHECK(moq_pub_remove_track(ctx->pub, out, 1000) == MOQ_OK);
            succeeded = true;
        } else {
            CHECK(rc == MOQ_ERR_NOMEM && out == NULL);
            CHECK(*ctx->alloc_calls == calls + nth);
        }
        CHECK(*ctx->live == live);
        if (succeeded) break;
    }
    CHECK(succeeded);
    CHECK(storage.before == UINT32_C(0xa5a5a5a5));
    for (size_t i = 0; i < sizeof(storage.after); ++i) CHECK(storage.after[i] == 0xa5);
    return failures;
}

#undef ORIGINAL_SIZE
#undef CHECK
