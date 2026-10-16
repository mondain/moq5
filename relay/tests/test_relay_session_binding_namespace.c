/*
 * Namespaces and discovery through the production relay binding over real
 * sessions: namespace-subscription updates arrive in order and exactly once
 * through blocked control streams, ring retries, cross-kind ordering,
 * byte-capacity limits and exhaustion, and a closing connection purges what
 * it held; root namespaces and full track names work end to end and across
 * draft profiles; announcement and namespace capacity, held accepts and
 * refusals answer with the right codes; a retried announce reads its own
 * copy; the newest PUBLISH_NAMESPACE wins; and the end of a namespace
 * releases its route.
 */
#include "relay_session_binding_harness.h"

/* A publisher's PUBLISH_NAMESPACE_DONE must remove the relay
 * route and notify watchers. Without the announcement-handle map, the binding
 * cannot translate the done event back to namespace bytes, so the route stays
 * occupied forever. */
static int
test_namespace_done_releases_route(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: namespace-done rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL && watch != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("gone") };
    moq_subscribe_namespace_cfg_t nsc;
    memset(&nsc, 0, sizeof(nsc));
    moq_subscribe_namespace_cfg_init(&nsc);
    nsc.track_namespace_prefix =
        (moq_namespace_t){ .parts = pfx, .count = 1 };
    nsc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nsc,
                                                  rig.now, &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t ns[2] = { B("gone"), B("cam") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = ns, .count = 2 };
    moq_announcement_t ann1;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann1) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t pub_ps, watch_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&watch_ps, 0, sizeof(watch_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &watch_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);
    R_CHECK(&rig, watch_ps.ns_found == 1);

    R_CHECK(&rig, moq_session_publish_namespace_done(pub->peer, ann1,
                                                     rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&watch_ps, 0, sizeof(watch_ps));
    relay_bind_test_peer_drain(&rig, watch, &watch_ps, false, false, 0, 0);
    R_CHECK(&rig, watch_ps.ns_gone == 1);

    moq_announcement_t ann2;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&watch_ps, 0, sizeof(watch_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &watch_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);
    R_CHECK(&rig, pub_ps.ns_rejected == 0);
    R_CHECK(&rig, watch_ps.ns_found == 1);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: namespace_done_releases_route\n");
    }
    return f;
}

/* A namespace discovered for an existing prefix subscription must reach the
 * subscriber even when the downstream control path is momentarily blocked.
 * draft-ietf-moq-transport-18 Section 6.2: a relay that has received an
 * authorized PUBLISH_NAMESPACE "MUST send a NAMESPACE message to any
 * subscriber that has sent SUBSCRIBE_NAMESPACE for that namespace, or a
 * prefix of that namespace" -- a MUST, with no best-effort exemption for a
 * congested downstream.
 *
 * blocked=false is the healthy control (the same ordering on an unconstrained
 * queue); blocked=true constrains the session action queue so the relay's
 * send_namespace meets WOULD_BLOCK at the moment the announce lands.
 */
static int
ns_found_survives_blocked_control(bool blocked)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved_actions = relay_bind_test_max_actions;
    /* A depth-1 action queue: the prefix-subscription response occupies the
     * single slot, so the NAMESPACE that follows it has nowhere to go. */
    relay_bind_test_max_actions = blocked ? 1u : 0u;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (ns_found_survives_blocked_control)\n");
        relay_bind_test_max_actions = saved_actions;
        return 1;
    }

    relay_bind_test_max_actions = 0u; /* publisher: normal queue */
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u; /* watcher: the blocked downstream path */
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4); /* handshakes */

    /* Watcher registers interest in the prefix BEFORE anything is announced:
     * this is the subscribe-before-announce ordering. */
    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix =
        (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg,
                                                  rig.now, &nsh) == MOQ_OK);
    if (!blocked) {
        relay_bind_test_rig_pump(&rig, 4);
    }

    /* The publisher announces a matching namespace. */
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);

    /* Pump well past any transient block: a merely deferred NAMESPACE would
     * be delivered here, so a still-missing one was dropped, not delayed. */
    relay_bind_test_rig_pump(&rig, 64);
    peer_state_t pub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    printf("  ns_found_survives_blocked_control(blocked=%d): "
           "namespace_accepted=%d ns_found=%d\n",
           (int)blocked, (int)pub_ps.namespace_accepted, w_ps.ns_found);

    /* The announce itself must succeed either way -- otherwise the ordering
     * never reached the state under test and the check below is vacuous. */
    R_CHECK(&rig, pub_ps.namespace_accepted);
    R_CHECK(&rig, w_ps.ns_found == 1);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved_actions;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: ns_found_survives_blocked_control(blocked=%d)\n",
               (int)blocked);
    }
    return f;
}

/* --- root Track Namespace, end to end through session + bind + core --------
 *
 * draft-18 Section 2.4.1 gives a Track Namespace 0..32 fields, so the ROOT
 * namespace (zero fields) is a legal announce that a relay must accept, store,
 * route to matching prefix subscribers, and withdraw. draft-16 Section 2.4.1
 * gives 1..32, so the same announce is a protocol error there; that floor is
 * enforced by the session profile, ahead of the relay.
 *
 * This drives the real production entry points -- moq_session_publish_namespace
 * over a real session, the bind event path, and moqr_core_announce/unannounce
 * -- not a helper model.
 *
 * d16 = the draft-16 arm, where the root announce must be REFUSED. */
static int
root_namespace_end_to_end(bool d16)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 0u;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    moq_version_t ver = d16 ? MOQ_VERSION_DRAFT_16 : MOQ_VERSION_DRAFT_18;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (root_namespace_end_to_end)\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, ver);
    conn_t *watch = relay_bind_test_rig_connect(&rig, ver);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    /* A ROOT prefix subscription: it matches every namespace, including the
     * root namespace itself. */
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = NULL,
                                                      .count = 0 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg,
                                                  rig.now, &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);

    peer_state_t pub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&w_ps, 0, sizeof(w_ps));

    /* A present-but-EMPTY field is a protocol error in BOTH profiles: 0..32
     * fields is about the FIELD COUNT, never about zero-length fields. */
    moq_bytes_t empty_part[1] = { { NULL, 0 } };
    moq_publish_namespace_cfg_t bad;
    memset(&bad, 0, sizeof(bad));
    moq_publish_namespace_cfg_init(&bad);
    bad.track_namespace = (moq_namespace_t){ .parts = empty_part, .count = 1 };
    moq_announcement_t bad_ann;
    int bad_rc = moq_session_publish_namespace(pub->peer, &bad, rig.now,
                                               &bad_ann);
    R_CHECK(&rig, bad_rc == MOQ_ERR_INVAL);   /* exact, not merely non-OK */

    /* The ROOT namespace itself. */
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = NULL, .count = 0 };
    moq_announcement_t ann;
    int rc = moq_session_publish_namespace(pub->peer, &pc, rig.now, &ann);
    printf("  root_ns(%s): empty_field_rc=%d root_rc=%d\n",
           d16 ? "d16" : "d18", bad_rc, rc);

    if (d16) {
        /* draft-16 Section 2.4.1: 1..32 fields. The profile refuses it before
         * the relay ever sees an event, so nothing is announced or routed. */
        R_CHECK(&rig, rc == MOQ_ERR_INVAL);   /* exact profile refusal */
        relay_bind_test_rig_pump(&rig, 32);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        printf("  root_ns(d16): accepted=%d ns_found=%d\n",
               (int)pub_ps.namespace_accepted, w_ps.ns_found);
        R_CHECK(&rig, !pub_ps.namespace_accepted);
        R_CHECK(&rig, w_ps.ns_found == 0);
        relay_bind_test_rig_destroy(&rig);
        relay_bind_test_rv_reset();
        R_CHECK(&rig, a.live == 0);
        relay_bind_test_max_actions = saved;
        int fd = rig.failures;
        if (fd == 0) {
            printf("PASS: root_namespace_end_to_end(d16 refused)\n");
        }
        return fd;
    }

    /* draft-18: accepted end to end, and ROUTED to the matching prefix. */
    R_CHECK(&rig, rc == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    printf("  root_ns(d18): accepted=%d ns_found=%d\n",
           (int)pub_ps.namespace_accepted, w_ps.ns_found);
    R_CHECK(&rig, pub_ps.namespace_accepted);   /* the relay accepted it     */
    R_CHECK(&rig, w_ps.ns_found == 1);          /* and routed it, exactly once */

    /* A second announce of the same root namespace on the same session is
     * refused by the SESSION with MOQ_ERR_INVAL -- one live announcement per
     * namespace per session. So no second event reaches the relay, and the
     * unchanged ns_found below records session-level de-duplication; it is NOT
     * a proof of relay-side idempotence, which this path cannot reach. */
    moq_announcement_t ann2;
    int rc2 = moq_session_publish_namespace(pub->peer, &pc, rig.now, &ann2);
    R_CHECK(&rig, rc2 == MOQ_ERR_INVAL);
    relay_bind_test_rig_pump(&rig, 24);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    printf("  root_ns(d18): duplicate refused rc=%d, ns_found=%d\n",
           rc2, w_ps.ns_found);
    R_CHECK(&rig, w_ps.ns_found == 1);

    /* Withdrawal: the root namespace goes away exactly once. */
    R_CHECK(&rig, moq_session_publish_namespace_done(pub->peer, ann,
                                                     rig.now) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    printf("  root_ns(d18): ns_gone=%d\n", w_ps.ns_gone);
    R_CHECK(&rig, w_ps.ns_gone == 1);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: root_namespace_end_to_end(d18 accepted+routed)\n");
    }
    return f;
}

/* bind's defense-in-depth on Track Namespace Fields.
 *
 * The session parser rejects a malformed present field before any event reaches
 * the relay, so ann_store's arm cannot be driven through a real production
 * path. This drives the PRODUCTION validator directly through the gated probe
 * rather than copying its predicate: every present field must have len > 0 and
 * non-NULL data, while a zero-FIELD root namespace stays acceptable. */
static int
ann_store_rejects_empty_fields(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 0u;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (ann_store_rejects_empty_fields)\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    static const uint8_t backing[4] = { 'l', 'i', 'v', 'e' };
    moq_bytes_t nul0[1]     = { { NULL, 0 } };
    moq_bytes_t nonnull0[1] = { { backing, 0 } };
    moq_bytes_t trailing[2] = { { backing, 4 }, { backing, 0 } };
    moq_bytes_t good[1]     = { { backing, 4 } };

    bool r_nul0     = moqr_bind_debug_ann_store_probe(rig.bind, 0, 9001,
                          (moqr_ns_t){ (const moq_bytes_t *)nul0, 1 });
    bool r_nonnull0 = moqr_bind_debug_ann_store_probe(rig.bind, 0, 9002,
                          (moqr_ns_t){ (const moq_bytes_t *)nonnull0, 1 });
    bool r_trailing = moqr_bind_debug_ann_store_probe(rig.bind, 0, 9003,
                          (moqr_ns_t){ (const moq_bytes_t *)trailing, 2 });
    bool r_root     = moqr_bind_debug_ann_store_probe(rig.bind, 0, 9004,
                          (moqr_ns_t){ NULL, 0 });
    bool r_good     = moqr_bind_debug_ann_store_probe(rig.bind, 0, 9005,
                          (moqr_ns_t){ (const moq_bytes_t *)good, 1 });
    printf("  ann_store: nul0=%d nonnull0=%d trailing=%d root=%d good=%d\n",
           (int)r_nul0, (int)r_nonnull0, (int)r_trailing, (int)r_root,
           (int)r_good);

    R_CHECK(&rig, !r_nul0);       /* NULL, zero-length                       */
    R_CHECK(&rig, !r_nonnull0);   /* NON-NULL, zero-length                   */
    R_CHECK(&rig, !r_trailing);   /* every field inspected, not only field 0 */
    R_CHECK(&rig, r_root);        /* zero FIELDS is the legal root namespace */
    R_CHECK(&rig, r_good);        /* a well-formed field still stores        */

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: ann_store_rejects_empty_fields\n");
    }
    return f;
}

/* A root FULL TRACK NAME over a real d18 session, through bind and core.
 *
 * draft-18 Section 2.4.1 builds a Full Track Name from a 0..32-field Track
 * Namespace plus a Track Name, so a track may legitimately live directly under
 * the ROOT namespace. This goes further than namespace discovery: it drives
 * subscribe -> upstream publish/open -> routed acceptance through the
 * production entry points. */
static int
root_full_track_name_session_flow(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 0u;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (root_full_track_name_session_flow)\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    relay_bind_test_rig_pump(&rig, 4);

    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));

    /* The publisher owns the ROOT namespace. */
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = NULL, .count = 0 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);

    /* A subscriber asks for a track whose namespace IS the root. */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = NULL, .count = 0 };
    scfg.track_name = B("v");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    int src = moq_session_subscribe(sub->peer, &scfg, rig.now, &sh);
    R_CHECK(&rig, src == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);

    /* The relay must resolve it to the root publisher and issue the upstream
     * subscribe, which the publisher accepts. */
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    printf("  root_ftn: up_subs=%d up_seen=%d subscribe_ok=%d errors=%d\n",
           pub_ps.up_subs, (int)pub_ps.up_seen, (int)sub_ps.subscribe_ok,
           sub_ps.subscribe_errors);
    R_CHECK(&rig, pub_ps.up_subs == 1);       /* routed upstream exactly once */
    R_CHECK(&rig, pub_ps.up_seen);            /* and the publisher accepted it */
    R_CHECK(&rig, sub_ps.subscribe_ok);       /* accepted downstream           */
    R_CHECK(&rig, sub_ps.subscribe_errors == 0);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: root_full_track_name_session_flow\n");
    }
    return f;
}

/* --- mixed-profile namespace discovery ------------------------------------
 *
 * One listener serves draft-16 and draft-18 sessions concurrently over the same
 * relay core and bind. draft-18 Section 2.4.1 allows a 0-field Track Namespace;
 * draft-16 Section 2.4.1 requires 1..32. So a ROOT namespace announced by a d18
 * publisher has NO representation a d16 peer may legally receive: sending it a
 * reconstructed zero-field Track Namespace would put an invalid namespace on a
 * d16 wire.
 *
 * This records what the assembled stack actually does. The acceptable outcome is
 * that the root FOUND/GONE pair is suppressed for the d16 subscriber while its
 * session stays healthy, and that NON-root discovery is unchanged for both. */
static int
mixed_profile_root_discovery(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 0u;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (mixed_profile_root_discovery)\n");
        return 1;
    }
    conn_t *pub18 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *pub16 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_16);
    conn_t *sub16 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_16);
    conn_t *sub18 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub18 && pub16 && sub16 && sub18);
    relay_bind_test_rig_pump(&rig, 6);

    /* Both subscribers take the EMPTY prefix, which is legal in both drafts and
     * matches every namespace. */
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? sub16 : sub18;
        moq_subscribe_namespace_cfg_t nscfg;
        memset(&nscfg, 0, sizeof(nscfg));
        moq_subscribe_namespace_cfg_init(&nscfg);
        nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = NULL,
                                                          .count = 0 };
        nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t nsh;
        R_CHECK(&rig, moq_session_subscribe_namespace(w->peer, &nscfg, rig.now,
                                                      &nsh) == MOQ_OK);
    }
    relay_bind_test_rig_pump(&rig, 8);

    peer_state_t p18, p16, s16, s18;
    memset(&p18, 0, sizeof(p18));
    memset(&p16, 0, sizeof(p16));
    memset(&s16, 0, sizeof(s16));
    memset(&s18, 0, sizeof(s18));

    /* 1. d18 publisher announces the ROOT namespace. */
    moq_publish_namespace_cfg_t root;
    memset(&root, 0, sizeof(root));
    moq_publish_namespace_cfg_init(&root);
    root.track_namespace = (moq_namespace_t){ .parts = NULL, .count = 0 };
    moq_announcement_t root_ann;
    int root_rc = moq_session_publish_namespace(pub18->peer, &root, rig.now,
                                                &root_ann);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, pub18, &p18, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub16, &s16, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub18, &s18, false, false, 0, 0);
    printf("  mixed: root_rc=%d d18pub_accepted=%d | d16sub found=%d "
           "gone=%d | d18sub found=%d gone=%d\n",
           root_rc, (int)p18.namespace_accepted, s16.ns_found, s16.ns_gone,
           s18.ns_found, s18.ns_gone);
    R_CHECK(&rig, root_rc == MOQ_OK);          /* exact d18 root publish     */
    R_CHECK(&rig, p18.namespace_accepted);     /* the relay accepted it      */
    R_CHECK(&rig, s18.ns_found == 1);          /* d18 sees it exactly once   */
    R_CHECK(&rig, s18.ns_gone == 0);
    /* The production contract: a peer whose profile cannot represent a
     * zero-field Track Namespace is never sent one. */
    R_CHECK(&rig, s16.ns_found == 0);   /* no root FOUND on a d16 wire */
    R_CHECK(&rig, s16.ns_gone == 0);
    int s16_root_found = 0;
    /* Suppression happens BEFORE retain/owed accounting, so the d16 peer's
     * root update was never queued, retained or counted as owed. */
    {
        moqr_bind_stats_t rs;
        moqr_bind_get_stats(rig.bind, &rs);
        printf("  mixed: after root live=%u retained=%llu failed=%llu\n",
               rs.nsu_live, (unsigned long long)rs.nsu_retained,
               (unsigned long long)rs.nsu_failed_closed);
        R_CHECK(&rig, rs.nsu_live == 0);
        R_CHECK(&rig, rs.nsu_retained == 0);
        R_CHECK(&rig, rs.nsu_failed_closed == 0);
    }

    /* 2. Withdraw it, and see whether a GONE appears without a FOUND. */
    if (root_rc == MOQ_OK) {
        R_CHECK(&rig, moq_session_publish_namespace_done(
                          pub18->peer, root_ann, rig.now) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 32);
        relay_bind_test_peer_drain(&rig, sub16, &s16, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, sub18, &s18, false, false, 0, 0);
        printf("  mixed: after root withdraw | d16sub found=%d gone=%d | "
               "d18sub found=%d gone=%d\n",
               s16.ns_found, s16.ns_gone, s18.ns_found, s18.ns_gone);
        R_CHECK(&rig, s18.ns_found == 1);   /* still exactly one FOUND ...   */
        R_CHECK(&rig, s18.ns_gone == 1);    /* ... matched by exactly one GONE */
        /* Suppression is SYMMETRIC: no GONE either, so the d16 peer is never
         * left holding half a pair. */
        R_CHECK(&rig, s16.ns_found == 0);
        R_CHECK(&rig, s16.ns_gone == 0);
    }

    /* 3. Health + unchanged non-root discovery: a d18 publisher's ordinary
     *    namespace must still reach BOTH subscribers. */
    moq_bytes_t nsa[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsa, .count = 2 };
    moq_announcement_t a18;
    R_CHECK(&rig, moq_session_publish_namespace(pub18->peer, &pc, rig.now,
                                                &a18) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, sub16, &s16, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub18, &s18, false, false, 0, 0);
    printf("  mixed: d18 non-root | d16sub found=%d | d18sub found=%d\n",
           s16.ns_found, s18.ns_found);
    /* Exact cumulative counts for BOTH profiles. The d16 root baseline is zero
     * because the gate suppressed it, so these totals count only the non-root
     * updates that peer is legitimately owed. */
    R_CHECK(&rig, s18.ns_found == 2);
    R_CHECK(&rig, s16.ns_found == s16_root_found + 1);

    /* 4. The inverse healthy arm: a d16 publisher's non-root namespace is
     *    visible to both empty-prefix subscribers. */
    moq_bytes_t nsb[2] = { B("live"), B("cam2") };
    pc.track_namespace = (moq_namespace_t){ .parts = nsb, .count = 2 };
    moq_announcement_t a16;
    R_CHECK(&rig, moq_session_publish_namespace(pub16->peer, &pc, rig.now,
                                                &a16) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, pub16, &p16, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub16, &s16, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub18, &s18, false, false, 0, 0);
    printf("  mixed: d16 non-root | d16sub found=%d | d18sub found=%d\n",
           s16.ns_found, s18.ns_found);
    R_CHECK(&rig, p16.namespace_accepted);            /* d16 publisher OK    */
    R_CHECK(&rig, s18.ns_found == 3);
    R_CHECK(&rig, s16.ns_found == s16_root_found + 2);

    moqr_bind_stats_t st;
    moqr_bind_get_stats(rig.bind, &st);
    printf("  mixed: conns=%u session_errors=%llu\n", st.conns,
           (unsigned long long)st.session_errors);
    R_CHECK(&rig, st.conns == 4);              /* all four still live        */
    R_CHECK(&rig, st.session_errors == 0);     /* d16 session stayed healthy */

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    return rig.failures;
}

/* --- draft-18 Section 6.2 durability battery -------------------------------
 * A namespace update owed to a matching prefix subscriber is a MUST, so a
 * blocked control path may delay it but must never drop it, duplicate it,
 * reorder it, or let it escape the ring's byte bound.
 * blocked=false arms are the same-order healthy controls. */

/* Park an update, then keep it parked for several pumps before letting the
 * peer drain: repeated WOULD_BLOCK must stay ONE logical update. */
static int
nsu_case(moq_version_t version, bool blocked, bool with_gone, int stall_pumps)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = blocked ? 1u : 0u;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (nsu_case)\n");
        relay_bind_test_max_actions = saved;
        return 1;
    }
    relay_bind_test_max_actions = 0u;        /* publisher: normal queue */
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_max_actions = blocked ? 1u : 0u; /* watcher: the blocked path */
    conn_t *watch = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    if (!blocked) {
        relay_bind_test_rig_pump(&rig, 4);
    }

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    /* Stall: pump WITHOUT draining the watcher so the retry re-blocks. */
    for (int i = 0; i < stall_pumps; i++) {
        relay_bind_test_rig_pump(&rig, 1);
    }
    if (with_gone) {
        /* The publisher's own depth-1 queue may still hold its announce; the
         * subject under test is the WATCHER's blocked path, so let the
         * publisher drain and retry its client-side call. */
        moq_result_t drc = MOQ_ERR_WOULD_BLOCK;
        for (int t = 0; t < 32 && drc == MOQ_ERR_WOULD_BLOCK; t++) {
            drc = moq_session_publish_namespace_done(pub->peer, ann, rig.now);
            if (drc == MOQ_ERR_WOULD_BLOCK) {
                /* Free the PUBLISHER's own depth-1 queue by consuming its
                 * events; the watcher is deliberately left undrained. */
                peer_state_t p_ps;
                memset(&p_ps, 0, sizeof(p_ps));
                relay_bind_test_peer_drain(&rig, pub, &p_ps, false, false, 0, 0);
                relay_bind_test_rig_pump(&rig, 2);
            }
        }
        R_CHECK(&rig, drc == MOQ_OK);
    }
    relay_bind_test_rig_pump(&rig, 64);

    peer_state_t w_ps;
    memset(&w_ps, 0, sizeof(w_ps));
    for (int d = 0; d < 6; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 32);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    printf("  nsu_case(d%d blocked=%d gone=%d stall=%d): found=%d gone=%d "
           "order=%s retained=%llu failed_closed=%llu\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18, (int)blocked,
           (int)with_gone, stall_pumps, w_ps.ns_found, w_ps.ns_gone,
           w_ps.order, (unsigned long long)bs.nsu_retained,
           (unsigned long long)bs.nsu_failed_closed);

    /* Exactly once, never duplicated by the retries. */
    R_CHECK(&rig, w_ps.ns_found == 1);
    if (with_gone) {
        R_CHECK(&rig, w_ps.ns_gone == 1);
        /* GONE must never overtake or cancel the retained FOUND. */
        {
        const char *n = strchr(w_ps.order, 'N');
        const char *g = strchr(w_ps.order, 'G');
        R_CHECK(&rig, n != NULL && g != NULL && n < g);   /* FOUND before GONE */
    }
    }
    /* A recoverable block is not a session error. */
    R_CHECK(&rig, bs.session_errors == 0);
    R_CHECK(&rig, bs.nsu_failed_closed == 0);
    if (!blocked) {
        /* Healthy discovery allocates and parks nothing. */
        R_CHECK(&rig, bs.nsu_retained == 0);
    } else {
        /* Repeated WOULD_BLOCK stays ONE logical update. */
        R_CHECK(&rig, bs.nsu_retained >= 1u);
    }

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* every retained byte released */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_case(d%d blocked=%d gone=%d stall=%d)\n",
               version == MOQ_VERSION_DRAFT_16 ? 16 : 18, (int)blocked,
               (int)with_gone, stall_pumps);
    }
    return f;
}

/* A multi-part suffix with binary and empty components must arrive exactly
 * once and byte-identically after being retained and replayed. */
static int
nsu_multipart_bytes_exact(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 1u;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (nsu_multipart)\n");
        relay_bind_test_max_actions = saved;
        return 1;
    }
    relay_bind_test_max_actions = 0u; /* publisher: normal queue */
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u; /* watcher: the blocked downstream path */
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);

    /* Binary bytes including NUL and high bits, plus a zero-length part. */
    static const uint8_t bin[3] = { 0xFF, 0x00, 0x7F };
    moq_bytes_t nsp[3];
    nsp[0] = B("live");
    nsp[1].data = bin;
    nsp[1].len = sizeof(bin);
    nsp[2] = B("z");
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 3 };
    moq_announcement_t ann;
    moq_result_t arc = moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                     &ann);
    printf("  nsu_multipart announce rc=%d\n", (int)arc);
    R_CHECK(&rig, arc == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 64);
    peer_state_t w_ps;
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 32);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    printf("  nsu_multipart: found=%d parts=%zu lens=%zu,%zu total=%zu\n",
           w_ps.ns_found, w_ps.ns_seen_count, w_ps.ns_seen_lens[0],
           w_ps.ns_seen_lens[1], w_ps.ns_seen_total);
    R_CHECK(&rig, w_ps.ns_found == 1);
    R_CHECK(&rig, w_ps.ns_seen_count == 2);           /* suffix after "live" */
    R_CHECK(&rig, w_ps.ns_seen_lens[0] == sizeof(bin));
    R_CHECK(&rig, w_ps.ns_seen_lens[1] == 1);
    R_CHECK(&rig, w_ps.ns_seen_total == sizeof(bin) + 1);
    R_CHECK(&rig, memcmp(w_ps.ns_seen_bytes, bin, sizeof(bin)) == 0);
    R_CHECK(&rig, w_ps.ns_seen_bytes[sizeof(bin)] == 'z');

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_multipart_bytes_exact\n");
    }
    return f;
}

/* Closing the subscriber while an update is held must purge it: no leak, and
 * no stale handle reused by a later connection. */
static int
nsu_close_purges_held(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_max_actions = 1u;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (nsu_close_purges)\n");
        relay_bind_test_max_actions = saved;
        return 1;
    }
    relay_bind_test_max_actions = 0u; /* publisher: normal queue */
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u; /* watcher: the blocked downstream path */
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 2); /* the update is now held, undelivered */

    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    R_CHECK(&rig, held.nsu_retained >= 1);

    /* Tear the subscriber down while the update is still held. */
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, watch->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 32);

    moqr_bind_stats_t after;
    moqr_bind_get_stats(rig.bind, &after);
    printf("  nsu_close_purges: retained=%llu failed_closed=%llu "
           "session_errors=%llu\n",
           (unsigned long long)after.nsu_retained,
           (unsigned long long)after.nsu_failed_closed,
           (unsigned long long)after.session_errors);
    R_CHECK(&rig, after.nsu_failed_closed == 0);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* purge released the held bytes */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_close_purges_held\n");
    }
    return f;
}

/* Blocked NS_GONE, deterministic: the watcher's SimPair is simply not stepped
 * while the publisher and the real bind pump keep running, so the relay's
 * NAMESPACE_DONE write genuinely meets WOULD_BLOCK and must be retained.
 * draft-18 Section 6.2 owes the withdrawal exactly as it owes the arrival. */
static int
nsu_gone_survives_block(moq_version_t version)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (nsu_gone_survives_block)\n");
        relay_bind_test_max_actions = saved;
        return 1;
    }
    relay_bind_test_max_actions = 0u; /* publisher: never throttled */
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_max_actions = 1u; /* watcher: single-slot outbound edge */
    conn_t *watch = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    /* -- 1. establish and drain phase 1 completely ------------------------ */
    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 16);

    /* ONE cumulative watcher ledger for the whole test: the phase-1 FOUND is
     * never erased, so the final assertion is a true cumulative order. */
    peer_state_t pub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);
    R_CHECK(&rig, w_ps.ns_found == 1);
    R_CHECK(&rig, w_ps.ns_gone == 0);

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_retained == 0);   /* healthy phase 1 parked nothing */

    /* -- 2/3. queue exactly one relay->watcher response, then stop stepping
     *         the watcher so that response can never drain. --------------- */
    moq_track_status_cfg_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    moq_track_status_cfg_init(&tcfg);
    tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    tcfg.track_name = B("video");
    moq_track_status_handle_t tsh;
    R_CHECK(&rig, moq_session_track_status(watch->peer, &tcfg, rig.now,
                                           &tsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 1); /* deliver the request; the answer now occupies the slot */

    /* -- 4. withdraw through the publisher while the watcher stays frozen -- */
    R_CHECK(&rig, moq_session_publish_namespace_done(pub->peer, ann,
                                                     rig.now) == MOQ_OK);
    relay_bind_test_rig_pump_except(&rig, watch, 8);

    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    peer_state_t frozen;
    memset(&frozen, 0, sizeof(frozen));
    relay_bind_test_peer_drain(&rig, watch, &frozen, false, false, 0, 0);
    printf("  nsu_gone(d%d): held retained=%llu (base %llu) gone_seen=%d\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18,
           (unsigned long long)held.nsu_retained,
           (unsigned long long)base.nsu_retained, frozen.ns_gone);

    /* -- 5. exactly one retained update, and no GONE observed yet --------- */
    R_CHECK(&rig, held.nsu_retained == base.nsu_retained + 1u);
    R_CHECK(&rig, frozen.ns_gone == 0);
    R_CHECK(&rig, held.nsu_failed_closed == 0);
    R_CHECK(&rig, held.session_errors == 0);

    /* Keep pumping publisher+bind with the watcher still frozen: the retry
     * must re-block, never duplicate the held update. */
    relay_bind_test_rig_pump_except(&rig, watch, 24);
    moqr_bind_stats_t still;
    moqr_bind_get_stats(rig.bind, &still);
    R_CHECK(&rig, still.nsu_retained == base.nsu_retained + 1u);
    R_CHECK(&rig, still.nsu_failed_closed == 0);

    /* -- 6. resume the watcher: one FOUND then one GONE, exactly once ----- */
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    printf("  nsu_gone(d%d): cumulative found=%d gone=%d order=%s\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18, w_ps.ns_found,
           w_ps.ns_gone, w_ps.order);
    R_CHECK(&rig, w_ps.ns_found == 1);
    R_CHECK(&rig, w_ps.ns_gone == 1);
    {
        const char *n = strchr(w_ps.order, 'N');
        const char *g = strchr(w_ps.order, 'G');
        R_CHECK(&rig, n != NULL && g != NULL && n < g);   /* FOUND before GONE */
    }

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_gone_survives_block(d%d)\n",
               version == MOQ_VERSION_DRAFT_16 ? 16 : 18);
    }
    return f;
}

/* A zero-part suffix: the watcher subscribes to the EXACT namespace, so the
 * update the relay owes carries no tuple fields and no bytes at all. This is
 * what makes the sidecar's explicit occupancy bit load-bearing -- a live slot
 * whose byte pointer is legitimately NULL. */
static int
nsu_zero_suffix_retained(moq_version_t version)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: rig create (nsu_zero_suffix)\n");
        relay_bind_test_max_actions = saved;
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_max_actions = 1u;
    conn_t *watch = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub && watch);
    relay_bind_test_rig_pump(&rig, 4);

    /* Prefix == the whole namespace, so the suffix is empty. */
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = nsp, .count = 2 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    peer_state_t pub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    for (int d = 0; d < 4; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 4);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);

    /* Occupy the watcher's single outbound slot, then freeze its pair. */
    moq_track_status_cfg_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    moq_track_status_cfg_init(&tcfg);
    tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    tcfg.track_name = B("video");
    moq_track_status_handle_t tsh;
    R_CHECK(&rig, moq_session_track_status(watch->peer, &tcfg, rig.now,
                                           &tsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 1);

    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump_except(&rig, watch, 8);

    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    peer_state_t frozen;
    memset(&frozen, 0, sizeof(frozen));
    relay_bind_test_peer_drain(&rig, watch, &frozen, false, false, 0, 0);
    printf("  nsu_zero_suffix(d%d): held retained=%llu (base %llu) found=%d\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18,
           (unsigned long long)held.nsu_retained,
           (unsigned long long)base.nsu_retained, frozen.ns_found);
    /* Retained with an occupied slot even though it owns zero bytes. */
    R_CHECK(&rig, held.nsu_retained == base.nsu_retained + 1u);
    /* Occupancy is tracked even though this payload owns no bytes at all. */
    R_CHECK(&rig, held.nsu_live == 1);
    R_CHECK(&rig, frozen.ns_found == 0);
    R_CHECK(&rig, held.nsu_failed_closed == 0);
    R_CHECK(&rig, held.session_errors == 0);

    /* Repeated block must not duplicate the zero-byte payload. */
    relay_bind_test_rig_pump_except(&rig, watch, 24);
    moqr_bind_stats_t still;
    moqr_bind_get_stats(rig.bind, &still);
    R_CHECK(&rig, still.nsu_retained == base.nsu_retained + 1u);
    R_CHECK(&rig, still.nsu_failed_closed == 0);

    /* Reopen: delivered exactly once, and the suffix really is empty. */
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    printf("  nsu_zero_suffix(d%d): found=%d parts=%zu total=%zu\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18, w_ps.ns_found,
           w_ps.ns_seen_count, w_ps.ns_seen_total);
    R_CHECK(&rig, w_ps.ns_found == 1);
    R_CHECK(&rig, w_ps.ns_seen_count == 0);
    R_CHECK(&rig, w_ps.ns_seen_total == 0);
    /* Release decremented the live-slot ledger: a zero-byte payload is
     * reclaimed exactly like a byte-owning one. */
    moqr_bind_stats_t done;
    moqr_bind_get_stats(rig.bind, &done);
    printf("  nsu_zero_suffix(d%d): live_after=%u\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18, done.nsu_live);
    R_CHECK(&rig, done.nsu_live == 0);

    /* The WITHDRAWAL takes the same blocked-retained-replayed path, so the
     * empty-suffix update is proven for GONE as well as FOUND. Occupy the
     * watcher's slot again, freeze it, withdraw, then release. */
    {
        moq_track_status_cfg_t tcfg2;
        memset(&tcfg2, 0, sizeof(tcfg2));
        moq_track_status_cfg_init(&tcfg2);
        tcfg2.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
        tcfg2.track_name = B("again");
        moq_track_status_handle_t th2;
        (void)moq_session_track_status(watch->peer, &tcfg2, rig.now, &th2);
        relay_bind_test_rig_pump(&rig, 1);
        watch->frozen = true;
        R_CHECK(&rig, moq_session_publish_namespace_done(pub->peer, ann,
                                                         rig.now) == MOQ_OK);
        relay_bind_test_rig_pump_except(&rig, watch, 8);
        moqr_bind_stats_t gheld;
        moqr_bind_get_stats(rig.bind, &gheld);
        R_CHECK(&rig, gheld.nsu_retained == done.nsu_retained + 1u);
        R_CHECK(&rig, gheld.nsu_live == 1);        /* the GONE is held        */
        R_CHECK(&rig, gheld.session_errors == 0);
        watch->frozen = false;
        for (int d = 0; d < 10; d++) {
            relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
            relay_bind_test_rig_pump(&rig, 8);
        }
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        moqr_bind_stats_t gdone;
        moqr_bind_get_stats(rig.bind, &gdone);
        printf("  nsu_zero_suffix(d%d): gone=%d found=%d live_after=%u\n",
               version == MOQ_VERSION_DRAFT_16 ? 16 : 18, w_ps.ns_gone,
               w_ps.ns_found, gdone.nsu_live);
        R_CHECK(&rig, w_ps.ns_gone == 1);   /* delivered exactly once, in order */
        R_CHECK(&rig, w_ps.ns_found == 1);  /* and no extra FOUND appeared      */
        R_CHECK(&rig, gdone.nsu_live == 0); /* owed accounting drains to zero   */
        R_CHECK(&rig, gdone.nsu_failed_closed == 0);
        R_CHECK(&rig, gdone.session_errors == 0);
    }

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* release decremented the live ledger */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_zero_suffix_retained(d%d)\n",
               version == MOQ_VERSION_DRAFT_16 ? 16 : 18);
    }
    return f;
}

/* A hard failure on a ring-HEAD retry detaches its own connection, and detach
 * purges + compacts the ordered ring underneath the caller. If the caller then
 * pops "the head" it discards whatever entry compaction moved there -- another
 * connection's retained namespace update. The purge-epoch stamp is what makes
 * the caller notice the slot it was retrying is gone.
 *
 * A = the subscriber whose parked SUB_DONE hard-fails on retry.
 * B = an independent watcher whose retained NAMESPACE sits behind it. */
static int
nsu_purge_during_ring_retry(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_rv_ann_always_allow = true; /* revoke the SUBSCRIBE only */
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    relay_bind_test_server_streaming_objects = true;
    relay_bind_test_client_streaming_objects = true;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_server_streaming_objects = false;
        relay_bind_test_client_streaming_objects = false;
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_purge_during_ring_retry)\n");
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u;        /* A: single-slot outbound edge */
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_server_streaming_objects = false;
    relay_bind_test_client_streaming_objects = false;
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18); /* B */
    R_CHECK(&rig, pub && sub && watch);
    peer_state_t pub_ps, sub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_rig_pump(&rig, 4);

    /* A subscribes under a revalidation lease; B watches the prefix. */
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, sub, &pub_ps, &sub_ps, nsp);
    R_CHECK(&rig, !sub_ps.done_seen);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    for (int d = 0; d < 4; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 4);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);

    /* A begins an object so its SUB_DONE must RESET a begun subgroup. */
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

    /* The subscribe-grant setup already announced live/cam1, so drain the
     * watcher until that first NAMESPACE is delivered and nothing is held;
     * B's blocked update below must be a NEW one. */
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);

    /* Occupy both subscriber edges, then freeze them: from here the relay's
     * writes to A and B genuinely block, so both entries park in the ring. */
    for (int i = 0; i < 2; i++) {
        conn_t *c = (i == 0) ? sub : watch;
        moq_track_status_cfg_t tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        moq_track_status_cfg_init(&tcfg);
        tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
        tcfg.track_name = B("video");
        moq_track_status_handle_t th;
        (void)moq_session_track_status(c->peer, &tcfg, rig.now, &th);
    }
    relay_bind_test_rig_pump(&rig, 1);
    sub->frozen = true;
    watch->frozen = true;

    /* A's SUB_DONE (revoke) parks FIRST, at the ring head... */
    relay_bind_test_rv_decision = MOQR_AUTH_DENY;
    relay_bind_test_rv_deny_code = 0x7;
    relay_bind_test_rig_pump(&rig, 2);
    /* ...then B's NAMESPACE parks behind it. */
    moq_bytes_t nsp2[2] = { B("live"), B("cam2") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp2, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moqr_bind_stats_t parked;
    moqr_bind_get_stats(rig.bind, &parked);
    printf("  purge_retry: parked retained=%llu (base %llu) live=%u\n",
           (unsigned long long)parked.nsu_retained,
           (unsigned long long)base.nsu_retained, parked.nsu_live);
    /* Non-vacuity: B's update really is held before A's retry runs. */
    R_CHECK(&rig, parked.nsu_retained == base.nsu_retained + 1u);
    R_CHECK(&rig, parked.nsu_live == 1);

    /* A's NEXT ring-head retry hard-fails inside the real reset call. */
    moqr_bind_debug_fail_sg_reset(1);
    relay_bind_test_rig_pump(&rig, 8);

    moqr_bind_stats_t after;
    moqr_bind_get_stats(rig.bind, &after);
    printf("  purge_retry: after A-fail live=%u sess_err=%llu "
           "conns=%u\n",
           after.nsu_live, (unsigned long long)after.session_errors,
           after.conns);
    /* A was closed and detached by the production fail-closed path... */
    R_CHECK(&rig, after.session_errors >= 1);
    R_CHECK(&rig, after.conns < parked.conns);
    /* ...and B's retained update SURVIVED the purge+compaction. */
    R_CHECK(&rig, after.nsu_live == 1);

    /* Reopen B: its namespace arrives exactly once. */
    watch->frozen = false;
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, watch, &w_ps, false, false, 0, 0);
    printf("  purge_retry: B found=%d order=%s\n", w_ps.ns_found, w_ps.order);
    /* One NAMESPACE from setup, plus the one that survived the purge. */
    R_CHECK(&rig, w_ps.ns_found == 2);

    moqr_bind_stats_t fin;
    moqr_bind_get_stats(rig.bind, &fin);
    R_CHECK(&rig, fin.nsu_live == 0);   /* queue drained cleanly */

    sub->frozen = false;
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);         /* no leak */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_purge_during_ring_retry\n");
    }
    return f;
}

/* Ordered cross-kind FIFO: namespace updates and scalar terminals share ONE
 * ordered ring, so output queued for a connection must leave in the order it
 * was produced regardless of kind. The watcher's pair is frozen, so every
 * output below is genuinely parked rather than raced through.
 *
 * ns_first=true : namespace A -> scalar B -> namespace C
 * ns_first=false: scalar A -> namespace B -> scalar C
 *
 * The scalar-first arm needs two INDEPENDENT scalar terminals produced while
 * the watcher edge is frozen. Revoking one subscription at a time (the test
 * hook keys its DENY on the requested track name) yields exactly that: one
 * SUBSCRIBE_DONE per revocation, around a publisher-driven NAMESPACE.
 * The recorded arrival string is the proof: T=TRACK_STATUS_OK, N=NAMESPACE,
 * D=SUBSCRIBE_DONE. */
static int
nsu_cross_kind_order(bool ns_first)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_rv_second_track = !ns_first; /* two revocable subscriptions */
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_cross_kind_order)\n");
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u; /* the watcher: single-slot outbound edge */
    conn_t *w = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && w);
    peer_state_t pub_ps, w_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&w_ps, 0, sizeof(w_ps));
    relay_bind_test_rig_pump(&rig, 4);

    /* The watcher both subscribes to a track (so a revoke yields a scalar
     * SUBSCRIBE_DONE) and watches the prefix (so an announce yields a
     * NAMESPACE). */
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, w, &pub_ps, &w_ps, nsp);
    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(w->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, w, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, w, &w_ps, false, false, 0, 0);

    /* Everything above is delivered; the order string starts fresh here. */
    memset(&w_ps, 0, sizeof(w_ps));
    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);

    /* Occupy the single slot, then freeze: from here nothing drains. */
    moq_track_status_cfg_t tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    moq_track_status_cfg_init(&tcfg);
    tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    tcfg.track_name = B("video");
    moq_track_status_handle_t th1;
    R_CHECK(&rig, moq_session_track_status(w->peer, &tcfg, rig.now, &th1) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 1);
    w->frozen = true;

    moq_bytes_t ns2[2] = { B("live"), B("cam2") };
    moq_bytes_t ns3[2] = { B("live"), B("cam3") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    moq_announcement_t a2, a3;

    if (!ns_first) {
        /* A = scalar: revoke ONLY the "video" subscription. Revocation lands
         * on the next revalidation tick, so pump past the lease BEFORE the
         * namespace is produced, or B would be queued first. */
        relay_bind_test_rv_deny_code = 0x7;
        relay_bind_test_rv_deny_track = "video";
        relay_bind_test_rig_pump_except(&rig, w, 12);
        /* Non-vacuity for THIS arm: scalar A must already occupy the ordered
         * ring before the namespace is produced, and no namespace may have
         * been retained yet. */
        moqr_bind_stats_t after_a;
        moqr_bind_get_stats(rig.bind, &after_a);
        printf("    [A scalar] pending_hw=%u retained=%llu\n",
               after_a.pending_high_water,
               (unsigned long long)after_a.nsu_retained);
        R_CHECK(&rig, after_a.pending_high_water >= 1);
        /* B = namespace. */
        pc.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                    &a2) == MOQ_OK);
        relay_bind_test_rig_pump_except(&rig, w, 6);
        /* B really did join behind A rather than replacing it. */
        {
            moqr_bind_stats_t after_b;
            moqr_bind_get_stats(rig.bind, &after_b);
            R_CHECK(&rig, after_b.nsu_retained > after_a.nsu_retained);
        }
        /* C = scalar: now revoke ONLY the "audio" subscription. */
        relay_bind_test_rv_deny_track = "audio";
        relay_bind_test_rig_pump_except(&rig, w, 12);
    } else {
        /* A = namespace, then B = scalar (revoke), then C = namespace. */
        pc.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                    &a2) == MOQ_OK);
        relay_bind_test_rig_pump_except(&rig, w, 2);
        relay_bind_test_rv_decision = MOQR_AUTH_DENY;
        relay_bind_test_rv_deny_code = 0x7;
        relay_bind_test_rig_pump_except(&rig, w, 4);
        relay_bind_test_rv_decision = MOQR_AUTH_ALLOW;
        pc.track_namespace = (moq_namespace_t){ .parts = ns3, .count = 2 };
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                    &a3) == MOQ_OK);
        relay_bind_test_rig_pump_except(&rig, w, 4);
    }

    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    peer_state_t frozen;
    memset(&frozen, 0, sizeof(frozen));
    relay_bind_test_peer_drain(&rig, w, &frozen, false, false, 0, 0);
    printf("  cross_kind(ns_first=%d): held live=%u retained=%llu frozen_order=%s\n",
           (int)ns_first, held.nsu_live,
           (unsigned long long)held.nsu_retained, frozen.order);
    /* Non-vacuity: the namespace part really is retained, and nothing was
     * attempted inline past the block. */
    R_CHECK(&rig, held.nsu_retained > base.nsu_retained);
    R_CHECK(&rig, frozen.order_len == 0);

    /* Reopen: the arrival order is the proof. */
    w->frozen = false;
    for (int d = 0; d < 10; d++) {
        relay_bind_test_peer_drain(&rig, w, &w_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, w, &w_ps, false, false, 0, 0);
    printf("  cross_kind(ns_first=%d): order=%s\n", (int)ns_first, w_ps.order);
    /* ns_first=0: T(occupier) E(scalar A) N(namespace B) D(scalar C)
     * ns_first=1: T(occupier) N(namespace A) D(scalar B) N(namespace C)
     * Scalar A is a TRACK_STATUS_ERROR (the track is unannounced) -- still a
     * scalar terminal on the same ordered ring, which is what matters here. */
    /* ns_first=1: T(occupier) N(namespace A) D(scalar B) N(namespace C)
     * ns_first=0: T(occupier) D(scalar A) N(namespace B) D(scalar C) */
    R_CHECK(&rig, strcmp(w_ps.order, ns_first ? "TNDN" : "TDND") == 0);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_cross_kind_order(ns_first=%d)\n", (int)ns_first);
    }
    return f;
}

/* Held-state exhaustion must fail CLOSED and stay isolated. Two watchers hold
 * the same prefix, so one announce owes a NAMESPACE to both; the suffix copy
 * for whichever is served first is made to fail its allocation. That watcher
 * must be closed and detached, while the other's retained update survives and
 * is delivered exactly once. */
static int
nsu_exhaustion_fail_closed(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_exhaustion_fail_closed)\n");
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u;
    conn_t *w1 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *w2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && w1 && w2);
    peer_state_t p1, p2;
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? w1 : w2;
        moq_subscribe_namespace_cfg_t nscfg;
        memset(&nscfg, 0, sizeof(nscfg));
        moq_subscribe_namespace_cfg_init(&nscfg);
        nscfg.track_namespace_prefix =
            (moq_namespace_t){ .parts = pfx, .count = 1 };
        nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t nsh;
        R_CHECK(&rig, moq_session_subscribe_namespace(w->peer, &nscfg, rig.now,
                                                      &nsh) == MOQ_OK);
    }
    for (int d = 0; d < 6; d++) {
        relay_bind_test_peer_drain(&rig, w1, &p1, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, w2, &p2, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 6);
    }
    relay_bind_test_peer_drain(&rig, w1, &p1, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, w2, &p2, false, false, 0, 0);

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);
    R_CHECK(&rig, base.nsu_failed_closed == 0);
    uint32_t conns0 = base.conns;

    /* Occupy both edges and freeze, so the announce below must be retained. */
    moq_bytes_t ns2[2] = { B("live"), B("cam2") };
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? w1 : w2;
        moq_track_status_cfg_t tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        moq_track_status_cfg_init(&tcfg);
        tcfg.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
        tcfg.track_name = B("video");
        moq_track_status_handle_t th;
        (void)moq_session_track_status(w->peer, &tcfg, rig.now, &th);
    }
    relay_bind_test_rig_pump(&rig, 1);
    w1->frozen = true;
    w2->frozen = true;

    /* Fail the next retention exactly once, so ONE watcher's update cannot be
     * held. Injected at the ownership boundary rather than by allocation size,
     * because the announce path itself allocates same-sized blocks. */
    moqr_bind_debug_fail_nsu_store(1);

    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ann) == MOQ_OK);
    /* Both watchers are frozen, so relay_bind_test_rig_pump alone leaves their edges shut:
     * the survivor's update must still be HELD when we look. */
    relay_bind_test_rig_pump(&rig, 8);

    moqr_bind_stats_t hit;
    moqr_bind_get_stats(rig.bind, &hit);
    printf("  exhaustion: failed_closed=%llu ordered_failed=%llu live=%u "
           "conns %u->%u\n",
           (unsigned long long)hit.nsu_failed_closed,
           (unsigned long long)hit.ordered_failed_closed, hit.nsu_live,
           conns0, hit.conns);
    /* Fail closed, not drop-and-continue: the affected conn is gone. */
    R_CHECK(&rig, hit.nsu_failed_closed >= 1);
    R_CHECK(&rig, hit.conns < conns0);
    /* Isolation: the OTHER watcher's update is still owned. */
    R_CHECK(&rig, hit.nsu_live == 1);

    /* And it delivers exactly once when its edge reopens. */
    w1->frozen = false;
    w2->frozen = false;
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, w1, &p1, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, w2, &p2, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, w1, &p1, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, w2, &p2, false, false, 0, 0);
    printf("  exhaustion: survivor found w1=%d w2=%d\n", p1.ns_found,
           p2.ns_found);
    R_CHECK(&rig, p1.ns_found + p2.ns_found == 1);

    moqr_bind_stats_t fin;
    moqr_bind_get_stats(rig.bind, &fin);
    R_CHECK(&rig, fin.nsu_live == 0);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);   /* every allocation returned */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_exhaustion_fail_closed\n");
    }
    return f;
}

/* The ordered ring is SHARED by namespace updates and scalar terminals, so a
 * full ring must fail closed on EITHER insertion kind, and must take down only
 * the connection whose output could not be queued. The injection drives the
 * same bind_ordered_room() predicate production uses, so an injected full and
 * a genuinely full ring cannot diverge.
 *
 * ns_insert=true : the refused insertion is a NAMESPACE update.
 * ns_insert=false: the refused insertion is a scalar terminal. */
static int
nsu_ring_full_fail_closed(bool ns_insert)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_rv_second_track = !ns_insert;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_ring_full_fail_closed)\n");
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u;
    conn_t *victim = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *other = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && victim && other);
    peer_state_t pub_ps, vp, op;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&vp, 0, sizeof(vp));
    memset(&op, 0, sizeof(op));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_bytes_t pfx[1] = { B("live") };
    /* The victim also holds subscriptions, so a revoke gives a scalar. */
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, victim, &pub_ps, &vp, nsp);
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? victim : other;
        moq_subscribe_namespace_cfg_t nscfg;
        memset(&nscfg, 0, sizeof(nscfg));
        moq_subscribe_namespace_cfg_init(&nscfg);
        nscfg.track_namespace_prefix =
            (moq_namespace_t){ .parts = pfx, .count = 1 };
        nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t nsh;
        R_CHECK(&rig, moq_session_subscribe_namespace(w->peer, &nscfg, rig.now,
                                                      &nsh) == MOQ_OK);
    }
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, victim, &vp, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, other, &op, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 6);
    }
    relay_bind_test_peer_drain(&rig, victim, &vp, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, other, &op, false, false, 0, 0);

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);
    uint32_t conns0 = base.conns;

    /* Occupy both watcher edges and freeze them. */
    moq_bytes_t ns2[2] = { B("live"), B("cam2") };
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? victim : other;
        moq_track_status_cfg_t tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        moq_track_status_cfg_init(&tcfg);
        tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
        tcfg.track_name = B("video");
        moq_track_status_handle_t th;
        (void)moq_session_track_status(w->peer, &tcfg, rig.now, &th);
    }
    relay_bind_test_rig_pump(&rig, 1);
    victim->frozen = true;
    other->frozen = true;

    /* Give `other` a genuinely HELD namespace first: it must survive. */
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
    moq_announcement_t a2;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &a2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    R_CHECK(&rig, held.nsu_live >= 1);   /* non-vacuity: something IS held */

    /* Now refuse exactly one insertion of the kind under test. */
    moqr_bind_debug_ring_full(1, ns_insert);
    if (ns_insert) {
        moq_bytes_t ns3[2] = { B("live"), B("cam3") };
        pc.track_namespace = (moq_namespace_t){ .parts = ns3, .count = 2 };
        moq_announcement_t a3;
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                    &a3) == MOQ_OK);
    } else {
        relay_bind_test_rv_deny_track = "video";
        relay_bind_test_rv_deny_code = 0x7;
    }
    relay_bind_test_rig_pump(&rig, 12);

    moqr_bind_stats_t hit;
    moqr_bind_get_stats(rig.bind, &hit);
    printf("  ring_full(ns=%d): nsu_fc=%llu ord_fc=%llu live=%u conns %u->%u\n",
           (int)ns_insert, (unsigned long long)hit.nsu_failed_closed,
           (unsigned long long)hit.ordered_failed_closed, hit.nsu_live,
           conns0, hit.conns);
    /* Fail closed: the refused connection is gone, in the RIGHT counter
     * domain (namespace failures never counted as generic, or vice versa). */
    R_CHECK(&rig, hit.conns < conns0);
    if (ns_insert) {
        R_CHECK(&rig, hit.nsu_failed_closed >= 1);
    } else {
        R_CHECK(&rig, hit.ordered_failed_closed >= 1);
    }
    /* Isolation: the other connection's held entry was NOT disturbed. */
    R_CHECK(&rig, hit.nsu_live >= 1);

    /* Reopen: the survivor still delivers. */
    victim->frozen = false;
    other->frozen = false;
    for (int d = 0; d < 10; d++) {
        relay_bind_test_peer_drain(&rig, victim, &vp, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, other, &op, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, other, &op, false, false, 0, 0);
    printf("  ring_full(ns=%d): survivor other found=%d\n", (int)ns_insert,
           op.ns_found);
    R_CHECK(&rig, op.ns_found >= 1);

    moqr_bind_stats_t fin;
    moqr_bind_get_stats(rig.bind, &fin);
    R_CHECK(&rig, fin.nsu_live == 0);   /* queue drained, no sidecar stranded */

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);         /* teardown returns every allocation */
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_ring_full_fail_closed(ns_insert=%d)\n",
               (int)ns_insert);
    }
    return f;
}

/* One insertion, ONE admission decision. A namespace update admitted as a
 * namespace must not additionally be admitted as a scalar: a second, scalar-
 * classified room check would let a namespace insertion answer to a refusal
 * aimed at scalars, and would leave a live sidecar with no FIFO entry behind it
 * -- a dropped mandatory update on a session that stays open.
 *
 * A scalar-targeted refusal is armed while a namespace update is the very next
 * thing to park. The namespace must park intact and keep the refusal armed for
 * the scalar that follows. */
static int
nsu_ns_insert_keeps_scalar_full(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_rv_ann_always_allow = true;
    relay_bind_test_rv_second_track = true; /* two tracks: revoking one gives a scalar */
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_ns_insert_keeps_scalar_full)\n");
        return 1;
    }
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_max_actions = 1u;
    conn_t *watcher = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *victim = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && watcher && victim);
    peer_state_t pub_ps, wp, vp;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&wp, 0, sizeof(wp));
    memset(&vp, 0, sizeof(vp));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_bytes_t pfx[1] = { B("live") };
    /* The victim carries the subscriptions whose revoke yields the scalar. */
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, victim, &pub_ps, &vp, nsp);
    /* Only the watcher owes NAMESPACE updates. */
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(watcher->peer, &nscfg,
                                                  rig.now, &nsh) == MOQ_OK);
    for (int d = 0; d < 8; d++) {
        relay_bind_test_peer_drain(&rig, watcher, &wp, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, victim, &vp, false, false, 0, 0);
        relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 6);
    }
    relay_bind_test_peer_drain(&rig, watcher, &wp, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, victim, &vp, false, false, 0, 0);

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);
    uint32_t conns0 = base.conns;
    int found0 = wp.ns_found;

    /* Occupy both edges and freeze them, so the next output of either kind
     * must join the ordered ring rather than go out inline. */
    for (int i = 0; i < 2; i++) {
        conn_t *w = (i == 0) ? watcher : victim;
        moq_track_status_cfg_t tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        moq_track_status_cfg_init(&tcfg);
        tcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
        tcfg.track_name = B("video");
        moq_track_status_handle_t th;
        (void)moq_session_track_status(w->peer, &tcfg, rig.now, &th);
    }
    relay_bind_test_rig_pump(&rig, 1);
    watcher->frozen = true;
    victim->frozen = true;

    /* Arm a SCALAR-targeted refusal, then park a NAMESPACE update. */
    moqr_bind_debug_ring_full(1, false);
    moq_bytes_t ns2[2] = { B("live"), B("cam2") };
    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = ns2, .count = 2 };
    moq_announcement_t a2;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &a2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);

    moqr_bind_stats_t held;
    moqr_bind_get_stats(rig.bind, &held);
    printf("  ns_keeps_scalar: held retained=%llu live=%u nsu_fc=%llu "
           "ord_fc=%llu conns %u->%u\n",
           (unsigned long long)(held.nsu_retained - base.nsu_retained),
           held.nsu_live, (unsigned long long)held.nsu_failed_closed,
           (unsigned long long)held.ordered_failed_closed, conns0, held.conns);
    /* The namespace parked as a namespace: one held update, nothing failed
     * closed, and the connection it targets is untouched. */
    R_CHECK(&rig, held.nsu_retained == base.nsu_retained + 1u);
    R_CHECK(&rig, held.nsu_live == 1u);
    R_CHECK(&rig, held.nsu_failed_closed == base.nsu_failed_closed);
    R_CHECK(&rig, held.ordered_failed_closed == base.ordered_failed_closed);
    R_CHECK(&rig, held.conns == conns0);

    /* The scalar refusal must STILL be armed: the next scalar terminal is the
     * one that consumes it, on the scalar fail-closed path. */
    relay_bind_test_rv_deny_track = "video";
    relay_bind_test_rv_deny_code = 0x7;
    relay_bind_test_rig_pump(&rig, 12);

    moqr_bind_stats_t hit;
    moqr_bind_get_stats(rig.bind, &hit);
    printf("  ns_keeps_scalar: scalar ord_fc=%llu nsu_fc=%llu live=%u "
           "conns %u->%u\n",
           (unsigned long long)hit.ordered_failed_closed,
           (unsigned long long)hit.nsu_failed_closed, hit.nsu_live,
           conns0, hit.conns);
    R_CHECK(&rig, hit.ordered_failed_closed >= base.ordered_failed_closed + 1u);
    R_CHECK(&rig, hit.nsu_failed_closed == base.nsu_failed_closed);
    R_CHECK(&rig, hit.conns < conns0);        /* the VICTIM closed, not both */
    R_CHECK(&rig, hit.nsu_live == 1u);        /* the held update is untouched */

    /* The watcher stayed live and still owns exactly one queued update. */
    watcher->frozen = false;
    for (int d = 0; d < 10; d++) {
        relay_bind_test_peer_drain(&rig, watcher, &wp, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, watcher, &wp, false, false, 0, 0);
    printf("  ns_keeps_scalar: watcher found %d->%d\n", found0, wp.ns_found);
    R_CHECK(&rig, wp.ns_found == found0 + 1);   /* delivered exactly once */

    moqr_bind_stats_t fin;
    moqr_bind_get_stats(rig.bind, &fin);
    R_CHECK(&rig, fin.nsu_live == 0);   /* no sidecar stranded off the FIFO */

    moqr_bind_debug_ring_full(0, false);
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_ns_insert_keeps_scalar_full\n");
    }
    return f;
}

/* Operation-specific WOULD_BLOCK: byte capacity, not a connection-wide fact.
 *
 * `send_namespace` stack-encodes into MOQ_FULL_TRACK_NAME_MAX+128 and treats a
 * send-arena shortfall as temporary WOULD_BLOCK. So with the arena mostly
 * consumed, a LONG namespace is refused while a SHORT one still fits the same
 * residual -- the case the frozen-pair fixtures could not express, because
 * they made every refusal uniform.
 *
 * O (large) queues and holds the arena; A (long) is byte-refused and parks;
 * B (small) provably fits that same residual. Production must still deliver
 * exactly O, A, B once each and in that order. Identity is asserted, not
 * counts: each namespace's suffix begins with its own marker byte.
 *
 * matched_control=true runs the same state but issues B INSTEAD of A, proving
 * B really crosses at the identical pre-drain point. */
static int
nsu_byte_capacity_order(bool matched_control)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    uint32_t saved = relay_bind_test_max_actions;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_max_actions = saved;
        printf("FAIL: rig create (nsu_byte_capacity_order)\n");
        return 1;
    }
    /* Action slots are deliberately GENEROUS: any refusal below must be byte
     * capacity, never slot exhaustion. */
    relay_bind_test_max_actions = 0u;
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *w = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && w);
    peer_state_t pub_ps, wp;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&wp, 0, sizeof(wp));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t pfx[1] = { B("live") };
    moq_subscribe_namespace_cfg_t nscfg;
    memset(&nscfg, 0, sizeof(nscfg));
    moq_subscribe_namespace_cfg_init(&nscfg);
    nscfg.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
    nscfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(w->peer, &nscfg, rig.now,
                                                  &nsh) == MOQ_OK);
    for (int d = 0; d < 6; d++) {
        relay_bind_test_peer_drain(&rig, w, &wp, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 6);
    }
    relay_bind_test_peer_drain(&rig, w, &wp, false, false, 0, 0);
    memset(&wp, 0, sizeof(wp));

    moqr_bind_stats_t base;
    moqr_bind_get_stats(rig.bind, &base);
    R_CHECK(&rig, base.nsu_live == 0);

    /* Three namespaces under the same prefix, each individually inside the
     * full-track-name ceiling, distinguished by their first suffix byte. */
    static uint8_t big_o[2400], big_a[1900], small_b[8];
    memset(big_o, 'o', sizeof(big_o));   big_o[0] = 'O';
    memset(big_a, 'a', sizeof(big_a));   big_a[0] = 'A';
    memset(small_b, 'b', sizeof(small_b)); small_b[0] = 'B';
    R_CHECK(&rig, 4 + sizeof(big_o) < MOQ_FULL_TRACK_NAME_MAX);
    R_CHECK(&rig, 4 + sizeof(big_a) < MOQ_FULL_TRACK_NAME_MAX);

    moq_bytes_t nO[2] = { B("live"), { big_o, sizeof(big_o) } };
    moq_bytes_t nA[2] = { B("live"), { big_a, sizeof(big_a) } };
    moq_bytes_t nB[2] = { B("live"), { small_b, sizeof(small_b) } };

    /* Freeze the watcher direction so queued send bytes stay resident. */
    w->frozen = true;

    moq_publish_namespace_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    moq_publish_namespace_cfg_init(&pc);
    moq_announcement_t aO, aB;

    /* (1) O prefills the arena and must really queue. */
    pc.track_namespace = (moq_namespace_t){ .parts = nO, .count = 2 };
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &aO) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    moqr_bind_stats_t afterO;
    moqr_bind_get_stats(rig.bind, &afterO);

    /* (2) the second namespace: long A (refused on bytes) or, in the matched
     *     control, small B at the IDENTICAL residual state. */
    if (!matched_control) {
        /* A and B are queued BACK-TO-BACK with no intervening pump, then the
         * publisher pair is advanced so both reach the relay, then the bind
         * pump runs EXACTLY ONCE. So A fresh-attempts and parks, and B is the
         * immediately following intent in that same batch -- the only shape
         * in which `parked -> blocked` can be the thing that stops B. */
        /* One SimPair advancement does not deterministically carry two
         * publisher requests, so the two announces are driven as CORE inputs
         * in order. The output path under test -- bind intent execution and
         * the real session sends -- is unchanged. */
        R_CHECK(&rig, afterO.nsu_live == 0);   /* no prior pending A existed */
        moqr_binding_t ab;
        R_CHECK(&rig, moqr_core_binding_open(rig.core, 9001, &ab) == MOQR_OK);
        R_CHECK(&rig, moqr_core_announce(
                          rig.core, ab,
                          (moqr_ns_t){ (const moq_bytes_t *)nA, 2 }) ==
                          MOQR_OK);
        R_CHECK(&rig, moqr_core_announce(
                          rig.core, ab,
                          (moqr_ns_t){ (const moq_bytes_t *)nB, 2 }) ==
                          MOQR_OK);
        relay_bind_test_rig_bind_pump_once(&rig);
    } else {
        pc.track_namespace = (moq_namespace_t){ .parts = nB, .count = 2 };
        R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                    &aB) == MOQ_OK);
        relay_bind_test_rig_step_pair(&rig, pub);
        relay_bind_test_rig_bind_pump_once(&rig);
    }
    moqr_bind_stats_t after2;
    moqr_bind_get_stats(rig.bind, &after2);
    printf("  byte_cap(control=%d): afterO live=%u retained=%llu | after2 "
           "live=%u retained=%llu errors=%llu\n",
           (int)matched_control, afterO.nsu_live,
           (unsigned long long)afterO.nsu_retained, after2.nsu_live,
           (unsigned long long)after2.nsu_retained,
           (unsigned long long)after2.session_errors);
    /* Whatever happened, it was NOT a hard error and NOT fail-closed. */
    R_CHECK(&rig, after2.session_errors == 0);
    R_CHECK(&rig, after2.nsu_failed_closed == 0);
    R_CHECK(&rig, after2.ordered_failed_closed == 0);
    if (!matched_control) {
        /* BOTH A and B were retained in that single pump: A on real byte
         * capacity, B because the propagation-set `blocked` force-parked it.
         * Action slots are unconstrained, so neither refusal is slot-driven. */
        R_CHECK(&rig, after2.nsu_retained == afterO.nsu_retained + 2u);
        R_CHECK(&rig, after2.nsu_live == 2);
        R_CHECK(&rig, after2.pending_high_water >= 2);
    } else {
        /* The matched control crosses at the same residual: nothing parked. */
        R_CHECK(&rig, after2.nsu_retained == afterO.nsu_retained);
    }

    /* Nothing may have crossed while frozen. */
    peer_state_t frozen;
    memset(&frozen, 0, sizeof(frozen));
    relay_bind_test_peer_drain(&rig, w, &frozen, false, false, 0, 0);
    R_CHECK(&rig, frozen.ns_found == 0);

    /* Release and observe identity + order. */
    w->frozen = false;
    for (int d = 0; d < 12; d++) {
        relay_bind_test_peer_drain(&rig, w, &wp, false, false, 0, 0);
        relay_bind_test_rig_pump(&rig, 8);
    }
    relay_bind_test_peer_drain(&rig, w, &wp, false, false, 0, 0);
    printf("  byte_cap(control=%d): found=%d ids=%s\n", (int)matched_control,
           wp.ns_found, wp.ns_ids);
    if (matched_control) {
        /* B really crosses at that residual: O then B, once each. */
        R_CHECK(&rig, strcmp(wp.ns_ids, "OB") == 0);
    } else {
        R_CHECK(&rig, strcmp(wp.ns_ids, "OAB") == 0);
    }

    moqr_bind_stats_t fin;
    moqr_bind_get_stats(rig.bind, &fin);
    R_CHECK(&rig, fin.nsu_live == 0);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    relay_bind_test_max_actions = saved;
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: nsu_byte_capacity_order(control=%d)\n",
               (int)matched_control);
    }
    return f;
}

/* This relay has one source slot per exact namespace. Drafts 16 and 18 permit
 * establishment/idle time to select which publisher to reject or disconnect
 * when an implementation cannot retain every publisher. Pin the relay's
 * deterministic policy: a newly established publisher replaces the incumbent,
 * the old generation is cancelled and purged, and subsequent demand is routed
 * only to the replacement. */
static int
publish_namespace_newest_wins(moq_version_t version)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: publish_namespace_newest_wins rig create\n");
        return 1;
    }
    conn_t *old_pub = relay_bind_test_rig_connect(&rig, version);
    conn_t *new_pub = relay_bind_test_rig_connect(&rig, version);
    conn_t *sub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, old_pub != NULL && new_pub != NULL && sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t parts[2] = { B("show"), B("camera") };
    moq_publish_namespace_cfg_t pcfg;
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    moq_announcement_t old_ann;
    moq_announcement_t new_ann;
    R_CHECK(&rig, moq_session_publish_namespace(old_pub->peer, &pcfg, rig.now,
                                                &old_ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t old_state;
    peer_state_t new_state;
    memset(&old_state, 0, sizeof(old_state));
    memset(&new_state, 0, sizeof(new_state));
    relay_bind_test_peer_drain(&rig, old_pub, &old_state, false, false, 0, 0);
    R_CHECK(&rig, old_state.namespace_accepted);

    /* Establish an old-generation track before replacement. The takeover must
     * retire this source and terminate its downstream subscriber, not merely
     * swap the namespace owner while stale track state survives. */
    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    scfg.track_name = B("video");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t old_sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &old_sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, old_pub, &old_state, true, false, 0, 0);
    R_CHECK(&rig, old_state.up_subs == 1 && old_state.up_seen);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t old_sub_state;
    memset(&old_sub_state, 0, sizeof(old_sub_state));
    relay_bind_test_peer_drain(&rig, sub, &old_sub_state, false, false, 0, 0);
    R_CHECK(&rig, old_sub_state.subscribe_ok);

    R_CHECK(&rig, moq_session_publish_namespace(new_pub->peer, &pcfg, rig.now,
                                                &new_ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 10);
    relay_bind_test_peer_drain(&rig, old_pub, &old_state, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, new_pub, &new_state, false, false, 0, 0);
    R_CHECK(&rig, old_state.ns_cancelled == 1);
    R_CHECK(&rig, old_state.ns_cancelled_code ==
                      (version == MOQ_VERSION_DRAFT_18
                           ? 0u
                           : MOQ_REQUEST_ERROR_UNINTERESTED));
    R_CHECK(&rig, new_state.namespace_accepted);
    R_CHECK(&rig, new_state.ns_rejected == 0);
    relay_bind_test_peer_drain(&rig, sub, &old_sub_state, false, false, 0, 0);
    R_CHECK(&rig, old_sub_state.done_count == 1);

    /* A duplicate from the winning session is still invalid and must not
     * evict that session's own accepted announcement. */
    moq_announcement_t duplicate;
    R_CHECK(&rig, moq_session_publish_namespace(new_pub->peer, &pcfg, rig.now,
                                                &duplicate) == MOQ_ERR_INVAL);

    conn_t *new_sub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, new_sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(new_sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, old_pub, &old_state, true, false, 0, 0);
    relay_bind_test_peer_drain(&rig, new_pub, &new_state, true, false, 0, 0);
    R_CHECK(&rig, old_state.up_subs == 1);
    R_CHECK(&rig, new_state.up_subs == 1);
    R_CHECK(&rig, new_state.up_seen);

    /* Cancellation frees the losing session's request. It may publish the
     * namespace again later; the binding mirror must have retired the old
     * handle or repeated reconnect/takeover cycles consume slots and refuse a
     * publisher whose wire state is valid. */
    memset(&old_state, 0, sizeof(old_state));
    memset(&new_state, 0, sizeof(new_state));
    moq_announcement_t third_ann;
    R_CHECK(&rig, moq_session_publish_namespace(old_pub->peer, &pcfg, rig.now,
                                                &third_ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 10);
    relay_bind_test_peer_drain(&rig, old_pub, &old_state, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, new_pub, &new_state, false, false, 0, 0);
    R_CHECK(&rig, old_state.namespace_accepted);
    R_CHECK(&rig, old_state.ns_rejected == 0);
    R_CHECK(&rig, new_state.ns_cancelled == 1);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    if (f == 0 && rig.failures == 0) {
        printf("PASS: publish_namespace_newest_wins (draft=%llu)\n",
               (unsigned long long)version);
    }
    return f + rig.failures;
}

/* A track table full of LIVE tracks refuses a SUBSCRIBE that needs a new
 * track with the draft's capacity code and a nonzero, jittered Retry
 * Interval:
 * draft-18 EXCESSIVE_LOAD (0x9, REQUEST_ERROR Section 10.6) and draft-16
 * INTERNAL_ERROR (0x0; its Section 13.4.2 registry has no load code). The
 * refusal is counted under refusals[tracks], and the live track is
 * untouched. */
static int
track_capacity_code_case(moq_version_t version, uint64_t want_code,
                         const char *label)
{
    int f = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_tracks = 1;
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_tracks = 0;
    if (crc != MOQR_OK) {
        printf("FAIL: track_capacity_code(%s): rig\n", label);
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    conn_t *s1 = relay_bind_test_rig_connect(&rig, version);
    conn_t *s2 = relay_bind_test_rig_connect(&rig, version);
    peer_state_t pub_ps, s1_ps, s2_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&s1_ps, 0, sizeof(s1_ps));
    memset(&s2_ps, 0, sizeof(s2_ps));
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("cap"), B("x") };
    moq_publish_namespace_cfg_t pcfg;
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    (void)moq_session_publish_namespace(pub->peer, &pcfg, rig.now, &ann);
    relay_bind_test_rig_pump(&rig, 4);

    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("live");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t h1, h2;
    R_CHECK(&rig, moq_session_subscribe(s1->peer, &scfg, rig.now, &h1) ==
                           MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, s1, &s1_ps, false, false, 0, 0);
    R_CHECK(&rig, s1_ps.subscribe_ok && s1_ps.subscribe_errors == 0);

    scfg.track_name = B("other");
    R_CHECK(&rig, moq_session_subscribe(s2->peer, &scfg, rig.now, &h2) ==
                           MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, s2, &s2_ps, false, false, 0, 0);
    R_CHECK(&rig, s2_ps.subscribe_errors == 1);
    R_CHECK(&rig, s2_ps.sub_error_code == want_code);
    R_CHECK(&rig, s2_ps.sub_error_can_retry);
    /* Jittered per request around one second (d18 Section 10.6). */
    R_CHECK(&rig, s2_ps.sub_error_retry_ms >= 750 &&
                      s2_ps.sub_error_retry_ms <= 1250);
    /* Each refused request gets its own interval, so refused clients are
     * spread out, not synchronized. One pair can still land on the same
     * value (one in 501), so three refusals must not all share one. */
    uint64_t retries[3] = { s2_ps.sub_error_retry_ms, 0, 0 };
    static const char *const more[] = { "third", "fourth" };
    for (int k = 0; k < 2; k++) {
        moq_subscription_t hk;
        scfg.track_name = B(more[k]);
        R_CHECK(&rig, moq_session_subscribe(s2->peer, &scfg, rig.now, &hk) ==
                          MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        memset(&s2_ps, 0, sizeof(s2_ps));
        relay_bind_test_peer_drain(&rig, s2, &s2_ps, false, false, 0, 0);
        R_CHECK(&rig, s2_ps.subscribe_errors == 1 && s2_ps.sub_error_can_retry);
        R_CHECK(&rig, s2_ps.sub_error_retry_ms >= 750 &&
                          s2_ps.sub_error_retry_ms <= 1250);
        retries[k + 1] = s2_ps.sub_error_retry_ms;
    }
    R_CHECK(&rig, retries[0] != retries[1] || retries[1] != retries[2]);
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.refusals[MOQR_REFUSE_TRACKS] == 3);
    R_CHECK(&rig, st.tracks == 1 && st.subs_active == 1);

    f += rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: track_capacity_code(%s): leak %ld\n", label, a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: track_capacity_code(%s)\n", label);
    }
    return f;
}

static int
test_track_capacity_code(void)
{
    int f = 0;
    f += track_capacity_code_case(MOQ_VERSION_DRAFT_18,
                                  MOQ_REQUEST_ERROR_EXCESSIVE_LOAD, "d18");
    f += track_capacity_code_case(MOQ_VERSION_DRAFT_16,
                                  MOQ_REQUEST_ERROR_INTERNAL_ERROR, "d16");
    return f;
}

/* A PUBLISH_NAMESPACE that a trie full of LIVE announces cannot admit is
 * refused with the draft's capacity code and a Retry Interval, whether it is
 * handled inline or resumed after a deferred authorization: draft-18
 * EXCESSIVE_LOAD, draft-16 INTERNAL_ERROR. */
static int
ns_capacity_code_case(moq_version_t version, bool deferred,
                      uint64_t want_code, const char *label)
{
    int f = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_ns_nodes = 2; /* root + one namespace */
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    relay_bind_test_authorize = deferred ? relay_bind_test_defer_hook : NULL;
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_ns_nodes = 0;
    if (crc != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        printf("FAIL: ns_capacity_code(%s): rig\n", label);
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t a1[1] = { B("live") };
    moq_publish_namespace_cfg_t pc;
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = a1, .count = 1 };
    moq_announcement_t h1, h2;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &h1) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.namespace_accepted && ps.ns_rejected == 0);

    if (deferred) {
        relay_bind_test_defer_action = MOQR_AUTH_PUBLISH_NAMESPACE;
        relay_bind_test_defer_last = 0;
    }
    moq_bytes_t a2[1] = { B("more") };
    pc.track_namespace = (moq_namespace_t){ .parts = a2, .count = 1 };
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &h2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    if (deferred) {
        memset(&ps, 0, sizeof(ps));
        relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
        R_CHECK(&rig, ps.ns_rejected == 0);   /* parked, not refused yet */
        R_CHECK(&rig, relay_bind_test_defer_last != 0);
        R_CHECK(&rig, moqr_bind_auth_resolve(
                          rig.bind, relay_bind_test_defer_last,
                          &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                          rig.now) == MOQR_OK);
        relay_bind_test_rig_pump(&rig, 6);
    }
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.ns_rejected == 1);
    R_CHECK(&rig, ps.ns_rejected_code == want_code);
    R_CHECK(&rig, ps.ns_rejected_can_retry);
    R_CHECK(&rig, ps.ns_rejected_retry_ms >= 750 &&
                      ps.ns_rejected_retry_ms <= 1250);
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.refusals[MOQR_REFUSE_NS_NODES] == 1);

    f += rig.failures;
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    if (a.live != 0) {
        printf("FAIL: ns_capacity_code(%s): leak %ld\n", label, a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: ns_capacity_code(%s)\n", label);
    }
    return f;
}

static int
test_ns_capacity_code(void)
{
    int f = 0;
    f += ns_capacity_code_case(MOQ_VERSION_DRAFT_18, false,
                               MOQ_REQUEST_ERROR_EXCESSIVE_LOAD, "d18 inline");
    f += ns_capacity_code_case(MOQ_VERSION_DRAFT_16, false,
                               MOQ_REQUEST_ERROR_INTERNAL_ERROR, "d16 inline");
    f += ns_capacity_code_case(MOQ_VERSION_DRAFT_18, true,
                               MOQ_REQUEST_ERROR_EXCESSIVE_LOAD,
                               "d18 deferred");
    f += ns_capacity_code_case(MOQ_VERSION_DRAFT_16, true,
                               MOQ_REQUEST_ERROR_INTERNAL_ERROR,
                               "d16 deferred");
    return f;
}

/* An acceptance that meets a full session action queue is still sent. The
 * accepted connection's relay-side queue holds three actions; three
 * PUBLISH_NAMESPACE acceptances fill it in the same pump that admits a
 * PUBLISH_NAMESPACE or SUBSCRIBE_NAMESPACE into the core (or, for a deferred
 * authorization, before it resolves), so moq_session_accept_* returns
 * WOULD_BLOCK with the request pending. Once the queue drains the peer must
 * see exactly one REQUEST_OK for it, and for a namespace subscription the
 * matching NAMESPACE only after that REQUEST_OK (the session would refuse
 * it before), with no session closed and nothing left held. When the request
 * is gone before the acceptance commits (injected at the retry, since a
 * peer cannot withdraw a request it has not had answered), the core
 * admission is undone: the announce is withdrawn, or the namespace
 * subscription removed together with the NAMESPACE already parked behind
 * its acceptance, with the connection kept. */
enum { AH_NAMESPACE, AH_NS_SUB };
enum { AH_INLINE, AH_RESUMED, AH_GONE };

static int
accept_held_case(moq_version_t version, int family, int mode)
{
    static const char *const fam[] = { "namespace", "ns_sub" };
    static const char *const md[] = { "inline", "resumed", "gone" };
    char label[64];
    (void)snprintf(label, sizeof(label), "%s %s %s", fam[family], md[mode],
                   version == MOQ_VERSION_DRAFT_18 ? "d18" : "d16");
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (mode == AH_RESUMED) {
        relay_bind_test_authorize = relay_bind_test_defer_hook;
        relay_bind_test_defer_action = family == AH_NAMESPACE
                             ? MOQR_AUTH_PUBLISH_NAMESPACE
                             : MOQR_AUTH_SUBSCRIBE_NAMESPACE;
        relay_bind_test_defer_last = 0;
    }
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    if (crc != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: accept_held(%s): rig\n", label);
        return 1;
    }
    /* A live announce the namespace subscription matches. */
    (void)relay_bind_test_fetch_pub_open(&rig);
    relay_bind_test_max_actions = 3; /* read by relay_bind_test_rig_connect: this connection only */
    conn_t *v = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_max_actions = 0;
    R_CHECK(&rig, v != NULL);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig.core, &st0);

    moq_bytes_t xns[1] = { B("x") };
    moq_bytes_t dns[1] = { B("demo") };
    moq_announcement_t xh = { 0 };
    moq_ns_sub_handle_t nsh = { 0 };
    bool sent = false;
    if (mode == AH_RESUMED) {
        /* Park the request first; it resolves under the full queue. */
        if (family == AH_NAMESPACE) {
            moq_publish_namespace_cfg_t pcfg;
            moq_publish_namespace_cfg_init(&pcfg);
            pcfg.track_namespace = (moq_namespace_t){ .parts = xns, .count = 1 };
            R_CHECK(&rig, moq_session_publish_namespace(v->peer, &pcfg, rig.now,
                                                        &xh) == MOQ_OK);
        } else {
            moq_subscribe_namespace_cfg_t nc;
            memset(&nc, 0, sizeof(nc));
            moq_subscribe_namespace_cfg_init(&nc);
            nc.track_namespace_prefix =
                (moq_namespace_t){ .parts = dns, .count = 1 };
            nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
            R_CHECK(&rig, moq_session_subscribe_namespace(v->peer, &nc, rig.now,
                                                          &nsh) == MOQ_OK);
        }
        sent = true;
        relay_bind_test_rig_pump(&rig, 4);
        R_CHECK(&rig, relay_bind_test_defer_last != 0);
        relay_bind_test_defer_action = 0xFFFFFFFFu; /* the filler announces are allowed */
    }
    uint64_t ticket = relay_bind_test_defer_last;
    /* Three announces, delivered without a bind pump. */
    static const moq_bytes_t AH = { (const uint8_t *)"h", 1 };
    const char *tails[3] = { "t0", "t1", "t2" };
    for (int t = 0; t < 3; t++) {
        moq_bytes_t parts[2] = { AH, { (const uint8_t *)tails[t], 2 } };
        moq_publish_namespace_cfg_t pcfg;
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(v->peer, &pcfg, rig.now,
                                                    &ann) == MOQ_OK);
    }
    relay_bind_test_rig_step_pair(&rig, v);
    if (!sent) {
        if (family == AH_NAMESPACE) {
            moq_publish_namespace_cfg_t pcfg;
            moq_publish_namespace_cfg_init(&pcfg);
            pcfg.track_namespace = (moq_namespace_t){ .parts = xns, .count = 1 };
            R_CHECK(&rig, moq_session_publish_namespace(v->peer, &pcfg, rig.now,
                                                        &xh) == MOQ_OK);
        } else {
            moq_subscribe_namespace_cfg_t nc;
            memset(&nc, 0, sizeof(nc));
            moq_subscribe_namespace_cfg_init(&nc);
            nc.track_namespace_prefix =
                (moq_namespace_t){ .parts = dns, .count = 1 };
            nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
            R_CHECK(&rig, moq_session_subscribe_namespace(v->peer, &nc, rig.now,
                                                          &nsh) == MOQ_OK);
        }
        relay_bind_test_rig_step_pair(&rig, v);
    }
    relay_bind_test_rig_bind_pump_once(&rig); /* filler acceptances fill the queue */
    if (mode == AH_RESUMED) {
        R_CHECK(&rig, moqr_bind_auth_resolve(
                          rig.bind, ticket,
                          &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                          rig.now) == MOQR_OK);
    }
    moqr_bind_fwd_debug_t dbg;
    moqr_bind_debug_fwd_stats(rig.bind, &dbg);
    R_CHECK(&rig, dbg.accepts_blocked == 1 && dbg.accepts_pending == 1 &&
                      dbg.refusals_unparked == 0);
    moqr_core_stats_t st1;
    moqr_core_get_stats(rig.core, &st1);
    if (family == AH_NAMESPACE) {
        R_CHECK(&rig, st1.ns_nodes == st0.ns_nodes + 1u + 3u + 1u);   /* x, h, h/t* */
    } else {
        R_CHECK(&rig, st1.ns_subs == st0.ns_subs + 1u);
    }

    if (mode == AH_GONE) {
        moqr_bind_debug_accept_gone(1);   /* the next acceptance retry */
    }
    relay_bind_test_rig_pump(&rig, 8);
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    moqr_bind_debug_fwd_stats(rig.bind, &dbg);
    moqr_core_stats_t st2;
    moqr_core_get_stats(rig.core, &st2);
    if (mode == AH_GONE) {
        R_CHECK(&rig, dbg.accepts_sent == 3u && dbg.accepts_undone == 1);
        if (family == AH_NAMESPACE) {
            R_CHECK(&rig, ps.namespace_accepts == 3);
            /* "x" withdrawn and its node reclaimable; h and h/t* stay. */
            R_CHECK(&rig, st2.ns_nodes - st2.ns_nodes_reclaimable ==
                              st0.ns_nodes + 1u + 3u);
        } else {
            /* The NAMESPACE for "demo" was parked behind the held acceptance
             * and is dropped with the subscription. */
            moqr_bind_stats_t bs;
            moqr_bind_get_stats(rig.bind, &bs);
            R_CHECK(&rig, bs.nsu_retained >= 1 && bs.nsu_live == 0 &&
                              bs.nsu_failed_closed == 0);
            R_CHECK(&rig, ps.ns_sub_oks == 0 && ps.ns_found == 0);
            R_CHECK(&rig, st2.ns_subs == st0.ns_subs);
        }
    } else if (family == AH_NAMESPACE) {
        R_CHECK(&rig, ps.namespace_accepts == 4 && ps.ns_rejected == 0);
        R_CHECK(&rig, dbg.accepts_sent == 4);
    } else {
        R_CHECK(&rig, ps.namespace_accepts == 3);
        R_CHECK(&rig, ps.ns_sub_oks == 1 && ps.ns_sub_error == 0);
        R_CHECK(&rig, ps.ns_found == 1);   /* "demo", after the REQUEST_OK */
        R_CHECK(&rig, dbg.accepts_sent == 4);
    }
    R_CHECK(&rig, dbg.accepts_pending == 0 && dbg.refusals_pending == 0);
    R_CHECK(&rig, !ps.session_closed &&
                      moq_session_state(v->rsess) == MOQ_SESS_ESTABLISHED);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    if (a.live != 0) {
        printf("FAIL: accept_held(%s): leak %ld\n", label, a.live);
        f++;
    }
    printf("%s: accept_held(%s)\n", f == 0 ? "PASS" : "FAIL", label);
    return f;
}

/* The relay session's own ref for the namespace-subscription request stream,
 * read from the SimPair trace: the bidi delivered to the relay while armed. A
 * test then drives the real RESET / STOP ingress on it through the public
 * session API. */
static struct {
    bool             armed;
    int              refs;   /* distinct refs seen while armed */
    moq_stream_ref_t ref;
} g_nsref;

static void
nsref_trace(void *ctx, const moq_sim_trace_record_t *r)
{
    (void)ctx;
    if (!g_nsref.armed || r->kind != MOQ_SIM_TRACE_INPUT ||
        r->to != MOQ_PERSPECTIVE_SERVER ||
        r->input_kind != MOQ_SIM_INPUT_BIDI_BYTES ||
        r->struct_size < offsetof(moq_sim_trace_record_t, stream_ref) +
                             sizeof(r->stream_ref)) {
        return;
    }
    if (g_nsref.refs == 0 || g_nsref.ref._v != r->stream_ref._v) {
        g_nsref.ref = r->stream_ref;
        g_nsref.refs++;
    }
}

/* The peer's stream ends, as the relay's session sees it. */
enum { NSE_CANCEL, NSE_RESET, NSE_STOP };

static void
ns_sub_end(rig_t *rig, conn_t *v, moq_ns_sub_handle_t nsh, int how)
{
    if (how == NSE_CANCEL) {
        R_CHECK(rig, moq_session_cancel_namespace_sub(v->peer, nsh,
                                                      rig->now) == MOQ_OK);
    } else if (how == NSE_RESET) {
        R_CHECK(rig, moq_session_on_bidi_stream_reset(v->rsess, g_nsref.ref,
                                                      0, rig->now) == MOQ_OK);
    } else {
        R_CHECK(rig, moq_session_on_bidi_stream_stop(v->rsess, g_nsref.ref,
                                                     0, rig->now) == MOQ_OK);
    }
}

/* An accepted namespace subscription ends while two NAMESPACE updates for it
 * are parked behind a full action queue, and the connection must stay open.
 *   cancel: the peer cancels (a FIN on its request stream). The session keeps
 *           the subscription until its response half closes, so the parked
 *           updates are still accepted by the session and discarded by the
 *           peer.
 *   reset / stop: the peer resets or stops the stream, and the session
 *           retires the subscription (real ingress). The updates are owed
 *           to nobody: the first send meets STALE_HANDLE and is dropped, the
 *           subscription leaves the core, and the second is purged, instead
 *           of the connection failing. */
static int
ns_sub_cancel_parked_case(moq_version_t version, int how)
{
    static const char *const hn[] = { "cancel", "reset", "stop" };
    char label[32];
    (void)snprintf(label, sizeof(label), "%s %s", hn[how],
                   version == MOQ_VERSION_DRAFT_18 ? "d18" : "d16");
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: ns_sub_cancel_parked(%s): rig\n", label);
        return 1;
    }
    moqr_binding_t pb = relay_bind_test_fetch_pub_open(&rig); /* announces "demo" */
    relay_bind_test_max_actions = 3;
    relay_bind_test_trace_fn = nsref_trace;
    memset(&g_nsref, 0, sizeof(g_nsref));
    conn_t *v = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_trace_fn = NULL;
    relay_bind_test_max_actions = 0;
    R_CHECK(&rig, v != NULL);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);

    moq_bytes_t dns[1] = { B("demo") };
    moq_subscribe_namespace_cfg_t nc;
    memset(&nc, 0, sizeof(nc));
    moq_subscribe_namespace_cfg_init(&nc);
    nc.track_namespace_prefix = (moq_namespace_t){ .parts = dns, .count = 1 };
    nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nsh;
    R_CHECK(&rig, moq_session_subscribe_namespace(v->peer, &nc, rig.now,
                                                  &nsh) == MOQ_OK);
    g_nsref.armed = true;
    relay_bind_test_rig_pump(&rig, 6);
    g_nsref.armed = false;
    R_CHECK(&rig, g_nsref.refs == 1 && g_nsref.ref._v != 0);
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.ns_sub_oks == 1 && ps.ns_found == 1);

    /* Fill the relay's queue for this connection, then announce a second
     * matching namespace: its NAMESPACE is parked. */
    static const moq_bytes_t CH = { (const uint8_t *)"h", 1 };
    const char *tails[3] = { "t0", "t1", "t2" };
    for (int t = 0; t < 3; t++) {
        moq_bytes_t parts[2] = { CH, { (const uint8_t *)tails[t], 2 } };
        moq_publish_namespace_cfg_t pcfg;
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(v->peer, &pcfg, rig.now,
                                                    &ann) == MOQ_OK);
    }
    relay_bind_test_rig_step_pair(&rig, v);
    relay_bind_test_rig_bind_pump_once(&rig); /* the three acceptances fill the queue */
    moq_bytes_t late[2] = { B("demo"), B("late") };
    moq_bytes_t late2[2] = { B("demo"), B("later") };
    R_CHECK(&rig, moqr_core_announce(rig.core, pb,
                                     (moqr_ns_t){ late, 2 }) == MOQR_OK);
    R_CHECK(&rig, moqr_core_announce(rig.core, pb,
                                     (moqr_ns_t){ late2, 2 }) == MOQR_OK);
    relay_bind_test_rig_bind_pump_once(&rig); /* both NAMESPACE updates park */
    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.nsu_live == 2);

    ns_sub_end(&rig, v, nsh, how);
    relay_bind_test_rig_step_pair(&rig, v);
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.nsu_live == 0 && bs.nsu_failed_closed == 0);
    R_CHECK(&rig, ps.namespace_accepts == 3 && ps.ns_found == 1);
    if (how != NSE_CANCEL) {
        moqr_core_stats_t cs;
        moqr_core_get_stats(rig.core, &cs);
        R_CHECK(&rig, cs.ns_subs == 0);
    }
    R_CHECK(&rig, !ps.session_closed &&
                      moq_session_state(v->rsess) == MOQ_SESS_ESTABLISHED);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: ns_sub_cancel_parked(%s): leak %ld\n", label, a.live);
        f++;
    }
    printf("%s: ns_sub_cancel_parked(%s)\n", f == 0 ? "PASS" : "FAIL", label);
    return f;
}

/* A namespace subscription the session has retired (real RESET ingress on
 * its request stream) leaves the core at the first update that finds it gone;
 * the session reports no event for the retirement, so a quiet prefix keeps
 * its core subscription until an update is sent or the connection closes.
 * Cycling subscribe -> reset -> update on one long-lived connection therefore
 * never exhausts a small core pool (max_ns_subs = 2, four cycles), and a
 * retired subscription is sent no later update. */
static int
ns_sub_retire_cycles_case(moq_version_t version)
{
    const char *label = version == MOQ_VERSION_DRAFT_18 ? "d18" : "d16";
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_ns_subs = 2;
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_ns_subs = 0;
    if (crc != MOQR_OK) {
        printf("FAIL: ns_sub_retire_cycles(%s): rig\n", label);
        return 1;
    }
    moqr_binding_t pb;
    R_CHECK(&rig, moqr_core_binding_open(rig.core, 900, &pb) == MOQR_OK);
    relay_bind_test_trace_fn = nsref_trace;
    memset(&g_nsref, 0, sizeof(g_nsref));
    conn_t *v = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_trace_fn = NULL;
    R_CHECK(&rig, v != NULL);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    static const char *const pfx[4] = { "p0", "p1", "p2", "p3" };
    for (int c = 0; c < 4; c++) {
        moq_bytes_t root[1] = { B(pfx[c]) };
        moq_bytes_t n1[2] = { B(pfx[c]), B("a") };
        moq_bytes_t n2[2] = { B(pfx[c]), B("b") };
        moq_bytes_t n3[2] = { B(pfx[c]), B("c") };
        R_CHECK(&rig, moqr_core_announce(rig.core, pb,
                                         (moqr_ns_t){ n1, 2 }) == MOQR_OK);
        moq_subscribe_namespace_cfg_t nc;
        memset(&nc, 0, sizeof(nc));
        moq_subscribe_namespace_cfg_init(&nc);
        nc.track_namespace_prefix =
            (moq_namespace_t){ .parts = root, .count = 1 };
        nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t nsh;
        g_nsref.refs = 0;
        g_nsref.armed = true;
        R_CHECK(&rig, moq_session_subscribe_namespace(v->peer, &nc, rig.now,
                                                      &nsh) == MOQ_OK);
        relay_bind_test_rig_pump(&rig, 6);
        g_nsref.armed = false;
        R_CHECK(&rig, g_nsref.refs == 1);
        memset(&ps, 0, sizeof(ps));
        relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
        R_CHECK(&rig, ps.ns_sub_oks == 1 && ps.ns_sub_error == 0);
        R_CHECK(&rig, ps.ns_found == 1);   /* p<c>/a */
        /* The peer resets the stream; the next update finds it retired. */
        ns_sub_end(&rig, v, nsh, NSE_RESET);
        R_CHECK(&rig, moqr_core_announce(rig.core, pb,
                                         (moqr_ns_t){ n2, 2 }) == MOQR_OK);
        relay_bind_test_rig_pump(&rig, 6);
        /* Later updates are not sent to it. */
        R_CHECK(&rig, moqr_core_announce(rig.core, pb,
                                         (moqr_ns_t){ n3, 2 }) == MOQR_OK);
        relay_bind_test_rig_pump(&rig, 6);
        memset(&ps, 0, sizeof(ps));
        relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
        R_CHECK(&rig, ps.ns_found == 0);
        moqr_core_stats_t cs;
        moqr_core_get_stats(rig.core, &cs);
        R_CHECK(&rig, cs.ns_subs == 0);
        R_CHECK(&rig, !ps.session_closed &&
                          moq_session_state(v->rsess) == MOQ_SESS_ESTABLISHED);
    }
    moqr_bind_stats_t bs;
    moqr_bind_get_stats(rig.bind, &bs);
    R_CHECK(&rig, bs.nsu_failed_closed == 0);

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: ns_sub_retire_cycles(%s): leak %ld\n", label, a.live);
        f++;
    }
    printf("%s: ns_sub_retire_cycles(%s)\n", f == 0 ? "PASS" : "FAIL", label);
    return f;
}

static int
test_accept_held(void)
{
    int f = 0;
    for (int family = AH_NAMESPACE; family <= AH_NS_SUB; family++) {
        for (int mode = AH_INLINE; mode <= AH_GONE; mode++) {
            f += accept_held_case(MOQ_VERSION_DRAFT_18, family, mode);
            f += accept_held_case(MOQ_VERSION_DRAFT_16, family, mode);
        }
    }
    for (int how = NSE_CANCEL; how <= NSE_STOP; how++) {
        f += ns_sub_cancel_parked_case(MOQ_VERSION_DRAFT_18, how);
        f += ns_sub_cancel_parked_case(MOQ_VERSION_DRAFT_16, how);
    }
    f += ns_sub_retire_cycles_case(MOQ_VERSION_DRAFT_18);
    f += ns_sub_retire_cycles_case(MOQ_VERSION_DRAFT_16);
    return f;
}

/* A connection's announce table full of live announces refuses the next
 * PUBLISH_NAMESPACE with the draft's capacity code and a Retry Interval
 * (d18 EXCESSIVE_LOAD, d16 INTERNAL_ERROR); the live announce is kept. */
static int
ann_table_capacity_case(moq_version_t version, uint64_t want_code,
                        const char *label)
{
    int f = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_announces = 1;
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_announces = 0;
    if (crc != MOQR_OK) {
        printf("FAIL: ann_table_capacity(%s): rig\n", label);
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t a1[1] = { B("one") };
    moq_publish_namespace_cfg_t pc;
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = a1, .count = 1 };
    moq_announcement_t h1, h2;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &h1) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.namespace_accepted && ps.ns_rejected == 0);
    moq_bytes_t a2[1] = { B("two") };
    pc.track_namespace = (moq_namespace_t){ .parts = a2, .count = 1 };
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &h2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.ns_rejected == 1);
    R_CHECK(&rig, ps.ns_rejected_code == want_code);
    R_CHECK(&rig, ps.ns_rejected_can_retry);
    R_CHECK(&rig, ps.ns_rejected_retry_ms >= 750 &&
                      ps.ns_rejected_retry_ms <= 1250);
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.ns_nodes == 2);   /* root + the live announce */

    f += rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: ann_table_capacity(%s): leak %ld\n", label, a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: ann_table_capacity(%s)\n", label);
    }
    return f;
}

static int
test_ann_table_capacity(void)
{
    int f = 0;
    f += ann_table_capacity_case(MOQ_VERSION_DRAFT_18,
                                 MOQ_REQUEST_ERROR_EXCESSIVE_LOAD, "d18");
    f += ann_table_capacity_case(MOQ_VERSION_DRAFT_16,
                                 MOQ_REQUEST_ERROR_INTERNAL_ERROR, "d16");
    return f;
}

/* A capacity refusal that meets a full session action queue is still
 * answered. The refused connection's relay-side queue holds three actions;
 * three PUBLISH_NAMESPACE acceptances fill it in the same pump that refuses
 * the request (or, for a deferred authorization, before it resolves), so
 * moq_session_reject_* returns WOULD_BLOCK with the request pending. Once
 * the queue drains the peer must see exactly one rejection carrying the
 * draft's capacity code (d18 EXCESSIVE_LOAD, d16 INTERNAL_ERROR) and a
 * nonzero Retry Interval, with the live track or fetch that filled the pool
 * untouched and nothing held or leaked afterwards. Kinds: SUBSCRIBE and
 * PUBLISH against a full track table, FETCH against a full fetch pool, a
 * SUBSCRIBE resumed after a deferred authorization, and the same resumed
 * SUBSCRIBE when an inline refusal has taken every unreserved entry of the
 * held-terminal table (one poll, one parkable authorization): one inline
 * refusal is held, two more requests wait in the session, and the
 * resolution's refusal still finds the entry reserved for it at park time. */
enum { CRH_SUB, CRH_PUB, CRH_FETCH, CRH_SUB_AUTH, CRH_SUB_AUTH_FULL };

static int
capacity_refusal_held_case(moq_version_t version, int kind)
{
    static const char *const kn[] = { "subscribe", "publish", "fetch",
                                      "subscribe_auth",
                                      "subscribe_auth_full_table" };
    bool auth = kind == CRH_SUB_AUTH || kind == CRH_SUB_AUTH_FULL;
    int want_errors = kind == CRH_SUB_AUTH_FULL ? 4 : 1;
    char label[64];
    (void)snprintf(label, sizeof(label), "%s %s", kn[kind],
                   version == MOQ_VERSION_DRAFT_18 ? "d18" : "d16");
    uint64_t want_code = version == MOQ_VERSION_DRAFT_18
                             ? MOQ_REQUEST_ERROR_EXCESSIVE_LOAD
                             : MOQ_REQUEST_ERROR_INTERNAL_ERROR;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (kind == CRH_FETCH) {
        relay_bind_test_max_fetches = 1;
    } else {
        relay_bind_test_max_tracks = 1;
    }
    if (auth) {
        relay_bind_test_authorize = relay_bind_test_defer_hook;
        relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
        relay_bind_test_defer_last = 0;
    }
    if (kind == CRH_SUB_AUTH_FULL) {
        relay_bind_test_max_parked = 1;
    }
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_fetches = 0;
    relay_bind_test_max_tracks = 0;
    relay_bind_test_max_parked = 0;
    if (crc != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: capacity_refusal_held(%s): rig\n", label);
        return 1;
    }
    /* The pool filler: a live pushed track (the only track slot), and for
     * FETCH a live retained fetch on it (the only fetch slot). */
    moqr_binding_t pb = relay_bind_test_fetch_pub_open(&rig);
    moqr_track_t live = relay_bind_test_fetch_pub_track(&rig, pb, "v1");
    relay_bind_test_fetch_ing(&rig, live, 0, 0, 0, 0x11);
    moqr_fetch_t held_fetch = MOQR_FETCH_INVALID;
    if (kind == CRH_FETCH) {
        moq_bytes_t nsb[1] = { B("demo") };
        moqr_fetch_req_t fr;
        moqr_fetch_req_init(&fr);
        fr.ns = (moqr_ns_t){ nsb, 1 };
        fr.name = B("v1");
        fr.end_group = 0;
        fr.end_object = 1;
        fr.cookie = 77;
        moqr_fetch_plan_t plan;
        R_CHECK(&rig, moqr_core_fetch_open(rig.core, pb, &fr, rig.now,
                                           &held_fetch, &plan) == MOQR_OK);
        R_CHECK(&rig, plan.admit != MOQR_FETCH_REJECT);
    }
    relay_bind_test_max_actions = 3; /* read by relay_bind_test_rig_connect: this connection only */
    conn_t *v = relay_bind_test_rig_connect(&rig, version);
    relay_bind_test_max_actions = 0;
    R_CHECK(&rig, v != NULL);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);

    moq_bytes_t dns[1] = { B("demo") };
    if (auth) {
        /* Park the request first; it resolves under the full queue. */
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = (moq_namespace_t){ .parts = dns, .count = 1 };
        sc.track_name = B("other");
        sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        moq_subscription_t sh;
        R_CHECK(&rig, moq_session_subscribe(v->peer, &sc, rig.now, &sh) ==
                          MOQ_OK);
        relay_bind_test_rig_pump(&rig, 4);
        R_CHECK(&rig, relay_bind_test_defer_last != 0);
        if (kind == CRH_SUB_AUTH_FULL) {
            relay_bind_test_defer_action = 0xFFFFFFFFu; /* the inline requests are allowed */
        }
    }
    uint64_t ticket = relay_bind_test_defer_last;
    /* Three announces, delivered without a bind pump. */
    static const moq_bytes_t CH = { (const uint8_t *)"h", 1 };
    const char *tails[3] = { "t0", "t1", "t2" };
    for (int t = 0; t < 3; t++) {
        moq_bytes_t parts[2] = { CH, { (const uint8_t *)tails[t], 2 } };
        moq_publish_namespace_cfg_t pcfg;
        moq_publish_namespace_cfg_init(&pcfg);
        pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
        moq_announcement_t ann;
        R_CHECK(&rig, moq_session_publish_namespace(v->peer, &pcfg, rig.now,
                                                    &ann) == MOQ_OK);
    }
    relay_bind_test_rig_step_pair(&rig, v);
    if (kind == CRH_SUB) {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = (moq_namespace_t){ .parts = dns, .count = 1 };
        sc.track_name = B("other");
        sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        moq_subscription_t sh;
        R_CHECK(&rig, moq_session_subscribe(v->peer, &sc, rig.now, &sh) ==
                          MOQ_OK);
    } else if (kind == CRH_PUB) {
        moq_bytes_t pns[1] = { B("pushns") };
        moq_publish_cfg_t pc;
        memset(&pc, 0, sizeof(pc));
        moq_publish_cfg_init(&pc);
        pc.track_namespace = (moq_namespace_t){ .parts = pns, .count = 1 };
        pc.track_name = B("pv");
        moq_publication_t ph;
        R_CHECK(&rig, moq_session_publish(v->peer, &pc, rig.now, &ph) ==
                          MOQ_OK);
    } else if (kind == CRH_FETCH) {
        moq_fetch_t fh;
        relay_bind_test_fetch_issue(&rig, v, "v1", 0, 0, 0, 1, &fh);
    } else if (kind == CRH_SUB_AUTH_FULL) {
        const char *names[3] = { "x0", "x1", "x2" };
        for (int k = 0; k < 3; k++) {
            moq_subscribe_cfg_t sc;
            moq_subscribe_cfg_init(&sc);
            sc.track_namespace = (moq_namespace_t){ .parts = dns, .count = 1 };
            sc.track_name = B(names[k]);
            sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
            moq_subscription_t sh;
            R_CHECK(&rig, moq_session_subscribe(v->peer, &sc, rig.now,
                                                &sh) == MOQ_OK);
        }
    }
    relay_bind_test_rig_step_pair(&rig, v);
    relay_bind_test_rig_bind_pump_once(&rig); /* acceptances fill the queue, then refusal */
    if (auth) {
        R_CHECK(&rig, moqr_bind_auth_resolve(
                          rig.bind, ticket,
                          &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                          rig.now) == MOQR_OK);
    }
    moqr_bind_fwd_debug_t st;
    moqr_bind_debug_fwd_stats(rig.bind, &st);
    uint64_t want_held = kind == CRH_SUB_AUTH_FULL ? 2u : 1u;
    R_CHECK(&rig, st.refusals_blocked == want_held &&
                      st.refusals_pending == want_held &&
                      st.refusals_unparked == 0);

    relay_bind_test_rig_pump(&rig, 8);
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, v, &ps, false, false, 0, 0);
    int errors = kind == CRH_PUB     ? ps.publish_errors
                 : kind == CRH_FETCH ? ps.fetch_error
                                     : ps.subscribe_errors;
    uint64_t code = kind == CRH_PUB     ? ps.publish_error_code
                    : kind == CRH_FETCH ? ps.fetch_error_code
                                        : ps.sub_error_code;
    bool can_retry = kind == CRH_PUB     ? ps.publish_error_can_retry
                     : kind == CRH_FETCH ? ps.fetch_error_can_retry
                                         : ps.sub_error_can_retry;
    uint64_t retry = kind == CRH_PUB     ? ps.publish_error_retry_ms
                     : kind == CRH_FETCH ? ps.fetch_error_retry_ms
                                         : ps.sub_error_retry_ms;
    if (errors != want_errors || code != want_code || !can_retry ||
        retry < 750 || retry > 1250) {
        printf("  capacity_refusal_held(%s): errors=%d code=%llu retry=%d/%llu\n",
               label, errors, (unsigned long long)code, (int)can_retry,
               (unsigned long long)retry);
    }
    R_CHECK(&rig, errors == want_errors);
    R_CHECK(&rig, code == want_code);
    R_CHECK(&rig, can_retry && retry >= 750 && retry <= 1250);
    moqr_bind_debug_fwd_stats(rig.bind, &st);
    R_CHECK(&rig, st.refusals_sent == (uint64_t)want_errors &&
                      st.refusals_pending == 0 && st.refusals_unparked == 0);
    R_CHECK(&rig, !ps.session_closed &&
                      moq_session_state(v->rsess) == MOQ_SESS_ESTABLISHED);

    /* The pool filler is untouched. */
    moqr_core_stats_t cs;
    moqr_core_get_stats(rig.core, &cs);
    R_CHECK(&rig, cs.tracks == 1);
    relay_bind_test_fetch_ing(&rig, live, 0, 0, 1, 0x12);
    if (kind == CRH_FETCH) {
        R_CHECK(&rig, moqr_core_fetch_close(rig.core, held_fetch) == MOQR_OK);
    }

    int f = rig.failures;
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    if (a.live != 0) {
        printf("FAIL: capacity_refusal_held(%s): leak %ld\n", label, a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: capacity_refusal_held(%s)\n", label);
    } else {
        printf("FAIL: capacity_refusal_held(%s)\n", label);
    }
    return f;
}

static int
test_capacity_refusal_held(void)
{
    int f = 0;
    for (int kind = CRH_SUB; kind <= CRH_SUB_AUTH_FULL; kind++) {
        f += capacity_refusal_held_case(MOQ_VERSION_DRAFT_18, kind);
        f += capacity_refusal_held_case(MOQ_VERSION_DRAFT_16, kind);
    }
    return f;
}

/* An announce retried after an intent drain uses the binding's own copy of
 * the namespace. The schedule: a publisher that also watches every namespace
 * withdraws "a" and announces "b" in one pump, into a full trie. The NS_GONE
 * for "a" (to that same session) still borrows a's node, so the announce of
 * "b" first meets WOULD_BLOCK; the drain that follows sends the NS_GONE on
 * that session, which may reset the session's event memory, and the retry is
 * admitted. Another watcher must then learn exactly "b". */
static int
test_announce_retry_reads_own_copy(void)
{
    int f = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    relay_bind_test_max_ns_nodes = 3; /* root + two namespaces */
    moqr_result_t crc = relay_bind_test_rig_create(&rig, &a);
    relay_bind_test_max_ns_nodes = 0;
    if (crc != MOQR_OK) {
        printf("FAIL: announce_retry_reads_own_copy: rig\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *other = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *watch = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_rig_pump(&rig, 4);
    moq_subscribe_namespace_cfg_t nsc;
    moq_subscribe_namespace_cfg_init(&nsc);
    nsc.track_namespace_prefix = (moq_namespace_t){ .parts = NULL, .count = 0 };
    nsc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t w1, w2;
    R_CHECK(&rig, moq_session_subscribe_namespace(pub->peer, &nsc, rig.now,
                                                  &w1) == MOQ_OK);
    R_CHECK(&rig, moq_session_subscribe_namespace(watch->peer, &nsc, rig.now,
                                                  &w2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t na[1] = { B("a") }, nq[1] = { B("q") }, nb[1] = { B("b") };
    moq_publish_namespace_cfg_t pc;
    moq_publish_namespace_cfg_init(&pc);
    moq_announcement_t ha, hq, hb;
    pc.track_namespace = (moq_namespace_t){ .parts = na, .count = 1 };
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &ha) == MOQ_OK);
    pc.track_namespace = (moq_namespace_t){ .parts = nq, .count = 1 };
    R_CHECK(&rig, moq_session_publish_namespace(other->peer, &pc, rig.now,
                                                &hq) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps, ws;
    memset(&ps, 0, sizeof(ps));
    memset(&ws, 0, sizeof(ws));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &ws, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, other, &ws, false, false, 0, 0);
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.ns_nodes == 3);   /* full */

    /* Withdraw a and announce b, delivered together, one bind pump. */
    R_CHECK(&rig, moq_session_publish_namespace_done(pub->peer, ha,
                                                     rig.now) == MOQ_OK);
    pc.track_namespace = (moq_namespace_t){ .parts = nb, .count = 1 };
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pc, rig.now,
                                                &hb) == MOQ_OK);
    relay_bind_test_rig_step_pair(&rig, pub);
    relay_bind_test_rig_bind_pump_once(&rig);
    relay_bind_test_rig_pump(&rig, 6);
    memset(&ps, 0, sizeof(ps));
    memset(&ws, 0, sizeof(ws));
    relay_bind_test_peer_drain(&rig, pub, &ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, watch, &ws, false, false, 0, 0);
    R_CHECK(&rig, ps.namespace_accepted && ps.ns_rejected == 0);
    /* The watcher saw a go and b arrive, with b's exact bytes. */
    R_CHECK(&rig, ws.ns_gone == 1);
    R_CHECK(&rig, ws.ns_found == 1);
    R_CHECK(&rig, ws.ns_seen_count == 1 && ws.ns_seen_total == 1 &&
                      ws.ns_seen_bytes[0] == 'b');
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.ns_nodes_reclaimed == 1);

    f += rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: announce_retry_reads_own_copy: leak %ld\n", a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: announce_retry_reads_own_copy\n");
    }
    return f;
}

/* Newest-wins across connections, inside one pump: a SUBSCRIBE queues an
 * upstream SUBSCRIBE to the incumbent publisher, then a second publisher's
 * PUBLISH_NAMESPACE for the same namespace force-withdraws the incumbent and
 * frees the pending track before the pump executes its intents. The queued
 * upstream request must not reach the incumbent (its track and name bytes are
 * gone); the subscriber gets an answer, and nothing reads freed memory. */
static int
test_newest_wins_drops_pending_upstream(void)
{
    int f = 0;
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: newest_wins_drops_pending_upstream: rig\n");
        return 1;
    }
    conn_t *p1 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *p2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[1] = { B("nw") };
    moq_publish_namespace_cfg_t pc;
    moq_publish_namespace_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 1 };
    moq_announcement_t h1, h2;
    R_CHECK(&rig, moq_session_publish_namespace(p1->peer, &pc, rig.now,
                                                &h1) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t p1s, p2s, ss;
    memset(&p1s, 0, sizeof(p1s));
    memset(&p2s, 0, sizeof(p2s));
    memset(&ss, 0, sizeof(ss));
    relay_bind_test_peer_drain(&rig, p1, &p1s, false, false, 0, 0);
    R_CHECK(&rig, p1s.namespace_accepted);

    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 1 };
    sc.track_name = B("t");
    sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    R_CHECK(&rig, moq_session_publish_namespace(p2->peer, &pc, rig.now,
                                                &h2) == MOQ_OK);
    relay_bind_test_rig_step_pair(&rig, sub);
    relay_bind_test_rig_step_pair(&rig, p2);
    relay_bind_test_rig_bind_pump_once(&rig); /* SUBSCRIBE, then the newest-wins announce */
    relay_bind_test_rig_pump(&rig, 6);
    memset(&p1s, 0, sizeof(p1s));
    relay_bind_test_peer_drain(&rig, p1, &p1s, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, p2, &p2s, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, sub, &ss, false, false, 0, 0);
    R_CHECK(&rig, p1s.up_subs == 0);         /* nothing reached the incumbent */
    R_CHECK(&rig, p2s.namespace_accepted);   /* the newcomer won */
    R_CHECK(&rig, ss.subscribe_errors == 1);

    f += rig.failures;
    relay_bind_test_rig_destroy(&rig);
    if (a.live != 0) {
        printf("FAIL: newest_wins_drops_pending_upstream: leak %ld\n", a.live);
        f++;
    }
    if (f == 0) {
        printf("PASS: newest_wins_drops_pending_upstream\n");
    }
    return f;
}

int
main(void)
{
    int failures = 0;
    failures += publish_namespace_newest_wins(MOQ_VERSION_DRAFT_16);
    failures += publish_namespace_newest_wins(MOQ_VERSION_DRAFT_18);
    failures += test_track_capacity_code();
    failures += test_ns_capacity_code();
    failures += test_capacity_refusal_held();
    failures += test_ann_table_capacity();
    failures += test_accept_held();
    failures += test_announce_retry_reads_own_copy();
    failures += test_newest_wins_drops_pending_upstream();
    failures += test_namespace_done_releases_route();

    failures += ns_found_survives_blocked_control(false);   /* control */
    failures += ns_found_survives_blocked_control(true);
    failures += nsu_case(MOQ_VERSION_DRAFT_18, false, false, 0);  /* control */
    failures += nsu_case(MOQ_VERSION_DRAFT_18, true, false, 0);
    failures += nsu_case(MOQ_VERSION_DRAFT_16, false, false, 0);  /* control */
    failures += nsu_case(MOQ_VERSION_DRAFT_16, true, false, 0);
    failures += nsu_gone_survives_block(MOQ_VERSION_DRAFT_18);
    failures += nsu_gone_survives_block(MOQ_VERSION_DRAFT_16);
    failures += nsu_zero_suffix_retained(MOQ_VERSION_DRAFT_18);
    failures += nsu_zero_suffix_retained(MOQ_VERSION_DRAFT_16);
    failures += nsu_purge_during_ring_retry();
    failures += nsu_cross_kind_order(true);
    failures += nsu_cross_kind_order(false);
    failures += nsu_byte_capacity_order(true);    /* matched control */
    failures += nsu_byte_capacity_order(false);
    failures += nsu_exhaustion_fail_closed();
    failures += nsu_ring_full_fail_closed(true);
    failures += nsu_ring_full_fail_closed(false);
    failures += nsu_ns_insert_keeps_scalar_full();
    failures += root_namespace_end_to_end(false);
    failures += root_namespace_end_to_end(true);
    failures += ann_store_rejects_empty_fields();
    failures += root_full_track_name_session_flow();
    failures += mixed_profile_root_discovery();
    failures += nsu_case(MOQ_VERSION_DRAFT_18, true, false, 6);  /* repeated */
    failures += nsu_multipart_bytes_exact();
    failures += nsu_close_purges_held();
    return failures == 0 ? 0 : 1; /* exit status truncates to 8 bits */
}
