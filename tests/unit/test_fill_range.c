#include "../../core/src/session/fill_range_internal.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include "test_support.h"
#include <inttypes.h>
#include <string.h>

/* The cases are written in the wire's filter form; the resolver takes the
 * core's. The mapping is the one the draft-22 profile applies on receipt,
 * including its refusal of an end-group overflow. */
static moq_result_t resolve_d22(const moq_d22_location_filter_t *f, bool has_largest,
    uint64_t lg, uint64_t lo, bool paused, moq_fill_range_t *out)
{
    if (!f) return MOQ_ERR_INVAL;
    uint32_t type; uint64_t sg = 0, so = 0, eg = 0, eo = 0;
    switch (f->type) {
    case MOQ_D22_FILTER_NONE: type = MOQ_SUBSCRIBE_FILTER_NONE; break;
    case MOQ_D22_FILTER_NEXT_OBJECT: type = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT; break;
    case MOQ_D22_FILTER_RELATIVE:
        if (f->start_group == 0) type = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        else { type = MOQ_SUBSCRIBE_FILTER_RELATIVE_GROUP_INTERNAL; sg = f->start_group; }
        break;
    case MOQ_D22_FILTER_ABSOLUTE:
        type = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START; sg = f->start_group; so = f->start_object; break;
    case MOQ_D22_FILTER_GROUP_END:
    case MOQ_D22_FILTER_RANGE:
        if (f->end_group_delta > UINT64_MAX - f->start_group) return MOQ_ERR_PROTO;
        type = f->type == MOQ_D22_FILTER_GROUP_END ? MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE
                                                   : MOQ_SUBSCRIBE_FILTER_OBJECT_END_INTERNAL;
        sg = f->start_group; so = f->start_object;
        eg = f->start_group + f->end_group_delta; eo = f->end_object; break;
    default:
        return MOQ_ERR_PROTO;
    }
    return moq_fill_range_resolve(type, sg, so, eg, eo, has_largest, lg, lo, paused, out);
}

#define STREAM(sg, so, eg, eo) \
    ((moq_fill_range_t){ .outcome = MOQ_FILL_RANGE_STREAM, \
        .start_group = (sg), .start_object = (so), .end_group = (eg), .end_object = (eo) })
#define NONE(reason) ((moq_fill_range_t){ .outcome = (reason) })

static int expect(const char *name, const moq_d22_location_filter_t *filter,
    bool has_largest, uint64_t lg, uint64_t lo, bool paused, moq_result_t result,
    moq_fill_range_t expected)
{
    int failures = 0;
    struct guarded {
        uint64_t before;
        moq_fill_range_t range;
        uint64_t after;
    } output;
    memset(&output, 0xa5, sizeof(output));
    output.before = UINT64_C(0x0f3aef38c9190401);
    output.after = UINT64_C(0x95e118f404561ef7);
    moq_fill_range_t saved;
    memcpy(&saved, &output.range, sizeof(saved));
    moq_d22_location_filter_t input;
    if (filter) memcpy(&input, filter, sizeof(input));
    moq_result_t rc = resolve_d22(filter, has_largest, lg, lo, paused, &output.range);
    if (rc != result) {
        fprintf(stderr, "FAIL: %s: result %d, expected %d\n", name, rc, result);
        failures++;
    }
    if (output.before != UINT64_C(0x0f3aef38c9190401) ||
        output.after != UINT64_C(0x95e118f404561ef7)) {
        fprintf(stderr, "FAIL: %s: output boundary canary changed\n", name);
        failures++;
    }
    if (filter && memcmp(&input, filter, sizeof(input)) != 0) {
        fprintf(stderr, "FAIL: %s: input mutated\n", name);
        failures++;
    }
    if (result != MOQ_OK) {
        if (memcmp(&saved, &output.range, sizeof(saved)) != 0) {
            fprintf(stderr, "FAIL: %s: refused output changed\n", name);
            failures++;
        }
        return failures;
    }
    if (rc != MOQ_OK) return failures;
#define FIELD(member) do { \
    if (output.range.member != expected.member) { \
        fprintf(stderr, "FAIL: %s: " #member "=%" PRIu64 ", expected %" PRIu64 "\n", \
            name, (uint64_t)output.range.member, (uint64_t)expected.member); \
        failures++; \
    } \
} while (0)
    FIELD(outcome);
    FIELD(start_group);
    FIELD(start_object);
    FIELD(end_group);
    FIELD(end_object);
#undef FIELD
    return failures;
}

/* Independent order embedding for the small grid: object coordinates
 * 0,1,2,UINT64_MAX map to ranks 0,1,2,3 within each group. All grid input
 * endpoints belong to that set, so interval intersection is integer min/max. */
static moq_fill_range_t grid_expected(int start, int end, int largest)
{
    if (end < start) return NONE(MOQ_FILL_RANGE_NO_STREAM_EMPTY);
    if (start > largest) return NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST);
    if (end > largest) end = largest;
    return STREAM((uint64_t)(start / 4), (uint64_t)(start % 4),
        (uint64_t)(end / 4), end % 4 == 3 ? UINT64_MAX : (uint64_t)(end % 4));
}

static int test_small_grid(void)
{
    int failures = 0;
    char name[128];
    for (int lg = 0; lg < 3; ++lg) {
        for (int lo = 0; lo < 3; ++lo) {
            int largest = 4 * lg + lo;
            moq_d22_location_filter_t f = { .type = MOQ_D22_FILTER_NONE };
            snprintf(name, sizeof(name), "grid.none.largest_%d_%d", lg, lo);
            failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                MOQ_OK, STREAM(0, 0, (uint64_t)lg, (uint64_t)lo));
            f.type = MOQ_D22_FILTER_NEXT_OBJECT;
            snprintf(name, sizeof(name), "grid.next.largest_%d_%d", lg, lo);
            failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                MOQ_OK, NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST));
            for (int offset = 0; offset < 6; ++offset) {
                f.type = MOQ_D22_FILTER_RELATIVE;
                f.start_group = (uint64_t)offset;
                int start_group = lg + 1 - offset;
                if (start_group < 0) start_group = 0;
                snprintf(name, sizeof(name), "grid.relative_%d.largest_%d_%d", offset, lg, lo);
                failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                    MOQ_OK, grid_expected(4 * start_group, 24, largest));
            }
            for (int sg = 0; sg < 3; ++sg) {
                for (int so = 0; so < 3; ++so) {
                    f.type = MOQ_D22_FILTER_ABSOLUTE;
                    f.start_group = (uint64_t)sg;
                    f.start_object = (uint64_t)so;
                    snprintf(name, sizeof(name), "grid.absolute_%d_%d.largest_%d_%d", sg, so, lg, lo);
                    failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                        MOQ_OK, grid_expected(4 * sg + so, 24, largest));
                    for (int delta = 0; delta < 3; ++delta) {
                        f.type = MOQ_D22_FILTER_GROUP_END;
                        f.end_group_delta = (uint64_t)delta;
                        f.end_object = UINT64_MAX;
                        snprintf(name, sizeof(name), "grid.group_end_%d_%d_%d.largest_%d_%d", sg, so, delta, lg, lo);
                        failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                            MOQ_OK, grid_expected(4 * sg + so, 4 * (sg + delta) + 3, largest));
                        for (int eo = 0; eo < 3; ++eo) {
                            f.type = MOQ_D22_FILTER_RANGE;
                            f.end_object = (uint64_t)eo;
                            snprintf(name, sizeof(name), "grid.range_%d_%d_%d_%d.largest_%d_%d", sg, so, delta, eo, lg, lo);
                            failures += expect(name, &f, true, (uint64_t)lg, (uint64_t)lo, false,
                                MOQ_OK, grid_expected(4 * sg + so, 4 * (sg + delta) + eo, largest));
                        }
                    }
                }
            }
        }
    }
    MOQ_TEST_PASS("fill.range.small_complete_grid");
    return failures;
}

static int test_relative_extremes(void)
{
    int failures = 0;
    const struct {
        const char *name;
        uint64_t largest, offset, start;
        bool after;
    } cases[] = {
        { "relative.max.offset0", UINT64_MAX, 0, UINT64_MAX, false },
        { "relative.max.offset1", UINT64_MAX, 1, UINT64_MAX, false },
        { "relative.max.offset2", UINT64_MAX, 2, UINT64_MAX - 1, false },
        { "relative.max.offsetmax", UINT64_MAX, UINT64_MAX, 1, false },
        { "relative.maxminus1.offset0", UINT64_MAX - 1, 0, 0, true },
        { "relative.maxminus1.offset1", UINT64_MAX - 1, 1, UINT64_MAX - 1, false },
        { "relative.maxminus1.offset2", UINT64_MAX - 1, 2, UINT64_MAX - 2, false },
        { "relative.maxminus1.offsetmax", UINT64_MAX - 1, UINT64_MAX, 0, false },
        { "relative.zero.offset0", 0, 0, 0, true },
        { "relative.zero.offset1", 0, 1, 0, false },
        { "relative.zero.offset2", 0, 2, 0, false },
        { "relative.zero.offsetmax", 0, UINT64_MAX, 0, false },
        { "relative.one.offset3", 1, 3, 0, false },
        { "relative.one.offset4", 1, 4, 0, false }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        moq_d22_location_filter_t f = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = cases[i].offset };
        moq_fill_range_t expected = cases[i].after ?
            NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST) : STREAM(cases[i].start, 0, cases[i].largest, 7);
        failures += expect(cases[i].name, &f, true, cases[i].largest, 7, false, MOQ_OK, expected);
    }
    MOQ_TEST_PASS("fill.range.relative_clamps_whole_expression");
    return failures;
}

static int test_end_and_object_extremes(void)
{
    int failures = 0;
    const struct {
        const char *name;
        moq_d22_location_filter_t filter;
        uint64_t lg, lo;
        moq_fill_range_t expected;
    } cases[] = {
        { "whole_group.earlier.object_max", { .type = MOQ_D22_FILTER_GROUP_END, .start_group = 2,
            .start_object = 10 }, 4, 0, STREAM(2, 10, 2, UINT64_MAX) },
        { "whole_group.cross_earlier.object_max", { .type = MOQ_D22_FILTER_GROUP_END, .start_group = 1,
            .end_group_delta = 1 }, 4, 7, STREAM(1, 0, 2, UINT64_MAX) },
        { "whole_group.current.clips_object", { .type = MOQ_D22_FILTER_GROUP_END, .start_group = 2,
            .end_group_delta = 2 }, 4, 7, STREAM(2, 0, 4, 7) },
        { "whole_group.future.clips_group", { .type = MOQ_D22_FILTER_GROUP_END, .start_group = 2,
            .end_group_delta = 3 }, 4, 7, STREAM(2, 0, 4, 7) },
        { "range.earlier.retains_object", { .type = MOQ_D22_FILTER_RANGE, .start_group = 2,
            .end_group_delta = 1, .end_object = 99 }, 4, 7, STREAM(2, 0, 3, 99) },
        { "range.future.small_object_lexical_clip", { .type = MOQ_D22_FILTER_RANGE, .start_group = 2,
            .end_group_delta = 3, .end_object = 0 }, 4, 17, STREAM(2, 0, 4, 17) },
        { "range.same_group.exact_empty", { .type = MOQ_D22_FILTER_RANGE, .start_group = 2,
            .start_object = UINT64_MAX, .end_object = UINT64_MAX - 1 }, 2, UINT64_MAX,
            NONE(MOQ_FILL_RANGE_NO_STREAM_EMPTY) },
        { "range.same_group.last_object", { .type = MOQ_D22_FILTER_RANGE, .start_group = 2,
            .start_object = UINT64_MAX, .end_object = UINT64_MAX }, 2, UINT64_MAX,
            STREAM(2, UINT64_MAX, 2, UINT64_MAX) },
        { "absolute.max_location", { .type = MOQ_D22_FILTER_ABSOLUTE, .start_group = UINT64_MAX,
            .start_object = UINT64_MAX }, UINT64_MAX, UINT64_MAX,
            STREAM(UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX) },
        { "absolute.after_same_group", { .type = MOQ_D22_FILTER_ABSOLUTE, .start_group = 4,
            .start_object = UINT64_MAX }, 4, UINT64_MAX - 1, NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST) },
        { "next.max_object_never_wraps", { .type = MOQ_D22_FILTER_NEXT_OBJECT }, 4, UINT64_MAX,
            NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST) },
        { "next.max_location_never_wraps", { .type = MOQ_D22_FILTER_NEXT_OBJECT }, UINT64_MAX, UINT64_MAX,
            NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST) },
        { "none.max_snapshot", { .type = MOQ_D22_FILTER_NONE }, UINT64_MAX, UINT64_MAX,
            STREAM(0, 0, UINT64_MAX, UINT64_MAX) },
        { "group_end.max_delta_exact", { .type = MOQ_D22_FILTER_GROUP_END, .end_group_delta = UINT64_MAX },
            UINT64_MAX, UINT64_MAX, STREAM(0, 0, UINT64_MAX, UINT64_MAX) },
        { "range.max_group_delta_zero", { .type = MOQ_D22_FILTER_RANGE, .start_group = UINT64_MAX,
            .start_object = 1, .end_object = UINT64_MAX }, UINT64_MAX, UINT64_MAX,
            STREAM(UINT64_MAX, 1, UINT64_MAX, UINT64_MAX) }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        failures += expect(cases[i].name, &cases[i].filter, true, cases[i].lg, cases[i].lo,
            false, MOQ_OK, cases[i].expected);
    MOQ_TEST_PASS("fill.range.group_ends_and_max_objects");
    return failures;
}

static int test_validation_and_no_stream(void)
{
    int failures = 0;
    char name[128];
    for (int paused = 0; paused < 2; ++paused) {
        for (int have = 0; have < 2; ++have) {
            for (int type = MOQ_D22_FILTER_NONE; type <= MOQ_D22_FILTER_NEXT_OBJECT; ++type) {
                moq_d22_location_filter_t f = { .type = (moq_d22_filter_type_t)type };
                if (paused || !have) {
                    snprintf(name, sizeof(name), "no_stream.type%d.have%d.paused%d", type, have, paused);
                    failures += expect(name, &f, have != 0, UINT64_MAX, UINT64_MAX, paused != 0, MOQ_OK,
                        NONE(paused ? MOQ_FILL_RANGE_NO_STREAM_PAUSED : MOQ_FILL_RANGE_NO_STREAM_NO_LARGEST));
                }
            }
            for (int type = MOQ_D22_FILTER_GROUP_END; type <= MOQ_D22_FILTER_RANGE; ++type) {
                moq_d22_location_filter_t f = { .type = (moq_d22_filter_type_t)type,
                    .start_group = UINT64_MAX, .end_group_delta = 1 };
                snprintf(name, sizeof(name), "overflow.type%d.have%d.paused%d", type, have, paused);
                failures += expect(name, &f, have != 0, 3, 4, paused != 0, MOQ_ERR_PROTO, NONE(0));
                f.start_group = 1;
                f.end_group_delta = UINT64_MAX;
                failures += expect(name, &f, have != 0, 3, 4, paused != 0, MOQ_ERR_PROTO, NONE(0));
            }
            const int invalid[] = { -1, 6, 99 };
            for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
                moq_d22_location_filter_t f = { .type = (moq_d22_filter_type_t)invalid[i] };
                snprintf(name, sizeof(name), "unknown%d.have%d.paused%d", invalid[i], have, paused);
                failures += expect(name, &f, have != 0, 3, 4, paused != 0, MOQ_ERR_PROTO, NONE(0));
            }
            moq_d22_location_filter_t empty = { .type = MOQ_D22_FILTER_RANGE,
                .start_group = 9, .start_object = 7, .end_object = 1 };
            snprintf(name, sizeof(name), "reason_precedence.have%d.paused%d", have, paused);
            failures += expect(name, &empty, have != 0, 3, 4, paused != 0, MOQ_OK,
                NONE(paused ? MOQ_FILL_RANGE_NO_STREAM_PAUSED : !have ?
                    MOQ_FILL_RANGE_NO_STREAM_NO_LARGEST : MOQ_FILL_RANGE_NO_STREAM_EMPTY));
        }
    }
    failures += expect("null_filter", NULL, true, 0, 0, false, MOQ_ERR_INVAL, NONE(0));
    moq_d22_location_filter_t valid = { .type = MOQ_D22_FILTER_NONE };
    MOQ_TEST_CHECK_EQ_INT(resolve_d22(&valid, true, 0, 0, false, NULL), MOQ_ERR_INVAL);
    MOQ_TEST_PASS("fill.range.validation_precedes_no_stream_canaries");
    return failures;
}

static int test_inactive_and_snapshot(void)
{
    int failures = 0;
    moq_d22_location_filter_t f = { .type = MOQ_D22_FILTER_NONE,
        .start_group = UINT64_MAX, .start_object = UINT64_MAX,
        .end_group_delta = UINT64_MAX, .end_object = UINT64_MAX };
    failures += expect("inactive.none", &f, true, 4, 9, false, MOQ_OK, STREAM(0, 0, 4, 9));
    f.type = MOQ_D22_FILTER_NEXT_OBJECT;
    failures += expect("inactive.next", &f, true, 4, 9, false, MOQ_OK,
        NONE(MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST));
    f.type = MOQ_D22_FILTER_RELATIVE;
    f.start_group = 1;
    failures += expect("inactive.relative", &f, true, 4, 9, false, MOQ_OK, STREAM(4, 0, 4, 9));
    f.type = MOQ_D22_FILTER_ABSOLUTE;
    f.start_group = 2;
    f.start_object = 1;
    failures += expect("inactive.absolute", &f, true, 4, 9, false, MOQ_OK, STREAM(2, 1, 4, 9));
    f.type = MOQ_D22_FILTER_GROUP_END;
    f.end_group_delta = 0;
    f.end_object = 0;
    failures += expect("inactive.group_end_object", &f, true, 4, 9, false, MOQ_OK, STREAM(2, 1, 2, UINT64_MAX));
    moq_fill_range_t out = {0};
    uint64_t lg = 4, lo = 9;
    bool have = true, paused = false;
    MOQ_TEST_CHECK_EQ_INT(resolve_d22(&f, have, lg, lo, paused, &out), MOQ_OK);
    moq_fill_range_t saved;
    memcpy(&saved, &out, sizeof(saved));
    memset(&f, 0, sizeof(f));
    lg = 1; lo = 2; have = false; paused = true;
    MOQ_TEST_CHECK(memcmp(&out, &saved, sizeof(out)) == 0);
    MOQ_TEST_CHECK(out.outcome == MOQ_FILL_RANGE_STREAM && out.start_group == 2 &&
        out.start_object == 1 && out.end_group == 2 && out.end_object == UINT64_MAX);
    failures += expect("snapshot.new_call_does_not_change_old_value", &f, have, lg, lo, paused, MOQ_OK,
        NONE(MOQ_FILL_RANGE_NO_STREAM_PAUSED));
    MOQ_TEST_CHECK(memcmp(&out, &saved, sizeof(out)) == 0);
    MOQ_TEST_PASS("fill.range.inactive_fields_and_snapshot_value_ownership");
    return failures;
}

int main(void)
{
    int failures = test_small_grid() + test_relative_extremes() + test_end_and_object_extremes() +
        test_validation_and_no_stream() + test_inactive_and_snapshot();
    printf("fill range: %d failures\n", failures);
    return failures ? 1 : 0;
}
