/*
 * Terminal outcomes of requests ended by a peer stream signal. For every
 * request family carried on its own bidirectional stream, a peer RESET_STREAM
 * or STOP_SENDING surfaces exactly one event naming the cause and the peer's
 * error code using the family's terminal event. D18 TRACK_STATUS keeps its
 * legacy silent retirement; D22 uses the distinct typed terminal.
 * A publisher whose subscription (SUBSCRIBE- or PUBLISH-initiated) is torn
 * down this way resets every subgroup stream of it still open, with
 * CANCELLED, atomically with the terminal event.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

/* The profile under test; every stream-correlated profile is covered. */
static moq_version_t g_version;

static moq_simpair_t *mk_pair(void)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
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

/* Poll until an event of `kind` appears (copying it out) or the queue empties. */
static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    memset(out, 0, sizeof(*out));
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

#define FIRST_REF(s, pool, cap, out) do {                                   \
    (out) = (moq_stream_ref_t){ 0 };                                          \
    for (size_t i_ = 0; i_ < (s)->cap; i_++)                                  \
        if ((s)->pool[i_].rs.ref._v != 0) { (out) = (s)->pool[i_].rs.ref; break; } \
} while (0)

static moq_result_t signal(moq_session_t *s, moq_stream_ref_t ref, bool reset, uint64_t code)
{
    return reset ? moq_session_on_bidi_stream_reset(s, ref, code, 5)
                 : moq_session_on_bidi_stream_stop(s, ref, code, 5);
}

static int test_subscribe(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
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
    drain(c); drain(sv);
    /* Responder side: the subscriber's signal on the request stream. */
    moq_stream_ref_t ref; FIRST_REF(sv, subs, sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x77), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_UNSUBSCRIBED, &ev));
    MOQ_TEST_CHECK(ev.detail_size >= sizeof(moq_unsubscribed_event_t));
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.stream_error_code, 0x77);
    moq_event_cleanup(&ev);
    /* Requester side: the publisher's signal on the same stream. */
    moq_stream_ref_t cref; FIRST_REF(c, subs, sub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x78), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_UNSUBSCRIBED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.stream_error_code, 0x78);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_fetch(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_fetch_cfg_t cfg; moq_fetch_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.start_group = 0; cfg.start_object = 0; cfg.end_group = 10; cfg.end_object = 0;
    moq_fetch_t fh;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &cfg, 1, &fh), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_FETCH_REQUEST), 1);
    drain(c);
    moq_event_t ev;
    /* Publisher (responder): the fetcher's signal cancels the request. */
    moq_stream_ref_t ref; FIRST_REF(sv, fetches, fetch_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x79), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_CANCELLED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_cancelled.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_cancelled.stream_error_code, 0x79);
    moq_event_cleanup(&ev);
    /* Fetcher (requester): FETCH_RESET gains the cause beside its code. */
    moq_stream_ref_t cref; FIRST_REF(c, fetches, fetch_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x7a), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_RESET, &ev));
    MOQ_TEST_CHECK(ev.detail_size >= sizeof(moq_fetch_reset_event_t));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.error_code, 0x7a);
    MOQ_TEST_CHECK(ev.u.fetch_reset.stop_sending == !reset);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    moq_event_cleanup(&ev);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_track_status(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_track_status_cfg_t cfg; moq_track_status_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_track_status_handle_t th;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_track_status(c, &cfg, 1, &th), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_TRACK_STATUS_REQUEST, &ev));
    moq_track_status_handle_t sh = ev.u.track_status_request.handle;
    moq_event_cleanup(&ev);
    drain(c);
    /* D22 responder: typed status terminal, not a peer REQUEST_ERROR. */
    moq_stream_ref_t ref; FIRST_REF(sv, track_statuses, ts_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x7b), (int)MOQ_OK);
    if (g_version == MOQ_VERSION_DRAFT_22) {
        MOQ_TEST_CHECK(take(sv, MOQ_EVENT_TRACK_STATUS_TERMINATED, &ev));
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.local_role, MOQ_TRACK_STATUS_RESPONDER);
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.cause,
            reset ? MOQ_TRACK_STATUS_PEER_RESET_STREAM : MOQ_TRACK_STATUS_PEER_STOP_SENDING);
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.stream_error_code, 0x7b);
        MOQ_TEST_CHECK(moq_track_status_handle_eq(ev.u.track_status_terminated.handle, sh));
        moq_event_cleanup(&ev);
    } else {
        MOQ_TEST_CHECK_EQ_U64(moq_session_poll_events(sv, &ev, 1), 0);
    }
    /* The handle is stale after the event. */
    moq_accept_track_status_cfg_t ac; moq_accept_track_status_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_track_status(sv, sh, &ac, 6), (int)MOQ_ERR_STALE_HANDLE);
    /* Requester: pending request, peer signal before any response. */
    moq_stream_ref_t cref; FIRST_REF(c, track_statuses, ts_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x7c), (int)MOQ_OK);
    if (g_version == MOQ_VERSION_DRAFT_22) {
        MOQ_TEST_CHECK(take(c, MOQ_EVENT_TRACK_STATUS_TERMINATED, &ev));
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.local_role, MOQ_TRACK_STATUS_REQUESTER);
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.cause,
            reset ? MOQ_TRACK_STATUS_PEER_RESET_STREAM : MOQ_TRACK_STATUS_PEER_STOP_SENDING);
        MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.stream_error_code, 0x7c);
        MOQ_TEST_CHECK(moq_track_status_handle_eq(ev.u.track_status_terminated.handle, th));
        moq_event_cleanup(&ev);
        MOQ_TEST_CHECK(moq_session_stream_awaits_peer_terminal(c, cref) == !reset);
        if (!reset) {
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(
                c, cref, NULL, 0, true, 7), (int)MOQ_OK);
            MOQ_TEST_CHECK(!moq_session_stream_awaits_peer_terminal(c, cref));
            MOQ_TEST_CHECK_EQ_U64(moq_session_poll_events(c, &ev, 1), 0);
        }
    } else {
        MOQ_TEST_CHECK_EQ_U64(moq_session_poll_events(c, &ev, 1), 0);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_publish_namespace(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    moq_announcement_t ah;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish_namespace(c, &cfg, 1, &ah), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(sv); drain(c);
    moq_event_t ev;
    /* D22 observation is distinct; legacy DONE remains handle-only. */
    moq_stream_ref_t ref; FIRST_REF(sv, announcements, ann_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x7d), (int)MOQ_OK);
    if (g_version == MOQ_VERSION_DRAFT_22) {
        bool found = take(sv, MOQ_EVENT_NAMESPACE_TERMINATED, &ev);
        MOQ_TEST_CHECK(found);
        if (found) {
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.local_role, MOQ_NAMESPACE_ROLE_RESPONDER);
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.cause,
                reset ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING);
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.stream_error_code, 0x7d);
        }
    } else {
        MOQ_TEST_CHECK(take(sv, MOQ_EVENT_NAMESPACE_DONE, &ev));
        MOQ_TEST_CHECK_EQ_U64(ev.detail_size, sizeof(moq_namespace_done_event_t));
    }
    moq_event_cleanup(&ev);
    /* Legacy CANCELLED has no stream-cause/code tail. */
    moq_stream_ref_t cref; FIRST_REF(c, announcements, ann_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x7e), (int)MOQ_OK);
    if (g_version == MOQ_VERSION_DRAFT_22) {
        bool found = take(c, MOQ_EVENT_NAMESPACE_TERMINATED, &ev);
        MOQ_TEST_CHECK(found);
        if (found) {
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.local_role, MOQ_NAMESPACE_ROLE_REQUESTER);
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.cause,
                reset ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING);
            MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.stream_error_code, 0x7e);
            MOQ_TEST_CHECK(moq_announcement_eq(ev.u.namespace_terminated.ann, ah));
        }
    } else {
        MOQ_TEST_CHECK(take(c, MOQ_EVENT_NAMESPACE_CANCELLED, &ev));
        MOQ_TEST_CHECK_EQ_U64(ev.detail_size, sizeof(moq_namespace_cancelled_event_t));
        MOQ_TEST_CHECK(moq_announcement_eq(ev.u.namespace_cancelled.ann, ah));
    }
    moq_event_cleanup(&ev);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_subscribe_namespace(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_namespace_cfg_t cfg; moq_subscribe_namespace_cfg_init(&cfg);
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("pre") };
    cfg.track_namespace_prefix = (moq_namespace_t){ parts, 1 };
    moq_ns_sub_handle_t nh;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe_namespace(c, &cfg, 1, &nh), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_NS_SUB_REQUEST, &ev));
    moq_ns_sub_handle_t sh = ev.u.ns_sub_request.handle;
    moq_event_cleanup(&ev);
    drain(c);
    /* Publisher (responder), request still pending. */
    moq_stream_ref_t ref = { 0 };
    for (size_t i = 0; i < sv->ns_sub_cap; i++)
        if (sv->ns_subs[i].stream_ref._v != 0) { ref = sv->ns_subs[i].stream_ref; break; }
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x7f), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.local_role, MOQ_REQUEST_ROLE_RESPONDER);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.stream_error_code, 0x7f);
    MOQ_TEST_CHECK(moq_ns_sub_handle_eq(ev.u.ns_subscription_terminated.handle, sh));
    moq_event_cleanup(&ev);
    /* Subscriber (requester), no response yet and no announced suffixes. */
    moq_stream_ref_t cref = { 0 };
    for (size_t i = 0; i < c->ns_sub_cap; i++)
        if (c->ns_subs[i].stream_ref._v != 0) { cref = c->ns_subs[i].stream_ref; break; }
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x80), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.local_role, MOQ_REQUEST_ROLE_REQUESTER);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.stream_error_code, 0x80);
    MOQ_TEST_CHECK(moq_ns_sub_handle_eq(ev.u.ns_subscription_terminated.handle, nh));
    moq_event_cleanup(&ev);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_subscribe_tracks(bool reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_tracks_cfg_t cfg; moq_subscribe_tracks_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace_prefix = (moq_namespace_t){ parts, 1 };
    moq_track_sub_handle_t th;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe_tracks(c, &cfg, 1, &th), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST), 1);
    drain(c);
    moq_event_t ev;
    moq_stream_ref_t ref; FIRST_REF(sv, track_subs, track_sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x81), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_tracks_cancelled.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_tracks_cancelled.stream_error_code, 0x81);
    moq_event_cleanup(&ev);
    moq_stream_ref_t cref; FIRST_REF(c, track_subs, track_sub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x82), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_tracks_cancelled.stream_error_code, 0x82);
    moq_event_cleanup(&ev);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_publish(bool reset)
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
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, &ph), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_PUBLISH_REQUEST), 1);
    drain(c);
    moq_event_t ev;
    /* Subscriber (responder) with the PUBLISH pending: a stream signal is a
     * PUBLISH_FINISHED without a PUBLISH_DONE, distinguished by its cause. */
    moq_stream_ref_t ref; FIRST_REF(sv, publishes, pub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x83), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_FINISHED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_finished.status_code, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_finished.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_finished.stream_error_code, 0x83);
    moq_event_cleanup(&ev);
    /* Publisher (requester). */
    moq_stream_ref_t cref; FIRST_REF(c, publishes, pub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x84), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_PUBLISH_UNSUBSCRIBED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_unsubscribed.cause,
        reset ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING);
    MOQ_TEST_CHECK_EQ_U64(ev.u.publish_unsubscribed.stream_error_code, 0x84);
    moq_event_cleanup(&ev);
    moq_simpair_destroy(sp);
    return failures;
}

/* -- A torn-down subscription resets its open data streams --------------- */

static moq_simpair_t *mk_pair_actions(uint32_t max_actions)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.max_actions = max_actions;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    return sp;
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

static const moq_action_t *queued_first(const moq_session_t *s, moq_action_kind_t kind)
{
    for (size_t i = s->action_head; i < s->action_tail; i++)
        if (s->actions[i % s->action_cap].kind == kind)
            return &s->actions[i % s->action_cap];
    return NULL;
}

/* When the subscriber tears the SUBSCRIBE request stream down (RESET_STREAM
 * or STOP_SENDING), the publisher resets every subgroup stream of that
 * subscription it still has open, with CANCELLED (0x1); a subgroup already
 * closed with a FIN is left alone, and the reset streams take no more
 * objects. With room for the event but not for every reset, the teardown
 * still completes at once: the resets that fit are queued, the rest are owed
 * on their streams and go out as polling frees capacity -- every one of
 * them, exactly once. */
static int test_subscribe_teardown_resets_subgroups(bool reset, bool tight)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair_actions(tight ? 8 : 0);
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

    /* Two subgroups left open, one finished with a FIN. */
    moq_subgroup_handle_t open1, open2, done;
    moq_subgroup_cfg_t g; moq_subgroup_cfg_init(&g);
    g.group_id = 0; g.subgroup_id = 0; g.publisher_priority = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, sh, &g, 3, &done), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, done, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(sv, done, 3), (int)MOQ_OK);
    g.group_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, sh, &g, 3, &open1), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, open1, 0), (int)MOQ_OK);
    g.subgroup_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, sh, &g, 3, &open2), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(sv, open2, 0), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);

    moq_stream_ref_t ref; FIRST_REF(sv, subs, sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    if (tight) {
        /* One free action slot: one reset fits, the other is owed. */
        size_t obj = 1;
        while (action_queue_avail(sv) > 1 && obj < 64)
            if (write_one(sv, open1, obj++) != MOQ_OK) break;
        MOQ_TEST_CHECK_EQ_SIZE(action_queue_avail(sv), 1);
        MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x77), (int)MOQ_OK);
        MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
        MOQ_TEST_CHECK_EQ_SIZE(queued_kind(sv, MOQ_ACTION_RESET_DATA), 1);
        size_t owed = 0;
        for (size_t i = 0; i < sv->sg_cap; i++)
            if (sv->subgroups[i].state == MOQ_SG_RESET_OWED) owed++;
        MOQ_TEST_CHECK_EQ_SIZE(owed, 1);
        /* Polling frees capacity; the owed reset follows. */
        size_t resets = 0;
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1) > 0) {
            if (a.kind == MOQ_ACTION_RESET_DATA) {
                resets++;
                MOQ_TEST_CHECK_EQ_U64(a.u.reset_data.error_code, 0x1);
            }
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK_EQ_SIZE(resets, 2);
    } else {
        MOQ_TEST_CHECK_EQ_INT((int)signal(sv, ref, reset, 0x77), (int)MOQ_OK);
        MOQ_TEST_CHECK(sub_resolve_handle(sv, sh) < 0);
        MOQ_TEST_CHECK_EQ_SIZE(queued_kind(sv, MOQ_ACTION_RESET_DATA), 2);
        const moq_action_t *ra = queued_first(sv, MOQ_ACTION_RESET_DATA);
        if (ra) MOQ_TEST_CHECK_EQ_U64(ra->u.reset_data.error_code, 0x1);
    }
    size_t live = 0;
    for (size_t i = 0; i < sv->sg_cap; i++)
        if (sv->subgroups[i].state == MOQ_SG_OPEN ||
            sv->subgroups[i].state == MOQ_SG_STREAMING) live++;
    MOQ_TEST_CHECK_EQ_SIZE(live, 0);
    MOQ_TEST_CHECK(write_one(sv, open1, 99) != MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_UNSUBSCRIBED), 1);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* The same holds for a subscription PUBLISH began: the subscriber's
 * teardown of the PUBLISH request stream resets every subgroup stream of
 * the publication still open, with CANCELLED, and leaves a finished one
 * alone. */
static int test_publish_teardown_resets_subgroups(bool reset)
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
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, &ph), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev));
    moq_publication_t sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);

    moq_subgroup_handle_t open1, done;
    moq_subgroup_cfg_t g; moq_subgroup_cfg_init(&g);
    g.group_id = 0; g.subgroup_id = 0; g.publisher_priority = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_pub_subgroup(c, ph, &g, 3, &done), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(c, done, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(c, done, 3), (int)MOQ_OK);
    g.group_id = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_pub_subgroup(c, ph, &g, 3, &open1), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_one(c, open1, 0), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);

    moq_stream_ref_t cref; FIRST_REF(c, publishes, pub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)signal(c, cref, reset, 0x84), (int)MOQ_OK);
    MOQ_TEST_CHECK(pub_resolve_handle(c, ph) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(queued_kind(c, MOQ_ACTION_RESET_DATA), 1);
    const moq_action_t *ra = queued_first(c, MOQ_ACTION_RESET_DATA);
    if (ra) MOQ_TEST_CHECK_EQ_U64(ra->u.reset_data.error_code, 0x1);
    MOQ_TEST_CHECK(write_one(c, open1, 1) != MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_PUBLISH_UNSUBSCRIBED), 1);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++)
    for (int r = 0; r < 2; r++) {
        g_version = versions[v];
        bool reset = (r == 0);
        failures += test_subscribe(reset);
        failures += test_fetch(reset);
        failures += test_track_status(reset);
        failures += test_publish_namespace(reset);
        failures += test_subscribe_namespace(reset);
        failures += test_subscribe_tracks(reset);
        failures += test_publish(reset);
        failures += test_subscribe_teardown_resets_subgroups(reset, false);
        failures += test_subscribe_teardown_resets_subgroups(reset, true);
        failures += test_publish_teardown_resets_subgroups(reset);
    }
    /* Every appended event stays inside the detail union. */
    MOQ_TEST_CHECK(sizeof(moq_track_status_terminated_event_t) == 24);
    MOQ_TEST_CHECK(sizeof(moq_namespace_terminated_event_t) == 24);
    MOQ_TEST_CHECK(sizeof(moq_ns_subscription_terminated_event_t) == 24);
    MOQ_TEST_CHECK(sizeof(moq_publish_finished_event_t) <= MOQ_EVENT_DETAIL_MAX);
    MOQ_TEST_CHECK_EQ_SIZE(sizeof(moq_event_t), 144);
    if (failures == 0) MOQ_TEST_PASS("request_terminal");
    return failures != 0;
}
