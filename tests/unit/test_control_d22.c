/* Independent draft-22 literal vectors, not just encoder/decoder round trips.
 * The codec is private and does not register a usable draft-22 session yet. */
#include "../../core/src/wire/control_d22_internal.h"
#include "moq/control_d18.h"
#include "test_support.h"
#include <string.h>

static int test_filter_vectors(void)
{
    int failures = 0;
    static const struct {
        moq_d22_location_filter_t value;
        uint8_t wire[8];
        size_t len;
    } cases[] = {
        {{.type = MOQ_D22_FILTER_NONE}, {0}, 1},
        {{.type = MOQ_D22_FILTER_RELATIVE, .start_group = 1}, {1,1}, 2},
        {{.type = MOQ_D22_FILTER_ABSOLUTE, .start_group = 3,
          .start_object = 4}, {2,3,4}, 3},
        {{.type = MOQ_D22_FILTER_GROUP_END, .start_group = 3,
          .start_object = 4, .end_group_delta = 2}, {3,3,4,2}, 4},
        {{.type = MOQ_D22_FILTER_RANGE, .start_group = 3,
          .start_object = 4, .end_group_delta = 2, .end_object = 9},
          {4,3,4,2,9}, 5},
        {{.type = MOQ_D22_FILTER_NEXT_OBJECT}, {5}, 1},
        {{.type = MOQ_D22_FILTER_RELATIVE, .start_group = 128},
          {1,0x80,0x80}, 3}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint8_t bytes[64];
        memset(bytes, 0xa5, sizeof(bytes));
        moq_buf_writer_t w = {bytes, sizeof(bytes), 1};
        moq_result_t rc = moq_d22_encode_location_filter(&w, &cases[i].value);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) continue;
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, cases[i].len + 1);
        MOQ_TEST_CHECK(memcmp(bytes + 1, cases[i].wire, cases[i].len) == 0);
        MOQ_TEST_CHECK(bytes[0] == 0xa5 && bytes[w.pos] == 0xa5);
        moq_buf_reader_t r = {cases[i].wire, cases[i].len, 0};
        moq_d22_location_filter_t out;
        memset(&out, 0xa5, sizeof(out));
        rc = moq_d22_decode_location_filter(&r, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) continue;
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, cases[i].len);
        MOQ_TEST_CHECK_EQ_INT(out.type, cases[i].value.type);
        MOQ_TEST_CHECK_EQ_U64(out.start_group, cases[i].value.start_group);
        MOQ_TEST_CHECK_EQ_U64(out.start_object, cases[i].value.start_object);
        MOQ_TEST_CHECK_EQ_U64(out.end_group_delta, cases[i].value.end_group_delta);
        MOQ_TEST_CHECK_EQ_U64(out.end_object, cases[i].value.end_object);
        for (size_t n = 0; n < cases[i].len; ++n) {
            r = (moq_buf_reader_t){cases[i].wire, n, 0};
            moq_d22_location_filter_t before;
            memset(&out, 0xa5, sizeof(out));
            memcpy(&before, &out, sizeof(before));
            MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_location_filter(&r, &out),
                                  MOQ_ERR_BUFFER);
            MOQ_TEST_CHECK_EQ_SIZE(r.pos, 0);
            MOQ_TEST_CHECK(memcmp(&before, &out, sizeof(out)) == 0);
            w = (moq_buf_writer_t){bytes, n + 1, 1};
            MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w,
                                  &cases[i].value), MOQ_ERR_BUFFER);
            MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
            MOQ_TEST_CHECK(bytes[0] == 0xa5);
        }
    }
    MOQ_TEST_PASS("d22.filter.literal_forms_and_prefixes");
    return failures;
}

static int test_filter_bounds(void)
{
    int failures = 0;
    /* Full-range end includes UINT64_MAX, without an exclusive-end conversion. */
    const uint8_t wire[] = {4, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0, 0, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    moq_buf_reader_t r = {wire, sizeof(wire), 0};
    moq_d22_location_filter_t f = {0};
    moq_result_t rc = moq_d22_decode_location_filter(&r, &f);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(f.start_group, UINT64_MAX);
        MOQ_TEST_CHECK_EQ_U64(f.end_object, UINT64_MAX);
        uint8_t encoded[sizeof(wire)];
        moq_buf_writer_t w = {encoded, sizeof(encoded), 0};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w, &f), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, sizeof(wire));
        MOQ_TEST_CHECK(memcmp(encoded, wire, sizeof(wire)) == 0);
    }
    uint8_t overflow[sizeof(wire)];
    memcpy(overflow, wire, sizeof(wire));
    overflow[11] = 1; /* UINT64_MAX + EndGroupDelta */
    r = (moq_buf_reader_t){overflow, sizeof(overflow), 0};
    moq_d22_location_filter_t before;
    memset(&f, 0xa5, sizeof(f));
    memcpy(&before, &f, sizeof(f));
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_location_filter(&r, &f), MOQ_ERR_PROTO);
    MOQ_TEST_CHECK_EQ_SIZE(r.pos, 0);
    MOQ_TEST_CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    f = (moq_d22_location_filter_t){.type = MOQ_D22_FILTER_RANGE,
        .start_group = UINT64_MAX, .end_group_delta = 1};
    moq_buf_writer_t w = {overflow, sizeof(overflow), 1};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w, &f), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    const moq_d22_filter_type_t invalid_types[] = {
        (moq_d22_filter_type_t)-1, (moq_d22_filter_type_t)6
    };
    for (size_t i = 0; i < sizeof(invalid_types)/sizeof(invalid_types[0]); ++i) {
        f = (moq_d22_location_filter_t){.type = invalid_types[i]};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_location_filter(&w, &f), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    }
    for (unsigned int type = 6; type < 128; ++type) {
        uint8_t bad[] = {(uint8_t)type};
        r = (moq_buf_reader_t){bad, sizeof(bad), 0};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_location_filter(&r, &f), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 0);
    }
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(0, 0), 1);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(12, 0), 13);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(12, 1), 12);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(12, 2), 11);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(12, 14), 0);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(0, UINT64_MAX), 0);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(UINT64_MAX, 0), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(UINT64_MAX, 1), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(UINT64_MAX, 2), UINT64_MAX-1);
    MOQ_TEST_CHECK_EQ_U64(moq_d22_relative_start_group(UINT64_MAX, UINT64_MAX), 1);
    MOQ_TEST_PASS("d22.filter.full_width_and_clamped_arithmetic");
    return failures;
}

static int test_envelope(void)
{
    int failures = 0;
    const uint8_t wire[] = {0xaf,0, 0,4, 6,2, 2,1};
    for (size_t n = 0; n <= sizeof(wire); ++n) {
        moq_buf_reader_t r = {wire, n, 0};
        moq_control_envelope_t out, before;
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        moq_result_t rc = moq_d22_decode_envelope(&r, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, n == sizeof(wire) ? MOQ_OK : MOQ_ERR_BUFFER);
        if (n == sizeof(wire) && rc == MOQ_OK) {
            MOQ_TEST_CHECK_EQ_U64(out.msg_type, MOQ_D22_SETUP);
            MOQ_TEST_CHECK_EQ_SIZE(out.payload_len, 4);
            MOQ_TEST_CHECK(out.payload == wire + 4);
            MOQ_TEST_CHECK_EQ_SIZE(r.pos, sizeof(wire));
        } else {
            MOQ_TEST_CHECK_EQ_SIZE(r.pos, 0);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        }
    }
    MOQ_TEST_PASS("d22.envelope.transactional_prefixes");
    return failures;
}

static int test_setup(void)
{
    int failures = 0;
    const uint8_t wire[] = {0xaf,0, 0,4, 6,2, 2,1};
    const moq_d22_kvp_t options[] = {{.type = 6, .integer = 2},
                                   {.type = 8, .integer = 1}};
    uint8_t encoded[32];
    memset(encoded, 0xa5, sizeof(encoded));
    moq_buf_writer_t w = {encoded, sizeof(encoded), 1};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_setup(&w, options, 2), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, sizeof(wire) + 1);
    MOQ_TEST_CHECK(memcmp(encoded + 1, wire, sizeof(wire)) == 0);
    MOQ_TEST_CHECK(encoded[0] == 0xa5);
    moq_d22_setup_options_t opts;
    memset(&opts, 0, sizeof(opts));
    moq_result_t rc = moq_d22_decode_setup_options(wire + 4, 4, &opts);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK(opts.has_max_filter_ranges && opts.has_max_request_updates);
        MOQ_TEST_CHECK_EQ_U64(opts.max_filter_ranges, 2);
        MOQ_TEST_CHECK_EQ_U64(opts.max_request_updates, 1);
        MOQ_TEST_CHECK(opts.encoded.data == wire + 4 && opts.encoded.len == 4);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(NULL, 0, &opts), MOQ_OK);
    MOQ_TEST_CHECK(!opts.has_max_filter_ranges && !opts.has_max_request_updates);
    MOQ_TEST_CHECK_EQ_U64(opts.max_request_updates, 0);
    const uint8_t explicit_zero[] = {8,0};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(explicit_zero, 2, &opts), MOQ_OK);
    MOQ_TEST_CHECK(opts.has_max_request_updates && opts.max_request_updates == 0);
    for (size_t n = 0; n < sizeof(wire); ++n) {
        w = (moq_buf_writer_t){encoded, n + 1, 1};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_setup(&w, options, 2), MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    }
    MOQ_TEST_PASS("d22.setup.negotiated_limits_and_capacity");
    return failures;
}

static int test_setup_kvp_policy(void)
{
    int failures = 0;
    const uint8_t all[] = {1,1,'/', 2,2,3,0, 1,0, 1,1,'h',
                           1,8, 1,1,'m', 1,1};
    moq_d22_setup_options_t opts = {0};
    moq_result_t rc = moq_d22_decode_setup_options(all, sizeof(all), &opts);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK(opts.has_path && opts.has_authority && opts.has_implementation);
        MOQ_TEST_CHECK(opts.path.len == 1 && opts.path.data[0] == '/');
        MOQ_TEST_CHECK(opts.authority.len == 1 && opts.authority.data[0] == 'h');
        MOQ_TEST_CHECK(opts.implementation.len == 1 && opts.implementation.data[0] == 'm');
        MOQ_TEST_CHECK_EQ_SIZE(opts.auth_token_count, 1);
        MOQ_TEST_CHECK(opts.has_max_auth_token_cache_size && opts.max_auth_token_cache_size == 0);
    }
    const uint8_t unknown[] = {9,1,'x', 0,0, 1,0xff,
                              0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(unknown, sizeof(unknown), &opts), MOQ_OK);
    const uint8_t tokens[] = {3,2,3,0, 0,3,1,7,0};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(tokens, sizeof(tokens), &opts), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(opts.auth_token_count, 2);
    const uint8_t duplicate[] = {6,1,0,2};
    moq_d22_setup_options_t before;
    memset(&opts, 0xa5, sizeof(opts));
    memcpy(&before, &opts, sizeof(opts));
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(duplicate, sizeof(duplicate), &opts), MOQ_ERR_PROTO);
    MOQ_TEST_CHECK(memcmp(&opts, &before, sizeof(opts)) == 0);
    const uint8_t bad_token[] = {3,1,1}; /* REGISTER without alias/type */
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(bad_token, sizeof(bad_token), &opts), MOQ_D22_ERR_KVP_FORMAT);
    const uint8_t truncated[] = {7,2,'x'};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(truncated, sizeof(truncated), &opts), MOQ_ERR_PROTO);
    const uint8_t too_long[] = {9,0xc1,0,0}; /* 65536, over odd KVP bound */
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(too_long, sizeof(too_long), &opts), MOQ_ERR_PROTO);
    const uint8_t overflow[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0, 1,0};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_setup_options(overflow, sizeof(overflow), &opts), MOQ_ERR_PROTO);
    MOQ_TEST_PASS("d22.setup.known_unknown_duplicates_tokens_and_bounds");
    return failures;
}

static int test_setup_token_shapes(void)
{
    int failures = 0;
    static const struct {
        uint8_t body[7];
        size_t len;
        bool valid;
    } rows[] = {
        {{3,2,0,9}, 4, true},       /* DELETE */
        {{3,3,1,9,0}, 5, true},     /* REGISTER, empty value */
        {{3,2,2,9}, 4, true},       /* USE_ALIAS */
        {{3,2,3,0}, 4, true},       /* USE_VALUE, empty value */
        {{3,4,1,9,0,'x'}, 6, true},
        {{3,3,3,0,'x'}, 5, true},
        {{3,3,0,9,0}, 5, false},    /* DELETE cannot carry a value */
        {{3,3,2,9,0}, 5, false},    /* USE_ALIAS cannot carry a value */
        {{3,1,0}, 3, false},
        {{3,2,1,9}, 4, false},
        {{3,1,2}, 3, false},
        {{3,1,3}, 3, false},
        {{3,1,4}, 3, false},
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i) {
        moq_d22_setup_options_t out, before;
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        moq_result_t rc = moq_d22_decode_setup_options(rows[i].body,
                                                       rows[i].len, &out);
        MOQ_TEST_CHECK_EQ_INT(rc, rows[i].valid ? MOQ_OK : MOQ_D22_ERR_KVP_FORMAT);
        if (rows[i].valid && rc == MOQ_OK) {
            MOQ_TEST_CHECK_EQ_SIZE(out.auth_token_count, 1);
            MOQ_TEST_CHECK(out.encoded.data == rows[i].body);
        } else if (!rows[i].valid) {
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        }
        moq_d22_kvp_t option = {.type = 3,
            .bytes = {rows[i].body + 2, rows[i].len - 2}};
        uint8_t encoded[32] = {0xa5};
        moq_buf_writer_t w = {encoded, sizeof(encoded), 1};
        rc = moq_d22_encode_setup(&w, &option, 1);
        MOQ_TEST_CHECK_EQ_INT(rc, rows[i].valid ? MOQ_OK : MOQ_ERR_INVAL);
        if (rows[i].valid && rc == MOQ_OK) {
            MOQ_TEST_CHECK_EQ_SIZE(w.pos, 5 + rows[i].len);
            MOQ_TEST_CHECK(memcmp(encoded + 5, rows[i].body, rows[i].len) == 0);
        } else if (!rows[i].valid) {
            MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
        }
        MOQ_TEST_CHECK(encoded[0] == 0xa5);
    }
    MOQ_TEST_PASS("d22.setup.token_exact_bodies_and_transaction");
    return failures;
}

static int test_kvp_transaction(void)
{
    int failures = 0;
    const uint8_t wire[] = {1,3,'a','b','c'};
    for (size_t n = 0; n < sizeof(wire); ++n) {
        moq_buf_reader_t r = {wire, n, 0};
        uint64_t previous = 6;
        moq_d22_kvp_t out, before;
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_kvp(&r, &previous, &out), MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 0);
        MOQ_TEST_CHECK_EQ_U64(previous, 6);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    moq_buf_reader_t r = {wire, sizeof(wire), 0};
    uint64_t previous = 6;
    moq_d22_kvp_t out = {0};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_kvp(&r, &previous, &out), MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(previous, 7);
    MOQ_TEST_CHECK_EQ_U64(out.type, 7);
    MOQ_TEST_CHECK(out.bytes.len == 3 && out.bytes.data == wire + 2);
    MOQ_TEST_PASS("d22.kvp.cursor_output_and_type_transaction");
    return failures;
}

static int test_goaway(void)
{
    int failures = 0;
    const uint8_t wire[] = {0x10,0,3, 0,0x80,0x80}; /* empty URI, 128ms */
    uint8_t encoded[32];
    memset(encoded, 0xa5, sizeof(encoded));
    moq_buf_writer_t w = {encoded, sizeof(encoded), 1};
    moq_d22_goaway_t goaway = {.timeout_ms = 128};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_goaway(&w, &goaway), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, sizeof(wire) + 1);
    MOQ_TEST_CHECK(memcmp(encoded + 1, wire, sizeof(wire)) == 0);
    MOQ_TEST_CHECK(encoded[0] == 0xa5);
    for (size_t n = 0; n < sizeof(wire); ++n) {
        w = (moq_buf_writer_t){encoded, n + 1, 1};
        MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_goaway(&w, &goaway), MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    }
    moq_d22_goaway_t out, before;
    for (size_t n = 0; n < 3; ++n) {
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_goaway(wire + 3, n, &out), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_goaway(wire + 3, 3, &out), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(out.uri.len, 0);
    MOQ_TEST_CHECK_EQ_U64(out.timeout_ms, 128);
    const uint8_t old_body[] = {0,0,2};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_goaway(old_body, sizeof(old_body), &out), MOQ_ERR_PROTO);
    MOQ_TEST_PASS("d22.goaway.literal_transaction_and_no_request_id");
    return failures;
}

static int test_encode_rejection(void)
{
    int failures = 0;
    static uint8_t bytes[8208], uri[8193];
    memset(bytes, 0xa5, sizeof(bytes));
    moq_buf_writer_t w = {bytes, sizeof(bytes), 1};
    moq_d22_goaway_t goaway = {{uri,8192}, UINT64_MAX};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_goaway(&w, &goaway), MOQ_OK);
    moq_d22_goaway_t out = {0};
    if (w.pos > 4) {
        MOQ_TEST_CHECK_EQ_INT(moq_d22_decode_goaway(bytes + 4, w.pos - 4, &out), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(out.uri.len, 8192);
        MOQ_TEST_CHECK_EQ_U64(out.timeout_ms, UINT64_MAX);
    }
    goaway.uri.len++;
    w.pos = 1;
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_goaway(&w, &goaway), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    const moq_d22_kvp_t unsorted[] = {{.type = 8}, {.type = 6}};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_setup(&w, unsorted, 2), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    const moq_d22_kvp_t duplicate[] = {{.type = 8}, {.type = 8}};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_setup(&w, duplicate, 2), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    const moq_d22_kvp_t bad_pointer = {.type = 1, .bytes = {NULL,1}};
    MOQ_TEST_CHECK_EQ_INT(moq_d22_encode_setup(&w, &bad_pointer, 1), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_SIZE(w.pos, 1);
    MOQ_TEST_CHECK(bytes[0] == 0xa5);
    MOQ_TEST_PASS("d22.encode.invalid_inputs_and_uri_boundary");
    return failures;
}

static int test_d18_discriminators(void)
{
    int failures = 0;
    /* -18 requires a third GOAWAY field and length-prefixes its filter. */
    const uint8_t body[] = {0,0};
    moq_d18_goaway_t goaway = {0};
    MOQ_TEST_CHECK(moq_d18_decode_goaway(body, sizeof(body), &goaway) != MOQ_OK);
    const uint8_t params[] = {0x21,5};
    moq_buf_reader_t r = {params, sizeof(params), 0};
    moq_d18_msg_params_t out;
    MOQ_TEST_CHECK(moq_d18_decode_msg_params(&r, 1,
        MOQ_D18_PARAM_BIT_SUBSCRIPTION_FILTER, &out) != MOQ_OK);
    MOQ_TEST_PASS("d22.d18_wire_is_not_a_substitute");
    return failures;
}

int main(void)
{
    int failures = test_filter_vectors() + test_filter_bounds() + test_envelope()
        + test_setup() + test_setup_kvp_policy() + test_setup_token_shapes()
        + test_kvp_transaction()
        + test_goaway() + test_encode_rejection() + test_d18_discriminators();
    printf("draft-22 private wire: %d failures\n", failures);
    return failures ? 1 : 0;
}
