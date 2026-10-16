/*
 * The publisher facade's answers: TRACK_STATUS, requests nothing else takes,
 * Track Properties and DYNAMIC_GROUPS / NEW_GROUP_REQUEST.
 *
 * TRACK_STATUS (draft-22 §9.13; draft-18 §10.14 and draft-16 §9.19 alike):
 * "The receiver of a TRACK_STATUS message treats it identically as if it had
 * received a SUBSCRIBE message ... If successful, the publisher responds with
 * a TRACK_STATUS_OK with the same parameters and Track Properties it would
 * have set in a SUBSCRIBE_OK." Unsupported requests (draft-22 §1.6; drafts 16
 * and 18 §4): "Limited endpoints SHOULD respond to any unsupported messages
 * with the appropriate NOT_SUPPORTED error code, rather than ignoring them."
 * NEW_GROUP_REQUEST (draft-22 §9.20.19): an Original Publisher that supports
 * dynamic groups "SHOULD end the current Group and begin a new Group" for a
 * value of 0 or one larger than the current group; one that does not
 * "ignores the parameter".
 *
 * Pinned, on drafts 16, 18 and 22 (SUBSCRIBE_TRACKS from 18):
 *   - tick answers a TRACK_STATUS for its track with the track's Largest
 *     Object and Track Properties, refuses one for a track it lacks with
 *     DOES_NOT_EXIST, and applies the subscribe authorization;
 *   - manual dispatch leaves a request for a track it lacks IGNORED;
 *   - tick refuses a SUBSCRIBE for a track it lacks (DOES_NOT_EXIST) and the
 *     request kinds it does not serve (NOT_SUPPORTED); no request is left
 *     unanswered;
 *   - an answer the session cannot take yet is held and sent by a later tick,
 *     and becomes DOES_NOT_EXIST when its track is removed first;
 *   - an acceptance the session can never encode (Track Properties larger
 *     than its send buffer, on drafts 18 and 22) is answered with an
 *     INTERNAL_ERROR refusal, sent at once or after being held, never taken
 *     as answered;
 *   - SUBSCRIBE_OK, TRACK_STATUS_OK, the retained-group fetch answer and
 *     PUBLISH carry the track's Track Properties; dynamic_groups adds
 *     DYNAMIC_GROUPS = 1 in the session's encoding, and the subscriber sees
 *     the track's dynamic-group support;
 *   - NEW_GROUP_REQUEST on a SUBSCRIBE and on an update reaches
 *     on_new_group_request when it warrants a new group, and never for a
 *     track without DYNAMIC_GROUPS;
 *   - add_track refuses Track Properties the session would refuse.
 */
#include <moq/moq.h>
#include <moq/publisher.h>
#include <moq/sim.h>
#include "test_support.h"
#include <stdlib.h>
#include <string.h>

static int failures;

static const moq_version_t k_versions[] = {
    MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22
};

/* One application-range property: absolute type 0x78 (a two-byte integer on
 * draft-16) with value 1. Drafts 18 and 22 encode the integer as vi64. */
static const uint8_t k_props_d16[] = { 0x40, 0x78, 0x01 };
static const uint8_t k_props_d18[] = { 0x78, 0x01 };
/* The same set with DYNAMIC_GROUPS = 1 added before it: delta 0x30, value 1,
 * then 0x78 - 0x30 = 0x48 (a two-byte integer on draft-16). */
static const uint8_t k_dyn_d16[] = { 0x30, 0x01, 0x40, 0x48, 0x01 };
static const uint8_t k_dyn_d18[] = { 0x30, 0x01, 0x48, 0x01 };

static moq_bytes_t k_ns_parts[] = { { (const uint8_t *)"live", 4 } };
static moq_bytes_t k_other_parts[] = { { (const uint8_t *)"elsewhere", 9 } };

typedef struct new_group_log {
    int calls;
    uint64_t requested;
    bool has_current;
    uint64_t current;
} new_group_log_t;

static void on_new_group(void *ctx, moq_pub_track_t *track,
                         const moq_pub_new_group_request_info_t *info)
{
    (void)track;
    new_group_log_t *l = ctx;
    l->calls++;
    l->requested = info->requested_group;
    l->has_current = info->has_current_group;
    l->current = info->current_group;
}

typedef struct rig {
    moq_simpair_t *sp;
    moq_session_t *cl, *sv;
    moq_publisher_t *pub;
    moq_pub_track_t *track;
    moq_version_t v;
    new_group_log_t ng;
} rig_t;

static moq_pub_accept_decision_t accept_every(void *ctx, const moq_pub_subscribe_info_t *info,
                                              moq_request_error_t *code)
{
    (void)ctx; (void)info; (void)code;
    return MOQ_PUB_DECISION_ACCEPT;
}

static bool rig_open(rig_t *r, moq_version_t v, moq_pub_accept_mode_t mode,
                     uint32_t max_actions, bool with_props, bool dynamic)
{
    memset(r, 0, sizeof(*r));
    r->v = v;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = v;
    cfg.max_actions = max_actions;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    if (moq_simpair_create(&cfg, &r->sp) != MOQ_OK) return false;
    if (moq_simpair_start(r->sp) != MOQ_OK ||
        moq_simpair_run_until_quiescent(r->sp, 32, NULL) != MOQ_OK) return false;
    r->cl = moq_simpair_client(r->sp);
    r->sv = moq_simpair_server(r->sp);
    moq_event_t e;
    while (moq_session_poll_events(r->cl, &e, 1)) moq_event_cleanup(&e);
    while (moq_session_poll_events(r->sv, &e, 1)) moq_event_cleanup(&e);
    moq_pub_cfg_t pc;
    moq_pub_cfg_init_sized(&pc, sizeof(pc));
    pc.accept_mode = mode;
    if (mode == MOQ_PUB_CALLBACK) pc.on_subscribe = accept_every;
    pc.on_new_group_request = on_new_group;
    pc.on_new_group_request_ctx = &r->ng;
    if (moq_pub_create(r->sv, moq_alloc_default(), &pc, &r->pub) != MOQ_OK) return false;
    moq_pub_track_cfg_t tc;
    moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
    tc.track_namespace = (moq_namespace_t){ k_ns_parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("v");
    if (with_props)
        tc.track_properties = v == MOQ_VERSION_DRAFT_16
            ? (moq_bytes_t){ k_props_d16, sizeof(k_props_d16) }
            : (moq_bytes_t){ k_props_d18, sizeof(k_props_d18) };
    tc.dynamic_groups = dynamic;
    return moq_pub_add_track(r->pub, &tc, moq_simpair_now_us(r->sp), &r->track) == MOQ_OK;
}

static void rig_close(rig_t *r)
{
    if (r->pub) moq_pub_destroy(r->pub);
    if (r->sp) moq_simpair_destroy(r->sp);
}

/* Tick the publisher and move the wire until both settle. */
static void rig_pump(rig_t *r)
{
    for (int i = 0; i < 8; i++) {
        (void)moq_pub_tick(r->pub, moq_simpair_now_us(r->sp));
        (void)moq_simpair_run_until_quiescent(r->sp, 32, NULL);
    }
}

static const uint8_t *expected_props(const rig_t *r, bool dynamic, size_t *len)
{
    bool d16 = r->v == MOQ_VERSION_DRAFT_16;
    if (dynamic) {
        *len = d16 ? sizeof(k_dyn_d16) : sizeof(k_dyn_d18);
        return d16 ? k_dyn_d16 : k_dyn_d18;
    }
    *len = d16 ? sizeof(k_props_d16) : sizeof(k_props_d18);
    return d16 ? k_props_d16 : k_props_d18;
}

typedef struct answers {
    int ts_ok, ts_err, sub_ok, sub_err, fetch_ok, ns_rej, pub_err, nss_err, st_err;
    uint64_t ts_code, sub_code, ns_code, pub_code, nss_code, st_code;
    bool ts_largest, sub_dynamic;
    uint64_t ts_group, ts_object;
    uint8_t ts_props[16], sub_props[16], fetch_props[16];
    size_t ts_len, sub_len, fetch_len;
} answers_t;

static void collect(moq_session_t *cl, answers_t *a)
{
    moq_event_t e;
    while (moq_session_poll_events(cl, &e, 1)) {
        switch (e.kind) {
        case MOQ_EVENT_TRACK_STATUS_OK:
            a->ts_ok++;
            a->ts_largest = e.u.track_status_ok.has_largest;
            a->ts_group = e.u.track_status_ok.largest_group;
            a->ts_object = e.u.track_status_ok.largest_object;
            a->ts_len = e.u.track_status_ok.track_properties.len;
            if (a->ts_len && a->ts_len <= sizeof(a->ts_props))
                memcpy(a->ts_props, e.u.track_status_ok.track_properties.data, a->ts_len);
            break;
        case MOQ_EVENT_TRACK_STATUS_ERROR:
            a->ts_err++; a->ts_code = e.u.track_status_error.error_code; break;
        case MOQ_EVENT_SUBSCRIBE_OK:
            a->sub_ok++;
            a->sub_dynamic = e.u.subscribe_ok.dynamic_groups;
            a->sub_len = e.u.subscribe_ok.track_properties.len;
            if (a->sub_len && a->sub_len <= sizeof(a->sub_props))
                memcpy(a->sub_props, e.u.subscribe_ok.track_properties.data, a->sub_len);
            break;
        case MOQ_EVENT_SUBSCRIBE_ERROR:
            a->sub_err++; a->sub_code = e.u.subscribe_error.error_code; break;
        case MOQ_EVENT_FETCH_OK:
            a->fetch_ok++;
            a->fetch_len = e.u.fetch_ok.track_properties.len;
            if (a->fetch_len && a->fetch_len <= sizeof(a->fetch_props))
                memcpy(a->fetch_props, e.u.fetch_ok.track_properties.data, a->fetch_len);
            break;
        case MOQ_EVENT_NAMESPACE_REJECTED:
            a->ns_rej++; a->ns_code = e.u.namespace_rejected.error_code; break;
        case MOQ_EVENT_PUBLISH_ERROR:
            a->pub_err++; a->pub_code = e.u.publish_error.error_code; break;
        case MOQ_EVENT_NS_SUB_ERROR:
            a->nss_err++; a->nss_code = e.u.ns_sub_error.error_code; break;
        case MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR:
            a->st_err++; a->st_code = e.u.subscribe_tracks_error.error_code; break;
        default:
            break;
        }
        moq_event_cleanup(&e);
    }
}

static moq_result_t client_status(rig_t *r, const moq_bytes_t *parts)
{
    moq_track_status_cfg_t ts;
    moq_track_status_cfg_init(&ts);
    ts.track_namespace = (moq_namespace_t){ (moq_bytes_t *)parts, 1 };
    ts.track_name = MOQ_BYTES_LITERAL("v");
    moq_track_status_handle_t h;
    return moq_session_track_status(r->cl, &ts, moq_simpair_now_us(r->sp), &h);
}

static moq_result_t client_subscribe(rig_t *r, const moq_bytes_t *parts, bool new_group,
                                     uint64_t value, moq_subscription_t *out)
{
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ (moq_bytes_t *)parts, 1 };
    sc.track_name = MOQ_BYTES_LITERAL("v");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    sc.has_new_group_request = new_group;
    sc.new_group_request = value;
    moq_subscription_t h;
    return moq_session_subscribe(r->cl, &sc, moq_simpair_now_us(r->sp), out ? out : &h);
}

static void write_object(rig_t *r, uint64_t group, uint64_t object)
{
    moq_rcbuf_t *payload = NULL;
    MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"abc", 3, &payload) == MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_pub_write_object(r->pub, r->track, group, object, payload,
                                               moq_simpair_now_us(r->sp)), MOQ_OK);
    moq_rcbuf_decref(payload);
}

/* -- TRACK_STATUS -------------------------------------------------------- */

static void test_status_own_track(moq_version_t v, bool dynamic)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, true, dynamic));
    /* A subscriber makes the track publish; the status then reports it. */
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(&r, k_ns_parts, false, 0, NULL), MOQ_OK);
    rig_pump(&r);
    write_object(&r, 4, 0);
    write_object(&r, 4, 1);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.sub_ok, 1);
    size_t want_len;
    const uint8_t *want = expected_props(&r, dynamic, &want_len);
    MOQ_TEST_CHECK_EQ_SIZE(a.sub_len, want_len);
    MOQ_TEST_CHECK(a.sub_len == want_len && memcmp(a.sub_props, want, want_len) == 0);
    MOQ_TEST_CHECK(a.sub_dynamic == dynamic);

    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
    MOQ_TEST_CHECK(a.ts_largest && a.ts_group == 4 && a.ts_object == 1);
    /* Draft-16's TRACK_STATUS answer has no Track Properties field. */
    size_t ts_want = v == MOQ_VERSION_DRAFT_16 ? 0 : want_len;
    MOQ_TEST_CHECK_EQ_SIZE(a.ts_len, ts_want);
    MOQ_TEST_CHECK(ts_want == 0 || memcmp(a.ts_props, want, ts_want) == 0);
    rig_close(&r);
}

static void test_status_refusals(moq_version_t v)
{
    /* No such track: DOES_NOT_EXIST. */
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, false, false));
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_other_parts), MOQ_OK);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.ts_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    rig_close(&r);
    /* REJECT_ALL authorizes nothing. */
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_REJECT_ALL, 0, false, false));
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.ts_code, MOQ_REQUEST_ERROR_UNAUTHORIZED);
    rig_close(&r);
    /* A callback mode answers only for a track with an accepted subscription
     * on this session, as a standalone FETCH is served; the callback, which
     * authorizes subscriptions, is not consulted. */
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_CALLBACK, 0, false, false));
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.ts_code, MOQ_REQUEST_ERROR_UNAUTHORIZED);
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(&r, k_ns_parts, false, 0, NULL), MOQ_OK);
    rig_pump(&r);
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.sub_ok, 1);
    MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
    rig_close(&r);
}

/* Manual dispatch: this publisher's track is CONSUMED and answered; another
 * track's request stays IGNORED for some other owner and is not answered. */
static void test_status_manual(moq_version_t v)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, true, false));
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_other_parts), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    (void)moq_simpair_run_until_quiescent(r.sp, 32, NULL);
    int consumed = 0, ignored = 0;
    moq_event_t e;
    while (moq_session_poll_events(r.sv, &e, 1)) {
        moq_pub_event_result_t res;
        if (e.kind == MOQ_EVENT_TRACK_STATUS_REQUEST) {
            MOQ_TEST_CHECK_EQ_INT(moq_pub_handle_event(r.pub, &e, moq_simpair_now_us(r.sp),
                                                       &res), MOQ_OK);
            if (res == MOQ_PUB_EVENT_CONSUMED) consumed++;
            if (res == MOQ_PUB_EVENT_IGNORED) ignored++;
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_INT(consumed, 1);
    MOQ_TEST_CHECK_EQ_INT(ignored, 1);
    (void)moq_simpair_run_until_quiescent(r.sp, 32, NULL);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
    MOQ_TEST_CHECK_EQ_INT(a.ts_err, 0);
    rig_close(&r);
}

/* With no action credit the answer is held; a later tick sends it. A track
 * removed while its answer is held is answered DOES_NOT_EXIST. */
static void test_status_held(moq_version_t v, bool remove_first)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 1, false, false));
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    (void)moq_simpair_run_until_quiescent(r.sp, 32, NULL);
    /* Occupy the server's single action slot before the publisher ticks. */
    moq_publish_namespace_cfg_t filler;
    moq_publish_namespace_cfg_init(&filler);
    filler.track_namespace = (moq_namespace_t){ k_other_parts, 1 };
    moq_announcement_t fa;
    MOQ_TEST_CHECK_EQ_INT(moq_session_publish_namespace(r.sv, &filler,
                                                        moq_simpair_now_us(r.sp), &fa),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_action_capacity(r.sv), 0);
    moq_result_t trc = moq_pub_tick(r.pub, moq_simpair_now_us(r.sp));
    MOQ_TEST_CHECK_EQ_INT(trc, MOQ_ERR_WOULD_BLOCK);
    if (remove_first)
        MOQ_TEST_CHECK_EQ_INT(moq_pub_remove_track(r.pub, r.track, moq_simpair_now_us(r.sp)),
                              MOQ_OK);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    if (remove_first) {
        MOQ_TEST_CHECK_EQ_INT(a.ts_err, 1);
        MOQ_TEST_CHECK_EQ_U64(a.ts_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    } else {
        MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
    }
    rig_close(&r);
}

/* A legal Track Properties block larger than the session's 4096-byte send
 * buffer: one odd-type (length-prefixed) property, type 0x79, 5000 bytes. */
static uint8_t *big_props(moq_version_t v, size_t *len)
{
    const size_t value = 5000;
    uint8_t *p = malloc(value + 8);
    if (!p) return NULL;
    size_t n = 0;
    if (v == MOQ_VERSION_DRAFT_16) {
        p[n++] = 0x40; p[n++] = 0x79;                       /* QUIC varint */
        p[n++] = (uint8_t)(0x40 | (value >> 8)); p[n++] = (uint8_t)value;
    } else {
        p[n++] = 0x79;                                       /* vi64 */
        p[n++] = (uint8_t)(0x80 | (value >> 8)); p[n++] = (uint8_t)value;
    }
    memset(p + n, 'p', value);
    *len = n + value;
    return p;
}

/* A TRACK_STATUS whose acceptance cannot be encoded gets a terminal refusal,
 * sent at once (held = false) or by a later tick after the session could not
 * take the answer (held = true); the request is not left pending and the
 * session stays up. A TRACK_STATUS for a track whose properties fit is
 * still accepted. */
static void test_status_unencodable(moq_version_t v, bool held)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, held ? 1 : 0, false, false));
    size_t blen = 0;
    uint8_t *bp = big_props(v, &blen);
    MOQ_TEST_CHECK(bp != NULL);
    if (!bp) { rig_close(&r); return; }
    moq_pub_track_cfg_t tc;
    moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
    tc.track_namespace = (moq_namespace_t){ k_other_parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("v");
    tc.track_properties = (moq_bytes_t){ bp, blen };
    moq_pub_track_t *big = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(r.pub, &tc, moq_simpair_now_us(r.sp), &big),
                          MOQ_OK);
    free(bp);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_other_parts), MOQ_OK);
    (void)moq_simpair_run_until_quiescent(r.sp, 32, NULL);
    if (held) {
        /* Occupy the server's single action slot before the publisher
         * ticks: the acceptance is held, and fails to encode later. */
        moq_publish_namespace_cfg_t filler;
        moq_publish_namespace_cfg_init(&filler);
        filler.track_namespace = (moq_namespace_t){ k_ns_parts, 1 };
        moq_announcement_t fa;
        MOQ_TEST_CHECK_EQ_INT(moq_session_publish_namespace(r.sv, &filler,
                                                            moq_simpair_now_us(r.sp), &fa),
                              MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(moq_session_action_capacity(r.sv), 0);
        MOQ_TEST_CHECK_EQ_INT(moq_pub_tick(r.pub, moq_simpair_now_us(r.sp)),
                              MOQ_ERR_WOULD_BLOCK);
    }
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    if (v == MOQ_VERSION_DRAFT_16) {
        /* Draft-16's TRACK_STATUS_OK carries no Track Properties: the
         * acceptance fits and is sent. */
        MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
        MOQ_TEST_CHECK_EQ_INT(a.ts_err, 0);
    } else {
        MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 0);
        MOQ_TEST_CHECK_EQ_INT(a.ts_err, 1);
        MOQ_TEST_CHECK_EQ_U64(a.ts_code, MOQ_REQUEST_ERROR_INTERNAL_ERROR);
    }
    MOQ_TEST_CHECK(moq_session_state(r.sv) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(r.cl) == MOQ_SESS_ESTABLISHED);
    /* The fitting track still answers. */
    MOQ_TEST_CHECK_EQ_INT(client_status(&r, k_ns_parts), MOQ_OK);
    rig_pump(&r);
    memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.ts_ok, 1);
    MOQ_TEST_CHECK_EQ_INT(a.ts_err, 0);
    rig_close(&r);
}

/* -- Requests nothing else takes ---------------------------------------- */

static void test_refuse_unserved(moq_version_t v)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, false, false));
    uint64_t now = moq_simpair_now_us(r.sp);
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(&r, k_other_parts, false, 0, NULL), MOQ_OK);
    moq_publish_namespace_cfg_t pn;
    moq_publish_namespace_cfg_init(&pn);
    pn.track_namespace = (moq_namespace_t){ k_other_parts, 1 };
    moq_announcement_t ann;
    MOQ_TEST_CHECK_EQ_INT(moq_session_publish_namespace(r.cl, &pn, now, &ann), MOQ_OK);
    moq_publish_cfg_t pc;
    moq_publish_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ k_other_parts, 1 };
    pc.track_name = MOQ_BYTES_LITERAL("pushed");
    moq_publication_t pubh;
    MOQ_TEST_CHECK_EQ_INT(moq_session_publish(r.cl, &pc, now, &pubh), MOQ_OK);
    moq_subscribe_namespace_cfg_t sn;
    moq_subscribe_namespace_cfg_init(&sn);
    sn.track_namespace_prefix = (moq_namespace_t){ k_other_parts, 1 };
    sn.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_namespace(r.cl, &sn, now, &nsh), MOQ_OK);
    bool tracks = v != MOQ_VERSION_DRAFT_16;
    if (tracks) {
        moq_subscribe_tracks_cfg_t st;
        moq_subscribe_tracks_cfg_init(&st);
        st.track_namespace_prefix = (moq_namespace_t){ k_ns_parts, 1 };
        moq_track_sub_handle_t sth;
        MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(r.cl, &st, now, &sth), MOQ_OK);
    }
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.sub_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.sub_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    MOQ_TEST_CHECK_EQ_INT(a.ns_rej, 1);
    MOQ_TEST_CHECK_EQ_U64(a.ns_code, MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    MOQ_TEST_CHECK_EQ_INT(a.pub_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.pub_code, MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    MOQ_TEST_CHECK_EQ_INT(a.nss_err, 1);
    MOQ_TEST_CHECK_EQ_U64(a.nss_code, MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    if (tracks) {
        MOQ_TEST_CHECK_EQ_INT(a.st_err, 1);
        MOQ_TEST_CHECK_EQ_U64(a.st_code, MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    }
    MOQ_TEST_CHECK(moq_session_state(r.cl) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(r.sv) == MOQ_SESS_ESTABLISHED);
    rig_close(&r);
}

/* -- Track Properties on the other answers ------------------------------ */

static void test_fetch_and_publish_properties(moq_version_t v)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, true, false));
    size_t want_len;
    const uint8_t *want = expected_props(&r, false, &want_len);
    /* The retained group answers a FETCH. */
    moq_rcbuf_t *obj = NULL;
    MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"cat", 3, &obj) == MOQ_OK);
    moq_pub_retained_object_t ro = { .object_id = 0, .payload = obj };
    moq_pub_retained_group_cfg_t rg;
    moq_pub_retained_group_cfg_init(&rg);
    rg.group_id = 2;
    rg.objects = &ro;
    rg.object_count = 1;
    MOQ_TEST_CHECK_EQ_INT(moq_pub_set_retained_group(r.pub, r.track, &rg), MOQ_OK);
    moq_rcbuf_decref(obj);
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    fc.track_namespace = (moq_namespace_t){ k_ns_parts, 1 };
    fc.track_name = MOQ_BYTES_LITERAL("v");
    fc.start_group = 2;
    fc.end_group = 2;
    fc.end_object = 1;
    moq_fetch_t fh;
    MOQ_TEST_CHECK_EQ_INT(moq_session_fetch(r.cl, &fc, moq_simpair_now_us(r.sp), &fh), MOQ_OK);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.fetch_ok, 1);
    MOQ_TEST_CHECK_EQ_SIZE(a.fetch_len, want_len);
    MOQ_TEST_CHECK(a.fetch_len == want_len && memcmp(a.fetch_props, want, want_len) == 0);
    /* PUBLISH carries the track's properties when the publish cfg has none. */
    moq_pub_publish_cfg_t pp;
    moq_pub_publish_cfg_init(&pp);
    MOQ_TEST_CHECK_EQ_INT(moq_pub_publish_track(r.pub, r.track, &pp,
                                                moq_simpair_now_us(r.sp)), MOQ_OK);
    (void)moq_simpair_run_until_quiescent(r.sp, 32, NULL);
    size_t publish_len = SIZE_MAX;
    bool same = false;
    moq_event_t e;
    while (moq_session_poll_events(r.cl, &e, 1)) {
        if (e.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            publish_len = e.u.publish_request.track_properties.len;
            same = publish_len == want_len &&
                   memcmp(e.u.publish_request.track_properties.data, want, want_len) == 0;
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(publish_len, want_len);
    MOQ_TEST_CHECK(same);
    rig_close(&r);
}

/* Track Properties the session would refuse fail add_track. */
static void test_add_track_refusals(void)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    if (!sp) return;
    (void)moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_pub_cfg_t pc;
    moq_pub_cfg_init_sized(&pc, sizeof(pc));
    pc.accept_mode = MOQ_PUB_ACCEPT_ALL;
    moq_publisher_t *pub = NULL;
    MOQ_TEST_CHECK(moq_pub_create(moq_simpair_server(sp), moq_alloc_default(), &pc, &pub) ==
                   MOQ_OK);
    static const uint8_t dyn_two[] = { 0x30, 0x02 };   /* §10.6: 0 or 1 only */
    static const uint8_t dyn_zero[] = { 0x30, 0x00 };
    moq_pub_track_cfg_t tc;
    moq_pub_track_t *t = NULL;
    moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
    tc.track_namespace = (moq_namespace_t){ k_ns_parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("v");
    tc.track_properties = (moq_bytes_t){ dyn_two, sizeof(dyn_two) };
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_ERR_INVAL);
    tc.track_properties = (moq_bytes_t){ dyn_zero, sizeof(dyn_zero) };
    tc.dynamic_groups = true;
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_ERR_INVAL);
    tc.track_properties = (moq_bytes_t){ NULL, 2 };
    tc.dynamic_groups = false;
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_ERR_INVAL);
    /* A caller whose struct ends before the fields sends none. */
    tc.track_properties = (moq_bytes_t){ dyn_two, sizeof(dyn_two) };
    tc.dynamic_groups = true;
    tc.struct_size = (uint32_t)offsetof(moq_pub_track_cfg_t, _reserved_track_tail2);
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_OK);
    moq_pub_destroy(pub);
    moq_simpair_destroy(sp);
}

/* The dynamic_groups convenience flag against a DYNAMIC_GROUPS property the
 * application already stated inside IMMUTABLE_PROPERTIES (§10.6/§10.7:
 * looked up in both lists): an explicit zero there is refused before the
 * track exists, an explicit one is kept as it is, and an absent property is
 * added. */
static void test_add_track_immutable_dynamic_groups(moq_version_t v)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = v;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    if (!sp) return;
    (void)moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_pub_cfg_t pc;
    moq_pub_cfg_init_sized(&pc, sizeof(pc));
    pc.accept_mode = MOQ_PUB_ACCEPT_ALL;
    moq_publisher_t *pub = NULL;
    MOQ_TEST_CHECK(moq_pub_create(moq_simpair_server(sp), moq_alloc_default(), &pc, &pub) ==
                   MOQ_OK);
    static const uint8_t imm_zero[] = { 0x0B, 0x02, 0x30, 0x00 };
    static const uint8_t imm_one[] = { 0x0B, 0x02, 0x30, 0x01 };
    moq_pub_track_cfg_t tc;
    moq_pub_track_t *t = NULL;
    moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
    tc.track_namespace = (moq_namespace_t){ k_ns_parts, 1 };
    tc.track_name = MOQ_BYTES_LITERAL("z");
    tc.dynamic_groups = true;
    tc.track_properties = (moq_bytes_t){ imm_zero, sizeof(imm_zero) };
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(t == NULL);
    tc.track_name = MOQ_BYTES_LITERAL("o");
    tc.track_properties = (moq_bytes_t){ imm_one, sizeof(imm_one) };
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_OK);
    tc.track_name = MOQ_BYTES_LITERAL("a");
    tc.track_properties = (moq_bytes_t){ NULL, 0 };
    t = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_pub_add_track(pub, &tc, 1, &t), MOQ_OK);
    moq_pub_destroy(pub);
    moq_simpair_destroy(sp);
}

/* -- NEW_GROUP_REQUEST --------------------------------------------------- */

static void test_new_group(moq_version_t v, bool dynamic)
{
    rig_t r;
    MOQ_TEST_CHECK(rig_open(&r, v, MOQ_PUB_ACCEPT_ALL, 0, false, dynamic));
    /* No group yet: any value warrants one. */
    moq_subscription_t s1;
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(&r, k_ns_parts, true, 7, &s1), MOQ_OK);
    rig_pump(&r);
    MOQ_TEST_CHECK_EQ_INT(r.ng.calls, dynamic ? 1 : 0);
    if (dynamic) {
        MOQ_TEST_CHECK_EQ_U64(r.ng.requested, 7);
        MOQ_TEST_CHECK(!r.ng.has_current);
    }
    write_object(&r, 5, 0);
    rig_pump(&r);
    answers_t a; memset(&a, 0, sizeof(a));
    collect(r.cl, &a);
    MOQ_TEST_CHECK_EQ_INT(a.sub_ok, 1);
    /* An update asking for group 6 (one past the current 5) warrants one; one
     * naming a group already published does not. */
    moq_subscription_update_cfg_t u;
    moq_subscription_update_cfg_init_sized(&u, sizeof(u));
    u.has_new_group_request = true;
    u.new_group_request = 6;
    moq_result_t urc = moq_session_update_subscription(r.cl, s1, &u, moq_simpair_now_us(r.sp));
    /* A subscriber may only ask on an update when the track advertises
     * DYNAMIC_GROUPS (§9.20.19), so the session refuses it otherwise. */
    MOQ_TEST_CHECK_EQ_INT(urc, dynamic ? MOQ_OK : MOQ_ERR_INVAL);
    rig_pump(&r);
    if (dynamic) {
        MOQ_TEST_CHECK_EQ_INT(r.ng.calls, 2);
        MOQ_TEST_CHECK_EQ_U64(r.ng.requested, 6);
        MOQ_TEST_CHECK(r.ng.has_current && r.ng.current == 5);
        u.new_group_request = 5;
        MOQ_TEST_CHECK_EQ_INT(moq_session_update_subscription(r.cl, s1, &u,
                                                              moq_simpair_now_us(r.sp)), MOQ_OK);
        rig_pump(&r);
        MOQ_TEST_CHECK_EQ_INT(r.ng.calls, 2);
        u.new_group_request = 0;   /* "no Group information" */
        MOQ_TEST_CHECK_EQ_INT(moq_session_update_subscription(r.cl, s1, &u,
                                                              moq_simpair_now_us(r.sp)), MOQ_OK);
        rig_pump(&r);
        MOQ_TEST_CHECK_EQ_INT(r.ng.calls, 3);
        MOQ_TEST_CHECK_EQ_U64(r.ng.requested, 0);
    }
    MOQ_TEST_CHECK(moq_session_state(r.sv) == MOQ_SESS_ESTABLISHED);
    rig_close(&r);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(k_versions) / sizeof(k_versions[0]); i++) {
        moq_version_t v = k_versions[i];
        test_status_own_track(v, false);
        test_status_own_track(v, true);
        test_status_refusals(v);
        test_status_manual(v);
        test_status_held(v, false);
        test_status_held(v, true);
        test_status_unencodable(v, false);
        test_status_unencodable(v, true);
        test_refuse_unserved(v);
        test_fetch_and_publish_properties(v);
        test_new_group(v, true);
        test_new_group(v, false);
    }
    test_add_track_refusals();
    test_add_track_immutable_dynamic_groups(MOQ_VERSION_DRAFT_18);
    test_add_track_immutable_dynamic_groups(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_PASS("publisher_track_status");
    return failures != 0;
}
