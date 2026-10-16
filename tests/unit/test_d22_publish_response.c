/*
 * PUBLISH on a profile whose response carries no subscription parameters.
 *
 * The subscriber's accept sends a bare REQUEST_OK; the publisher keeps the
 * initial settings it put on the PUBLISH. An accept that tries to override
 * those settings is refused (MOQ_ERR_INVAL); changing them afterwards is a
 * separate, explicit update.
 * Draft-18 keeps carrying the choices on PUBLISH_OK.
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
        moq_simpair_destroy(sp); return NULL;
    }
    if (moq_session_state(moq_simpair_client(sp)) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(moq_simpair_server(sp)) != MOQ_SESS_ESTABLISHED) {
        moq_simpair_destroy(sp); return NULL;
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

static int count_kind(moq_session_t *s, moq_event_kind_t kind)
{
    moq_event_t ev; int n = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) { if (ev.kind == kind) n++; moq_event_cleanup(&ev); }
    return n;
}

/* Publish a paused track (FORWARD 0) from the client; the server receives it. */
static bool start_publish(moq_simpair_t *sp, moq_publication_t *ph, moq_publication_t *sph)
{
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.has_forward = true;
    cfg.forward = false;
    if (moq_session_publish(c, &cfg, 1, ph) != MOQ_OK) return false;
    if (moq_simpair_run_until_quiescent(sp, 16, NULL) != MOQ_OK) return false;
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev)) return false;
    bool paused = !ev.u.publish_request.forward;
    *sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    drain(c);
    return paused;
}

static int publish_slot(moq_session_t *s)
{
    for (size_t i = 0; i < s->pub_cap; i++)
        if (s->publishes[i].state != MOQ_PUB_FREE) return (int)i;
    return -1;
}

/* Refused accept choices, followed by acceptance and a separate real UPDATE. */
static int test_accept_with_choices(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    bool started = start_publish(sp, &ph, &sph);
    MOQ_TEST_CHECK(started);
    if (!started) goto done;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init_sized(&ac, sizeof(ac));
    ac.has_subscriber_priority = true; ac.subscriber_priority = 10;
    ac.has_forward = true; ac.forward = true;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 6), (int)MOQ_ERR_INVAL);
    int ss = publish_slot(sv);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss < 0) goto done;
    MOQ_TEST_CHECK_EQ_INT(sv->publishes[ss].state, MOQ_PUB_PENDING_SUBSCRIBER);
    MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    moq_accept_publish_cfg_init(&ac);
    moq_result_t rc = moq_session_accept_publish(sv, sph, &ac, 7);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_publication_update_cfg_t update;
    moq_publication_update_cfg_init(&update);
    update.has_subscriber_priority = true; update.subscriber_priority = 10;
    update.has_forward = true; update.forward = true;
    rc = moq_session_update_publication(sv, sph, &update, 8);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t ev;
    /* The publisher sees its response, reflecting its own initial settings,
     * and then the subscriber's choices as an update, in that order. */
    {
        moq_event_t evs[16]; size_t n = 0;
        while (n < 16 && moq_session_poll_events(c, &evs[n], 1) > 0) n++;
        int ok_at = -1, upd_at = -1;
        for (size_t i = 0; i < n; i++) {
            if (evs[i].kind == MOQ_EVENT_PUBLISH_OK && ok_at < 0) ok_at = (int)i;
            if (evs[i].kind == MOQ_EVENT_PUBLISH_UPDATED && upd_at < 0) upd_at = (int)i;
        }
        MOQ_TEST_CHECK(ok_at >= 0 && upd_at > ok_at);
        if (ok_at >= 0) {
            MOQ_TEST_CHECK(!evs[ok_at].u.publish_ok.send_allowed);
            MOQ_TEST_CHECK_EQ_INT((int)evs[ok_at].u.publish_ok.subscriber_priority, 128);
        }
        if (upd_at >= 0) {
            MOQ_TEST_CHECK(evs[upd_at].u.publish_updated.has_subscriber_priority);
            MOQ_TEST_CHECK_EQ_INT((int)evs[upd_at].u.publish_updated.subscriber_priority, 10);
            MOQ_TEST_CHECK(evs[upd_at].u.publish_updated.has_forward &&
                           evs[upd_at].u.publish_updated.forward);
        }
        for (size_t i = 0; i < n; i++) moq_event_cleanup(&evs[i]);
    }
    int cs = publish_slot(c);
    MOQ_TEST_CHECK(cs >= 0);
    if (cs >= 0) {
        MOQ_TEST_CHECK(c->publishes[cs].send_allowed);
        MOQ_TEST_CHECK_EQ_INT((int)c->publishes[cs].subscriber_priority, 10);
    }
    /* The update was acknowledged on the subscriber side. */
    bool found = take(sv, MOQ_EVENT_PUBLICATION_UPDATE_OK, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) goto done;
    moq_event_cleanup(&ev);
    if (ss >= 0) MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* No choices: the bare response alone; the publisher stays paused. */
static int test_accept_without_choices(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    bool started = start_publish(sp, &ph, &sph);
    MOQ_TEST_CHECK(started);
    if (!started) goto done;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    moq_result_t rc = moq_session_accept_publish(sv, sph, &ac, 6);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    int ss = publish_slot(sv);
    if (ss >= 0) MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t ev;
    bool found = take(c, MOQ_EVENT_PUBLISH_OK, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) goto done;
    MOQ_TEST_CHECK(!ev.u.publish_ok.send_allowed);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_PUBLISH_UPDATED), 0);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* D22 PUBLISH_OK has no group-order form, so this is an invalid override. */
static int test_group_order_refused(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    bool started = start_publish(sp, &ph, &sph);
    MOQ_TEST_CHECK(started);
    if (!started) goto done;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    ac.group_order = MOQ_GROUP_ORDER_DESCENDING;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 6), (int)MOQ_ERR_INVAL);
    int ss = publish_slot(sv);
    if (ss >= 0) MOQ_TEST_CHECK_EQ_INT((int)sv->publishes[ss].state, (int)MOQ_PUB_PENDING_SUBSCRIBER);
    moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 7), (int)MOQ_OK);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-18 carries the choices on the response itself. */
static int test_d18_choices_on_response(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_18);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    bool started = start_publish(sp, &ph, &sph);
    MOQ_TEST_CHECK(started);
    if (!started) goto done;
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init_sized(&ac, sizeof(ac));
    ac.has_subscriber_priority = true; ac.subscriber_priority = 10;
    ac.has_forward = true; ac.forward = true;
    moq_result_t rc = moq_session_accept_publish(sv, sph, &ac, 6);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    int ss = publish_slot(sv);
    if (ss >= 0) MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    rc = moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t ev;
    bool found = take(c, MOQ_EVENT_PUBLISH_OK, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) goto done;
    MOQ_TEST_CHECK(ev.u.publish_ok.send_allowed);
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.publish_ok.subscriber_priority, 10);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_PUBLISH_UPDATED), 0);
done:
    moq_simpair_destroy(sp);
    return failures;
}


/* The PUBLISH's initial settings reach the subscriber, and an accept that
 * keeps them sends no follow-up update. */
static int test_initial_settings_surfaced(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    /* A PUBLISH with SUBSCRIBER_PRIORITY 5, GROUP_ORDER descending and an
     * absolute-start filter at (3,0), fed to the server as the client's first
     * request (ID 0). */
    const uint8_t buf[] = {0x1d, 0, 19, 0, 1, 4, 'l', 'i', 'v', 'e', 1, 'v', 7,
        3, 0x20, 5, 1, 2, 3, 0, 1, 2};
    moq_result_t rc = moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(0x7100),
        buf, sizeof(buf), false, 5);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t ev;
    bool found = take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev);
    MOQ_TEST_CHECK(found);
    if (!found) goto done;
    moq_publication_t sph = ev.u.publish_request.pub;
    /* This seven-byte window is reserved in the original public layout. */
    const uint8_t reserved[7] = {0};
    const uint8_t *event_bytes = (const uint8_t *)&ev.u.publish_request;
    MOQ_TEST_CHECK(memcmp(event_bytes +
        offsetof(moq_publish_request_event_t, forward) + sizeof(ev.u.publish_request.forward),
        reserved, sizeof(reserved)) == 0);
    moq_event_cleanup(&ev);
    moq_publish_initial_parameters_t initial;
    rc = moq_session_get_publish_initial_parameters(sv, sph, &initial, sizeof(initial));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_U64(initial.struct_size, sizeof(initial));
    MOQ_TEST_CHECK_EQ_U64(initial.flags, MOQ_PUBLISH_INITIAL_HAS_SUBSCRIBER_PRIORITY |
        MOQ_PUBLISH_INITIAL_HAS_GROUP_ORDER | MOQ_PUBLISH_INITIAL_HAS_LOCATION);
    MOQ_TEST_CHECK_EQ_INT((int)initial.subscriber_priority, 5);
    MOQ_TEST_CHECK_EQ_INT((int)initial.group_order, (int)MOQ_GROUP_ORDER_DESCENDING);
    MOQ_TEST_CHECK_EQ_U64(initial.location.kind, MOQ_LOCATION_FILTER_ABSOLUTE_START);
    MOQ_TEST_CHECK_EQ_U64(initial.location.start_group, 3);
    MOQ_TEST_CHECK_EQ_U64(initial.location.start_object, 0);
    /* Explicit equal-priority accept is still invalid under3218. */
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    ac.has_subscriber_priority = true; ac.subscriber_priority = 5;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 6), (int)MOQ_ERR_INVAL);
    moq_accept_publish_cfg_init(&ac);
    rc = moq_session_accept_publish(sv, sph, &ac, 7);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    int ss = pub_resolve_handle(sv, sph);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss >= 0) MOQ_TEST_CHECK(!sv->publishes[ss].update_pending);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_initial_settings_surfaced();
    failures += test_accept_with_choices();
    failures += test_accept_without_choices();
    failures += test_group_order_refused();
    failures += test_d18_choices_on_response();
    if (failures) { fprintf(stderr, "FAIL: test_d22_publish_response (%d)\n", failures); return 1; }
    printf("PASS: test_d22_publish_response\n");
    return 0;
}
