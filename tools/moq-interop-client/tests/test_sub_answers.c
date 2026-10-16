/* Forwarded SUBSCRIBE answers at the interop client's publisher, over a
 * SimPair, in every draft. The relay side (client session) first subscribes
 * to another track in the published namespace (a catalog, as Red5 does),
 * then to the test track; on draft-22 also to the test track a second time.
 * The publisher side (server session) answers with sub_answers exactly as
 * the client's pump does: classify each SUBSCRIBE_REQUEST, send the queued
 * refusals, accept the test subscription. Pinned:
 *   - the other track gets exactly one SUBSCRIBE_ERROR, DOES_NOT_EXIST;
 *   - the test track gets exactly one SUBSCRIBE_OK, although its request
 *     arrived second;
 *   - on draft-22 the further test-track request gets exactly one
 *     SUBSCRIBE_ERROR, NOT_SUPPORTED, and the first stays accepted;
 *   - no request is left unanswered and the session never closes.
 * The publisher verdict the client's cases use (interop_publisher_verdict):
 *   - an accepted test SUBSCRIBE together with a request that could not be
 *     answered (refusal queue full, or the session refusing the refusal) is
 *     a failed step, not a pass;
 *   - a request that goes unanswered after the step passed (late, during
 *     object delivery) fails the delivery step;
 *   - a refusal the session cannot take yet (a full action queue,
 *     MOQ_ERR_WOULD_BLOCK) stays queued under the same handle and holds the
 *     step at WAIT; once the queue drains it is sent exactly once, the relay
 *     sees one DOES_NOT_EXIST, and later flushes send nothing more. */
#include "sub_answers.h"

#include <moq/moq.h>
#include <moq/sim.h>

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static moq_bytes_t k_ns[] = {
    { (const uint8_t *)"moq-test", 8 },
    { (const uint8_t *)"interop", 7 },
};
static const moq_bytes_t k_test = { (const uint8_t *)"test-track", 10 };
static const moq_bytes_t k_catalog = { (const uint8_t *)"catalog", 7 };

typedef struct {
    moq_subscription_t h;
    unsigned ok, err;
    moq_request_error_t code;
} relay_sub_t;

static moq_result_t subscribe(moq_session_t *c, moq_bytes_t name, uint64_t now,
                              moq_subscription_t *out)
{
    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ k_ns, 2 };
    scfg.track_name = name;
    scfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
    return moq_session_subscribe(c, &scfg, now, out);
}

/* The publisher's pump: classify, refuse, accept. */
static void publisher_cycle(moq_session_t *sv, interop_sub_answers_t *a,
                            bool *accepted, uint64_t now)
{
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            interop_sub_answers_on_request(a, sv, &ev.u.subscribe_request);
        CHECK(ev.kind != MOQ_EVENT_SESSION_CLOSED);
        moq_event_cleanup(&ev);
    }
    interop_sub_answers_flush(a, sv, now);
    if (a->have_test && !*accepted) {
        moq_accept_subscribe_cfg_t acfg;
        moq_accept_subscribe_cfg_init(&acfg);
        moq_result_t rc = moq_session_accept_subscribe(sv, a->test_sub, &acfg, now);
        CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        if (rc == MOQ_OK) *accepted = true;
    }
}

static void relay_drain(moq_session_t *c, relay_sub_t *subs, size_t n)
{
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) == 1) {
        moq_subscription_t h = MOQ_SUBSCRIPTION_INVALID;
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK) h = ev.u.subscribe_ok.sub;
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_ERROR) h = ev.u.subscribe_error.sub;
        CHECK(ev.kind != MOQ_EVENT_SESSION_CLOSED);
        for (size_t i = 0; i < n; i++) {
            if (!moq_subscription_eq(h, subs[i].h)) continue;
            if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK) subs[i].ok++;
            else { subs[i].err++; subs[i].code = ev.u.subscribe_error.error_code; }
        }
        moq_event_cleanup(&ev);
    }
}

static moq_simpair_t *make_pair(moq_version_t v, uint32_t max_actions)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = v;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x5EDu;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 64;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 64;
    cfg.max_actions = max_actions;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || moq_simpair_start(sp) != MOQ_OK) {
        if (sp) moq_simpair_destroy(sp);
        return NULL;
    }
    moq_simpair_run_until_quiescent(sp, 256, NULL);
    return sp;
}

/* Events the relay side does not track (requests the publisher sent it). */
static void drain_all(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1) {
        CHECK(ev.kind != MOQ_EVENT_SESSION_CLOSED);
        moq_event_cleanup(&ev);
    }
}

/* A refusal the session cannot take yet is kept, under the same handle, and
 * sent exactly once when the action queue drains. */
static void run_refusal_would_block(moq_version_t v, const char *label)
{
    int before = g_failures;
    moq_simpair_t *sp = make_pair(v, 8);
    if (sp == NULL) { fprintf(stderr, "FAIL %s: pair start\n", label); g_failures++; return; }
    moq_session_t *relay = moq_simpair_client(sp), *pub = moq_simpair_server(sp);
    uint64_t now = 1000;
    interop_sub_answers_t a;
    interop_sub_answers_init(&a, (moq_namespace_t){ k_ns, 2 }, k_test);
    relay_sub_t cat;
    memset(&cat, 0, sizeof(cat));
    CHECK(subscribe(relay, k_catalog, ++now, &cat.h) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 256, NULL);
    moq_event_t ev;
    size_t requests = 0;
    while (moq_session_poll_events(pub, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            interop_sub_answers_on_request(&a, pub, &ev.u.subscribe_request);
            requests++;
        }
        moq_event_cleanup(&ev);
    }
    CHECK(requests == 1 && a.n_pending == 1 && !a.have_test);
    moq_subscription_t held = a.pending[0];

    /* Fill the publisher's action queue without letting the pair drain it:
     * its own SUBSCRIBEs toward the relay, until the session refuses one. */
    static moq_bytes_t fill_ns[] = { { (const uint8_t *)"fill", 4 } };
    moq_result_t frc = MOQ_OK;
    for (int i = 0; i < 64 && frc == MOQ_OK; i++) {
        char name[8];
        int n = snprintf(name, sizeof(name), "f%d", i);
        moq_subscribe_cfg_t scfg;
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ fill_ns, 1 };
        scfg.track_name = (moq_bytes_t){ (const uint8_t *)name, (size_t)n };
        scfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        moq_subscription_t h;
        frc = moq_session_subscribe(pub, &scfg, ++now, &h);
    }
    CHECK(frc == MOQ_ERR_WOULD_BLOCK);

    interop_sub_answers_flush(&a, pub, ++now);
    CHECK(a.n_pending == 1 && moq_subscription_eq(a.pending[0], held));
    CHECK(a.refused_other == 0 && !a.unanswered);
    CHECK(interop_publisher_verdict(true, &a, NULL) == INTEROP_VERDICT_WAIT);

    /* Release the pressure, then flush: one refusal; later flushes: none. */
    for (int i = 0; i < 4; i++) {
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        interop_sub_answers_flush(&a, pub, ++now);
        drain_all(pub);
    }
    moq_simpair_run_until_quiescent(sp, 256, NULL);
    while (moq_session_poll_events(relay, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK && moq_subscription_eq(ev.u.subscribe_ok.sub, cat.h))
            cat.ok++;
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_ERROR && moq_subscription_eq(ev.u.subscribe_error.sub, cat.h)) {
            cat.err++;
            cat.code = ev.u.subscribe_error.error_code;
        }
        CHECK(ev.kind != MOQ_EVENT_SESSION_CLOSED);
        moq_event_cleanup(&ev);
    }
    CHECK(a.n_pending == 0 && a.refused_other == 1 && !a.unanswered);
    CHECK(cat.ok == 0 && cat.err == 1 && cat.code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    CHECK(interop_publisher_verdict(true, &a, NULL) == INTEROP_VERDICT_PASS);
    CHECK(moq_session_state(relay) == MOQ_SESS_ESTABLISHED);
    printf("%s: refusal held at WOULD_BLOCK, then SUBSCRIBE_ERROR x%u (code 0x%llx), "
           "refusals sent %u\n", label, cat.err, (unsigned long long)cat.code,
           a.refused_other);
    if (g_failures != before) fprintf(stderr, "FAIL %s (would-block)\n", label);
    moq_simpair_destroy(sp);
}

/* The test SUBSCRIBE accepted while another request goes unanswered, at the
 * same time or later during delivery: the publisher step fails. */
static void run_unanswered_verdicts(void)
{
    moq_subscribe_request_event_t req;
    const char *why;

    /* Accepted, plus a full refusal queue in the same pump. */
    interop_sub_answers_t a;
    interop_sub_answers_init(&a, (moq_namespace_t){ k_ns, 2 }, k_test);
    memset(&req, 0, sizeof(req));
    req.track_namespace = (moq_namespace_t){ k_ns, 2 };
    req.track_name = k_catalog;
    for (unsigned i = 0; i < INTEROP_SUB_ANSWERS_MAX; i++)
        CHECK(!interop_sub_answers_on_request(&a, NULL, &req));
    CHECK(!a.unanswered && a.n_pending == INTEROP_SUB_ANSWERS_MAX);
    req.track_name = k_test;
    CHECK(interop_sub_answers_on_request(&a, NULL, &req));
    req.track_name = k_catalog;
    CHECK(!interop_sub_answers_on_request(&a, NULL, &req));
    CHECK(a.unanswered && a.n_pending == INTEROP_SUB_ANSWERS_MAX);
    why = NULL;
    CHECK(interop_publisher_verdict(true, &a, &why) == INTEROP_VERDICT_FAIL);
    CHECK(why != NULL && strstr(why, "queue full") != NULL);
    printf("accepted + refusal queue full: %s\n", why ? why : "(pass)");

    /* Accepted, then a permanent refusal error (the session refuses the
     * refusal) in a real session: the step fails; it does not pass first. */
    moq_simpair_t *sp = make_pair(MOQ_VERSION_DRAFT_22, 0);
    CHECK(sp != NULL);
    if (sp == NULL) return;
    moq_session_t *pub = moq_simpair_server(sp);
    interop_sub_answers_init(&a, (moq_namespace_t){ k_ns, 2 }, k_test);
    memset(&req, 0, sizeof(req));
    req.sub = MOQ_SUBSCRIPTION_INVALID;           /* no such request here */
    req.track_namespace = (moq_namespace_t){ k_ns, 2 };
    req.track_name = k_catalog;
    CHECK(!interop_sub_answers_on_request(&a, pub, &req));
    CHECK(interop_publisher_verdict(true, &a, NULL) == INTEROP_VERDICT_WAIT);
    interop_sub_answers_flush(&a, pub, 5000);
    CHECK(a.unanswered && a.failed_rc != MOQ_OK && a.n_pending == 0);
    why = NULL;
    CHECK(interop_publisher_verdict(true, &a, &why) == INTEROP_VERDICT_FAIL);
    CHECK(why != NULL && strstr(why, "refusal failed") != NULL);
    printf("accepted + refusal error %d: %s\n", (int)a.failed_rc, why ? why : "(pass)");

    /* Late: the delivery step finished, but a request forwarded during it
     * went unanswered; the failure is sticky across later flushes. */
    interop_sub_answers_flush(&a, pub, 6000);
    why = NULL;
    CHECK(interop_publisher_verdict(true, &a, &why) == INTEROP_VERDICT_FAIL);
    CHECK(why != NULL);
    moq_simpair_destroy(sp);
}

static void run_version(moq_version_t v, const char *label)
{
    int before = g_failures;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = v;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x5EDu;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 16;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 16;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || moq_simpair_start(sp) != MOQ_OK) {
        fprintf(stderr, "FAIL %s: pair start\n", label);
        g_failures++;
        if (sp) moq_simpair_destroy(sp);
        return;
    }
    moq_session_t *relay = moq_simpair_client(sp), *pub = moq_simpair_server(sp);
    moq_simpair_run_until_quiescent(sp, 256, NULL);
    CHECK(moq_session_state(relay) == MOQ_SESS_ESTABLISHED);

    interop_sub_answers_t a;
    interop_sub_answers_init(&a, (moq_namespace_t){ k_ns, 2 }, k_test);
    bool accepted = false;
    bool concurrent = moq_session_supports_concurrent_subscriptions(relay);
    relay_sub_t subs[3];
    memset(subs, 0, sizeof(subs));
    size_t n = concurrent ? 3 : 2;
    uint64_t now = 1000;

    /* Red5's order: the catalog first, then the test track, each forwarded
     * (and seen by the publisher) before the next is sent. */
    CHECK(subscribe(relay, k_catalog, ++now, &subs[0].h) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 256, NULL);
    CHECK(subscribe(relay, k_test, ++now, &subs[1].h) == MOQ_OK);
    if (concurrent) CHECK(subscribe(relay, k_test, ++now, &subs[2].h) == MOQ_OK);
    for (int i = 0; i < 6; i++) {
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        publisher_cycle(pub, &a, &accepted, ++now);
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        relay_drain(relay, subs, n);
    }

    CHECK(subs[0].ok == 0 && subs[0].err == 1);
    CHECK(subs[0].code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    CHECK(subs[1].ok == 1 && subs[1].err == 0);
    CHECK(accepted);
    if (concurrent) {
        CHECK(subs[2].ok == 0 && subs[2].err == 1);
        CHECK(subs[2].code == MOQ_REQUEST_ERROR_NOT_SUPPORTED);
        CHECK(a.refused_duplicate == 1);
    }
    CHECK(a.refused_other == 1);
    CHECK(!a.unanswered && a.n_pending == 0);
    CHECK(moq_session_state(relay) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(pub) == MOQ_SESS_ESTABLISHED);
    for (size_t i = 0; i < n; i++)
        printf("%s: %-14s SUBSCRIBE_OK x%u SUBSCRIBE_ERROR x%u (code 0x%llx)\n",
               label, i == 0 ? "catalog" : i == 1 ? "test-track" : "test-track #2",
               subs[i].ok, subs[i].err, (unsigned long long)subs[i].code);
    if (g_failures != before) fprintf(stderr, "FAIL %s\n", label);
    moq_simpair_destroy(sp);
}

int main(void)
{
    CHECK(interop_full_track_name_eq((moq_namespace_t){ k_ns, 2 }, k_test,
                                     (moq_namespace_t){ k_ns, 2 }, k_test));
    CHECK(!interop_full_track_name_eq((moq_namespace_t){ k_ns, 2 }, k_catalog,
                                      (moq_namespace_t){ k_ns, 2 }, k_test));
    CHECK(!interop_full_track_name_eq((moq_namespace_t){ k_ns, 1 }, k_test,
                                      (moq_namespace_t){ k_ns, 2 }, k_test));

    run_version(MOQ_VERSION_DRAFT_16, "draft-16");
    run_version(MOQ_VERSION_DRAFT_18, "draft-18");
    run_version(MOQ_VERSION_DRAFT_22, "draft-22");
    run_refusal_would_block(MOQ_VERSION_DRAFT_16, "draft-16");
    run_refusal_would_block(MOQ_VERSION_DRAFT_18, "draft-18");
    run_refusal_would_block(MOQ_VERSION_DRAFT_22, "draft-22");
    run_unanswered_verdicts();

    if (g_failures) {
        fprintf(stderr, "test_sub_answers: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS: sub_answers\n");
    return 0;
}
