/*
 * REDIRECT answering PUBLISH and SUBSCRIBE_TRACKS. Draft-22 lets a REQUEST_ERROR
 * with code REDIRECT answer every request family; the requester surfaces
 * MOQ_EVENT_REQUEST_REDIRECT with the family's handle and the request ends.
 * Draft-18 limits REDIRECT to five families, so the reject refuses the code
 * there before anything is sent.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_simpair_t *mk_pair(moq_version_t version)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    if (moq_simpair_run_until_quiescent(sp, 16, NULL) != MOQ_OK) {
        moq_simpair_destroy(sp);
        return NULL;
    }
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

static int test_publish_redirect(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) goto done;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    MOQ_TEST_CHECK(c != NULL && sv != NULL);
    if (!c || !sv) goto done;
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t ph;
    moq_result_t r = moq_session_publish(c, &cfg, 1, &ph);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    r = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    moq_event_t ev;
    bool got_publish_request = take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev);
    MOQ_TEST_CHECK(got_publish_request);
    if (!got_publish_request) goto done;
    moq_publication_t sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    drain(c);
    moq_reject_publish_cfg_t rc; moq_reject_publish_cfg_init(&rc);
    rc.error_code = MOQ_REQUEST_ERROR_REDIRECT;
    rc.can_retry = true; rc.retry_after_ms = 1500;
    rc.reason = MOQ_BYTES_LITERAL("elsewhere");
    r = moq_session_reject_publish(sv, sph, &rc, 6);
    if (version == MOQ_VERSION_DRAFT_18) {
        MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_ERR_INVAL);
        goto done;
    }
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    r = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    bool got_publish_redirect = take(c, MOQ_EVENT_REQUEST_REDIRECT, &ev);
    MOQ_TEST_CHECK(got_publish_redirect);
    if (!got_publish_redirect) goto done;
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.request_redirect.family, (int)MOQ_REQUEST_FAMILY_PUBLISH);
    MOQ_TEST_CHECK(ev.u.request_redirect.handle.publication._opaque == ph._opaque);
    MOQ_TEST_CHECK_EQ_U64(ev.u.request_redirect.error_code, MOQ_REQUEST_ERROR_REDIRECT);
    MOQ_TEST_CHECK(ev.u.request_redirect.can_retry);
    MOQ_TEST_CHECK_EQ_U64(ev.u.request_redirect.retry_after_ms, 1500);
    MOQ_TEST_CHECK_EQ_U64(ev.u.request_redirect.connect_uri.len, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.request_redirect.track_namespace.count, 0);
    MOQ_TEST_CHECK(ev.u.request_redirect.reason.len == 9 &&
                   memcmp(ev.u.request_redirect.reason.data, "elsewhere", 9) == 0);
    moq_event_cleanup(&ev);
    /* No ordinary error event beside the redirect; the request is gone. */
    bool got_publish_error = take(c, MOQ_EVENT_PUBLISH_ERROR, &ev);
    MOQ_TEST_CHECK(!got_publish_error);
    if (got_publish_error) moq_event_cleanup(&ev);
    moq_finish_publish_cfg_t fc; moq_finish_publish_cfg_init(&fc);
    MOQ_TEST_CHECK(moq_session_finish_publish(c, ph, &fc, 8) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
    return failures;
}

static int test_subscribe_tracks_redirect(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) goto done;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    MOQ_TEST_CHECK(c != NULL && sv != NULL);
    if (!c || !sv) goto done;
    drain(c); drain(sv);
    moq_subscribe_tracks_cfg_t cfg; moq_subscribe_tracks_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace_prefix = (moq_namespace_t){ parts, 1 };
    moq_track_sub_handle_t th;
    moq_result_t r = moq_session_subscribe_tracks(c, &cfg, 1, &th);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    r = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    moq_event_t ev;
    bool got_subscribe_tracks_request = take(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST, &ev);
    MOQ_TEST_CHECK(got_subscribe_tracks_request);
    if (!got_subscribe_tracks_request) goto done;
    moq_track_sub_handle_t sh = ev.u.subscribe_tracks_request.handle;
    moq_event_cleanup(&ev);
    drain(c);
    moq_reject_subscribe_tracks_cfg_t rc; moq_reject_subscribe_tracks_cfg_init(&rc);
    rc.error_code = MOQ_REQUEST_ERROR_REDIRECT;
    r = moq_session_reject_subscribe_tracks(sv, sh, &rc, 6);
    if (version == MOQ_VERSION_DRAFT_18) {
        MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_ERR_INVAL);
        goto done;
    }
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    r = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)r, (int)MOQ_OK);
    if (r != MOQ_OK) goto done;
    bool got_subscribe_tracks_redirect = take(c, MOQ_EVENT_REQUEST_REDIRECT, &ev);
    MOQ_TEST_CHECK(got_subscribe_tracks_redirect);
    if (!got_subscribe_tracks_redirect) goto done;
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.request_redirect.family, (int)MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS);
    MOQ_TEST_CHECK(ev.u.request_redirect.handle.track_sub._opaque == th._opaque);
    MOQ_TEST_CHECK(!ev.u.request_redirect.can_retry);
    moq_event_cleanup(&ev);
    bool got_subscribe_tracks_error = take(c, MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR, &ev);
    MOQ_TEST_CHECK(!got_subscribe_tracks_error);
    if (got_subscribe_tracks_error) moq_event_cleanup(&ev);
    /* The request only drains its stream's FIN now; no live entry remains. */
    MOQ_TEST_CHECK(c->track_subs[0].state == MOQ_TRACK_SUB_DRAINING_RESPONSE ||
                   c->track_subs[0].state == MOQ_TRACK_SUB_FREE);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
    return failures;
}

static int test_literal_track_name(bool tracks, bool server_request,
                                   bool nonempty, bool fragmented)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *requester = server_request ? moq_simpair_server(sp) :
                                               moq_simpair_client(sp);
    moq_session_t *peer = server_request ? moq_simpair_client(sp) :
                                         moq_simpair_server(sp);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(requester), MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(peer), MOQ_VERSION_DRAFT_22);
    drain(requester);
    drain(peer);
    const moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("literal")};
    moq_track_sub_handle_t th = {0};
    moq_publication_t ph = {0};
    moq_result_t rc;
    moq_stream_ref_t ref = {0};
    if (tracks) {
        moq_subscribe_tracks_cfg_t cfg;
        moq_subscribe_tracks_cfg_init(&cfg);
        cfg.track_namespace_prefix = (moq_namespace_t){parts, 1};
        rc = moq_session_subscribe_tracks(requester, &cfg, 1, &th);
    } else {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){parts, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("original");
        rc = moq_session_publish(requester, &cfg, 1, &ph);
    }
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    if (tracks) {
        int slot = track_sub_resolve_handle(requester, th);
        MOQ_TEST_CHECK(slot >= 0);
        if (slot < 0) goto done;
        ref = requester->track_subs[slot].rs.ref;
    } else {
        int slot = pub_resolve_handle(requester, ph);
        MOQ_TEST_CHECK(slot >= 0);
        if (slot < 0) goto done;
        ref = requester->publishes[slot].rs.ref;
    }
    drain(requester);
    /* D22 9.4.1/9.4.2: REQUEST_ERROR REDIRECT, no retry/reason/URI,
     * empty namespace, and either an empty Track Name or the literal "t".
     * These peer bytes deliberately bypass our outbound namespace validation. */
    const uint8_t empty[] = {5, 0, 6, 0x34, 0, 0, 0, 0, 0};
    const uint8_t named[] = {5, 0, 7, 0x34, 0, 0, 0, 0, 1, 't'};
    const uint8_t *wire = nonempty ? named : empty;
    size_t len = nonempty ? sizeof(named) : sizeof(empty);
    size_t prefix = fragmented ? len - 1 : 0;
    if (prefix) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(requester,
            ref, wire, prefix, false, 2), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(requester), MOQ_SESS_ESTABLISHED);
        moq_event_t unexpected;
        size_t count = moq_session_poll_events(requester, &unexpected, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 0);
        if (count) moq_event_cleanup(&unexpected);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(requester,
        ref, wire + prefix, len - prefix, true, 3), MOQ_OK);
    size_t redirects = 0, closes = 0;
    moq_event_t event;
    while (moq_session_poll_events(requester, &event, 1)) {
        if (event.kind == MOQ_EVENT_SESSION_CLOSED) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(event.u.closed.code, 3);
        } else if (event.kind == MOQ_EVENT_REQUEST_REDIRECT) {
            ++redirects;
            MOQ_TEST_CHECK_EQ_INT(event.u.request_redirect.family,
                tracks ? MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS : MOQ_REQUEST_FAMILY_PUBLISH);
            MOQ_TEST_CHECK_EQ_U64(tracks ? event.u.request_redirect.handle.track_sub._opaque :
                event.u.request_redirect.handle.publication._opaque,
                tracks ? th._opaque : ph._opaque);
            MOQ_TEST_CHECK_EQ_SIZE(event.u.request_redirect.track_name.len, nonempty ? 1 : 0);
            if (event.u.request_redirect.track_name.len == 1)
                MOQ_TEST_CHECK(event.u.request_redirect.track_name.data[0] == 't');
        } else {
            MOQ_TEST_CHECK(event.kind != MOQ_EVENT_PUBLISH_ERROR);
            MOQ_TEST_CHECK(event.kind != MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR);
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, tracks && nonempty ? 1 : 0);
    MOQ_TEST_CHECK_EQ_SIZE(redirects, tracks && nonempty ? 0 : 1);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(requester),
        tracks && nonempty ? MOQ_SESS_CLOSED : MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_publish_redirect(MOQ_VERSION_DRAFT_22);
    failures += test_publish_redirect(MOQ_VERSION_DRAFT_18);
    failures += test_subscribe_tracks_redirect(MOQ_VERSION_DRAFT_22);
    failures += test_subscribe_tracks_redirect(MOQ_VERSION_DRAFT_18);
    for (unsigned server = 0; server < 2; ++server) {
        for (unsigned fragmented = 0; fragmented < 2; ++fragmented) {
            failures += test_literal_track_name(true, server != 0, false, fragmented != 0);
            failures += test_literal_track_name(true, server != 0, true, fragmented != 0);
            failures += test_literal_track_name(false, server != 0, true, fragmented != 0);
        }
    }
    if (failures) { fprintf(stderr, "FAIL: test_d22_redirect (%d)\n", failures); return 1; }
    printf("PASS: test_d22_redirect\n");
    return 0;
}
