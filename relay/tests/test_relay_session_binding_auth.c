/*
 * Authorization through the production relay binding over real sessions:
 * the token set reaches the hook without truncation, every auth point
 * fails closed on a non-ALLOW verdict, the static-toy verifier applies its
 * action×prefix policy, a DEFER verdict parks the request and resumes or
 * rejects it end to end (including stale and failed parks), and a
 * revalidation lease revokes, reserves, resumes and retires its grant —
 * with publisher cancels surviving action-queue backpressure.
 */
#include "relay_session_binding_harness.h"

/* -- token-mirror seam: the auth token set is never silently truncated ----- */

#define AUTH_MIRROR_IDX      8u        /* first index past the old cap of 8  */
#define AUTH_MIRROR_SENTINEL 0xA5A5u   /* deciding token_type at that index  */

typedef struct {
    int    calls;
    size_t last_count;
    bool   decider_seen;   /* token_type at AUTH_MIRROR_IDX == sentinel      */
    bool   corrupt;        /* any mirrored token diverged from fill pattern  */
} auth_probe_t;

/* Recording hook: verifies the whole mirror matches fill_tokens() and ALLOWs
 * only when the deciding token at AUTH_MIRROR_IDX survived intact. */
static void
auth_probe_hook(void *ctx, const moqr_auth_request_t *req,
                moqr_auth_verdict_t *out)
{
    auth_probe_t *p = (auth_probe_t *)ctx;
    p->calls++;
    p->last_count = req->token_count;
    p->corrupt = false;
    for (size_t i = 0; i < req->token_count; i++) {
        uint64_t want = (i == AUTH_MIRROR_IDX) ? (uint64_t)AUTH_MIRROR_SENTINEL
                                               : (uint64_t)(1000 + i);
        if (req->tokens[i].token_type != want ||
            req->tokens[i].token_value.len != 1) {
            p->corrupt = true;
        }
    }
    p->decider_seen =
        req->token_count > AUTH_MIRROR_IDX &&
        req->tokens[AUTH_MIRROR_IDX].token_type == (uint64_t)AUTH_MIRROR_SENTINEL;
    if (p->decider_seen && !p->corrupt) {
        out->decision = MOQR_AUTH_ALLOW;
        out->reason = MOQR_AUTH_REASON_OK;
    } else {
        out->decision = MOQR_AUTH_DENY;
        out->reason = MOQR_AUTH_REASON_UNSCOPED;
    }
}

/* Distinct-typed resolved tokens; the deciding token at AUTH_MIRROR_IDX carries
 * the sentinel type, so the hook can ALLOW only if the mirror kept that index. */
static void
fill_tokens(moq_resolved_token_t *t, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        t[i].token_type = (i == AUTH_MIRROR_IDX) ? (uint64_t)AUTH_MIRROR_SENTINEL
                                                 : (uint64_t)(1000 + i);
        t[i].token_value = B("v");
    }
}

static int
test_auth_token_mirror(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    auth_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    relay_bind_test_authorize = auth_probe_hook;
    relay_bind_test_authorize_ctx = &probe;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_authorize_ctx = NULL;
        printf("FAIL: token-mirror rig create\n");
        return 1;
    }

    moq_bytes_t nsp[1] = { B("ns") };
    moqr_ns_t ns = { nsp, 1 };
    moq_resolved_token_t toks[20];
    memset(toks, 0, sizeof(toks));
    moqr_auth_verdict_t v;
    bool consulted;

    /* (1) A deciding token past index 7 reaches the hook: 12 tokens, decider
     * at index 8. Truncating the set to 8 would drop it and force a spurious
     * DENY, so this guards against exactly that regression. */
    fill_tokens(toks, 12);
    memset(&probe, 0, sizeof(probe));
    consulted = moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE, ns, B("t"),
                                    toks, 12, 1, rig.now, &v);
    R_CHECK(&rig, consulted);
    R_CHECK(&rig, probe.calls == 1);
    R_CHECK(&rig, probe.last_count == 12);
    R_CHECK(&rig, probe.decider_seen);
    R_CHECK(&rig, !probe.corrupt);
    R_CHECK(&rig, v.decision == MOQR_AUTH_ALLOW);

    /* (2) Exactly the cap (16) is accepted and every token mirrored intact. */
    fill_tokens(toks, 16);
    memset(&probe, 0, sizeof(probe));
    consulted = moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE, ns, B("t"),
                                    toks, 16, 1, rig.now, &v);
    R_CHECK(&rig, consulted);
    R_CHECK(&rig, probe.last_count == 16);
    R_CHECK(&rig, !probe.corrupt);
    R_CHECK(&rig, v.decision == MOQR_AUTH_ALLOW);

    /* (3) One past the cap (17) fails closed WITHOUT consulting the hook. */
    fill_tokens(toks, 17);
    memset(&probe, 0, sizeof(probe));
    consulted = moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE, ns, B("t"),
                                    toks, 17, 1, rig.now, &v);
    R_CHECK(&rig, !consulted);
    R_CHECK(&rig, probe.calls == 0);            /* hook never saw a truncated set */
    R_CHECK(&rig, v.decision == MOQR_AUTH_DENY);

    /* (4) A positive count with a NULL array fails closed, hook untouched. */
    memset(&probe, 0, sizeof(probe));
    consulted = moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE, ns, B("t"),
                                    NULL, 5, 1, rig.now, &v);
    R_CHECK(&rig, !consulted);
    R_CHECK(&rig, probe.calls == 0);
    R_CHECK(&rig, v.decision == MOQR_AUTH_DENY);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_token_mirror\n");
    }
    return f;
}

/* -- deny/defer matrix: every auth point fails closed on non-ALLOW --------- */

/* Rules on one target action (DENY or DEFER), allows every other — so the
 * setup handshake succeeds and exactly the action under test is refused. */
typedef struct {
    moqr_auth_action_t   target;
    moqr_auth_decision_t decision;
    int                  calls;
} auth_gate_t;

static void
auth_gate_hook(void *ctx, const moqr_auth_request_t *req,
               moqr_auth_verdict_t *out)
{
    auth_gate_t *g = (auth_gate_t *)ctx;
    g->calls++;
    if (req->action == g->target) {
        out->decision = g->decision;
        out->reason = MOQR_AUTH_REASON_POLICY;
    } else {
        out->decision = MOQR_AUTH_ALLOW;
        out->reason = MOQR_AUTH_REASON_OK;
    }
}

/* Drive one denied/deferred request through the real wire and prove: the peer
 * sees the action's specific rejection carrying UNAUTHORIZED, no persistent
 * core state was created, and the decision was counted under the right verdict
 * (never ALLOW). DENY and DEFER share the fail-closed path, so both run here. */
static int
run_deny_case(moqr_auth_action_t action, moqr_auth_decision_t decision,
              const char *name)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    auth_gate_t gate;
    memset(&gate, 0, sizeof(gate));
    gate.target = action;
    gate.decision = decision;
    relay_bind_test_authorize = auth_gate_hook;
    relay_bind_test_authorize_ctx = &gate;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_authorize_ctx = NULL;
        printf("FAIL: %s rig create\n", name);
        return 1;
    }
    conn_t *cn = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, cn != NULL);
    relay_bind_test_rig_pump(&rig, 4); /* setup handshake — ALLOWED (target != CLIENT_SETUP) */

    /* Baseline after the (allowed) handshake, before the refused op. "Core not
     * mutated" == counters unchanged from here, which also catches a node/sub
     * leaked on the reject path. */
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig.core, &st0);

    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moq_namespace_t ns = { .parts = nsp, .count = 2 };

    switch (action) {
    case MOQR_AUTH_PUBLISH_NAMESPACE: {
        moq_publish_namespace_cfg_t c;
        memset(&c, 0, sizeof(c));
        moq_publish_namespace_cfg_init(&c);
        c.track_namespace = ns;
        moq_announcement_t h;
        R_CHECK(&rig, moq_session_publish_namespace(cn->peer, &c, rig.now,
                                                    &h) == MOQ_OK);
        break;
    }
    case MOQR_AUTH_SUBSCRIBE: {
        moq_subscribe_cfg_t c;
        memset(&c, 0, sizeof(c));
        moq_subscribe_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = B("v");
        c.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        moq_subscription_t h;
        R_CHECK(&rig, moq_session_subscribe(cn->peer, &c, rig.now, &h) ==
                          MOQ_OK);
        break;
    }
    case MOQR_AUTH_SUBSCRIBE_NAMESPACE: {
        moq_bytes_t pfx[1] = { B("live") };
        moq_subscribe_namespace_cfg_t c;
        memset(&c, 0, sizeof(c));
        moq_subscribe_namespace_cfg_init(&c);
        c.track_namespace_prefix = (moq_namespace_t){ .parts = pfx, .count = 1 };
        c.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t h;
        R_CHECK(&rig, moq_session_subscribe_namespace(cn->peer, &c, rig.now,
                                                      &h) == MOQ_OK);
        break;
    }
    case MOQR_AUTH_TRACK_STATUS: {
        moq_track_status_cfg_t c;
        memset(&c, 0, sizeof(c));
        moq_track_status_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = B("v");
        moq_track_status_handle_t h;
        R_CHECK(&rig, moq_session_track_status(cn->peer, &c, rig.now, &h) ==
                          MOQ_OK);
        break;
    }
    case MOQR_AUTH_PUBLISH: {
        moq_publish_cfg_t c;
        memset(&c, 0, sizeof(c));
        moq_publish_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = B("v");
        moq_publication_t h;
        R_CHECK(&rig, moq_session_publish(cn->peer, &c, rig.now, &h) ==
                          MOQ_OK);
        break;
    }
    default:
        R_CHECK(&rig, false); /* unreachable */
        break;
    }
    relay_bind_test_rig_pump(&rig, 8);

    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, cn, &ps, false, false, 0, 0);
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);

    switch (action) {
    case MOQR_AUTH_PUBLISH_NAMESPACE:
        R_CHECK(&rig, ps.ns_rejected == 1);
        R_CHECK(&rig, ps.ns_rejected_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
        R_CHECK(&rig, st.ns_nodes == st0.ns_nodes); /* nothing announced */
        break;
    case MOQR_AUTH_SUBSCRIBE:
        R_CHECK(&rig, ps.subscribe_errors == 1);
        R_CHECK(&rig, ps.sub_error_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
        R_CHECK(&rig, relay_bind_test_sub_reason_eq(&ps, "unauthorized"));
        R_CHECK(&rig, st.subs == st0.subs); /* no subscription created */
        break;
    case MOQR_AUTH_SUBSCRIBE_NAMESPACE:
        R_CHECK(&rig, ps.ns_sub_error == 1);
        R_CHECK(&rig, ps.ns_sub_error_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
        R_CHECK(&rig, st.ns_subs == st0.ns_subs); /* no namespace watcher */
        break;
    case MOQR_AUTH_TRACK_STATUS:
        R_CHECK(&rig, ps.ts_error == 1);
        R_CHECK(&rig, ps.ts_error_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
        break;
    case MOQR_AUTH_PUBLISH:
        R_CHECK(&rig, ps.publish_error);
        R_CHECK(&rig, ps.publish_error_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
        R_CHECK(&rig, st.tracks == st0.tracks); /* no publish track opened */
        break;
    default:
        break;
    }
    R_CHECK(&rig, st.auth_decisions[action][decision] >= 1);
    R_CHECK(&rig, st.auth_decisions[action][MOQR_AUTH_ALLOW] == 0);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_deny %s\n", name);
    }
    return f;
}

/* Setup DENY/DEFER: the relay hard-closes with the session UNAUTHORIZED code
 * (0x2, distinct from the request-level 0x1), the CLOSE reaches the peer, and
 * the relay core binding is torn down. */
static int
run_setup_deny_case(moqr_auth_decision_t decision, const char *name)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    auth_gate_t gate;
    memset(&gate, 0, sizeof(gate));
    gate.target = MOQR_AUTH_CLIENT_SETUP;
    gate.decision = decision;
    relay_bind_test_authorize = auth_gate_hook;
    relay_bind_test_authorize_ctx = &gate;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_authorize_ctx = NULL;
        printf("FAIL: %s rig create\n", name);
        return 1;
    }
    conn_t *cn = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, cn != NULL);
    relay_bind_test_rig_pump(&rig, 8); /* setup completes on the relay -> DENY -> close */

    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, cn, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.session_closed);       /* CLOSE_SESSION reached the peer */
    R_CHECK(&rig, ps.close_code == 0x2u);   /* session UNAUTHORIZED, not 0x1  */
    moqr_core_stats_t st;
    moqr_core_get_stats(rig.core, &st);
    R_CHECK(&rig, st.bindings == 0);        /* relay binding torn down        */
    R_CHECK(&rig,
            st.auth_decisions[MOQR_AUTH_CLIENT_SETUP][decision] >= 1);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_setup_deny %s\n", name);
    }
    return f;
}

static int
test_auth_deny_matrix(void)
{
    int f = 0;
    f += run_setup_deny_case(MOQR_AUTH_DENY, "setup/deny");
    f += run_setup_deny_case(MOQR_AUTH_DEFER, "setup/defer");
    f += run_deny_case(MOQR_AUTH_PUBLISH_NAMESPACE, MOQR_AUTH_DENY,
                       "announce/deny");
    f += run_deny_case(MOQR_AUTH_PUBLISH_NAMESPACE, MOQR_AUTH_DEFER,
                       "announce/defer");
    f += run_deny_case(MOQR_AUTH_SUBSCRIBE, MOQR_AUTH_DENY, "subscribe/deny");
    f += run_deny_case(MOQR_AUTH_SUBSCRIBE, MOQR_AUTH_DEFER, "subscribe/defer");
    f += run_deny_case(MOQR_AUTH_SUBSCRIBE_NAMESPACE, MOQR_AUTH_DENY,
                       "ns_sub/deny");
    f += run_deny_case(MOQR_AUTH_SUBSCRIBE_NAMESPACE, MOQR_AUTH_DEFER,
                       "ns_sub/defer");
    f += run_deny_case(MOQR_AUTH_TRACK_STATUS, MOQR_AUTH_DENY,
                       "track_status/deny");
    f += run_deny_case(MOQR_AUTH_TRACK_STATUS, MOQR_AUTH_DEFER,
                       "track_status/defer");
    f += run_deny_case(MOQR_AUTH_PUBLISH, MOQR_AUTH_DENY, "publish/deny");
    f += run_deny_case(MOQR_AUTH_PUBLISH, MOQR_AUTH_DEFER, "publish/defer");
    return f;
}

/* -- static-toy verifier: deterministic action×prefix policy --------------- */

static int
test_auth_toy_verifier(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);

    /* Policy: deny SUBSCRIBE under the "live" prefix, deny PUBLISH anywhere;
     * else the default (ALLOW). Rule order is significant — first match wins. */
    moq_bytes_t live[1] = { B("live") };
    moqr_auth_toy_rule_t rules[] = {
        { MOQR_AUTH_SUBSCRIBE, { live, 1 }, MOQR_AUTH_DENY,
          MOQR_AUTH_REASON_UNSCOPED },
        { MOQR_AUTH_PUBLISH, { NULL, 0 }, MOQR_AUTH_DENY,
          MOQR_AUTH_REASON_POLICY },
    };
    moqr_auth_toy_t toy = { rules, 2, MOQR_AUTH_ALLOW, MOQR_AUTH_REASON_OK };
    relay_bind_test_authorize = moqr_auth_toy_authorize;
    relay_bind_test_authorize_ctx = &toy;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_authorize_ctx = NULL;
        printf("FAIL: toy rig create\n");
        return 1;
    }

    moqr_auth_verdict_t v;
    moq_bytes_t livecam[2] = { B("live"), B("cam") };
    moq_bytes_t vod[1] = { B("vod") };

    /* SUBSCRIBE under live/ -> rule 0 -> DENY(UNSCOPED). */
    moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE,
                        (moqr_ns_t){ livecam, 2 }, B("t"), NULL, 0, 1, rig.now,
                        &v);
    R_CHECK(&rig, v.decision == MOQR_AUTH_DENY);
    R_CHECK(&rig, v.reason == MOQR_AUTH_REASON_UNSCOPED);

    /* SUBSCRIBE elsewhere -> no rule -> default ALLOW. */
    moqr_bind_auth_eval(rig.core, MOQR_AUTH_SUBSCRIBE, (moqr_ns_t){ vod, 1 },
                        B("t"), NULL, 0, 1, rig.now, &v);
    R_CHECK(&rig, v.decision == MOQR_AUTH_ALLOW);

    /* PUBLISH anywhere -> rule 1 (any prefix) -> DENY(POLICY). */
    moqr_bind_auth_eval(rig.core, MOQR_AUTH_PUBLISH, (moqr_ns_t){ vod, 1 },
                        B("t"), NULL, 0, 1, rig.now, &v);
    R_CHECK(&rig, v.decision == MOQR_AUTH_DENY);
    R_CHECK(&rig, v.reason == MOQR_AUTH_REASON_POLICY);

    /* Announce under live/ -> action mismatch on both rules -> default ALLOW.
     * (Same namespace as the denied SUBSCRIBE — proves the action is part of
     * the match, not just the prefix.) */
    moqr_bind_auth_eval(rig.core, MOQR_AUTH_PUBLISH_NAMESPACE,
                        (moqr_ns_t){ livecam, 2 }, (moq_bytes_t){ NULL, 0 },
                        NULL, 0, 1, rig.now, &v);
    R_CHECK(&rig, v.decision == MOQR_AUTH_ALLOW);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_toy_verifier\n");
    }
    return f;
}

/* The toy contract is ALLOW/DENY only: a rule or default mis-authored with
 * DEFER must fail closed as DENY, never surface DEFER. */
static int
test_auth_toy_clamps_defer(void)
{
    int failures = 0;
    moqr_auth_toy_rule_t rules[] = {
        { MOQR_AUTH_SUBSCRIBE, { NULL, 0 }, MOQR_AUTH_DEFER,
          MOQR_AUTH_REASON_OK },
    };
    moqr_auth_toy_t toy = { rules, 1, MOQR_AUTH_DEFER, MOQR_AUTH_REASON_OK };

    moqr_auth_request_t req;
    memset(&req, 0, sizeof(req));
    req.struct_size = (uint32_t)sizeof(req);
    moqr_auth_verdict_t v;

    /* Hits the DEFER rule -> clamped to DENY. */
    req.action = MOQR_AUTH_SUBSCRIBE;
    memset(&v, 0, sizeof(v));
    moqr_auth_toy_authorize(&toy, &req, &v);
    if (v.decision != MOQR_AUTH_DENY) {
        printf("FAIL: toy DEFER rule not clamped (decision=%u)\n", v.decision);
        failures++;
    }

    /* No rule -> DEFER default -> clamped to DENY. */
    req.action = MOQR_AUTH_PUBLISH;
    memset(&v, 0, sizeof(v));
    moqr_auth_toy_authorize(&toy, &req, &v);
    if (v.decision != MOQR_AUTH_DENY) {
        printf("FAIL: toy DEFER default not clamped (decision=%u)\n",
               v.decision);
        failures++;
    }

    if (failures == 0) {
        printf("PASS: auth_toy_clamps_defer\n");
    }
    return failures;
}

/* Drive a subscribe that the hook DEFERs; return the parked ticket. Asserts the
 * request is parked (not rejected) and left no core subscription. */
static uint64_t
defer_a_subscribe(rig_t *rig, conn_t *sub, moq_subscription_t *sh)
{
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig->core, &st0);
    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    R_CHECK(rig, moq_session_subscribe(sub->peer, &sc, rig->now, sh) == MOQ_OK);
    relay_bind_test_defer_last = 0;
    relay_bind_test_rig_pump(rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(rig, sub, &ps, false, false, 0, 0);
    R_CHECK(rig, ps.subscribe_errors == 0);   /* parked, not rejected */
    R_CHECK(rig, !ps.subscribe_ok);           /* not accepted yet     */
    moqr_core_stats_t st1;
    moqr_core_get_stats(rig->core, &st1);
    R_CHECK(rig, st1.subs == st0.subs);        /* no core sub on park  */
    return relay_bind_test_defer_last;
}

/* DEFER a subscribe, then resolve ALLOW -> it resumes and the peer is accepted;
 * a core subscription now exists. Also proves the request is NOT rejected and
 * creates NO core state while parked. */
static int
test_auth_defer_resume(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_authorize_ctx = NULL;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: defer rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    if (sub == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_18,
                                     (moqr_ns_t){ nsp, 2 }, B("video"))) {
        goto cleanup;
    }
    relay_bind_test_rig_pump(&rig, 2);
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig.core, &st0);

    moq_subscription_t sh;
    uint64_t tkt = defer_a_subscribe(&rig, sub, &sh);
    R_CHECK(&rig, tkt != 0);

    R_CHECK(&rig, moqr_bind_auth_resolve(
                      rig.bind, tkt,
                      &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                      rig.now) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 8);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.subscribe_ok);            /* resumed + accepted */
    moqr_core_stats_t st2;
    moqr_core_get_stats(rig.core, &st2);
    R_CHECK(&rig, st2.subs > st0.subs);        /* core sub created   */

cleanup:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_defer_resume\n");
    }
    return f;
}

/* A semantic object-end subscription remains parked until authorization.
 * DENY creates no subscription; ALLOW preserves the borrowed filter. */
static int
test_auth_defer_object_end(bool allow)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_authorize_ctx = NULL;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: defer filter rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_22);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    if (sub == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_22,
                                     (moqr_ns_t){ nsp, 2 }, B("video"))) {
        goto cleanup;
    }
    relay_bind_test_rig_pump(&rig, 2);
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig.core, &st0);

    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_OBJECT_END;
    loc.start_group = 0;
    loc.start_object = 0;
    loc.end_group = 1;
    loc.end_object = 0;
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.location = &loc;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &sc, rig.now, &sh) ==
                      MOQ_OK);
    memset(&loc, 0xA5, sizeof(loc));
    relay_bind_test_defer_last = 0;
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.subscribe_errors == 0 && !ps.subscribe_ok);   /* parked */
    uint64_t tkt = relay_bind_test_defer_last;
    R_CHECK(&rig, tkt != 0);

    moqr_auth_verdict_t v = { .decision = allow ? MOQR_AUTH_ALLOW
                                                : MOQR_AUTH_DENY,
                              .error_code = allow ? 0 : 0x20 };
    R_CHECK(&rig, moqr_bind_auth_resolve(rig.bind, tkt, &v, rig.now) ==
                      MOQR_OK);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    if (allow) {
        R_CHECK(&rig, ps.subscribe_ok && ps.subscribe_errors == 0);
    } else {
        R_CHECK(&rig, ps.subscribe_errors == 1 && !ps.subscribe_ok);
        R_CHECK(&rig, ps.sub_error_code == 0x20);
        R_CHECK(&rig, relay_bind_test_sub_reason_eq(&ps, "uninterested"));
    }
    R_CHECK(&rig, !ps.session_closed);
    moqr_core_stats_t st1;
    moqr_core_get_stats(rig.core, &st1);
    R_CHECK(&rig, st1.subs == st0.subs + (allow ? 1u : 0u));

cleanup:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_defer_object_end(%s)\n", allow ? "allow" : "deny");
    }
    return f;
}

/* DEFER a subscribe, then resolve DENY with a custom wire code -> the peer is
 * rejected carrying exactly that code. */
static int
test_auth_defer_reject(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_authorize_ctx = NULL;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: defer rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    if (sub == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_18,
                                     (moqr_ns_t){ nsp, 2 }, B("video"))) {
        goto cleanup;
    }
    relay_bind_test_rig_pump(&rig, 2);

    moq_subscription_t sh;
    uint64_t tkt = defer_a_subscribe(&rig, sub, &sh);
    R_CHECK(&rig, tkt != 0);

    R_CHECK(&rig, moqr_bind_auth_resolve(
                      rig.bind, tkt,
                      &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_DENY,
                                              .error_code = 0x20 },
                      rig.now) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, sub, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.subscribe_errors == 1);
    /* UNINTERESTED (0x20): a code draft-18 assigns, so the peer reads it
     * verbatim instead of applying Section 15's unknown-code rule. */
    R_CHECK(&rig, ps.sub_error_code == 0x20);   /* custom code preserved */
    R_CHECK(&rig, relay_bind_test_sub_reason_eq(&ps, "uninterested"));

cleanup:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_defer_reject\n");
    }
    return f;
}

/* Binding close before resolve retires the ticket; resolving it (or any
 * unknown ticket) is a stale no-op that touches no session. */
static int
test_auth_defer_stale(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_authorize_ctx = NULL;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: defer rig create\n");
        return 1;
    }
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, sub != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    if (sub == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_18,
                                     (moqr_ns_t){ nsp, 2 }, B("video"))) {
        goto cleanup;
    }
    relay_bind_test_rig_pump(&rig, 2);

    moq_subscription_t sh;
    uint64_t tkt = defer_a_subscribe(&rig, sub, &sh);
    R_CHECK(&rig, tkt != 0);

    /* Close the connection: its parked ticket is retired. */
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, sub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 2);

    R_CHECK(&rig, moqr_bind_auth_resolve(
                      rig.bind, tkt,
                      &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                      rig.now) == MOQR_ERR_STALE_HANDLE);
    R_CHECK(&rig, moqr_bind_auth_resolve(
                      rig.bind, 424242,
                      &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                      rig.now) == MOQR_ERR_STALE_HANDLE);

cleanup:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_defer_stale\n");
    }
    return f;
}

/* A park failure (here a duplicate live ticket) must fail closed with a
 * DEFINITE wire code, never 0: a valid DEFER leaves the site's err at 0. */
static int
test_auth_defer_park_fail(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    relay_bind_test_defer_fixed = 5000; /* every DEFER reuses one ticket -> 2nd park dups */
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_authorize_ctx = NULL;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        relay_bind_test_defer_fixed = 0;
        printf("FAIL: defer rig create\n");
        return 1;
    }
    conn_t *s1 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *s2 = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, s1 != NULL && s2 != NULL);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t nsp[2] = { B("live"), B("cam") };
    if (s1 == NULL || s2 == NULL ||
        !relay_bind_test_rig_publish(&rig, MOQ_VERSION_DRAFT_18,
                                     (moqr_ns_t){ nsp, 2 }, B("video"))) {
        goto cleanup;
    }
    relay_bind_test_rig_pump(&rig, 2);

    /* s1 parks ticket 5000. */
    moq_subscription_t sh1;
    (void)defer_a_subscribe(&rig, s1, &sh1);

    /* s2's DEFER reuses ticket 5000 -> park duplicate -> reject fail-closed. */
    moq_subscribe_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    sc.track_name = B("video");
    sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh2;
    R_CHECK(&rig, moq_session_subscribe(s2->peer, &sc, rig.now, &sh2) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t ps;
    memset(&ps, 0, sizeof(ps));
    relay_bind_test_peer_drain(&rig, s2, &ps, false, false, 0, 0);
    R_CHECK(&rig, ps.subscribe_errors == 1);
    R_CHECK(&rig, ps.sub_error_code == MOQ_REQUEST_ERROR_UNAUTHORIZED); /* not 0 */

cleanup:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_authorize_ctx = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    relay_bind_test_defer_fixed = 0;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: auth_defer_park_fail\n");
    }
    return f;
}

static int
test_auth_defer_matrix(void)
{
    int f = 0;
    f += relay_bind_test_run_defer_case(MOQR_AUTH_PUBLISH_NAMESPACE, MOQR_AUTH_ALLOW,
                                        "announce/allow");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_PUBLISH_NAMESPACE, MOQR_AUTH_DENY,
                                        "announce/deny");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_SUBSCRIBE_NAMESPACE, MOQR_AUTH_ALLOW,
                                        "ns_sub/allow");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_SUBSCRIBE_NAMESPACE, MOQR_AUTH_DENY,
                                        "ns_sub/deny");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_TRACK_STATUS, MOQR_AUTH_ALLOW,
                                        "track_status/allow");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_TRACK_STATUS, MOQR_AUTH_DENY,
                                        "track_status/deny");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_PUBLISH, MOQR_AUTH_ALLOW, "publish/allow");
    f += relay_bind_test_run_defer_case(MOQR_AUTH_PUBLISH, MOQR_AUTH_DENY, "publish/deny");
    return f;
}

/* Subscribe ALLOW + lease creates a grant; flipping the hook to DENY (custom
 * code) revokes it on the next due tick, and the subscriber sees a wire
 * SUBSCRIBE_DONE carrying that code. */
static int
test_reval_subscribe_revoke(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_rv_lease_sub = 3000; /* announce carries no grant: isolate the sub */
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        printf("FAIL: reval_subscribe rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, sub, &pub_ps, &sub_ps, nsp);
    R_CHECK(&rig, !sub_ps.done_seen);

    /* Flip to DENY (custom code) and let the lease elapse. */
    relay_bind_test_rv_decision = MOQR_AUTH_DENY;
    relay_bind_test_rv_deny_code = 0x7;
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.done_seen);        /* wire SUBSCRIBE_DONE */
    R_CHECK(&rig, sub_ps.done_code == 0x7); /* custom denial code preserved */

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_subscribe_revoke\n");
    }
    return f;
}

/* Announce ALLOW + lease creates a grant; flipping the hook to `decision`
 * revokes it on the next due tick, withdrawing the namespace: the publisher
 * sees a wire NAMESPACE_CANCELLED carrying `expect_code`. */
static int
announce_reval_case(moq_version_t version, moqr_auth_decision_t decision,
                    uint32_t deny_code, uint64_t expect_code, const char *name)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_rv_lease_ann = 3000;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        printf("FAIL: reval_announce %s rig create\n", name);
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub != NULL);
    peer_state_t pub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted); /* accepted: grant is live */
    R_CHECK(&rig, pub_ps.ns_cancelled == 0);

    /* Flip and let the lease elapse -> revalidation revokes -> withdraw. */
    relay_bind_test_rv_decision = decision;
    relay_bind_test_rv_deny_code = deny_code;
    relay_bind_test_rig_pump(&rig, 8);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.ns_cancelled == 1);
    R_CHECK(&rig, pub_ps.ns_cancelled_code == expect_code);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_announce_%s\n", name);
    }
    return f;
}

static int
test_reval_announce_revoke(void)
{
    int f = 0;
    /* draft-16 carries a REQUEST_ERROR code on PUBLISH_NAMESPACE_CANCEL, so the
     * relay's denial code reaches the publisher: custom DENY preserved, DEFER
     * defaults to UNAUTHORIZED. */
    f += announce_reval_case(MOQ_VERSION_DRAFT_16, MOQR_AUTH_DENY, 0x7u, 0x7u,
                             "d16_deny_custom");
    f += announce_reval_case(MOQ_VERSION_DRAFT_16, MOQR_AUTH_DEFER, 0u, 0x1u,
                             "d16_defer_default");
    /* draft-18 revokes by cancelling the request bidi (RESET+STOP), which
     * carries only the fixed §3.3.3 CANCELLED code, NOT a REQUEST_ERROR code:
     * the namespace is still withdrawn but the denial code is not conveyable on
     * the wire (protocol constraint, not a binding defect). */
    f += announce_reval_case(MOQ_VERSION_DRAFT_18, MOQR_AUTH_DENY, 0x7u, 0x0u,
                             "d18_deny_withdraw");
    return f;
}

/* A relay-topology force-withdraw (the shard manager's split-brain loser
 * enforcement) reaches a LIVE publisher as a wire NAMESPACE_CANCELLED. This is
 * the PENDING-CANCEL source, distinct from a revoked grant: no auth hook and
 * no lease exist here, the cancel is queued by moqr_core_force_withdraw and
 * rides the binding's peek/ack drain, targeting the announcement's real wire
 * handle via the session cookie the binding stored at announce. draft-16
 * carries the GOING_AWAY request-error code; draft-18 cancels the request
 * bidi, which carries only the fixed CANCELLED code (protocol constraint). */
static int
forcewithdraw_cancel_case(moq_version_t version, uint64_t expect_code,
                          const char *name)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        printf("FAIL: forcewithdraw_cancel %s rig create\n", name);
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    R_CHECK(&rig, pub != NULL);
    peer_state_t pub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);
    R_CHECK(&rig, pub_ps.ns_cancelled == 0);

    /* The manager-style topology withdrawal, straight against the core. */
    moqr_ns_t rns = { nsp, 2 };
    R_CHECK(&rig, moqr_core_force_withdraw(rig.core, rns, 0x6u, rig.now) ==
                      MOQR_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.ns_cancelled == 1);
    R_CHECK(&rig, pub_ps.ns_cancelled_code == expect_code);

    /* Withdrawn and acked: a repeat is a no-op and cancels nothing twice. */
    R_CHECK(&rig, moqr_core_force_withdraw(rig.core, rns, 0x6u, rig.now) ==
                      MOQR_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.ns_cancelled == 1);

    relay_bind_test_rig_destroy(&rig);
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: forcewithdraw_cancel_%s\n", name);
    }
    return f;
}

static int
test_forcewithdraw_publisher_cancel(void)
{
    int f = 0;
    f += forcewithdraw_cancel_case(MOQ_VERSION_DRAFT_16, 0x6u, "d16");
    f += forcewithdraw_cancel_case(MOQ_VERSION_DRAFT_18, 0x0u, "d18");
    return f;
}

/* grant_reserve failure (byte budget) fails the request closed BEFORE the core
 * op: no wire accept and no active core state, for both announce and subscribe. */
static int
test_reval_reserve_capacity(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_rv_lease_ann = 3000;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    relay_bind_test_grant_bytes = 1; /* any grant material overflows -> CAPACITY */
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        printf("FAIL: reval_capacity rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);
    moqr_core_stats_t st0;
    moqr_core_get_stats(rig.core, &st0);

    /* Announce: reserve fails -> wire NAMESPACE_REJECTED(UNAUTHORIZED), no ns. */
    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, !pub_ps.namespace_accepted);
    R_CHECK(&rig, pub_ps.ns_rejected == 1 &&
                      pub_ps.ns_rejected_code == 0x1u); /* UNAUTHORIZED */

    /* Subscribe: reserve fails -> wire SUBSCRIBE_ERROR(UNAUTHORIZED), and the
     * relay never subscribed upstream (no active core state). */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("video");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* would accept upstream */
    R_CHECK(&rig, !sub_ps.subscribe_ok);
    R_CHECK(&rig, sub_ps.subscribe_errors == 1 &&
                      sub_ps.sub_error_code == 0x1u); /* UNAUTHORIZED */
    R_CHECK(&rig, relay_bind_test_sub_reason_eq(&sub_ps, "unauthorized"));
    R_CHECK(&rig, pub_ps.up_subs == 0); /* relay never subscribed upstream */

    /* Neither failed request created active core state. */
    moqr_core_stats_t st1;
    moqr_core_get_stats(rig.core, &st1);
    R_CHECK(&rig, st1.ns_nodes == st0.ns_nodes && st1.subs == st0.subs);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_reserve_capacity\n");
    }
    return f;
}

/* Integration guard: closing a connection that owns a live grant, then ticking
 * past the lease with a now-DENY hook, runs the real conn_detach -> retire path
 * cleanly (no crash, no spurious wire teardown, balanced allocation). The
 * decisive "a retired grant is never revalidated" assertion lives at the core
 * level in test_relay_control's grant_retire_no_reval. */
static int
test_reval_conn_close_retires(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_rv_lease_sub = 3000;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        printf("FAIL: reval_conn_close rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    relay_bind_test_reval_make_subscribe_grant(&rig, pub, sub, &pub_ps, &sub_ps, nsp);

    /* Close the subscriber connection: conn_detach retires its grant. */
    R_CHECK(&rig, moqr_bind_conn_close(rig.bind, sub->rsess) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 4);

    moqr_bind_stats_t bs0;
    moqr_bind_get_stats(rig.bind, &bs0);
    /* Flip to DENY and advance well past the lease. With the grant retired the
     * tick must not revoke anything (no crash, no spurious wire teardown). */
    relay_bind_test_rv_decision = MOQR_AUTH_DENY;
    relay_bind_test_rv_deny_code = 0x7;
    relay_bind_test_rig_pump(&rig, 8);
    moqr_bind_stats_t bs1;
    moqr_bind_get_stats(rig.bind, &bs1);
    R_CHECK(&rig, bs1.session_errors == bs0.session_errors);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0); /* a non-retired grant would leak here */
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_conn_close_retires\n");
    }
    return f;
}

/* An async ALLOW that carries a lease (a DEFER resolved with
 * ALLOW + revalidate_after_us) must create a revalidation grant, exactly like a
 * synchronous ALLOW — otherwise an async-approved subscription is permanently
 * authorized. Proof over real sessions: DEFER a subscribe, resolve it ALLOW +
 * lease, and a later revalidation (the hook DEFERs again -> revoke) tears the
 * subscription down on the wire (SUBSCRIBE_DONE). */
static int
test_reval_resume_creates_grant(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE; /* DEFER subscribes, allow the rest */
    relay_bind_test_defer_fixed = 0;
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        relay_bind_test_defer_action = 0xFFFFFFFFu;
        printf("FAIL: reval_resume rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    conn_t *sub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_18);
    R_CHECK(&rig, pub && sub);
    peer_state_t pub_ps, sub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    memset(&sub_ps, 0, sizeof(sub_ps));
    relay_bind_test_rig_pump(&rig, 4);

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 4);

    /* Subscribe -> DEFER -> parked. */
    moq_subscribe_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    scfg.track_name = B("video");
    scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sh;
    R_CHECK(&rig, moq_session_subscribe(sub->peer, &scfg, rig.now, &sh) ==
                      MOQ_OK);
    relay_bind_test_defer_last = 0;
    relay_bind_test_rig_pump(&rig, 4);
    uint64_t tkt = relay_bind_test_defer_last;
    R_CHECK(&rig, tkt != 0); /* parked, not answered */

    /* Resolve ALLOW + lease: the resume path must reserve + commit a grant. The
     * lease is generous so the resume + upstream-accept round-trip completes
     * before the first revalidation is due. */
    R_CHECK(&rig, moqr_bind_auth_resolve(
                      rig.bind, tkt,
                      &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW,
                                              .revalidate_after_us = 20000 },
                      rig.now) == MOQR_OK);
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, true, false, 0, 0); /* accept upstream */
    relay_bind_test_rig_pump(&rig, 4);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.subscribe_ok); /* resumed subscribe */
    R_CHECK(&rig, !sub_ps.done_seen);

    /* Tick past the lease: revalidation re-invokes the hook (DEFER -> revoke).
     * Without a resume-path grant, nothing here would revoke. */
    relay_bind_test_rig_pump(&rig, 20);
    relay_bind_test_peer_drain(&rig, sub, &sub_ps, false, false, 0, 0);
    R_CHECK(&rig, sub_ps.done_seen); /* the async ALLOW was revalidatable */

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_defer_action = 0xFFFFFFFFu;
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_resume_creates_grant\n");
    }
    return f;
}

/* A publisher-side cancel that hits session backpressure
 * (cancel_namespace WOULD_BLOCK) must not be lost. The core grant is peeked, not
 * drained, so the cancel is retried each pump until it lands. Constrain the
 * action queue and keep it full with unrelated responses across the revoke
 * window: the cancel is withheld the whole time, then delivered once the queue
 * drains. Under the old drain-on-poll behavior the first blocked attempt would
 * lose the grant and the cancel would never arrive. */
static int
test_reval_cancel_backpressure(void)
{
    ca_t a;
    relay_bind_test_ca_init(&a);
    relay_bind_test_rv_lease_ann = 3000;
    relay_bind_test_authorize = relay_bind_test_rv_hook;
    relay_bind_test_max_actions = 1; /* one action slot: a filler blocks the cancel */
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &a) != MOQR_OK) {
        relay_bind_test_rv_reset();
        printf("FAIL: reval_backpressure rig create\n");
        return 1;
    }
    conn_t *pub = relay_bind_test_rig_connect(&rig, MOQ_VERSION_DRAFT_16); /* d16 carries code */
    R_CHECK(&rig, pub != NULL);
    peer_state_t pub_ps;
    memset(&pub_ps, 0, sizeof(pub_ps));
    relay_bind_test_rig_pump(&rig, 12); /* setup spreads over more pumps at max_actions 1 */

    moq_bytes_t nsp[2] = { B("live"), B("cam1") };
    moq_publish_namespace_cfg_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = nsp, .count = 2 };
    moq_announcement_t ann;
    R_CHECK(&rig, moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                                &ann) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 12);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.namespace_accepted);

    /* Flip to DENY, then keep the pub session's action queue full while the
     * lease elapses: each cycle the pub asks track_status (the relay answers ->
     * 1 queued action), so the revoke's cancel that follows in the same pump
     * WOULD_BLOCKs. */
    relay_bind_test_rv_decision = MOQR_AUTH_DENY;
    relay_bind_test_rv_deny_code = 0x7;
    for (int i = 0; i < 12; i++) {
        /* Filler: a throwaway announce the relay DENIES. The reject is a
         * synchronous session write in the same pump step, queued BEFORE the
         * revoke's cancel, so the one-slot action queue is full when the cancel
         * is attempted -> cancel_namespace WOULD_BLOCKs. (track_status answers
         * via an intent, which lands after the cancel, so it would not block.) */
        char jn[2] = { 'j', (char)('0' + i) };
        moq_bytes_t jns[2] = { B("junk"),
                               (moq_bytes_t){ (const uint8_t *)jn, 2 } };
        moq_publish_namespace_cfg_t jc;
        memset(&jc, 0, sizeof(jc));
        moq_publish_namespace_cfg_init(&jc);
        jc.track_namespace = (moq_namespace_t){ .parts = jns, .count = 2 };
        moq_announcement_t jh;
        (void)moq_session_publish_namespace(pub->peer, &jc, rig.now, &jh);
        relay_bind_test_rig_pump(&rig, 1);
    }
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.ns_cancelled == 0); /* cancel withheld under backpressure */

    /* Stop the fillers: the queue drains and the withheld cancel is delivered. */
    relay_bind_test_rig_pump(&rig, 12);
    relay_bind_test_peer_drain(&rig, pub, &pub_ps, false, false, 0, 0);
    R_CHECK(&rig, pub_ps.ns_cancelled == 1);
    R_CHECK(&rig, pub_ps.ns_cancelled_code == 0x7u);

    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_rv_reset();
    R_CHECK(&rig, a.live == 0);
    int f = rig.failures;
    if (f == 0) {
        printf("PASS: reval_cancel_backpressure\n");
    }
    return f;
}

int
main(void)
{
    int failures = 0;
    failures += test_auth_token_mirror();
    failures += test_auth_deny_matrix();
    failures += test_auth_toy_verifier();
    failures += test_auth_toy_clamps_defer();
    failures += test_auth_defer_resume();
    failures += test_auth_defer_reject();
    failures += test_auth_defer_object_end(false);
    failures += test_auth_defer_object_end(true);
    failures += test_auth_defer_stale();
    failures += test_auth_defer_park_fail();
    failures += test_auth_defer_matrix();
    failures += test_reval_subscribe_revoke();
    failures += test_reval_announce_revoke();
    failures += test_forcewithdraw_publisher_cancel();
    failures += test_reval_reserve_capacity();
    failures += test_reval_conn_close_retires();
    failures += test_reval_resume_creates_grant();
    failures += test_reval_cancel_backpressure();
    return failures == 0 ? 0 : 1; /* exit status truncates to 8 bits */
}
