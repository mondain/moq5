/*
 * FETCH ranges given as a LOCATION_FILTER on draft-22. A relative start is
 * resolved against the track's Largest Object (StartGroup groups before the
 * next group), Next Object starts just past it, and an omitted end is the
 * Largest Object itself. A start past the Largest Object, or a track with no
 * objects, is refused with INVALID_RANGE on the request stream, never a
 * session close.
 *
 * The request's form and FILL_TIMEOUT reach the publisher with the event
 * (moq_fetch_request_options_t). Under moq_session_cfg_t.defer_fetch_ranges
 * the range is surfaced unresolved for the application to resolve against a
 * Largest Object of its own, and an accept whose End Location contradicts
 * the request is refused with nothing sent. A fetcher states those forms
 * through moq_fetch_cfg_t.location and has_fill_timeout, which only a profile
 * with a Location Filter on FETCH can carry. End of Timed-Out Range is
 * written on draft-22 fetch streams and refused on draft-16 and draft-18,
 * whose fetch streams have no such marker.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <moq/vi64.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <string.h>

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

/* A pair under 22 whose server has published (5,2) on live/track. */
static moq_simpair_t *mk_pair_with_largest(void)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t scfg; moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = MOQ_BYTES_LITERAL("track");
    moq_subscription_t ch;
    if (moq_session_subscribe(c, &scfg, 1, &ch) != MOQ_OK) goto fail;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) goto fail;
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    if (moq_session_accept_subscribe(sv, sh, &ac, 2) != MOQ_OK) goto fail;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_subgroup_cfg_t sgc; moq_subgroup_cfg_init(&sgc);
    sgc.group_id = 5; sgc.subgroup_id = 0;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(sv, sh, &sgc, 3, &sg) != MOQ_OK) goto fail;
    uint8_t d[] = { 0xC0, 0xDE };
    moq_rcbuf_t *pay = NULL;
    moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &pay);
    moq_result_t wr = moq_session_write_object(sv, sg, 2, pay, 3);
    moq_rcbuf_decref(pay);
    if (wr != MOQ_OK) goto fail;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    return sp;
fail:
    moq_simpair_destroy(sp);
    return NULL;
}

/* Encode a FETCH for ns/name with the given filter (NULL: none) and, when
 * fill_timeout is not UINT64_MAX, a FILL_TIMEOUT. */
static size_t encode_fetch_ft(uint8_t *out, size_t cap, uint64_t request_id,
                              const char *name, const moq_d22_location_filter_t *f,
                              uint64_t fill_timeout);

static size_t encode_fetch(uint8_t *out, size_t cap, uint64_t request_id,
                           const char *name, const moq_d22_location_filter_t *f)
{
    return encode_fetch_ft(out, cap, request_id, name, f, UINT64_MAX);
}

static size_t encode_fetch_ft(uint8_t *out, size_t cap, uint64_t request_id,
                              const char *name, const moq_d22_location_filter_t *f,
                              uint64_t fill_timeout)
{
    uint8_t ns_buf[32];
    moq_buf_writer_t nw;
    moq_buf_writer_init(&nw, ns_buf, sizeof(ns_buf));
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { parts, 1 };
    if (moq_d22_encode_namespace(&nw, &ns) != MOQ_OK) return 0;
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_FETCH;
    m.request_id = request_id;
    m.ns.encoded.data = ns_buf;
    m.ns.encoded.len = moq_buf_writer_offset(&nw);
    m.ns.count = 1;
    m.track_name.data = (const uint8_t *)name;
    m.track_name.len = strlen(name);
    /* Parameters in ascending type order: FILL_TIMEOUT (0x0a), then
     * LOCATION_FILTER (0x21). */
    moq_d22_param_t p[2];
    memset(p, 0, sizeof(p));
    size_t np = 0;
    if (fill_timeout != UINT64_MAX) {
        p[np].type = 0x0a;
        p[np].value.integer = fill_timeout;
        np++;
    }
    if (f) {
        p[np].type = 0x21;
        p[np].value.filter = *f;
        np++;
    }
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.max_filter_ranges = 8;
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, out, cap);
    if (moq_d22_encode_message(&w, &m, np ? p : NULL, np, &ctx) != MOQ_OK) return 0;
    return moq_buf_writer_offset(&w);
}

/* Feed a FETCH to the server on a fresh request stream. */
static moq_result_t send_fetch(moq_session_t *sv, uint64_t ref, uint64_t request_id,
                               const char *name, const moq_d22_location_filter_t *f)
{
    uint8_t buf[128];
    size_t n = encode_fetch(buf, sizeof(buf), request_id, name, f);
    if (n == 0) return MOQ_ERR_INVAL;
    return moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(ref), buf, n, false, 10);
}

/* The REQUEST_ERROR code the server queued on `ref`, or UINT64_MAX. */
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
                ctx.request_type = MOQ_D22_FETCH;
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

static int expect_request(moq_session_t *sv, uint64_t ref, uint64_t rid,
                          const moq_d22_location_filter_t *f,
                          uint64_t sg, uint64_t so, uint64_t eg, uint64_t eo)
{
    int failures = 0;
    MOQ_TEST_CHECK_EQ_INT((int)send_fetch(sv, ref, rid, "track", f), (int)MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, sg);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_object, so);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, eg);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, eo);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

static int expect_invalid_range(moq_session_t *sv, uint64_t ref, uint64_t rid,
                                const char *name, const moq_d22_location_filter_t *f)
{
    int failures = 0;
    MOQ_TEST_CHECK_EQ_INT((int)send_fetch(sv, ref, rid, name, f), (int)MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(!take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(take_error_code(sv, ref), MOQ_REQUEST_ERROR_INVALID_RANGE);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

/* -- Options on the event, and the application's resolution ------------- */

/* Feed a FETCH (filter f, FILL_TIMEOUT unless UINT64_MAX) and take its
 * FETCH_REQUEST; false when none is surfaced. */
static bool request_ft(moq_session_t *sv, uint64_t ref, uint64_t rid, const char *name,
                       const moq_d22_location_filter_t *f, uint64_t fill_timeout,
                       moq_event_t *out)
{
    uint8_t buf[160];
    size_t n = encode_fetch_ft(buf, sizeof(buf), rid, name, f, fill_timeout);
    if (n == 0) return false;
    if (moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(ref), buf, n,
                                         false, 10) != MOQ_OK)
        return false;
    return take(sv, MOQ_EVENT_FETCH_REQUEST, out);
}

/* The number of actions queued, all drained. */
static int drain_actions(moq_session_t *sv)
{
    int n = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) { n++; moq_action_cleanup(&a); }
    return n;
}

static moq_result_t accept_end(moq_session_t *sv, moq_fetch_t fh, uint64_t eg, uint64_t eo)
{
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = eg;
    ac.end_object = eo;
    return moq_session_accept_fetch(sv, fh, &ac, 20);
}

/* The FETCH_REQUEST carries the request's Location Filter and FILL_TIMEOUT
 * whether or not the session resolved its range. */
static int test_options_surfaced(moq_session_t *sv)
{
    int failures = 0;
    moq_event_t ev;
    moq_d22_location_filter_t rel1 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 1 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7101, 20, "track", &rel1, 250, &ev));
    MOQ_TEST_CHECK(ev.detail_size >= offsetof(moq_fetch_request_event_t, options) +
                                         sizeof(ev.u.fetch_request.options));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 5);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 5);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 3);
    const moq_fetch_request_options_t *o = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(o != NULL);
    if (o) {
        MOQ_TEST_CHECK_EQ_U64(o->struct_size, sizeof(*o));
        MOQ_TEST_CHECK(!o->range_unresolved);
        MOQ_TEST_CHECK(o->flags & MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT);
        MOQ_TEST_CHECK_EQ_U64(o->fill_timeout_ms, 250);
        MOQ_TEST_CHECK_EQ_U64(o->location.struct_size, MOQ_LOCATION_FILTER_CFG_V0_SIZE);
        MOQ_TEST_CHECK_EQ_INT((int)o->location.kind, (int)MOQ_LOCATION_FILTER_RELATIVE_GROUP);
        MOQ_TEST_CHECK_EQ_U64(o->location.relative_group_offset, 1);
    }
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);

    /* No filter is NONE; no FILL_TIMEOUT leaves its presence flag clear. */
    MOQ_TEST_CHECK(request_ft(sv, 0x7102, 22, "track", NULL, UINT64_MAX, &ev));
    o = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(o != NULL);
    if (o) {
        MOQ_TEST_CHECK(!o->range_unresolved);
        MOQ_TEST_CHECK(!(o->flags & MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT));
        MOQ_TEST_CHECK(o->location.kind == MOQ_LOCATION_FILTER_NONE);
    }
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);

    /* A FILL_TIMEOUT of zero is present, not absent. */
    moq_d22_location_filter_t rng = { .type = MOQ_D22_FILTER_RANGE, .start_group = 1,
                                      .start_object = 0, .end_group_delta = 2, .end_object = 1 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7103, 24, "track", &rng, 0, &ev));
    o = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(o && (o->flags & MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT) && o->fill_timeout_ms == 0);
    MOQ_TEST_CHECK(o && o->location.kind == MOQ_LOCATION_FILTER_OBJECT_END &&
                   o->location.start_group == 1 && o->location.end_group == 3 &&
                   o->location.end_object == 1);
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

/* Deferred: every form is surfaced as sent, and the accept's End must agree
 * with the request. A refused accept sends nothing and leaves the fetch
 * pending, so the application can still answer it. */
static int test_deferred_resolution(moq_session_t *sv)
{
    int failures = 0;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_set_defer_fetch_ranges(NULL, true),
                          (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_set_defer_fetch_ranges(sv, true), (int)MOQ_OK);
    moq_event_t ev;

    /* Relative one group back: surfaced with nothing resolved. The End is
     * the application's Largest Object, which fixes the start too. */
    moq_d22_location_filter_t rel1 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 1 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7201, 30, "track", &rel1, UINT64_MAX, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_object, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 0);
    MOQ_TEST_CHECK(ev.u.fetch_request.options &&
                   ev.u.fetch_request.options->range_unresolved);
    moq_fetch_t f_rel1 = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    /* Resolved against a Largest Object of {9, 4}, the relay's own. */
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rel1, 9, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK(drain_actions(sv) == 2);   /* FETCH_OK + FETCH_HEADER */

    /* A track this session never published: surfaced, not refused. */
    moq_d22_location_filter_t abs0 = { .type = MOQ_D22_FILTER_ABSOLUTE };
    MOQ_TEST_CHECK(request_ft(sv, 0x7202, 32, "other", &abs0, UINT64_MAX, &ev));
    MOQ_TEST_CHECK(ev.u.fetch_request.options &&
                   ev.u.fetch_request.options->range_unresolved);
    moq_fetch_t f_other = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_U64(take_error_code(sv, 0x7202), UINT64_MAX);   /* nothing sent */
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_other, 0, 1), (int)MOQ_OK);
    (void)drain_actions(sv);

    /* Relative zero is past an ordinary Largest group; MAX clamps instead. */
    moq_d22_location_filter_t rel0 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 0 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7203, 34, "track", &rel0, UINT64_MAX, &ev));
    moq_fetch_t f_rel0 = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rel0, 5, 3), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rel0, 5, 0), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(drain_actions(sv) == 0);

    /* Next Object: past the Largest Object by definition; answered with
     * INVALID_RANGE by the application. */
    moq_d22_location_filter_t next = { .type = MOQ_D22_FILTER_NEXT_OBJECT };
    MOQ_TEST_CHECK(request_ft(sv, 0x7204, 36, "track", &next, UINT64_MAX, &ev));
    MOQ_TEST_CHECK(ev.u.fetch_request.options &&
                   ev.u.fetch_request.options->location.kind ==
                       MOQ_LOCATION_FILTER_NEXT_OBJECT);
    moq_fetch_t f_next = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_next, 5, 3), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(drain_actions(sv) == 0);
    moq_reject_fetch_cfg_t rj;
    moq_reject_fetch_cfg_init(&rj);
    rj.error_code = MOQ_REQUEST_ERROR_INVALID_RANGE;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_fetch(sv, f_next, &rj, 21), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(take_error_code(sv, 0x7204), MOQ_REQUEST_ERROR_INVALID_RANGE);

    /* Absolute start, open end: an End before the start is refused. */
    moq_d22_location_filter_t abs51 = { .type = MOQ_D22_FILTER_ABSOLUTE, .start_group = 5,
                                        .start_object = 1 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7205, 38, "track", &abs51, UINT64_MAX, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 5);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_object, 1);
    moq_fetch_t f_abs = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_abs, 5, 1), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_abs, 4, 0), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(drain_actions(sv) == 0);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_abs, 5, 2), (int)MOQ_OK);
    MOQ_TEST_CHECK(drain_actions(sv) == 2);

    /* A bounded range: the End may stop short of the request's end (the
     * Largest Object is before it) but never pass it. */
    moq_d22_location_filter_t rng = { .type = MOQ_D22_FILTER_RANGE, .start_group = 1,
                                      .start_object = 0, .end_group_delta = 2, .end_object = 1 };
    MOQ_TEST_CHECK(request_ft(sv, 0x7206, 40, "track", &rng, UINT64_MAX, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 3);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 2);
    moq_fetch_t f_rng = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rng, 3, 3), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rng, 3, 0), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rng, 0, 0), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(drain_actions(sv) == 0);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_rng, 2, 0), (int)MOQ_OK);
    (void)drain_actions(sv);

    /* Switched back: the session resolves again. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_set_defer_fetch_ranges(sv, false), (int)MOQ_OK);
    MOQ_TEST_CHECK(request_ft(sv, 0x7207, 42, "track", &rel1, UINT64_MAX, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 5);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 3);
    MOQ_TEST_CHECK(ev.u.fetch_request.options &&
                   !ev.u.fetch_request.options->range_unresolved);
    moq_fetch_t f_res = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    (void)drain_actions(sv);
    /* A resolved fetch's accept is not checked against a form. */
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, f_res, 9, 9), (int)MOQ_OK);
    (void)drain_actions(sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

/* The configuration field is read only when struct_size covers it. */
static int test_cfg_gate(void)
{
    int failures = 0;
    for (int full = 0; full <= 1; full++) {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_SERVER);
        cfg.defer_fetch_ranges = true;
        if (!full)
            cfg.struct_size = (uint32_t)offsetof(moq_session_cfg_t, defer_fetch_ranges);
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_create(&cfg, 0, &s), (int)MOQ_OK);
        if (!s) continue;
        MOQ_TEST_CHECK(s->defer_fetch_ranges == (full != 0));
        moq_session_destroy(s);
    }
    return failures;
}

/* A started pair under `version`. */
static moq_simpair_t *mk_pair(moq_version_t version)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(moq_simpair_client(sp));
    drain(moq_simpair_server(sp));
    return sp;
}

static int next_object_row(bool deferred, bool has_largest,
                           uint64_t group, uint64_t object)
{
    int failures = 0;
    test_alloc_state_t alloc_state = {0};
    moq_alloc_t alloc = test_allocator(&alloc_state);
    moq_simpair_cfg_t pair_cfg = MOQ_SIMPAIR_CFG_INIT;
    pair_cfg.alloc = &alloc;
    pair_cfg.version = MOQ_VERSION_DRAFT_22;
    pair_cfg.client_send_request_capacity = true;
    pair_cfg.client_initial_request_capacity = 32;
    pair_cfg.server_send_request_capacity = true;
    pair_cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    moq_session_t *client = NULL, *server = NULL;
    moq_event_t ev;
    bool event_owned = false, started = false;
#define NEXT_NEED(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: next_object_%s history=%u largest={%llu,%llu}: %s\n", \
                deferred ? "deferred" : "automatic", (unsigned)has_largest, \
                (unsigned long long)group, (unsigned long long)object, #expr); \
        failures++; \
        goto done; \
    } \
} while (0)
    NEXT_NEED(moq_simpair_create(&pair_cfg, &sp) == MOQ_OK);
    client = moq_simpair_client(sp);
    server = moq_simpair_server(sp);
    NEXT_NEED(moq_simpair_start(sp) == MOQ_OK);
    NEXT_NEED(moq_simpair_run_until_quiescent(sp, 16, NULL) == MOQ_OK);
    NEXT_NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    NEXT_NEED(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    started = true;
    drain(client);
    drain(server);
    NEXT_NEED(moq_session_set_defer_fetch_ranges(server, deferred) == MOQ_OK);
    moq_bytes_t part = MOQ_BYTES_LITERAL("next-object-boundary");
    moq_namespace_t ns = { &part, 1 };
    moq_bytes_t name = MOQ_BYTES_LITERAL("v");
    if (has_largest)
        NEXT_NEED(moq_session_note_object_published(server, &ns, name,
                                                    group, object) == MOQ_OK);
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = ns;
    cfg.track_name = name;
    cfg.location = &location;
    moq_fetch_t client_fetch;
    NEXT_NEED(moq_session_fetch(client, &cfg, moq_simpair_now_us(sp),
                                &client_fetch) == MOQ_OK);
    NEXT_NEED(moq_simpair_run_until_quiescent(sp, 16, NULL) == MOQ_OK);
    event_owned = moq_session_poll_events(server, &ev, 1) == 1;
    NEXT_NEED(event_owned == deferred);
    if (deferred) {
        NEXT_NEED(ev.kind == MOQ_EVENT_FETCH_REQUEST);
        const moq_fetch_request_options_t *options = ev.u.fetch_request.options;
        NEXT_NEED(options && options->range_unresolved &&
                  options->location.kind == MOQ_LOCATION_FILTER_NEXT_OBJECT);
        moq_fetch_t server_fetch = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
        event_owned = false;
        moq_accept_fetch_cfg_t answer;
        moq_accept_fetch_cfg_init(&answer);
        answer.end_group = group;
        answer.end_object = object == UINT64_MAX ? 0 : object + 1;
        NEXT_NEED(moq_session_accept_fetch(server, server_fetch, &answer,
                    moq_simpair_now_us(sp)) == MOQ_ERR_INVAL);
        NEXT_NEED(drain_actions(server) == 0);
        event_owned = moq_session_poll_events(client, &ev, 1) == 1;
        NEXT_NEED(!event_owned);
        moq_reject_fetch_cfg_t reject;
        moq_reject_fetch_cfg_init(&reject);
        reject.error_code = MOQ_REQUEST_ERROR_INVALID_RANGE;
        NEXT_NEED(moq_session_reject_fetch(server, server_fetch, &reject,
                                          moq_simpair_now_us(sp)) == MOQ_OK);
        NEXT_NEED(moq_simpair_run_until_quiescent(sp, 16, NULL) == MOQ_OK);
    }
    event_owned = moq_session_poll_events(client, &ev, 1) == 1;
    NEXT_NEED(event_owned);
    NEXT_NEED(ev.kind == MOQ_EVENT_FETCH_ERROR);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_error.fetch._opaque, client_fetch._opaque);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_error.error_code, MOQ_REQUEST_ERROR_INVALID_RANGE);
    moq_event_cleanup(&ev);
    event_owned = false;
    event_owned = moq_session_poll_events(client, &ev, 1) == 1;
    NEXT_NEED(!event_owned);
    event_owned = moq_session_poll_events(server, &ev, 1) == 1;
    NEXT_NEED(!event_owned);
done:
    if (event_owned) moq_event_cleanup(&ev);
    if (started) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(client), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(server), (int)MOQ_SESS_ESTABLISHED);
    }
    if (sp) moq_simpair_destroy(sp);
    MOQ_TEST_CHECK_EQ_INT(alloc_state.balance, 0);
    printf("%s: next_object_%s history=%u largest={%llu,%llu}\n",
           failures ? "FAIL" : "PASS", deferred ? "deferred" : "automatic",
           (unsigned)has_largest, (unsigned long long)group,
           (unsigned long long)object);
#undef NEXT_NEED
    return failures;
}

static int test_next_object_boundaries(void)
{
    int failures = 0;
    const uint64_t groups[] = {0, 5, UINT64_MAX};
    const uint64_t objects[] = {0, 2, UINT64_MAX - 1, UINT64_MAX};
    for (unsigned mode = 0; mode < 2; mode++) {
        failures += next_object_row(mode != 0, false, 0, 0);
        for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
            for (size_t o = 0; o < sizeof(objects) / sizeof(objects[0]); o++)
                failures += next_object_row(mode != 0, true, groups[g], objects[o]);
    }
    return failures;
}

/* Exercise the deferred application's End through the public request,
 * response, object and completion path, without seeding session history. */
static int deferred_relative_row(const char *name, uint64_t offset,
                                  uint64_t end_group, uint64_t end_object,
                                  uint64_t start_group, uint64_t last_object,
                                  moq_result_t expected)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_rcbuf_t *payload = NULL;
    moq_event_t ev;
    bool event_owned = false;
#define DEFERRED_NEED_OK(expr) do { \
    moq_result_t result_ = (expr); \
    MOQ_TEST_CHECK_EQ_INT((int)result_, (int)MOQ_OK); \
    if (result_ != MOQ_OK) goto done; \
} while (0)
#define DEFERRED_TAKE(session, kind) do { \
    event_owned = take((session), (kind), &ev); \
    MOQ_TEST_CHECK(event_owned); \
    if (!event_owned) goto done; \
} while (0)
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    DEFERRED_NEED_OK(moq_session_set_defer_fetch_ranges(sv, true));
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    location.relative_group_offset = offset;
    moq_bytes_t part = MOQ_BYTES_LITERAL("live");
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("deferred-boundary");
    cfg.group_order = MOQ_GROUP_ORDER_ASCENDING;
    cfg.location = &location;
    moq_fetch_t client_fetch;
    DEFERRED_NEED_OK(moq_session_fetch(c, &cfg, moq_simpair_now_us(sp), &client_fetch));
    DEFERRED_NEED_OK(moq_simpair_run_until_quiescent(sp, 16, NULL));
    DEFERRED_TAKE(sv, MOQ_EVENT_FETCH_REQUEST);
    const moq_fetch_request_options_t *options = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(options && options->range_unresolved &&
                   options->location.kind == MOQ_LOCATION_FILTER_RELATIVE_GROUP &&
                   options->location.relative_group_offset == offset);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_object, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 0);
    moq_fetch_t server_fetch = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    event_owned = false;

    moq_accept_fetch_cfg_t answer;
    moq_accept_fetch_cfg_init(&answer);
    answer.end_group = end_group;
    answer.end_object = end_object;
    moq_result_t rc = moq_session_accept_fetch(sv, server_fetch, &answer,
                                               moq_simpair_now_us(sp));
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)expected);
    if (rc != expected) goto done;
    if (expected != MOQ_OK) {
        MOQ_TEST_CHECK_EQ_INT(drain_actions(sv), 0);
        moq_reject_fetch_cfg_t reject;
        moq_reject_fetch_cfg_init(&reject);
        reject.error_code = MOQ_REQUEST_ERROR_INVALID_RANGE;
        DEFERRED_NEED_OK(moq_session_reject_fetch(sv, server_fetch, &reject,
                                                  moq_simpair_now_us(sp)));
        DEFERRED_NEED_OK(moq_simpair_run_until_quiescent(sp, 16, NULL));
        DEFERRED_TAKE(c, MOQ_EVENT_FETCH_ERROR);
        MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_error.fetch._opaque, client_fetch._opaque);
        MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_error.error_code, MOQ_REQUEST_ERROR_INVALID_RANGE);
        moq_event_cleanup(&ev);
        event_owned = false;
        goto done;
    }

    DEFERRED_NEED_OK(moq_simpair_run_until_quiescent(sp, 16, NULL));
    DEFERRED_TAKE(c, MOQ_EVENT_FETCH_OK);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.fetch._opaque, client_fetch._opaque);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_group, end_group);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_object, end_object);
    MOQ_TEST_CHECK(!ev.u.fetch_ok.end_of_track);
    moq_event_cleanup(&ev);
    event_owned = false;

    const uint8_t bytes[] = { 0x37, 0x66 };
    DEFERRED_NEED_OK(moq_rcbuf_create(moq_alloc_default(), bytes, sizeof(bytes), &payload));
    unsigned count = start_group == end_group && last_object == 0 ? 1 : 2;
    for (unsigned i = 0; i < count; i++) {
        uint64_t group = i == 0 ? start_group : end_group;
        uint64_t object = i == 0 ? 0 : last_object;
        moq_fetch_object_cfg_t object_cfg;
        moq_fetch_object_cfg_init(&object_cfg);
        object_cfg.group_id = group;
        object_cfg.object_id = object;
        object_cfg.payload = payload;
        DEFERRED_NEED_OK(moq_session_write_fetch_object(sv, server_fetch, &object_cfg,
                                                        moq_simpair_now_us(sp)));
        DEFERRED_NEED_OK(moq_simpair_run_until_quiescent(sp, 16, NULL));
        DEFERRED_TAKE(c, MOQ_EVENT_FETCH_OBJECT);
        MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_object.fetch._opaque, client_fetch._opaque);
        MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_object.group_id, group);
        MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_object.object_id, object);
        MOQ_TEST_CHECK(ev.u.fetch_object.payload != NULL);
        if (ev.u.fetch_object.payload) {
            size_t len = moq_rcbuf_len(ev.u.fetch_object.payload);
            MOQ_TEST_CHECK_EQ_SIZE(len, sizeof(bytes));
            if (len == sizeof(bytes))
                MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(ev.u.fetch_object.payload),
                                      bytes, sizeof(bytes)) == 0);
        }
        moq_event_cleanup(&ev);
        event_owned = false;
    }
    DEFERRED_NEED_OK(moq_session_end_fetch(sv, server_fetch, moq_simpair_now_us(sp)));
    DEFERRED_NEED_OK(moq_simpair_run_until_quiescent(sp, 16, NULL));
    DEFERRED_TAKE(c, MOQ_EVENT_FETCH_COMPLETE);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_complete.fetch._opaque, client_fetch._opaque);
done:
    if (event_owned) moq_event_cleanup(&ev);
    if (payload) moq_rcbuf_decref(payload);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    printf("%s: deferred_relative_%s\n", failures ? "FAIL" : "PASS", name);
#undef DEFERRED_TAKE
#undef DEFERRED_NEED_OK
    return failures;
}

static int test_deferred_relative_boundaries(void)
{
    int failures = 0;
    const struct {
        const char *name;
        uint64_t offset, end_group, end_object, start_group, last_object;
        moq_result_t expected;
    } rows[] = {
        { "zero_max_first", 0, UINT64_MAX, 1, UINT64_MAX, 0, MOQ_OK },
        { "zero_max_ordinary", 0, UINT64_MAX, 3, UINT64_MAX, 2, MOQ_OK },
        { "zero_max_whole", 0, UINT64_MAX, 0, UINT64_MAX, UINT64_MAX, MOQ_OK },
        { "zero_ordinary", 0, 5, 3, 0, 2, MOQ_ERR_INVAL },
        { "zero_ordinary_whole", 0, 5, 0, 0, UINT64_MAX, MOQ_ERR_INVAL },
        { "zero_before_max", 0, UINT64_MAX - 1, 0, 0, UINT64_MAX, MOQ_ERR_INVAL },
        { "one_max", 1, UINT64_MAX, 0, UINT64_MAX, UINT64_MAX, MOQ_OK },
        { "two_max", 2, UINT64_MAX, 0, UINT64_MAX - 1, UINT64_MAX, MOQ_OK },
        { "two_origin", 2, 0, 1, 0, 0, MOQ_OK },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        failures += deferred_relative_row(rows[i].name, rows[i].offset,
            rows[i].end_group, rows[i].end_object, rows[i].start_group,
            rows[i].last_object, rows[i].expected);
    return failures;
}

/* An accepted fetch of live/track {5,0}..{5,2} on the pair's server. */
static bool accepted_fetch(moq_simpair_t *sp, moq_fetch_t *sv_fetch)
{
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    fc.track_namespace = (moq_namespace_t){ parts, 1 };
    fc.track_name = MOQ_BYTES_LITERAL("track");
    fc.start_group = 5;
    fc.end_group = 5;
    fc.end_object = 3;
    moq_fetch_t ch;
    if (moq_session_fetch(c, &fc, 30, &ch) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_FETCH_REQUEST, &ev)) return false;
    *sv_fetch = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 5;
    ac.end_object = 3;
    if (moq_session_accept_fetch(sv, *sv_fetch, &ac, 31) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c);
    return true;
}

/* Draft-22 fetch streams carry End of Timed-Out Range from the fetch writers
 * as from the fill writers; draft-16 and draft-18 have no such marker, and
 * refuse it with nothing queued and the prior Location unchanged. */
static int test_timed_out_writers(void)
{
    int failures = 0;
    {
        moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
        MOQ_TEST_CHECK(sp != NULL);
        if (!sp) return failures;
        moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
        moq_fetch_t fh;
        MOQ_TEST_CHECK(accepted_fetch(sp, &fh));
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range(sv, fh,
            MOQ_FETCH_RANGE_TIMED_OUT, 5, 1, 32), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range_before_group(sv, fh,
            MOQ_FETCH_RANGE_TIMED_OUT, 6, 32), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_end_fetch(sv, fh, 32), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        int gaps = 0;
        moq_event_t ev;
        while (moq_session_poll_events(c, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_FETCH_GAP) {
                MOQ_TEST_CHECK_EQ_INT((int)ev.u.fetch_gap.range_kind,
                                      (int)MOQ_FETCH_RANGE_TIMED_OUT);
                MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_gap.group_id, 5);
                MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_gap.object_id,
                                      gaps == 0 ? 1 : MOQ_VI64_MAX);
                gaps++;
            }
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK_EQ_INT(gaps, 2);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
        moq_simpair_destroy(sp);
    }
    static const moq_version_t older[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18 };
    for (size_t v = 0; v < 2; v++) {
        moq_simpair_t *sp = mk_pair(older[v]);
        MOQ_TEST_CHECK(sp != NULL);
        if (!sp) continue;
        moq_session_t *sv = moq_simpair_server(sp);
        moq_fetch_t fh;
        moq_session_t *c = moq_simpair_client(sp);
        /* A track the server has published something on is not needed: an
         * explicit range is surfaced as sent on these drafts. */
        moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
        moq_fetch_cfg_t fc;
        moq_fetch_cfg_init_sized(&fc, sizeof(fc));
        fc.track_namespace = (moq_namespace_t){ parts, 1 };
        fc.track_name = MOQ_BYTES_LITERAL("track");
        fc.start_group = 5;
        fc.end_group = 5;
        fc.end_object = 3;
        moq_fetch_t ch;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 30, &ch), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        moq_event_t ev;
        MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
        fh = ev.u.fetch_request.fetch;
        /* No Location Filter on these drafts' FETCH: no options. */
        MOQ_TEST_CHECK(ev.u.fetch_request.options == NULL);
        moq_event_cleanup(&ev);
        moq_accept_fetch_cfg_t ac;
        moq_accept_fetch_cfg_init(&ac);
        ac.end_group = 5;
        ac.end_object = 3;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_fetch(sv, fh, &ac, 31), (int)MOQ_OK);
        (void)drain_actions(sv);
        moq_fetch_entry_t *fe = NULL;
        for (size_t i = 0; i < sv->fetch_cap; i++)
            if (sv->fetches[i].state == MOQ_FETCH_ACCEPTED) { fe = &sv->fetches[i]; break; }
        MOQ_TEST_CHECK(fe != NULL);
        if (fe) {
            moq_fetch_prior_object_t snap = fe->prior;
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range(sv, fh,
                MOQ_FETCH_RANGE_TIMED_OUT, 5, 1, 32), (int)MOQ_ERR_INVAL);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range_before_group(sv, fh,
                MOQ_FETCH_RANGE_TIMED_OUT, 6, 32), (int)MOQ_ERR_INVAL);
            MOQ_TEST_CHECK(memcmp(&fe->prior, &snap, sizeof(snap)) == 0);
            MOQ_TEST_CHECK(drain_actions(sv) == 0);
            /* The kind is refused for what it is, whatever the queue: with
             * no room left it is still INVAL, never WOULD_BLOCK. */
            moq_result_t wr = MOQ_OK;
            for (uint64_t o = 0; o < 4096 && wr == MOQ_OK; o++)
                wr = moq_session_write_fetch_range(sv, fh, MOQ_FETCH_RANGE_UNKNOWN,
                                                   5, o, 32);
            MOQ_TEST_CHECK_EQ_INT((int)wr, (int)MOQ_ERR_WOULD_BLOCK);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range(sv, fh,
                MOQ_FETCH_RANGE_TIMED_OUT, 5, 9000, 32), (int)MOQ_ERR_INVAL);
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_range_before_group(sv, fh,
                MOQ_FETCH_RANGE_TIMED_OUT, 6, 32), (int)MOQ_ERR_INVAL);
        }
        moq_simpair_destroy(sp);
    }
    return failures;
}

/* A fetcher states a range relative to the publisher's Largest Object, or
 * none, and a FILL_TIMEOUT; the publisher sees exactly that. FETCH_OK's End
 * is not checked against a start the fetcher cannot know. The older drafts'
 * FETCH has neither field: the request is refused before anything is sent. */
static int test_semantic_fetch(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair_with_largest();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    loc.relative_group_offset = 2;
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    fc.track_namespace = (moq_namespace_t){ parts, 1 };
    fc.track_name = MOQ_BYTES_LITERAL("track");
    fc.location = &loc;
    fc.has_fill_timeout = true;
    fc.fill_timeout_ms = 0;
    moq_fetch_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 40, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 4);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 5);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_object, 3);
    const moq_fetch_request_options_t *o = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(o && (o->flags & MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT) && o->fill_timeout_ms == 0);
    MOQ_TEST_CHECK(o && o->location.kind == MOQ_LOCATION_FILTER_RELATIVE_GROUP &&
                   o->location.relative_group_offset == 2);
    moq_fetch_t sfh = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)accept_end(sv, sfh, 5, 3), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_OK, &ev));
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);

    /* NONE: no filter on the wire, the whole track through the Largest. */
    loc.kind = MOQ_LOCATION_FILTER_NONE;
    fc.has_fill_timeout = false;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 41, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.start_group, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_request.end_group, 5);
    o = ev.u.fetch_request.options;
    MOQ_TEST_CHECK(o && o->location.kind == MOQ_LOCATION_FILTER_NONE &&
                   !(o->flags & MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT));
    moq_event_cleanup(&ev);

    /* Malformed forms are refused before anything is sent. */
    (void)drain_actions(c);
    moq_location_filter_cfg_t bad = loc;
    bad.kind = MOQ_LOCATION_FILTER_GROUP_END;
    bad.start_group = 4;
    bad.end_group = 3;
    fc.location = &bad;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 42, &ch), (int)MOQ_ERR_INVAL);
    bad.kind = MOQ_LOCATION_FILTER_NONE;
    bad.struct_size = MOQ_LOCATION_FILTER_CFG_V0_SIZE - 8;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 42, &ch), (int)MOQ_ERR_ABI_MISMATCH);
    MOQ_TEST_CHECK(drain_actions(c) == 0);
    moq_simpair_destroy(sp);

    static const moq_version_t older[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18 };
    for (size_t v = 0; v < 2; v++) {
        sp = mk_pair(older[v]);
        MOQ_TEST_CHECK(sp != NULL);
        if (!sp) continue;
        c = moq_simpair_client(sp);
        moq_location_filter_cfg_init(&loc);
        moq_fetch_cfg_init_sized(&fc, sizeof(fc));
        fc.track_namespace = (moq_namespace_t){ parts, 1 };
        fc.track_name = MOQ_BYTES_LITERAL("track");
        fc.end_group = 1;
        fc.location = &loc;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 43, &ch),
                              (int)MOQ_ERR_UNSUPPORTED);
        fc.location = NULL;
        fc.has_fill_timeout = true;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 43, &ch),
                              (int)MOQ_ERR_UNSUPPORTED);
        MOQ_TEST_CHECK(drain_actions(c) == 0);
        /* A fetch cfg sized before `location` never reads it. */
        fc.location = &loc;
        fc.has_fill_timeout = false;
        fc.struct_size = (uint32_t)offsetof(moq_fetch_cfg_t, location);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &fc, 43, &ch), (int)MOQ_OK);
        moq_simpair_destroy(sp);
    }
    return failures;
}

/* A FETCH arriving with the fetch pool full is rejected on its own request
 * stream: REQUEST_ERROR EXCESSIVE_LOAD with a non-zero Retry Interval and a
 * FIN (§6.4.2.3: "When an endpoint rejects a request without performing any
 * application processing, it SHOULD send a REQUEST_ERROR and FIN the
 * stream"; §12.3: EXCESSIVE_LOAD "The responder is overloaded and cannot
 * process the request at this time"). The session stays up, the requester's
 * later FIN on that stream is absorbed, and a FETCH that arrives once a slot
 * frees is admitted. */
static int test_fetch_pool_full(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair_with_largest();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    /* The pair's SUBSCRIBE used the client's Request ID 0. */
    size_t cap = sv->fetch_cap;
    moq_fetch_t first = MOQ_FETCH_INVALID;
    for (size_t i = 0; i < cap; i++) {
        MOQ_TEST_CHECK_EQ_INT((int)send_fetch(sv, 0x7100 + i, 2 * (i + 1), "track", NULL),
                              (int)MOQ_OK);
        moq_event_t ev;
        MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
        if (i == 0) first = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1) > 0) moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(fetch_find_free(sv) < 0);

    const uint64_t full_ref = 0x7100 + cap;
    MOQ_TEST_CHECK_EQ_INT((int)send_fetch(sv, full_ref, 2 * (cap + 1), "track", NULL),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_event_t ev;
    MOQ_TEST_CHECK(!take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    bool answered = false;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == full_ref) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.request_type = MOQ_D22_FETCH;
            ctx.max_filter_ranges = 8;
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                env.msg_type == MOQ_D22_REQUEST_ERROR &&
                moq_d22_decode_message(MOQ_D22_REQUEST_ERROR, env.payload,
                                       env.payload_len, &ctx, &m) == MOQ_OK) {
                MOQ_TEST_CHECK_EQ_U64(m.code, MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
                MOQ_TEST_CHECK(m.retry_interval != 0);
                MOQ_TEST_CHECK(a.u.send_bidi_stream.fin);
                answered = true;
            }
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(answered);

    /* The requester's FIN on the refused stream does not close the session. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(sv,
        moq_stream_ref_from_u64(full_ref), NULL, 0, true, 11), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);

    /* Freeing one slot admits the retried FETCH. */
    moq_reject_fetch_cfg_t rj;
    moq_reject_fetch_cfg_init(&rj);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_fetch(sv, first, &rj, 12), (int)MOQ_OK);
    while (moq_session_poll_actions(sv, &a, 1) > 0) moq_action_cleanup(&a);
    MOQ_TEST_CHECK_EQ_INT((int)send_fetch(sv, full_ref + 1, 2 * (cap + 2), "track", NULL),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_fetch_pool_full();
    moq_simpair_t *sp = mk_pair_with_largest();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return 1;
    moq_session_t *sv = moq_simpair_server(sp);
    /* The server-side record pins Largest Object (5,2). */
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { parts, 1 };
    moq_track_hist_t *h = track_hist_find_id(sv, &ns, MOQ_BYTES_LITERAL("track"));
    MOQ_TEST_CHECK(h && h->has_largest && h->largest_group == 5 && h->largest_object == 2);

    /* Relative start one group back from the next group: the current group,
     * through the Largest Object (exclusive end object 3). */
    moq_d22_location_filter_t rel1 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 1 };
    failures += expect_request(sv, 0x7001, 2, &rel1, 5, 0, 5, 3);
    /* Two groups back. */
    moq_d22_location_filter_t rel2 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 2 };
    failures += expect_request(sv, 0x7002, 4, &rel2, 4, 0, 5, 3);
    /* Absolute start with no end: the end is the Largest Object. */
    moq_d22_location_filter_t abs = { .type = MOQ_D22_FILTER_ABSOLUTE, .start_group = 5, .start_object = 1 };
    failures += expect_request(sv, 0x7003, 6, &abs, 5, 1, 5, 3);
    /* No filter: everything through the Largest Object. */
    failures += expect_request(sv, 0x7004, 8, NULL, 0, 0, 5, 3);
    /* An explicit range stays as given (inclusive end object 1 -> exclusive 2). */
    moq_d22_location_filter_t rng = { .type = MOQ_D22_FILTER_RANGE, .start_group = 1,
                                      .start_object = 0, .end_group_delta = 2, .end_object = 1 };
    failures += expect_request(sv, 0x7005, 10, &rng, 1, 0, 3, 2);

    /* Relative zero is the next group: past the Largest Object. */
    moq_d22_location_filter_t rel0 = { .type = MOQ_D22_FILTER_RELATIVE, .start_group = 0 };
    failures += expect_invalid_range(sv, 0x7006, 12, "track", &rel0);
    /* Next Object is past the Largest Object by definition. */
    moq_d22_location_filter_t next = { .type = MOQ_D22_FILTER_NEXT_OBJECT };
    failures += expect_invalid_range(sv, 0x7007, 14, "track", &next);
    /* An absolute start beyond the Largest Object. */
    moq_d22_location_filter_t beyond = { .type = MOQ_D22_FILTER_ABSOLUTE, .start_group = 5, .start_object = 3 };
    failures += expect_invalid_range(sv, 0x7008, 16, "track", &beyond);
    /* A track with no objects. */
    moq_d22_location_filter_t zero = { .type = MOQ_D22_FILTER_ABSOLUTE };
    failures += expect_invalid_range(sv, 0x7009, 18, "other", &zero);

    failures += test_options_surfaced(sv);
    failures += test_deferred_resolution(sv);
    moq_simpair_destroy(sp);
    failures += test_cfg_gate();
    failures += test_deferred_relative_boundaries();
    failures += test_next_object_boundaries();
    failures += test_timed_out_writers();
    failures += test_semantic_fetch();
    if (failures) { fprintf(stderr, "FAIL: test_d22_fetch_filter (%d)\n", failures); return 1; }
    printf("PASS: test_d22_fetch_filter\n");
    return 0;
}
