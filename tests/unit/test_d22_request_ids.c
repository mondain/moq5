/*
 * Request IDs on draft-22 request streams: a REQUEST_UPDATE consumes an ID like
 * any request, so the next request from that side carries the following ID; an
 * update with the wrong parity closes the session with INVALID_REQUEST_ID.
 * PUBLISH acceptance never invents an UPDATE; a separate explicit UPDATE
 * consumes an ID only after its output can be queued. A request refused for
 * local capacity carries the draft's capacity code (EXCESSIVE_LOAD where
 * registered), on every draft.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include <string.h>

static moq_simpair_t *mk_pair(void)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = MOQ_VERSION_DRAFT_22;
    scfg.client_send_request_capacity = true; scfg.client_initial_request_capacity = 64;
    scfg.server_send_request_capacity = true; scfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    return sp;
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

static uint64_t close_code(moq_session_t *s)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION) code = a.u.close_session.code;
        moq_action_cleanup(&a);
    }
    return code;
}

/* Subscribe from the client and accept on the server; returns the server
 * handle and the server-side stream ref. */
static bool establish(moq_simpair_t *sp, const char *name, moq_subscription_t *ch,
                      moq_subscription_t *sh, moq_stream_ref_t *sref)
{
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    if (moq_session_subscribe(c, &cfg, 1, ch) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) return false;
    *sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    int slot = sub_resolve_handle(sv, *sh);
    if (slot < 0) return false;
    *sref = sv->subs[slot].rs.ref;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    if (moq_session_accept_subscribe(sv, *sh, &ac, 2) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    return true;
}

static int test_update_consumes_request_id(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscription_t ch, sh; moq_stream_ref_t sref;
    MOQ_TEST_CHECK(establish(sp, "a", &ch, &sh, &sref));
    int s0 = sub_resolve_handle(sv, sh);
    MOQ_TEST_CHECK(s0 >= 0 && sv->subs[s0].request_id == 0);
    /* The update takes the client's next ID (2). */
    moq_subscription_update_cfg_t uc; moq_subscription_update_cfg_init(&uc);
    uc.has_subscriber_priority = true; uc.subscriber_priority = 7;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_subscription(c, ch, &uc, 3), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_UPDATED, &ev));
    moq_event_cleanup(&ev);
    drain(c);
    /* So the next request from the client carries ID 4. */
    moq_subscription_t ch2, sh2; moq_stream_ref_t sref2;
    MOQ_TEST_CHECK(establish(sp, "b", &ch2, &sh2, &sref2));
    int s1 = sub_resolve_handle(sv, sh2);
    MOQ_TEST_CHECK(s1 >= 0);
    if (s1 >= 0) MOQ_TEST_CHECK_EQ_U64(sv->subs[s1].request_id, 4);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_update_wrong_parity_closes(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscription_t ch, sh; moq_stream_ref_t sref;
    MOQ_TEST_CHECK(establish(sp, "a", &ch, &sh, &sref));
    /* A REQUEST_UPDATE with a server-parity (odd) ID, fed on the established
     * stream as if from the client. */
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_REQUEST_UPDATE;
    m.request_id = 3;
    moq_d22_param_t prio;
    memset(&prio, 0, sizeof(prio));
    prio.type = 0x20; prio.value.integer = 9;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request_type = MOQ_D22_SUBSCRIBE;
    ctx.update_from_subscriber = true;
    ctx.max_filter_ranges = 8;
    uint8_t buf[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    MOQ_TEST_CHECK_EQ_INT((int)moq_d22_encode_message(&w, &m, &prio, 1, &ctx), (int)MOQ_OK);
    (void)moq_session_on_bidi_stream_bytes(sv, sref, buf, moq_buf_writer_offset(&w), false, 5);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK_EQ_U64(close_code(sv), 0x4);   /* INVALID_REQUEST_ID */
    moq_simpair_destroy(sp);
    return failures;
}

static int test_accept_publish_then_explicit_update(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t ph;
    moq_result_t rc = moq_session_publish(c, &cfg, 1, &ph);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t ev;
    bool found = take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) goto done;
    moq_publication_t sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    const int ss = pub_resolve_handle(sv, sph);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss < 0) goto done;
    drain(c);
    /* Fill the server's action queue down to one free slot with its own
     * subscribes (each opens a request stream). */
    int opened = 0;
    while (action_queue_avail(sv) > 1) {
        moq_subscribe_cfg_t sc; moq_subscribe_cfg_init(&sc);
        sc.track_namespace = (moq_namespace_t){ parts, 1 };
        char name[16]; snprintf(name, sizeof(name), "t%d", opened);
        sc.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
        moq_subscription_t h;
        if (moq_session_subscribe(sv, &sc, 4, &h) != MOQ_OK) break;
        opened++;
    }
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), 1);
    if (action_queue_avail(sv) != 1) goto done;
    const uint64_t update_id = 1 + 2 * (uint64_t)opened;
    MOQ_TEST_CHECK(update_id < 128);
    if (update_id >= 128) goto done;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init_sized(&ac, sizeof(ac));
    ac.has_subscriber_priority = true; ac.subscriber_priority = 10;
    const uint64_t epoch = sv->borrow_epoch;
    const size_t send_len = sv->send_len;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 5), (int)MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)sv->publishes[ss].state, (int)MOQ_PUB_PENDING_SUBSCRIBER);
    MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_U64(sv->borrow_epoch, epoch);
    MOQ_TEST_CHECK_EQ_SIZE(sv->send_len, send_len);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), 1);

    /* One free action suffices for the response, without a hidden UPDATE. */
    ac.has_subscriber_priority = false;
    rc = moq_session_accept_publish(sv, sph, &ac, 6);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_INT((int)sv->publishes[ss].state, (int)MOQ_PUB_ESTABLISHED);
    MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), 0);

    moq_publication_update_cfg_t update;
    moq_publication_update_cfg_init(&update);
    update.has_subscriber_priority = true; update.subscriber_priority = 10;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(sv, sph, &update, 7),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), 0);

    moq_action_t a;
    unsigned responses = 0;
    const uint8_t accepted[] = {7, 0, 1, 0};
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == sv->publishes[ss].rs.ref._v) {
            ++responses;
            MOQ_TEST_CHECK(!a.u.send_bidi_stream.fin);
            MOQ_TEST_CHECK_EQ_SIZE(a.u.send_bidi_stream.len, sizeof(accepted));
            MOQ_TEST_CHECK(a.u.send_bidi_stream.data &&
                a.u.send_bidi_stream.len == sizeof(accepted) &&
                !memcmp(a.u.send_bidi_stream.data, accepted, sizeof(accepted)));
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT((int)responses, 1);

    rc = moq_session_update_publication(sv, sph, &update, 8);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK(sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_U64(sv->publishes[ss].update_request_id, update_id);
    const uint8_t expected_update[] = {2, 0, 4, (uint8_t)update_id, 1, 0x20, 10};
    size_t count = moq_session_poll_actions(sv, &a, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, 1);
    if (count) {
        MOQ_TEST_CHECK_EQ_INT((int)a.kind, (int)MOQ_ACTION_SEND_BIDI_STREAM);
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            MOQ_TEST_CHECK_EQ_U64(a.u.send_bidi_stream.stream_ref._v, sv->publishes[ss].rs.ref._v);
            MOQ_TEST_CHECK(!a.u.send_bidi_stream.fin);
            MOQ_TEST_CHECK_EQ_SIZE(a.u.send_bidi_stream.len, sizeof(expected_update));
            MOQ_TEST_CHECK(a.u.send_bidi_stream.data &&
                a.u.send_bidi_stream.len == sizeof(expected_update) &&
                !memcmp(a.u.send_bidi_stream.data, expected_update, sizeof(expected_update)));
        }
        moq_action_cleanup(&a);
    }
    count = moq_session_poll_actions(sv, &a, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, 0);
    if (count) moq_action_cleanup(&a);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* -- Capacity refusals ----------------------------------------------------
 * A request refused because a local pool or registry is exhausted carries
 * EXCESSIVE_LOAD (draft-22 §12.3 / §16.11.2 0x9: "The responder is overloaded
 * and cannot process the request at this time. The sender SHOULD use the
 * Retry Interval to indicate when the request can be retried"; draft-18
 * §10.6 the same) with a non-zero Retry Interval, so the requester sees
 * can_retry. Draft-16 registers no EXCESSIVE_LOAD request code and keeps
 * INTERNAL_ERROR without retry. The session stays up. Each case shrinks one
 * fresh server pool to zero before the request arrives. */
enum { CAP_SUB_POOL, CAP_SUB_HISTORY, CAP_TRACK_STATUS, CAP_TS_HISTORY,
       CAP_TRACKS, CAP_ANNOUNCE, CAP_PUBLISH, CAP_PUBLISH_HISTORY, CAP_COUNT };
static const char *const cap_names[CAP_COUNT] = {
    "subscribe pool", "subscribe history", "track status pool",
    "track status history", "subscribe-tracks pool", "announcement pool",
    "publish pool", "publish history" };

static int capacity_refusal(moq_version_t version, int which)
{
    int failures = 0;
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    scfg.client_send_request_capacity = true; scfg.client_initial_request_capacity = 64;
    scfg.server_send_request_capacity = true; scfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&scfg, &sp) == MOQ_OK);
    if (!sp) return failures;
    MOQ_TEST_CHECK(moq_simpair_start(sp) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    switch (which) {
    case CAP_SUB_POOL:        sv->sub_cap = 0; break;
    case CAP_TRACK_STATUS:    sv->ts_cap = 0; break;
    case CAP_TRACKS:          sv->track_sub_cap = 0; break;
    case CAP_ANNOUNCE:        sv->ann_cap = 0; break;
    case CAP_PUBLISH:         sv->pub_cap = 0; break;
    default:                  sv->th_cap = 0; break;   /* the history registry */
    }
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("cap") };
    moq_namespace_t ns = { parts, 1 };
    moq_event_kind_t want = 0;
    moq_result_t rc = MOQ_OK;
    uint64_t now = 10;
    if (which == CAP_SUB_POOL || which == CAP_SUB_HISTORY) {
        moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = MOQ_BYTES_LITERAL("t");
        moq_subscription_t h;
        rc = moq_session_subscribe(c, &cfg, now, &h);
        want = MOQ_EVENT_SUBSCRIBE_ERROR;
    } else if (which == CAP_TRACK_STATUS || which == CAP_TS_HISTORY) {
        moq_track_status_cfg_t cfg; moq_track_status_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = MOQ_BYTES_LITERAL("t");
        moq_track_status_handle_t h;
        rc = moq_session_track_status(c, &cfg, now, &h);
        want = MOQ_EVENT_TRACK_STATUS_ERROR;
    } else if (which == CAP_TRACKS) {
        moq_subscribe_tracks_cfg_t cfg; moq_subscribe_tracks_cfg_init(&cfg);
        cfg.track_namespace_prefix = ns;
        moq_track_sub_handle_t h;
        rc = moq_session_subscribe_tracks(c, &cfg, now, &h);
        want = MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR;
    } else if (which == CAP_ANNOUNCE) {
        moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
        cfg.track_namespace = ns;
        moq_announcement_t h;
        rc = moq_session_publish_namespace(c, &cfg, now, &h);
        want = MOQ_EVENT_NAMESPACE_REJECTED;
    } else {
        moq_publish_cfg_t cfg; moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns; cfg.track_name = MOQ_BYTES_LITERAL("t");
        moq_publication_t h;
        rc = moq_session_publish(c, &cfg, now, &h);
        want = MOQ_EVENT_PUBLISH_ERROR;
    }
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_event_t ev;
    bool got = take(c, want, &ev);
    MOQ_TEST_CHECK(got);
    if (got) {
        uint64_t code = 0;
        bool retry = false;
        switch (want) {
        case MOQ_EVENT_SUBSCRIBE_ERROR:
            code = ev.u.subscribe_error.error_code; retry = ev.u.subscribe_error.can_retry; break;
        case MOQ_EVENT_TRACK_STATUS_ERROR:
            code = ev.u.track_status_error.error_code; retry = ev.u.track_status_error.can_retry; break;
        case MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR:
            code = ev.u.subscribe_tracks_error.error_code; retry = ev.u.subscribe_tracks_error.can_retry; break;
        case MOQ_EVENT_NAMESPACE_REJECTED:
            code = ev.u.namespace_rejected.error_code; retry = ev.u.namespace_rejected.can_retry; break;
        default:
            code = ev.u.publish_error.error_code; retry = ev.u.publish_error.can_retry; break;
        }
        const bool registered = version != MOQ_VERSION_DRAFT_16;
        MOQ_TEST_CHECK_EQ_U64(code, registered ? MOQ_REQUEST_ERROR_EXCESSIVE_LOAD
                                               : MOQ_REQUEST_ERROR_INTERNAL_ERROR);
        MOQ_TEST_CHECK(retry == registered);
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    if (failures)
        fprintf(stderr, "capacity_refusal v=%u %s\n", (unsigned)version, cap_names[which]);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_update_consumes_request_id();
    failures += test_update_wrong_parity_closes();
    failures += test_accept_publish_then_explicit_update();
    {
        const moq_version_t versions[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18,
                                           MOQ_VERSION_DRAFT_22 };
        for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++)
            for (int which = 0; which < CAP_COUNT; which++) {
                /* Draft-16 has no SUBSCRIBE_TRACKS. */
                if (versions[v] == MOQ_VERSION_DRAFT_16 && which == CAP_TRACKS) continue;
                failures += capacity_refusal(versions[v], which);
            }
    }
    if (failures) { fprintf(stderr, "FAIL: test_d22_request_ids (%d)\n", failures); return 1; }
    printf("PASS: test_d22_request_ids\n");
    return 0;
}
