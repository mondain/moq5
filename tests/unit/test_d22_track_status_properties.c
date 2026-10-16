/*
 * TRACK_STATUS answers and the INCLUDE_PROPERTIES request parameter.
 *
 * Draft-22 §9.13: "If successful, the publisher responds with a
 * TRACK_STATUS_OK with the same parameters and Track Properties it would have
 * set in a SUBSCRIBE_OK." §9.3: "Track Properties are populated in
 * TRACK_STATUS_OK". §9.20.21: INCLUDE_PROPERTIES "MAY appear in SUBSCRIBE,
 * TRACK_STATUS, FETCH or SUBSCRIBE_TRACKS ... If INCLUDE_PROPERTIES is 0, the
 * Track Properties are still present in the message, but they SHOULD be
 * empty." Draft-18 §10.14 carries the same TRACK_STATUS_OK sentence; draft-16
 * answers with a REQUEST_OK that has no Track Properties.
 *
 * Pinned here:
 *   - the answer's Track Properties reach the requester (drafts 18 and 22)
 *     and are validated, never sent, on draft-16;
 *   - a request that asked for none gets none, whatever the application
 *     supplied;
 *   - SUBSCRIBE, FETCH and TRACK_STATUS carry INCLUDE_PROPERTIES exactly as
 *     selected, never when not selected or when the caller's struct ends
 *     before the field, and selecting it on drafts 16/18 is UNSUPPORTED
 *     before anything is queued;
 *   - invalid answer properties are refused before anything changes;
 *   - the reported Largest Object is at least the track's registry record,
 *     as SUBSCRIBE_OK reports it;
 *   - the initializer symbols keep their original extent;
 *   - every draft's FETCH_OK carries the application's track properties
 *     (draft-16 §9.17 Figure 19 ends FETCH_OK with Track Extensions).
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/params_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <stddef.h>
#include <string.h>

static int failures;

/* One Track Property in the application range (type 0x78, value 1). */
static const uint8_t k_props[] = { 0x78, 0x01 };
/* DYNAMIC_GROUPS = 2: out of range (§10.6), refused on draft-22. */
static const uint8_t k_bad_props[] = { 0x30, 0x02 };

static moq_bytes_t k_ns_parts[] = { { (const uint8_t *)"live", 4 } };

static moq_simpair_t *mk_pair(moq_version_t v)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = v;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_client(sp), &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(moq_simpair_server(sp), &ev, 1) > 0) moq_event_cleanup(&ev);
    return sp;
}

static moq_namespace_t live_ns(void) { return (moq_namespace_t){ k_ns_parts, 1 }; }

/* The server's TRACK_STATUS_REQUEST handle, after delivery. */
static moq_track_status_handle_t server_ts_request(moq_simpair_t *sp)
{
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_track_status_handle_t h = { 0 };
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_server(sp), &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_TRACK_STATUS_REQUEST) h = ev.u.track_status_request.handle;
        moq_event_cleanup(&ev);
    }
    return h;
}

typedef struct ts_answer {
    bool     ok;
    bool     has_largest;
    uint64_t largest_group, largest_object;
    uint8_t  props[16];
    size_t   props_len;
} ts_answer_t;

static ts_answer_t client_ts_answer(moq_simpair_t *sp)
{
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    ts_answer_t a;
    memset(&a, 0, sizeof(a));
    a.props_len = SIZE_MAX;
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_client(sp), &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_TRACK_STATUS_OK) {
            const moq_track_status_ok_event_t *ok = &ev.u.track_status_ok;
            a.ok = true;
            a.has_largest = ok->has_largest;
            a.largest_group = ok->largest_group;
            a.largest_object = ok->largest_object;
            a.props_len = ok->track_properties.len;
            if (a.props_len <= sizeof(a.props) && a.props_len)
                memcpy(a.props, ok->track_properties.data, a.props_len);
        }
        moq_event_cleanup(&ev);
    }
    return a;
}

/* A TRACK_STATUS for live/v from the client. */
static moq_result_t client_ts(moq_simpair_t *sp, int include, size_t cfg_size)
{
    moq_track_status_cfg_t cfg;
    moq_track_status_cfg_init(&cfg);
    cfg.track_namespace = live_ns();
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    if (include >= 0) {
        cfg.has_include_properties = true;
        cfg.include_properties = include != 0;
    }
    if (cfg_size) cfg.struct_size = (uint32_t)cfg_size;
    moq_track_status_handle_t h;
    return moq_session_track_status(moq_simpair_client(sp), &cfg,
                                    moq_simpair_now_us(sp), &h);
}

/* -- Answers carry the application's Track Properties ------------------ */

static void test_answer_properties(moq_version_t v)
{
    moq_simpair_t *sp = mk_pair(v);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, -1, 0), MOQ_OK);
    moq_track_status_handle_t h = server_ts_request(sp);
    MOQ_TEST_CHECK(h._opaque != 0);
    moq_accept_track_status_cfg_t ac;
    moq_accept_track_status_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(moq_simpair_server(sp), h, &ac,
                                                          moq_simpair_now_us(sp)), MOQ_OK);
    ts_answer_t a = client_ts_answer(sp);
    MOQ_TEST_CHECK(a.ok);
    if (v == MOQ_VERSION_DRAFT_16) {
        MOQ_TEST_CHECK_EQ_SIZE(a.props_len, 0);
    } else {
        MOQ_TEST_CHECK_EQ_SIZE(a.props_len, sizeof(k_props));
        MOQ_TEST_CHECK(memcmp(a.props, k_props, sizeof(k_props)) == 0);
    }
    MOQ_TEST_CHECK(moq_session_state(moq_simpair_client(sp)) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(moq_simpair_server(sp)) == MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
}

/* An accept cfg whose struct_size ends at expires_ms sends no properties,
 * whatever the bytes past it hold. */
static void test_answer_old_cfg(void)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, -1, 0), MOQ_OK);
    moq_track_status_handle_t h = server_ts_request(sp);
    moq_accept_track_status_cfg_t ac;
    moq_accept_track_status_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    ac.struct_size = (uint32_t)offsetof(moq_accept_track_status_cfg_t, track_properties);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(moq_simpair_server(sp), h, &ac,
                                                          moq_simpair_now_us(sp)), MOQ_OK);
    ts_answer_t a = client_ts_answer(sp);
    MOQ_TEST_CHECK(a.ok);
    MOQ_TEST_CHECK_EQ_SIZE(a.props_len, 0);
    moq_simpair_destroy(sp);
}

/* INCLUDE_PROPERTIES = 0 on the request empties the answer's properties; 1
 * and the default keep them. */
static void test_answer_include(int include, size_t expected)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, include, 0), MOQ_OK);
    moq_track_status_handle_t h = server_ts_request(sp);
    moq_accept_track_status_cfg_t ac;
    moq_accept_track_status_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(moq_simpair_server(sp), h, &ac,
                                                          moq_simpair_now_us(sp)), MOQ_OK);
    ts_answer_t a = client_ts_answer(sp);
    MOQ_TEST_CHECK(a.ok);
    MOQ_TEST_CHECK_EQ_SIZE(a.props_len, expected);
    moq_simpair_destroy(sp);
}

/* Invalid properties are refused before anything is queued, and the request
 * can still be answered. */
static void test_answer_invalid(void)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, -1, 0), MOQ_OK);
    moq_track_status_handle_t h = server_ts_request(sp);
    moq_session_t *sv = moq_simpair_server(sp);
    moq_accept_track_status_cfg_t ac;
    moq_accept_track_status_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ k_bad_props, sizeof(k_bad_props) };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(sv, h, &ac,
                                                          moq_simpair_now_us(sp)),
                          MOQ_ERR_INVAL);
    ac.track_properties = (moq_bytes_t){ NULL, 1 };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(sv, h, &ac,
                                                          moq_simpair_now_us(sp)),
                          MOQ_ERR_INVAL);
    moq_action_t act;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(sv, &act, 1), 0);
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(sv, h, &ac,
                                                          moq_simpair_now_us(sp)), MOQ_OK);
    ts_answer_t a = client_ts_answer(sp);
    MOQ_TEST_CHECK(a.ok);
    MOQ_TEST_CHECK_EQ_SIZE(a.props_len, sizeof(k_props));
    moq_simpair_destroy(sp);
}

/* The answer reports at least the Largest Object the server's registry holds
 * for the track (here from an accepted SUBSCRIBE that reported {5, 3}). */
static void test_answer_largest(moq_version_t v, bool cfg_has, uint64_t cfg_g,
                                uint64_t cfg_o, uint64_t want_g, uint64_t want_o)
{
    moq_simpair_t *sp = mk_pair(v);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_session_t *cl = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = live_ns();
    sc.track_name = MOQ_BYTES_LITERAL("v");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t csub;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe(cl, &sc, moq_simpair_now_us(sp), &csub), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_subscription_t ssub = MOQ_SUBSCRIPTION_INVALID;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t as;
    moq_accept_subscribe_cfg_init(&as);
    as.has_largest = true;
    as.largest_group = 5;
    as.largest_object = 3;
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe(sv, ssub, &as, moq_simpair_now_us(sp)),
                          MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    while (moq_session_poll_events(cl, &ev, 1) > 0) moq_event_cleanup(&ev);

    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, -1, 0), MOQ_OK);
    moq_track_status_handle_t h = server_ts_request(sp);
    moq_accept_track_status_cfg_t ac;
    moq_accept_track_status_cfg_init(&ac);
    ac.has_largest = cfg_has;
    ac.largest_group = cfg_g;
    ac.largest_object = cfg_o;
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_track_status(sv, h, &ac, moq_simpair_now_us(sp)),
                          MOQ_OK);
    ts_answer_t a = client_ts_answer(sp);
    MOQ_TEST_CHECK(a.ok);
    MOQ_TEST_CHECK(a.has_largest);
    MOQ_TEST_CHECK_EQ_U64(a.largest_group, want_g);
    MOQ_TEST_CHECK_EQ_U64(a.largest_object, want_o);
    moq_simpair_destroy(sp);
}

/* -- INCLUDE_PROPERTIES on outbound requests --------------------------- */

/* The INCLUDE_PROPERTIES value the client's next request carries: -1 when
 * absent, -2 when no request was found. */
static int sent_include(moq_session_t *cl, uint64_t type)
{
    int found = -2;
    moq_action_t a;
    while (moq_session_poll_actions(cl, &a, 1) > 0) {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            data = a.u.open_bidi_stream.data;
            len = a.u.open_bidi_stream.len;
        }
        if (data) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, data, len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.max_filter_ranges = 0;
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK && env.msg_type == type &&
                moq_d22_decode_message(type, env.payload, env.payload_len, &ctx, &m) == MOQ_OK) {
                found = -1;
                moq_buf_reader_t pr;
                moq_buf_reader_init(&pr, m.params.encoded.data, m.params.encoded.len);
                uint64_t prev = 0;
                while (moq_buf_reader_remaining(&pr) > 0) {
                    moq_d22_param_t pv;
                    memset(&pv, 0, sizeof(pv));
                    if (moq_d22_param_next_scanned(&pr, &prev, &pv) != MOQ_OK) { found = -3; break; }
                    if (pv.type == 0x35) found = (int)pv.value.integer;
                }
            }
        }
        moq_action_cleanup(&a);
    }
    return found;
}

static moq_result_t client_subscribe(moq_session_t *cl, int include, size_t cfg_size,
                                     uint64_t now)
{
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = live_ns();
    sc.track_name = MOQ_BYTES_LITERAL("v");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    if (include >= 0) {
        sc.has_include_properties = true;
        sc.include_properties = include != 0;
    }
    if (cfg_size) sc.struct_size = (uint32_t)cfg_size;
    moq_subscription_t h;
    return moq_session_subscribe(cl, &sc, now, &h);
}

static moq_result_t client_fetch(moq_session_t *cl, int include, size_t cfg_size,
                                 uint64_t now)
{
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    fc.track_namespace = live_ns();
    fc.track_name = MOQ_BYTES_LITERAL("v");
    fc.start_group = 0;
    fc.end_group = 1;
    if (include >= 0) {
        fc.has_include_properties = true;
        fc.include_properties = include != 0;
    }
    if (cfg_size) fc.struct_size = (uint32_t)cfg_size;
    moq_fetch_t h;
    return moq_session_fetch(cl, &fc, now, &h);
}

static void test_request_carries(int which, int include, size_t cfg_size, int want)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_session_t *cl = moq_simpair_client(sp);
    uint64_t now = moq_simpair_now_us(sp);
    uint64_t type = which == 0 ? MOQ_D22_SUBSCRIBE
                  : which == 1 ? MOQ_D22_FETCH : MOQ_D22_TRACK_STATUS;
    moq_result_t rc = which == 0 ? client_subscribe(cl, include, cfg_size, now)
                    : which == 1 ? client_fetch(cl, include, cfg_size, now)
                                 : client_ts(sp, include, cfg_size);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    int got = sent_include(cl, type);
    if (got != want)
        fprintf(stderr, "  request %d include %d size %zu: carried %d, expected %d\n",
                which, include, cfg_size, got, want);
    MOQ_TEST_CHECK_EQ_INT(got, want);
    moq_simpair_destroy(sp);
}

/* End to end: a SUBSCRIBE that asked for none is answered with none. */
static void test_subscribe_answer_empty(void)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_session_t *cl = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(cl, 0, 0, moq_simpair_now_us(sp)), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_subscription_t ssub = MOQ_SUBSCRIPTION_INVALID;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t as;
    moq_accept_subscribe_cfg_init(&as);
    as.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe(sv, ssub, &as, moq_simpair_now_us(sp)),
                          MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    size_t props = SIZE_MAX;
    while (moq_session_poll_events(cl, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK) props = ev.u.subscribe_ok.track_properties.len;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_SIZE(props, 0);
    moq_simpair_destroy(sp);
}

/* Drafts 16 and 18 have no INCLUDE_PROPERTIES: selecting it is refused
 * before anything is queued. A joining fetch never carries it. */
static void test_unsupported(moq_version_t v)
{
    moq_simpair_t *sp = mk_pair(v);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_session_t *cl = moq_simpair_client(sp);
    uint64_t now = moq_simpair_now_us(sp);
    for (int include = 0; include <= 1; include++) {
        MOQ_TEST_CHECK_EQ_INT(client_subscribe(cl, include, 0, now), MOQ_ERR_UNSUPPORTED);
        MOQ_TEST_CHECK_EQ_INT(client_fetch(cl, include, 0, now), MOQ_ERR_UNSUPPORTED);
        MOQ_TEST_CHECK_EQ_INT(client_ts(sp, include, 0), MOQ_ERR_UNSUPPORTED);
    }
    moq_action_t a;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(cl, &a, 1), 0);
    /* Not selected: the requests go out as before. */
    MOQ_TEST_CHECK_EQ_INT(client_subscribe(cl, -1, 0, now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(client_ts(sp, -1, 0), MOQ_OK);
    moq_simpair_destroy(sp);
}

static void test_joining_fetch_refused(void)
{
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_18);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_fetch_cfg_t fc;
    moq_fetch_cfg_init_sized(&fc, sizeof(fc));
    fc.is_joining = true;
    fc.joining_relative = true;
    fc.joining_sub = (moq_subscription_t){ 1 };
    fc.has_include_properties = true;
    fc.include_properties = false;
    moq_fetch_t h;
    MOQ_TEST_CHECK_EQ_INT(moq_session_fetch(moq_simpair_client(sp), &fc,
                                            moq_simpair_now_us(sp), &h), MOQ_ERR_INVAL);
    moq_simpair_destroy(sp);
}

/* -- FETCH_OK on every draft -------------------------------------------- */

static void test_fetch_ok_properties(moq_version_t v)
{
    /* Draft-16 Track Extensions use absolute types: 0x78 (two-byte varint,
     * even, so a varint value of 1). Drafts 18/22 use the delta form. */
    static const uint8_t d16_ext[] = { 0x40, 0x78, 0x01 };
    const uint8_t *props = v == MOQ_VERSION_DRAFT_16 ? d16_ext : k_props;
    size_t props_len = v == MOQ_VERSION_DRAFT_16 ? sizeof(d16_ext) : sizeof(k_props);
    moq_simpair_t *sp = mk_pair(v);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_session_t *cl = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    MOQ_TEST_CHECK_EQ_INT(client_fetch(cl, -1, 0, moq_simpair_now_us(sp)), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_fetch_t h = { 0 };
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) h = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 1;
    ac.track_properties = (moq_bytes_t){ props, props_len };
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_fetch(sv, h, &ac, moq_simpair_now_us(sp)),
                          MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    size_t got = SIZE_MAX;
    bool same = false;
    while (moq_session_poll_events(cl, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_OK) {
            got = ev.u.fetch_ok.track_properties.len;
            same = got == props_len &&
                   memcmp(ev.u.fetch_ok.track_properties.data, props, props_len) == 0;
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_SIZE(got, props_len);
    MOQ_TEST_CHECK(same);
    MOQ_TEST_CHECK(moq_session_state(cl) == MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
}

/* -- Initializers ------------------------------------------------------- */

static void test_initializers(void)
{
    union { moq_track_status_cfg_t c; unsigned char b[sizeof(moq_track_status_cfg_t)]; } t;
    memset(&t, 0xA5, sizeof(t));
    (moq_track_status_cfg_init)(&t.c);
    MOQ_TEST_CHECK_EQ_U64(t.c.struct_size,
                          offsetof(moq_track_status_cfg_t, has_include_properties));
    for (size_t i = offsetof(moq_track_status_cfg_t, has_include_properties); i < sizeof(t); i++)
        MOQ_TEST_CHECK(t.b[i] == 0xA5);
    memset(&t, 0xA5, sizeof(t));
    moq_track_status_cfg_init(&t.c);
    MOQ_TEST_CHECK_EQ_U64(t.c.struct_size, sizeof(moq_track_status_cfg_t));
    MOQ_TEST_CHECK(!t.c.has_include_properties && !t.c.include_properties);

    union { moq_accept_track_status_cfg_t c;
            unsigned char b[sizeof(moq_accept_track_status_cfg_t)]; } a;
    memset(&a, 0xA5, sizeof(a));
    (moq_accept_track_status_cfg_init)(&a.c);
    MOQ_TEST_CHECK_EQ_U64(a.c.struct_size,
                          offsetof(moq_accept_track_status_cfg_t, track_properties));
    for (size_t i = offsetof(moq_accept_track_status_cfg_t, track_properties); i < sizeof(a); i++)
        MOQ_TEST_CHECK(a.b[i] == 0xA5);
    memset(&a, 0xA5, sizeof(a));
    moq_accept_track_status_cfg_init(&a.c);
    MOQ_TEST_CHECK_EQ_U64(a.c.struct_size, sizeof(moq_accept_track_status_cfg_t));
    MOQ_TEST_CHECK(a.c.track_properties.data == NULL && a.c.track_properties.len == 0);
    /* A size too small to stamp touches nothing. */
    memset(&a, 0xA5, sizeof(a));
    moq_accept_track_status_cfg_init_sized(&a.c, 2);
    MOQ_TEST_CHECK(a.b[0] == 0xA5 && a.b[3] == 0xA5);
}

int main(void)
{
    test_answer_properties(MOQ_VERSION_DRAFT_16);
    test_answer_properties(MOQ_VERSION_DRAFT_18);
    test_answer_properties(MOQ_VERSION_DRAFT_22);
    test_answer_old_cfg();
    test_answer_include(0, 0);
    test_answer_include(1, sizeof(k_props));
    test_answer_include(-1, sizeof(k_props));
    test_answer_invalid();
    for (int v = 0; v < 3; v++) {
        moq_version_t ver = v == 0 ? MOQ_VERSION_DRAFT_16
                          : v == 1 ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_22;
        test_answer_largest(ver, false, 0, 0, 5, 3);   /* registry only */
        test_answer_largest(ver, true, 2, 9, 5, 3);    /* registry larger */
        test_answer_largest(ver, true, 7, 0, 7, 0);    /* application larger */
    }
    for (int which = 0; which < 3; which++) {
        test_request_carries(which, 0, 0, 0);
        test_request_carries(which, 1, 0, 1);
        test_request_carries(which, -1, 0, -1);
    }
    /* A caller whose struct ends before the pair sends nothing. */
    test_request_carries(0, 0, offsetof(moq_subscribe_cfg_t, has_include_properties), -1);
    test_request_carries(1, 0, offsetof(moq_fetch_cfg_t, has_include_properties), -1);
    test_request_carries(2, 0, offsetof(moq_track_status_cfg_t, has_include_properties), -1);
    test_subscribe_answer_empty();
    test_unsupported(MOQ_VERSION_DRAFT_16);
    test_unsupported(MOQ_VERSION_DRAFT_18);
    test_joining_fetch_refused();
    test_initializers();
    test_fetch_ok_properties(MOQ_VERSION_DRAFT_16);
    test_fetch_ok_properties(MOQ_VERSION_DRAFT_18);
    test_fetch_ok_properties(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_PASS("d22_track_status_properties");
    return failures != 0;
}
