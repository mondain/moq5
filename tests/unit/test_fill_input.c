#include "../../core/src/session/fill_input_internal.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/fill_owner_internal.h"
#include "moq/session.h"
#include "test_support.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(moq_fill_t) == 8, "fill handle width");
_Static_assert(offsetof(moq_fill_cfg_t, struct_size) == 0, "fill size offset");
_Static_assert(offsetof(moq_fill_cfg_t, has_subscriber_priority) == 4, "priority presence offset");
_Static_assert(offsetof(moq_fill_cfg_t, subscriber_priority) == 5, "priority offset");
_Static_assert(offsetof(moq_fill_cfg_t, has_group_order) == 6, "order presence offset");
_Static_assert(offsetof(moq_fill_cfg_t, has_fill_timeout) == 7, "timeout presence offset");
_Static_assert(offsetof(moq_fill_cfg_t, fill_timeout_ms) == 8, "timeout offset");
_Static_assert(offsetof(moq_fill_cfg_t, group_order) == 16, "order offset");
_Static_assert(sizeof(((moq_fill_cfg_t *)0)->group_order) == 4, "fixed-width order");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(moq_fill_cfg_t, location) == 24, "LP64 location offset");
_Static_assert(MOQ_FILL_CFG_V0_SIZE == 32, "LP64 floor");
#else
_Static_assert(offsetof(moq_fill_cfg_t, location) == 20, "ILP32 location offset");
_Static_assert(MOQ_FILL_CFG_V0_SIZE == 24, "ILP32 floor");
#endif
_Static_assert(sizeof(moq_fill_cfg_t) == MOQ_FILL_CFG_V0_SIZE, "initial fill layout");

static int test_init(void)
{
    int failures = 0;
    for (size_t n = 0; n <= sizeof(moq_fill_cfg_t) + 9; ++n) {
        uint8_t *p = malloc(n + 8);
        MOQ_TEST_CHECK(p != NULL);
        if (!p) continue;
        memset(p, 0xa5, n + 8);
        moq_fill_cfg_init_sized((moq_fill_cfg_t *)p, n);
        size_t written = n < 4 ? 0 : n;
        if (written > sizeof(moq_fill_cfg_t)) written = sizeof(moq_fill_cfg_t);
        if (written) {
            uint32_t stamp;
            memcpy(&stamp, p, sizeof(stamp));
            MOQ_TEST_CHECK_EQ_SIZE(stamp, written);
            for (size_t i = 4; i < written; ++i) MOQ_TEST_CHECK(p[i] == 0);
        }
        for (size_t i = written; i < n + 8; ++i) MOQ_TEST_CHECK(p[i] == 0xa5);
        free(p);
    }
    moq_fill_cfg_t cfg = {0};
    moq_fill_cfg_init(&cfg);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.struct_size, sizeof(cfg));
    MOQ_TEST_CHECK(!cfg.has_subscriber_priority && !cfg.has_group_order);
    MOQ_TEST_CHECK(!cfg.has_fill_timeout && cfg.location == NULL);
    moq_fill_cfg_init_sized(NULL, SIZE_MAX);
    moq_fill_cfg_init_sized(&cfg, SIZE_MAX);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.struct_size, sizeof(cfg));
    return failures;
}

static int test_normalize(void)
{
    int failures = 0;
    moq_fill_input_t out = {0};
    MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(NULL, &out), MOQ_OK);
    MOQ_TEST_CHECK(!out.requested && !out.has_location);
    moq_fill_cfg_t cfg = {.struct_size = sizeof(cfg),
        .subscriber_priority = 255, .group_order = UINT32_MAX,
        .fill_timeout_ms = UINT64_MAX};
    moq_result_t rc = moq_fill_input_normalize(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK(out.requested && !out.has_subscriber_priority);
        MOQ_TEST_CHECK(!out.has_group_order && !out.has_fill_timeout);
        MOQ_TEST_CHECK_EQ_U64(out.fill_timeout_ms, 0);
        MOQ_TEST_CHECK_EQ_INT(out.group_order, 0);
        MOQ_TEST_CHECK_EQ_INT(out.subscriber_priority, 0);
        MOQ_TEST_CHECK(!out.has_location);
    }
    for (uint32_t order = 1; order <= 2; ++order) {
        cfg.has_subscriber_priority = cfg.has_group_order = cfg.has_fill_timeout = true;
        cfg.group_order = order;
        cfg.fill_timeout_ms = 0;
        rc = moq_fill_input_normalize(&cfg, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc == MOQ_OK) {
            MOQ_TEST_CHECK(out.has_subscriber_priority && out.has_group_order && out.has_fill_timeout);
            MOQ_TEST_CHECK_EQ_INT(out.subscriber_priority, 255);
            MOQ_TEST_CHECK_EQ_INT(out.group_order, order);
            MOQ_TEST_CHECK_EQ_U64(out.fill_timeout_ms, 0);
        }
    }
    cfg.fill_timeout_ms = UINT64_MAX;
    moq_location_filter_cfg_t loc = {.struct_size = sizeof(loc),
        .kind = MOQ_LOCATION_FILTER_OBJECT_END, .start_group = UINT64_MAX,
        .start_object = UINT64_MAX, .end_group = UINT64_MAX, .end_object = UINT64_MAX};
    cfg.location = &loc;
    moq_fill_cfg_t cfg_before;
    moq_location_filter_cfg_t loc_before;
    memcpy(&cfg_before, &cfg, sizeof(cfg));
    memcpy(&loc_before, &loc, sizeof(loc));
    rc = moq_fill_input_normalize(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(memcmp(&cfg, &cfg_before, sizeof(cfg)) == 0);
    MOQ_TEST_CHECK(memcmp(&loc, &loc_before, sizeof(loc)) == 0);
    if (rc == MOQ_OK) {
        memset(&cfg, 0, sizeof(cfg));
        memset(&loc, 0, sizeof(loc));
        MOQ_TEST_CHECK(out.requested && out.has_location && out.has_fill_timeout);
        MOQ_TEST_CHECK_EQ_INT(out.filter_type, MOQ_SUBSCRIBE_FILTER_OBJECT_END_INTERNAL);
        MOQ_TEST_CHECK_EQ_U64(out.start_group, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(out.start_object, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(out.end_group, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(out.end_object, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(out.fill_timeout_ms, UINT64_MAX);
    }
    loc = (moq_location_filter_cfg_t){.struct_size = sizeof(loc),
        .kind = MOQ_LOCATION_FILTER_NONE, .start_group = UINT64_MAX, .end_group = 0};
    cfg = (moq_fill_cfg_t){.struct_size = sizeof(cfg), .location = &loc};
    rc = moq_fill_input_normalize(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK(out.has_location);
        MOQ_TEST_CHECK_EQ_INT(out.filter_type, MOQ_SUBSCRIBE_FILTER_NONE);
    }
    loc.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    loc.relative_group_offset = UINT64_MAX;
    rc = moq_fill_input_normalize(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) MOQ_TEST_CHECK_EQ_U64(out.start_group, UINT64_MAX);
    struct { moq_fill_cfg_t cfg; uint64_t future[2]; } future;
    memset(&future, 0xa5, sizeof(future));
    future.cfg = (moq_fill_cfg_t){.struct_size = sizeof(future)};
    const uint64_t tail = future.future[0];
    rc = moq_fill_input_normalize(&future.cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) MOQ_TEST_CHECK(out.requested && !out.has_location);
    MOQ_TEST_CHECK_EQ_U64(future.future[0], tail);
    struct { moq_location_filter_cfg_t cfg; uint64_t future; } future_loc = {
        .cfg = {.struct_size = sizeof(future_loc), .kind = MOQ_LOCATION_FILTER_ABSOLUTE_START,
                .start_group = UINT64_MAX, .start_object = UINT64_MAX},
        .future = UINT64_C(0xfedcba9876543210)
    };
    cfg = (moq_fill_cfg_t){.struct_size = sizeof(cfg), .location = &future_loc.cfg};
    rc = moq_fill_input_normalize(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_INT(out.filter_type, MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START);
        MOQ_TEST_CHECK_EQ_U64(out.start_group, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(out.start_object, UINT64_MAX);
    }
    MOQ_TEST_CHECK_EQ_U64(future_loc.future, UINT64_C(0xfedcba9876543210));
    rc = moq_fill_input_normalize(NULL, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK(!out.requested && !out.has_location && !out.has_group_order);
        MOQ_TEST_CHECK(!out.has_fill_timeout && !out.has_subscriber_priority);
    }
    /* An actual exact-v0 allocation, not just a short stamp in a full cfg. */
    moq_fill_cfg_t *exact = malloc(MOQ_FILL_CFG_V0_SIZE);
    MOQ_TEST_CHECK(exact != NULL);
    if (exact) {
        memset(exact, 0, MOQ_FILL_CFG_V0_SIZE);
        exact->struct_size = MOQ_FILL_CFG_V0_SIZE;
        rc = moq_fill_input_normalize(exact, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc == MOQ_OK) MOQ_TEST_CHECK(out.requested && !out.has_location);
        free(exact);
    }
    return failures;
}

static int test_refusal(void)
{
    int failures = 0;
    moq_fill_input_t out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    for (uint32_t stamp = 0; stamp < 4; ++stamp) {
        uint32_t *p = malloc(sizeof(*p));
        MOQ_TEST_CHECK(p != NULL);
        if (!p) continue;
        *p = stamp;
        MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize((const moq_fill_cfg_t *)p,
            &out), MOQ_ERR_ABI_MISMATCH);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        free(p);
    }
    for (size_t n = 4; n < MOQ_FILL_CFG_V0_SIZE; ++n) {
        uint8_t *p = malloc(n);
        MOQ_TEST_CHECK(p != NULL);
        if (!p) continue;
        memset(p, 0xa5, n);
        uint32_t stamp = (uint32_t)n;
        memcpy(p, &stamp, sizeof(stamp));
        MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize((const moq_fill_cfg_t *)p,
            &out), MOQ_ERR_ABI_MISMATCH);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        free(p);
    }
    moq_fill_cfg_t cfg = {.struct_size = sizeof(cfg), .has_group_order = true};
    const uint32_t invalid_orders[] = {0, 3, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid_orders)/sizeof(invalid_orders[0]); ++i) {
        cfg.group_order = invalid_orders[i];
        MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(&cfg, &out), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    cfg.has_group_order = false;
    cfg.has_subscriber_priority = true;
    cfg.subscriber_priority = 7;
    for (size_t n = 4; n < MOQ_LOCATION_FILTER_CFG_V0_SIZE; ++n) {
        uint8_t *p = malloc(n);
        MOQ_TEST_CHECK(p != NULL);
        if (!p) continue;
        memset(p, 0xa5, n);
        uint32_t stamp = (uint32_t)n;
        memcpy(p, &stamp, sizeof(stamp));
        cfg.location = (const moq_location_filter_cfg_t *)p;
        MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(&cfg, &out), MOQ_ERR_ABI_MISMATCH);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        free(p);
    }
    moq_location_filter_cfg_t loc = {.struct_size = sizeof(loc), .kind = UINT32_MAX};
    cfg.location = &loc;
    MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(&cfg, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    loc.kind = MOQ_LOCATION_FILTER_GROUP_END;
    loc.start_group = 2;
    loc.end_group = 1;
    MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(&cfg, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(&cfg, NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_fill_input_normalize(NULL, NULL), MOQ_ERR_INVAL);
    return failures;
}

static int test_handles(void)
{
    int failures = 0;
    const uint64_t generations[] = {1, 2, UINT64_C(0x08000000)};
    const uint32_t packed_generations[] = {1, 3, 0x0fffffff};
    const size_t slots[] = {0, 65535};
    for (size_t g = 0; g < 3; ++g) {
        for (size_t k = 0; k < 2; ++k) {
            moq_fill_t h = MOQ_FILL_INVALID;
            moq_result_t rc = moq_fill_handle_encode(7, slots[k], generations[g], &h);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc != MOQ_OK) continue;
            MOQ_TEST_CHECK(moq_fill_is_valid(h));
            MOQ_TEST_CHECK_EQ_INT(moq_handle_pool_tag(h._opaque), MOQ_HANDLE_POOL_FILL);
            MOQ_TEST_CHECK_EQ_INT(moq_handle_generation(h._opaque), packed_generations[g]);
            MOQ_TEST_CHECK_EQ_SIZE(moq_handle_slot(h._opaque), slots[k]);
            moq_fill_identity_t identity = {0};
            rc = moq_fill_handle_decode(7, h, &identity);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc == MOQ_OK) {
                MOQ_TEST_CHECK_EQ_SIZE(identity.slot, slots[k]);
                MOQ_TEST_CHECK_EQ_U64(identity.generation, generations[g]);
            }
            MOQ_TEST_CHECK(moq_fill_eq(h, h));
            MOQ_TEST_CHECK(!moq_fill_eq(h, MOQ_FILL_INVALID));
            MOQ_TEST_CHECK_EQ_U64(moq_fill_hash(h), h._opaque * UINT64_C(0x9e3779b97f4a7c15));
            MOQ_TEST_CHECK_EQ_U64(moq_fill_id_for_trace(h), h._opaque);
        }
    }
    /* Independently authored literal, not just encode/decode round trips. */
    const moq_fill_t literal = {UINT64_C(0xa000700000030005)};
    moq_fill_identity_t out, before;
    memset(&out, 0xa5, sizeof(out));
    moq_result_t rc = moq_fill_handle_decode(7, literal, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_SIZE(out.slot, 5);
        MOQ_TEST_CHECK_EQ_U64(out.generation, 2);
    }
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    for (uint32_t pool = 0; pool <= 15; ++pool) {
        if (pool == MOQ_HANDLE_POOL_FILL) continue;
        const moq_fill_t wrong = {(literal._opaque & UINT64_C(0x0fffffffffffffff)) |
                                 ((uint64_t)pool << 60)};
        MOQ_TEST_CHECK(!moq_fill_is_valid(wrong));
        MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_decode(7, wrong, &out), MOQ_ERR_STALE_HANDLE);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    const moq_fill_t invalid[] = {{0},
        {UINT64_C(0xa000700000020005)}, {UINT64_C(0xa000000000030005)}};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        MOQ_TEST_CHECK(!moq_fill_is_valid(invalid[i]));
        MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_decode(7, invalid[i], &out), MOQ_ERR_STALE_HANDLE);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_decode(8, literal, &out), MOQ_ERR_WRONG_SESSION);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_decode(0, literal, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_decode(7, literal, NULL), MOQ_ERR_INVAL);

    /* These helpers receive no table authority: a refused representation must
     * not lead this caller to commit its separately held owner reservation. */
    moq_fill_entry_t entry = {.generation = UINT64_C(0x08000001), .active = false};
    moq_fill_entry_t entry_before;
    memcpy(&entry_before, &entry, sizeof(entry));
    moq_fill_t h = literal;
    const uint64_t bad_generations[] = {0, entry.generation,
        UINT64_C(0x100000002), UINT64_MAX};
    for (size_t i = 0; i < sizeof(bad_generations)/sizeof(bad_generations[0]); ++i) {
        MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_encode(7, 0, bad_generations[i], &h), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_U64(h._opaque, literal._opaque);
        MOQ_TEST_CHECK(memcmp(&entry, &entry_before, sizeof(entry)) == 0);
    }
    const size_t bad_slots[] = {65536, SIZE_MAX};
    for (size_t i = 0; i < sizeof(bad_slots)/sizeof(bad_slots[0]); ++i) {
        MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_encode(7, bad_slots[i], 1, &h), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_U64(h._opaque, literal._opaque);
    }
#if SIZE_MAX > UINT32_MAX
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_encode(7, (size_t)UINT64_C(0x100000000),
        1, &h), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_U64(h._opaque, literal._opaque);
#endif
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_encode(0, 0, 1, &h), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_U64(h._opaque, literal._opaque);
    MOQ_TEST_CHECK_EQ_INT(moq_fill_handle_encode(7, 0, 1, NULL), MOQ_ERR_INVAL);
    return failures;
}

int main(void)
{
    int failures = 0;
#define RUN_TEST(fn) do { int n = fn(); failures += n; \
    printf("%s %s (%d failures)\n", n ? "FAIL" : "PASS", #fn, n); } while (0)
    RUN_TEST(test_init);
    RUN_TEST(test_normalize);
    RUN_TEST(test_refusal);
    RUN_TEST(test_handles);
#undef RUN_TEST
    printf("fill input: %d failures\n", failures);
    return failures ? 1 : 0;
}
