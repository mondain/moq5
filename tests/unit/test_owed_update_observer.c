/*
 * The owed-update observers as a parent-liveness observation, on both kinds
 * of publisher-role request (a subscription the peer made with SUBSCRIBE, and
 * one this side began with PUBLISH), over two real draft-22 sessions:
 *
 *   active, no answer owed      -> MOQ_ERR_WRONG_STATE
 *   active, an answer owed      -> MOQ_OK (the owed Request ID reported)
 *   ended by the session itself, terminal held behind owed resets
 *                               -> MOQ_ERR_STALE_HANDLE
 *   ended and freed             -> MOQ_ERR_STALE_HANDLE
 *   subscriber role             -> MOQ_ERR_WRONG_STATE
 *   wrong pool / foreign session-> MOQ_ERR_STALE_HANDLE
 *
 * Every observation, with or without outputs, leaves the session as it was:
 * no action, event, time or borrow-epoch change, and requested outputs
 * untouched on refusal. The held state is real: an update the session
 * refuses on its own (two identical authorization tokens) while its action
 * queue cannot take every reset of the request's open subgroups; it is
 * observed from inside the transport step that handles the update, before
 * the session's queued output is taken.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"

#include <string.h>

enum { SG = 4 };

typedef struct snap {
    size_t   actions, events;
    uint64_t now, epoch;
} snap_t;

static snap_t
snap_of(const moq_session_t *s)
{
    snap_t n = { s->action_tail - s->action_head, s->event_tail - s->event_head,
                 s->last_now_us, s->borrow_epoch };
    return n;
}

static bool
snap_eq(snap_t a, snap_t b)
{
    return a.actions == b.actions && a.events == b.events && a.now == b.now &&
           a.epoch == b.epoch;
}

/* Observe the request with no outputs and with outputs (sentinels kept on
 * refusal); both must agree and change nothing. Returns the result. */
static moq_result_t
observe(moq_session_t *s, bool publication, uint64_t raw, int *failures,
        uint64_t *owed_id)
{
    snap_t before = snap_of(s);
    moq_result_t a = publication
        ? moq_session_publication_owed_update_sized(
              s, (moq_publication_t){ ._opaque = raw }, NULL, NULL, NULL, 0)
        : moq_session_subscription_owed_update_sized(
              s, (moq_subscription_t){ ._opaque = raw }, NULL, NULL, NULL, 0);
    uint64_t id = 0xA5A5A5A5u;
    bool has = true;
    moq_location_filter_cfg_t f;
    memset(&f, 0x5A, sizeof(f));
    moq_location_filter_cfg_t f0 = f;
    moq_result_t b = publication
        ? moq_session_publication_owed_update_sized(
              s, (moq_publication_t){ ._opaque = raw }, &id, &has, &f, sizeof(f))
        : moq_session_subscription_owed_update_sized(
              s, (moq_subscription_t){ ._opaque = raw }, &id, &has, &f, sizeof(f));
    if (a != b) (*failures)++;
    if (!snap_eq(before, snap_of(s))) {
        printf("FAIL: observation changed the session\n");
        (*failures)++;
    }
    if (b != MOQ_OK && (id != 0xA5A5A5A5u || !has || memcmp(&f, &f0, sizeof(f)) != 0)) {
        printf("FAIL: outputs written on refusal\n");
        (*failures)++;
    }
    if (owed_id) *owed_id = id;
    return a;
}

typedef struct probe {
    moq_session_t *pub;
    bool publication;
    uint64_t raw;
    bool armed;
    bool held_seen;
    moq_result_t held_rc;
    int failures;
} probe_t;
static probe_t g_probe;

static void
probe_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    probe_t *p = ctx;
    if (!p->armed || p->pub == NULL || r->kind != MOQ_SIM_TRACE_INPUT ||
        r->to != MOQ_PERSPECTIVE_SERVER || p->held_seen)
        return;
    bool held = false;
    for (size_t i = 0; i < p->pub->sub_cap; i++)
        held |= p->pub->subs[i].state == MOQ_SUB_DONE_HELD;
    for (size_t i = 0; i < p->pub->pub_cap; i++)
        held |= p->pub->publishes[i].state == MOQ_PUB_DONE_HELD;
    if (!held) return;
    p->held_seen = true;
    p->held_rc = observe(p->pub, p->publication, p->raw, &p->failures, NULL);
}

static void
settle(moq_simpair_t *sp)
{
    size_t steps = 0;
    (void)moq_simpair_run_until_quiescent(sp, 512, &steps);
}

static int
run(bool publication)
{
    int failures = 0;
    memset(&g_probe, 0, sizeof(g_probe));
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x0B5u;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_actions = 3;   /* the server cannot take every reset at once */
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 64;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 64;
    cfg.server_defer_subscription_updates = true;
    cfg.trace_fn = probe_fn;
    cfg.trace_ctx = &g_probe;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    MOQ_TEST_CHECK(moq_simpair_start(sp) == MOQ_OK);
    moq_session_t *srv = moq_simpair_server(sp), *cli = moq_simpair_client(sp);
    settle(sp);
    moq_event_t ev;
    while (moq_session_poll_events(srv, &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(cli, &ev, 1) > 0) moq_event_cleanup(&ev);

    moq_bytes_t part = MOQ_BYTES_LITERAL("obs");
    moq_namespace_t ns = { &part, 1 };
    uint64_t raw = 0, cli_raw = 0;
    if (publication) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init_sized(&pc, sizeof(pc));
        pc.track_namespace = ns;
        pc.track_name = MOQ_BYTES_LITERAL("t");
        moq_publication_t ph;
        MOQ_TEST_CHECK(moq_session_publish(srv, &pc, 1, &ph) == MOQ_OK);
        raw = ph._opaque;
        settle(sp);
        while (moq_session_poll_events(cli, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) {
                cli_raw = ev.u.publish_request.pub._opaque;
                moq_accept_publish_cfg_t a;
                moq_accept_publish_cfg_init(&a);
                MOQ_TEST_CHECK(moq_session_accept_publish(
                                   cli, ev.u.publish_request.pub, &a, 1) == MOQ_OK);
            }
            moq_event_cleanup(&ev);
        }
    } else {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = ns;
        sc.track_name = MOQ_BYTES_LITERAL("t");
        moq_subscription_t ch;
        MOQ_TEST_CHECK(moq_session_subscribe(cli, &sc, 1, &ch) == MOQ_OK);
        cli_raw = ch._opaque;
        settle(sp);
        while (moq_session_poll_events(srv, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
                raw = ev.u.subscribe_request.sub._opaque;
                moq_accept_subscribe_cfg_t a;
                moq_accept_subscribe_cfg_init(&a);
                MOQ_TEST_CHECK(moq_session_accept_subscribe(
                                   srv, ev.u.subscribe_request.sub, &a, 1) == MOQ_OK);
            }
            moq_event_cleanup(&ev);
        }
    }
    settle(sp);
    while (moq_session_poll_events(srv, &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(cli, &ev, 1) > 0) moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(raw != 0 && cli_raw != 0);

    /* Active, nothing owed. */
    MOQ_TEST_CHECK_EQ_INT((int)observe(srv, publication, raw, &failures, NULL),
                          (int)MOQ_ERR_WRONG_STATE);
    /* Subscriber role (the peer's own handle on its own session). */
    MOQ_TEST_CHECK_EQ_INT((int)observe(cli, publication, cli_raw, &failures, NULL),
                          (int)MOQ_ERR_WRONG_STATE);
    /* Foreign session: this side's handle presented to the peer. */
    MOQ_TEST_CHECK_EQ_INT((int)observe(cli, publication, raw, &failures, NULL),
                          (int)MOQ_ERR_STALE_HANDLE);
    /* Wrong pool: the handle presented to the other kind's observer. */
    MOQ_TEST_CHECK_EQ_INT((int)observe(srv, !publication, raw, &failures, NULL),
                          (int)MOQ_ERR_STALE_HANDLE);

    /* An answer owed: the peer's ordinary update, deferred here. */
    if (publication) {
        moq_publication_update_cfg_t uc;
        moq_publication_update_cfg_init(&uc);
        uc.has_forward = true;
        uc.forward = true;
        MOQ_TEST_CHECK(moq_session_update_publication(
                           cli, (moq_publication_t){ ._opaque = cli_raw }, &uc, 2) ==
                       MOQ_OK);
    } else {
        moq_subscription_update_cfg_t uc;
        moq_subscription_update_cfg_init_sized(&uc, sizeof(uc));
        uc.has_forward = true;
        uc.forward = true;
        MOQ_TEST_CHECK(moq_session_update_subscription(
                           cli, (moq_subscription_t){ ._opaque = cli_raw }, &uc, 2) ==
                       MOQ_OK);
    }
    settle(sp);
    uint64_t owed = 0;
    MOQ_TEST_CHECK_EQ_INT((int)observe(srv, publication, raw, &failures, &owed),
                          (int)MOQ_OK);
    {
        moq_accept_subscription_update_cfg_t ac;
        moq_accept_subscription_update_cfg_init(&ac);
        ac.update_request_id = owed;
        moq_result_t arc = MOQ_ERR_WOULD_BLOCK;
        for (int k = 0; k < 8 && arc == MOQ_ERR_WOULD_BLOCK; k++) {
            arc = publication
                ? moq_session_accept_publication_update(
                      srv, (moq_publication_t){ ._opaque = raw }, &ac, 3)
                : moq_session_accept_subscription_update(
                      srv, (moq_subscription_t){ ._opaque = raw }, &ac, 3);
            if (arc == MOQ_ERR_WOULD_BLOCK) settle(sp);
        }
        MOQ_TEST_CHECK_EQ_INT((int)arc, (int)MOQ_OK);
    }
    settle(sp);
    while (moq_session_poll_events(srv, &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(cli, &ev, 1) > 0) moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)observe(srv, publication, raw, &failures, NULL),
                          (int)MOQ_ERR_WRONG_STATE);

    /* SG open subgroups, each with one object. */
    for (int g = 0; g < SG; g++) {
        moq_subgroup_cfg_t sgc;
        moq_subgroup_cfg_init(&sgc);
        sgc.group_id = (uint64_t)g;
        moq_subgroup_handle_t h;
        moq_result_t orc = publication
            ? moq_session_open_pub_subgroup(srv, (moq_publication_t){ ._opaque = raw },
                                            &sgc, 4, &h)
            : moq_session_open_subgroup(srv, (moq_subscription_t){ ._opaque = raw },
                                        &sgc, 4, &h);
        MOQ_TEST_CHECK(orc == MOQ_OK);
        uint8_t b[2] = { 1, 2 };
        moq_rcbuf_t *pl = NULL;
        MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), b, 2, &pl) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_write_object(srv, h, 0, pl, 4) == MOQ_OK);
        moq_rcbuf_decref(pl);
        settle(sp);
    }
    while (moq_session_poll_events(cli, &ev, 1) > 0) moq_event_cleanup(&ev);

    /* The refused update: two identical tokens. */
    moq_auth_token_t dup[2] = {
        { .token_type = 1, .token_value = MOQ_BYTES_LITERAL("same") },
        { .token_type = 1, .token_value = MOQ_BYTES_LITERAL("same") },
    };
    if (publication) {
        moq_publication_update_cfg_t uc;
        moq_publication_update_cfg_init(&uc);
        uc.auth_tokens = dup;
        uc.auth_token_count = 2;
        MOQ_TEST_CHECK(moq_session_update_publication(
                           cli, (moq_publication_t){ ._opaque = cli_raw }, &uc, 5) ==
                       MOQ_OK);
    } else {
        moq_subscription_update_cfg_t uc;
        moq_subscription_update_cfg_init_sized(&uc, sizeof(uc));
        uc.auth_tokens = dup;
        uc.auth_token_count = 2;
        MOQ_TEST_CHECK(moq_session_update_subscription(
                           cli, (moq_subscription_t){ ._opaque = cli_raw }, &uc, 5) ==
                       MOQ_OK);
    }
    g_probe.pub = srv;
    g_probe.publication = publication;
    g_probe.raw = raw;
    g_probe.armed = true;
    settle(sp);
    g_probe.armed = false;
    MOQ_TEST_CHECK(g_probe.held_seen);
    MOQ_TEST_CHECK_EQ_INT((int)g_probe.held_rc, (int)MOQ_ERR_STALE_HANDLE);
    failures += g_probe.failures;
    /* Drained and freed: still stale. */
    MOQ_TEST_CHECK_EQ_INT((int)observe(srv, publication, raw, &failures, NULL),
                          (int)MOQ_ERR_STALE_HANDLE);
    moq_simpair_destroy(sp);
    printf("%s: owed_update_observer(%s): held=%d\n", failures ? "FAIL" : "PASS",
           publication ? "publication" : "subscription", (int)g_probe.held_seen);
    return failures;
}

int
main(void)
{
    return (run(false) + run(true)) == 0 ? 0 : 1;
}
