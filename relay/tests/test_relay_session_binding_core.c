/*
 * Core session flows through the production relay binding, each through
 * real wire bytes: the parity flow on draft-16, draft-18 and draft-22
 * (announce/accept, subscribe coalescing — one upstream SUBSCRIBE for two
 * downstreams — upstream resolution, retained + live delivery with exact
 * payload bytes, filters, publish push, namespace discovery, track status,
 * unsubscribe/linger, close/retire) and a run-twice trace hash that must
 * come out equal.
 *
 * Around that flow, the binding's own lifecycle: undersized maps refuse on
 * the wire instead of orphaning, a full deferred-output ring, teardown and
 * shutdown with work still queued, close churn on either side, upstream
 * terminals reaching every downstream, fail-closed allocation-failure
 * sweeps, request credit on long-lived connections, intent routing and
 * reentry, the warm-track linger armed with the real clock, downstream
 * subgroup-slot reclamation, and an omitted SUBSCRIPTION_FILTER meaning
 * unfiltered on draft-16 and draft-18.
 */
#include "relay_session_binding_harness.h"

/* Transport-step every connection once with NO bind pump — used to stage
 * multiple inbound events into one subsequent bind pump. */
static void
rig_transport_only(rig_t *r)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (r->conns[i].used) {
            (void)moq_simpair_advance_to(r->conns[i].sp, r->now + 1);
            size_t st = 0;
            (void)moq_simpair_run_until_quiescent(r->conns[i].sp, 64, &st);
        }
    }
}

static uint64_t
parity_hash_run(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        return 0;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub1 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    peer_state_t pub_ps, s1_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s1_ps, 0, sizeof(s1_ps));
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("h"), B("x") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    (void)moq_session_publish_namespace(pub->peer, &pcfg, rig.now, &ann);
    relay_bind_test_rig_pump(&rig, 4);
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("t");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    (void)moq_session_subscribe(sub1->peer, &scfg, rig.now, &sh);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 4);
    if (pub_ps.up_seen) {
        moq_subgroup_cfg_t sgc;
        moq_subgroup_cfg_init(&sgc);
        sgc.group_id = 1;
        moq_subgroup_handle_t sgh;
        if (moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sgc,
                                      rig.now, &sgh) == MOQ_OK) {
            uint8_t body[32];
            memset(body, 0x77, sizeof(body));
            moq_rcbuf_t *pl = NULL;
            (void)moq_rcbuf_create(&a.vt, body, sizeof(body), &pl);
            (void)moq_session_write_object(pub->peer, sgh, 0, pl, rig.now);
            moq_rcbuf_decref(pl);
            (void)moq_session_close_subgroup(pub->peer, sgh, rig.now);
        }
    }
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub1, &s1_ps, false, false, 0, 0);
    uint64_t h = moqr_trace_hash(rig.trace);
    if (s1_ps.objects != 1) {
        h = 0;   /* delivery failure collapses the hash: test fails */
    }
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        h = 0;
    }
    return h;
}

/* Undersized binding maps: the wire answer is a refusal, never a silent
 * orphan. Each dimension gets its own minimal rig. */
static int
small_caps_flow(void)
{
    int failures = 0;

    /* Downstream-sub cap: two subscribers to distinct published tracks on
     * ONE connection; dsubs=1 forces the second to REJECT. */
    {
        ca_t a;
        relay_bind_test_ca_init(&a);
        rig_t rig;
        R_CHECK(&rig, relay_bind_test_rig_create_ex(&rig, &a, 1, 0, 0, 0) == MOQR_OK);
        conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
        conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
        R_CHECK(&rig, pub && sub);
        relay_bind_test_rig_pump(&rig, 4);
        moq_bytes_t nsp[2] = { B("cap"), B("d") };
        moq_publish_namespace_cfg_t pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ nsp, 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg,
                                                    rig.now, &ann) ==
                          MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        peer_state_t pub_ps, sub_ps;
        memset(&pub_ps, 0, sizeof(pub_ps));
        memset(&sub_ps, 0, sizeof(sub_ps));
        moq_subscribe_cfg_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ nsp, 2 };
        scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        scfg.track_name = B("t1");
        moq_subscription_t h1;
        R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now,
                                            &h1) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* accept t1 */
        relay_bind_test_rig_pump(&rig, 4);
        scfg.track_name = B("t2");
        moq_subscription_t h2;
        R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now,
                                            &h2) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* accept t2 */
        relay_bind_test_rig_pump(&rig, 6);
        relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
        R_CHECK(&rig, sub_ps.subscribe_ok);            /* t1 established */
        R_CHECK(&rig, sub_ps.subscribe_errors == 1);   /* t2 refused      */
        /* A full tracking table is a capacity refusal (d18 Section 10.6). */
        R_CHECK(&rig, sub_ps.sub_error_code ==
                          MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
        R_CHECK(&rig, relay_bind_test_sub_reason_eq(&sub_ps, "excessive load"));
        R_CHECK(&rig, sub_ps.sub_error_can_retry &&
                          sub_ps.sub_error_retry_ms >= 750 &&
                          sub_ps.sub_error_retry_ms <= 1250);
        moqr_core_stats_t cs;
        moqr_core_get_stats(rig.core, &cs);
        R_CHECK(&rig, cs.subs == 1);   /* refused sub left no live state */
        failures += rig.failures;
        relay_bind_test_rig_destroy(&rig);
        R_CHECK(&rig, a.live == 0);
    }

    /* Publish cap: two PUBLISH pushes on one connection; pubs=1 forces the
     * second to REJECT on the wire. */
    {
        ca_t a;
        relay_bind_test_ca_init(&a);
        rig_t rig;
        R_CHECK(&rig, relay_bind_test_rig_create_ex(&rig, &a, 0, 0, 1, 0) == MOQR_OK);
        conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
        R_CHECK(&rig, pub != NULL);
        relay_bind_test_rig_pump(&rig, 4);
        moq_bytes_t nsp[2] = { B("cap"), B("p") };
        moq_publish_namespace_cfg_t pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ nsp, 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg,
                                                    rig.now, &ann) ==
                          MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        peer_state_t pub_ps;
        memset(&pub_ps, 0, sizeof(pub_ps));
        moq_publish_cfg_t pubc;
        memset(&pubc, 0, sizeof(pubc));
        moq_publish_cfg_init(&pubc);
        pubc.track_namespace = (moq_namespace_t){ nsp, 2 };
        pubc.track_name = B("p1");
        moq_publication_t ph1;
        R_CHECK(&rig, moq_session_publish(pub->peer, &pubc, rig.now,
                                          &ph1) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        pubc.track_name = B("p2");
        moq_publication_t ph2;
        R_CHECK(&rig, moq_session_publish(pub->peer, &pubc, rig.now,
                                          &ph2) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
        R_CHECK(&rig, pub_ps.publish_ok);      /* first accepted     */
        R_CHECK(&rig, pub_ps.publish_error);   /* second refused     */
        /* A full publish table is a capacity refusal (d18 Section 10.6). */
        R_CHECK(&rig, pub_ps.publish_errors == 1 &&
                          pub_ps.publish_error_code ==
                              MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
        R_CHECK(&rig, pub_ps.publish_error_can_retry &&
                          pub_ps.publish_error_retry_ms >= 750 &&
                          pub_ps.publish_error_retry_ms <= 1250);
        failures += rig.failures;
        relay_bind_test_rig_destroy(&rig);
        R_CHECK(&rig, a.live == 0);
    }

    /* Upstream-sub cap: two subscriptions to distinct announced-but-
     * unpublished tracks; usubs=1 forces the second's upstream to fail,
     * surfacing as SUBSCRIBE_ERROR downstream (not a lost subscription). */
    {
        ca_t a;
        relay_bind_test_ca_init(&a);
        rig_t rig;
        R_CHECK(&rig, relay_bind_test_rig_create_ex(&rig, &a, 0, 1, 0, 0) == MOQR_OK);
        conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
        conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
        R_CHECK(&rig, pub && sub);
        relay_bind_test_rig_pump(&rig, 4);
        moq_bytes_t nsp[2] = { B("cap"), B("u") };
        moq_publish_namespace_cfg_t pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ nsp, 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg,
                                                    rig.now, &ann) ==
                          MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        peer_state_t sub_ps;
        memset(&sub_ps, 0, sizeof(sub_ps));
        moq_subscribe_cfg_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ nsp, 2 };
        scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        scfg.track_name = B("t1");
        moq_subscription_t h1;
        R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now,
                                            &h1) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4); /* t1 takes the one upstream slot (parks)    */
        scfg.track_name = B("t2");
        moq_subscription_t h2;
        R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now,
                                            &h2) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 6); /* t2's upstream cannot be tracked -> ERROR  */
        relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
        R_CHECK(&rig, sub_ps.subscribe_errors == 1);
        /* A full upstream table is a capacity refusal (d18 Section 10.6). */
        R_CHECK(&rig, sub_ps.sub_error_code ==
                          MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
        R_CHECK(&rig, relay_bind_test_sub_reason_eq(&sub_ps, "excessive load"));
        R_CHECK(&rig, sub_ps.sub_error_can_retry &&
                          sub_ps.sub_error_retry_ms >= 750 &&
                          sub_ps.sub_error_retry_ms <= 1250);
        failures += rig.failures;
        relay_bind_test_rig_destroy(&rig);
        R_CHECK(&rig, a.live == 0);
    }

    if (failures == 0) {
        printf("PASS: small_caps_flow\n");
    }
    return failures;
}

/* Shared fixture for the ring-pressure tests. With max_subs=2,
 * max_ns_subs=1, max_ns_nodes=2 the ring clamps to 2 (the largest single
 * atomic reservation). We build: a watcher on prefix {"n"}, a publisher of
 * {"n"} with two subscribers parked on {"n"}/"t" pending the relay's
 * upstream, and capture that upstream unaccepted. Two drivers exercise it:
 *   - teardown: close the publisher. binding_close is resumable — it rejects
 *     the parked subs and fans out NS_GONE across as many retries as the
 *     2-slot ring needs; every withdrawal must still land.
 *   - ring-full: accept the captured upstream. That fans out 2 ACCEPT_SUB
 *     (== ring size) atomically, so a later-slot connection's control
 *     request in the SAME pump meets a full ring and must drain+retry
 *     rather than drop.
 */
typedef struct ring_arm {
    conn_t *watch;
    conn_t *pub;
    conn_t *s1;
    conn_t *s2;
    moq_subscription_t up;   /* relay's upstream subscription (captured) */
    bool               up_valid;
} ring_arm_t;

static moqr_result_t
ring_rig_create(rig_t *r, ca_t *a, uint32_t ns_nodes)
{
    memset(r, 0, sizeof(*r));
    r->alloc = a;
    r->now = 1;
    if (moqr_trace_create(&a->vt, 512, &r->trace) != MOQR_OK) {
        return MOQR_ERR_NOMEM;
    }
    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a->vt);
    cfg.trace = r->trace;
    cfg.max_subs = 2;
    cfg.max_ns_subs = 1;
    cfg.max_ns_nodes = ns_nodes;
    cfg.max_intents = 1;   /* clamps up to the atomic-reservation min:
                            * max_subs + 1 (a track's terminals + its
                            * upstream release) == 3 here */
    cfg.log_budget.max_groups = 4;
    if (moqr_core_create(&cfg, &r->core) != MOQR_OK) {
        moqr_trace_destroy(r->trace);
        return MOQR_ERR_NOMEM;
    }
    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), &a->vt);
    bcfg.core = r->core;
    bcfg.max_conns = MAX_CONNS;
    if (moqr_bind_create(&bcfg, &r->bind) != MOQR_OK) {
        moqr_core_destroy(r->core);
        moqr_trace_destroy(r->trace);
        return MOQR_ERR_NOMEM;
    }
    /* Tight clamp: max(2, max_subs + 1, max_ns_subs, max_ns_nodes) = 3. */
    moqr_core_limits_t lim;
    moqr_core_get_limits(r->core, &lim);
    R_CHECK(r, lim.max_intents == 3);
    return MOQR_OK;
}

static void
ring_arm(rig_t *r, ring_arm_t *out)
{
    out->watch = relay_bind_test_rig_connect(r, MOQ_VERSION_DRAFT_18);
    out->pub = relay_bind_test_rig_connect(r, MOQ_VERSION_DRAFT_18);
    out->s1 = relay_bind_test_rig_connect(r, MOQ_VERSION_DRAFT_18);
    out->s2 = relay_bind_test_rig_connect(r, MOQ_VERSION_DRAFT_18);
    R_CHECK(r, out->watch && out->pub && out->s1 && out->s2);
    relay_bind_test_rig_pump(r, 4);

    moq_bytes_t pfx[1] = { B("n") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ pfx, 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(r, moq_session_subscribe_namespace(out->watch->peer, &nscfg,
                                               r->now, &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(r, 4);

    moq_bytes_t nsp[1] = { B("n") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ nsp, 1 };
    moq_announcement_t ann;
    R_CHECK(r, moq_session_publish_namespace(out->pub->peer, &pcfg, r->now,
                                             &ann) == MOQ_OK);
    relay_bind_test_rig_pump(r, 4); /* watcher gets NS_FOUND; drain it */
    peer_state_t w0;
    memset(&w0, 0, sizeof(w0));
    relay_bind_test_peer_drain(r, out->watch, &w0, false, false, 0, 0);
    R_CHECK(r, w0.ns_found == 1);

    conn_t *sc[2] = { out->s1, out->s2 };
    for (int i = 0; i < 2; i++) {
        moq_subscribe_cfg_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ nsp, 1 };
        scfg.track_name = B("t");
        scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        moq_subscription_t sh;
        R_CHECK(r, moq_session_subscribe(sc[i]->peer, &scfg, r->now, &sh) ==
                       MOQ_OK);
        relay_bind_test_rig_pump(r, 4); /* park (upstream stays unanswered)             */
    }
    /* Capture the relay's upstream SUBSCRIBE at the publisher WITHOUT
     * accepting (subs stay parked). The close test ignores it; the
     * accept-fill tests resolve it to fan out 2 ACCEPT_SUB (== ring size). */
    out->up_valid = false;
    moq_event_t evs[8];
    size_t n;
    while ((n = moq_session_poll_events(out->pub->peer, evs, 8)) > 0) {
        for (size_t e = 0; e < n; e++) {
            if (evs[e].kind == MOQ_EVENT_SUBSCRIBE_REQUEST &&
                !out->up_valid) {
                out->up = evs[e].u.subscribe_request.sub;
                out->up_valid = true;
            }
            moq_event_cleanup(&evs[e]);
        }
    }
    R_CHECK(r, out->up_valid);
}

/* Accept the captured upstream at the publisher peer, so the relay's
 * SUBSCRIBE_OK fans out ACCEPT_SUB to the parked subs. */
static void
ring_accept_upstream(rig_t *r, const ring_arm_t *arm)
{
    moq_accept_subscribe_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    moq_accept_subscribe_cfg_init(&cfg);
    R_CHECK(r, moq_session_accept_subscribe(arm->pub->peer, arm->up, &cfg,
                                            r->now) == MOQ_OK);
}

/* binding_close executes (never discards) pending intents even when its
 * fan-out exceeds the ring: the close drains and retries as needed, and the
 * closing publisher's parked-sub rejects + NS_GONE all reach their peers. */
static int
teardown_pending_intent_flow(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (ring_rig_create(&rig, &a, 2) != MOQR_OK) {
        printf("FAIL: teardown rig create\n");
        return 1;
    }
    ring_arm_t arm;
    ring_arm(&rig, &arm);

    (void)moq_session_on_transport_close(arm.pub->rsess, 0, rig.now);
    rig.now += 1000;
    (void)moqr_bind_pump(rig.bind, rig.now);
    relay_bind_test_rig_pump(&rig, 6);

    peer_state_t w, p1, p2;
    memset(&w, 0, sizeof(w));
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    relay_bind_test_peer_drain(&rig, arm.watch, &w, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, arm.s1, &p1, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, arm.s2, &p2, false, false, 0, 0);
    R_CHECK(&rig, w.ns_gone == 1);            /* withdrawal delivered      */
    R_CHECK(&rig, p1.subscribe_errors == 1);  /* parked sub rejected       */
    R_CHECK(&rig, p2.subscribe_errors == 1);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: teardown leak: %ld\n", a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: teardown_pending_intent_flow\n");
    }
    return f;
}

/* Ring-full TRACK_STATUS_REQUEST must not be dropped: resolving the parked
 * subs fans out 2 ACCEPT_SUB (== ring size) in the same pump the asker's
 * status request is translated, so the request meets a full ring and must
 * drain+retry rather than drop. */
static int
ring_full_track_status_flow(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (ring_rig_create(&rig, &a, 2) != MOQR_OK) {
        printf("FAIL: track-status rig create\n");
        return 1;
    }
    ring_arm_t arm;
    ring_arm(&rig, &arm);
    conn_t *asker = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, asker != NULL);
    relay_bind_test_rig_pump(&rig, 2);

    /* Same pump: accept upstream (2 ACCEPT_SUB fill the ring) + asker
     * TRACK_STATUS. asker is a higher slot than the publisher. */
    ring_accept_upstream(&rig, &arm);
    moq_bytes_t nsp[1] = { B("n") };
    moq_track_status_cfg_t tsc;
    memset(&tsc, 0, sizeof(tsc));
    moq_track_status_cfg_init(&tsc);
    tsc.track_namespace = (moq_namespace_t){ nsp, 1 };
    tsc.track_name = B("t");
    moq_track_status_handle_t tsh;
    R_CHECK(&rig, moq_session_track_status(asker->peer, &tsc, rig.now,
                                           &tsh) == MOQ_OK);
    rig_transport_only(&rig);
    rig.now += 1000;
    (void)moqr_bind_pump(rig.bind, rig.now);
    relay_bind_test_rig_pump(&rig, 6);

    peer_state_t ask_ps;
    memset(&ask_ps, 0, sizeof(ask_ps));
    relay_bind_test_peer_drain(&rig, asker, &ask_ps, false, false, 0, 0);
    /* The track resolved ACTIVE, so the answer is TRACK_STATUS_OK — an
     * answer, not a dropped request. */
    R_CHECK(&rig, ask_ps.ts_ok);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: track-status ring leak: %ld\n", a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: ring_full_track_status_flow\n");
    }
    return f;
}

/* Ring-full PUBLISH_REQUEST must not be dropped: same fill, a pusher's
 * PUBLISH meets the full ring and is answered. */
static int
ring_full_publish_flow(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (ring_rig_create(&rig, &a, 2) != MOQR_OK) {
        printf("FAIL: publish rig create\n");
        return 1;
    }
    ring_arm_t arm;
    ring_arm(&rig, &arm);
    conn_t *pusher = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pusher != NULL);
    relay_bind_test_rig_pump(&rig, 2);

    ring_accept_upstream(&rig, &arm);
    moq_bytes_t nsp2[1] = { B("p") };
    moq_publish_cfg_t pubc;
    memset(&pubc, 0, sizeof(pubc));
    moq_publish_cfg_init(&pubc);
    pubc.track_namespace = (moq_namespace_t){ nsp2, 1 };
    pubc.track_name = B("push");
    moq_publication_t ph;
    R_CHECK(&rig, moq_session_publish(pusher->peer, &pubc, rig.now, &ph) ==
                      MOQ_OK);
    rig_transport_only(&rig);
    rig.now += 1000;
    (void)moqr_bind_pump(rig.bind, rig.now);
    relay_bind_test_rig_pump(&rig, 6);

    peer_state_t push_ps;
    memset(&push_ps, 0, sizeof(push_ps));
    relay_bind_test_peer_drain(&rig, pusher, &push_ps, false, false, 0, 0);
    R_CHECK(&rig, push_ps.publish_ok);   /* answered despite full ring */

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: publish ring leak: %ld\n", a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: ring_full_publish_flow\n");
    }
    return f;
}

/* Shutdown safety: moqr_bind_destroy is strictly memory-only (no session
 * and no core calls). The transport (and its sessions) is destroyed FIRST,
 * with relay work still pending, then the binding — proving destroy never
 * reaches into a freed session. ASan-clean under this ordering is the
 * regression lock. */
static int
shutdown_no_session_flow(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: shutdown rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[1] = { B("n") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ nsp, 1 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    /* Drive a subscribe so intents (UPSTREAM_SUBSCRIBE / ACCEPT_SUB) are in
     * flight, then STOP pumping — leave work pending in the core ring. */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ nsp, 1 };
    scfg.track_name = B("t");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    rig_transport_only(&rig);
    (void)moqr_bind_pump(rig.bind, rig.now);   /* queues an upstream sub */

    /* Simulate adapter teardown: destroy the SimPairs (freeing all
     * sessions) BEFORE the binding. A binding that touched sessions in
     * destroy would use-after-free here (caught by ASan). */
    for (int i = 0; i < MAX_CONNS; i++) {
        if (rig.conns[i].used) {
            moq_simpair_destroy(rig.conns[i].sp);
            rig.conns[i].used = false;
        }
    }
    moqr_bind_destroy(rig.bind);   /* must make ZERO session calls */
    rig.bind = NULL;
    moqr_core_destroy(rig.core);
    rig.core = NULL;
    moqr_trace_destroy(rig.trace);
    rig.trace = NULL;

    if (a.live != 0) {
        printf("FAIL: shutdown leak: %ld\n", a.live);
        return 1;
    }
    printf("PASS: shutdown_no_session_flow\n");
    return 0;
}

/* Latency accounting records one observation per successful delivery even
 * when the delivery clock is BEHIND the record's arrival time — the latency
 * saturates to 0us instead of the observation being dropped, so
 * forward_latency.count stays equal to deliveries_written. The monotonic wire
 * flow can't deliver before ingest, so this ingests a FUTURE-arrival record
 * directly into an active track and delivers it through the binding at the
 * current (lower) clock. */
static int
test_latency_clock_regression(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: regression rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[1] = { B("reg") };
    if (sub == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_18,
                                     (moqr_ns_t){ nsp, 1 }, B("t"))) {
        goto cleanup;
    }

    /* A paused core observer gives this clock-injection fixture the real
     * track handle without manufacturing a publisher or metadata. It cannot
     * contribute a successful delivery to the latency counters. */
    moqr_binding_t observer = MOQR_BINDING_INVALID;
    moqr_result_t rc = moqr_core_binding_open(rig.core, 999, &observer);
    R_CHECK(&rig, rc == MOQR_OK);
    if (rc != MOQR_OK) {
        goto cleanup;
    }
    moqr_subscribe_req_t req;
    moqr_subscribe_req_init(&req);
    req.ns = (moqr_ns_t){ nsp, 1 };
    req.name = B("t");
    req.forward = false;
    moqr_sub_t observed = MOQR_SUB_INVALID;
    rc = moqr_core_subscribe(rig.core, observer, &req, &observed);
    R_CHECK(&rig, rc == MOQR_OK);
    if (rc != MOQR_OK) {
        goto cleanup;
    }
    moqr_track_t track = MOQR_TRACK_INVALID;
    uint64_t track_gen = 0;
    rc = moqr_core_sub_track(rig.core, observed, &track, &track_gen);
    R_CHECK(&rig, rc == MOQR_OK);
    if (rc != MOQR_OK) {
        goto cleanup;
    }

    /* Subscriber subscribes; the track is already ACTIVE so it resolves. */
    moq_bytes_t snsp[1] = { B("reg") };
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = snsp, .count = 1 };
    scfg.track_name = B("t");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    scfg.start_group = 0;
    scfg.start_object = 0;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.subscribe_ok && ps.subscribe_errors == 0 &&
                      !ps.session_closed);
    if (!ps.subscribe_ok || ps.subscribe_errors != 0 || ps.session_closed) {
        goto cleanup;
    }

    /* Ingest one object whose arrival_us is far in the FUTURE. */
    uint8_t body[16];
    memset(body, 0xAB, sizeof(body));
    moq_rcbuf_t *pl = NULL;
    moq_result_t mrc = moq_rcbuf_create(&a.vt, body, sizeof(body), &pl);
    R_CHECK(&rig, mrc == MOQ_OK);
    if (mrc != MOQ_OK) {
        goto cleanup;
    }
    moqr_log_append_desc_t d;
    moqr_log_append_desc_init(&d);
    d.group_id = 1;
    d.subgroup_id = 0;
    d.object_id = 0;
    d.publisher_priority = 1;
    d.payload = pl;
    d.now_us = rig.now + 10000000;   /* 10s ahead of the delivery clock */
    if (moqr_core_ingest(rig.core, track, &d) != MOQR_OK) {
        moq_rcbuf_decref(pl);
        R_CHECK(&rig, 0);
        goto cleanup;
    }

    /* Deliver at the current (lower) clock: the observation saturates to 0. */
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.objects == 1 && ps.last_group == 1 && ps.last_object == 0 &&
                      ps.last_len == sizeof(body) && ps.last_byte == 0xAB);

    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.deliveries_written == 1);
    R_CHECK(&rig, bs.forward_latency.count == bs.deliveries_written);
    R_CHECK(&rig, bs.forward_latency.sum_us == 0);   /* all saturated to 0 */
    R_CHECK(&rig, bs.forward_latency.bucket[0] == bs.forward_latency.count);

    R_CHECK(&rig, moqr_core_unsubscribe(rig.core, observed, rig.now) == MOQR_OK);
    R_CHECK(&rig, moqr_core_binding_close(rig.core, observer, rig.now) == MOQR_OK);

cleanup:
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: latency_clock_regression\n");
    }
    return f;
}

/* An inbound publisher finishing a PUBLISH must release the
 * relay's active-source state. Without the PUBLISH_FINISHED handler, a second
 * PUBLISH for the same track is rejected as a duplicate active source. */
static int
test_publish_finished_releases_track(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: publish-finished rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t ns[1] = { B("finish") };
    moq_publish_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = ns, .count = 1 };
    pc.track_name = B("push");

    moq_publication_t p1;
    R_CHECK(&rig, moq_session_publish(pub->peer, &pc, rig.now, &p1) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.publish_ok);
    R_CHECK(&rig, !ps.publish_error);

    moq_finish_publish_cfg_t fc;
    memset(&fc, 0, sizeof(fc));
    moq_finish_publish_cfg_init(&fc);
    R_CHECK(&rig, moq_session_finish_publish(pub->peer, p1, &fc, rig.now) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);

    moq_publication_t p2;
    R_CHECK(&rig, moq_session_publish(pub->peer, &pc, rig.now, &p2) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.publish_ok);
    R_CHECK(&rig, !ps.publish_error);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: publish_finished_releases_track\n");
    }
    return f;
}

/* Request types the relay does not yet implement must receive a
 * terminal wire error. Falling through bind_on_event's default leaves the peer
 * waiting forever. */
static int
test_unsupported_requests_reject(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: unsupported rig create\n");
        return 1;
    }
    conn_t *cn = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, cn != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t ns[1] = { B("unsupported") };

    moq_fetch_cfg_t fc;
    memset(&fc, 0, sizeof(fc));
    moq_fetch_cfg_init(&fc);
    fc.track_namespace = (moq_namespace_t){ .parts = ns, .count = 1 };
    fc.track_name = B("fetch");
    fc.start_group = 0;
    fc.start_object = 0;
    fc.end_group = 1;
    fc.end_object = 0;
    moq_fetch_t fh;
    R_CHECK(&rig, moq_session_fetch(cn->peer, &fc, rig.now, &fh) == MOQ_OK);

    moq_subscribe_tracks_cfg_t stc;
    memset(&stc, 0, sizeof(stc));
    moq_subscribe_tracks_cfg_init(&stc);
    stc.track_namespace_prefix =
        (moq_namespace_t){ .parts = ns, .count = 1 };
    moq_track_sub_handle_t sth;
    R_CHECK(&rig, moq_session_subscribe_tracks(cn->peer, &stc, rig.now, &sth) ==
                      MOQ_OK);

    relay_bind_test_rig_pump(&rig, 8);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, cn, &ps, false, false, 0, 0);
    /* FETCH is wired now: a fetch for a track with no publisher rejects
     * DOES_NOT_EXIST (the unknown-track terminal), not the old blanket
     * NOT_SUPPORTED. SUBSCRIBE_TRACKS is still unsupported. */
    R_CHECK(&rig, ps.fetch_error == 1);
    R_CHECK(&rig, ps.fetch_error_code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    R_CHECK(&rig, ps.tracks_error == 1);
    R_CHECK(&rig, ps.tracks_error_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: unsupported_requests_reject\n");
    }
    return f;
}

/* When the origin ends the relay's upstream subscription
 * (SUBSCRIBE_DONE), the relay must terminate downstream subscribers on the wire
 * and release the upstream — no zombie ACTIVE track, no hung subscriber. The
 * subscriber uses an open-ended (LARGEST_OBJECT) filter, which never self-
 * completes, so only an explicit wire DONE keeps it from hanging. */
static int
test_upstream_done_terminates_downstream(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: upstream-done rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL && sub != NULL);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT; /* open-ended */
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, true, 0, 0); /* accept upstream */
    R_CHECK(&rig, pub_ps.up_seen && pub_ps.up_subs == 1);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.subscribe_ok);
    R_CHECK(&rig, !sub_ps.done_seen);

    /* Origin ends the upstream subscription with a specific status code. */
    moq_done_subscribe_cfg_t dc;
    moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x4u;
    R_CHECK(&rig, moq_session_done_subscribe(pub->peer, pub_ps.up_sub, &dc,
                                             rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.done_seen);         /* terminal DONE, not a hang */
    R_CHECK(&rig, sub_ps.done_code == 0x4u); /* origin's status forwarded */

    /* Upstream tracking slot released: a fresh subscribe re-triggers a NEW
     * upstream SUBSCRIBE (the publisher is still announced). */
    conn_t *sub2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, sub2 != NULL);
    peer_state_t sub2_ps;
    memset(&sub2_ps, 0, sizeof(sub2_ps));
    relay_bind_test_rig_pump(&rig, 4);
    moq_subscription_t sh2;
    R_CHECK(&rig, moq_session_subscribe(sub2->peer, &sc, rig.now, &sh2) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, true, 0, 0); /* accept the NEW upstream */
    R_CHECK(&rig, pub_ps.up_subs == 2);               /* a second upstream sub */
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub2, &sub2_ps, false, false, 0, 0);
    R_CHECK(&rig, sub2_ps.subscribe_ok); /* rejoin path works */

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: upstream_done_terminates_downstream\n");
    }
    return f;
}

/* Symmetry: a push publisher finishing (PUBLISH_FINISHED) is
 * the same "source ended" contract as an upstream SUBSCRIBE_DONE — its
 * downstream subscribers must get a wire SUBSCRIBE_DONE, not be left hanging on
 * a WARM track. The subscriber uses an open-ended filter so only a wire DONE
 * saves it. (test_publish_finished_releases_track covers republish alone; this
 * adds the downstream-terminal case.) */
static int
test_publish_finished_terminates_downstream(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: publish-finished-terminates rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL && sub != NULL);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    /* Publisher pushes a track (creates the ACTIVE source). */
    moq_bytes_t nsp[1] = { B("pushed") };
    moq_publish_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 1 };
    pc.track_name = B("track");
    moq_publication_t p1;
    R_CHECK(&rig, moq_session_publish(pub->peer, &pc, rig.now, &p1) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.publish_ok);

    /* Downstream subscribes open-ended to the pushed track. */
    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 1 };
    sc.track_name = B("track");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT; /* open-ended */
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.subscribe_ok);
    R_CHECK(&rig, !sub_ps.done_seen);

    /* Publisher finishes the push with a specific status code. */
    moq_finish_publish_cfg_t fc;
    memset(&fc, 0, sizeof(fc));
    moq_finish_publish_cfg_init(&fc);
    fc.status_code = 0x4u;
    R_CHECK(&rig, moq_session_finish_publish(pub->peer, p1, &fc, rig.now) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.done_seen);         /* terminal DONE, not a hang */
    R_CHECK(&rig, sub_ps.done_code == 0x4u); /* finish status forwarded */

    /* The source is released: the same track can be pushed again. */
    moq_publication_t p2;
    R_CHECK(&rig, moq_session_publish(pub->peer, &pc, rig.now, &p2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&pub_ps, 0, sizeof(pub_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.publish_ok && !pub_ps.publish_error);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: publish_finished_terminates_downstream\n");
    }
    return f;
}

/* A REDIRECT rejection of the relay's upstream SUBSCRIBE (before
 * OK) is terminal — the relay does not follow the redirect, so parked downstream
 * subscribers must be rejected on the wire (GOING_AWAY), not left waiting. */
static int
test_upstream_redirect_rejects_downstream(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: upstream-redirect rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL && sub != NULL);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    /* Capture the relay's upstream SUBSCRIBE at the origin WITHOUT accepting it,
     * then reject it with REDIRECT. */
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_subs == 1);
    moq_reject_subscribe_cfg_t rj;
    moq_reject_subscribe_cfg_init(&rj);
    rj.error_code = MOQ_REQUEST_ERROR_REDIRECT;
    /* All-empty redirect target: a client REDIRECT must carry a zero-length URI
     * (reuse current) — still surfaces REQUEST_REDIRECT at the relay. */
    R_CHECK(&rig, moq_session_reject_subscribe(pub->peer, pub_ps.up_sub, &rj,
                                               rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, !sub_ps.subscribe_ok);
    R_CHECK(&rig, sub_ps.subscribe_errors == 1); /* parked downstream rejected */
    /* REQUEST_ERROR INTERNAL_ERROR: the relay carries no Redirect structure
     * and follows no redirect, so it can neither restate REDIRECT nor claim a
     * TIMEOUT that never elapsed. 0x0 is assigned identically in both
     * registries. */
    R_CHECK(&rig, sub_ps.sub_error_code == 0x0u);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: upstream_redirect_rejects_downstream\n");
    }
    return f;
}

/* A per-request GOAWAY on the relay's ESTABLISHED upstream
 * SUBSCRIBE is terminal — the relay does not migrate the request, so its active
 * (open-ended) downstream subscribers must receive a wire SUBSCRIBE_DONE
 * (GOING_AWAY), not hang. */
static int
test_upstream_goaway_terminates_downstream(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: upstream-goaway rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL && sub != NULL);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT; /* open-ended */
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, true, 0, 0); /* accept upstream */
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.subscribe_ok);
    R_CHECK(&rig, !sub_ps.done_seen);

    /* Origin sends a per-request GOAWAY on the established upstream sub. */
    moq_request_goaway_cfg_t gc;
    moq_request_goaway_cfg_init(&gc);
    R_CHECK(&rig, moq_session_request_goaway_subscribe(pub->peer, pub_ps.up_sub,
                                                       &gc, rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.done_seen);         /* terminal DONE, not a hang */
    /* PUBLISH_DONE GOING_AWAY (0x4): the upstream publisher issued a GOAWAY —
     * the status registry's own value, not REQUEST_ERROR's. */
    R_CHECK(&rig, sub_ps.done_code == 0x4u);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: upstream_goaway_terminates_downstream\n");
    }
    return f;
}

/* -- The deferred-output ring must never hold a borrowed-payload intent ------- */

/* Advance ONE connection's transport (deliver its queued wire bytes to the
 * relay) WITHOUT pumping the binding — so several peer messages can be staged
 * into a single subsequent bind pump. */
static void
deliver_without_pump(rig_t *r, conn_t *cn)
{
    r->now += 1000;
    (void)moq_simpair_advance_to(cn->sp, r->now);
    size_t steps = 0;
    (void)moq_simpair_run_until_quiescent(cn->sp, 64, &steps);
}

/* Bring one track "live"/<ns2>/"v" to WARM with a retained record: announce,
 * downstream subscribe, upstream accept + one object, downstream unsubscribe,
 * then let the linger expire (a tick warms the track, record kept). A later
 * subscribe to a WARM track is the fan-out that emits ACCEPT_SUB THEN
 * UPSTREAM_SUBSCRIBE (relay.c:1935/1952) — the scalar-then-borrowed pairing. */
static void
warm_track(rig_t *r, conn_t *pub, conn_t *sub, moq_bytes_t ns2)
{
    moq_bytes_t nsp[2] = { B("live"), ns2 };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(r, moq_session_publish_namespace(pub->peer, &pcfg, r->now, &ann) ==
                   MOQ_OK);
    relay_bind_test_rig_pump(r, 16);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t sh;
    R_CHECK(r, moq_session_subscribe(sub->peer, &scfg, r->now, &sh) == MOQ_OK);
    relay_bind_test_rig_pump(r, 20);

    peer_state_t pub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    relay_bind_test_peer_drain(r, pub, &pub_ps, true, false, 0, 0); /* accept the upstream */
    R_CHECK(r, pub_ps.up_seen);
    relay_bind_test_rig_pump(r, 20);

    /* Origin writes one object so the track log keeps a record (survives WARM). */
    moq_subgroup_cfg_t sgc;
    moq_subgroup_cfg_init(&sgc);
    sgc.group_id = 0;
    sgc.subgroup_id = 0;
    sgc.publisher_priority = 0;
    moq_subgroup_handle_t sgh;
    R_CHECK(r, moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sgc, r->now,
                                         &sgh) == MOQ_OK);
    uint8_t body[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    moq_rcbuf_t *pl = NULL;
    R_CHECK(r, moq_rcbuf_create(&r->alloc->vt, body, sizeof(body), &pl) ==
                   MOQ_OK);
    R_CHECK(r, moq_session_write_object(pub->peer, sgh, 0, pl, r->now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    (void)moq_session_close_subgroup(pub->peer, sgh, r->now);
    relay_bind_test_rig_pump(r, 20);

    /* Downstream leaves; linger (500us) expires within a 1000us cycle, so the
     * next tick transitions the track to WARM with its record retained. */
    R_CHECK(r, moq_session_unsubscribe(sub->peer, sh, r->now) == MOQ_OK);
    relay_bind_test_rig_pump(r, 20);
}

/* A warm-track rejoin emits ACCEPT_SUB then UPSTREAM_SUBSCRIBE in one fan-out;
 * the UPSTREAM_SUBSCRIBE's ns_parts BORROW the track key (relay.h:102), valid
 * only until the next mutating core call. If the ACCEPT blocks on a full
 * downstream queue, the binding must execute the borrowed UPSTREAM inline and
 * NEVER park it across pumps. We stage two warm-track rejoins into a single
 * pump on a depth-1 subscriber: the first rejoin's ACCEPT fills the queue, the
 * second's ACCEPT WOULD_BLOCKs, leaving the second's UPSTREAM_SUBSCRIBE as a
 * borrowed intent sitting behind a blocked scalar in the same drain. The
 * binding's pending_nonscalar_blocked telemetry (0 iff pending[] only ever held
 * scalar-safe kinds) is the discriminating invariant. */
static int
borrowed_intent_never_parked(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_actions = 0;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: borrowed_remainder rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* roomy */
    conn_t *warm = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* roomy: warms   */
    relay_bind_test_max_actions = 1;                         /* depth-1 rejoin */
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 0;
    R_CHECK(&rig, pub && warm && sub);
    relay_bind_test_rig_pump(&rig, 12);

    /* Warm the tracks with a ROOMY subscriber (WARM is a track-side state, so a
     * different, depth-1 subscriber can later rejoin it — this decouples the
     * easy setup from the depth-1 pressure the rejoin needs). */
    warm_track(&rig, pub, warm, B("a"));
    warm_track(&rig, pub, warm, B("b"));

    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.pending_nonscalar_blocked == 0);   /* clean after setup   */

    /* Stage BOTH warm-rejoin subscribes (from the depth-1 sub) into the relay
     * before it pumps. deliver_without_pump drains the depth-1 peer queue between them. */
    moq_subscribe_cfg_t rc;
    memset(&rc, 0, sizeof(rc));
    moq_subscribe_cfg_init(&rc);
    rc.track_name = B("v");
    rc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;

    moq_bytes_t nsa[2] = { B("live"), B("a") };
    rc.track_namespace = (moq_namespace_t){ .parts = nsa, .count = 2 };
    moq_subscription_t rha;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &rc, rig.now, &rha) == MOQ_OK);
    deliver_without_pump(&rig, sub);

    moq_bytes_t nsb[2] = { B("live"), B("b") };
    rc.track_namespace = (moq_namespace_t){ .parts = nsb, .count = 2 };
    moq_subscription_t rhb;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &rc, rig.now, &rhb) == MOQ_OK);
    deliver_without_pump(&rig, sub);

    /* One pump processes both rejoins on a full depth-1 downstream queue. */
    (void)moqr_bind_pump(rig.bind, rig.now);

    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.pending_nonscalar_blocked == 0);   /* THE invariant       */
    R_CHECK(&rig, bs.pending_high_water > 0);           /* a scalar DID block  */

    /* Drain to quiescence (retries any parked scalar; a parked borrowed
     * UPSTREAM_SUBSCRIBE would dangle here — ASan-visible). */
    relay_bind_test_rig_pump(&rig, 40);
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.pending_nonscalar_blocked == 0);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: borrowed_intent_never_parked\n");
    }
    return f;
}

#define REENTRY_NS_WATCHERS 33u
#define REENTRY_STATUS_INTENTS 33u
#define REENTRY_STATUS_COOKIE_BASE 0x4E700000u

typedef struct intent_reentry_router_rec {
    uint64_t status_cookie[REENTRY_STATUS_INTENTS + 4u];
    uint32_t status_n;
    uint32_t ns_found;
    uint32_t ns_gone;
    uint32_t other;
    char     order[96];
    uint32_t order_len;
} intent_reentry_router_rec_t;

static void
intent_reentry_router_reset(intent_reentry_router_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
}

static void
intent_reentry_router_mark(intent_reentry_router_rec_t *rec, char c)
{
    if (rec->order_len + 1u < sizeof(rec->order)) {
        rec->order[rec->order_len++] = c;
        rec->order[rec->order_len] = '\0';
    }
}

static bool
intent_reentry_router(void *ctx, const moqr_intent_t *it, uint64_t now_us)
{
    (void)now_us;
    intent_reentry_router_rec_t *rec = ctx;
    switch (it->kind) {
    case MOQR_INTENT_TRACK_STATUS_ERROR:
        if (rec->status_n <
            (uint32_t)(sizeof(rec->status_cookie) /
                       sizeof(rec->status_cookie[0]))) {
            rec->status_cookie[rec->status_n] = it->cookie;
        }
        rec->status_n++;
        intent_reentry_router_mark(rec, 'E');
        break;
    case MOQR_INTENT_NS_FOUND:
        rec->ns_found++;
        intent_reentry_router_mark(rec, 'N');
        break;
    case MOQR_INTENT_NS_GONE:
        rec->ns_gone++;
        intent_reentry_router_mark(rec, 'G');
        break;
    default:
        rec->other++;
        intent_reentry_router_mark(rec, '?');
        break;
    }
    return true;
}

static moqr_result_t
intent_reentry_rig_create(rig_t *r, ca_t *a,
                          intent_reentry_router_rec_t *rec)
{
    memset(r, 0, sizeof(*r));
    r->alloc = a;
    r->now = 1;
    if (moqr_trace_create(&a->vt, 512, &r->trace) != MOQR_OK) {
        return MOQR_ERR_NOMEM;
    }
    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a->vt);
    cfg.trace = r->trace;
    cfg.max_bindings = 80;
    cfg.max_tracks = 4;
    cfg.max_subs = 2;
    cfg.max_ns_nodes = 8;
    cfg.max_ns_subs = REENTRY_NS_WATCHERS;
    cfg.max_intents = REENTRY_STATUS_INTENTS + 1u;
    cfg.log_budget.max_groups = 4;
    cfg.log_budget.max_bytes = 1u << 20;
    cfg.linger_us = 500;
    if (moqr_core_create(&cfg, &r->core) != MOQR_OK) {
        moqr_trace_destroy(r->trace);
        return MOQR_ERR_NOMEM;
    }
    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), &a->vt);
    bcfg.core = r->core;
    bcfg.max_conns = MAX_CONNS;
    bcfg.max_downstream_subs = 2;
    bcfg.max_upstream_subs = 2;
    bcfg.max_publishes = 2;
    bcfg.max_announces = 2;
    bcfg.max_open_subgroups = 4;
    bcfg.router = intent_reentry_router;
    bcfg.router_ctx = rec;
    if (moqr_bind_create(&bcfg, &r->bind) != MOQR_OK) {
        moqr_core_destroy(r->core);
        moqr_trace_destroy(r->trace);
        return MOQR_ERR_NOMEM;
    }
    moqr_core_limits_t lim;
    moqr_core_get_limits(r->core, &lim);
    R_CHECK(r, lim.max_intents == REENTRY_STATUS_INTENTS + 1u);
    R_CHECK(r, lim.max_ns_subs == REENTRY_NS_WATCHERS);
    return MOQR_OK;
}

static void
intent_reentry_install_watchers(rig_t *r, moqr_binding_t watchers[])
{
    moq_bytes_t pfx[1] = { B("dead") };
    moqr_ns_t prefix = { .parts = pfx, .count = 1 };
    for (uint32_t i = 0; i < REENTRY_NS_WATCHERS; i++) {
        uint64_t cookie = MOQR_SHARD_COOKIE_BASE + 0x100u + i;
        R_CHECK(r, moqr_core_binding_open(r->core, cookie, &watchers[i]) ==
                       MOQR_OK);
        R_CHECK(r, moqr_core_ns_subscribe(r->core, watchers[i], prefix,
                                          0xD000u + i) == MOQR_OK);
    }
}

typedef struct detach_pending_observation {
    int           calls;
    moqr_result_t close_rc;
    bool          open_after_close;
} detach_pending_observation_t;

static void
observe_detach_pending(moqr_bind_t *bind, moq_session_t *session, void *ctx)
{
    detach_pending_observation_t *obs = ctx;
    obs->calls++;
    obs->close_rc = moqr_bind_conn_close(bind, session);
    obs->open_after_close = moqr_bind_conn_is_open(bind, session);
}

/* The bind-owned intent scratch must not be reused by a retry helper while an
 * outer intent batch is live. This drives the real path:
 *   SUB_DONE -> injected reset failure -> conn_detach -> binding_close WOULD_BLOCK
 * because the core ring still holds the tail of the outer batch and the closing
 * connection owns a namespace with a 33-watcher NS_GONE fanout. The old shared
 * scratch recursively polled that tail into b->pump_intents, overwriting the
 * outer batch; status cookie 0 was skipped and the tail duplicated. */
static int
intent_retry_reentry_preserves_outer_batch(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    intent_reentry_router_rec_t rec;
    intent_reentry_router_reset(&rec);
    relay_bind_test_server_streaming_objects = true;
    relay_bind_test_client_streaming_objects = true;
    rig_t rig;
    if (intent_reentry_rig_create(&rig, &a, &rec) != MOQR_OK) {
        relay_bind_test_server_streaming_objects = false;
        relay_bind_test_client_streaming_objects = false;
        printf("FAIL: intent_retry_reentry rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_server_streaming_objects = false;
    relay_bind_test_client_streaming_objects = false;
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moqr_binding_t watchers[REENTRY_NS_WATCHERS];
    intent_reentry_install_watchers(&rig, watchers);

    /* The future closing connection owns a namespace observed only through the
     * router. Its close therefore has a large, deterministic NS_GONE fanout. */
    moq_bytes_t dead[2] = { B("dead"), B("owned") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = dead, .count = 2 };
    moq_announcement_t dead_ann;
    R_CHECK(&rig, moq_session_publish_namespace(sub->peer, &pcfg, rig.now,
                                                &dead_ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 12);
    R_CHECK(&rig, rec.ns_found == REENTRY_NS_WATCHERS);
    R_CHECK(&rig, rec.ns_gone == 0);
    peer_state_t sub_ctrl;
    memset(&sub_ctrl, 0, sizeof(sub_ctrl));
    relay_bind_test_peer_drain(&rig, sub, &sub_ctrl, false, false, 0, 0);
    R_CHECK(&rig, sub_ctrl.namespace_accepted);
    intent_reentry_router_reset(&rec);

    /* The same connection is also a downstream subscriber with a begun subgroup,
     * so SUB_DONE's reset failure closes that namespace-owning binding. */
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    moq_bytes_t live[2] = { B("live"), B("cam1") };
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, sub, &pub_ps, &sub_ps, live);
    R_CHECK(&rig, !sub_ps.done_seen);

    moq_subgroup_cfg_t sca;
    moq_subgroup_cfg_init(&sca);
    sca.group_id = 1;
    sca.subgroup_id = 0;
    moq_subgroup_handle_t sga;
    R_CHECK(&rig, moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sca,
                                            rig.now, &sga) == MOQ_OK);
    R_CHECK(&rig, moq_session_begin_object(pub->peer, sga, 0, 96, rig.now) ==
                      MOQ_OK);
    {
        uint8_t body[32];
        memset(body, 0xC5, sizeof(body));
        moq_rcbuf_t *pl = NULL;
        R_CHECK(&rig, moq_rcbuf_create(&a.vt, body, sizeof(body), &pl) ==
                          MOQ_OK);
        R_CHECK(&rig,
                moq_session_write_object_data(pub->peer, sga, pl, rig.now) ==
                    MOQ_OK);
        moq_rcbuf_decref(pl);
    }
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.chunks >= 1);

    uint32_t up_slot = 0;
    uint64_t up_handle = 0;
    R_CHECK(&rig, moqr_bind_debug_first_usub(rig.bind, &up_slot, &up_handle));
    moqr_bind_debug_fail_sg_reset(1);
    uint64_t suppressed0 =
        moqr_bind_debug_intent_retry_suppressed(rig.bind);
    detach_pending_observation_t detach_obs;
    memset(&detach_obs, 0, sizeof(detach_obs));
    moqr_bind_debug_on_detach_pending(observe_detach_pending, &detach_obs);
    moqr_bind_debug_upstream_terminated(rig.bind, up_slot,
                                        MOQ_REQUEST_FAMILY_SUBSCRIBE,
                                        up_handle, 0, rig.now);

    moq_bytes_t miss_ns_part[1] = { B("missing") };
    moqr_ns_t miss_ns = { .parts = miss_ns_part, .count = 1 };
    for (uint32_t i = 0; i < REENTRY_STATUS_INTENTS; i++) {
        R_CHECK(&rig, moqr_core_track_status(
                          rig.core, watchers[i], miss_ns, B("track"),
                          REENTRY_STATUS_COOKIE_BASE + i) == MOQR_OK);
    }
    moqr_core_stats_t cs;
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.intent_highwater >= REENTRY_STATUS_INTENTS + 1u);

    rig.now += 1000;
    (void)moqr_bind_pump(rig.bind, rig.now);
    moqr_bind_debug_on_detach_pending(NULL, NULL);

    uint64_t suppressed1 =
        moqr_bind_debug_intent_retry_suppressed(rig.bind);
    R_CHECK(&rig, detach_obs.calls == 1);
    R_CHECK(&rig, detach_obs.close_rc == MOQR_ERR_WOULD_BLOCK);
    R_CHECK(&rig, detach_obs.open_after_close);
    R_CHECK(&rig, suppressed1 == suppressed0 + 1u);
    R_CHECK(&rig, rec.status_n == REENTRY_STATUS_INTENTS);
    for (uint32_t i = 0; i < REENTRY_STATUS_INTENTS &&
                         i < rec.status_n; i++) {
        R_CHECK(&rig, rec.status_cookie[i] == REENTRY_STATUS_COOKIE_BASE + i);
    }
    R_CHECK(&rig, rec.ns_gone == REENTRY_NS_WATCHERS);
    R_CHECK(&rig, rec.other == 0);
    for (uint32_t i = 0; i < rec.status_n && i < sizeof(rec.order); i++) {
        if (i < REENTRY_STATUS_INTENTS) {
            R_CHECK(&rig, rec.order[i] == 'E');
        }
    }
    R_CHECK(&rig, rec.order_len == REENTRY_STATUS_INTENTS +
                                  REENTRY_NS_WATCHERS);

    moqr_bind_debug_fail_sg_reset(0);
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: intent_retry_reentry_preserves_outer_batch\n");
    }
    return f;
}

/* Subgroup-slot reclamation: a downstream subgroup slot pool of 2, fed THREE distinct
 * (group, subgroup) streams each ending in a plain FIN (no EOG). A relay that
 * held a slot per (group, subgroup) until EOG/SUB_DONE would leave the third
 * candidate at `freei < 0` → WOULD_BLOCK forever (only two objects delivered,
 * no terminal). The upstream `MOQ_EVENT_SUBGROUP_FINISHED` seals the log
 * subgroup and the acknowledged SEAL notice — the only durable FIN path
 * (records carry `subgroup_end` as advisory metadata) — closes the
 * downstream stream and reclaims the slot, so all three objects flow.
 * `objects == 3` depends on the seal pipeline: without it delivery stops at
 * two. */
static int
subgroup_slot_reclamation(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_max_open_subgroups = 2; /* tiny downstream subgroup slot pool */
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_max_open_subgroups = 0;
        printf("FAIL: subgroup_slot_reclamation rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    scfg.start_group = 0;
    scfg.start_object = 0;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    peer_state_t pub_ps, s_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* accept upstream */
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 4);

    /* Three distinct (group, subgroup) streams, each a plain FIN (no EOG). */
    relay_bind_test_pub_subgroup_plain_fin(&rig, pub, pub_ps.up_sub, 0, 0, 0);
    relay_bind_test_pub_subgroup_plain_fin(&rig, pub, pub_ps.up_sub, 1, 0, 0);
    relay_bind_test_pub_subgroup_plain_fin(&rig, pub, pub_ps.up_sub, 2, 0, 0);
    relay_bind_test_rig_pump(&rig, 40); /* generous: prove the stall is permanent, not slow */
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);

    /* All three objects reach the subscriber (the pool of 2 was reclaimed twice
     * across three streams), with no error and no premature DONE. */
    R_CHECK(&rig, s_ps.objects == 3);
    R_CHECK(&rig, s_ps.subscribe_errors == 0);
    R_CHECK(&rig, !s_ps.done_seen);

    relay_bind_test_max_open_subgroups = 0;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: subgroup_slot_reclamation\n");
    }
    return f;
}

/* Multi-object and EOG shapes through a pool of 2: a 3-object sealed subgroup
 * must deliver all three BEFORE its stream closes (subgroup_end only on the last
 * record), an EOG subgroup still reclaims, and a further plain-FIN subgroup
 * reclaims after the pool has churned. */
static int
subgroup_reclamation_shapes(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_max_open_subgroups = 2;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_max_open_subgroups = 0;
        printf("FAIL: subgroup_reclamation_shapes rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    scfg.start_group = 0;
    scfg.start_object = 0;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    peer_state_t pub_ps, s_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 4);

    relay_bind_test_pub_subgroup_n(&rig, pub, pub_ps.up_sub, 0, 0, 3, false); /* 3 obj, FIN */
    relay_bind_test_pub_subgroup_n(&rig, pub, pub_ps.up_sub, 1, 0, 0, true); /* EOG only   */
    relay_bind_test_pub_subgroup_n(&rig, pub, pub_ps.up_sub, 2, 0, 1, false); /* 1 obj, FIN */
    relay_bind_test_rig_pump(&rig, 40);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);

    /* 3 + 1 (EOG status) + 1 = 5 objects, all reclaimed through the pool of 2. */
    R_CHECK(&rig, s_ps.objects == 5);
    R_CHECK(&rig, s_ps.subscribe_errors == 0);
    R_CHECK(&rig, !s_ps.done_seen);

    relay_bind_test_max_open_subgroups = 0;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: subgroup_reclamation_shapes\n");
    }
    return f;
}

/* Write one object on an already-open subgroup handle (the stream stays open —
 * no FIN, so no SUBGROUP_FINISHED / seal). */
static void
pub_write_obj(rig_t *r, conn_t *pub, moq_subgroup_handle_t h, uint64_t object_id)
{
    uint8_t body[4] = { (uint8_t)object_id, 0x11, 0x22, 0x33 };
    moq_rcbuf_t *pl = NULL;
    R_CHECK(r, moq_rcbuf_create(&r->alloc->vt, body, sizeof(body), &pl) ==
                   MOQ_OK);
    R_CHECK(r, moq_session_write_object(pub->peer, h, object_id, pl, r->now) ==
                   MOQ_OK);
    moq_rcbuf_decref(pl);
}

/* Eviction cleanup must not close a LIVE lower-group subgroup. Three subgroup
 * streams (groups 0/1/2, each held OPEN — no FIN/EOG) with a log of only 2
 * groups: ingesting group 2 evicts group 0 and arms the sub's eviction jump. A
 * naive cleanup that closed every open subgroup below the next-delivered group
 * would close group 1 (still retained) when the acknowledged EVICT_WATERMARK
 * notice lands — then a later group-1 object would reopen the FIN'd stream and
 * fault the subscriber. The correct watermark is the oldest RETAINED group, so group 1
 * survives and its second object is delivered on the same stream. */
static int
subgroup_eviction_spares_live_lower_group(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_log_max_groups = 2; /* group 0 evicts when group 2 is ingested */
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_log_max_groups = 0;
        printf("FAIL: subgroup_eviction_spares_live_lower_group rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    scfg.start_group = 0;
    scfg.start_object = 0;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    peer_state_t pub_ps, s_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 4);

    /* Open three subgroup streams (groups 0,1,2), held OPEN — no FIN. */
    moq_subgroup_handle_t h0, h1, h2;
    moq_subgroup_cfg_t sc;
    moq_subgroup_cfg_init(&sc);
    sc.group_id = 0;
    sc.subgroup_id = 0;
    R_CHECK(&rig, moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sc,
                                            rig.now, &h0) == MOQ_OK);
    pub_write_obj(&rig, pub, h0, 0);
    sc.group_id = 1;
    R_CHECK(&rig, moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sc,
                                            rig.now, &h1) == MOQ_OK);
    pub_write_obj(&rig, pub, h1, 0);
    relay_bind_test_rig_pump(&rig, 8); /* deliver g0o0, g1o0: downstream subgroups 0,1 open */

    sc.group_id = 2;
    R_CHECK(&rig, moq_session_open_subgroup(pub->peer, pub_ps.up_sub, &sc,
                                            rig.now, &h2) == MOQ_OK);
    pub_write_obj(&rig, pub, h2, 0);   /* group 2 ingest evicts group 0 */
    relay_bind_test_rig_pump(&rig, 8); /* the watermark notice is acknowledged, then group 2 */

    pub_write_obj(&rig, pub, h1, 1);   /* more data on the RETAINED group 1 */
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);

    /* All four objects (g0o0, g1o0, g2o0, g1o1) reach the subscriber, and — the
     * discriminating invariant — group 1's still-retained subgroup stream was
     * NEVER closed then reopened (the buggy `group < delivered` watermark closes
     * it at the EVICT_WATERMARK notice, so g1o1 arrives on a FIN'd subgroup). */
    R_CHECK(&rig, s_ps.objects == 4);
    R_CHECK(&rig, !s_ps.reopen_after_finish);
    R_CHECK(&rig, s_ps.subscribe_errors == 0);
    R_CHECK(&rig, !s_ps.session_closed);

    (void)moq_session_close_subgroup(pub->peer, h1, rig.now);
    (void)moq_session_close_subgroup(pub->peer, h2, rig.now);
    relay_bind_test_rig_pump(&rig, 8);

    relay_bind_test_log_max_groups = 0;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: subgroup_eviction_spares_live_lower_group\n");
    }
    return f;
}

/* A long-lived connection must keep sending peer-originated requests past
 * the initial request-credit window. The subscriber fires far more FETCHes than
 * the tiny initial MAX_REQUEST_ID allows; on draft-16 the binding must raise the
 * ceiling (auto-grant) so the peer never permanently blocks, and every request
 * reaches a terminal (fetch_error NOT_SUPPORTED). Draft-18 has no MAX_REQUEST_ID
 * so the peer never blocks at all. `max_actions` drives the backpressure case (a
 * depth-1 session queue where the grant itself can WOULD_BLOCK and must retry). */
static int
long_lived_request_credit(moq_version_t version, uint32_t max_actions)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_initial_request_capacity = 32; /* 16 requests before exhaustion */
    relay_bind_test_grant_window = 32;
    relay_bind_test_max_actions = max_actions;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_initial_request_capacity = 0;
        relay_bind_test_grant_window = 0;
        relay_bind_test_max_actions = 0;
        printf("FAIL: long_lived_request_credit rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 6);

    moq_bytes_t nsp[1] = { B("nx") };
    moq_fetch_cfg_t fc;
    memset(&fc, 0, sizeof(fc));
    moq_fetch_cfg_init(&fc);
    fc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 1 };
    fc.track_name = B("t");
    fc.start_group = 0;
    fc.start_object = 0;
    fc.end_group = 1;
    fc.end_object = 0;

    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    const int target = 120;              /* >> the 16-request initial window */
    int sent = 0, consec_block = 0;
    for (int guard = 0; sent < target && guard < target * 12; guard++) {
        moq_fetch_t fh;
        moq_result_t rc = moq_session_fetch(sub->peer, &fc, rig.now, &fh);
        if (rc == MOQ_OK) {
            sent++;
            consec_block = 0;
        } else if (rc == MOQ_ERR_REQUEST_BLOCKED) {
            /* Peer out of credit: pumping lets the relay grant more and the
             * peer receive it. A permanent block (no grant) fails fast. */
            if (++consec_block > 40) {
                break;
            }
        } else {
            R_CHECK(&rig, false);   /* unexpected send outcome */
            break;
        }
        relay_bind_test_rig_pump(&rig, 1);
        relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    }
    relay_bind_test_rig_pump(&rig, 12);
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);

    /* Every request was accepted for send (credit stayed ahead of consumption)
     * and every one reached a terminal — no permanent block, no hang. */
    R_CHECK(&rig, sent == target);
    R_CHECK(&rig, ps.fetch_error == target);
    R_CHECK(&rig, !ps.session_closed);

    relay_bind_test_initial_request_capacity = 0;
    relay_bind_test_grant_window = 0;
    relay_bind_test_max_actions = 0;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: long_lived_request_credit(draft-%d%s)\n", (int)version,
               max_actions == 1 ? ", depth-1" : "");
    }
    return f;
}

/* ---- Close churn while work is in flight --------------------------------- *
 * Connections disappearing mid-delivery / mid-lifecycle must leave no hang, no
 * stale-session touch (ASan), no duplicate terminal, no leaked allocator ref,
 * and no retained downstream route state. Deterministic SimPair, no wall clock. */
static int
close_churn_downstream(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: close_churn_downstream rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t pub_ps, s_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_seen);
    moq_subscription_t up_sub = pub_ps.up_sub;
    relay_bind_test_rig_pump(&rig, 6);

    /* Publish a small matrix, then pump only PARTIALLY: objects + FINs are still
     * in flight when the subscriber vanishes. */
    relay_bind_test_dp_send_subgroup(&rig, pub, up_sub, 0, 0);
    relay_bind_test_dp_send_subgroup(&rig, pub, up_sub, 1, 0);
    relay_bind_test_rig_pump(&rig, 3);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);
    R_CHECK(&rig, s_ps.objects >= 1);   /* delivery genuinely in flight */

    /* Subscriber disappears mid-delivery. */
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, sub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 30); /* to quiescence — relay must not touch the dead conn */

    moqr_core_stats_t cs;
    moqr_core_get_stats(rig.core, &cs);
    /* The relay dropped the downstream subscription — no retained active sub. */
    R_CHECK(&rig, cs.subs == 0);
    /* No NEW delivery is produced for the departed peer after the close is
     * processed: drain, pump more, drain again — the count must not grow. */
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);
    int settled = s_ps.objects;
    relay_bind_test_rig_pump(&rig, 20);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);
    R_CHECK(&rig, s_ps.objects == settled);   /* relay stopped delivering */

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* no leaked refs across the churn */
    if (f == 0 && rig.failures == 0) {
        printf("PASS: close_churn_downstream\n");
    }
    return f + rig.failures;
}

static int
close_churn_publisher(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: close_churn_publisher rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t pub_ps, s_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_seen);
    moq_subscription_t up_sub = pub_ps.up_sub;
    relay_bind_test_rig_pump(&rig, 6);

    /* Publisher serves an object, then its transport drops before source-end. */
    relay_bind_test_dp_send_subgroup(&rig, pub, up_sub, 0, 0);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);
    R_CHECK(&rig, s_ps.objects >= 1);

    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, pub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 30);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);

    moqr_core_stats_t cs;
    moqr_core_get_stats(rig.core, &cs);

    /* The failover contract: an abrupt publisher close with no alternate
     * announce TERMINATES the downstream subscriber explicitly — exactly
     * one SUBSCRIBE_DONE, never a silently retained sourceless
     * subscription. The namespace stays reclaimable (asserted below). */
    R_CHECK(&rig, cs.subs == 0);            /* retired via explicit terminal */
    R_CHECK(&rig, !s_ps.session_closed);
    R_CHECK(&rig, s_ps.subscribe_errors == 0);
    R_CHECK(&rig, s_ps.done_count == 1);    /* the one explicit terminal     */

    /* Reclaimability: a fresh publisher re-announces the same namespace and a
     * fresh subscriber is served — the abrupt close did not wedge the
     * namespace/track routing. */
    conn_t *pub2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub2 && sub2);
    relay_bind_test_rig_pump(&rig, 4);
    moq_publish_namespace_cfg_t pcfg2;
    memset(&pcfg2, 0, sizeof(pcfg2));
    moq_publish_namespace_cfg_init(&pcfg2);
    pcfg2.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann2;
    R_CHECK(&rig, moq_session_publish_namespace(pub2->peer, &pcfg2, rig.now,
                                                &ann2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    moq_subscription_t sh2;
    R_CHECK(&rig, moq_session_subscribe(sub2->peer, &scfg, rig.now, &sh2) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t pub2_ps, s2_ps;
    memset(&pub2_ps, 0, sizeof(pub2_ps));
    memset(&s2_ps, 0, sizeof(s2_ps));
    relay_bind_test_peer_drain(&rig, pub2, &pub2_ps, true, false, 0, 0);
    R_CHECK(&rig, pub2_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_dp_send_subgroup(&rig, pub2, pub2_ps.up_sub, 5, 0);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub2, &s2_ps, false, false, 0, 0);

    R_CHECK(&rig, s2_ps.objects >= 1);   /* namespace reclaimable + serving */
    (void)cs;

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    if (f == 0 && rig.failures == 0) {
        printf("PASS: close_churn_publisher\n");
    }
    return f + rig.failures;
}

static int
close_churn_pending_scalar(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_max_actions = 0;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: close_churn_pending_scalar rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* roomy */
    conn_t *warm = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* roomy: warms */
    relay_bind_test_max_actions = 1;                         /* depth-1 rejoin */
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 0;
    R_CHECK(&rig, pub && warm && sub);
    relay_bind_test_rig_pump(&rig, 4);

    /* Warm two tracks with a roomy subscriber (WARM is a track-side state). */
    warm_track(&rig, pub, warm, B("a"));
    warm_track(&rig, pub, warm, B("b"));

    /* Stage BOTH warm-rejoin subscribes from the depth-1 sub, then ONE pump so a
     * scalar (the 2nd rejoin's ACCEPT_SUB — this is exactly the queued
     * ACCEPT_SUB + UPSTREAM_SUBSCRIBE churn) blocks into the pending ring. */
    moq_subscribe_cfg_t rc;
    memset(&rc, 0, sizeof(rc));
    moq_subscribe_cfg_init(&rc);
    rc.track_name = B("v");
    rc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_bytes_t nsa[2] = { B("live"), B("a") };
    rc.track_namespace = (moq_namespace_t){ .parts = nsa, .count = 2 };
    moq_subscription_t rha;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &rc, rig.now, &rha) ==
                      MOQ_OK);
    deliver_without_pump(&rig, sub);
    moq_bytes_t nsb[2] = { B("live"), B("b") };
    rc.track_namespace = (moq_namespace_t){ .parts = nsb, .count = 2 };
    moq_subscription_t rhb;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &rc, rig.now, &rhb) ==
                      MOQ_OK);
    deliver_without_pump(&rig, sub);
    (void)moqr_bind_pump(rig.bind, rig.now);

    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.pending_nonscalar_blocked == 0);
    R_CHECK(&rig, bs.pending_high_water > 0);   /* a scalar IS pending */

    /* Close the depth-1 subscriber WITH the scalar still pending: the purge must
     * drop it — never retry it against the dead session. */
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, sub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 40); /* quiescence — no touch of the closed session */

    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.pending_nonscalar_blocked == 0);
    moqr_core_stats_t cs;
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.subs == 0);   /* the closed sub's subscriptions are gone */

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* purge freed the pending, no leak */
    if (f == 0 && rig.failures == 0) {
        printf("PASS: close_churn_pending_scalar\n");
    }
    return f + rig.failures;
}

/* ---- Route/subscription projection oracle -------------------------------- *
 * The relay's route dump + stats must reflect the SAME logical state as the
 * wire-observable scenario. A small local model is updated at each step and
 * compared against get_stats, route_dump_json, route-epoch monotonicity, and the
 * wire events (namespace found/gone, subscribe OK/terminal). */
#define J_HAS(hay, needle) (strstr((hay), (needle)) != NULL)

static int
route_projection_oracle(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: route_projection_oracle rig create\n");
        return 1;
    }
    char buf[4096];
    size_t w = 0;
    moqr_core_stats_t cs;

    /* Empty baseline. */
    moqr_core_get_stats(rig.core, &cs);
    uint64_t epoch = cs.route_epoch;
    R_CHECK(&rig, moqr_core_route_dump_json(rig.core, buf, sizeof(buf), &w) ==
                      MOQR_OK);
    R_CHECK(&rig, J_HAS(buf, "\"announces\":[]"));
    R_CHECK(&rig, J_HAS(buf, "\"namespace_subscriptions\":[]"));

    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* cookie 1 */
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* cookie 2 */
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* cookie 3 */
    R_CHECK(&rig, pub && watch && sub);
    relay_bind_test_rig_pump(&rig, 4);

    /* Watcher subscribes to namespace prefix "live". */
    moq_subscribe_namespace_cfg_t nsc;
    memset(&nsc, 0, sizeof(nsc));
    moq_subscribe_namespace_cfg_init(&nsc);
    moq_bytes_t pfx[1] = { B("live") };
    nsc.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nsc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nsc, rig.now,
                                                  &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);

    /* Publisher announces live/cam. */
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t w_ps, pub_ps, s_ps;
    memset(&w_ps, 0, sizeof(w_ps));
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    R_CHECK(&rig, w_ps.ns_found >= 1);   /* NAMESPACE_FOUND on the wire */

    /* Model: announce live/cam (binding 1) and the namespace watcher on "live"
     * (binding 2) are both present; no track yet; the route epoch advanced. */
    R_CHECK(&rig, moqr_core_route_dump_json(rig.core, buf, sizeof(buf), &w) ==
                      MOQR_OK);
    R_CHECK(&rig, J_HAS(buf, "\"namespace\":[\"live\",\"cam\"],\"binding\":1"));
    R_CHECK(&rig, J_HAS(buf, "\"prefix\":[\"live\"],\"binding\":2"));
    R_CHECK(&rig, J_HAS(buf, "\"tracks\":[]"));
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.route_epoch > epoch);   /* advanced on the mutations */
    epoch = cs.route_epoch;
    R_CHECK(&rig, cs.subs == 0);             /* no subscriber yet */

    /* Subscriber subscribes to live/cam track v; publisher serves a matrix. */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_dp_send_subgroup(&rig, pub, pub_ps.up_sub, 0, 0);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);

    /* Model: track live/cam "v" is ACTIVE (upstream binding 1) and carries one
     * ACTIVE subscriber (binding 3); stats agree and the epoch advanced. */
    R_CHECK(&rig, moqr_core_route_dump_json(rig.core, buf, sizeof(buf), &w) ==
                      MOQR_OK);
    R_CHECK(&rig,
            J_HAS(buf, "\"name\":\"v\",\"state\":\"active\",\"upstream_binding\":1"));
    R_CHECK(&rig, J_HAS(buf, "\"binding\":3,\"state\":\"active\""));
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.subs == 1 && cs.subs_active == 1 && cs.subs_parked == 0);
    R_CHECK(&rig, cs.subs == cs.subs_active + cs.subs_parked);   /* invariant */
    R_CHECK(&rig, cs.tracks == 1);
    R_CHECK(&rig, cs.route_epoch > epoch);
    epoch = cs.route_epoch;

    /* Unsubscribe; then close the publisher (withdraws the announce). */
    R_CHECK(&rig, moq_session_unsubscribe(sub->peer, sh, rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, pub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 10);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    /* Model: unsubscribe removed the active subscriber; the publisher close
     * withdrew the announce (watcher saw NAMESPACE_GONE); the track dropped to
     * WARM with no upstream and no subscriptions; the watcher route persists. */
    R_CHECK(&rig, moqr_core_route_dump_json(rig.core, buf, sizeof(buf), &w) ==
                      MOQR_OK);
    R_CHECK(&rig, J_HAS(buf, "\"announces\":[]"));            /* announce gone */
    R_CHECK(&rig, !J_HAS(buf, "\"binding\":3,\"state\":\"active\"")); /* sub gone */
    R_CHECK(&rig, J_HAS(buf, "\"state\":\"warm\",\"upstream_binding\":null"));
    R_CHECK(&rig, J_HAS(buf, "\"prefix\":[\"live\"],\"binding\":2")); /* watcher */
    R_CHECK(&rig, w_ps.ns_gone >= 1);   /* NAMESPACE_GONE on the wire */
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.subs == 0 && cs.subs_active == 0);
    R_CHECK(&rig, cs.route_epoch > epoch);
    epoch = cs.route_epoch;

    /* Re-announce the same namespace with a fresh publisher: dump + wire become
     * coherent again (announce back, watcher re-notified, epoch advances). */
    conn_t *pub2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub2 != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_announcement_t ann2;
    R_CHECK(&rig, moq_session_publish_namespace(pub2->peer, &pcfg, rig.now,
                                                &ann2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    R_CHECK(&rig, w_ps.ns_found >= 1);   /* watcher re-notified */
    R_CHECK(&rig, moqr_core_route_dump_json(rig.core, buf, sizeof(buf), &w) ==
                      MOQR_OK);
    R_CHECK(&rig, !J_HAS(buf, "\"announces\":[]"));   /* announce back */
    R_CHECK(&rig, J_HAS(buf, "\"namespace\":[\"live\",\"cam\"],\"binding\":"));
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.route_epoch > epoch);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    if (f == 0 && rig.failures == 0) {
        printf("PASS: route_projection_oracle\n");
    }
    return f + rig.failures;
}

/* ---- Production-binding allocation-failure sweeps ------------------------- *
 * A fail-at-N test allocator (see ca_t.fail_at) injects a single OOM at each
 * allocation index; a focused production-binding flow is replayed for every N.
 * At each injection point the relay must fail closed — no leak, no UAF, no stale
 * route state — and a no-failure replay must still pass. */

/* Best-effort drain (no R_CHECK — every op may fail under injected OOM): polls a
 * peer, accepts any upstream SUBSCRIBE (capturing its handle), and cleans up. */
static void
oom_drain(rig_t *r, conn_t *cn, moq_subscription_t *out_up)
{
    moq_event_t evs[16];
    size_t n;
    while ((n = moq_session_poll_events(cn->peer, evs, 16)) > 0) {
        for (size_t e = 0; e < n; e++) {
            if (evs[e].kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
                if (out_up != NULL) {
                    *out_up = evs[e].u.subscribe_request.sub;
                }
                moq_accept_subscribe_cfg_t cfg;
                moq_accept_subscribe_cfg_init(&cfg);
                (void)moq_session_accept_subscribe(
                    cn->peer, evs[e].u.subscribe_request.sub, &cfg, r->now);
            }
            moq_event_cleanup(&evs[e]);
        }
    }
}

/* Announce + namespace-watch + subscribe + upstream accept + one object +
 * unsubscribe over the production binding. Every op is best-effort so a mid-flow
 * OOM just bails; the caller checks the allocator balance after relay_bind_test_rig_destroy. */
static void
oom_bind_flow(rig_t *rig)
{
    conn_t *pub = relay_bind_test_rig_connect(rig, MOQ_VERSION_DRAFT_18);
    conn_t *watch = relay_bind_test_rig_connect(rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(rig, MOQ_VERSION_DRAFT_18);
    if (!pub || !watch || !sub) {
        return; /* OOM during connect — relay_bind_test_rig_connect already cleaned up */
    }
    relay_bind_test_rig_pump(rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nsc;
    memset(&nsc, 0, sizeof(nsc));
    moq_subscribe_namespace_cfg_init(&nsc);
    nsc.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nsc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    (void)moq_session_subscribe_namespace(watch->peer, &nsc, rig->now, &nsh);
    relay_bind_test_rig_pump(rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    (void)moq_session_publish_namespace(pub->peer, &pcfg, rig->now, &ann);
    relay_bind_test_rig_pump(rig, 4);
    oom_drain(rig, watch, NULL);

    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh = { 0 };
    bool subscribed =
        moq_session_subscribe(sub->peer, &scfg, rig->now, &sh) == MOQ_OK;
    relay_bind_test_rig_pump(rig, 6);
    moq_subscription_t up_sub = { 0 };
    oom_drain(rig, pub, &up_sub);
    relay_bind_test_rig_pump(rig, 6);

    if (up_sub._opaque != 0) {
        moq_subgroup_cfg_t sgc;
        moq_subgroup_cfg_init(&sgc);
        sgc.group_id = 0;
        sgc.subgroup_id = 0;
        moq_subgroup_handle_t sgh;
        if (moq_session_open_subgroup(pub->peer, up_sub, &sgc, rig->now, &sgh) ==
            MOQ_OK) {
            uint8_t body[4] = { 0xAB, 1, 2, 3 };
            moq_rcbuf_t *pl = NULL;
            if (moq_rcbuf_create(&rig->alloc->vt, body, sizeof(body), &pl) ==
                MOQ_OK) {
                (void)moq_session_write_object(pub->peer, sgh, 0, pl, rig->now);
                moq_rcbuf_decref(pl);
            }
            (void)moq_session_close_subgroup(pub->peer, sgh, rig->now);
        }
    }
    relay_bind_test_rig_pump(rig, 6);
    oom_drain(rig, sub, NULL);

    if (subscribed) {
        (void)moq_session_unsubscribe(sub->peer, sh, rig->now);
    }
    relay_bind_test_rig_pump(rig, 8);
}

/* Focused unit test: the fail-at allocator fires at exactly the intended
 * attempt index, later attempts still succeed, and byte accounting balances. */
static int
test_oom_allocator(void)
{
    int failures = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    a.fail_at = 3;   /* fail exactly the 3rd attempt */
    void *p1 = a.vt.alloc(16, a.vt.ctx);
    void *p2 = a.vt.alloc(16, a.vt.ctx);
    void *p3 = a.vt.alloc(16, a.vt.ctx);   /* injected failure */
    void *p4 = a.vt.alloc(16, a.vt.ctx);   /* succeeds again */
    if (!(p1 != NULL && p2 != NULL && p3 == NULL && p4 != NULL)) {
        printf("FAIL: oom allocator fired at the wrong index\n");
        failures++;
    }
    if (a.attempts != 4 || a.allocs != 3) {
        printf("FAIL: oom allocator accounting attempts=%ld allocs=%ld\n",
               a.attempts, a.allocs);
        failures++;
    }
    a.vt.free(p1, 16, a.vt.ctx);
    a.vt.free(p2, 16, a.vt.ctx);
    a.vt.free(p4, 16, a.vt.ctx);
    if (a.live != 0) {
        printf("FAIL: oom allocator live=%ld after frees\n", a.live);
        failures++;
    }
    if (failures == 0) {
        printf("PASS: test_oom_allocator\n");
    }
    return failures;
}

static int
test_oom_binding_flow(void)
{
    int failures = 0;

    /* No-failure control + measurement of the flow's allocation count. */
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: test_oom_binding_flow control rig create\n");
        return 1;
    }
    oom_bind_flow(&rig);
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: oom control leaked %ld bytes\n", a.live);
        failures++;
    }
    long total = a.attempts;

    /* Sweep: inject a single OOM at each allocation index. */
    int fired = 0;
    for (long fa = 1; fa <= total; fa++) {
        ca_t af;
        relay_bind_test_ca_init(&af);
        af.fail_at = fa;
        rig_t rf;
        if (relay_bind_test_rig_create(&rf, &af) == MOQR_OK) {
            oom_bind_flow(&rf);
            relay_bind_test_rig_destroy(&rf);
        }
        if (af.attempts >= fa) {
            fired++;   /* the injected failure actually fired */
        }
        if (af.live != 0) {
            printf("FAIL: oom sweep fail_at=%ld leaked %ld bytes\n", fa,
                   af.live);
            failures++;
        }
    }
    if (fired != (int)total) {   /* every injection point must actually fire */
        printf("FAIL: oom sweep injected %d of %d points\n", fired,
               (int)total);
        failures++;
    }
    if (failures == 0) {
        printf("PASS: test_oom_binding_flow (swept fail_at 1..%ld)\n", total);
    }
    return failures;
}

/* A retire path must arm the warm-track linger with the REAL clock. When
 * the last subscriber of an ACTIVE track retires because its bounded range
 * completed (a core-internal retire in next_delivery, not an app unsubscribe),
 * the track should linger for `linger_us` before the tick warms it (so a quick
 * rejoin reuses the upstream). If that retire arms the deadline with a stale
 * clock (now_us == 0), the deadline is already in the past and the very next
 * tick warms the track PREMATURELY — the relay drops the upstream immediately.
 * Observed at the origin as an early UNSUBSCRIBED. */
static int
range_completion_linger(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_linger_us = 15000; /* linger spans ~15 pumps at 1000us/cycle */
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_linger_us = 0;
        printf("FAIL: range_completion_linger rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    /* Bounded range: group 0 only. */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE;
    scfg.start_group = 0;
    scfg.start_object = 0;
    scfg.end_group = 0;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t pub_ps, s_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s_ps, 0, sizeof(s_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* accept upstream */
    R_CHECK(&rig, pub_ps.up_seen);
    relay_bind_test_rig_pump(&rig, 6);

    /* Origin publishes group 0 (in range, EOG) then group 1 (past the range —
     * its arrival is the live edge that completes the subscription). */
    relay_bind_test_pub_subgroup_n(&rig, pub, pub_ps.up_sub, 0, 0, 1, true);
    relay_bind_test_pub_subgroup_n(&rig, pub, pub_ps.up_sub, 1, 0, 1, false);
    relay_bind_test_rig_pump(&rig, 8); /* deliver group 0, ingest group 1 -> range completes */
    relay_bind_test_peer_drain(&rig, sub, &s_ps, false, false, 0, 0);
    R_CHECK(&rig, s_ps.done_seen);       /* the range completed (SUBSCRIBE_DONE) */

    /* The last subscriber of the ACTIVE track just retired (range complete).
     * A correctly clocked retire armed the warm deadline at now_us + linger_us
     * (now_us is well past linger_us here), so the track is STILL lingering and
     * the origin has NOT been unsubscribed. The buggy now_us==0 retire armed it
     * at 0 + linger_us — already in the past — so the very next tick warmed the
     * track and dropped the upstream. */
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.unsubscribed == 0);

    /* Fewer cycles than the linger window: still lingering. */
    relay_bind_test_rig_pump(&rig, 3);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.unsubscribed == 0);   /* fails if linger expiry is computed from 0 */

    /* Past the linger deadline the track warms and releases the upstream. */
    relay_bind_test_rig_pump(&rig, 15);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.unsubscribed == 1);   /* eventually expires */

    relay_bind_test_linger_us = 0;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: range_completion_linger\n");
    }
    return f;
}

/* Records what the bind's intent router received, and can ask the binding to
 * park a routed ACCEPT_SUB (return false) a bounded number of times. */
typedef struct rtr_rec {
    int                count;        /* total intents routed                  */
    int                accept_count; /* ACCEPT_SUB intents routed             */
    moqr_intent_kind_t last_kind;
    uint64_t           last_cookie;
    moqr_track_t       last_track;
    uint64_t           last_track_gen;
    int                park_accepts; /* return false for this many ACCEPT_SUB */
} rtr_rec_t;

static bool
rtr_route(void *ctx, const moqr_intent_t *it, uint64_t now_us)
{
    (void)now_us;
    rtr_rec_t *r = ctx;
    r->count++;
    r->last_kind = it->kind;
    r->last_cookie = it->binding_cookie;
    r->last_track = it->track;
    r->last_track_gen = it->track_gen;
    if (it->kind == MOQR_INTENT_ACCEPT_SUB) {
        r->accept_count++;
        if (r->park_accepts > 0) {
            r->park_accepts--;
            return false; /* not now: the binding parks it for retry */
        }
    }
    return true; /* consumed */
}

/* Intent-router seam: an output intent whose binding_cookie is a reserved
 * shard-manager cookie (>= MOQR_SHARD_COOKIE_BASE) is handed to the router
 * instead of a session; a normal cookie is not; and a router that declines a
 * deferrable ACCEPT_SUB has it parked and retried, like a WOULD_BLOCKed write.
 * The router firing depends on the router hook in bind_try_intent; without it
 * reserved intents are silently moot. */
/* -- Attach-time draft validation ------------------------------------------ *
 * The binding encodes every terminal for the draft the reading peer speaks, so
 * a connection whose draft is unknown has no safe emission. Attach must refuse
 * it outright rather than substitute a plausible default: a silently assumed
 * draft is exactly the wrong-numbering failure this parameter exists to stop.
 * The rejection must also happen before any slot or core binding is taken, or a
 * refused attach would still cost capacity. */
static int
test_conn_version_fail_closed(void)
{
    int failures = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);

    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a.vt);
    moqr_core_t *core = NULL;
    MOQ_TEST_CHECK(moqr_core_create(&cfg, &core) == MOQR_OK);

    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), &a.vt);
    bcfg.core = core;
    bcfg.max_conns = 1;   /* the capacity boundary a rejected attach must not cost */
    moqr_bind_t *bind = NULL;
    MOQ_TEST_CHECK(moqr_bind_create(&bcfg, &bind) == MOQR_OK);

    /* Two real sessions; the second only ever attaches after the rejections. */
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = &a.vt;
    scfg.seed = 0x11117777u;
    scfg.version = MOQ_VERSION_DRAFT_18;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&scfg, &sp) == MOQ_OK);
    MOQ_TEST_CHECK(moq_simpair_start(sp) == MOQ_OK);
    moq_session_t *sess = moq_simpair_server(sp);

    /* Unattached and null lookups name no draft at all. */
    MOQ_TEST_CHECK(moqr_bind_conn_version(NULL, sess) == 0);
    MOQ_TEST_CHECK(moqr_bind_conn_version(bind, NULL) == 0);
    MOQ_TEST_CHECK(moqr_bind_conn_version(bind, sess) == 0);

    /* Every unsupported value is refused, including the zero a caller gets from
     * a connection whose version is not yet negotiated. */
    MOQ_TEST_CHECK(moqr_bind_conn_open(bind, sess, (moq_version_t)0) ==
                   MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(moqr_bind_conn_open(bind, sess, (moq_version_t)17) ==
                   MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(moqr_bind_conn_open(bind, sess, (moq_version_t)0xFFFFu) ==
                   MOQR_ERR_INVAL);
    /* Refused attaches named no draft and, at max_conns == 1, cost no slot. */
    MOQ_TEST_CHECK(moqr_bind_conn_version(bind, sess) == 0);

    /* d18 round-trips exactly, and the boundary was still free. */
    MOQ_TEST_CHECK(moqr_bind_conn_open(bind, sess, MOQ_VERSION_DRAFT_18) ==
                   MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_conn_version(bind, sess) == MOQ_VERSION_DRAFT_18);
    MOQ_TEST_CHECK(moqr_bind_conn_close(bind, sess) == MOQR_OK);

    /* d16 round-trips exactly on the same reclaimed slot. */
    moq_simpair_cfg_t scfg16 = MOQ_SIMPAIR_CFG_INIT;
    scfg16.alloc = &a.vt;
    scfg16.seed = 0x22228888u;
    scfg16.version = MOQ_VERSION_DRAFT_16;
    moq_simpair_t *sp16 = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&scfg16, &sp16) == MOQ_OK);
    MOQ_TEST_CHECK(moq_simpair_start(sp16) == MOQ_OK);
    moq_session_t *sess16 = moq_simpair_server(sp16);
    MOQ_TEST_CHECK(moqr_bind_conn_open(bind, sess16, MOQ_VERSION_DRAFT_16) ==
                   MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_conn_version(bind, sess16) == MOQ_VERSION_DRAFT_16);

    moq_simpair_destroy(sp16);
    moq_simpair_destroy(sp);
    moqr_bind_destroy(bind);
    moqr_core_destroy(core);
    return failures;
}

static int
test_intent_router(void)
{
    int failures = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rtr_rec_t rec;
    memset(&rec, 0, sizeof(rec));

    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), &a.vt);
    cfg.log_budget.max_groups = 8;
    cfg.log_budget.max_bytes = 1u << 20;
    cfg.linger_us = 500;
    moqr_core_t *core = NULL;
    MOQ_TEST_CHECK(moqr_core_create(&cfg, &core) == MOQR_OK);

    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), &a.vt);
    bcfg.core = core;
    bcfg.max_conns = 8;
    bcfg.router = rtr_route;
    bcfg.router_ctx = &rec;
    /* Leave router_cookie_base 0: a router with base 0 defaults to
     * MOQR_SHARD_COOKIE_BASE, so real connection cookies still reach sessions
     * (proven by (2) below — a normal-cookie ACCEPT_SUB is not routed). */
    moqr_bind_t *bind = NULL;
    MOQ_TEST_CHECK(moqr_bind_create(&bcfg, &bind) == MOQR_OK);
    uint64_t now = 1;

    /* (1) A reserved-cookie announcer's UPSTREAM_SUBSCRIBE is routed. */
    moqr_binding_t pb;   /* reserved pseudo-binding (announcer) */
    MOQ_TEST_CHECK(moqr_core_binding_open(core, MOQR_SHARD_COOKIE_BASE | 5u,
                                          &pb) == MOQR_OK);
    moqr_binding_t subb; /* a normal binding (subscriber) */
    MOQ_TEST_CHECK(moqr_core_binding_open(core, 1, &subb) == MOQR_OK);
    moq_bytes_t nsp[1] = { B("demo") };
    moqr_ns_t ns = { nsp, 1 };
    MOQ_TEST_CHECK(moqr_core_announce(core, pb, ns) == MOQR_OK);
    moqr_subscribe_req_t rq;
    moqr_subscribe_req_init(&rq);
    rq.ns = ns;
    rq.name = B("video");
    rq.filter.type = MOQR_FILTER_ABSOLUTE_START;
    rq.cookie = 100;
    moqr_sub_t s;
    MOQ_TEST_CHECK(moqr_core_subscribe(core, subb, &rq, &s) == MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_pump(bind, now) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT(rec.count, 1);
    MOQ_TEST_CHECK(rec.last_kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE);
    MOQ_TEST_CHECK_EQ_U64(rec.last_cookie, MOQR_SHARD_COOKIE_BASE | 5u);
    moqr_track_t track = rec.last_track;
    uint64_t tgen = rec.last_track_gen;

    /* (2) The subscriber is a NORMAL cookie: resolving the upstream accepts it,
     * and that ACCEPT_SUB (cookie 1) is NOT routed. */
    MOQ_TEST_CHECK(moqr_core_upstream_ok(core, track, tgen, 777, true, 4, 9) ==
                   MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_pump(bind, now) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT(rec.count, 1);   /* still 1: cookie 1 went to a session */

    /* (3) A reserved-cookie SUBSCRIBER's ACCEPT_SUB is routed, and a router that
     * declines it once has it parked and retried. Both announcer and subscriber
     * are reserved here so both of their intents route (giving us the track
     * handle from the routed UPSTREAM_SUBSCRIBE). */
    rec.park_accepts = 1;   /* decline the first routed ACCEPT_SUB once */
    moqr_binding_t pubb;    /* reserved announcer */
    MOQ_TEST_CHECK(moqr_core_binding_open(core, MOQR_SHARD_COOKIE_BASE | 7u,
                                          &pubb) == MOQR_OK);
    moqr_binding_t psub;    /* reserved pump subscriber */
    MOQ_TEST_CHECK(moqr_core_binding_open(core, MOQR_SHARD_COOKIE_BASE | 6u,
                                          &psub) == MOQR_OK);
    moq_bytes_t nsp2[1] = { B("demo2") };
    moqr_ns_t ns2 = { nsp2, 1 };
    MOQ_TEST_CHECK(moqr_core_announce(core, pubb, ns2) == MOQR_OK);
    moqr_subscribe_req_t rq2;
    moqr_subscribe_req_init(&rq2);
    rq2.ns = ns2;
    rq2.name = B("video");
    rq2.filter.type = MOQR_FILTER_ABSOLUTE_START;
    rq2.cookie = 200;
    moqr_sub_t s2;
    MOQ_TEST_CHECK(moqr_core_subscribe(core, psub, &rq2, &s2) == MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_pump(bind, now) == MOQR_OK); /* UPSTREAM_SUBSCRIBE -> pubb, routed */
    MOQ_TEST_CHECK(rec.last_kind == MOQR_INTENT_UPSTREAM_SUBSCRIBE);
    moqr_track_t track2 = rec.last_track;
    uint64_t tgen2 = rec.last_track_gen;
    int accepts_before = rec.accept_count;
    /* Resolve upstream: psub's ACCEPT_SUB targets a reserved cookie -> routed;
     * the router declines it once (parked), the retry consumes it. */
    MOQ_TEST_CHECK(moqr_core_upstream_ok(core, track2, tgen2, 777, true, 4, 9) ==
                   MOQR_OK);
    MOQ_TEST_CHECK(moqr_bind_pump(bind, now) == MOQR_OK); /* route ACCEPT_SUB -> false -> park */
    MOQ_TEST_CHECK_EQ_INT(rec.accept_count, accepts_before + 1);
    MOQ_TEST_CHECK(moqr_bind_pump(bind, now) == MOQR_OK); /* retry parked ACCEPT_SUB -> consumed */
    MOQ_TEST_CHECK_EQ_INT(rec.accept_count, accepts_before + 2);
    MOQ_TEST_CHECK_EQ_INT(rec.park_accepts, 0);

    moqr_bind_destroy(bind);
    moqr_core_destroy(core);
    MOQ_TEST_CHECK_EQ_INT((int)a.live, 0);
    MOQ_TEST_PASS("intent_router");
    return failures;
}

/* An omitted SUBSCRIPTION_FILTER is explicitly unfiltered in drafts 16 and
 * 18. The session surfaces omission as FILTER_NONE; the binding maps it to
 * AbsoluteStart 0/0, independently of the request initializer's Largest
 * default. test_relay_unfiltered also checks lower-object delivery. */
static int
subscribe_omitted_filter_is_unfiltered(moq_version_t version)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: subscribe_omitted_filter rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    conn_t *sub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub != NULL && sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t parts[2] = { B("show"), B("camera") };
    moq_publish_namespace_cfg_t pcfg;
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);

    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    scfg.track_name = B("video");
    R_CHECK(&rig, scfg.filter == MOQ_SUBSCRIBE_FILTER_NONE);
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);

    peer_state_t pub_state;
    peer_state_t sub_state;
    memset(&pub_state, 0, sizeof(pub_state));
    memset(&sub_state, 0, sizeof(sub_state));
    relay_bind_test_peer_drain(&rig, pub, &pub_state, true, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &sub_state, false, false, 0, 0);
    R_CHECK(&rig, pub_state.up_seen);
    R_CHECK(&rig, sub_state.subscribe_ok);
    R_CHECK(&rig, sub_state.subscribe_errors == 0);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    if (f == 0 && rig.failures == 0) {
        printf("PASS: subscribe_omitted_filter_is_unfiltered (draft=%llu)\n",
               (unsigned long long)version);
    }
    return f + rig.failures;
}

int
main(void)
{
    int failures = 0;
    failures += subscribe_omitted_filter_is_unfiltered(MOQ_VERSION_DRAFT_16);
    failures += subscribe_omitted_filter_is_unfiltered(MOQ_VERSION_DRAFT_18);
    failures += test_conn_version_fail_closed();
    failures += test_intent_router();
    failures += test_oom_allocator();
    failures += test_oom_binding_flow();
    failures += route_projection_oracle();
    failures += close_churn_downstream();
    failures += close_churn_publisher();
    failures += close_churn_pending_scalar();
    failures += range_completion_linger();
    failures += long_lived_request_credit(MOQ_VERSION_DRAFT_16, 0);
    failures += long_lived_request_credit(MOQ_VERSION_DRAFT_18, 0);
    failures += long_lived_request_credit(MOQ_VERSION_DRAFT_16, 1);
    failures += borrowed_intent_never_parked();
    failures += intent_retry_reentry_preserves_outer_batch();
    failures += subgroup_slot_reclamation();
    failures += subgroup_reclamation_shapes();
    failures += subgroup_eviction_spares_live_lower_group();
    failures += relay_bind_test_parity_flow(MOQ_VERSION_DRAFT_16);
    failures += relay_bind_test_parity_flow(MOQ_VERSION_DRAFT_18);
    failures += relay_bind_test_parity_flow(MOQ_VERSION_DRAFT_22);

    failures += small_caps_flow();
    failures += teardown_pending_intent_flow();
    failures += ring_full_track_status_flow();
    failures += ring_full_publish_flow();
    failures += shutdown_no_session_flow();
    failures += test_latency_clock_regression();
    failures += test_publish_finished_releases_track();
    failures += test_unsupported_requests_reject();
    failures += test_upstream_done_terminates_downstream();
    failures += test_publish_finished_terminates_downstream();
    failures += test_upstream_redirect_rejects_downstream();
    failures += test_upstream_goaway_terminates_downstream();

    uint64_t h1 = parity_hash_run();
    uint64_t h2 = parity_hash_run();
    if (h1 == 0 || h1 != h2) {
        printf("FAIL: parity hash: %llx vs %llx\n",
               (unsigned long long)h1, (unsigned long long)h2);
        failures++;
    } else {
        printf("PASS: parity_hash (run-twice equal)\n");
    }
    return failures == 0 ? 0 : 1; /* exit status truncates to 8 bits */
}
