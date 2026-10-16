/*
 * The subscriber's inbound data streams when a subscription ends on its side.
 *
 * "The subscriber SHOULD send STOP_SENDING on all streams related to the
 * subscription when it deletes subscription state" (draft-16 §9.15, draft-18
 * §10.11, draft-22 §9.9), and "Objects can arrive after a subscription has
 * been cancelled. Subscribers SHOULD retain sufficient state to quickly
 * discard these unwanted Objects" (draft-16 §10.1, draft-18 §11.1, draft-22
 * §3.1.3). A subscriber receiving a PUBLISH it does not want abandons
 * "reading any publisher initiated streams associated with that subscription
 * using a STOP_SENDING frame" (draft-16 §9.13, draft-18 §10.10, draft-22
 * §9.8).
 *
 * Pinned on every profile where the path exists:
 *   - a local cancel (unsubscribe, rejecting a PUBLISH) queues one STOP_DATA
 *     with CANCELLED (0x1) for each open inbound stream of the subscription,
 *     atomically with the cancel itself, and frees each stream's receive
 *     entry; the stream's identity is kept only so late bytes are consumed;
 *   - a stream whose FIN already arrived needs no STOP and is released;
 *   - the same holds when the peer tears the request stream down (18/22),
 *     including a FIN that fails the request on draft-22, whose own reset of
 *     the request stream is reserved with the stops;
 *   - the STOPs never hold the cancel back: a short action queue refuses
 *     only when the cancel's own actions do not fit (nothing changed); a
 *     STOP that does not fit is owed and queued once actions are polled,
 *     so a queue smaller than the number of open streams still cancels;
 *   - late bytes on a stopped stream surface no event, and the publisher's
 *     resets pair with the stops over the simulator, leaving no state behind.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_version_t g_version;

static moq_simpair_t *mk_pair(uint32_t max_actions)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.max_actions = max_actions;
    scfg.client_send_request_capacity = true;
    scfg.client_initial_request_capacity = 16;
    scfg.server_send_request_capacity = true;
    scfg.server_initial_request_capacity = 16;
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

/* Drains the queue, counting events of `kind` and of the data-stream kinds. */
static int count_events(moq_session_t *s, moq_event_kind_t kind, int *data_events)
{
    moq_event_t ev; int n = 0;
    *data_events = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == kind) n++;
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED || ev.kind == MOQ_EVENT_OBJECT_CHUNK ||
            ev.kind == MOQ_EVENT_SUBGROUP_FINISHED || ev.kind == MOQ_EVENT_SUBGROUP_RESET)
            (*data_events)++;
        moq_event_cleanup(&ev);
    }
    return n;
}

static moq_result_t write_one(moq_session_t *s, moq_subgroup_handle_t h, uint64_t obj)
{
    moq_rcbuf_t *pl = NULL;
    moq_result_t rc = moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
    if (rc < 0) return rc;
    rc = moq_session_write_object(s, h, obj, pl, 3);
    moq_rcbuf_decref(pl);
    return rc;
}

static size_t queued_kind(const moq_session_t *s, moq_action_kind_t kind)
{
    size_t n = 0;
    for (size_t i = s->action_head; i < s->action_tail; i++)
        if (s->actions[i % s->action_cap].kind == kind) n++;
    return n;
}

static size_t queued_total(const moq_session_t *s)
{
    return s->action_tail - s->action_head;
}

/* Every queued STOP_DATA carries CANCELLED and names one of `refs`. */
static int check_stops(const moq_session_t *s, const moq_stream_ref_t *refs, size_t n)
{
    int failures = 0;
    for (size_t i = s->action_head; i < s->action_tail; i++) {
        const moq_action_t *a = &s->actions[i % s->action_cap];
        if (a->kind != MOQ_ACTION_STOP_DATA) continue;
        MOQ_TEST_CHECK_EQ_U64(a->u.stop_data.error_code, 0x1);
        bool known = false;
        for (size_t k = 0; k < n; k++)
            if (refs[k]._v == a->u.stop_data.stream_ref._v) known = true;
        MOQ_TEST_CHECK(known);
    }
    return failures;
}

/* Receive entries bound to the subscription / publication; their refs. */
static size_t bound_rx(const moq_session_t *s, moq_subscription_t sub, moq_publication_t pub,
                       moq_stream_ref_t *refs, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < s->rx_cap; i++) {
        const moq_rx_stream_t *rx = &s->rx_streams[i];
        if (!rx->active) continue;
        bool m = (moq_subscription_is_valid(sub) && moq_subscription_is_valid(rx->sub) &&
                  moq_subscription_eq(rx->sub, sub)) ||
                 (moq_publication_is_valid(pub) && moq_publication_is_valid(rx->pub_handle) &&
                  moq_publication_eq(rx->pub_handle, pub));
        if (!m) continue;
        if (refs && n < cap) refs[n] = rx->stream_ref;
        n++;
    }
    return n;
}

static size_t active_rx(const moq_session_t *s)
{
    size_t n = 0;
    for (size_t i = 0; i < s->rx_cap; i++) if (s->rx_streams[i].active) n++;
    return n;
}

/* Leave exactly `avail` free action slots on `s` with harmless fillers. */
static void fill_actions(moq_session_t *s, size_t avail)
{
    while (action_queue_avail(s) > avail) {
        moq_action_t a;
        memset(&a, 0, sizeof(a));
        a.kind = MOQ_ACTION_SEND_CONTROL;
        a.detail_size = (uint32_t)sizeof(moq_send_control_action_t);
        if (push_action(s, &a) < 0) break;
    }
}

static void discard_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

/* Polls every action, which also lets a STOP owed for want of capacity be
 * queued; returns the STOP_DATA seen, each checked to be CANCELLED for one of
 * `refs`. */
static size_t poll_stops(moq_session_t *s, const moq_stream_ref_t *refs, size_t n,
                         int *failures_out)
{
    int failures = 0;
    size_t stops = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_STOP_DATA) {
            stops++;
            MOQ_TEST_CHECK_EQ_U64(a.u.stop_data.error_code, 0x1);
            bool known = false;
            for (size_t k = 0; k < n; k++)
                if (refs[k]._v == a.u.stop_data.stream_ref._v) known = true;
            MOQ_TEST_CHECK(known);
        }
        moq_action_cleanup(&a);
    }
    *failures_out += failures;
    return stops;
}

/* Entries still owing their STOP for want of action capacity. */
static size_t owed_stops(const moq_session_t *s)
{
    size_t n = 0;
    for (size_t i = 0; i < s->rx_cap; i++)
        if (s->rx_streams[i].active && s->rx_streams[i].parse_state == MOQ_RX_NEED_STOP)
            n++;
    return n;
}

/* Server publishes to the client's subscription: one subgroup finished with
 * a FIN and two left open, each with one object delivered. */
static int setup_subscription(moq_simpair_t *sp, moq_subscription_t *ch,
                              moq_subscription_t *sh, moq_subgroup_handle_t *open1,
                              moq_subgroup_handle_t *open2)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    *sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, *sh, &ac, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);

    moq_subgroup_handle_t done;
    moq_subgroup_cfg_t g; moq_subgroup_cfg_init(&g);
    g.group_id = 0; g.subgroup_id = 0; g.publisher_priority = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, *sh, &g, 3, &done), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, done, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(sv, done, 3), (int)MOQ_OK);
    g.group_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, *sh, &g, 3, open1), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, *open1, 0), (int)MOQ_OK);
    g.subgroup_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, *sh, &g, 3, open2), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, *open2, 0), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    int data = 0;
    MOQ_TEST_CHECK_EQ_INT(count_events(c, MOQ_EVENT_OBJECT_RECEIVED, &data), 3);
    drain(sv);
    /* The finished stream released its entry; the two open ones hold one each. */
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, *ch, MOQ_PUBLICATION_INVALID, NULL, 0), 2);
    return failures;
}

/* After the cancel: the publisher's late object on an open subgroup reaches a
 * stopped stream and surfaces nothing; the publisher's resets answer the
 * stops; no receive state is left and both sessions stay up. */
static int check_pairing(moq_simpair_t *sp, moq_subgroup_handle_t open1, bool late)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    if (late) MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, open1, 1), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    int data = 0;
    (void)count_events(c, MOQ_EVENT_OBJECT_RECEIVED, &data);
    MOQ_TEST_CHECK_EQ_INT(data, 0);
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 0);
    size_t live = 0;
    for (size_t i = 0; i < sv->sg_cap; i++)
        if (sv->subgroups[i].state == MOQ_SG_OPEN ||
            sv->subgroups[i].state == MOQ_SG_STREAMING) live++;
    MOQ_TEST_CHECK_EQ_SIZE(live, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

/* moq_session_unsubscribe with two open inbound subgroup streams. */
static int test_unsubscribe(bool tight, bool late)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(tight ? 16 : 0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    moq_subscription_t ch, sh;
    moq_subgroup_handle_t open1, open2;
    failures += setup_subscription(sp, &ch, &sh, &open1, &open2);
    moq_stream_ref_t refs[2];
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, refs, 2), 2);
    discard_actions(c);
    bool d16 = !moq_session_uses_request_streams(c);

    if (tight) {
        /* No room for the cancel itself: refused, nothing changed. */
        fill_actions(c, 0);
        size_t before = queued_total(c);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 5), (int)MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_SIZE(queued_total(c), before);
        MOQ_TEST_CHECK(sub_resolve_handle(c, ch) >= 0);
        MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, NULL, 0), 2);
        MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 0);
        discard_actions(c);
        /* Room for the cancel alone -- fewer free slots than open streams:
         * the cancel still succeeds, both STOPs are owed with the buffers
         * already released, and polling the actions issues them. */
        fill_actions(c, 1);
        before = queued_total(c);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 5), (int)MOQ_OK);
        MOQ_TEST_CHECK(sub_resolve_handle(c, ch) < 0);
        MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_STOP_DATA), 0);
        MOQ_TEST_CHECK_EQ_SIZE(queued_total(c), before + 1);
        if (!d16)
            MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_ABORT_BIDI_STREAM), 1);
        MOQ_TEST_CHECK_EQ_SIZE(owed_stops(c), 2);
        MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 0);
        MOQ_TEST_CHECK_EQ_SIZE(c->recv_input_bytes, (size_t)0);
        MOQ_TEST_CHECK_EQ_SIZE(c->recv_payload_bytes, (size_t)0);
        MOQ_TEST_CHECK_EQ_SIZE(poll_stops(c, refs, 2, &failures), 2);
        MOQ_TEST_CHECK_EQ_SIZE(owed_stops(c), 0);
        MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
        MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 2);
        moq_simpair_destroy(sp);
        return failures;
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK(sub_resolve_handle(c, ch) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_STOP_DATA), 2);
    failures += check_stops(c, refs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, d16 ? MOQ_ACTION_SEND_CONTROL
                                              : MOQ_ACTION_ABORT_BIDI_STREAM), 1);
    MOQ_TEST_CHECK_EQ_SIZE(queued_total(c), 3);
    /* The receive entries are free; only the identities are kept. */
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 2);
    MOQ_TEST_CHECK(moq_session_has_transport_stream(c, refs[0]));
    MOQ_TEST_CHECK(moq_session_has_transport_stream(c, refs[1]));
    failures += check_pairing(sp, open1, late);
    moq_simpair_destroy(sp);
    return failures;
}

/* A stream whose FIN is in but whose last object is still parked (the event
 * queue was full) needs no STOP: the cancel releases it without the event. */
static int test_unsubscribe_fin_already_in(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    moq_subscription_t ch, sh;
    moq_subgroup_handle_t open1, open2;
    failures += setup_subscription(sp, &ch, &sh, &open1, &open2);
    moq_stream_ref_t refs[2];
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, refs, 2), 2);
    /* Mark the first stream as having seen its FIN. */
    for (size_t i = 0; i < c->rx_cap; i++)
        if (c->rx_streams[i].active && c->rx_streams[i].stream_ref._v == refs[0]._v)
            c->rx_streams[i].pending_fin = true;
    discard_actions(c);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_STOP_DATA), 1);
    failures += check_stops(c, &refs[1], 1);
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 1);
    MOQ_TEST_CHECK(!moq_session_has_transport_stream(c, refs[0]));
    MOQ_TEST_CHECK(moq_session_has_transport_stream(c, refs[1]));
    int data = 0;
    (void)count_events(c, MOQ_EVENT_OBJECT_RECEIVED, &data);
    MOQ_TEST_CHECK_EQ_INT(data, 0);
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-18/22: the publisher tears the SUBSCRIBE request stream down. The
 * subscriber deletes the subscription (one UNSUBSCRIBED) and stops its open
 * inbound streams with it; a short queue refuses the whole teardown. */
static int test_peer_teardown(bool reset, bool tight)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(tight ? 16 : 0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    moq_subscription_t ch, sh;
    moq_subgroup_handle_t open1, open2;
    failures += setup_subscription(sp, &ch, &sh, &open1, &open2);
    moq_stream_ref_t refs[2];
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, refs, 2), 2);
    int sslot = sub_resolve_handle(c, ch);
    MOQ_TEST_CHECK(sslot >= 0);
    if (sslot < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t cref = c->subs[sslot].rs.ref;
    discard_actions(c);
    /* Tight: a full action queue; the teardown needs none of its own. */
    if (tight) fill_actions(c, 0);
    moq_result_t rc = reset ? moq_session_on_bidi_stream_reset(c, cref, 0x9, 5)
                            : moq_session_on_bidi_stream_stop(c, cref, 0x9, 5);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    MOQ_TEST_CHECK(sub_resolve_handle(c, ch) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_STOP_DATA), tight ? 0 : 2);
    failures += check_stops(c, refs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(owed_stops(c), tight ? 2 : 0);
    int data = 0;
    MOQ_TEST_CHECK_EQ_INT(count_events(c, MOQ_EVENT_UNSUBSCRIBED, &data), 1);
    MOQ_TEST_CHECK_EQ_INT(data, 0);
    MOQ_TEST_CHECK_EQ_SIZE(poll_stops(c, refs, 2, &failures), 2);
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
    MOQ_TEST_CHECK_EQ_SIZE(c->rx_stopped_count, 2);
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-22: the publisher closes the SUBSCRIBE request stream with a FIN and
 * no PUBLISH_DONE, which fails the subscription alone. The subscriber resets
 * its own direction of the request stream and stops the open data streams.
 * The reset is reserved with the teardown -- a full queue refuses it with
 * nothing changed -- and the STOPs never take its slot: with two free slots
 * one STOP is queued, the reset keeps the other, and the second STOP is owed
 * until the actions are polled. */
static int test_peer_fin_without_done(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(16);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    moq_subscription_t ch, sh;
    moq_subgroup_handle_t open1, open2;
    failures += setup_subscription(sp, &ch, &sh, &open1, &open2);
    moq_stream_ref_t refs[2];
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, refs, 2), 2);
    int sslot = sub_resolve_handle(c, ch);
    MOQ_TEST_CHECK(sslot >= 0);
    if (sslot < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t cref = c->subs[sslot].rs.ref;
    discard_actions(c);
    fill_actions(c, 0);
    size_t before = queued_total(c);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 5),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_SIZE(queued_total(c), before);
    MOQ_TEST_CHECK(sub_resolve_handle(c, ch) >= 0);
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(c, ch, MOQ_PUBLICATION_INVALID, NULL, 0), 2);
    discard_actions(c);
    fill_actions(c, 2);
    before = queued_total(c);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, NULL, 0, true, 5),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK(sub_resolve_handle(c, ch) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_STOP_DATA), 1);
    failures += check_stops(c, refs, 2);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_RESET_BIDI_STREAM), 1);
    MOQ_TEST_CHECK_EQ_SIZE(queued_total(c), before + 2);
    MOQ_TEST_CHECK_EQ_SIZE(owed_stops(c), 1);
    int data = 0;
    MOQ_TEST_CHECK_EQ_INT(count_events(c, MOQ_EVENT_UNSUBSCRIBED, &data), 1);
    MOQ_TEST_CHECK_EQ_INT(data, 0);
    MOQ_TEST_CHECK_EQ_SIZE(poll_stops(c, refs, 2, &failures), 2);
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(c), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Client publishes; the server is the subscriber. Objects flow on one open
 * subgroup before the server answers (accept = false) or after it accepted. */
static int setup_publication(moq_simpair_t *sp, bool accept, moq_publication_t *ph,
                             moq_publication_t *sph, moq_subgroup_handle_t *open1)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, ph), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev));
    *sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    if (accept) {
        moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, *sph, &ac, 2), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
    }
    moq_subgroup_cfg_t g; moq_subgroup_cfg_init(&g);
    g.group_id = 1; g.subgroup_id = 0; g.publisher_priority = 1;
    /* Objects may flow before PUBLISH_OK (draft-22 §9.8, draft-18 §10.10);
     * the publisher API opens subgroups only once established, so the
     * pending publication is presented as established for this open and its first object. */
    int cslot = pub_resolve_handle(c, *ph);
    MOQ_TEST_CHECK(cslot >= 0);
    if (cslot < 0) return failures;
    moq_pub_state_t was_state = c->publishes[cslot].state;
    bool was_allowed = c->publishes[cslot].send_allowed;
    c->publishes[cslot].state = MOQ_PUB_ESTABLISHED;
    c->publishes[cslot].send_allowed = true;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_pub_subgroup(c, *ph, &g, 3, open1), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(c, *open1, 0), (int)MOQ_OK);
    c->publishes[cslot].state = was_state;
    c->publishes[cslot].send_allowed = was_allowed;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c);
    int data = 0;
    MOQ_TEST_CHECK_EQ_INT(count_events(sv, MOQ_EVENT_OBJECT_RECEIVED, &data), 1);
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(sv, MOQ_SUBSCRIPTION_INVALID, *sph, NULL, 0), 1);
    return failures;
}

/* Rejecting a PUBLISH whose early objects already opened a stream stops it. */
static int test_reject_publish(bool tight)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(tight ? 16 : 0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    moq_subgroup_handle_t open1;
    failures += setup_publication(sp, false, &ph, &sph, &open1);
    moq_stream_ref_t ref;
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(sv, MOQ_SUBSCRIPTION_INVALID, sph, &ref, 1), 1);
    discard_actions(sv);
    moq_reject_publish_cfg_t rj; moq_reject_publish_cfg_init(&rj);
    rj.error_code = MOQ_REQUEST_ERROR_UNINTERESTED;
    if (tight) {
        /* No room for the response: refused, nothing changed. */
        fill_actions(sv, 0);
        size_t before = queued_total(sv);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_publish(sv, sph, &rj, 5),
                              (int)MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_SIZE(queued_total(sv), before);
        MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) >= 0);
        MOQ_TEST_CHECK_EQ_SIZE(bound_rx(sv, MOQ_SUBSCRIPTION_INVALID, sph, NULL, 0), 1);
        discard_actions(sv);
        /* Room for the response only: it succeeds and the STOP is owed. */
        fill_actions(sv, 1);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_publish(sv, sph, &rj, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(sv, MOQ_ACTION_STOP_DATA), tight ? 0 : 1);
    failures += check_stops(sv, &ref, 1);
    MOQ_TEST_CHECK_EQ_SIZE(owed_stops(sv), tight ? 1 : 0);
    if (tight) {
        MOQ_TEST_CHECK_EQ_SIZE(poll_stops(sv, &ref, 1, &failures), 1);
        MOQ_TEST_CHECK_EQ_SIZE(owed_stops(sv), 0);
    }
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(sv), 0);
    MOQ_TEST_CHECK_EQ_SIZE(sv->rx_stopped_count, 1);
    if (!tight) {
        /* The publisher learns of the refusal, ends its subgroup, and the
         * stopped stream's terminal clears the kept identity. */
        (void)write_one(c, open1, 1);
        moq_simpair_run_until_quiescent(sp, 32, NULL);
        int data = 0;
        (void)count_events(sv, MOQ_EVENT_OBJECT_RECEIVED, &data);
        MOQ_TEST_CHECK_EQ_INT(data, 0);
        MOQ_TEST_CHECK_EQ_SIZE(sv->rx_stopped_count, 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    }
    moq_simpair_destroy(sp);
    return failures;
}

/* Draft-18/22: the publisher tears an accepted PUBLISH's request stream down;
 * the subscriber surfaces PUBLISH_FINISHED and stops the open stream. */
static int test_publish_peer_teardown(bool reset, bool tight)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(tight ? 16 : 0);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *sv = moq_simpair_server(sp);
    moq_publication_t ph, sph;
    moq_subgroup_handle_t open1;
    failures += setup_publication(sp, true, &ph, &sph, &open1);
    moq_stream_ref_t ref;
    MOQ_TEST_CHECK_EQ_SIZE(bound_rx(sv, MOQ_SUBSCRIPTION_INVALID, sph, &ref, 1), 1);
    int pslot = pub_resolve_handle(sv, sph);
    MOQ_TEST_CHECK(pslot >= 0);
    if (pslot < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t sref = sv->publishes[pslot].rs.ref;
    discard_actions(sv);
    /* Tight: a full action queue; the teardown needs none of its own. */
    if (tight) fill_actions(sv, 0);
    moq_result_t rc = reset ? moq_session_on_bidi_stream_reset(sv, sref, 0x9, 5)
                            : moq_session_on_bidi_stream_stop(sv, sref, 0x9, 5);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    MOQ_TEST_CHECK(pub_resolve_handle(sv, sph) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(sv, MOQ_ACTION_STOP_DATA), tight ? 0 : 1);
    failures += check_stops(sv, &ref, 1);
    MOQ_TEST_CHECK_EQ_SIZE(owed_stops(sv), tight ? 1 : 0);
    int data = 0;
    MOQ_TEST_CHECK_EQ_INT(count_events(sv, MOQ_EVENT_PUBLISH_FINISHED, &data), 1);
    MOQ_TEST_CHECK_EQ_INT(data, 0);
    MOQ_TEST_CHECK_EQ_SIZE(poll_stops(sv, &ref, 1, &failures), 1);
    MOQ_TEST_CHECK_EQ_SIZE(active_rx(sv), 0);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        g_version = versions[v];
        failures += test_unsubscribe(false, false);
        failures += test_unsubscribe(false, true);
        failures += test_unsubscribe(true, false);
        failures += test_unsubscribe_fin_already_in();
        failures += test_reject_publish(false);
        failures += test_reject_publish(true);
        if (g_version == MOQ_VERSION_DRAFT_16) continue;
        if (g_version == MOQ_VERSION_DRAFT_22) failures += test_peer_fin_without_done();
        for (int r = 0; r < 2; r++) {
            failures += test_peer_teardown(r == 0, false);
            failures += test_peer_teardown(r == 0, true);
            failures += test_publish_peer_teardown(r == 0, false);
            failures += test_publish_peer_teardown(r == 0, true);
        }
    }
    if (failures == 0) MOQ_TEST_PASS("subscriber_cancel_streams");
    return failures != 0;
}
