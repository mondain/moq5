/*
 * Request teardowns under a short action queue.
 *
 * A publisher whose subscription or publication the subscriber tears down
 * (RESET_STREAM or STOP_SENDING on the request stream) MUST reset every
 * stream of it still open (draft-22 §3.1.2, §11.3.2). However many subgroup
 * streams are open -- more than the action queue holds -- the teardown
 * completes at once: each reset that fits is queued, each one that does not
 * is owed on its stream and goes out as polling frees capacity. Every reset
 * is sent, exactly once, and none is dropped.
 *
 * A request the peer ends with a bare FIN before the messages it owed
 * (draft-22) fails, and this side resets its own direction of the request
 * stream. A FETCH whose data stream already started also stops that stream.
 * Both actions are reserved together: with room for only one the teardown
 * refuses with nothing changed, and a retry completes it with both.
 *
 * Rejecting a subscriber's update ends the subscription with
 * PUBLISH_DONE(UPDATE_FAILED), and "A sender MUST NOT send PUBLISH_DONE until
 * it has closed all streams it will ever open" (draft-22 §9.9). With more
 * open subgroups than the action queue holds, the reject still completes at
 * once: the REQUEST_ERROR goes out, the resets that fit are queued, the rest
 * are owed, and the PUBLISH_DONE is held until every reset is queued -- so it
 * always follows them. Every profile; subscriptions and publications.
 *
 * The same holds for a draft-22 subscription, or PUBLISH-established
 * subscription, whose only open sending stream is a fill (draft-22 section
 * 9.9): rejecting its update closes the fill stream (RESET) before the
 * PUBLISH_DONE, also when the action queue has no room for that RESET when
 * the reject is made -- the terminal is then held until polling frees room
 * and the RESET goes first. Another owner's fill is untouched.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <moq/control.h>
#include <moq/control_d18.h>
#include <string.h>

static moq_version_t g_version;
static moq_bytes_t g_parts[] = { { (const uint8_t *)"live", 4 } };

#define SUBGROUPS 12u
#define ACTIONS   8u

static bool g_defer;

static moq_simpair_t *mk_pair(uint32_t max_actions)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.max_actions = max_actions;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    scfg.server_defer_subscription_updates = g_defer;
    scfg.server_max_open_subgroups = 64;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    return sp;
}

static void pump(moq_simpair_t *sp)
{
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static void drain_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

static size_t count_kind(moq_session_t *s, moq_event_kind_t kind)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == kind) n++;
        moq_event_cleanup(&ev);
    }
    return n;
}

/* Open SUBGROUPS subgroup streams (one object each) on the subscription or
 * publication, the queue drained after each so all fit; returns their refs. */
static size_t open_many(moq_session_t *sv, moq_subscription_t sub, moq_publication_t pub,
                        uint64_t *refs)
{
    size_t n = 0;
    for (size_t g = 0; g < SUBGROUPS; g++) {
        moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
        sc.group_id = g;
        moq_subgroup_handle_t h;
        moq_result_t rc = moq_subscription_is_valid(sub)
            ? moq_session_open_subgroup(sv, sub, &sc, 3, &h)
            : moq_session_open_pub_subgroup(sv, pub, &sc, 3, &h);
        if (rc != MOQ_OK) break;
        moq_rcbuf_t *pl = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
        (void)moq_session_write_object(sv, h, 0, pl, 3);
        moq_rcbuf_decref(pl);
        int slot = (int)moq_handle_slot(h._opaque);
        refs[n++] = sv->subgroups[slot].stream_ref._v;
        drain_actions(sv);
    }
    return n;
}

/* Poll everything, re-polling as owed resets are released; count each
 * stream's CANCELLED reset. */
static int check_all_reset(moq_session_t *sv, const uint64_t *refs, size_t n)
{
    int failures = 0;
    size_t hits[SUBGROUPS] = { 0 };
    size_t other = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_DATA) {
            bool found = false;
            for (size_t k = 0; k < n; k++)
                if (refs[k] == a.u.reset_data.stream_ref._v) { hits[k]++; found = true; }
            if (!found) other++;
            MOQ_TEST_CHECK_EQ_U64(a.u.reset_data.error_code, 0x1);
        }
        moq_action_cleanup(&a);
    }
    for (size_t k = 0; k < n; k++) MOQ_TEST_CHECK_EQ_SIZE(hits[k], 1);
    MOQ_TEST_CHECK_EQ_SIZE(other, 0);
    for (size_t i = 0; i < sv->sg_cap; i++)
        MOQ_TEST_CHECK(sv->subgroups[i].state != MOQ_SG_RESET_OWED);
    return failures;
}

static int test_subscription_teardown(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t ch, sh = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) sh = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 2), (int)MOQ_OK);
    pump(sp);
    drain(sv);
    uint64_t refs[SUBGROUPS];
    size_t n = open_many(sv, sh, MOQ_PUBLICATION_INVALID, refs);
    MOQ_TEST_CHECK_EQ_SIZE(n, SUBGROUPS);
    int ss = sub_resolve_handle(sv, sh);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t ref = sv->subs[ss].rs.ref;
    moq_result_t rc = reset ? moq_session_on_bidi_stream_reset(sv, ref, 0x7, 4)
                            : moq_session_on_bidi_stream_stop(sv, ref, 0x7, 4);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(sv, MOQ_EVENT_UNSUBSCRIBED), 1);
    failures += check_all_reset(sv, refs, n);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_publication_teardown(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t sph, cph = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(sv, &cfg, 1, &sph), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) cph = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
    }
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(c, cph, &ac, 2), (int)MOQ_OK);
    pump(sp);
    drain(sv);
    uint64_t refs[SUBGROUPS];
    size_t n = open_many(sv, MOQ_SUBSCRIPTION_INVALID, sph, refs);
    MOQ_TEST_CHECK_EQ_SIZE(n, SUBGROUPS);
    int ps = pub_resolve_handle(sv, sph);
    MOQ_TEST_CHECK(ps >= 0);
    if (ps < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t ref = sv->publishes[ps].rs.ref;
    moq_result_t rc = reset ? moq_session_on_bidi_stream_reset(sv, ref, 0x7, 4)
                            : moq_session_on_bidi_stream_stop(sv, ref, 0x7, 4);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(sv, MOQ_EVENT_PUBLISH_UNSUBSCRIBED), 1);
    failures += check_all_reset(sv, refs, n);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Fill `s`'s action queue until `free_slots` remain, with SUBSCRIBEs. */
static void occupy(moq_session_t *s, size_t free_slots)
{
    char name[8];
    for (int k = 0; k < 64 && action_queue_avail(s) > free_slots; k++) {
        moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
        snprintf(name, sizeof(name), "o%d", k);
        cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
        cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
        moq_subscription_t h;
        if (moq_session_subscribe(s, &cfg, 4, &h) != MOQ_OK) break;
    }
}

/* Draft-22 fetcher: the publisher's FIN arrives before FETCH_OK, after the
 * data stream's header. The STOP of the data stream and the reset of our
 * direction of the request stream are reserved together. */
static int test_fetch_fin_reserves_both(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_fetch_cfg_t cfg; moq_fetch_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.end_group = 4;
    moq_fetch_t fh, sf = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &cfg, 1, &fh), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) sf = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 3;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_fetch(sv, sf, &ac, 3), (int)MOQ_OK);
    /* Only the data stream's header reaches the fetcher. */
    uint8_t hdr[64]; size_t hl = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA && hl == 0) {
            memcpy(hdr, a.u.send_data.header, a.u.send_data.header_len);
            hl = a.u.send_data.header_len;
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(hl > 0);
    moq_stream_ref_t data_ref = moq_stream_ref_from_u64(0x9001);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_bytes(c, data_ref, hdr, hl, false, 4),
                          (int)MOQ_OK);
    int fs = -1;
    for (size_t i = 0; i < c->fetch_cap; i++)
        if (c->fetches[i].rs.ref._v != 0) fs = (int)i;
    MOQ_TEST_CHECK(fs >= 0);
    if (fs < 0) { moq_simpair_destroy(sp); return failures; }
    MOQ_TEST_CHECK(c->fetches[fs].data_stream_started);
    moq_stream_ref_t cref = c->fetches[fs].rs.ref;
    drain(c);
    drain_actions(c);

    occupy(c, 1);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(c), 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 5),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(c), 1);
    MOQ_TEST_CHECK(moq_fetch_is_valid(fh) && c->fetches[fs].rs.ref._v == cref._v);

    drain_actions(c);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 6),
                          (int)MOQ_OK);
    bool stopped = false, reset_own = false;
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_STOP_DATA && a.u.stop_data.stream_ref._v == data_ref._v)
            stopped = true;
        if (a.kind == MOQ_ACTION_RESET_BIDI_STREAM &&
            a.u.reset_bidi_stream.stream_ref._v == cref._v)
            reset_own = true;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(stopped);
    MOQ_TEST_CHECK(reset_own);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(c, MOQ_EVENT_FETCH_RESET), 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-22 publisher: the subscriber FINs its PUBLISH stream before
 * PUBLISH_OK. The teardown and the reset of our direction are reserved
 * together. */
static int test_publish_fin_reserves_both(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t ph;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, &ph), (int)MOQ_OK);
    pump(sp);
    drain(c); drain(sv);
    int ps = pub_resolve_handle(c, ph);
    MOQ_TEST_CHECK(ps >= 0);
    if (ps < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t cref = c->publishes[ps].rs.ref;
    occupy(c, 0);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(c), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 5),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(pub_resolve_handle(c, ph) >= 0);
    drain_actions(c);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 6),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK(pub_resolve_handle(c, ph) < 0);
    bool reset_own = false;
    moq_action_t a;
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_BIDI_STREAM &&
            a.u.reset_bidi_stream.stream_ref._v == cref._v)
            reset_own = true;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(reset_own);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(c, MOQ_EVENT_PUBLISH_UNSUBSCRIBED), 1);
    moq_simpair_destroy(sp);
    return failures;
}

/* The message types in one SEND_CONTROL / SEND_BIDI_STREAM payload. */
static size_t msg_types(const uint8_t *data, size_t len, uint64_t *types, size_t cap)
{
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, data, len);
    size_t n = 0;
    while (r.pos < r.len && n < cap) {
        moq_control_envelope_t env;
        moq_result_t rc = g_version == MOQ_VERSION_DRAFT_16 ? moq_control_decode_envelope(&r, &env)
                        : g_version == MOQ_VERSION_DRAFT_18 ? moq_d18_decode_envelope(&r, &env)
                        : moq_d22_decode_envelope(&r, &env);
        if (rc != MOQ_OK) break;
        types[n++] = env.msg_type;
    }
    return n;
}

/* Poll everything; check one CANCELLED reset per stream, then exactly one
 * PUBLISH_DONE after the last of them. */
static int check_resets_then_done(moq_session_t *sv, const uint64_t *refs, size_t n)
{
    int failures = 0;
    size_t hits[SUBGROUPS] = { 0 };
    long idx = 0, last_reset = -1, done_at = -1, error_at = -1;
    size_t dones = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_DATA) {
            for (size_t k = 0; k < n; k++)
                if (refs[k] == a.u.reset_data.stream_ref._v) hits[k]++;
            MOQ_TEST_CHECK_EQ_U64(a.u.reset_data.error_code, 0x1);
            last_reset = idx;
        }
        const uint8_t *d = NULL; size_t dl = 0;
        if (a.kind == MOQ_ACTION_SEND_CONTROL) { d = a.u.send_control.data; dl = a.u.send_control.len; }
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) { d = a.u.send_bidi_stream.data; dl = a.u.send_bidi_stream.len; }
        if (d) {
            uint64_t t[8];
            size_t nt = msg_types(d, dl, t, 8);
            for (size_t k = 0; k < nt; k++) {
                if (t[k] == 0x0B) { dones++; done_at = idx; }
                if (t[k] == 0x05 && error_at < 0) error_at = idx;
            }
        }
        idx++;
        moq_action_cleanup(&a);
    }
    for (size_t k = 0; k < n; k++) MOQ_TEST_CHECK_EQ_SIZE(hits[k], 1);
    MOQ_TEST_CHECK_EQ_SIZE(dones, 1);
    MOQ_TEST_CHECK(error_at >= 0 && error_at < done_at);
    MOQ_TEST_CHECK(last_reset >= 0 && last_reset < done_at);
    for (size_t i = 0; i < sv->sg_cap; i++)
        MOQ_TEST_CHECK(sv->subgroups[i].state != MOQ_SG_RESET_OWED);
    return failures;
}

static int test_reject_subscription_update_ex(bool peer_resets);
static int test_reject_subscription_update(void)
{
    return test_reject_subscription_update_ex(false);
}

/* Poll everything; check one CANCELLED reset per stream and no PUBLISH_DONE. */
static int check_resets_no_done(moq_session_t *sv, const uint64_t *refs, size_t n)
{
    int failures = 0;
    size_t hits[SUBGROUPS] = { 0 };
    size_t dones = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_DATA)
            for (size_t k = 0; k < n; k++)
                if (refs[k] == a.u.reset_data.stream_ref._v) hits[k]++;
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            uint64_t t[8];
            size_t nt = msg_types(a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, t, 8);
            for (size_t k = 0; k < nt; k++) if (t[k] == 0x0B) dones++;
        }
        moq_action_cleanup(&a);
    }
    for (size_t k = 0; k < n; k++) MOQ_TEST_CHECK_EQ_SIZE(hits[k], 1);
    MOQ_TEST_CHECK_EQ_SIZE(dones, 0);
    return failures;
}

static int test_reject_subscription_update_ex(bool peer_resets)
{
    int failures = 0;
    g_defer = true;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    g_defer = false;
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t ch, sh = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) sh = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 2), (int)MOQ_OK);
    pump(sp);
    drain(c); drain(sv);
    uint64_t refs[SUBGROUPS];
    size_t n = open_many(sv, sh, MOQ_PUBLICATION_INVALID, refs);
    MOQ_TEST_CHECK_EQ_SIZE(n, SUBGROUPS);
    moq_subscription_update_cfg_t up;
    moq_subscription_update_cfg_init_sized(&up, sizeof(up));
    up.has_subscriber_priority = true; up.subscriber_priority = 9;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_subscription(c, ch, &up, 3), (int)MOQ_OK);
    pump(sp);
    uint64_t id = 0;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_UPDATED) id = ev.u.subscribe_updated.update_request_id;
        moq_event_cleanup(&ev);
    }
    drain_actions(sv);
    moq_reject_subscription_update_cfg_t rj; moq_reject_subscription_update_cfg_init(&rj);
    rj.update_request_id = id;
    rj.error_code = 0x5;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_subscription_update(sv, sh, &rj, 4),
                          (int)MOQ_OK);
    /* The subscription has ended: nothing more may be answered on it. */
    MOQ_TEST_CHECK(moq_session_reject_subscription_update(sv, sh, &rj, 4) != MOQ_OK);
    if (peer_resets) {
        /* The subscriber abandons the request stream while the terminal is
         * held: it is dropped, and every owed reset still goes out. */
        int ss = sub_resolve_handle(sv, sh);
        MOQ_TEST_CHECK(ss >= 0);
        if (ss >= 0)
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_reset(sv,
                sv->subs[ss].rs.ref, 0x1, 5), (int)MOQ_OK);
        MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
        failures += check_resets_no_done(sv, refs, n);
    } else {
        failures += check_resets_then_done(sv, refs, n);
    }
    MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_reject_publication_update(void)
{
    int failures = 0;
    g_defer = true;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    g_defer = false;
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t sph, cph = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(sv, &cfg, 1, &sph), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) cph = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
    }
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(c, cph, &ac, 2), (int)MOQ_OK);
    pump(sp);
    drain(c); drain(sv);
    uint64_t refs[SUBGROUPS];
    size_t n = open_many(sv, MOQ_SUBSCRIPTION_INVALID, sph, refs);
    MOQ_TEST_CHECK_EQ_SIZE(n, SUBGROUPS);
    moq_publication_update_cfg_t up; moq_publication_update_cfg_init(&up);
    up.has_subscriber_priority = true; up.subscriber_priority = 9;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(c, cph, &up, 3), (int)MOQ_OK);
    pump(sp);
    uint64_t id = 0;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_UPDATED) id = ev.u.publish_updated.update_request_id;
        moq_event_cleanup(&ev);
    }
    drain_actions(sv);
    moq_reject_subscription_update_cfg_t rj; moq_reject_subscription_update_cfg_init(&rj);
    rj.update_request_id = id;
    rj.error_code = 0x5;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_publication_update(sv, sph, &rj, 4),
                          (int)MOQ_OK);
    failures += check_resets_then_done(sv, refs, n);
    MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* The fill table slot of `sv`'s fill handle `sfill` (SIZE_MAX if none). */
static size_t fill_slot_of(const moq_session_t *sv, moq_fill_t sfill)
{
    if (!moq_fill_is_valid(sfill)) return SIZE_MAX;
    for (size_t i = 0; i < sv->fills.capacity; i++)
        if (sv->fill_storage[i].active &&
            sv->fill_side[i].handle._opaque == sfill._opaque)
            return i;
    return SIZE_MAX;
}

/* The fill child's sending stream on `sv` for the publisher-side fill
 * `sfill` (0 when it has none). */
static uint64_t fill_stream_of(const moq_session_t *sv, moq_fill_t sfill)
{
    size_t slot = fill_slot_of(sv, sfill);
    if (slot == SIZE_MAX) return 0;
    const moq_fill_entry_t *e = &sv->fill_storage[slot];
    return e->active && e->stream_bound ? e->stream._v : 0;
}

/* Take the FILL_REQUEST on `sv` (other events dropped), open the fill and
 * write one Object on it; returns its stream ref (0 on failure). */
static uint64_t open_requested_fill(moq_session_t *sv, moq_fill_t *out)
{
    moq_event_t ev;
    bool have = false;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_REQUEST && !have) {
            *out = ev.u.fill_request.fill;
            have = true;
        }
        moq_event_cleanup(&ev);
    }
    if (!have) return 0;
    if (moq_session_open_fill(sv, *out, 3) != MOQ_OK) return 0;
    moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
    oc.group_id = 0; oc.subgroup_id = 0; oc.object_id = 0; oc.publisher_priority = 5;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &oc.payload);
    moq_result_t rc = moq_session_write_fill_object(sv, *out, &oc, 3);
    moq_rcbuf_decref(oc.payload);
    if (rc != MOQ_OK) return 0;
    drain_actions(sv);
    return fill_stream_of(sv, *out);
}

/* A subscription on track `name` with a fill from (0,0), accepted with
 * Largest (3,0); its fill opened with one Object. */
static uint64_t sub_with_open_fill(moq_simpair_t *sp, const char *name,
                                   moq_subscription_t *ch, moq_subscription_t *sh,
                                   moq_fill_t *sfill)
{
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = 0; loc.start_object = 0;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    cfg.fill = &fill;
    *sh = (moq_subscription_t){ 0 };
    if (moq_session_subscribe(c, &cfg, 1, ch) != MOQ_OK) return 0;
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) *sh = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 3; ac.largest_object = 0;
    if (moq_session_accept_subscribe(sv, *sh, &ac, 2) != MOQ_OK) return 0;
    pump(sp);
    drain(c);
    return open_requested_fill(sv, sfill);
}

/* Poll every action of `sv`: the fill stream `fill_ref`'s RESET comes
 * exactly once and before the single PUBLISH_DONE; `healthy_ref` is never
 * reset; no other stream is reset. */
static int check_fill_reset_then_done(moq_session_t *sv, uint64_t fill_ref,
                                      uint64_t healthy_ref)
{
    int failures = 0;
    long idx = 0, fill_reset_at = -1, done_at = -1;
    size_t dones = 0, fill_resets = 0, other_resets = 0, healthy_resets = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_DATA) {
            uint64_t r = a.u.reset_data.stream_ref._v;
            if (r == fill_ref) { fill_resets++; fill_reset_at = idx; }
            else if (r == healthy_ref) healthy_resets++;
            else other_resets++;
        }
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            uint64_t t[8];
            size_t nt = msg_types(a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, t, 8);
            for (size_t k = 0; k < nt; k++)
                if (t[k] == 0x0B) { dones++; done_at = idx; }
        }
        idx++;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(fill_resets, 1);
    MOQ_TEST_CHECK_EQ_SIZE(healthy_resets, 0);
    MOQ_TEST_CHECK_EQ_SIZE(other_resets, 0);
    MOQ_TEST_CHECK_EQ_SIZE(dones, 1);
    MOQ_TEST_CHECK(fill_reset_at >= 0 && fill_reset_at < done_at);
    return failures;
}

/* Draft-22: subscription A's only open sending stream is a fill; B, another
 * subscription, has a healthy open fill of its own. A's update is rejected:
 * - with no free action slot the reject is refused before any effect: A, its
 *   fill, its owed answer and the queue are unchanged;
 * - with exactly the reject's own two slots free it is taken; every action is
 *   then polled with no further peer input: A's fill RESET comes before the
 *   single PUBLISH_DONE, B's fill is never reset and stays open, nothing else
 *   is reset, and nothing is left owed. */
static int test_reject_update_closes_fill_first(void)
{
    int failures = 0;
    g_defer = true;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    g_defer = false;
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscription_t ch, sh, chb, shb;
    moq_fill_t sfill = { 0 }, sfill_b = { 0 };
    uint64_t fill_ref = sub_with_open_fill(sp, "v", &ch, &sh, &sfill);
    uint64_t healthy_ref = sub_with_open_fill(sp, "w", &chb, &shb, &sfill_b);
    MOQ_TEST_CHECK(fill_ref != 0 && healthy_ref != 0 && fill_ref != healthy_ref);
    if (!fill_ref || !healthy_ref) { moq_simpair_destroy(sp); return failures; }

    moq_subscription_update_cfg_t up;
    moq_subscription_update_cfg_init_sized(&up, sizeof(up));
    up.has_subscriber_priority = true; up.subscriber_priority = 9;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_subscription(c, ch, &up, 3), (int)MOQ_OK);
    pump(sp);
    uint64_t id = 0;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_UPDATED) id = ev.u.subscribe_updated.update_request_id;
        moq_event_cleanup(&ev);
    }
    drain_actions(sv);
    moq_reject_subscription_update_cfg_t rj; moq_reject_subscription_update_cfg_init(&rj);
    rj.update_request_id = id;
    rj.error_code = 0x5;

    /* No free slot: refused before any effect. */
    occupy(sv, 0);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), (size_t)0);
    size_t queued = sv->action_tail - sv->action_head;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_subscription_update(sv, sh, &rj, 4),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_SIZE((size_t)(sv->action_tail - sv->action_head), queued);
    int ss = sub_resolve_handle(sv, sh);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss >= 0) {
        MOQ_TEST_CHECK(sv->subs[ss].state != MOQ_SUB_DONE_HELD);
        MOQ_TEST_CHECK(sv->subs[ss].owed_update.owed);
    }
    MOQ_TEST_CHECK_EQ_U64(fill_stream_of(sv, sfill), fill_ref);
    MOQ_TEST_CHECK_EQ_U64(fill_stream_of(sv, sfill_b), healthy_ref);

    /* Exactly the reject's own two slots free: taken. */
    moq_action_t a;
    for (int k = 0; k < 2 && moq_session_poll_actions(sv, &a, 1) > 0; k++)
        moq_action_cleanup(&a);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), (size_t)2);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_subscription_update(sv, sh, &rj, 4),
                          (int)MOQ_OK);
    failures += check_fill_reset_then_done(sv, fill_ref, healthy_ref);
    MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
    /* B and its fill are untouched. */
    MOQ_TEST_CHECK(sub_resolve_handle(sv, shb) >= 0);
    MOQ_TEST_CHECK_EQ_U64(fill_stream_of(sv, sfill_b), healthy_ref);
    {
        size_t slot = fill_slot_of(sv, sfill_b);
        MOQ_TEST_CHECK(slot != SIZE_MAX);
        if (slot != SIZE_MAX) {
            MOQ_TEST_CHECK(!sv->fill_storage[slot].data_reset);
            MOQ_TEST_CHECK(!sv->fill_storage[slot].parent_cancelled);
        }
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-22: the same ordering for a PUBLISH-established subscription. The
 * publication's only open sending stream is a fill its subscriber asked for
 * in an accepted update; a later update is rejected with exactly the reject's
 * own two slots free. The fill RESET comes before the single PUBLISH_DONE and
 * nothing else is reset. */
static int test_reject_publication_update_closes_fill_first(void)
{
    int failures = 0;
    g_defer = true;
    moq_simpair_t *sp = mk_pair(ACTIONS);
    g_defer = false;
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t sph, cph = { 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(sv, &cfg, 1, &sph), (int)MOQ_OK);
    pump(sp);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) cph = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
    }
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(c, cph, &ac, 2), (int)MOQ_OK);
    pump(sp);
    drain(c); drain(sv);
    /* One Object, so a fill from (0,0) has history to cover. */
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    sc.group_id = 0;
    moq_subgroup_handle_t h;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_pub_subgroup(sv, sph, &sc, 3, &h), (int)MOQ_OK);
    moq_rcbuf_t *pl = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(sv, h, 0, pl, 3), (int)MOQ_OK);
    moq_rcbuf_decref(pl);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(sv, h, 3), (int)MOQ_OK);
    pump(sp);
    drain(c); drain_actions(sv);

    /* The subscriber's update asks for a fill; the publisher accepts it. */
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = 0; loc.start_object = 0;
    moq_fill_cfg_t fill; moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_publication_update_cfg_t up; moq_publication_update_cfg_init(&up);
    up.fill = &fill;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(c, cph, &up, 3), (int)MOQ_OK);
    pump(sp);
    uint64_t id = 0;
    moq_fill_t sfill = { 0 };
    bool have_fill = false;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_UPDATED) id = ev.u.publish_updated.update_request_id;
        if (ev.kind == MOQ_EVENT_FILL_REQUEST) { sfill = ev.u.fill_request.fill; have_fill = true; }
        moq_event_cleanup(&ev);
    }
    moq_accept_subscription_update_cfg_t acc; moq_accept_subscription_update_cfg_init(&acc);
    acc.update_request_id = id;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publication_update(sv, sph, &acc, 3),
                          (int)MOQ_OK);
    pump(sp);
    drain(c);
    uint64_t fill_ref = 0;
    if (have_fill) {
        drain(sv);
        if (moq_session_open_fill(sv, sfill, 3) == MOQ_OK) {
            moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
            oc.group_id = 0; oc.object_id = 0; oc.publisher_priority = 5;
            moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"y", 1, &oc.payload);
            if (moq_session_write_fill_object(sv, sfill, &oc, 3) == MOQ_OK) {
                drain_actions(sv);
                fill_ref = fill_stream_of(sv, sfill);
            }
            moq_rcbuf_decref(oc.payload);
        }
    } else {
        fill_ref = open_requested_fill(sv, &sfill);
    }
    MOQ_TEST_CHECK(fill_ref != 0);
    if (!fill_ref) { moq_simpair_destroy(sp); return failures; }

    /* A second update, rejected. */
    moq_publication_update_cfg_t up2; moq_publication_update_cfg_init(&up2);
    up2.has_subscriber_priority = true; up2.subscriber_priority = 9;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(c, cph, &up2, 4), (int)MOQ_OK);
    pump(sp);
    id = 0;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_PUBLISH_UPDATED) id = ev.u.publish_updated.update_request_id;
        moq_event_cleanup(&ev);
    }
    drain_actions(sv);
    occupy(sv, 2);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), (size_t)2);
    moq_reject_subscription_update_cfg_t rj; moq_reject_subscription_update_cfg_init(&rj);
    rj.update_request_id = id;
    rj.error_code = 0x5;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_publication_update(sv, sph, &rj, 5),
                          (int)MOQ_OK);
    failures += check_fill_reset_then_done(sv, fill_ref, 0);
    MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        g_version = versions[i];
        int before = failures;
        failures += test_subscription_teardown(true);
        failures += test_subscription_teardown(false);
        failures += test_publication_teardown(true);
        failures += test_publication_teardown(false);
        if (failures != before)
            fprintf(stderr, "  version 0x%llx: %d failures\n",
                    (unsigned long long)versions[i], failures - before);
    }
    static const moq_version_t all[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 3; i++) {
        g_version = all[i];
        int before = failures;
        failures += test_reject_subscription_update();
        failures += test_reject_publication_update();
        if (all[i] != MOQ_VERSION_DRAFT_16)   /* no request stream on draft-16 */
            failures += test_reject_subscription_update_ex(true);
        if (failures != before)
            fprintf(stderr, "  reject, version 0x%llx: %d failures\n",
                    (unsigned long long)all[i], failures - before);
    }
    g_version = MOQ_VERSION_DRAFT_22;
    failures += test_reject_update_closes_fill_first();
    failures += test_reject_publication_update_closes_fill_first();
    failures += test_fetch_fin_reserves_both();
    failures += test_publish_fin_reserves_both();
    if (failures == 0) MOQ_TEST_PASS("teardown_capacity");
    return failures != 0;
}
