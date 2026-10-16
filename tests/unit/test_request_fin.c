/*
 * A FIN on a request stream under a profile where FIN is not a cancellation.
 *
 * The requester's FIN only closes its sending direction: the responder keeps
 * the request and still answers it. The responder's FIN before its required
 * messages (the response, and PUBLISH_DONE for an established subscription's
 * publisher) fails that one request, never the session: the requester's
 * terminal event carries MISSING_RESPONSE, or TRUNCATED_RESPONSE when a
 * partial message was buffered, and its own direction is reset. A responder
 * FIN after its required messages completes the request with cause zero.
 *
 * The draft-18 profile keeps reading a requester FIN before the response as a
 * cancel and a responder FIN before the response as a protocol violation.
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

static moq_result_t fin(moq_session_t *s, moq_stream_ref_t ref)
{
    return moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, true, 5);
}

/* The failed requester resets its own sending direction (§6.4.2.3). */
static bool took_reset(moq_session_t *s, moq_stream_ref_t ref)
{
    moq_action_t a; bool found = false;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_BIDI_STREAM &&
            a.u.reset_bidi_stream.stream_ref._v == ref._v)
            found = true;
        moq_action_cleanup(&a);
    }
    return found;
}

/* On an unexpected close, name the reason the session queued. */
static void report_close(moq_session_t *s, const char *where)
{
    if (moq_session_state(s) != MOQ_SESS_CLOSED) return;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION)
            fprintf(stderr, "  %s: session closed: code=%llu reason=%.*s\n", where,
                    (unsigned long long)a.u.close_session.code,
                    (int)a.u.close_session.reason.len,
                    (const char *)a.u.close_session.reason.data);
        moq_action_cleanup(&a);
    }
}

static void subscribe_cfg(moq_subscribe_cfg_t *cfg, moq_bytes_t *parts)
{
    moq_subscribe_cfg_init(cfg);
    cfg->track_namespace = (moq_namespace_t){ parts, 1 };
    cfg->track_name = MOQ_BYTES_LITERAL("v");
}

/* Requester FIN before the response: the responder still answers. */
static int test_subscribe_requester_fin(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t cfg; subscribe_cfg(&cfg, parts);
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_stream_ref_t ref; FIRST_REF(sv, subs, sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(sv, ref), (int)MOQ_OK);
    report_close(sv, "subscribe requester fin");
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_UNSUBSCRIBED), 0);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 6), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_SUBSCRIBE_OK, &ev));
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Requester FIN on an established subscription: the publisher keeps serving. */
static int test_subscribe_established_requester_fin(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t cfg; subscribe_cfg(&cfg, parts);
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 6), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    moq_stream_ref_t ref; FIRST_REF(sv, subs, sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(sv, ref), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_UNSUBSCRIBED), 0);
    /* Objects still flow to the subscriber. */
    moq_subgroup_cfg_t sgc; moq_subgroup_cfg_init(&sgc);
    sgc.group_id = 1; sgc.subgroup_id = 0;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(sv, sh, &sgc, 7, &sg), (int)MOQ_OK);
    uint8_t d[] = { 0xAB };
    moq_rcbuf_t *pay = NULL;
    moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &pay);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(sv, sg, 0, pay, 7), (int)MOQ_OK);
    moq_rcbuf_decref(pay);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_OBJECT_RECEIVED), 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* Responder FIN before the response (bare, or after a partial message). */
static int test_subscribe_responder_fin(moq_version_t version, bool truncated)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t cfg; subscribe_cfg(&cfg, parts);
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    moq_stream_ref_t cref; FIRST_REF(c, subs, sub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    if (truncated) {
        /* One byte of a response envelope, then the FIN. */
        static const uint8_t partial[1] = { 0x04 };
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, cref, partial, 1, false, 5),
                              (int)MOQ_OK);
    }
    moq_result_t rc = fin(c, cref);
    if (version == MOQ_VERSION_DRAFT_18) {
        /* Still a protocol violation on this profile. */
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_CLOSED);
        moq_simpair_destroy(sp);
        return failures;
    }
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_UNSUBSCRIBED, &ev));
    MOQ_TEST_CHECK(moq_subscription_eq(ev.u.unsubscribed.sub, ch));
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.cause,
        truncated ? MOQ_REQUEST_TERMINAL_TRUNCATED_RESPONSE : MOQ_REQUEST_TERMINAL_MISSING_RESPONSE);
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.stream_error_code, 0);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(took_reset(c, cref));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_accept_subscribe_cfg_t ac; (void)ac;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 7), (int)MOQ_ERR_STALE_HANDLE);
    moq_simpair_destroy(sp);
    return failures;
}

/* Established subscription: the publisher's FIN without PUBLISH_DONE fails it. */
static int test_subscribe_established_fin_without_done(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t cfg; subscribe_cfg(&cfg, parts);
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    moq_subscription_t sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe(sv, sh, &ac, 6), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_SUBSCRIBE_OK, &ev));
    moq_event_cleanup(&ev);
    moq_stream_ref_t cref; FIRST_REF(c, subs, sub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_UNSUBSCRIBED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.unsubscribed.cause, MOQ_REQUEST_TERMINAL_MISSING_RESPONSE);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* PUBLISH: the subscriber's FIN after its response leaves the publication up;
 * before the response it fails the publication. */
static int test_publish_subscriber_fin(bool after_response)
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
    drain(c);
    if (after_response) {
        moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 6), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        MOQ_TEST_CHECK(take(c, MOQ_EVENT_PUBLISH_OK, &ev));
        moq_event_cleanup(&ev);
    }
    moq_stream_ref_t cref; FIRST_REF(c, publishes, pub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    if (after_response) {
        MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_PUBLISH_UNSUBSCRIBED), 0);
        MOQ_TEST_CHECK(c->publishes[0].state != MOQ_PUB_FREE);
        /* The publication still ends by message: PUBLISH_DONE with our FIN
         * goes out on the request bidi. (The FIN above was injected on this
         * side only, so the pair is not pumped past this point.) */
        moq_finish_publish_cfg_t fc; moq_finish_publish_cfg_init(&fc);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_finish_publish(c, ph, &fc, 8), (int)MOQ_OK);
        moq_action_t a; bool sent_done = false;
        while (moq_session_poll_actions(c, &a, 1) > 0) {
            if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
                a.u.send_bidi_stream.stream_ref._v == cref._v && a.u.send_bidi_stream.fin)
                sent_done = true;
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK(sent_done);
    } else {
        MOQ_TEST_CHECK(take(c, MOQ_EVENT_PUBLISH_UNSUBSCRIBED, &ev));
        MOQ_TEST_CHECK_EQ_U64(ev.u.publish_unsubscribed.cause, MOQ_REQUEST_TERMINAL_MISSING_RESPONSE);
        moq_event_cleanup(&ev);
        MOQ_TEST_CHECK(took_reset(c, cref));
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* SUBSCRIBE_TRACKS: the requester's FIN is no cancel; the responder's FIN
 * after its response completes the subscription with cause zero. */
static int test_subscribe_tracks_fin(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
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
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST, &ev));
    moq_track_sub_handle_t sh = ev.u.subscribe_tracks_request.handle;
    moq_event_cleanup(&ev);
    drain(c);
    moq_stream_ref_t ref; FIRST_REF(sv, track_subs, track_sub_cap, ref);
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(sv, ref), (int)MOQ_OK);
    moq_accept_subscribe_tracks_cfg_t ac; moq_accept_subscribe_tracks_cfg_init(&ac);
    if (version == MOQ_VERSION_DRAFT_18) {
        MOQ_TEST_CHECK(take(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED, &ev));
        moq_event_cleanup(&ev);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe_tracks(sv, sh, &ac, 6),
                              (int)MOQ_ERR_STALE_HANDLE);
        moq_simpair_destroy(sp);
        return failures;
    }
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_subscribe_tracks(sv, sh, &ac, 6), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_SUBSCRIBE_TRACKS_OK, &ev));
    moq_event_cleanup(&ev);
    /* Responder FIN after the response: complete, not failed. */
    moq_stream_ref_t cref; FIRST_REF(c, track_subs, track_sub_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_tracks_cancelled.cause, MOQ_REQUEST_TERMINAL_NONE);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* SUBSCRIBE_NAMESPACE: requester FIN keeps the request; responder FIN before
 * the response fails it with the typed namespace-subscription terminal. */
static int test_subscribe_namespace_fin(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
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
    moq_stream_ref_t ref = { 0 };
    for (size_t i = 0; i < sv->ns_sub_cap; i++)
        if (sv->ns_subs[i].stream_ref._v != 0) { ref = sv->ns_subs[i].stream_ref; break; }
    MOQ_TEST_CHECK(ref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(sv, ref), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED), 0);
    /* Requester side first: a responder FIN before REQUEST_OK fails it. */
    moq_stream_ref_t cref = { 0 };
    for (size_t i = 0; i < c->ns_sub_cap; i++)
        if (c->ns_subs[i].stream_ref._v != 0) { cref = c->ns_subs[i].stream_ref; break; }
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.local_role, MOQ_REQUEST_ROLE_REQUESTER);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_subscription_terminated.cause, MOQ_REQUEST_TERMINAL_MISSING_RESPONSE);
    MOQ_TEST_CHECK(moq_ns_sub_handle_eq(ev.u.ns_subscription_terminated.handle, nh));
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    /* The responder, its requester's FIN recorded, still accepts. */
    moq_accept_ns_sub_cfg_t ac; moq_accept_ns_sub_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_ns_sub(sv, sh, &ac, 6), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_fetch_responder_fin(moq_version_t version)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
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
    moq_stream_ref_t cref; FIRST_REF(c, fetches, fetch_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    (void)fin(c, cref);
    if (version == MOQ_VERSION_DRAFT_18) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_CLOSED);
        moq_simpair_destroy(sp);
        return failures;
    }
    moq_event_t ev;
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_RESET, &ev));
    MOQ_TEST_CHECK(moq_fetch_eq(ev.u.fetch_reset.fetch, fh));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.cause, MOQ_REQUEST_TERMINAL_MISSING_RESPONSE);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.error_code, 0);
    MOQ_TEST_CHECK(!ev.u.fetch_reset.stop_sending);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(took_reset(c, cref));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_track_status_responder_fin(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
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
    MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_TRACK_STATUS_REQUEST), 1);
    drain(c);
    moq_stream_ref_t cref; FIRST_REF(c, track_statuses, ts_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_TRACK_STATUS_TERMINATED, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.local_role, MOQ_TRACK_STATUS_REQUESTER);
    MOQ_TEST_CHECK_EQ_U64(ev.u.track_status_terminated.cause, MOQ_TRACK_STATUS_MISSING_RESPONSE);
    MOQ_TEST_CHECK(moq_track_status_handle_eq(ev.u.track_status_terminated.handle, th));
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

/* D22 namespace FIN outcomes. */
static int test_publish_namespace_fin(bool after_response)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22);
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
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_NAMESPACE_PUBLISHED, &ev));
    moq_announcement_t sh = ev.u.namespace_published.ann;
    moq_event_cleanup(&ev);
    drain(c);
    if (after_response) {
        moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_namespace(sv, sh, &ac, 6), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        drain(c);
    }
    moq_stream_ref_t cref; FIRST_REF(c, announcements, ann_cap, cref);
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)fin(c, cref), (int)MOQ_OK);
    bool found = take(c, MOQ_EVENT_NAMESPACE_TERMINATED, &ev);
    MOQ_TEST_CHECK(found);
    if (found) {
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.cause,
            after_response ? MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE : MOQ_NAMESPACE_TERMINAL_MISSING_RESPONSE);
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.local_role, MOQ_NAMESPACE_ROLE_REQUESTER);
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_terminated.stream_error_code, 0);
        MOQ_TEST_CHECK(moq_announcement_eq(ev.u.namespace_terminated.ann, ah));
        moq_event_cleanup(&ev);
    }
    if (!after_response) {
        MOQ_TEST_CHECK(took_reset(c, cref));
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_subscribe_requester_fin();
    failures += test_subscribe_established_requester_fin();
    failures += test_subscribe_responder_fin(MOQ_VERSION_DRAFT_22, false);
    failures += test_subscribe_responder_fin(MOQ_VERSION_DRAFT_22, true);
    failures += test_subscribe_responder_fin(MOQ_VERSION_DRAFT_18, false);
    failures += test_subscribe_established_fin_without_done();
    failures += test_publish_subscriber_fin(true);
    failures += test_publish_subscriber_fin(false);
    failures += test_subscribe_tracks_fin(MOQ_VERSION_DRAFT_22);
    failures += test_subscribe_tracks_fin(MOQ_VERSION_DRAFT_18);
    failures += test_subscribe_namespace_fin();
    failures += test_fetch_responder_fin(MOQ_VERSION_DRAFT_22);
    failures += test_fetch_responder_fin(MOQ_VERSION_DRAFT_18);
    failures += test_track_status_responder_fin();
    failures += test_publish_namespace_fin(true);
    failures += test_publish_namespace_fin(false);
    if (failures) { fprintf(stderr, "FAIL: test_request_fin (%d)\n", failures); return 1; }
    printf("PASS: test_request_fin\n");
    return 0;
}
