/*
 * Filters on draft-22 subscriptions. A Range Filter beyond the advertised
 * MAX_FILTER_RANGES (this side advertises none) is refused with INVALID_FILTER
 * on the request stream and never closes the session. A subscribe may give
 * its filter in the semantic form: a relative start more than one group back
 * and an absolute end object travel on 22, and the publisher reads the exact
 * form back; draft-18 carries only the forms its wire has.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/request_stream_internal.h"
#include "test_session_support.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <string.h>

static moq_simpair_t *mk_pair_with_cache(moq_version_t version, uint64_t cache_size)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    scfg.auth_token_cache_size = cache_size;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    if (moq_simpair_run_until_quiescent(sp, 16, NULL) < 0) {
        moq_simpair_destroy(sp);
        return NULL;
    }
    return sp;
}

static moq_simpair_t *mk_pair(moq_version_t version)
{
    return mk_pair_with_cache(version, 0);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    bool found = false;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (!found && ev.kind == kind) { *out = ev; found = true; continue; }
        moq_event_cleanup(&ev);
    }
    return found;
}

static uint64_t take_error_code(moq_session_t *sv, uint64_t ref)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                env.msg_type == MOQ_D22_REQUEST_ERROR) {
                moq_d22_message_context_t ctx;
                memset(&ctx, 0, sizeof(ctx));
                ctx.request_type = MOQ_D22_SUBSCRIBE;
                ctx.max_filter_ranges = 8;
                moq_d22_message_t m;
                if (moq_d22_decode_message(MOQ_D22_REQUEST_ERROR, env.payload,
                                           env.payload_len, &ctx, &m) == MOQ_OK)
                    code = m.code;
            }
        }
        moq_action_cleanup(&a);
    }
    return code;
}

static int test_range_filter_refused(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    /* SUBSCRIBE with one SUBGROUP_FILTER range 3-5 (delta encoded 3, 2). */
    uint8_t ns_buf[16];
    moq_buf_writer_t nw;
    moq_buf_writer_init(&nw, ns_buf, sizeof(ns_buf));
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { parts, 1 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_d22_encode_namespace(&nw, &ns), (int)MOQ_OK);
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_SUBSCRIBE;
    m.request_id = 0;
    m.ns.encoded.data = ns_buf; m.ns.encoded.len = moq_buf_writer_offset(&nw); m.ns.count = 1;
    m.track_name = MOQ_BYTES_LITERAL("v");
    static const uint8_t range_bytes[] = { 0x03, 0x02 };
    moq_d22_param_t p;
    memset(&p, 0, sizeof(p));
    p.type = 37;
    p.value.ranges.set_id = 0;
    p.value.ranges.ranges = (moq_bytes_t){ range_bytes, sizeof(range_bytes) };
    p.value.ranges.count = 1;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.max_filter_ranges = 8;   /* the sender's own limit: it may encode one */
    uint8_t buf[128];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    MOQ_TEST_CHECK_EQ_INT((int)moq_d22_encode_message(&w, &m, &p, 1, &ctx), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(0x7200),
        buf, moq_buf_writer_offset(&w), false, 5), (int)MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(take_error_code(sv, 0x7200), MOQ_REQUEST_ERROR_INVALID_FILTER);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Subscribe with a semantic filter; the publisher reads the exact form back. */
static int subscribe_with(moq_simpair_t *sp, const moq_location_filter_cfg_t *lf,
                          moq_result_t want, moq_location_filter_cfg_t *seen)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.struct_size = sizeof(cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    static int n; char name[8]; snprintf(name, sizeof(name), "t%d", n++);
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    cfg.location = lf;
    moq_subscription_t ch;
    moq_result_t rc = moq_session_subscribe(c, &cfg, 1, &ch);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)want);
    if (rc != MOQ_OK) return failures;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscription_filter(sv, sh, seen), (int)MOQ_OK);
    moq_location_filter_cfg_t mine; moq_location_filter_cfg_init(&mine);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscription_filter(c, ch, &mine), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(mine.kind, seen->kind);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

static int test_location_cfg(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    drain(moq_simpair_client(sp)); drain(moq_simpair_server(sp));
    moq_location_filter_cfg_t seen; moq_location_filter_cfg_init(&seen);
    bool d22 = version == MOQ_VERSION_DRAFT_22;

    /* Two groups before the next one: only draft-22 can say so. */
    moq_location_filter_cfg_t rel; moq_location_filter_cfg_init(&rel);
    rel.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP; rel.relative_group_offset = 2;
    failures += subscribe_with(sp, &rel, d22 ? MOQ_OK : MOQ_ERR_UNSUPPORTED, &seen);
    if (d22) {
        MOQ_TEST_CHECK_EQ_U64(seen.kind, MOQ_LOCATION_FILTER_RELATIVE_GROUP);
        MOQ_TEST_CHECK_EQ_U64(seen.relative_group_offset, 2);
    }
    /* The next group: every profile has it. */
    moq_location_filter_cfg_init(&rel);
    rel.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP; rel.relative_group_offset = 0;
    failures += subscribe_with(sp, &rel, MOQ_OK, &seen);
    MOQ_TEST_CHECK_EQ_U64(seen.kind, MOQ_LOCATION_FILTER_RELATIVE_GROUP);
    MOQ_TEST_CHECK_EQ_U64(seen.relative_group_offset, 0);
    /* An absolute end object: only draft-22 can say so. */
    moq_location_filter_cfg_t rng; moq_location_filter_cfg_init(&rng);
    rng.kind = MOQ_LOCATION_FILTER_OBJECT_END;
    rng.start_group = 1; rng.start_object = 0; rng.end_group = 3; rng.end_object = 4;
    failures += subscribe_with(sp, &rng, d22 ? MOQ_OK : MOQ_ERR_UNSUPPORTED, &seen);
    if (d22) {
        MOQ_TEST_CHECK_EQ_U64(seen.kind, MOQ_LOCATION_FILTER_OBJECT_END);
        MOQ_TEST_CHECK_EQ_U64(seen.end_group, 3);
        MOQ_TEST_CHECK_EQ_U64(seen.end_object, 4);
    }
    /* A group end: every profile has it. */
    moq_location_filter_cfg_init(&rng);
    rng.kind = MOQ_LOCATION_FILTER_GROUP_END;
    rng.start_group = 1; rng.start_object = 2; rng.end_group = 3;
    failures += subscribe_with(sp, &rng, MOQ_OK, &seen);
    MOQ_TEST_CHECK_EQ_U64(seen.kind, MOQ_LOCATION_FILTER_GROUP_END);
    MOQ_TEST_CHECK_EQ_U64(seen.start_object, 2);
    MOQ_TEST_CHECK_EQ_U64(seen.end_group, 3);
    /* An inverted range is refused before anything is sent. */
    rng.end_group = 0;
    failures += subscribe_with(sp, &rng, MOQ_ERR_INVAL, &seen);
    moq_simpair_destroy(sp);
    return failures;
}

/* Independent bodies: ID 2, namespace "live", track "v" where required,
 * one Subgroup Filter (SetID 0, range 3..5). No production encoder supplies
 * these expectations. The track-update filter instead names Track Property 2. */
typedef struct {
    const char *name;
    uint64_t type, target;
    uint8_t body[32];
    size_t len, params_offset;
} filter_case_t;

static const filter_case_t filter_cases[] = {
    {"SUBSCRIBE", MOQ_D22_SUBSCRIBE, 0,
     {2, 1, 4, 'l', 'i', 'v', 'e', 1, 'v', 1, 0x25, 3, 0, 3, 2}, 15, 9},
    {"FETCH", MOQ_D22_FETCH, 0,
     {2, 1, 4, 'l', 'i', 'v', 'e', 1, 'v', 1, 0x25, 3, 0, 3, 2}, 15, 9},
    {"SUBSCRIBE_TRACKS", MOQ_D22_SUBSCRIBE_TRACKS, 0,
     {2, 1, 4, 'l', 'i', 'v', 'e', 1, 0x25, 3, 0, 3, 2}, 13, 7},
    {"SUBSCRIBE update", MOQ_D22_REQUEST_UPDATE, MOQ_D22_SUBSCRIBE,
     {2, 1, 0x25, 3, 0, 3, 2}, 7, 1},
    {"PUBLISH subscriber update", MOQ_D22_REQUEST_UPDATE, MOQ_D22_PUBLISH,
     {2, 1, 0x25, 3, 0, 3, 2}, 7, 1},
    {"SUBSCRIBE_TRACKS update", MOQ_D22_REQUEST_UPDATE, MOQ_D22_SUBSCRIBE_TRACKS,
     {2, 1, 0x29, 4, 0, 2, 3, 2}, 8, 1},
};

static size_t filter_wire(const filter_case_t *fc, uint8_t *wire)
{
    size_t off = 0;
    /* All fixture message types fit the one-byte vi64 interval 0..127. */
    wire[off++] = (uint8_t)fc->type;
    wire[off++] = 0;
    wire[off++] = (uint8_t)fc->len;
    memcpy(wire + off, fc->body, fc->len);
    return off + fc->len;
}

static int test_decode_consumers(bool tracks)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    const moq_request_codec_t *codec = sv->profile->request_codec;
    for (size_t i = 0; i < sizeof(filter_cases) / sizeof(filter_cases[0]); ++i) {
        const filter_case_t *fc = &filter_cases[i];
        if ((fc->type == MOQ_D22_SUBSCRIBE_TRACKS ||
             fc->target == MOQ_D22_SUBSCRIBE_TRACKS) != tracks) continue;
        fprintf(stderr, "decode consumer: %s\n", fc->name);
        moq_request_tokens_t tokens = {0};
        uint64_t id = UINT64_MAX;
        moq_result_t rc;
        if (fc->type == MOQ_D22_SUBSCRIBE) {
            moq_decoded_subscribe_t d;
            rc = codec->decode_subscribe(fc->body, fc->len, &d, &id, &tokens);
            MOQ_TEST_CHECK_EQ_U64(d.request_id, 2);
        } else if (fc->type == MOQ_D22_FETCH) {
            moq_decoded_fetch_t d;
            rc = codec->decode_fetch(fc->body, fc->len, &d, &id, &tokens);
            MOQ_TEST_CHECK_EQ_U64(d.request_id, 2);
        } else if (fc->type == MOQ_D22_SUBSCRIBE_TRACKS) {
            moq_decoded_subscribe_tracks_request_t d;
            rc = codec->decode_subscribe_tracks(fc->body, fc->len, &d, &id, &tokens);
        } else {
            moq_decoded_request_update_t d = {0};
            moq_request_family_t family = fc->target == MOQ_D22_SUBSCRIBE ?
                MOQ_REQUEST_FAMILY_SUBSCRIBE : fc->target == MOQ_D22_PUBLISH ?
                MOQ_REQUEST_FAMILY_PUBLISH : MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS;
            rc = codec->decode_request_update(fc->body, fc->len, family,
                                              true, &d, &tokens);
            id = d.request_id;
        }
        MOQ_TEST_CHECK_EQ_INT((int)rc, MOQ_REQUEST_DECODE_INVALID_FILTER);
        MOQ_TEST_CHECK_EQ_U64(id, 2);
        MOQ_TEST_CHECK_EQ_SIZE(tokens.count, 0);
    }
    moq_simpair_destroy(sp);
    return failures;
}

static int test_decode_atomicity(void)
{
    int failures = 0;
    for (size_t i = 0; i < sizeof(filter_cases) / sizeof(filter_cases[0]); ++i) {
        const filter_case_t *fc = &filter_cases[i];
        moq_d22_message_context_t ctx = {0};
        ctx.request_type = fc->target;
        ctx.update_from_subscriber = true;
        moq_d22_message_t out, before;
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        moq_d22_message_t scanned = before;
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, fc->body,
            fc->len, &ctx, &out), MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        for (size_t n = 0; n < fc->len; ++n) {
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, fc->body,
                n, &ctx, &out), (int)MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, fc->body,
                n, &ctx, &scanned), (int)MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&scanned, &before, sizeof(scanned)) == 0);
        }
        uint8_t bad[32];
        memcpy(bad, fc->body, fc->len);
        bad[fc->len] = 0;
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, bad,
            fc->len + 1, &ctx, &out), (int)MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, bad,
            fc->len + 1, &ctx, &scanned), (int)MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(memcmp(&scanned, &before, sizeof(scanned)) == 0);
        /* A malformed second parameter must outrank the earlier refusal. */
        bad[fc->params_offset] = 2;
        bad[fc->len] = 1;
        MOQ_TEST_CHECK(moq_d22_decode_message(fc->type, bad,
            fc->len + 1, &ctx, &out) != MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        MOQ_TEST_CHECK(moq_d22_scan_message(fc->type, bad,
            fc->len + 1, &ctx, &scanned) != MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK(memcmp(&scanned, &before, sizeof(scanned)) == 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, fc->body,
            fc->len, &ctx, &scanned), MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK_EQ_U64(scanned.request_id, 2);
        MOQ_TEST_CHECK_EQ_U64(scanned.type, fc->type);
        MOQ_TEST_CHECK_EQ_SIZE(scanned.params.count, 1);
        memcpy(bad, fc->body, fc->params_offset);
        bad[fc->params_offset] = 0;
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, bad,
            fc->params_offset + 1, &ctx, &out), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(out.request_id, 2);
    }
    return failures;
}

/* A real public SUBSCRIBE/accept exchange consumes ID 0 before injected ID 2. */
static int establish_prior(moq_simpair_t *sp, moq_stream_ref_t *ref)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_version(c), (int)MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_version(sv), (int)MOQ_VERSION_DRAFT_22);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("prior");
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    bool found = take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) return failures;
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int slot = sub_resolve_handle(sv, sh);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) return failures;
    *ref = sv->subs[slot].rs.ref;
    MOQ_TEST_CHECK_EQ_U64(sv->subs[slot].request_id, 0);
    moq_accept_subscribe_cfg_t acc; moq_accept_subscribe_cfg_init(&acc);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &acc, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    found = take(c, MOQ_EVENT_SUBSCRIBE_OK, &ev);
    MOQ_TEST_CHECK(found);
    if (found) moq_event_cleanup(&ev);
    drain(sv);
    MOQ_TEST_CHECK_EQ_U64(moq_request_stream_peer_next_id(sv), 2);
    return failures;
}

static int count_unexpected_events(moq_session_t *s)
{
    int failures = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1)) {
        MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_SUBSCRIBE_REQUEST &&
            ev.kind != MOQ_EVENT_FETCH_REQUEST &&
            ev.kind != MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST &&
            ev.kind != MOQ_EVENT_SUBSCRIBE_UPDATED &&
            ev.kind != MOQ_EVENT_PUBLISH_UPDATED &&
            ev.kind != MOQ_EVENT_SESSION_CLOSED);
        moq_event_cleanup(&ev);
    }
    return failures;
}

static int expect_filter_error(moq_session_t *s, moq_stream_ref_t ref, bool update)
{
    int failures = 0;
    size_t errors = 0, dones = 0, fins = 0;
    /* Responses contain no Request ID; identity is the exact request bidi.
     * Bare REQUEST_ERROR: type 5, length 3, code 0x36, retry 0, empty reason. */
    static const uint8_t error[] = {5, 0, 3, 0x36, 0, 0};
    /* No streams were opened: UPDATE_FAILED, count 0, empty reason. */
    static const uint8_t done[] = {0x0b, 0, 3, 8, 0, 0};
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION)
            fprintf(stderr, "unexpected close code=0x%llx\n",
                    (unsigned long long)a.u.close_session.code);
        MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            MOQ_TEST_CHECK_EQ_U64(a.u.send_bidi_stream.stream_ref._v, ref._v);
            const uint8_t *p = a.u.send_bidi_stream.data;
            size_t len = a.u.send_bidi_stream.len;
            size_t off = 0;
            while (off < len) {
                MOQ_TEST_CHECK(len - off >= 3);
                if (len - off < 3) break;
                size_t n = 3 + ((size_t)p[off + 1] << 8) + p[off + 2];
                MOQ_TEST_CHECK(n <= len - off);
                if (n > len - off) break;
                if (p[off] == 5) {
                    MOQ_TEST_CHECK_EQ_SIZE(n, sizeof(error));
                    MOQ_TEST_CHECK(n == sizeof(error) &&
                        memcmp(p + off, error, sizeof(error)) == 0);
                    errors++;
                } else {
                    MOQ_TEST_CHECK(update && p[off] == 0x0b);
                    MOQ_TEST_CHECK(n == sizeof(done) &&
                        memcmp(p + off, done, sizeof(done)) == 0);
                    MOQ_TEST_CHECK_EQ_SIZE(errors, 1);
                    dones++;
                }
                off += n;
            }
            if (a.u.send_bidi_stream.fin) fins++;
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(errors, 1);
    MOQ_TEST_CHECK_EQ_SIZE(dones, update ? 1 : 0);
    MOQ_TEST_CHECK_EQ_SIZE(fins, 1);
    return failures;
}

static int test_nonzero_refusal(size_t which, int pressure)
{
    int failures = 0;
    const filter_case_t *fc = &filter_cases[which];
    fprintf(stderr, "ingress: %s, pressure=%d\n", fc->name, pressure);
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    moq_stream_ref_t prior_ref = {0};
    failures += establish_prior(sp, &prior_ref);
    if (failures) { moq_simpair_destroy(sp); return failures; }
    bool update = fc->type == MOQ_D22_REQUEST_UPDATE;
    moq_stream_ref_t ref = update ? prior_ref : moq_stream_ref_from_u64(0x7300 + which);
    if (pressure == 1) test_session_fill_action_queue(sv);
    if (pressure == 2) {
        moq_event_t filler = {0};
        filler.kind = MOQ_EVENT_SUBSCRIBE_OK;
        while (push_event(sv, &filler) == MOQ_OK) { }
    }
    uint8_t wire[32];
    size_t n = filter_wire(fc, wire);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(sv, ref, wire, n, false, 5);
    /* FETCH auto-refusal reserves output, but needs no application event. */
    bool blocked = pressure && !(pressure == 2 && fc->type == MOQ_D22_FETCH);
    MOQ_TEST_CHECK_EQ_INT((int)rc, blocked ? (int)MOQ_ERR_WOULD_BLOCK : (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    if (blocked) {
        MOQ_TEST_CHECK_EQ_U64(moq_request_stream_peer_next_id(sv), 2);
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1)) {
            MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION &&
                a.kind != MOQ_ACTION_SEND_BIDI_STREAM);
            moq_action_cleanup(&a);
        }
        drain(sv);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv, ref,
            NULL, 0, false, 6), (int)MOQ_OK);
    }
    failures += count_unexpected_events(sv);
    failures += expect_filter_error(sv, ref, update);
    MOQ_TEST_CHECK_EQ_U64(moq_request_stream_peer_next_id(sv), 4);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    /* A later valid SUBSCRIBE (ID 4) must still reach the application. */
    static const uint8_t next[] = {3, 0, 10, 4, 1, 4, 'l', 'i', 'v', 'e', 1, 'w', 0};
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv,
        moq_stream_ref_from_u64(0x7400), next, sizeof(next), false, 7), (int)MOQ_OK);
    moq_event_t ev;
    bool found = take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (found) {
        int slot = sub_resolve_handle(sv, ev.u.subscribe_request.sub);
        MOQ_TEST_CHECK(slot >= 0);
        if (slot >= 0) MOQ_TEST_CHECK_EQ_U64(sv->subs[slot].request_id, 4);
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_U64(moq_request_stream_peer_next_id(sv), 6);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* ID 130, REGISTER alias 7/type 0/value aa, nested subgroup range 3..5.
 * Three request contexts share the two fill-projector callsites. */
static const filter_case_t nested_cases[] = {
    {"nested SUBSCRIBE", MOQ_D22_SUBSCRIBE, 0,
     {0x80, 0x82, 0, 1, 'x', 2, 3, 4, 1, 7, 0, 0xaa,
      32, 6, 1, 37, 3, 9, 3, 2}, 20, 5},
    {"nested SUBSCRIBE update", MOQ_D22_REQUEST_UPDATE, MOQ_D22_SUBSCRIBE,
     {0x80, 0x82, 2, 3, 4, 1, 7, 0, 0xaa,
      32, 6, 1, 37, 3, 9, 3, 2}, 17, 2},
    {"nested PUBLISH update", MOQ_D22_REQUEST_UPDATE, MOQ_D22_PUBLISH,
     {0x80, 0x82, 2, 3, 4, 1, 7, 0, 0xaa,
      32, 6, 1, 37, 3, 9, 3, 2}, 17, 2},
};

static moq_result_t decode_nested(const moq_request_codec_t *codec,
    const filter_case_t *fc, const uint8_t *body, size_t len, bool subscriber,
    uint64_t *id, moq_request_tokens_t *tokens, moq_fill_input_t *fill)
{
    moq_result_t rc;
    if (fc->type == MOQ_D22_SUBSCRIBE) {
        moq_decoded_subscribe_t d = {0};
        rc = codec->decode_subscribe(body, len, &d, id, tokens);
        *fill = d.fill;
    } else {
        moq_decoded_request_update_t d = {0};
        rc = codec->decode_request_update(body, len,
            fc->target == MOQ_D22_SUBSCRIBE ? MOQ_REQUEST_FAMILY_SUBSCRIBE :
                                             MOQ_REQUEST_FAMILY_PUBLISH,
            subscriber, &d, tokens);
        *id = d.request_id;
        *fill = d.fill;
    }
    return rc;
}

static int test_nested_decode(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_version(sv), MOQ_VERSION_DRAFT_22);
    for (size_t i = 0; i < sizeof(nested_cases) / sizeof(nested_cases[0]); ++i) {
        const filter_case_t *fc = &nested_cases[i];
        /* Updates are projected for both directions; FILL is a separate scope.
         * Only the subscriber's update may carry FILL_PARAMETERS: on the
         * publisher's update of its own PUBLISH it is outside the message's
         * parameters. */
        for (int direction = 0; direction < (i ? 2 : 1); ++direction) {
            moq_request_tokens_t tokens = {0};
            moq_fill_input_t fill;
            uint64_t id = UINT64_C(0xfeed);
            if (i && direction == 0) {
                MOQ_TEST_CHECK_EQ_INT((int)decode_nested(sv->profile->request_codec,
                    fc, fc->body, fc->len, false, &id, &tokens, &fill), MOQ_ERR_PROTO);
                continue;
            }
            MOQ_TEST_CHECK_EQ_INT((int)decode_nested(sv->profile->request_codec,
                fc, fc->body, fc->len, direction != 0, &id, &tokens, &fill),
                MOQ_REQUEST_DECODE_INVALID_FILTER);
            MOQ_TEST_CHECK_EQ_U64(id, 130);
            MOQ_TEST_CHECK(fill.requested);
            MOQ_TEST_CHECK_EQ_SIZE(tokens.count, 1);
            MOQ_TEST_CHECK_EQ_U64(tokens.in[0].alias, 7);
            MOQ_TEST_CHECK_EQ_INT((int)tokens.in[0].op, MOQ_AUTH_OP_REGISTER);
            MOQ_TEST_CHECK_EQ_U64(tokens.in[0].token_type, 0);
            MOQ_TEST_CHECK_EQ_SIZE(tokens.in[0].token_value_len, 1);
            MOQ_TEST_CHECK(tokens.in[0].token_value == fc->body + fc->params_offset + 6);

            /* Normal non-range fill projection remains complete. */
            uint8_t normal[32];
            memcpy(normal, fc->body, fc->len - 6);
            normal[fc->len - 7] = 7;
            static const uint8_t settings[] = {3, 10, 17, 22, 9, 2, 2};
            memcpy(normal + fc->len - 6, settings, sizeof(settings));
            MOQ_TEST_CHECK_EQ_INT((int)decode_nested(sv->profile->request_codec,
                fc, normal, fc->len + 1, direction != 0, &id, &tokens, &fill), MOQ_OK);
            MOQ_TEST_CHECK_EQ_U64(id, 130);
            MOQ_TEST_CHECK(fill.requested && fill.has_fill_timeout &&
                fill.fill_timeout_ms == 17 && fill.has_subscriber_priority &&
                fill.subscriber_priority == 9 && fill.has_group_order &&
                fill.group_order == MOQ_GROUP_ORDER_DESCENDING);
        }
        moq_d22_message_context_t ctx = {.request_type = fc->target,
                                       .update_from_subscriber = true};
        moq_d22_message_t out, before;
        memset(&before, 0xa5, sizeof(before));
        memcpy(&out, &before, sizeof(out));
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, fc->body,
            fc->len, &ctx, &out), MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, fc->body,
            fc->len, &ctx, &out), MOQ_D22_ERR_INVALID_FILTER);
        MOQ_TEST_CHECK_EQ_U64(out.request_id, 130);
        for (size_t n = 0; n < fc->len; ++n) {
            memcpy(&out, &before, sizeof(out));
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, fc->body,
                n, &ctx, &out), MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, fc->body,
                n, &ctx, &out), MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        }
        for (int bad_kind = 0; bad_kind < 2; ++bad_kind) {
            uint8_t bad[32];
            memcpy(bad, fc->body, fc->len);
            bad[fc->len] = bad_kind ? 18 : 0;
            bad[fc->len + 1] = 2;
            if (bad_kind) bad[fc->params_offset] = 3;
            size_t len = fc->len + (bad_kind ? 2 : 1);
            memcpy(&out, &before, sizeof(out));
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_scan_message(fc->type, bad, len,
                &ctx, &out), MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
            MOQ_TEST_CHECK_EQ_INT((int)moq_d22_decode_message(fc->type, bad, len,
                &ctx, &out), MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(memcmp(&out, &before, sizeof(out)) == 0);
        }
    }
    moq_simpair_destroy(sp);
    return failures;
}

static int establish_nested_target(moq_simpair_t *sp, size_t which,
                                   moq_stream_ref_t *ref)
{
    int failures = establish_prior(sp, ref);
    if (failures || which != 2) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("live")};
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("pub");
    moq_publication_t pub;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(sv, &cfg, 3, &pub), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    bool found = take(c, MOQ_EVENT_PUBLISH_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) return failures;
    moq_publication_t peer_pub = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(c, peer_pub, &ac, 4), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    found = take(sv, MOQ_EVENT_PUBLISH_OK, &ev);
    MOQ_TEST_CHECK(found);
    if (found) moq_event_cleanup(&ev);
    int slot = pub_resolve_handle(sv, pub);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot >= 0) *ref = sv->publishes[slot].rs.ref;
    drain(c); drain(sv);
    return failures;
}

static int test_nested_ingress(size_t which, int pressure, int malformed)
{
    int failures = 0;
    const filter_case_t *fc = &nested_cases[which];
    fprintf(stderr, "%s pressure=%d malformed=%d\n", fc->name, pressure, malformed);
    moq_simpair_t *sp = mk_pair_with_cache(MOQ_VERSION_DRAFT_22, 1024);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    moq_stream_ref_t ref = {0};
    failures += establish_nested_target(sp, which, &ref);
    if (failures) { moq_simpair_destroy(sp); return failures; }
    bool update = which != 0;
    if (!update) ref = moq_stream_ref_from_u64(0x7500);
    filter_case_t body = *fc;
    if (malformed == 1) --body.len;
    if (malformed == 2) body.body[body.len++] = 0;
    if (malformed == 3) {
        body.body[body.params_offset] = 3;
        body.body[body.len++] = 18;
        body.body[body.len++] = 2;
    }
    if (pressure == 1) test_session_fill_action_queue(sv);
    if (pressure == 2) {
        moq_event_t filler = {0};
        filler.kind = MOQ_EVENT_SUBSCRIBE_OK;
        while (push_event(sv, &filler) == MOQ_OK) { }
    }
    uint8_t wire[32];
    size_t n = filter_wire(&body, wire);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(sv, ref, wire, n, false, 5);
    moq_request_stream_state_t *rs = sv->profile_state;
    if (malformed) {
        MOQ_TEST_CHECK_EQ_INT((int)rc, MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), MOQ_SESS_CLOSED);
        MOQ_TEST_CHECK_EQ_U64(rs->peer_next_request_id, 2);
        MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 0);
        MOQ_TEST_CHECK_EQ_SIZE(sv->peer_token_cache.used_bytes, 0);
        MOQ_TEST_CHECK_EQ_SIZE(sv->fills.live, 0);
        size_t closes = 0;
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1)) {
            if (a.kind == MOQ_ACTION_CLOSE_SESSION) {
                MOQ_TEST_CHECK_EQ_U64(a.u.close_session.code, 3);
                ++closes;
            }
            MOQ_TEST_CHECK(a.kind != MOQ_ACTION_SEND_BIDI_STREAM);
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK_EQ_SIZE(closes, 1);
        moq_event_t ev;
        while (moq_session_poll_events(sv, &ev, 1)) {
            MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_SUBSCRIBE_REQUEST &&
                ev.kind != MOQ_EVENT_SUBSCRIBE_UPDATED &&
                ev.kind != MOQ_EVENT_PUBLISH_UPDATED);
            moq_event_cleanup(&ev);
        }
        moq_simpair_destroy(sp);
        return failures;
    }
    bool blocked = pressure == 1 || (pressure == 2 && !update);
    MOQ_TEST_CHECK_EQ_INT((int)rc, blocked ? MOQ_ERR_WOULD_BLOCK : MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), MOQ_SESS_ESTABLISHED);
    if (blocked) {
        MOQ_TEST_CHECK_EQ_U64(rs->peer_next_request_id, 2);
        MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 0);
        MOQ_TEST_CHECK_EQ_SIZE(sv->peer_token_cache.used_bytes, 0);
        MOQ_TEST_CHECK_EQ_SIZE(sv->fills.live, 0);
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1)) {
            MOQ_TEST_CHECK(a.kind != MOQ_ACTION_SEND_BIDI_STREAM &&
                           a.kind != MOQ_ACTION_CLOSE_SESSION);
            moq_action_cleanup(&a);
        }
        drain(sv);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv, ref,
            NULL, 0, false, 6), MOQ_OK);
    }
    failures += count_unexpected_events(sv);
    failures += expect_filter_error(sv, ref, update);
    MOQ_TEST_CHECK_EQ_U64(rs->peer_next_request_id, 2);
    MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 1);
    if (rs->peer_seen_count == 1) {
        MOQ_TEST_CHECK_EQ_U64(rs->peer_seen_later[0].first, 130);
        MOQ_TEST_CHECK_EQ_U64(rs->peer_seen_later[0].last, 130);
    }
    MOQ_TEST_CHECK_EQ_SIZE(sv->peer_token_cache.used_bytes, MOQ_TOKEN_ENTRY_OVERHEAD + 1);
    MOQ_TEST_CHECK_EQ_SIZE(sv->fills.live, 0);
    memset(wire, 0, sizeof(wire));
    /* The next request resolves the registered, owned value through USE_ALIAS. */
    static const uint8_t next[] = {3, 0, 10, 0x80, 0x84, 0, 1, 'y', 1, 3, 2, 2, 7};
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv,
        moq_stream_ref_from_u64(0x7600), next, sizeof(next), false, 7), MOQ_OK);
    moq_event_t ev;
    bool found = take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (found) {
        int slot = sub_resolve_handle(sv, ev.u.subscribe_request.sub);
        MOQ_TEST_CHECK(slot >= 0);
        if (slot >= 0) MOQ_TEST_CHECK_EQ_U64(sv->subs[slot].request_id, 132);
        MOQ_TEST_CHECK_EQ_SIZE(ev.u.subscribe_request.token_count, 1);
        if (ev.u.subscribe_request.token_count == 1) {
            const moq_resolved_token_t *token = ev.u.subscribe_request.tokens;
            MOQ_TEST_CHECK_EQ_U64(token->token_type, 0);
            MOQ_TEST_CHECK_EQ_SIZE(token->token_value.len, 1);
            MOQ_TEST_CHECK(token->token_value.len == 1 && token->token_value.data[0] == 0xaa);
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 1);
    if (rs->peer_seen_count == 1) MOQ_TEST_CHECK_EQ_U64(rs->peer_seen_later[0].last, 132);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_nested_publisher_update(int pressure)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair_with_cache(MOQ_VERSION_DRAFT_22, 1024);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_stream_ref_t server_ref = {0};
    failures += establish_nested_target(sp, 2, &server_ref);
    if (failures) goto cleanup;
    moq_session_t *c = moq_simpair_client(sp);
    moq_stream_ref_t ref = {0};
    bool found_owner = false;
    for (size_t i = 0; i < c->pub_cap; ++i) {
        if (c->publishes[i].state == MOQ_PUB_ESTABLISHED &&
            c->publishes[i].role == MOQ_PUB_ROLE_SUBSCRIBER) {
            ref = c->publishes[i].rs.ref;
            found_owner = true;
            break;
        }
    }
    MOQ_TEST_CHECK(found_owner);
    if (!found_owner) goto cleanup;
    moq_request_stream_state_t *rs = c->profile_state;
    MOQ_TEST_CHECK_EQ_U64(rs->peer_next_request_id, 3);
    MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 0);
    filter_case_t body = nested_cases[2];
    body.body[1] = 0x83; /* Server request ID 131, after real PUBLISH ID 1. */
    uint8_t wire[32];
    size_t n = filter_wire(&body, wire);
    if (pressure == 1) test_session_fill_action_queue(c);
    if (pressure == 2) {
        moq_event_t filler = {0};
        filler.kind = MOQ_EVENT_SUBSCRIBE_OK;
        while (push_event(c, &filler) == MOQ_OK) { }
    }
    /* FILL_PARAMETERS is subscriber-originated (§3.4, §3.4.1): it is not a
     * parameter of the publisher's update of its own PUBLISH, so the update
     * is a protocol violation before its nested filter is examined. Nothing
     * of it is admitted: no request ID, token or fill. */
    moq_result_t rc = moq_session_on_bidi_stream_bytes(c, ref, wire, n, false, 5);
    (void)rc;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK_EQ_U64(rs->peer_next_request_id, 3);
    MOQ_TEST_CHECK_EQ_SIZE(rs->peer_seen_count, 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->peer_token_cache.used_bytes, 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->fills.live, 0);
    if (pressure != 2) {
        moq_event_t ev;
        bool closed = take(c, MOQ_EVENT_SESSION_CLOSED, &ev);
        MOQ_TEST_CHECK(closed);
        if (closed) {
            MOQ_TEST_CHECK_EQ_U64(ev.u.closed.code, 0x3);
            moq_event_cleanup(&ev);
        }
    }
cleanup:
    moq_simpair_destroy(sp);
    fprintf(stderr, "publisher-origin PUBLISH update pressure=%d failures=%d\n",
            pressure, failures);
    return failures;
}

static int test_nested_fill(void)
{
    int failures = test_nested_decode();
    for (size_t i = 0; i < sizeof(nested_cases) / sizeof(nested_cases[0]); ++i) {
        for (int pressure = 0; pressure < 3; ++pressure)
            failures += test_nested_ingress(i, pressure, 0);
        for (int malformed = 1; malformed <= 3; ++malformed)
            failures += test_nested_ingress(i, 0, malformed);
    }
    for (int pressure = 0; pressure < 3; ++pressure)
        failures += test_nested_publisher_update(pressure);
    return failures;
}

int main(int argc, char **argv)
{
    int failures = 0;
    if (argc == 2 && strcmp(argv[1], "--nested") == 0)
        return test_nested_fill() ? 1 : 0;
    if (argc == 2 && strcmp(argv[1], "--codec") == 0)
        return test_decode_atomicity() ? 1 : 0;
    bool identity = argc == 1 || (argc == 2 && strcmp(argv[1], "--identity") == 0);
    bool tracks = argc == 1 || (argc == 2 && strcmp(argv[1], "--tracks") == 0);
    if (!identity && !tracks) return 2;
    if (identity) {
        failures += test_decode_consumers(false);
        failures += test_decode_atomicity();
        for (int pressure = 0; pressure <= 2; ++pressure) {
            failures += test_nonzero_refusal(0, pressure);
            failures += test_nonzero_refusal(1, pressure);
        }
        failures += test_nonzero_refusal(3, 0);
        failures += test_nonzero_refusal(3, 1);
    }
    if (tracks) {
        failures += test_decode_consumers(true);
        for (int pressure = 0; pressure <= 2; ++pressure)
            failures += test_nonzero_refusal(2, pressure);
    }
    if (argc != 1) return failures ? 1 : 0;
    failures += test_nested_fill();
    failures += test_range_filter_refused();
    failures += test_location_cfg(MOQ_VERSION_DRAFT_22);
    failures += test_location_cfg(MOQ_VERSION_DRAFT_18);
    if (failures) { fprintf(stderr, "FAIL: test_d22_filters (%d)\n", failures); return 1; }
    printf("PASS: test_d22_filters\n");
    return 0;
}
