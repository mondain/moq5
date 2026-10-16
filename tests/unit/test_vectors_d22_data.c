/*
 * Draft-22 data-plane vectors authored from the specification text alone
 * (tests/vectors/d22): the subgroup header and its object fields, the object
 * datagram, the fetch header and object, and the End of Timed-Out Range
 * indicator. Every vector decodes through the draft-22 profile's ops to the
 * fields its derivation names and re-encodes byte for byte; every strict
 * prefix of a stream-framed item reports incomplete; the alternate reading of
 * the range indicator (a trailing Object Payload Length) is refused: the
 * indicator ends after its Object ID and the extra byte is not part of it.
 *
 * The FIRST_OBJECT vector carries the bit §2.2 requires of an Original
 * Publisher opening a new subgroup; the session reports it on the stream's
 * first object only.
 *
 * The second half pins the Type Flags values the draft lists as invalid, as
 * PROTOCOL_VIOLATION on receipt by a draft-22 session: for a subgroup header
 * SUBGROUP_ID_MODE 0b11, bit 4 clear, and values of 128 or more; for a
 * datagram bit 4 set, a bit with no defined meaning, PROPERTIES with a zero
 * length, STATUS together with END_OF_GROUP, and STATUS with PROPERTIES on a
 * non-Normal object.
 *
 * The last part pins the Properties Length (§11.1.2) as a full-width vi64 in
 * the FETCH and subgroup object readers shared by draft-18 and draft-22, and
 * the draft-16 Extension Headers Length (draft-16 §10.2.1.2) in that profile's
 * readers: a declared length of 2^32 with fewer bytes buffered stays
 * incomplete on every host width, so no object is delivered and a FIN inside
 * the block closes the session with PROTOCOL_VIOLATION (§11.3; draft-16
 * §10.4). Zero and small lengths still decode and deliver. The draft-16
 * subgroup reader also validates the whole Object Status value (draft-16
 * §10.2.1.1): 0x100 and 0x103 are unregistered, not Normal and End of Group,
 * and close the session with PROTOCOL_VIOLATION.
 *
 * Usage: test_vectors_d22_data <vector_dir>
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static int failures;

#define TRACK_ALIAS 7u

static int read_file(const char *dir, const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static bool bytes_equal(const char *what, const uint8_t *got, size_t got_len,
                        const uint8_t *want, size_t want_len)
{
    if (got_len == want_len && memcmp(got, want, want_len) == 0) return true;
    fprintf(stderr, "FAIL %s: re-encode mismatch (%zu bytes, vector %zu)\n  got:", what, got_len, want_len);
    for (size_t i = 0; i < got_len; i++) fprintf(stderr, " %02x", got[i]);
    fprintf(stderr, "\n want:");
    for (size_t i = 0; i < want_len; i++) fprintf(stderr, " %02x", want[i]);
    fprintf(stderr, "\n");
    return false;
}

/* -- Fixture: a draft-22 pair with one subscription at Track Alias 7 ---- */

typedef struct fixture {
    moq_simpair_t      *sp;
    moq_session_t      *client;   /* subscriber: receives the data plane */
    moq_session_t      *server;   /* publisher */
    moq_subscription_t  server_sub;
} fixture_t;

static bool fixture_open_version(fixture_t *fx, moq_version_t version)
{
    memset(fx, 0, sizeof(*fx));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    if (version == MOQ_VERSION_DRAFT_16) {          /* MAX_REQUEST_ID grants */
        scfg.server_send_request_capacity = true;
        scfg.server_initial_request_capacity = 32;
    }
    if (moq_simpair_create(&scfg, &fx->sp) < 0) return false;
    fx->client = moq_simpair_client(fx->sp);
    fx->server = moq_simpair_server(fx->sp);
    if (moq_simpair_start(fx->sp) < 0) return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);

    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t csub;
    if (moq_session_subscribe(fx->client, &cfg, moq_simpair_now_us(fx->sp), &csub) < 0)
        return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);

    moq_event_t ev; bool got = false;
    while (moq_session_poll_events(fx->server, &ev, 1) > 0) {
        if (!got && ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            fx->server_sub = ev.u.subscribe_request.sub; got = true;
        }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    acc.has_track_alias = true;
    acc.track_alias = TRACK_ALIAS;
    if (moq_session_accept_subscribe(fx->server, fx->server_sub, &acc,
                                     moq_simpair_now_us(fx->sp)) < 0)
        return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    while (moq_session_poll_events(fx->client, &ev, 1) > 0) moq_event_cleanup(&ev);
    return moq_session_state(fx->client) == MOQ_SESS_ESTABLISHED;
}

static bool fixture_open(fixture_t *fx)
{
    return fixture_open_version(fx, MOQ_VERSION_DRAFT_22);
}

static void fixture_close(fixture_t *fx)
{
    if (fx->sp) moq_simpair_destroy(fx->sp);
    memset(fx, 0, sizeof(*fx));
}

/* The session's queued CLOSE_SESSION code, or UINT64_MAX when none. */
static uint64_t close_code(moq_session_t *s)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION && code == UINT64_MAX)
            code = a.u.close_session.code;
        moq_action_cleanup(&a);
    }
    return code;
}

/* -- Vectors ------------------------------------------------------------ */

static void check_prefixes_incomplete(const char *what, const uint8_t *v, size_t len,
                                      size_t header_len,
                                      moq_result_t (*decode)(moq_session_t *, moq_buf_reader_t *, void *),
                                      moq_session_t *s, void *out)
{
    for (size_t n = 0; n < header_len && n < len; n++) {
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, n);
        moq_result_t rc = decode(s, &r, out);
        if (rc != MOQ_ERR_BUFFER) {
            fprintf(stderr, "FAIL %s: prefix of %zu bytes rc=%d, expected incomplete\n", what, n, (int)rc);
            failures++;
        }
    }
}

static moq_result_t dec_subgroup_header(moq_session_t *s, moq_buf_reader_t *r, void *out)
{ return s->profile->decode_subgroup_header(s, r, (moq_decoded_subgroup_header_t *)out); }
static moq_result_t dec_fetch_header(moq_session_t *s, moq_buf_reader_t *r, void *out)
{ return s->profile->decode_fetch_header(s, r, (moq_decoded_fetch_stream_header_t *)out); }
static moq_result_t dec_fetch_object_first(moq_session_t *s, moq_buf_reader_t *r, void *out)
{ return s->profile->decode_fetch_object(s, r, MOQ_GROUP_ORDER_ASCENDING, NULL, (moq_decoded_fetch_object_t *)out); }
static moq_result_t dec_object_first(moq_session_t *s, moq_buf_reader_t *r, void *out)
{ return s->profile->decode_object_header(s, r, false, 0, false, (moq_decoded_object_header_t *)out); }
static moq_result_t dec_object_after_5(moq_session_t *s, moq_buf_reader_t *r, void *out)
{ return s->profile->decode_object_header(s, r, false, 5, true, (moq_decoded_object_header_t *)out); }

static void test_vectors(const char *dir)
{
    fixture_t fx;
    if (!fixture_open(&fx)) { fprintf(stderr, "FAIL: fixture\n"); failures++; fixture_close(&fx); return; }
    moq_session_t *s = fx.client;
    const moq_profile_ops_t *p = s->profile;
    MOQ_TEST_CHECK_EQ_INT((int)p->version, (int)MOQ_VERSION_DRAFT_22);
    uint8_t v[64], enc[64]; size_t vlen;
    moq_buf_writer_t w;

    /* subgroup_header_flags_14: 14 07 02 01 03 */
    if (read_file(dir, "subgroup_header_flags_14.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_subgroup_header_t h; memset(&h, 0, sizeof(h));
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_subgroup_header(s, &r, &h), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);
        MOQ_TEST_CHECK_EQ_U64(h.track_alias, 7); MOQ_TEST_CHECK_EQ_U64(h.group_id, 2);
        MOQ_TEST_CHECK(h.subgroup_id_resolved && !h.subgroup_id_from_first_object);
        MOQ_TEST_CHECK_EQ_U64(h.subgroup_id, 1);
        MOQ_TEST_CHECK_EQ_INT(h.publisher_priority, 3);
        MOQ_TEST_CHECK(!h.has_extensions && !h.end_of_group && !h.first_object);
        moq_subgroup_header_encode_args_t a = { .track_alias = 7, .group_id = 2, .subgroup_id = 1,
                                                .publisher_priority = 3 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_subgroup_header(s, &w, &a), (int)MOQ_OK);
        if (!bytes_equal("subgroup_header_flags_14", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        check_prefixes_incomplete("subgroup_header_flags_14", v, vlen, vlen, dec_subgroup_header, s, &h);
    } else failures++;

    /* subgroup_header_flags_54: 54 07 02 01 03 -- the same header with the
     * FIRST_OBJECT bit (0x40, §11.3.1, required by §2.2). */
    if (read_file(dir, "subgroup_header_flags_54.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_subgroup_header_t h; memset(&h, 0, sizeof(h));
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_subgroup_header(s, &r, &h), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);
        MOQ_TEST_CHECK(h.first_object);
        MOQ_TEST_CHECK_EQ_U64(h.track_alias, 7); MOQ_TEST_CHECK_EQ_U64(h.group_id, 2);
        MOQ_TEST_CHECK(h.subgroup_id_resolved && !h.subgroup_id_from_first_object);
        MOQ_TEST_CHECK_EQ_U64(h.subgroup_id, 1);
        MOQ_TEST_CHECK_EQ_INT(h.publisher_priority, 3);
        MOQ_TEST_CHECK(!h.has_extensions && !h.end_of_group && !h.default_priority);
        moq_subgroup_header_encode_args_t a = { .track_alias = 7, .group_id = 2, .subgroup_id = 1,
                                                .publisher_priority = 3, .first_object = true };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_subgroup_header(s, &w, &a), (int)MOQ_OK);
        if (!bytes_equal("subgroup_header_flags_54", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        check_prefixes_incomplete("subgroup_header_flags_54", v, vlen, vlen, dec_subgroup_header, s, &h);
    } else failures++;

    /* subgroup_object_first: 05 02 68 69 (first object: delta is the ID) */
    if (read_file(dir, "subgroup_object_first.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_object_header_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_header(s, &r, false, 0, false, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 2);                       /* the payload follows */
        MOQ_TEST_CHECK_EQ_U64(o.object_id, 5); MOQ_TEST_CHECK_EQ_U64(o.payload_len, 2);
        MOQ_TEST_CHECK_EQ_INT((int)o.status, (int)MOQ_OBJECT_NORMAL);
        MOQ_TEST_CHECK(!o.has_properties);
        MOQ_TEST_CHECK(memcmp(v + 2, "hi", 2) == 0);
        moq_object_header_encode_args_t a = { .object_id = 5, .payload_len = 2 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_header(s, &w, &a), (int)MOQ_OK);
        memcpy(enc + moq_buf_writer_offset(&w), "hi", 2);
        if (!bytes_equal("subgroup_object_first", enc, moq_buf_writer_offset(&w) + 2, v, vlen)) failures++;
        check_prefixes_incomplete("subgroup_object_first", v, vlen, 2, dec_object_first, s, &o);
    } else failures++;

    /* subgroup_object_status_eog: 00 00 03 following Object 5 -> Object 6 */
    if (read_file(dir, "subgroup_object_status_eog.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_object_header_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_header(s, &r, false, 5, true, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);
        MOQ_TEST_CHECK_EQ_U64(o.object_id, 6); MOQ_TEST_CHECK_EQ_U64(o.payload_len, 0);
        MOQ_TEST_CHECK_EQ_INT((int)o.status, (int)MOQ_OBJECT_END_OF_GROUP);
        moq_object_header_encode_args_t a = { .object_id = 6, .prev_object_id = 5, .has_prev_object = true,
                                              .payload_len = 0, .object_status = 0x3 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_header(s, &w, &a), (int)MOQ_OK);
        if (!bytes_equal("subgroup_object_status_eog", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        check_prefixes_incomplete("subgroup_object_status_eog", v, vlen, vlen, dec_object_after_5, s, &o);
    } else failures++;

    /* subgroup_object_prior_gaps: 04 04 3c 03 02 02 02 68 69 following
     * Object 5 on a PROPERTIES subgroup -> Object 10 with Prior Group ID Gap
     * 3 and Prior Object ID Gap 2 (§10.8, §10.9). Encoded as the session
     * writes an object whose cfg sets both gaps: the properties gain them in
     * type order, then the header and the payload prefix follow. */
    if (read_file(dir, "subgroup_object_prior_gaps.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_object_header_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_header(s, &r, true, 5, true, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen - 2);
        MOQ_TEST_CHECK_EQ_U64(o.object_id, 10); MOQ_TEST_CHECK_EQ_U64(o.payload_len, 2);
        MOQ_TEST_CHECK_EQ_INT((int)o.malformed, (int)MOQ_MALFORMED_NONE);
        MOQ_TEST_CHECK(o.gaps.has_group_gap && o.gaps.group_gap == 3);
        MOQ_TEST_CHECK(o.gaps.has_object_gap && o.gaps.object_gap == 2);
        moq_obj_gaps_t scanned;
        MOQ_TEST_CHECK_EQ_INT((int)p->scan_object_gaps(o.properties, o.properties_len, &scanned),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK(scanned.has_group_gap && scanned.group_gap == 3 &&
                       scanned.has_object_gap && scanned.object_gap == 2);
        uint8_t pa[16], pb[16];
        size_t plen = 0;
        MOQ_TEST_CHECK_EQ_INT((int)p->add_object_gap(NULL, 0, false, 3, pa, sizeof(pa), &plen), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)p->add_object_gap(pa, plen, true, 2, pb, sizeof(pb), &plen), (int)MOQ_OK);
        moq_object_header_encode_args_t a = { .object_id = 10, .prev_object_id = 5,
                                              .has_prev_object = true, .has_extensions = true,
                                              .properties_len = plen, .header_only = true };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_header(s, &w, &a), (int)MOQ_OK);
        size_t n = moq_buf_writer_offset(&w);
        memcpy(enc + n, pb, plen);
        n += plen;
        moq_buf_writer_init(&w, enc + n, sizeof(enc) - n);
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_payload_prefix(s, &w, 2, true), (int)MOQ_OK);
        n += moq_buf_writer_offset(&w);
        memcpy(enc + n, "hi", 2);
        if (!bytes_equal("subgroup_object_prior_gaps", enc, n + 2, v, vlen)) failures++;
        /* The same gap a second time conflicts; the same value is kept. */
        MOQ_TEST_CHECK_EQ_INT((int)p->add_object_gap(pb, plen, false, 4, pa, sizeof(pa), &plen),
                              (int)MOQ_ERR_INVAL);
    } else failures++;

    /* object_datagram_flags_00: 00 07 02 05 03 68 69 */
    if (read_file(dir, "object_datagram_flags_00.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_object_datagram_t d;
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_datagram(s, v, vlen, &d), (int)MOQ_OK);
        MOQ_TEST_CHECK(d.sub_slot >= 0 && !d.unknown_alias);
        MOQ_TEST_CHECK_EQ_U64(d.group_id, 2); MOQ_TEST_CHECK_EQ_U64(d.object_id, 5);
        MOQ_TEST_CHECK_EQ_INT(d.publisher_priority, 3);
        MOQ_TEST_CHECK(!d.default_priority && !d.end_of_group && !d.is_status);
        MOQ_TEST_CHECK_EQ_SIZE(d.payload_len, 2);
        MOQ_TEST_CHECK(d.payload && memcmp(d.payload, "hi", 2) == 0);
        MOQ_TEST_CHECK_EQ_SIZE(d.properties_len, 0);
        moq_datagram_encode_args_t a = { .track_alias = 7, .group_id = 2, .object_id = 5,
                                         .publisher_priority = 3,
                                         .payload = (const uint8_t *)"hi", .payload_len = 2 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_datagram(s, &w, &a), (int)MOQ_OK);
        if (!bytes_equal("object_datagram_flags_00", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        /* A datagram is one unit: a prefix shorter than its header is refused. */
        for (size_t n = 0; n < 5; n++)
            MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_datagram(s, v, n, &d), (int)MOQ_ERR_PROTO);
    } else failures++;

    /* object_datagram_properties: 01 07 02 05 03 02 3e 02 68 69 */
    if (read_file(dir, "object_datagram_properties.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_object_datagram_t d;
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_datagram(s, v, vlen, &d), (int)MOQ_OK);
        MOQ_TEST_CHECK(d.sub_slot >= 0);
        MOQ_TEST_CHECK_EQ_U64(d.group_id, 2); MOQ_TEST_CHECK_EQ_U64(d.object_id, 5);
        MOQ_TEST_CHECK_EQ_INT(d.publisher_priority, 3);
        MOQ_TEST_CHECK_EQ_SIZE(d.properties_len, 2);
        MOQ_TEST_CHECK(d.properties && d.properties[0] == 0x3e && d.properties[1] == 0x02);
        MOQ_TEST_CHECK_EQ_SIZE(d.payload_len, 2);
        MOQ_TEST_CHECK(d.payload && memcmp(d.payload, "hi", 2) == 0);
        static const uint8_t props[] = { 0x3e, 0x02 };
        moq_datagram_encode_args_t a = { .track_alias = 7, .group_id = 2, .object_id = 5,
                                         .publisher_priority = 3,
                                         .properties = props, .properties_len = sizeof(props),
                                         .payload = (const uint8_t *)"hi", .payload_len = 2 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_object_datagram(s, &w, &a), (int)MOQ_OK);
        if (!bytes_equal("object_datagram_properties", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        for (size_t n = 0; n < 8; n++)
            MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_datagram(s, v, n, &d), (int)MOQ_ERR_PROTO);
    } else failures++;

    /* fetch_header: 05 06 */
    if (read_file(dir, "fetch_header.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_fetch_stream_header_t h;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_header(s, &r, &h), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);
        MOQ_TEST_CHECK_EQ_U64(h.request_id, 6);
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_fetch_header(s, &w, 6), (int)MOQ_OK);
        if (!bytes_equal("fetch_header", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        check_prefixes_incomplete("fetch_header", v, vlen, vlen, dec_fetch_header, s, &h);
    } else failures++;

    /* fetch_object_first: 1f 03 01 04 03 02 68 69 */
    moq_fetch_prior_object_t prior; memset(&prior, 0, sizeof(prior));
    if (read_file(dir, "fetch_object_first.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(s, &r, MOQ_GROUP_ORDER_ASCENDING, NULL, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);                    /* payload consumed inline */
        MOQ_TEST_CHECK(!o.is_range_marker && !o.datagram);
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 3); MOQ_TEST_CHECK_EQ_U64(o.subgroup_id, 1);
        MOQ_TEST_CHECK_EQ_U64(o.object_id, 4);
        MOQ_TEST_CHECK_EQ_INT(o.publisher_priority, 3);
        MOQ_TEST_CHECK_EQ_U64(o.payload_len, 2);
        MOQ_TEST_CHECK_EQ_SIZE(o.properties_len, 0);
        MOQ_TEST_CHECK(memcmp(v + vlen - 2, "hi", 2) == 0);
        moq_fetch_object_encode_args_t a = { .group_id = 3, .subgroup_id = 1, .object_id = 4,
                                             .publisher_priority = 3, .payload_len = 2 };
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_fetch_object(s, &w, MOQ_GROUP_ORDER_ASCENDING, &a, NULL), (int)MOQ_OK);
        memcpy(enc + moq_buf_writer_offset(&w), "hi", 2);
        if (!bytes_equal("fetch_object_first", enc, moq_buf_writer_offset(&w) + 2, v, vlen)) failures++;
        check_prefixes_incomplete("fetch_object_first", v, vlen, vlen, dec_fetch_object_first, s, &o);
        prior.has_prev = true; prior.has_actual = true;
        prior.group_id = 3; prior.subgroup_id = 1; prior.object_id = 4; prior.publisher_priority = 3;
    } else failures++;

    /* fetch_object_eor_timed_out: 82 0c 05 09 -- Group ID and Object ID only */
    if (read_file(dir, "fetch_object_eor_timed_out.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(s, &r, MOQ_GROUP_ORDER_ASCENDING, &prior, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, vlen);
        MOQ_TEST_CHECK(o.is_range_marker);
        MOQ_TEST_CHECK_EQ_INT((int)o.range_kind, (int)MOQ_FETCH_RANGE_TIMED_OUT);
        MOQ_TEST_CHECK_EQ_U64(o.group_id, 5); MOQ_TEST_CHECK_EQ_U64(o.object_id, 9);
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_fetch_range(s, &w, MOQ_FETCH_RANGE_TIMED_OUT, 5, 9), (int)MOQ_OK);
        if (!bytes_equal("fetch_object_eor_timed_out", enc, moq_buf_writer_offset(&w), v, vlen)) failures++;
        for (size_t n = 0; n < vlen; n++) {
            moq_buf_reader_t pr; moq_buf_reader_init(&pr, v, n);
            MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(s, &pr, MOQ_GROUP_ORDER_ASCENDING, &prior, &o), (int)MOQ_ERR_BUFFER);
        }
    } else failures++;

    /* fetch_object_eor_timed_out_alt_payload_length: 82 0c 05 09 00 -- the
     * indicator carries no Object Payload Length, so it ends after four bytes
     * and the trailing 00 is not part of it: the alternate reading is refused
     * (and never produced by the encoder). */
    if (read_file(dir, "fetch_object_eor_timed_out_alt_payload_length.bin", v, sizeof(v), &vlen) == 0) {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_t r; moq_buf_reader_init(&r, v, vlen);
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(s, &r, MOQ_GROUP_ORDER_ASCENDING, &prior, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK(o.is_range_marker);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 4);
        MOQ_TEST_CHECK_EQ_SIZE(moq_buf_reader_remaining(&r), 1);
        moq_buf_writer_init(&w, enc, sizeof(enc));
        MOQ_TEST_CHECK_EQ_INT((int)p->encode_fetch_range(s, &w, MOQ_FETCH_RANGE_TIMED_OUT, 5, 9), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(moq_buf_writer_offset(&w), 4);
    } else failures++;

    fixture_close(&fx);
}

/* -- Invalid Type Flags on receipt ---------------------------------------- */

static void expect_subgroup_header_violation(const char *what, const uint8_t *hdr, size_t len)
{
    fixture_t fx;
    if (!fixture_open(&fx)) { fprintf(stderr, "FAIL %s: fixture\n", what); failures++; fixture_close(&fx); return; }
    moq_stream_ref_t ref = moq_stream_ref_from_u64(UINT64_C(0x7000));
    moq_result_t rc = moq_session_on_data_bytes(fx.client, ref, hdr, len, false,
                                                moq_simpair_now_us(fx.sp));
    if (moq_session_state(fx.client) != MOQ_SESS_CLOSED || close_code(fx.client) != 0x3) {
        fprintf(stderr, "FAIL %s: expected PROTOCOL_VIOLATION close (rc=%d state=%d)\n",
                what, (int)rc, (int)moq_session_state(fx.client));
        failures++;
    }
    fixture_close(&fx);
}

static void expect_datagram_violation(const char *what, const uint8_t *dg, size_t len)
{
    fixture_t fx;
    if (!fixture_open(&fx)) { fprintf(stderr, "FAIL %s: fixture\n", what); failures++; fixture_close(&fx); return; }
    moq_result_t rc = moq_session_on_datagram(fx.client, dg, len, moq_simpair_now_us(fx.sp));
    if (moq_session_state(fx.client) != MOQ_SESS_CLOSED || close_code(fx.client) != 0x3) {
        fprintf(stderr, "FAIL %s: expected PROTOCOL_VIOLATION close (rc=%d state=%d)\n",
                what, (int)rc, (int)moq_session_state(fx.client));
        failures++;
    }
    fixture_close(&fx);
}

static void test_invalid_flags(void)
{
    /* A valid header on the same subscription is accepted (the fixture itself
     * is sound), so the refusals below are the flags' doing. */
    {
        fixture_t fx;
        if (fixture_open(&fx)) {
            static const uint8_t ok[] = { 0x14, 0x07, 0x02, 0x01, 0x03 };
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_bytes(fx.client,
                moq_stream_ref_from_u64(UINT64_C(0x7000)), ok, sizeof(ok), false,
                moq_simpair_now_us(fx.sp)), (int)MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.client), (int)MOQ_SESS_ESTABLISHED);
            static const uint8_t okdg[] = { 0x00, 0x07, 0x02, 0x05, 0x03, 0x68, 0x69 };
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_datagram(fx.client, okdg, sizeof(okdg),
                moq_simpair_now_us(fx.sp)), (int)MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.client), (int)MOQ_SESS_ESTABLISHED);
        } else failures++;
        fixture_close(&fx);
    }

    /* Subgroup header: SUBGROUP_ID_MODE 0b11 (0x10 | 0x06). */
    static const uint8_t sg_mode3[] = { 0x16, 0x07, 0x02, 0x01, 0x03 };
    expect_subgroup_header_violation("subgroup flags mode 0b11", sg_mode3, sizeof(sg_mode3));
    /* Subgroup header: bit 4 clear (0x04 would otherwise read as an explicit Subgroup ID). */
    static const uint8_t sg_bit4[] = { 0x04, 0x07, 0x02, 0x01, 0x03 };
    expect_subgroup_header_violation("subgroup flags bit 4 clear", sg_bit4, sizeof(sg_bit4));
    /* Subgroup header: value 144 (two-byte vi64 80 90: bit 4 set, bit 7 set). */
    static const uint8_t sg_144[] = { 0x80, 0x90, 0x07, 0x02, 0x01, 0x03 };
    expect_subgroup_header_violation("subgroup flags >= 128 (144)", sg_144, sizeof(sg_144));
    /* Subgroup header: value 276 (81 14: the valid 0x14 pattern plus bit 8). */
    static const uint8_t sg_276[] = { 0x81, 0x14, 0x07, 0x02, 0x01, 0x03 };
    expect_subgroup_header_violation("subgroup flags >= 128 (276)", sg_276, sizeof(sg_276));

    /* Datagram: bit 4 (0x10) set. */
    static const uint8_t dg_bit4[] = { 0x10, 0x07, 0x02, 0x05, 0x03, 0x68, 0x69 };
    expect_datagram_violation("datagram flags bit 4", dg_bit4, sizeof(dg_bit4));
    /* Datagram: bit 6 (0x40) has no defined meaning. */
    static const uint8_t dg_bit6[] = { 0x40, 0x07, 0x02, 0x05, 0x03, 0x68, 0x69 };
    expect_datagram_violation("datagram flags unspecified bit 0x40", dg_bit6, sizeof(dg_bit6));
    /* Datagram: value 128 (80 80) has no defined meaning. */
    static const uint8_t dg_128[] = { 0x80, 0x80, 0x07, 0x02, 0x05, 0x03, 0x68, 0x69 };
    expect_datagram_violation("datagram flags unspecified bit 0x80", dg_128, sizeof(dg_128));
    /* Datagram: PROPERTIES set with a Properties Length of 0. */
    static const uint8_t dg_props0[] = { 0x01, 0x07, 0x02, 0x05, 0x03, 0x00, 0x68, 0x69 };
    expect_datagram_violation("datagram PROPERTIES with zero length", dg_props0, sizeof(dg_props0));
    /* Datagram: STATUS and END_OF_GROUP both set. */
    static const uint8_t dg_status_eog[] = { 0x22, 0x07, 0x02, 0x05, 0x03, 0x03 };
    expect_datagram_violation("datagram STATUS + END_OF_GROUP", dg_status_eog, sizeof(dg_status_eog));
    /* Datagram: STATUS and PROPERTIES with a non-Normal status (End of Group). */
    static const uint8_t dg_status_props[] = { 0x21, 0x07, 0x02, 0x05, 0x03, 0x02, 0x3e, 0x02, 0x03 };
    expect_datagram_violation("datagram STATUS + PROPERTIES, status End of Group",
                              dg_status_props, sizeof(dg_status_props));
}

/* -- Properties Length wider than the buffered input ---------------------- */

/* Properties Length 2^32 (vi64 f1 00 00 00 00). The bytes after it belong to
 * the missing property block; read as payload framing they would spell an
 * empty Normal object. */
static const uint8_t fetch_props_2p32[] = {           /* first object, all deltas */
    0x3c, 0x00, 0x00, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t subgroup_props_2p32[] = {        /* Object Delta 0 */
    0x00, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
/* The draft-16 forms: Extension Headers Length 2^32 as the QUIC varint
 * c0 00 00 01 00 00 00 00. The FETCH object's 0x3c carries Group ID, Object
 * ID, Publisher Priority and Extensions, the same field order. */
static const uint8_t d16_fetch_ext_2p32[] = {
    0x3c, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t d16_subgroup_ext_2p32[] = {
    0x00, 0xc0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
/* Controls, the same bytes in every profile: length 0, and 2 carrying one
 * property (type 0x20, which no draft assigns, value 2), each followed by
 * the one-byte payload 'A'. The objects are Object 0, so the property must not
 * be a Prior Object ID Gap: a gap larger than the Object ID makes the Track
 * malformed (draft-22 §10.9). */
static const uint8_t fetch_props_0[] = { 0x3c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x41 };
static const uint8_t fetch_props_2[] = { 0x3c, 0x00, 0x00, 0x00, 0x02, 0x20, 0x02, 0x01, 0x41 };
static const uint8_t subgroup_props_0[] = { 0x00, 0x00, 0x01, 0x41 };
static const uint8_t subgroup_props_2[] = { 0x00, 0x02, 0x20, 0x02, 0x01, 0x41 };
/* Subgroup header with PROPERTIES and an explicit Subgroup ID: alias 7,
 * group 0, subgroup 0, priority 0. */
static const uint8_t subgroup_header_props[] = { 0x15, TRACK_ALIAS, 0x00, 0x00, 0x00 };

static void check_props_codec(const moq_profile_ops_t *p)
{
    const bool d16 = p->version == MOQ_VERSION_DRAFT_16;
    const char *ver = d16 ? "d16" : p->version == MOQ_VERSION_DRAFT_22 ? "d22" : "d18";
    const uint8_t *fetch_trunc = d16 ? d16_fetch_ext_2p32 : fetch_props_2p32;
    size_t fetch_trunc_len = d16 ? sizeof(d16_fetch_ext_2p32) : sizeof(fetch_props_2p32);
    const uint8_t *sg_trunc = d16 ? d16_subgroup_ext_2p32 : subgroup_props_2p32;
    size_t sg_trunc_len = d16 ? sizeof(d16_subgroup_ext_2p32) : sizeof(subgroup_props_2p32);
    moq_buf_reader_t r;

    /* The 2^32 length and every prefix of either input are incomplete. */
    for (size_t n = 0; n <= fetch_trunc_len; n++) {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_init(&r, fetch_trunc, n);
        moq_result_t rc = p->decode_fetch_object(NULL, &r, MOQ_GROUP_ORDER_ASCENDING, NULL, &o);
        if (rc != MOQ_ERR_BUFFER) {
            fprintf(stderr, "FAIL %s fetch Properties Length 2^32, %zu bytes: rc=%d, expected incomplete\n",
                    ver, n, (int)rc);
            failures++;
        }
    }
    for (size_t n = 0; n <= sg_trunc_len; n++) {
        moq_decoded_object_header_t o;
        moq_buf_reader_init(&r, sg_trunc, n);
        moq_result_t rc = p->decode_object_header(NULL, &r, true, 0, false, &o);
        if (rc != MOQ_ERR_BUFFER) {
            fprintf(stderr, "FAIL %s subgroup Properties Length 2^32, %zu bytes: rc=%d, expected incomplete\n",
                    ver, n, (int)rc);
            failures++;
        }
    }

    {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_init(&r, fetch_props_0, sizeof(fetch_props_0));
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(NULL, &r, MOQ_GROUP_ORDER_ASCENDING, NULL, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, sizeof(fetch_props_0));
        MOQ_TEST_CHECK_EQ_SIZE(o.properties_len, 0);
        MOQ_TEST_CHECK_EQ_U64(o.payload_len, 1);
    }
    {
        moq_decoded_fetch_object_t o;
        moq_buf_reader_init(&r, fetch_props_2, sizeof(fetch_props_2));
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_fetch_object(NULL, &r, MOQ_GROUP_ORDER_ASCENDING, NULL, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, sizeof(fetch_props_2));
        MOQ_TEST_CHECK_EQ_SIZE(o.properties_len, 2);
        MOQ_TEST_CHECK(o.properties == fetch_props_2 + 5);
        MOQ_TEST_CHECK_EQ_U64(o.payload_len, 1);
    }
    {
        moq_decoded_object_header_t o;
        moq_buf_reader_init(&r, subgroup_props_0, sizeof(subgroup_props_0));
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_header(NULL, &r, true, 0, false, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 3);                       /* the payload follows */
        MOQ_TEST_CHECK(!o.has_properties);
        MOQ_TEST_CHECK_EQ_U64(o.payload_len, 1);
        MOQ_TEST_CHECK_EQ_INT((int)o.status, (int)MOQ_OBJECT_NORMAL);
    }
    {
        moq_decoded_object_header_t o;
        moq_buf_reader_init(&r, subgroup_props_2, sizeof(subgroup_props_2));
        MOQ_TEST_CHECK_EQ_INT((int)p->decode_object_header(NULL, &r, true, 0, false, &o), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(r.pos, 5);
        MOQ_TEST_CHECK(o.has_properties);
        MOQ_TEST_CHECK_EQ_SIZE(o.properties_len, 2);
        MOQ_TEST_CHECK(o.properties == subgroup_props_2 + 2);
        MOQ_TEST_CHECK_EQ_U64(o.payload_len, 1);
        MOQ_TEST_CHECK_EQ_INT((int)o.status, (int)MOQ_OBJECT_NORMAL);
    }
}

/* Drain the session's events: the number of objects delivered (whole, first
 * chunk, or fetch), and the payload and properties lengths of the last. */
typedef struct delivered {
    size_t objects;
    size_t payload_len;
    size_t properties_len;
    uint8_t first_byte;
    moq_object_status_t status;
} delivered_t;

static delivered_t drain_objects(moq_session_t *s)
{
    delivered_t d; memset(&d, 0, sizeof(d));
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        moq_rcbuf_t *payload = NULL, *props = NULL;
        bool object = true;
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            payload = ev.u.object_received.payload; props = ev.u.object_received.properties;
            d.status = ev.u.object_received.status;
        } else if (ev.kind == MOQ_EVENT_FETCH_OBJECT) {
            payload = ev.u.fetch_object.payload; props = ev.u.fetch_object.properties;
        } else if (ev.kind == MOQ_EVENT_OBJECT_CHUNK) {
            object = ev.u.object_chunk.begin;
            payload = ev.u.object_chunk.chunk;
        } else {
            object = false;
        }
        if (object) {
            d.objects++;
            d.payload_len = payload ? moq_rcbuf_len(payload) : 0;
            d.first_byte = d.payload_len ? moq_rcbuf_data(payload)[0] : 0;
            d.properties_len = props ? moq_rcbuf_len(props) : 0;
        }
        moq_event_cleanup(&ev);
    }
    return d;
}

/* Feed one subgroup stream (the header above, then obj) to a fresh session
 * of the given version; report what was delivered and the close code. */
static delivered_t feed_subgroup(moq_version_t version, const uint8_t *obj, size_t len,
                                 bool fin, moq_session_state_t *state, uint64_t *code)
{
    delivered_t d; memset(&d, 0, sizeof(d));
    *state = MOQ_SESS_IDLE; *code = UINT64_MAX;
    fixture_t fx;
    if (!fixture_open_version(&fx, version)) {
        fprintf(stderr, "FAIL d%u subgroup fixture\n", (unsigned)version);
        failures++; fixture_close(&fx); return d;
    }
    uint8_t wire[32];
    memcpy(wire, subgroup_header_props, sizeof(subgroup_header_props));
    memcpy(wire + sizeof(subgroup_header_props), obj, len);
    (void)moq_session_on_data_bytes(fx.client, moq_stream_ref_from_u64(UINT64_C(0x7000)),
                                    wire, sizeof(subgroup_header_props) + len, fin,
                                    moq_simpair_now_us(fx.sp));
    d = drain_objects(fx.client);
    *state = moq_session_state(fx.client);
    *code = close_code(fx.client);
    fixture_close(&fx);
    return d;
}

/* Feed obj on the FETCH response stream the publisher's acceptance opened. */
static delivered_t feed_fetch(moq_version_t version, const uint8_t *obj, size_t len,
                              bool fin, moq_session_state_t *state, uint64_t *code)
{
    delivered_t d; memset(&d, 0, sizeof(d));
    *state = MOQ_SESS_IDLE; *code = UINT64_MAX;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    if (version == MOQ_VERSION_DRAFT_16) {
        cfg.server_send_request_capacity = true;
        cfg.server_initial_request_capacity = 32;
    }
    moq_simpair_t *sp = NULL;
    bool ok = moq_simpair_create(&cfg, &sp) == MOQ_OK && moq_simpair_start(sp) == MOQ_OK;
    moq_session_t *client = ok ? moq_simpair_client(sp) : NULL;
    moq_session_t *server = ok ? moq_simpair_server(sp) : NULL;
    if (ok) moq_simpair_run_until_quiescent(sp, 32, NULL);
    ok = ok && moq_session_state(client) == MOQ_SESS_ESTABLISHED;

    moq_fetch_t client_fetch = {0}, server_fetch = {0};
    if (ok) {
        moq_fetch_cfg_t fc;
        moq_fetch_cfg_init_sized(&fc, sizeof(fc));
        const moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
        fc.track_namespace = (moq_namespace_t){ parts, 1 };
        fc.track_name = MOQ_BYTES_LITERAL("v");
        fc.start_group = 0;
        fc.end_group = 1;
        ok = moq_session_fetch(client, &fc, moq_simpair_now_us(sp), &client_fetch) == MOQ_OK;
    }
    if (ok) {
        moq_simpair_run_until_quiescent(sp, 32, NULL);
        bool got = false;
        moq_event_t ev;
        while (moq_session_poll_events(server, &ev, 1) > 0) {
            if (!got && ev.kind == MOQ_EVENT_FETCH_REQUEST) {
                server_fetch = ev.u.fetch_request.fetch; got = true;
            }
            moq_event_cleanup(&ev);
        }
        moq_accept_fetch_cfg_t acc;
        moq_accept_fetch_cfg_init(&acc);
        acc.end_group = 1;
        ok = got && moq_session_accept_fetch(server, server_fetch, &acc,
                                             moq_simpair_now_us(sp)) == MOQ_OK;
    }
    int slot = -1;
    if (ok) {
        moq_simpair_run_until_quiescent(sp, 32, NULL);
        (void)drain_objects(client);
        slot = fetch_resolve_handle(client, client_fetch);
        ok = slot >= 0 && client->fetches[slot].data_stream_started;
    }
    if (!ok) {
        fprintf(stderr, "FAIL d%u fetch fixture\n", (unsigned)version);
        failures++;
    } else {
        (void)moq_session_on_data_bytes(client, client->fetches[slot].data_stream_ref,
                                        obj, len, fin, moq_simpair_now_us(sp));
        d = drain_objects(client);
        *state = moq_session_state(client);
        *code = close_code(client);
    }
    if (sp) moq_simpair_destroy(sp);
    return d;
}

static void check_props_session(moq_version_t version)
{
    typedef delivered_t (*feed_fn)(moq_version_t, const uint8_t *, size_t, bool,
                                   moq_session_state_t *, uint64_t *);
    struct {
        const char *what;
        feed_fn feed;
        const uint8_t *truncated; size_t truncated_len;
        const uint8_t *zero; size_t zero_len;
        const uint8_t *small; size_t small_len;
    } kinds[] = {
        { "fetch", feed_fetch, fetch_props_2p32, sizeof(fetch_props_2p32),
          fetch_props_0, sizeof(fetch_props_0), fetch_props_2, sizeof(fetch_props_2) },
        { "subgroup", feed_subgroup, subgroup_props_2p32, sizeof(subgroup_props_2p32),
          subgroup_props_0, sizeof(subgroup_props_0), subgroup_props_2, sizeof(subgroup_props_2) },
    };
    if (version == MOQ_VERSION_DRAFT_16) {
        kinds[0].truncated = d16_fetch_ext_2p32;
        kinds[0].truncated_len = sizeof(d16_fetch_ext_2p32);
        kinds[1].truncated = d16_subgroup_ext_2p32;
        kinds[1].truncated_len = sizeof(d16_subgroup_ext_2p32);
    }
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        moq_session_state_t st; uint64_t code; delivered_t d;

        /* Without FIN the object stays incomplete: nothing delivered, session open. */
        d = kinds[k].feed(version, kinds[k].truncated, kinds[k].truncated_len, false, &st, &code);
        if (d.objects != 0 || st != MOQ_SESS_ESTABLISHED || code != UINT64_MAX) {
            fprintf(stderr, "FAIL d%u %s Properties Length 2^32, no FIN: objects=%zu state=%d code=%" PRIu64 "\n",
                    (unsigned)version, kinds[k].what, d.objects, (int)st, code);
            failures++;
        }
        /* A FIN inside the property block is PROTOCOL_VIOLATION, still with no object. */
        d = kinds[k].feed(version, kinds[k].truncated, kinds[k].truncated_len, true, &st, &code);
        if (d.objects != 0 || st != MOQ_SESS_CLOSED || code != 0x3) {
            fprintf(stderr, "FAIL d%u %s Properties Length 2^32 at FIN: objects=%zu state=%d code=%" PRIu64 "\n",
                    (unsigned)version, kinds[k].what, d.objects, (int)st, code);
            failures++;
        }
        /* Controls: zero and two property bytes deliver one object with payload 'A'. */
        d = kinds[k].feed(version, kinds[k].zero, kinds[k].zero_len, true, &st, &code);
        if (d.objects != 1 || d.payload_len != 1 || d.first_byte != 0x41 ||
            d.properties_len != 0 || st != MOQ_SESS_ESTABLISHED) {
            fprintf(stderr, "FAIL d%u %s Properties Length 0: objects=%zu payload=%zu props=%zu state=%d\n",
                    (unsigned)version, kinds[k].what, d.objects, d.payload_len, d.properties_len, (int)st);
            failures++;
        }
        d = kinds[k].feed(version, kinds[k].small, kinds[k].small_len, true, &st, &code);
        if (d.objects != 1 || d.payload_len != 1 || d.first_byte != 0x41 ||
            d.properties_len != 2 || st != MOQ_SESS_ESTABLISHED) {
            fprintf(stderr, "FAIL d%u %s Properties Length 2: objects=%zu payload=%zu props=%zu state=%d\n",
                    (unsigned)version, kinds[k].what, d.objects, d.payload_len, d.properties_len, (int)st);
            failures++;
        }
    }
}

/* -- Draft-16 Object Status is validated at full width ----------------------- */

/* Zero-length objects on the PROPERTIES subgroup stream (Extension Headers
 * Length 0) carrying an Object Status. 0x100 and 0x103 are two-byte QUIC
 * varints whose low byte is a registered status. */
static const struct {
    const char *what;
    uint8_t bytes[5];
    size_t len;
    bool valid;
    moq_object_status_t status;
} d16_status_rows[] = {
    { "status 0x100", { 0x00, 0x00, 0x00, 0x41, 0x00 }, 5, false, 0 },
    { "status 0x103", { 0x00, 0x00, 0x00, 0x41, 0x03 }, 5, false, 0 },
    { "status 0x1",   { 0x00, 0x00, 0x00, 0x01 },       4, false, 0 },
    { "status 0x0",   { 0x00, 0x00, 0x00, 0x00 },       4, true, MOQ_OBJECT_NORMAL },
    { "status 0x3",   { 0x00, 0x00, 0x00, 0x03 },       4, true, MOQ_OBJECT_END_OF_GROUP },
    { "status 0x4",   { 0x00, 0x00, 0x00, 0x04 },       4, true, MOQ_OBJECT_END_OF_TRACK },
};

static void check_d16_status(void)
{
    const moq_profile_ops_t *p = moq_d16_profile_ops();
    for (size_t i = 0; i < sizeof(d16_status_rows) / sizeof(d16_status_rows[0]); i++) {
        moq_decoded_object_header_t o;
        moq_buf_reader_t r;
        moq_buf_reader_init(&r, d16_status_rows[i].bytes, d16_status_rows[i].len);
        moq_result_t rc = p->decode_object_header(NULL, &r, true, 0, false, &o);
        moq_result_t want = d16_status_rows[i].valid ? MOQ_OK : MOQ_ERR_PROTO;
        if (rc != want || (rc == MOQ_OK && (o.status != d16_status_rows[i].status ||
                                            o.payload_len != 0 ||
                                            r.pos != d16_status_rows[i].len))) {
            fprintf(stderr, "FAIL d16 decode %s: rc=%d status=%d, expected rc=%d\n",
                    d16_status_rows[i].what, (int)rc, (int)o.status, (int)want);
            failures++;
        }

        /* Bound session, no FIN: an unregistered status closes with
         * PROTOCOL_VIOLATION and delivers nothing; a registered one delivers
         * exactly one object with that status. */
        moq_session_state_t st; uint64_t code;
        delivered_t d = feed_subgroup(MOQ_VERSION_DRAFT_16, d16_status_rows[i].bytes,
                                      d16_status_rows[i].len, false, &st, &code);
        bool ok = d16_status_rows[i].valid
            ? (d.objects == 1 && d.status == d16_status_rows[i].status &&
               d.payload_len == 0 && st == MOQ_SESS_ESTABLISHED && code == UINT64_MAX)
            : (d.objects == 0 && st == MOQ_SESS_CLOSED && code == 0x3);
        if (!ok) {
            fprintf(stderr, "FAIL d16 session %s: objects=%zu status=%d state=%d code=%" PRIu64 "\n",
                    d16_status_rows[i].what, d.objects, (int)d.status, (int)st, code);
            failures++;
        }
    }
}

/* -- FIRST_OBJECT end to end ------------------------------------------- *
 * §2.2: "When the Original Publisher opens a new subgroup, it MUST set the
 * FIRST_OBJECT bit". moq_subgroup_cfg_t.first_object sets it; the receiver
 * reports it on the stream's first object only (no earlier object of the
 * subgroup exists), never on later objects. A subgroup opened without it, or
 * by a caller whose struct_size ends before the field, sends the bit clear.
 * Draft-16 has no such bit: the request is ignored and nothing is reported. */
static int first_object_flags(moq_session_t *client, uint64_t group, bool *flags, size_t cap)
{
    int n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED && ev.u.object_received.group_id == group &&
            (size_t)n < cap)
            flags[n++] = ev.u.object_received.first_object;
        moq_event_cleanup(&ev);
    }
    return n;
}

static void check_first_object_session(moq_version_t version)
{
    fixture_t fx;
    if (!fixture_open_version(&fx, version)) {
        fprintf(stderr, "FAIL: first_object fixture v=%u\n", (unsigned)version);
        failures++; fixture_close(&fx); return;
    }
    const bool has_bit = version != MOQ_VERSION_DRAFT_16;
    for (int variant = 0; variant < 3; variant++) {
        moq_subgroup_cfg_t sgc;
        moq_subgroup_cfg_init(&sgc);
        sgc.group_id = 10 + (uint64_t)variant;
        sgc.first_object = variant != 1;
        if (variant == 2)   /* a caller built before the field */
            sgc.struct_size = (uint32_t)offsetof(moq_subgroup_cfg_t, _reserved_sg3);
        moq_subgroup_handle_t sg;
        uint64_t now = moq_simpair_now_us(fx.sp);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(fx.server, fx.server_sub, &sgc,
                                                             now, &sg), (int)MOQ_OK);
        for (uint64_t o = 0; o < 3; o++) {
            uint8_t d[1] = { (uint8_t)o };
            moq_rcbuf_t *pl = NULL;
            moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &pl);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.server, sg, o, pl, now),
                                  (int)MOQ_OK);
            moq_rcbuf_decref(pl);
        }
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.server, sg, now), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(fx.sp, 16, NULL);
        bool flags[3] = { true, true, true };
        MOQ_TEST_CHECK_EQ_INT(first_object_flags(fx.client, sgc.group_id, flags, 3), 3);
        MOQ_TEST_CHECK(flags[0] == (has_bit && variant == 0));
        MOQ_TEST_CHECK(!flags[1] && !flags[2]);
    }
    MOQ_TEST_CHECK(moq_session_state(fx.client) == MOQ_SESS_ESTABLISHED);
    fixture_close(&fx);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/vectors/d22";
    test_vectors(dir);
    test_invalid_flags();
    check_props_codec(moq_d16_profile_ops());
    check_props_codec(moq_d18_profile_ops());
    check_props_codec(moq_d22_profile_ops());
    check_props_session(MOQ_VERSION_DRAFT_16);
    check_props_session(MOQ_VERSION_DRAFT_18);
    check_props_session(MOQ_VERSION_DRAFT_22);
    check_d16_status();
    check_first_object_session(MOQ_VERSION_DRAFT_16);
    check_first_object_session(MOQ_VERSION_DRAFT_18);
    check_first_object_session(MOQ_VERSION_DRAFT_22);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    MOQ_TEST_PASS("vectors_d22_data");
    return 0;
}
