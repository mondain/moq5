/*
 * The media sender answers what reaches its session: a TRACK_STATUS for one
 * of its tracks with the Largest Object and Track Properties a SUBSCRIBE_OK
 * would carry (draft-22 §9.13; draft-18 §10.14 and draft-16 §9.19 alike), a
 * TRACK_STATUS for a track it lacks with DOES_NOT_EXIST, and a request it
 * does not serve with NOT_SUPPORTED (draft-22 §1.6). A track added with
 * track_properties and dynamic_groups states them on SUBSCRIBE_OK with
 * DYNAMIC_GROUPS = 1 added, and a subscriber's NEW_GROUP_REQUEST is reported
 * by moq_media_sender_take_new_group_request (draft-22 §9.20.19). Drives the
 * production sender hook against a real SimPair peer on drafts 16, 18, 22.
 */
#include <moq/media_sender.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include <moq/session.h>
#include "test_support.h"
#include "test_alloc_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static int failures = 0;

/* Test seam (media_sender.c, MOQ_MEDIA_SENDER_TESTING). */
moq_media_sender_t *moq_media_sender_test_new_cfg(
    const moq_media_sender_cfg_t *cfg);
void moq_media_sender_test_pump(moq_media_sender_t *s,
                                moq_session_t *session, uint64_t now_us);
void moq_media_sender_test_free(moq_media_sender_t *s);

/* An unassigned property type below 64 (even, so an integer) with value 1:
 * the same two bytes on every draft. With DYNAMIC_GROUPS added the set is
 * {0x30: 1, 0x3C: 1}, the second delta 0x3C - 0x30. */
static const uint8_t k_props[] = { 0x3C, 0x01 };
static const uint8_t k_stated[] = { 0x30, 0x01, 0x0C, 0x01 };

static moq_simpair_t *pair(moq_alloc_t *alloc, moq_version_t ver)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = alloc; cfg.seed = 42; cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 16;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 16;
    cfg.version = ver;
    moq_simpair_t *sp = NULL;
    moq_simpair_create(&cfg, &sp);
    moq_simpair_start(sp);
    moq_simpair_run_until_quiescent(sp, 8, NULL);
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_client(sp), &ev, 1) == 1)
        moq_event_cleanup(&ev);
    while (moq_session_poll_events(moq_simpair_server(sp), &ev, 1) == 1)
        moq_event_cleanup(&ev);
    return sp;
}

typedef struct peer {
    moq_announcement_t ann;
    int ns_published;
    int sub_ok, ts_ok, ts_err, nss_err;
    bool sub_dynamic;
    uint8_t sub_props[16], ts_props[16];
    size_t sub_len, ts_len;
    bool ts_largest;
    uint64_t ts_err_code, nss_err_code;
} peer_t;

/* The relay side: accept the namespace, record answers. */
static void relay_drain(moq_session_t *srv, peer_t *p, uint64_t now)
{
    moq_event_t ev;
    while (moq_session_poll_events(srv, &ev, 1) == 1) {
        switch (ev.kind) {
        case MOQ_EVENT_NAMESPACE_PUBLISHED: {
            p->ann = ev.u.namespace_published.ann;
            p->ns_published++;
            moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
            (void)moq_session_accept_namespace(srv, p->ann, &ac, now);
            break;
        }
        case MOQ_EVENT_SUBSCRIBE_OK:
            p->sub_ok++;
            p->sub_dynamic = ev.u.subscribe_ok.dynamic_groups;
            p->sub_len = ev.u.subscribe_ok.track_properties.len;
            if (p->sub_len && p->sub_len <= sizeof(p->sub_props))
                memcpy(p->sub_props, ev.u.subscribe_ok.track_properties.data, p->sub_len);
            break;
        case MOQ_EVENT_TRACK_STATUS_OK:
            p->ts_ok++;
            p->ts_largest = ev.u.track_status_ok.has_largest;
            p->ts_len = ev.u.track_status_ok.track_properties.len;
            if (p->ts_len && p->ts_len <= sizeof(p->ts_props))
                memcpy(p->ts_props, ev.u.track_status_ok.track_properties.data, p->ts_len);
            break;
        case MOQ_EVENT_TRACK_STATUS_ERROR:
            p->ts_err++;
            p->ts_err_code = ev.u.track_status_error.error_code;
            break;
        case MOQ_EVENT_NS_SUB_ERROR:
            p->nss_err++;
            p->nss_err_code = ev.u.ns_sub_error.error_code;
            break;
        default:
            break;
        }
        moq_event_cleanup(&ev);
    }
}

static void cycle(moq_simpair_t *sp, moq_media_sender_t *s, peer_t *p, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        uint64_t now = moq_simpair_now_us(sp);
        moq_media_sender_test_pump(s, moq_simpair_client(sp), now);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        relay_drain(moq_simpair_server(sp), p, now);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
    }
}

static void test_sender(moq_version_t ver)
{
    test_alloc_state_t as = {0};
    moq_alloc_t alloc = test_allocator(&as);
    moq_simpair_t *sp = pair(&alloc, ver);
    moq_session_t *srv = moq_simpair_server(sp);

    moq_bytes_t ns_parts[2] = { MOQ_BYTES_LITERAL("svc"), MOQ_BYTES_LITERAL("demo") };
    moq_media_sender_cfg_t cfg;
    moq_media_sender_cfg_init_live_sized(&cfg, sizeof(cfg));
    cfg.namespace_ = (moq_namespace_t){ ns_parts, 2 };
    if (ver == MOQ_VERSION_DRAFT_22)
        cfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
    moq_media_sender_t *s = moq_media_sender_test_new_cfg(&cfg);
    MOQ_TEST_CHECK(s != NULL);

    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init(&tc);
    tc.name = MOQ_BYTES_LITERAL("v");
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec = MOQ_BYTES_LITERAL("av01");
    tc.bitrate = 1500000;
    tc.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    tc.dynamic_groups = true;
    moq_media_track_t *v = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_add_track(s, &tc, &v), MOQ_OK);
    uint64_t requested = 99;
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_take_new_group_request(s, v, &requested), MOQ_DONE);

    peer_t p; memset(&p, 0, sizeof(p));
    cycle(sp, s, &p, 6);
    MOQ_TEST_CHECK_EQ_INT(p.ns_published, 1);
    MOQ_TEST_CHECK(moq_media_sender_is_ready(s));

    /* SUBSCRIBE asking for a new group (value 0: no Group information). */
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ ns_parts, 2 };
    sc.track_name = MOQ_BYTES_LITERAL("v");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    sc.has_new_group_request = true;
    sc.new_group_request = 0;
    moq_subscription_t sub;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe(srv, &sc, moq_simpair_now_us(sp), &sub), MOQ_OK);
    cycle(sp, s, &p, 6);
    MOQ_TEST_CHECK_EQ_INT(p.sub_ok, 1);
    MOQ_TEST_CHECK(p.sub_dynamic);
    MOQ_TEST_CHECK_EQ_SIZE(p.sub_len, sizeof(k_stated));
    MOQ_TEST_CHECK(p.sub_len == sizeof(k_stated) &&
                   memcmp(p.sub_props, k_stated, sizeof(k_stated)) == 0);
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_take_new_group_request(s, v, &requested), MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(requested, 0);
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_take_new_group_request(s, v, &requested), MOQ_DONE);

    /* One keyframe, then the status reports the track. */
    moq_rcbuf_t *b = NULL;
    uint8_t d[8]; memset(d, 0x11, sizeof(d));
    MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &b) == MOQ_OK);
    moq_media_send_object_t o; memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o); o.payload = b;
    o.is_sync = true; o.starts_group = true;
    o.presentation_time_us = 1000;
    /* The draft-22 LOC-04 companion takes the timestamp as metadata. */
    moq_media_metadata_t m;
    memset(&m, 0, sizeof(m));
    m.struct_size = sizeof(m);
    moq_loc_metadata_init_sized(&m.loc, sizeof(m.loc));
    m.loc.present = MOQ_LOC_METADATA_HAS_TIMESTAMP;
    m.loc.timestamp = 1000;
    if (ver == MOQ_VERSION_DRAFT_22) o.metadata = &m;
    /* A successful write takes the payload's ownership. */
    moq_result_t wrc = moq_media_sender_write(s, v, &o);
    MOQ_TEST_CHECK_EQ_INT(wrc, MOQ_OK);
    if (wrc != MOQ_OK) moq_rcbuf_decref(b);
    cycle(sp, s, &p, 6);

    moq_track_status_cfg_t ts;
    moq_track_status_cfg_init(&ts);
    ts.track_namespace = (moq_namespace_t){ ns_parts, 2 };
    ts.track_name = MOQ_BYTES_LITERAL("v");
    moq_track_status_handle_t th;
    MOQ_TEST_CHECK_EQ_INT(moq_session_track_status(srv, &ts, moq_simpair_now_us(sp), &th), MOQ_OK);
    ts.track_name = MOQ_BYTES_LITERAL("absent");
    MOQ_TEST_CHECK_EQ_INT(moq_session_track_status(srv, &ts, moq_simpair_now_us(sp), &th), MOQ_OK);
    moq_subscribe_namespace_cfg_t sn;
    moq_subscribe_namespace_cfg_init(&sn);
    sn.track_namespace_prefix = (moq_namespace_t){ ns_parts, 1 };
    sn.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_namespace(srv, &sn, moq_simpair_now_us(sp), &nsh),
                          MOQ_OK);
    cycle(sp, s, &p, 6);
    MOQ_TEST_CHECK_EQ_INT(p.ts_ok, 1);
    MOQ_TEST_CHECK(p.ts_largest);
    /* Draft-16's TRACK_STATUS answer has no Track Properties field. */
    size_t ts_want = ver == MOQ_VERSION_DRAFT_16 ? 0 : sizeof(k_stated);
    MOQ_TEST_CHECK_EQ_SIZE(p.ts_len, ts_want);
    MOQ_TEST_CHECK(ts_want == 0 || memcmp(p.ts_props, k_stated, ts_want) == 0);
    MOQ_TEST_CHECK_EQ_INT(p.ts_err, 1);
    MOQ_TEST_CHECK_EQ_U64(p.ts_err_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    MOQ_TEST_CHECK_EQ_INT(p.nss_err, 1);
    MOQ_TEST_CHECK_EQ_U64(p.nss_err_code, MOQ_REQUEST_ERROR_NOT_SUPPORTED);

    /* An update past the current group asks for another; one naming a group
     * already published does not. The draft-22 companion numbers groups from
     * the wallclock (MSF-01 6.1), so "past" is chosen well above any of it. */
    moq_subscription_update_cfg_t u;
    moq_subscription_update_cfg_init_sized(&u, sizeof(u));
    u.has_new_group_request = true;
    u.new_group_request = UINT64_C(1) << 61;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_subscription(srv, sub, &u, moq_simpair_now_us(sp)),
                          MOQ_OK);
    cycle(sp, s, &p, 6);
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_take_new_group_request(s, v, &requested), MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(requested, UINT64_C(1) << 61);
    MOQ_TEST_CHECK_EQ_INT(moq_media_sender_take_new_group_request(NULL, v, NULL),
                          MOQ_ERR_INVAL);

    MOQ_TEST_CHECK(!moq_media_sender_is_fatal(s));
    MOQ_TEST_CHECK(moq_session_state(srv) == MOQ_SESS_ESTABLISHED);
    moq_media_sender_test_free(s);
    { moq_event_t e; while (moq_session_poll_events(moq_simpair_client(sp), &e, 1) == 1)
        moq_event_cleanup(&e);
      while (moq_session_poll_events(srv, &e, 1) == 1)
        moq_event_cleanup(&e); }
    moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
}

int main(void)
{
    test_sender(MOQ_VERSION_DRAFT_16);
    test_sender(MOQ_VERSION_DRAFT_18);
    test_sender(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_PASS("media_sender_track_status");
    return failures != 0;
}
