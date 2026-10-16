/*
 * The publisher's unilateral state notification on a subscription (draft-22
 * PUBLISH_STATE_NOTIFY): sent on the subscription's request stream from the
 * publisher of a SUBSCRIBE-initiated or PUBLISH-initiated subscription,
 * carrying a Forward change, the filter now in effect and the Largest Object
 * at which it took effect. The subscriber learns it as MOQ_EVENT_PUBLISH_STATE
 * and owes nothing back; no request ID is consumed. Draft-18 has no form for
 * it, so the call is refused there.
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

static int count_events(moq_session_t *s)
{
    moq_event_t ev; int n = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) { n++; moq_event_cleanup(&ev); }
    return n;
}

static int test_subscription_notify(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    /* The publisher writes (4,1), so its Largest Object is known. */
    moq_subgroup_cfg_t sgc; moq_subgroup_cfg_init(&sgc);
    sgc.group_id = 4; sgc.subgroup_id = 0;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, sh, &sgc, 3, &sg), (int)MOQ_OK);
    uint8_t d[] = { 0x01 };
    moq_rcbuf_t *pay = NULL;
    moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &pay);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(sv, sg, 1, pay, 3), (int)MOQ_OK);
    moq_rcbuf_decref(pay);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);

    moq_location_filter_cfg_t nf; moq_location_filter_cfg_init(&nf);
    nf.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    nf.start_group = 2; nf.start_object = 0;
    moq_publish_state_cfg_t nc; moq_publish_state_cfg_init(&nc);
    nc.has_forward = true; nc.forward = false;
    nc.filter = &nf;
    if (version == MOQ_VERSION_DRAFT_22) {
        moq_subscription_update_cfg_t update;
        moq_subscription_update_cfg_init_sized(&update, sizeof(update));
        update.has_forward = true; update.forward = false;
        update.location = &nf;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_subscription(c, ch, &update, 4),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_simpair_run_until_quiescent(sp, 16, NULL),
                              (int)MOQ_OK);
        bool acked = take(c, MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &ev);
        MOQ_TEST_CHECK(acked);
        if (acked) moq_event_cleanup(&ev);
        drain(c); drain(sv);
    }
    moq_result_t rc = moq_session_notify_subscription_state(sv, sh, &nc, 4);
    if (version == MOQ_VERSION_DRAFT_18) {
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_UNSUPPORTED);
        moq_simpair_destroy(sp);
        return failures;
    }
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    /* The subscriber may not notify. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_notify_subscription_state(c, ch, &nc, 4),
                          (int)MOQ_ERR_WRONG_STATE);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_PUBLISH_STATE, &ev));
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.publish_state.family, (int)MOQ_REQUEST_FAMILY_SUBSCRIBE);
    MOQ_TEST_CHECK(ev.u.publish_state.handle.subscription._opaque == ch._opaque);
    MOQ_TEST_CHECK(ev.u.publish_state.has_forward && !ev.u.publish_state.forward);
    MOQ_TEST_CHECK(ev.u.publish_state.has_largest);
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_state.largest_group, 4);
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_state.largest_object, 1);
    MOQ_TEST_CHECK(ev.u.publish_state.has_filter);
    moq_event_cleanup(&ev);
    moq_location_filter_cfg_t lf; moq_location_filter_cfg_init(&lf);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscription_filter(c, ch, &lf), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(lf.kind, MOQ_LOCATION_FILTER_ABSOLUTE_START);
    MOQ_TEST_CHECK_EQ_U64(lf.start_group, 2);
    /* Nothing comes back to the publisher, and both sides stay up. */
    MOQ_TEST_CHECK_EQ_INT(count_events(sv), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    /* The subscription still works: a later unsubscribe is accepted. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 9), (int)MOQ_OK);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_publication_notify(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t ph;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, &ph), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev));
    moq_publication_t sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    moq_publish_state_cfg_t nc; moq_publish_state_cfg_init(&nc);
    nc.has_forward = true; nc.forward = false;
    moq_publication_update_cfg_t update;
    moq_publication_update_cfg_init(&update);
    update.has_forward = true; update.forward = false;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(sv, sph, &update, 4),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_simpair_run_until_quiescent(sp, 16, NULL), (int)MOQ_OK);
    bool acked = take(sv, MOQ_EVENT_PUBLICATION_UPDATE_OK, &ev);
    MOQ_TEST_CHECK(acked);
    if (acked) moq_event_cleanup(&ev);
    drain(c); drain(sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_notify_publication_state(c, ph, &nc, 4), (int)MOQ_OK);
    /* The subscriber of the publication may not notify. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_notify_publication_state(sv, sph, &nc, 4),
                          (int)MOQ_ERR_WRONG_STATE);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_STATE, &ev));
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.publish_state.family, (int)MOQ_REQUEST_FAMILY_PUBLISH);
    MOQ_TEST_CHECK(ev.u.publish_state.handle.publication._opaque == sph._opaque);
    MOQ_TEST_CHECK(ev.u.publish_state.has_forward && !ev.u.publish_state.forward);
    MOQ_TEST_CHECK(!ev.u.publish_state.has_largest);   /* nothing published yet */
    MOQ_TEST_CHECK(!ev.u.publish_state.has_filter);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT(count_events(c), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_subscription_notify(MOQ_VERSION_DRAFT_22);
    failures += test_subscription_notify(MOQ_VERSION_DRAFT_18);
    failures += test_publication_notify();
    MOQ_TEST_CHECK(sizeof(moq_publish_state_event_t) <= MOQ_EVENT_DETAIL_MAX);
    if (failures) { fprintf(stderr, "FAIL: test_d22_state_notify (%d)\n", failures); return 1; }
    printf("PASS: test_d22_state_notify\n");
    return 0;
}
