/*
 * Late joiner on draft 22: the catalog bootstrap is the section 3.5 join at
 * the current group -- a SUBSCRIBE whose Location Filter starts at the Next
 * Object plus a fill whose Location filter is the current group from its
 * start -- and the retained catalog is rebuilt from that fill alone.
 *
 * A PRODUCTION media_sender (pull mode: advertise and answer) publishes its
 * catalog and installs it as the retained group BEFORE the receiver exists;
 * refresh is disabled and nothing is published again, so no later catalog
 * publication can rescue the receiver. A PRODUCTION media_receiver then
 * joins over the same simulated pair and must reach CATALOG_READY and
 * TRACK_ADDED from the fill, never issuing a Joining FETCH. Media then
 * flows on the auto-subscribed track (its own current-group fill is failed
 * on the wire by an origin that retains no media, which leaves the
 * subscription untouched), the finite track ends, and the receiver sees
 * TRACK_ENDED: the publisher's PUBLISH_DONE Stream Count must therefore have
 * counted the fill fetch stream the session opened alongside the facade's
 * own subgroup, or the subscriber's stream gate would hold the done back.
 *
 * The same flow runs with default configurations from the pointer-only
 * initializers and objects without a metadata tail -- a caller that predates
 * the companion selection, writing presentation times from 0 and no capture
 * time: AUTO resolves both ends to the draft-22 companion, and the sender
 * generates the LOC-04 block from the typed fields. A presentation time is
 * media time, so it goes out with Timescale 1000000 (LOC-04 2.3.1.2) and the
 * receiver reports it as application media time, never as a capture clock.
 */
#include <moq/media_sender.h>
#include <moq/media_receiver.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include <moq/session.h>
#include "test_support.h"
#include "test_alloc_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static int failures = 0;

/* -- media_sender test seams (media_sender.c, MOQ_MEDIA_SENDER_TESTING) -- */
moq_media_sender_t *moq_media_sender_test_new_cfg(
    const moq_media_sender_cfg_t *cfg);
void moq_media_sender_test_pump(moq_media_sender_t *s,
                                moq_session_t *session, uint64_t now_us);
void moq_media_sender_test_free(moq_media_sender_t *s);

/* -- media_receiver test seams (media_receiver.c) -------------------------- */
moq_media_receiver_t *moq_media_receiver_test_new_cfg(
    const moq_media_receiver_cfg_t *cfg);
void moq_media_receiver_test_destroy_with_session(moq_media_receiver_t *r,
                                                  moq_session_t *session, uint64_t now_us);
void moq_media_receiver_test_pump(moq_media_receiver_t *r,
                                  moq_session_t *session, uint64_t now_us);
bool moq_media_receiver_test_catalog_fetch_pending(const moq_media_receiver_t *r);
bool moq_media_receiver_test_catalog_fill_pending(const moq_media_receiver_t *r);
bool moq_media_receiver_test_joins_by_fill(const moq_media_receiver_t *r);

static moq_bytes_t g_ns_parts[2];

static moq_simpair_t *pair(moq_alloc_t *alloc, moq_version_t ver)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = alloc; cfg.seed = 42; cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    cfg.version = ver;
    moq_simpair_t *sp = NULL;
    moq_result_t rc = moq_simpair_create(&cfg, &sp);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) return NULL;
    rc = moq_simpair_start(sp);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) { moq_simpair_destroy(sp); return NULL; }
    rc = moq_simpair_run_until_quiescent(sp, 8, NULL);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) { moq_simpair_destroy(sp); return NULL; }
    moq_event_t ev;
    if (moq_session_poll_events(moq_simpair_client(sp), &ev, 1) == 1)
        moq_event_cleanup(&ev);
    if (moq_session_poll_events(moq_simpair_server(sp), &ev, 1) == 1)
        moq_event_cleanup(&ev);
    return sp;
}

static moq_media_track_t *add_video(moq_media_sender_t *s)
{
    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init(&tc);
    tc.name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec = (moq_bytes_t){ (const uint8_t *)"av01", 4 };
    tc.bitrate = 1500000;
    /* Finite media: a VOD track with a declared duration. */
    tc.is_live = false;
    tc.has_track_duration = true;
    tc.track_duration_ms = 2000;
    moq_media_track_t *t = NULL;
    moq_result_t rc = moq_media_sender_add_track(s, &tc, &t);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    return rc == MOQ_OK ? t : NULL;
}

/* Pump the sender hook and settle the link. */
static bool pump_sender(moq_media_sender_t *s, moq_simpair_t *sp, uint64_t now)
{
    moq_media_sender_test_pump(s, moq_simpair_client(sp), now);
    moq_result_t rc = moq_simpair_run_until_quiescent(sp, 8, NULL);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    return rc == MOQ_OK;
}

static bool pump_both(moq_media_sender_t *s, moq_media_receiver_t *r,
                      moq_simpair_t *sp, uint64_t now)
{
    if (!pump_sender(s, sp, now)) return false;
    moq_media_receiver_test_pump(r, moq_simpair_server(sp), now);
    moq_result_t rc = moq_simpair_run_until_quiescent(sp, 8, NULL);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    return rc == MOQ_OK;
}

static bool write_frame(moq_media_sender_t *s, moq_media_track_t *t,
                        const moq_alloc_t *alloc, bool sync, bool starts,
                        bool ends, uint64_t pts_us, bool with_metadata)
{
    static const uint8_t bytes[4] = { 1, 2, 3, 4 };
    moq_rcbuf_t *p = NULL;
    moq_result_t rc = moq_rcbuf_create(alloc, bytes, sizeof(bytes), &p);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) return false;
    moq_media_send_object_t o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.payload = p;
    o.is_sync = sync;
    o.starts_group = starts;
    o.ends_group = ends;
    o.presentation_time_us = pts_us;
    moq_media_metadata_t metadata = {0};
    metadata.struct_size = sizeof(metadata);
    moq_loc_metadata_init_sized(&metadata.loc, sizeof(metadata.loc));
    /* No Timescale: explicitly UNIX microseconds, not a PTS fallback. */
    metadata.loc.present = MOQ_LOC_METADATA_HAS_TIMESTAMP | MOQ_LOC_METADATA_HAS_MARKING;
    metadata.loc.timestamp = pts_us;
    metadata.loc.marking_len = 1;
    metadata.loc.marking[0] = sync ? 0x20 : 0;
    o.metadata = &metadata;
    if (!with_metadata) o.struct_size = MOQ_MEDIA_SEND_OBJECT_V0_SIZE;
    /* A successful write takes the caller's payload ref. */
    rc = moq_media_sender_write(s, t, &o);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) moq_rcbuf_decref(p);
    return rc == MOQ_OK;
}

static void test_late_joiner_fill_d22(bool defaults)
{
    test_alloc_state_t as = {0};
    moq_alloc_t alloc = test_allocator(&as);
    moq_simpair_t *sp = pair(&alloc, MOQ_VERSION_DRAFT_22);
    if (!sp) { MOQ_TEST_CHECK(as.balance == 0); return; }
    moq_session_t *srv = moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);
    moq_media_receiver_t *r = NULL;

    g_ns_parts[0] = MOQ_BYTES_LITERAL("svc");
    g_ns_parts[1] = MOQ_BYTES_LITERAL("demo");
    moq_media_sender_cfg_t cfg;
    if (defaults) {
        /* v0 stamp: pull mode and no timed refresh are the absent-tail
         * defaults, and so is the AUTO companion. */
        moq_media_sender_cfg_init_live(&cfg);
    } else {
        moq_media_sender_cfg_init_live_sized(&cfg, sizeof(cfg));
        cfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
        cfg.publish_tracks = false;                    /* pull: advertise + answer */
        cfg.catalog_refresh_interval_us = UINT64_MAX;  /* no later publication */
    }
    cfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    moq_media_sender_t *s = moq_media_sender_test_new_cfg(&cfg);
    MOQ_TEST_CHECK(s != NULL);
    if (!s) goto cleanup;
    moq_media_track_t *video = add_video(s);
    if (!video) goto cleanup;

    /* The namespace is accepted by the peer's raw session (the receiver does
     * not exist yet); the sender installs its retained catalog and becomes
     * ready. From here on nothing publishes a catalog again. */
    moq_event_t ev;
    for (int cycle = 0; cycle < 12 && !moq_media_sender_is_ready(s); cycle++) {
        if (!pump_sender(s, sp, now)) goto cleanup;
        while (moq_session_poll_events(srv, &ev, 1) == 1) {
            if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
                moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
                moq_result_t arc = moq_session_accept_namespace(srv,
                    ev.u.namespace_published.ann, &ac, now);
                MOQ_TEST_CHECK(arc == MOQ_OK);
                if (arc != MOQ_OK) { moq_event_cleanup(&ev); goto cleanup; }
            }
            moq_event_cleanup(&ev);
        }
        if (!pump_sender(s, sp, now)) goto cleanup;
    }
    MOQ_TEST_CHECK(moq_media_sender_is_ready(s));
    if (!moq_media_sender_is_ready(s)) goto cleanup;

    /* The late joiner. */
    moq_media_receiver_cfg_t rcfg;
    if (defaults) {
        moq_media_receiver_cfg_init_live(&rcfg);
    } else {
        moq_media_receiver_cfg_init_live_sized(&rcfg, sizeof(rcfg));
        rcfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
    }
    rcfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    rcfg.auto_subscribe = true;
    r = moq_media_receiver_test_new_cfg(&rcfg);
    MOQ_TEST_CHECK(r != NULL);
    if (!r) goto cleanup;

    bool catalog_ready = false, track_added = false;
    moq_media_track_t *rtrack = NULL;
    for (int cycle = 0; cycle < 16 && !(catalog_ready && track_added); cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {
            if (te.kind == MOQ_MEDIA_CATALOG_READY) catalog_ready = true;
            if (te.kind == MOQ_MEDIA_TRACK_ADDED) { track_added = true; rtrack = te.track; }
        }
    }
    MOQ_TEST_CHECK(moq_media_receiver_test_joins_by_fill(r));
    MOQ_TEST_CHECK(catalog_ready);
    MOQ_TEST_CHECK(track_added);
    MOQ_TEST_CHECK(!moq_media_receiver_test_catalog_fetch_pending(r));
    MOQ_TEST_CHECK(!moq_media_receiver_test_catalog_fill_pending(r));
    MOQ_TEST_CHECK(!moq_media_receiver_is_fatal(r));
    if (rtrack) {
        const moq_media_track_desc_t *d = moq_media_track_desc_get(rtrack);
        MOQ_TEST_CHECK(d != NULL);
        if (d) {
            MOQ_TEST_CHECK(!d->is_live);
            MOQ_TEST_CHECK(d->name.len == 1 && d->name.data[0] == 'v');
        }
    }
    if (!catalog_ready || !track_added || !rtrack || moq_media_receiver_is_fatal(r)) goto cleanup;

    /* Media on the auto-subscribed track: two frames of one group, with the
     * LOC properties written and read in the draft-22 revision. */
    for (int cycle = 0; cycle < 8 && !moq_media_sender_track_has_subscriber(s, video); cycle++)
        if (!pump_both(s, r, sp, now)) goto cleanup;
    MOQ_TEST_CHECK(moq_media_sender_track_has_subscriber(s, video));
    if (!moq_media_sender_track_has_subscriber(s, video)) goto cleanup;
    const uint64_t t0 = defaults ? 0u : 1000000u, t1 = defaults ? 33333u : 1033333u;
    if (!write_frame(s, video, &alloc, true, true, false, t0, !defaults)) goto cleanup;
    if (!write_frame(s, video, &alloc, false, false, true, t1, !defaults)) goto cleanup;
    int objects = 0;
    for (int cycle = 0; cycle < 12 && objects < 2; cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {}
        moq_media_object_t obj;
        while (moq_media_receiver_poll_object(r, &obj, sizeof(obj)) == MOQ_OK) {
            const uint64_t t = objects == 0 ? t0 : t1;
            MOQ_TEST_CHECK(obj.track == rtrack);
            MOQ_TEST_CHECK_EQ_SIZE(obj.payload.len, 4);
            MOQ_TEST_CHECK(obj.struct_size == sizeof(obj));
            MOQ_TEST_CHECK(obj.metadata.struct_size == sizeof(obj.metadata));
            if (defaults) {
                /* Media time with an explicit microsecond scale. */
                MOQ_TEST_CHECK(!obj.has_capture_time && obj.capture_time_us == 0);
                MOQ_TEST_CHECK(obj.metadata.loc.present ==
                    (MOQ_LOC_METADATA_HAS_TIMESTAMP | MOQ_LOC_METADATA_HAS_TIMESCALE |
                     MOQ_LOC_METADATA_HAS_MARKING));
                MOQ_TEST_CHECK_EQ_U64(obj.metadata.loc.timescale, 1000000u);
                MOQ_TEST_CHECK(obj.metadata.time_domain ==
                               MOQ_MEDIA_TIME_DOMAIN_APPLICATION_MEDIA);
            } else {
                MOQ_TEST_CHECK(obj.has_capture_time);
                MOQ_TEST_CHECK_EQ_U64(obj.capture_time_us, t);
                MOQ_TEST_CHECK(obj.metadata.loc.present ==
                    (MOQ_LOC_METADATA_HAS_TIMESTAMP | MOQ_LOC_METADATA_HAS_MARKING));
                MOQ_TEST_CHECK(obj.metadata.time_domain == MOQ_MEDIA_TIME_DOMAIN_UNIX_US);
            }
            MOQ_TEST_CHECK(obj.metadata.has_time_us && obj.metadata.independence_known);
            MOQ_TEST_CHECK_EQ_U64(obj.metadata.time_us, t);
            MOQ_TEST_CHECK_EQ_U64(obj.metadata.loc.timestamp, t);
            MOQ_TEST_CHECK_EQ_U64(obj.decode_time_us, t);
            MOQ_TEST_CHECK_EQ_U64(obj.presentation_time_us, t);
            MOQ_TEST_CHECK(obj.metadata.loc.marking_len == 1);
            MOQ_TEST_CHECK(obj.metadata.loc.marking[0] == (objects == 0 ? 0x20 : 0));
            MOQ_TEST_CHECK(obj.metadata.independent == (objects == 0));
            MOQ_TEST_CHECK(obj.keyframe == (objects == 0));
            MOQ_TEST_CHECK(obj.track_properties_ref == NULL);
            objects++;
            moq_media_object_cleanup(&obj);
        }
    }
    MOQ_TEST_CHECK_EQ_INT(objects, 2);
    if (objects != 2) {
        moq_media_sender_stats_t ss;
        moq_media_receiver_stats_t rs;
        moq_result_t sr = moq_media_sender_get_stats(s, &ss, sizeof(ss));
        moq_result_t rr = moq_media_receiver_get_stats(r, &rs, sizeof(rs));
        MOQ_TEST_CHECK(sr == MOQ_OK && rr == MOQ_OK);
        if (sr == MOQ_OK && rr == MOQ_OK)
            fprintf(stderr, "D22 media: sender written=%llu sent=%llu queued=%llu; receiver received=%llu parse_drops=%llu\n",
                (unsigned long long)ss.objects_written, (unsigned long long)ss.objects_sent,
                (unsigned long long)ss.objects_queued, (unsigned long long)rs.objects_received,
                (unsigned long long)rs.parse_drops);
    }

    /* The track ends. The publisher's done carries its exact Stream Count
     * (one subgroup it opened plus the fill fetch stream the session opened
     * and failed); the subscriber's stream gate releases the done promptly
     * and the receiver surfaces TRACK_ENDED. */
    moq_result_t erc = moq_media_sender_end_track(s, video);
    MOQ_TEST_CHECK(erc == MOQ_OK);
    if (erc != MOQ_OK) goto cleanup;
    bool ended = false;
    for (int cycle = 0; cycle < 16 && !ended; cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK)
            if (te.kind == MOQ_MEDIA_TRACK_ENDED && te.track == rtrack) ended = true;
        moq_media_object_t obj;
        while (moq_media_receiver_poll_object(r, &obj, sizeof(obj)) == MOQ_OK)
            moq_media_object_cleanup(&obj);
    }
    MOQ_TEST_CHECK(ended);
    MOQ_TEST_CHECK(!moq_media_receiver_is_fatal(r));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(srv), (int)MOQ_SESS_ESTABLISHED);

cleanup:
    if (r) moq_media_receiver_test_destroy_with_session(r, srv, now);
    moq_media_sender_test_free(s);
    moq_event_t e;
    while (moq_session_poll_events(moq_simpair_client(sp), &e, 1) == 1)
        moq_event_cleanup(&e);
    while (moq_session_poll_events(srv, &e, 1) == 1)
        moq_event_cleanup(&e);
    moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
    MOQ_TEST_PASS(defaults ? "late_joiner_fill_d22_defaults_auto" : "late_joiner_fill_d22");
}

/* The same late join on draft 18 still goes through the Joining FETCH, so the
 * two bootstrap paths are selected by the session, not by the receiver. */
static void test_late_joiner_fetch_d18(void)
{
    test_alloc_state_t as = {0};
    moq_alloc_t alloc = test_allocator(&as);
    moq_simpair_t *sp = pair(&alloc, MOQ_VERSION_DRAFT_18);
    if (!sp) { MOQ_TEST_CHECK(as.balance == 0); return; }
    moq_session_t *srv = moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);
    moq_media_receiver_t *r = NULL;

    g_ns_parts[0] = MOQ_BYTES_LITERAL("svc");
    g_ns_parts[1] = MOQ_BYTES_LITERAL("demo");
    moq_media_sender_cfg_t cfg;
    moq_media_sender_cfg_init_live_sized(&cfg, sizeof(cfg));
    cfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    cfg.publish_tracks = false;
    cfg.catalog_refresh_interval_us = UINT64_MAX;
    moq_media_sender_t *s = moq_media_sender_test_new_cfg(&cfg);
    MOQ_TEST_CHECK(s != NULL);
    if (!s || !add_video(s)) goto cleanup_d18;
    moq_event_t ev;
    for (int cycle = 0; cycle < 12 && !moq_media_sender_is_ready(s); cycle++) {
        if (!pump_sender(s, sp, now)) goto cleanup_d18;
        while (moq_session_poll_events(srv, &ev, 1) == 1) {
            if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
                moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
                moq_result_t arc = moq_session_accept_namespace(srv,
                    ev.u.namespace_published.ann, &ac, now);
                MOQ_TEST_CHECK(arc == MOQ_OK);
                if (arc != MOQ_OK) { moq_event_cleanup(&ev); goto cleanup_d18; }
            }
            moq_event_cleanup(&ev);
        }
        if (!pump_sender(s, sp, now)) goto cleanup_d18;
    }
    MOQ_TEST_CHECK(moq_media_sender_is_ready(s));
    if (!moq_media_sender_is_ready(s)) goto cleanup_d18;

    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live(&rcfg);
    rcfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    rcfg.auto_subscribe = false;
    r = moq_media_receiver_test_new_cfg(&rcfg);
    MOQ_TEST_CHECK(r != NULL);
    if (!r) goto cleanup_d18;
    bool catalog_ready = false;
    for (int cycle = 0; cycle < 16 && !catalog_ready; cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup_d18;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK)
            if (te.kind == MOQ_MEDIA_CATALOG_READY) catalog_ready = true;
    }
    MOQ_TEST_CHECK(!moq_media_receiver_test_joins_by_fill(r));
    MOQ_TEST_CHECK(catalog_ready);
    MOQ_TEST_CHECK(!moq_media_receiver_test_catalog_fill_pending(r));

cleanup_d18:
    if (r) moq_media_receiver_test_destroy_with_session(r, srv, now);
    moq_media_sender_test_free(s);
    moq_event_t e;
    while (moq_session_poll_events(moq_simpair_client(sp), &e, 1) == 1)
        moq_event_cleanup(&e);
    while (moq_session_poll_events(srv, &e, 1) == 1)
        moq_event_cleanup(&e);
    moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
    MOQ_TEST_PASS("late_joiner_fetch_d18");
}

/* -- A Track Properties Timescale and the typed capture time -------------
 * Draft 22, default (AUTO) sender and receiver. Track "v" declares LOC-04
 * Timescale 48000 in its Track Properties (Track scope, LOC-04 2.3.1.2), which
 * the receiver applies to every object without its own Timescale. A typed
 * capture time on it would be sent as an unscaled Timestamp and read as media
 * time at 48000, so the sender refuses it; the receiver must never see a
 * capture time in the media-time domain. Controls on the same session: the
 * presentation-time form carries its own Timescale 1000000; an explicit
 * metadata tail goes out unchanged and inherits the track scale; a capture
 * time on track "u", without Track Properties, stays Unix time. */

enum { KIND_CAPTURE = 1, KIND_PTS, KIND_EXPLICIT, KIND_OTHER_CAPTURE };

static moq_result_t write_kind(moq_media_sender_t *s, moq_media_track_t *t,
                               const moq_alloc_t *alloc, uint8_t kind)
{
    const uint8_t bytes[4] = { kind, 2, 3, 4 };
    moq_rcbuf_t *p = NULL;
    moq_result_t rc = moq_rcbuf_create(alloc, bytes, sizeof(bytes), &p);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) return rc;
    moq_media_send_object_t o;
    memset(&o, 0, sizeof(o));
    o.struct_size = MOQ_MEDIA_SEND_OBJECT_V0_SIZE;
    o.payload = p;
    o.is_sync = o.starts_group = o.ends_group = true;
    o.presentation_time_us = 0;
    moq_media_metadata_t m = {0};
    if (kind == KIND_CAPTURE || kind == KIND_OTHER_CAPTURE) {
        o.has_capture_time = true;
        o.capture_time_us = 48000;
    } else if (kind == KIND_EXPLICIT) {
        m.struct_size = sizeof(m);
        moq_loc_metadata_init_sized(&m.loc, sizeof(m.loc));
        m.loc.present = MOQ_LOC_METADATA_HAS_TIMESTAMP;
        m.loc.timestamp = 96000;
        o.struct_size = sizeof(o);
        o.metadata = &m;
    }
    rc = moq_media_sender_write(s, t, &o);
    if (rc != MOQ_OK) moq_rcbuf_decref(p);
    return rc;
}

static moq_media_track_t *add_raw(moq_media_sender_t *s, const char *name,
                                  bool audio, const uint8_t *props, size_t n)
{
    uint8_t copy[8];
    if (n) memcpy(copy, props, n);
    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init_sized(&tc, sizeof(tc));
    tc.name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    tc.media_type = audio ? MOQ_MEDIA_TYPE_AUDIO : MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec = audio ? (moq_bytes_t){ (const uint8_t *)"opus", 4 }
                     : (moq_bytes_t){ (const uint8_t *)"av01", 4 };
    tc.bitrate = 1500000;
    if (audio) {
        tc.samplerate = 48000;
        tc.channel_config = (moq_bytes_t){ (const uint8_t *)"2", 1 };
    }
    tc.track_properties = (moq_bytes_t){ copy, n };
    moq_media_track_t *t = NULL;
    moq_result_t rc = moq_media_sender_add_track(s, &tc, &t);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    memset(copy, 0xa5, sizeof(copy));   /* the registration copy is used */
    return rc == MOQ_OK ? t : NULL;
}

static void test_track_timescale_capture_d22(bool audio)
{
    static const uint8_t scale48k[] = { 0x08, 0xc0, 0xbb, 0x80 };
    test_alloc_state_t as = {0};
    moq_alloc_t alloc = test_allocator(&as);
    moq_simpair_t *sp = pair(&alloc, MOQ_VERSION_DRAFT_22);
    if (!sp) { MOQ_TEST_CHECK(as.balance == 0); return; }
    moq_session_t *srv = moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);
    moq_media_receiver_t *r = NULL;
    g_ns_parts[0] = MOQ_BYTES_LITERAL("svc");
    g_ns_parts[1] = MOQ_BYTES_LITERAL("scale");
    moq_media_sender_cfg_t cfg;
    moq_media_sender_cfg_init_live(&cfg);
    cfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    moq_media_sender_t *s = moq_media_sender_test_new_cfg(&cfg);
    MOQ_TEST_CHECK(s != NULL);
    moq_media_track_t *v = s ? add_raw(s, "v", audio, scale48k, sizeof(scale48k)) : NULL;
    moq_media_track_t *u = s ? add_raw(s, "u", audio, NULL, 0) : NULL;
    if (!v || !u) goto cleanup;
    moq_event_t ev;
    for (int cycle = 0; cycle < 12 && !moq_media_sender_is_ready(s); cycle++) {
        if (!pump_sender(s, sp, now)) goto cleanup;
        while (moq_session_poll_events(srv, &ev, 1) == 1) {
            if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
                moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
                MOQ_TEST_CHECK(moq_session_accept_namespace(srv,
                    ev.u.namespace_published.ann, &ac, now) == MOQ_OK);
            }
            moq_event_cleanup(&ev);
        }
        if (!pump_sender(s, sp, now)) goto cleanup;
    }
    MOQ_TEST_CHECK(moq_media_sender_is_ready(s));
    if (!moq_media_sender_is_ready(s)) goto cleanup;

    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live(&rcfg);
    rcfg.namespace_ = (moq_namespace_t){ g_ns_parts, 2 };
    rcfg.auto_subscribe = true;
    r = moq_media_receiver_test_new_cfg(&rcfg);
    MOQ_TEST_CHECK(r != NULL);
    if (!r) goto cleanup;
    moq_media_track_t *rv = NULL, *ru = NULL;
    for (int cycle = 0; cycle < 24 && !(moq_media_sender_track_has_subscriber(s, v) &&
                                         moq_media_sender_track_has_subscriber(s, u));
         cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {
            if (te.kind != MOQ_MEDIA_TRACK_ADDED) continue;
            if (te.desc->name.len == 1 && te.desc->name.data[0] == 'v') rv = te.track;
            if (te.desc->name.len == 1 && te.desc->name.data[0] == 'u') ru = te.track;
        }
    }
    MOQ_TEST_CHECK(rv && ru);
    MOQ_TEST_CHECK(moq_media_sender_track_has_subscriber(s, v) &&
                   moq_media_sender_track_has_subscriber(s, u));
    if (!rv || !ru) goto cleanup;

    moq_result_t rc = write_kind(s, v, &alloc, KIND_CAPTURE);
    if (rc != MOQ_ERR_INVAL)
        fprintf(stderr, "capture on the scaled track: write rc=%d\n", (int)rc);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)write_kind(s, v, &alloc, KIND_PTS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_kind(s, v, &alloc, KIND_EXPLICIT), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_kind(s, u, &alloc, KIND_OTHER_CAPTURE), (int)MOQ_OK);

    unsigned seen[5] = {0};
    for (int cycle = 0; cycle < 16 && seen[KIND_PTS] + seen[KIND_EXPLICIT] +
                                      seen[KIND_OTHER_CAPTURE] < 3; cycle++) {
        if (!pump_both(s, r, sp, now)) goto cleanup;
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {}
        moq_media_object_t obj;
        while (moq_media_receiver_poll_object(r, &obj, sizeof(obj)) == MOQ_OK) {
            uint8_t kind = obj.payload.len == 4 ? obj.payload.data[0] : 0;
            const moq_media_metadata_t *m = &obj.metadata;
            if (kind >= 1 && kind <= 4) seen[kind]++;
            MOQ_TEST_CHECK(obj.track == (kind == KIND_OTHER_CAPTURE ? ru : rv));
            if (obj.track == rv) {
                /* The peer observes the scale in the track's properties. */
                MOQ_TEST_CHECK(obj.track_properties_ref != NULL &&
                    moq_rcbuf_len(obj.track_properties_ref) == sizeof(scale48k) &&
                    memcmp(moq_rcbuf_data(obj.track_properties_ref), scale48k,
                           sizeof(scale48k)) == 0);
            }
            switch (kind) {
            case KIND_CAPTURE:
                /* Never delivered: it would read as media time at 48000. */
                fprintf(stderr, "capture delivered: domain=%u scale=%llu "
                        "capture=%d time_us=%llu\n", (unsigned)m->time_domain,
                        (unsigned long long)m->loc.timescale,
                        (int)obj.has_capture_time, (unsigned long long)m->time_us);
                MOQ_TEST_CHECK(m->time_domain == MOQ_MEDIA_TIME_DOMAIN_UNIX_US &&
                               obj.has_capture_time);
                break;
            case KIND_PTS:
                MOQ_TEST_CHECK(m->time_domain == MOQ_MEDIA_TIME_DOMAIN_APPLICATION_MEDIA);
                MOQ_TEST_CHECK_EQ_U64(m->loc.timescale, 1000000u);
                MOQ_TEST_CHECK(m->has_time_us && m->time_us == 0 && !obj.has_capture_time);
                break;
            case KIND_EXPLICIT:
                MOQ_TEST_CHECK(m->time_domain == MOQ_MEDIA_TIME_DOMAIN_APPLICATION_MEDIA);
                MOQ_TEST_CHECK_EQ_U64(m->loc.timescale, 48000u);
                MOQ_TEST_CHECK(m->has_time_us && m->time_us == 2000000u &&
                               !obj.has_capture_time);
                break;
            case KIND_OTHER_CAPTURE:
                MOQ_TEST_CHECK(m->time_domain == MOQ_MEDIA_TIME_DOMAIN_UNIX_US);
                MOQ_TEST_CHECK(obj.has_capture_time && obj.capture_time_us == 48000u);
                MOQ_TEST_CHECK(!(m->loc.present & MOQ_LOC_METADATA_HAS_TIMESCALE));
                break;
            default:
                MOQ_TEST_CHECK(false);
            }
            moq_media_object_cleanup(&obj);
        }
    }
    MOQ_TEST_CHECK(seen[KIND_CAPTURE] == 0);
    MOQ_TEST_CHECK(seen[KIND_PTS] == 1 && seen[KIND_EXPLICIT] == 1 &&
                   seen[KIND_OTHER_CAPTURE] == 1);
    MOQ_TEST_CHECK(!moq_media_sender_is_fatal(s) && !moq_media_receiver_is_fatal(r));
cleanup:
    if (r) moq_media_receiver_test_destroy_with_session(r, srv, now);
    moq_media_sender_test_free(s);
    moq_event_t e;
    while (moq_session_poll_events(moq_simpair_client(sp), &e, 1) == 1)
        moq_event_cleanup(&e);
    while (moq_session_poll_events(srv, &e, 1) == 1)
        moq_event_cleanup(&e);
    moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
    MOQ_TEST_PASS(audio ? "track_timescale_capture_d22_audio"
                        : "track_timescale_capture_d22_video");
}

int main(void)
{
    test_late_joiner_fill_d22(false);
    test_late_joiner_fill_d22(true);
    test_late_joiner_fetch_d18();
    test_track_timescale_capture_d22(false);
    test_track_timescale_capture_d22(true);
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
