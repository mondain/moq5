#include "../../core/src/session/location_d22_internal.h"
#include "test_support.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Pin the frozen prefix independently of pointer width and uint64 alignment. */
_Static_assert(offsetof(moq_location_filter_cfg_t, struct_size) == 0,
               "location cfg struct_size offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, kind) == 4,
               "location cfg kind offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, relative_group_offset) == 8,
               "location cfg relative_group_offset offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, start_group) == 16,
               "location cfg start_group offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, start_object) == 24,
               "location cfg start_object offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, end_group) == 32,
               "location cfg end_group offset");
_Static_assert(offsetof(moq_location_filter_cfg_t, end_object) == 40,
               "location cfg end_object offset");
_Static_assert(MOQ_LOCATION_FILTER_CFG_V0_SIZE == 48,
               "location cfg frozen v0 size");
_Static_assert(sizeof(moq_location_filter_cfg_t) >= MOQ_LOCATION_FILTER_CFG_V0_SIZE,
               "location cfg must contain its frozen prefix");

static int test_sizes(void)
{
    int failures = 0;
    for (size_t size = 0; size <= sizeof(moq_location_filter_cfg_t) + 8; ++size) {
        uint8_t *bytes = malloc(size + 8);
        MOQ_TEST_CHECK(bytes != NULL);
        if (!bytes) continue;
        memset(bytes, 0xa5, size + 8);
        moq_location_filter_cfg_init_sized((moq_location_filter_cfg_t *)bytes, size);
        size_t written = size < sizeof(uint32_t) ? 0 : size;
        if (written > sizeof(moq_location_filter_cfg_t))
            written = sizeof(moq_location_filter_cfg_t);
        if (written) {
            uint32_t stamp = 0;
            memcpy(&stamp, bytes, sizeof(stamp));
            MOQ_TEST_CHECK_EQ_SIZE(stamp, written);
            for (size_t i = sizeof(stamp); i < written; ++i)
                MOQ_TEST_CHECK(bytes[i] == 0);
        }
        for (size_t i = written; i < size + 8; ++i)
            MOQ_TEST_CHECK(bytes[i] == 0xa5);
        free(bytes);
    }
    moq_location_filter_cfg_t cfg;
    memset(&cfg, 0xa5, sizeof(cfg));
    moq_location_filter_cfg_init(&cfg);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.struct_size, sizeof(cfg));
    MOQ_TEST_CHECK_EQ_INT(cfg.kind, MOQ_LOCATION_FILTER_NONE);
    moq_location_filter_cfg_init_sized(NULL, sizeof(cfg));
    MOQ_TEST_PASS("d22.location_cfg.sized_init_canaries");
    return failures;
}

static int test_semantics(void)
{
    int failures = 0;
    static const struct {
        moq_location_filter_kind_t kind;
        uint8_t wire[6];
        size_t len;
    } rows[] = {
        {MOQ_LOCATION_FILTER_NONE, {0}, 1},
        {MOQ_LOCATION_FILTER_RELATIVE_GROUP, {1,2}, 2},
        {MOQ_LOCATION_FILTER_ABSOLUTE_START, {2,7,3}, 3},
        {MOQ_LOCATION_FILTER_GROUP_END, {3,7,3,2}, 4},
        {MOQ_LOCATION_FILTER_OBJECT_END, {4,7,3,2,0}, 5},
        {MOQ_LOCATION_FILTER_NEXT_OBJECT, {5}, 1},
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i) {
        moq_location_filter_cfg_t cfg = {.struct_size = sizeof(cfg),
            .kind = rows[i].kind, .relative_group_offset = 2,
            .start_group = 7, .start_object = 3, .end_group = 9, .end_object = 0};
        moq_d22_location_filter_t out;
        moq_result_t rc = moq_d22_from_location_cfg(&cfg, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) continue;
        uint8_t wire[40] = {0};
        moq_buf_writer_t w = {wire, sizeof(wire), 0};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w, &out), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, rows[i].len);
        MOQ_TEST_CHECK(memcmp(wire, rows[i].wire, rows[i].len) == 0);
    }
    moq_location_filter_cfg_t cfg = {.struct_size = sizeof(cfg),
        .kind = MOQ_LOCATION_FILTER_OBJECT_END, .start_group = UINT64_MAX,
        .start_object = UINT64_MAX, .end_group = UINT64_MAX, .end_object = UINT64_MAX};
    moq_d22_location_filter_t out = {0};
    moq_result_t rc = moq_d22_from_location_cfg(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(out.end_group_delta, 0);
        MOQ_TEST_CHECK_EQ_U64(out.end_object, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_INT(out.type, MOQ_D22_FILTER_RANGE);
    }
    cfg.kind = MOQ_LOCATION_FILTER_NONE;
    cfg.end_group = 0; /* inactive incoherent range does not invalidate NONE */
    MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(&cfg, &out), MOQ_OK);
    cfg.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    cfg.relative_group_offset = UINT64_MAX;
    rc = moq_d22_from_location_cfg(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) MOQ_TEST_CHECK_EQ_U64(out.start_group, UINT64_MAX);
    MOQ_TEST_PASS("d22.location_cfg.explicit_inclusive_semantics");
    return failures;
}

static int test_refusal(void)
{
    int failures = 0;
    moq_d22_location_filter_t out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    /* Exact allocations make an early field read visible under ASan at every
     * truncated v0 prefix, including reads gated on a partial field's offset. */
    for (size_t size = sizeof(uint32_t); size < MOQ_LOCATION_FILTER_CFG_V0_SIZE;
         ++size) {
        uint8_t *prefix = malloc(size);
        MOQ_TEST_CHECK(prefix != NULL);
        if (!prefix) continue;
        memset(prefix, 0xa5, size);
        uint32_t stamp = (uint32_t)size;
        memcpy(prefix, &stamp, sizeof(stamp));
        memset(&out, 0xa5, sizeof(out));
        MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(
            (const moq_location_filter_cfg_t *)prefix, &out), MOQ_ERR_ABI_MISMATCH);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        free(prefix);
    }
    moq_location_filter_cfg_t cfg = {.struct_size = sizeof(cfg), .kind = 99};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(&cfg, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    cfg.kind = MOQ_LOCATION_FILTER_OBJECT_END;
    cfg.start_group = 2;
    cfg.end_group = 1;
    MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(&cfg, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(NULL, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_d22_from_location_cfg(&cfg, NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    MOQ_TEST_PASS("d22.location_cfg.refusal_and_exact_short_allocations");
    return failures;
}

static int test_empty_object_range(void)
{
    int failures = 0;
    const moq_location_filter_cfg_t cfg = {.struct_size = sizeof(cfg),
        .kind = MOQ_LOCATION_FILTER_OBJECT_END, .start_group = 3,
        .start_object = 5, .end_group = 3, .end_object = 4};
    const uint8_t expected[] = {4, 3, 5, 0, 4};
    moq_d22_location_filter_t out;
    memset(&out, 0xa5, sizeof(out));
    moq_result_t rc = moq_d22_from_location_cfg(&cfg, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_INT(out.type, MOQ_D22_FILTER_RANGE);
        MOQ_TEST_CHECK_EQ_U64(out.start_group, 3);
        MOQ_TEST_CHECK_EQ_U64(out.start_object, 5);
        MOQ_TEST_CHECK_EQ_U64(out.end_group_delta, 0);
        MOQ_TEST_CHECK_EQ_U64(out.end_object, 4);
        uint8_t wire[sizeof(expected)] = {0};
        moq_buf_writer_t w = {wire, sizeof(wire), 0};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w, &out), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, sizeof(expected));
        MOQ_TEST_CHECK(memcmp(wire, expected, sizeof(expected)) == 0);
    }
    MOQ_TEST_PASS("d22.location_cfg.empty_same_group_object_range");
    return failures;
}

int main(void)
{
    int failures = test_sizes() + test_semantics() + test_refusal()
        + test_empty_object_range();
    printf("location cfg: %d failures\n", failures);
    return failures ? 1 : 0;
}
