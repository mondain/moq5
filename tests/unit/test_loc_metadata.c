#include <moq/loc.h>
#include "test_support.h"
#include "test_oom_support.h"
#include <stdlib.h>
#include <string.h>

#ifdef MOQ_LOC_OLD_MECHANISM_PROBE
/* One ordinary witness: the old normalized record cannot preserve the
 * independently specified four-byte LOC04 value. No library test seam. */
int main(void)
{
    int failures = 0;
    const uint8_t literal[] = {0x09, 4, 0xe8, 7, 0x11, 0x3e};
    moq_loc_headers_t h;
    moq_result_t rc = moq_loc_parse(MOQ_VERSION_DRAFT_22, MOQ_LOC_PROFILE_04,
        (moq_bytes_t){literal, sizeof(literal)}, &h);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return 1;
    moq_rcbuf_t *out = NULL;
    rc = moq_loc_encode(moq_alloc_default(), MOQ_VERSION_DRAFT_22,
                        MOQ_LOC_PROFILE_04, &h, &out);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(out != NULL);
    if (rc == MOQ_OK && out) {
        fprintf(stderr, "four-byte literal: %zu bytes; normalized re-encode: %zu bytes\n",
                sizeof(literal), moq_rcbuf_len(out));
        MOQ_TEST_CHECK_EQ_SIZE(moq_rcbuf_len(out), sizeof(literal));
        if (moq_rcbuf_len(out) == sizeof(literal))
            MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(out), literal, sizeof(literal)) == 0);
    }
    moq_rcbuf_decref(out);
    printf("CASE normalized_four_byte_loss: %s (%d failed checks)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
#else

#define D16 MOQ_VERSION_DRAFT_16
#define D18 MOQ_VERSION_DRAFT_18
#define D22 MOQ_VERSION_DRAFT_22
#define P01 MOQ_LOC_PROFILE_01
#define P04 MOQ_LOC_PROFILE_04

/* These bytes are literal spec vectors, not output from either LOC encoder. */
static const uint8_t full_literal[] = {
    0x08, 0x00,                         /* scale zero */
    0x01, 0x04, 0xe8, 7, 0x11, 0x3e,  /* marking, including opaque fourth byte */
    0x03, 0x80, 0xaa,                   /* raw V bit plus level 42 */
    0x01, 0x02, 0xab, 0xcd,             /* video config */
    0x02, 0x00,                         /* present-empty audio config */
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static int encoded_equals(moq_version_t version, moq_loc_profile_t profile,
    const moq_loc_metadata_t *m, const uint8_t *literal, size_t len)
{
    int failures = 0;
    moq_rcbuf_t *out = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(moq_alloc_default(), version,
        profile, m, &out), MOQ_OK);
    MOQ_TEST_CHECK((out != NULL) == (len != 0));
    if (out) {
        MOQ_TEST_CHECK_EQ_SIZE(moq_rcbuf_len(out), len);
        if (moq_rcbuf_len(out) == len)
            MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(out), literal, len) == 0);
    }
    moq_rcbuf_decref(out);
    return failures;
}

static int literal_and_immutable(void)
{
    int failures = 0;
    uint8_t wrapped[sizeof(full_literal) + 2];
    wrapped[0] = 0x0b;
    wrapped[1] = (uint8_t)sizeof(full_literal);
    memcpy(wrapped + 2, full_literal, sizeof(full_literal));
    for (size_t offset = 0; offset <= 2; offset += 2) {
        const uint8_t *input = offset ? wrapped : full_literal;
        size_t len = sizeof(full_literal) + offset;
        moq_loc_metadata_t m;
        memset(&m, 0xa5, sizeof(m));  /* parse does not require an input stamp */
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){input, len}, &m, sizeof(m)), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(m.struct_size, sizeof(m));
        MOQ_TEST_CHECK_EQ_U64(m.present, MOQ_LOC_METADATA_PRESENT_MASK);
        MOQ_TEST_CHECK_EQ_U64(m.timestamp, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(m.timescale, 0);
        MOQ_TEST_CHECK(m.marking_len == 4 &&
                       memcmp(m.marking, full_literal + 4, 4) == 0);
        MOQ_TEST_CHECK(m.audio_level == 0xaa);
        MOQ_TEST_CHECK(m.video_config.data == input + offset + 13);
        MOQ_TEST_CHECK_EQ_SIZE(m.video_config.len, 2);
        MOQ_TEST_CHECK(m.audio_config.data == input + offset + 17);
        MOQ_TEST_CHECK_EQ_SIZE(m.audio_config.len, 0);
        MOQ_TEST_CHECK(m.reserved[0] == 0 && m.reserved[1] == 0);
    }
    MOQ_TEST_CHECK(memcmp(wrapped + 2, full_literal, sizeof(full_literal)) == 0);

    /* Encoding is checked against the independent sorted literal, not a parse. */
    moq_loc_metadata_t m;
    moq_loc_metadata_init_sized(&m, sizeof(m));
    m.present = MOQ_LOC_METADATA_PRESENT_MASK;
    m.timestamp = UINT64_MAX;
    m.marking_len = 4;
    memcpy(m.marking, full_literal + 4, 4);
    m.audio_level = 0xaa;
    m.video_config = (moq_bytes_t){full_literal + 13, 2};
    m.audio_config = (moq_bytes_t){NULL, 0};
    failures += encoded_equals(D22, P04, &m, full_literal, sizeof(full_literal));

    /* Outer delta state survives an inner list; both spans retain original offsets. */
    const uint8_t mixed[] = {0x08, 0, 3, 4, 0x0f, 2, 0x31, 0x32,
                             2, 2, 0x41, 0x42, 3, 1};
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){mixed, sizeof(mixed)}, &m, sizeof(m)), MOQ_OK);
    MOQ_TEST_CHECK(m.present == (MOQ_LOC_METADATA_HAS_TIMESTAMP |
        MOQ_LOC_METADATA_HAS_TIMESCALE | MOQ_LOC_METADATA_HAS_VIDEO_CONFIG |
        MOQ_LOC_METADATA_HAS_AUDIO_CONFIG));
    MOQ_TEST_CHECK(m.timestamp == 1 && m.timescale == 0);
    MOQ_TEST_CHECK(m.video_config.data == mixed + 10 && m.video_config.len == 2);
    MOQ_TEST_CHECK(m.audio_config.data == mixed + 6 && m.audio_config.len == 2);

    /* Non-minimal inner integers are decoded without rewriting their backing. */
    uint8_t nonminimal[] = {0x0b, 4, 0x80, 0x0d, 0x80, 0};
    uint8_t saved[sizeof(nonminimal)];
    memcpy(saved, nonminimal, sizeof(saved));
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){nonminimal, sizeof(nonminimal)}, &m, sizeof(m)), MOQ_OK);
    MOQ_TEST_CHECK(m.present == MOQ_LOC_METADATA_HAS_VIDEO_CONFIG &&
                   m.video_config.len == 0 && m.video_config.data == nonminimal + 6);
    MOQ_TEST_CHECK(memcmp(nonminimal, saved, sizeof(saved)) == 0);

    /* Track and Object blocks remain separate, with no codec inheritance. */
    const uint8_t track[] = {8, 10, 5, 0};
    const uint8_t object[] = {0x10, 0};
    moq_loc_metadata_t t, o;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){track, sizeof(track)}, &t, sizeof(t)), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){object, sizeof(object)}, &o, sizeof(o)), MOQ_OK);
    MOQ_TEST_CHECK(t.present == (MOQ_LOC_METADATA_HAS_TIMESCALE |
                                 MOQ_LOC_METADATA_HAS_VIDEO_CONFIG));
    MOQ_TEST_CHECK(o.present == MOQ_LOC_METADATA_HAS_TIMESTAMP && o.timescale == 0);
    return failures;
}

static int raw_values_and_presence(void)
{
    int failures = 0;
    for (uint8_t n = 1; n <= 4; ++n) {
        const uint8_t literal[] = {9, n, 0xe8, 7, 0x11, 0x3e};
        moq_loc_metadata_t got, in;
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){literal, (size_t)n + 2}, &got, sizeof(got)), MOQ_OK);
        MOQ_TEST_CHECK(got.present == MOQ_LOC_METADATA_HAS_MARKING && got.marking_len == n);
        MOQ_TEST_CHECK(memcmp(got.marking, literal + 2, n) == 0);
        moq_loc_metadata_init_sized(&in, sizeof(in));
        in.present = MOQ_LOC_METADATA_HAS_MARKING;
        in.marking_len = n;
        memcpy(in.marking, literal + 2, n);
        failures += encoded_equals(D22, P04, &in, literal, (size_t)n + 2);
    }
    const uint8_t leading_zero[] = {9, 4, 0, 0, 0, 0};
    moq_loc_metadata_t m;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){leading_zero, sizeof(leading_zero)}, &m, sizeof(m)), MOQ_OK);
    failures += encoded_equals(D22, P04, &m, leading_zero, sizeof(leading_zero));

    const uint8_t config_literals[][4] = {{0x0d, 0}, {0x0f, 0}, {0x0d, 0, 2, 0}};
    const uint32_t masks[] = {MOQ_LOC_METADATA_HAS_VIDEO_CONFIG,
        MOQ_LOC_METADATA_HAS_AUDIO_CONFIG,
        MOQ_LOC_METADATA_HAS_VIDEO_CONFIG | MOQ_LOC_METADATA_HAS_AUDIO_CONFIG};
    for (size_t i = 0; i < 4; ++i) {
        size_t len = i == 3 ? 0 : (i == 2 ? 4 : 2);
        const uint8_t *data = i == 3 ? NULL : config_literals[i];
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){data, len}, &m, sizeof(m)), MOQ_OK);
        MOQ_TEST_CHECK(m.present == (i == 3 ? 0 : masks[i]));
        MOQ_TEST_CHECK(m.video_config.len == 0 && m.audio_config.len == 0);
        failures += encoded_equals(D22, P04, &m, data, len);
    }
    for (size_t i = 0; i < 4; ++i) {
        uint8_t literal[10] = {i < 2 ? 0x08 : 0x10, 0};
        size_t len = (i & 1) ? 10 : 2;
        if (i & 1) memset(literal + 1, 0xff, 9);
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){literal, len}, &m, sizeof(m)), MOQ_OK);
        uint64_t value = (i & 1) ? UINT64_MAX : 0;
        MOQ_TEST_CHECK(i < 2 ? m.timescale == value : m.timestamp == value);
        failures += encoded_equals(D22, P04, &m, literal, len);
    }
    for (unsigned v = 0; v <= UINT8_MAX; ++v) {
        uint8_t literal[] = {0x0c, 0, 0};
        size_t len = v < 128 ? 2 : 3;
        literal[1] = v < 128 ? (uint8_t)v : 0x80;
        literal[2] = (uint8_t)v;
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){literal, len}, &m, sizeof(m)), MOQ_OK);
        MOQ_TEST_CHECK(m.present == MOQ_LOC_METADATA_HAS_AUDIO_LEVEL && m.audio_level == v);
        failures += encoded_equals(D22, P04, &m, literal, len);
    }
    return failures;
}

static int refusal_vectors(void)
{
    int failures = 0;
    static const struct {
        const char *name;
        moq_result_t result;
        size_t len;
        uint8_t data[16];
    } vectors[] = {
        {"duplicate timestamp", MOQ_ERR_UNSUPPORTED, 4, {0x10, 1, 0, 2}},
        {"duplicate scale equal", MOQ_ERR_UNSUPPORTED, 4, {8, 0, 0, 0}},
        {"duplicate marking", MOQ_ERR_UNSUPPORTED, 6, {9, 1, 0xe0, 0, 1, 0xe0}},
        {"duplicate audio", MOQ_ERR_UNSUPPORTED, 4, {0x0c, 0, 0, 0}},
        {"duplicate video config", MOQ_ERR_UNSUPPORTED, 4, {0x0d, 0, 0, 0}},
        {"duplicate audio config", MOQ_ERR_UNSUPPORTED, 4, {0x0f, 0, 0, 0}},
        {"duplicate inner", MOQ_ERR_UNSUPPORTED, 6, {0x0b, 4, 0x10, 1, 0, 1}},
        {"duplicate inner outer", MOQ_ERR_UNSUPPORTED, 6, {0x0b, 2, 0x10, 1, 5, 2}},
        {"duplicate outer inner", MOQ_ERR_UNSUPPORTED, 8, {9, 1, 0xe0, 2, 3, 9, 1, 0xe0}},
        {"empty marking", MOQ_ERR_PROTO, 2, {9, 0}},
        {"five byte marking", MOQ_ERR_PROTO, 7, {9, 5, 0, 1, 2, 3, 4}},
        {"audio above byte", MOQ_ERR_PROTO, 3, {0x0c, 0x81, 0}},
        {"truncated type", MOQ_ERR_PROTO, 1, {0x80}},
        {"truncated timestamp", MOQ_ERR_PROTO, 2, {0x10, 0xff}},
        {"truncated config length", MOQ_ERR_PROTO, 2, {0x0d, 0x80}},
        {"truncated config body", MOQ_ERR_PROTO, 3, {0x0f, 2, 0}},
        {"oversized odd length", MOQ_ERR_PROTO, 4, {0x0d, 0xc1, 0, 0}},
        {"truncated inner", MOQ_ERR_PROTO, 3, {0x0b, 1, 0x10}},
        {"nested immutable", MOQ_ERR_PROTO, 4, {0x0b, 2, 0x0b, 0}},
        {"delta overflow", MOQ_ERR_PROTO, 12,
            {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1, 0}},
        {"late malformed leaves first field untouched", MOQ_ERR_PROTO, 3, {8, 1, 2}},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        moq_loc_metadata_t got, saved;
        memset(&got, 0xa5, sizeof(got));
        memcpy(&saved, &got, sizeof(saved));
        moq_result_t rc = moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){vectors[i].data, vectors[i].len}, &got, sizeof(got));
        if (rc != vectors[i].result)
            fprintf(stderr, "vector %s returned %d\n", vectors[i].name, (int)rc);
        MOQ_TEST_CHECK_EQ_INT(rc, vectors[i].result);
        MOQ_TEST_CHECK(memcmp(&got, &saved, sizeof(got)) == 0);
    }
    const uint8_t unknown[] = {0x20, 2, 0, 3, 1, 0};
    moq_loc_metadata_t got;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){unknown, sizeof(unknown)}, &got, sizeof(got)), MOQ_OK);
    MOQ_TEST_CHECK(got.present == 0);
    const uint8_t unknown_inner[] = {0x0b, 4, 0x20, 1, 1, 0, 5, 2};
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
        (moq_bytes_t){unknown_inner, sizeof(unknown_inner)}, &got, sizeof(got)), MOQ_OK);
    MOQ_TEST_CHECK(got.present == MOQ_LOC_METADATA_HAS_TIMESTAMP && got.timestamp == 2);
    return failures;
}

static int sizes_and_arguments(void)
{
    int failures = 0;
    struct { moq_loc_metadata_t m; uint8_t guard[16]; } f;
    moq_loc_metadata_init_sized(NULL, sizeof(f));
    for (size_t n = 0; n <= sizeof(f); ++n) {
        memset(&f, 0xa5, sizeof(f));
        moq_loc_metadata_init_sized(&f.m, n);
        size_t written = n < 4 ? 0 : (n < sizeof(f.m) ? n : sizeof(f.m));
        if (written) {
            MOQ_TEST_CHECK_EQ_SIZE(f.m.struct_size, written);
            for (size_t i = 4; i < written; ++i)
                MOQ_TEST_CHECK(((uint8_t *)&f)[i] == 0);
        }
        for (size_t i = written; i < sizeof(f); ++i)
            MOQ_TEST_CHECK(((uint8_t *)&f)[i] == 0xa5);
    }
    const moq_bytes_t empty = {NULL, 0};
    for (size_t n = 0; n <= sizeof(f); ++n) {
        memset(&f, 0xa5, sizeof(f));
        moq_result_t rc = moq_loc_parse_metadata(D22, P04, empty, &f.m, n);
        MOQ_TEST_CHECK_EQ_INT(rc, n < MOQ_LOC_METADATA_V0_SIZE ? MOQ_ERR_INVAL : MOQ_OK);
        if (rc == MOQ_OK) {
            MOQ_TEST_CHECK_EQ_SIZE(f.m.struct_size, sizeof(f.m));
            MOQ_TEST_CHECK(f.m.present == 0);
        }
        size_t written = rc == MOQ_OK ? sizeof(f.m) : 0;
        for (size_t i = written; i < sizeof(f); ++i)
            MOQ_TEST_CHECK(((uint8_t *)&f)[i] == 0xa5);
    }
    const moq_version_t versions[] = {(moq_version_t)0, (moq_version_t)19, D16, D18, D22};
    const moq_loc_profile_t profiles[] = {(moq_loc_profile_t)0, MOQ_LOC_PROFILE_02, P04, P04, P01};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
        memset(&f, 0xa5, sizeof(f));
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(versions[i], profiles[i],
            empty, &f.m, sizeof(f.m)), MOQ_ERR_INVAL);
        for (size_t j = 0; j < sizeof(f); ++j)
            MOQ_TEST_CHECK(((uint8_t *)&f)[j] == 0xa5);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04, empty, NULL,
        sizeof(f.m)), MOQ_ERR_INVAL);
    memset(&f, 0xa5, sizeof(f));
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04, (moq_bytes_t){NULL, 1},
        &f.m, sizeof(f.m)), MOQ_ERR_INVAL);
    for (size_t j = 0; j < sizeof(f); ++j)
        MOQ_TEST_CHECK(((uint8_t *)&f)[j] == 0xa5);
    return failures;
}

static int validation_and_allocation(void)
{
    int failures = 0;
    moq_rcbuf_t *sentinel = NULL;
    const uint8_t value = 42;
    MOQ_TEST_CHECK_EQ_INT(moq_rcbuf_create(moq_alloc_default(), &value, 1, &sentinel), MOQ_OK);
    if (!sentinel) return failures + 1;
    for (size_t i = 0; i < 11; ++i) {
        moq_loc_metadata_t m;
        moq_loc_metadata_init_sized(&m, sizeof(m));
        switch (i) {
        case 0: m.struct_size = MOQ_LOC_METADATA_V0_SIZE - 1; break;
        case 1: m.present = UINT32_C(0x80000000); break;
        case 2: m.reserved[0] = 1; break;
        case 3: m.reserved[1] = 1; break;
        case 4: m.present = MOQ_LOC_METADATA_HAS_MARKING; m.marking_len = 0; break;
        case 5: m.present = MOQ_LOC_METADATA_HAS_MARKING; m.marking_len = 5; break;
        case 6: m.present = MOQ_LOC_METADATA_HAS_VIDEO_CONFIG; m.video_config.len = 1; break;
        case 7: m.present = MOQ_LOC_METADATA_HAS_AUDIO_CONFIG; m.audio_config.len = 1; break;
        case 8: m.present = MOQ_LOC_METADATA_HAS_VIDEO_CONFIG;
                m.video_config = (moq_bytes_t){&value, 65536}; break;
        case 9: m.present = MOQ_LOC_METADATA_HAS_AUDIO_CONFIG;
                m.audio_config = (moq_bytes_t){&value, SIZE_MAX}; break;
        default: break;
        }
        oom_alloc_state_t state = {0};
        moq_alloc_t alloc = oom_allocator(&state);
        moq_rcbuf_t *out = sentinel;
        MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P04, &m, &out),
                             i == 10 ? MOQ_OK : MOQ_ERR_INVAL);
        MOQ_TEST_CHECK(out == (i == 10 ? NULL : sentinel));
        MOQ_TEST_CHECK(state.alloc_count == 0 && state.balance == 0);
    }
    moq_loc_metadata_t m;
    moq_loc_metadata_init_sized(&m, sizeof(m));
    oom_alloc_state_t state = {0};
    moq_alloc_t alloc = oom_allocator(&state), invalid = alloc;
    moq_rcbuf_t *out = sentinel;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(NULL, D22, P04, &m, &out), MOQ_ERR_INVAL);
    invalid.alloc = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&invalid, D22, P04, &m, &out), MOQ_ERR_INVAL);
    invalid = alloc; invalid.free = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&invalid, D22, P04, &m, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P04, NULL, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P04, &m, NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, (moq_version_t)0, P04, &m, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P01, &m, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(out == sentinel && state.alloc_count == 0);
    /* Exact four-byte heap input: the stamp is the only covered field. */
    uint32_t *short_input = malloc(sizeof(*short_input));
    MOQ_TEST_CHECK(short_input != NULL);
    if (short_input) {
        *short_input = (uint32_t)(MOQ_LOC_METADATA_V0_SIZE - 1);
        MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P04,
            (const moq_loc_metadata_t *)(const void *)short_input, &out), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK(out == sentinel && state.alloc_count == 0);
        free(short_input);
    }
    /* A future stamp does not cause reads beyond the frozen input allocation. */
    moq_loc_metadata_t *future = malloc(sizeof(*future));
    MOQ_TEST_CHECK(future != NULL);
    if (future) {
        moq_loc_metadata_init_sized(future, sizeof(*future));
        future->struct_size = (uint32_t)sizeof(*future) + 16;
        MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D22, P04, future, &out), MOQ_OK);
        MOQ_TEST_CHECK(out == NULL && state.alloc_count == 0);
        free(future);
    }
    uint8_t *config = malloc(65535);
    MOQ_TEST_CHECK(config != NULL);
    if (config) {
        memset(config, 0x6a, 65535);
        for (size_t large = 0; large < 2; ++large) {
            moq_loc_metadata_init_sized(&m, sizeof(m));
            m.present = large ? MOQ_LOC_METADATA_HAS_AUDIO_CONFIG : MOQ_LOC_METADATA_HAS_TIMESTAMP;
            m.timestamp = 1;
            m.audio_config = (moq_bytes_t){config, 65535};
            for (uint64_t fail = 0; fail <= (large ? 2u : 1u); ++fail) {
                state = (oom_alloc_state_t){.fail_at = fail};
                alloc = oom_allocator(&state);
                out = sentinel;
                moq_result_t rc = moq_loc_encode_metadata(&alloc, D22, P04, &m, &out);
                MOQ_TEST_CHECK_EQ_INT(rc, fail ? MOQ_ERR_NOMEM : MOQ_OK);
                if (fail) {
                    MOQ_TEST_CHECK(out == sentinel && state.balance == 0);
                    MOQ_TEST_CHECK_EQ_U64(state.alloc_count, fail);
                } else {
                    MOQ_TEST_CHECK(out != NULL && out != sentinel && state.balance == 1);
                    MOQ_TEST_CHECK_EQ_U64(state.alloc_count, large ? 2 : 1);
                    if (out && out != sentinel) {
                        if (large) {
                            const uint8_t prefix[] = {0x0f, 0xc0, 0xff, 0xff};
                            MOQ_TEST_CHECK_EQ_SIZE(moq_rcbuf_len(out), 65539);
                            MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(out), prefix, sizeof(prefix)) == 0);
                            MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(out) + 4, config, 65535) == 0);
                            config[0] = 0;
                            MOQ_TEST_CHECK(moq_rcbuf_data(out)[4] == 0x6a);
                            config[0] = 0x6a;
                        }
                        moq_rcbuf_decref(out);
                    }
                    MOQ_TEST_CHECK_EQ_U64(state.balance, 0);
                }
                MOQ_TEST_CHECK(config[0] == 0x6a && config[65534] == 0x6a);
            }
        }
        free(config);
    }
    /* Independently constructed maximum-length input borrows its original span. */
    uint8_t *literal = malloc(65539);
    MOQ_TEST_CHECK(literal != NULL);
    if (literal) {
        const uint8_t prefix[] = {0x0d, 0xc0, 0xff, 0xff};
        memcpy(literal, prefix, sizeof(prefix));
        memset(literal + sizeof(prefix), 0x6a, 65535);
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D22, P04,
            (moq_bytes_t){literal, 65539}, &m, sizeof(m)), MOQ_OK);
        MOQ_TEST_CHECK(m.present == MOQ_LOC_METADATA_HAS_VIDEO_CONFIG &&
                       m.video_config.len == 65535 && m.video_config.data == literal + 4);
        MOQ_TEST_CHECK(memcmp(literal, prefix, sizeof(prefix)) == 0);
        free(literal);
    }
    MOQ_TEST_CHECK(moq_rcbuf_len(sentinel) == 1 && moq_rcbuf_data(sentinel)[0] == value);
    moq_rcbuf_decref(sentinel);
    return failures;
}

static int legacy_controls(void)
{
    int failures = 0;
    const uint8_t d16max[] = {2, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t d18max[] = {2, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t duplicate[] = {2, 1, 0, 2};
    for (size_t i = 0; i < 2; ++i) {
        moq_version_t version = i ? D18 : D16;
        const uint8_t *literal = i ? d18max : d16max;
        size_t len = i ? sizeof(d18max) : sizeof(d16max);
        uint64_t value = i ? UINT64_MAX : (UINT64_MAX >> 2);
        moq_loc_headers_t *h = malloc(sizeof(*h));
        MOQ_TEST_CHECK(h != NULL);
        if (h) {
            moq_loc_headers_init(h);
            MOQ_TEST_CHECK_EQ_SIZE(h->struct_size, sizeof(*h));
            MOQ_TEST_CHECK_EQ_INT(moq_loc_parse(version, P01,
                (moq_bytes_t){literal, len}, h), MOQ_OK);
            MOQ_TEST_CHECK(h->has_timestamp && h->timestamp == value);
            moq_rcbuf_t *out = NULL;
            MOQ_TEST_CHECK_EQ_INT(moq_loc_encode(moq_alloc_default(), version, P01, h, &out), MOQ_OK);
            MOQ_TEST_CHECK(out && moq_rcbuf_len(out) == len);
            if (out && moq_rcbuf_len(out) == len)
                MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(out), literal, len) == 0);
            moq_rcbuf_decref(out);
            MOQ_TEST_CHECK_EQ_INT(moq_loc_parse(version, P01,
                (moq_bytes_t){duplicate, sizeof(duplicate)}, h), MOQ_OK);
            MOQ_TEST_CHECK(h->has_timestamp && h->timestamp == 2);
            free(h);
        }
        moq_loc_metadata_t m, saved;
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(version, P01,
            (moq_bytes_t){literal, len}, &m, sizeof(m)), MOQ_OK);
        MOQ_TEST_CHECK(m.present == MOQ_LOC_METADATA_HAS_TIMESTAMP && m.timestamp == value);
        failures += encoded_equals(version, P01, &m, literal, len);
        memset(&m, 0xa5, sizeof(m));
        memcpy(&saved, &m, sizeof(saved));
        MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(version, P01,
            (moq_bytes_t){duplicate, sizeof(duplicate)}, &m, sizeof(m)), MOQ_ERR_UNSUPPORTED);
        MOQ_TEST_CHECK(memcmp(&m, &saved, sizeof(m)) == 0);
    }
    moq_loc_metadata_t m;
    moq_loc_metadata_init_sized(&m, sizeof(m));
    m.present = MOQ_LOC_METADATA_HAS_TIMESTAMP;
    m.timestamp = (UINT64_MAX >> 2) + 1;
    oom_alloc_state_t state = {0};
    moq_alloc_t alloc = oom_allocator(&state);
    moq_rcbuf_t *out = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D16, P01, &m, &out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(state.alloc_count == 0);
    m.present = MOQ_LOC_METADATA_HAS_TIMESCALE;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D18, P01, &m, &out), MOQ_ERR_UNSUPPORTED);
    m.present = MOQ_LOC_METADATA_HAS_AUDIO_CONFIG;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D18, P01, &m, &out), MOQ_ERR_UNSUPPORTED);
    m.present = MOQ_LOC_METADATA_HAS_MARKING;
    m.marking_len = 2;
    m.marking[0] = 0;
    m.marking[1] = 1;
    MOQ_TEST_CHECK_EQ_INT(moq_loc_encode_metadata(&alloc, D18, P01, &m, &out), MOQ_ERR_UNSUPPORTED);
    const uint8_t marking[] = {4, 0x80, 0xe0};
    MOQ_TEST_CHECK_EQ_INT(moq_loc_parse_metadata(D18, P01,
        (moq_bytes_t){marking, sizeof(marking)}, &m, sizeof(m)), MOQ_OK);
    MOQ_TEST_CHECK(m.marking_len == 1 && m.marking[0] == 0xe0);
    failures += encoded_equals(D18, P01, &m, marking, sizeof(marking));
    MOQ_TEST_CHECK(state.alloc_count == 0);
    return failures;
}

int main(void)
{
    static const struct { const char *name; int (*run)(void); } cases[] = {
        {"literal_and_immutable", literal_and_immutable},
        {"raw_values_and_presence", raw_values_and_presence},
        {"refusal_vectors", refusal_vectors},
        {"sizes_and_arguments", sizes_and_arguments},
        {"validation_and_allocation", validation_and_allocation},
        {"legacy_controls", legacy_controls},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int n = cases[i].run();
        printf("CASE %s: %s (%d failed checks)\n", cases[i].name, n ? "FAIL" : "PASS", n);
        failures += n;
    }
    printf("%s: %zu cases, %d failures\n", failures ? "FAIL" : "PASS",
           sizeof(cases) / sizeof(cases[0]), failures);
    return failures != 0;
}
#endif
