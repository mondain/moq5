/* Native TRACK_STATUS contract, using only public sessions and SimPair.
 * Independent kind/byte expectations deliberately reject a generic terminal
 * record. No facade, private layout or synthesized event. */
#include <moq/moq.h>
#ifndef MOQ_TS_LAYOUT_ONLY
#include <moq/sim.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#endif

#ifdef __cplusplus
#define TS_STATIC_ASSERT static_assert
#else
#define TS_STATIC_ASSERT _Static_assert
#endif

enum {
    TS_TERMINATED = 51,
    TS_DETAIL_SIZE = 24,
    TS_PEER_RESET = 1,
    TS_PEER_STOP = 2,
    TS_MISSING_RESPONSE = 3,
    TS_REQUESTER = 1,
    TS_RESPONDER = 2
};

TS_STATIC_ASSERT(sizeof(moq_event_t) == 144, "frozen event envelope");
TS_STATIC_ASSERT(offsetof(moq_event_t, u) == 16, "frozen event prefix");
TS_STATIC_ASSERT(sizeof(moq_track_status_handle_t) == 8, "typed handle width");
TS_STATIC_ASSERT(MOQ_EVENT_TRACK_STATUS_TERMINATED == 51 &&
               MOQ_EVENT_FILL_CREATED == 52 && MOQ_EVENT_FILL_REQUEST == 53 &&
               MOQ_EVENT_FILL_OBJECT == 54 && MOQ_EVENT_FILL_GAP == 55 &&
               MOQ_EVENT_FILL_TERMINAL == 56 && MOQ_EVENT_NAMESPACE_TERMINATED == 57 &&
               MOQ_EVENT_PUBLISH_STATE == 58 &&
               MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED == 59, "typed registry");
#define PIN_TERMINAL(T, handle_field) \
    TS_STATIC_ASSERT(sizeof(T) == 24 && offsetof(T, handle_field) == 0 && \
                   offsetof(T, cause) == 8 && offsetof(T, local_role) == 12 && \
                   offsetof(T, stream_error_code) == 16, #T " scalar layout")
PIN_TERMINAL(moq_track_status_terminated_event_t, handle);
PIN_TERMINAL(moq_namespace_terminated_event_t, ann);
PIN_TERMINAL(moq_ns_subscription_terminated_event_t, handle);
#undef PIN_TERMINAL
TS_STATIC_ASSERT(sizeof(moq_namespace_done_event_t) == 8, "frozen namespace DONE");
TS_STATIC_ASSERT(sizeof(moq_namespace_cancelled_event_t) == (sizeof(void *) == 8 ? 32 : 24),
                 "frozen namespace CANCELLED");
#undef TS_STATIC_ASSERT

#ifndef MOQ_TS_LAYOUT_ONLY

typedef struct trace_refs {
    bool armed;
    bool short_record;
    bool requester_fin;
    unsigned opens;
    unsigned inputs;
    moq_stream_ref_t requester;
    moq_stream_ref_t responder;
} trace_refs_t;

typedef struct test_case {
    const char *name;
    moq_simpair_t *pair;
    moq_session_t *requester;
    moq_session_t *responder;
    moq_track_status_handle_t requester_handle;
    moq_track_status_handle_t responder_handle;
    trace_refs_t refs;
    unsigned setup_failures;
    unsigned failures;
    unsigned requester_close_actions;
} test_case_t;

static unsigned cases, failed_cases, setup_cases, failed_checks;

static bool check(test_case_t *t, bool setup, const char *field,
                  uint64_t actual, uint64_t expected)
{
    if (actual == expected) return true;
    if (setup) ++t->setup_failures;
    else ++t->failures;
    printf("%s %s: %s expected=%" PRIu64 " actual=%" PRIu64 "\n",
           setup ? "SETUP_FAIL" : "FAIL", t->name, field, expected, actual);
    return false;
}

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    trace_refs_t *refs = ctx;
    if (!refs->armed) return;
    if (r->struct_size < offsetof(moq_sim_trace_record_t, fin) + sizeof(r->fin)) {
        refs->short_record = true;
        return;
    }
    if (r->kind == MOQ_SIM_TRACE_ACTION &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM &&
        r->from == MOQ_PERSPECTIVE_CLIENT) {
        ++refs->opens;
        refs->requester = r->stream_ref;
    }
    if (r->kind == MOQ_SIM_TRACE_INPUT &&
        r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES &&
        r->to == MOQ_PERSPECTIVE_SERVER) {
        ++refs->inputs;
        refs->responder = r->stream_ref;
        refs->requester_fin = r->fin;
    }
}

static bool next_event(test_case_t *t, moq_session_t *s, moq_event_t *ev,
                       bool setup)
{
    size_t count = 0;
    moq_result_t rc = moq_session_poll_events_ex(s, ev, 1, sizeof(*ev), &count);
    if (!check(t, setup, "poll_events", rc, MOQ_OK)) return false;
    return count != 0;
}

static void quiet(test_case_t *t, moq_session_t *s, const char *field)
{
    moq_event_t ev;
    while (next_event(t, s, &ev, false)) {
        check(t, false, field, ev.kind, 0);
        moq_event_cleanup(&ev);
    }
}

static bool begin_case_delivery(test_case_t *t, const char *name, moq_version_t version,
                                uint32_t max_events, uint32_t max_actions, bool delay_fin)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.initial_now_us = 1;
    cfg.max_events = max_events;
    cfg.max_actions = max_actions;
    cfg.trace_fn = trace;
    cfg.trace_ctx = &t->refs;
    if (!check(t, true, "create", moq_simpair_create(&cfg, &t->pair), MOQ_OK))
        return false;
    if (!check(t, true, "pair", t->pair != NULL, true)) return false;
    t->requester = moq_simpair_client(t->pair);
    t->responder = moq_simpair_server(t->pair);
    if (!check(t, true, "start", moq_simpair_start(t->pair), MOQ_OK)) return false;
    if (!check(t, true, "setup.pump",
               moq_simpair_run_until_quiescent(t->pair, 32, NULL), MOQ_OK))
        return false;
    moq_session_t *sides[] = {t->requester, t->responder};
    for (size_t i = 0; i < 2; ++i) {
        check(t, true, "setup.version", moq_session_version(sides[i]), version);
        check(t, true, "setup.state", moq_session_state(sides[i]), MOQ_SESS_ESTABLISHED);
        moq_event_t ev;
        while (next_event(t, sides[i], &ev, true)) moq_event_cleanup(&ev);
    }
    if (t->setup_failures) return false;

    moq_track_status_cfg_t request;
    moq_track_status_cfg_init(&request);
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("live")};
    request.track_namespace = (moq_namespace_t){parts, 1};
    request.track_name = MOQ_BYTES_LITERAL("v");
    t->refs.armed = true;
    moq_result_t rc = moq_session_track_status(t->requester, &request, 1,
                                              &t->requester_handle);
    if (!check(t, true, "request", rc, MOQ_OK)) return false;
    if (delay_fin) {
        /* The real public encoder sends FIN. Deliver its bytes first, as a
         * transport may, and retain the FIN for the directional teardown. */
        moq_action_t a;
        size_t count = 0;
        rc = moq_session_poll_actions_ex(t->requester, &a, 1, sizeof(a), &count);
        if (!check(t, true, "request.poll", rc, MOQ_OK) ||
            !check(t, true, "request.action_count", count, 1)) return false;
        bool opened = check(t, true, "request.action_kind", a.kind, MOQ_ACTION_OPEN_BIDI_STREAM);
        if (opened) {
            t->refs.requester = a.u.open_bidi_stream.stream_ref;
            t->refs.responder = moq_stream_ref_from_u64(UINT64_C(0x38990001));
            check(t, true, "request.encoder_fin", a.u.open_bidi_stream.fin, true);
            rc = moq_session_on_bidi_stream_bytes(t->responder, t->refs.responder,
                a.u.open_bidi_stream.data, a.u.open_bidi_stream.len, false, 1);
        }
        moq_action_cleanup(&a);
        if (!opened) return false;
    } else {
        rc = moq_simpair_run_until_quiescent(t->pair, 32, NULL);
    }
    t->refs.armed = false;
    if (!check(t, true, "request.deliver", rc, MOQ_OK)) return false;
    if (!delay_fin) {
        check(t, true, "trace.full_record", t->refs.short_record, false);
        check(t, true, "trace.open_count", t->refs.opens, 1);
        check(t, true, "trace.input_count", t->refs.inputs, 1);
    }
    check(t, true, "trace.requester_ref", t->refs.requester._v != 0, true);
    check(t, true, "trace.responder_ref", t->refs.responder._v != 0, true);
    check(t, true, "requester.handle",
          moq_track_status_handle_is_valid(t->requester_handle), true);
    unsigned requests = 0;
    moq_event_t ev;
    while (next_event(t, t->responder, &ev, true)) {
        if (check(t, true, "request.event_kind", ev.kind, MOQ_EVENT_TRACK_STATUS_REQUEST)) {
            ++requests;
            t->responder_handle = ev.u.track_status_request.handle;
        }
        moq_event_cleanup(&ev);
    }
    check(t, true, "request.event_count", requests, 1);
    check(t, true, "responder.handle",
          moq_track_status_handle_is_valid(t->responder_handle), true);
    while (next_event(t, t->requester, &ev, true)) {
        check(t, true, "requester.unexpected_event", ev.kind, 0);
        moq_event_cleanup(&ev);
    }
    return t->setup_failures == 0;
}

static bool begin_case_limits(test_case_t *t, const char *name, moq_version_t version,
                               uint32_t max_events, uint32_t max_actions)
{
    return begin_case_delivery(t, name, version, max_events, max_actions, false);
}

static bool begin_case(test_case_t *t, const char *name, moq_version_t version)
{
    return begin_case_limits(t, name, version, 0, 0);
}

static void drain_actions(test_case_t *t, moq_session_t *s, unsigned expected_closes)
{
    unsigned closes = 0;
    for (;;) {
        moq_action_t action;
        size_t count = 0;
        moq_result_t rc = moq_session_poll_actions_ex(s, &action, 1, sizeof(action), &count);
        if (!check(t, t->setup_failures != 0, "poll_actions", rc, MOQ_OK) || !count)
            break;
        if (action.kind == MOQ_ACTION_CLOSE_SESSION) ++closes;
        moq_action_cleanup(&action);
    }
    if (!t->setup_failures)
        check(t, false, "close_action_count", closes, expected_closes);
}

static void end_case(test_case_t *t)
{
    if (t->pair) {
        moq_session_t *sides[] = {moq_simpair_client(t->pair), moq_simpair_server(t->pair)};
        for (size_t i = 0; i < 2; ++i) {
            drain_actions(t, sides[i], i == 0 ? t->requester_close_actions : 0);
            if (!t->setup_failures) quiet(t, sides[i], "end.unexpected_event");
            else {
                moq_event_t ev;
                while (next_event(t, sides[i], &ev, true)) moq_event_cleanup(&ev);
            }
        }
        moq_simpair_destroy(t->pair);
    }
    ++cases;
    if (t->setup_failures) ++setup_cases;
    if (t->failures) ++failed_cases;
    failed_checks += t->failures;
    printf("%s %s (behavior_failures=%u setup_failures=%u)\n",
           t->setup_failures ? "SETUP_FAIL" : t->failures ? "FAIL" : "PASS",
           t->name, t->failures, t->setup_failures);
}

static void established(test_case_t *t)
{
    check(t, false, "requester.state", moq_session_state(t->requester), MOQ_SESS_ESTABLISHED);
    check(t, false, "responder.state", moq_session_state(t->responder), MOQ_SESS_ESTABLISHED);
}

static void terminal(test_case_t *t, moq_session_t *s,
                     moq_track_status_handle_t handle, uint32_t cause,
                     uint32_t role, uint64_t code)
{
    unsigned count = 0;
    moq_event_t ev;
    while (next_event(t, s, &ev, false)) {
        ++count;
        if (check(t, false, "terminal.kind", ev.kind, TS_TERMINATED)) {
            check(t, false, "terminal.detail_size", ev.detail_size, TS_DETAIL_SIZE);
            if (ev.detail_size >= TS_DETAIL_SIZE) {
                /* Read the specified byte slots, never the generic union member. */
                const unsigned char *bytes = (const unsigned char *)&ev.u;
                moq_track_status_handle_t actual_handle;
                uint32_t actual_cause, actual_role;
                uint64_t actual_code;
                memcpy(&actual_handle, bytes, 8);
                memcpy(&actual_cause, bytes + 8, 4);
                memcpy(&actual_role, bytes + 12, 4);
                memcpy(&actual_code, bytes + 16, 8);
                check(t, false, "terminal.handle@0",
                      moq_track_status_handle_id_for_trace(actual_handle),
                      moq_track_status_handle_id_for_trace(handle));
                check(t, false, "terminal.cause@8", actual_cause, cause);
                check(t, false, "terminal.role@12", actual_role, role);
                check(t, false, "terminal.code@16", actual_code, code);
            }
        }
        moq_event_cleanup(&ev);
    }
    check(t, false, "terminal.event_count", count, 1);
}

static moq_result_t peer_signal(moq_session_t *s, moq_stream_ref_t ref,
                                bool reset, uint64_t code)
{
    return reset ? moq_session_on_bidi_stream_reset(s, ref, code, 1)
                 : moq_session_on_bidi_stream_stop(s, ref, code, 1);
}

static void signal_case(const char *name, moq_version_t version, bool responder,
                        bool reset, uint64_t code)
{
    test_case_t t;
    if (begin_case(&t, name, version)) {
        moq_session_t *s = responder ? t.responder : t.requester;
        moq_stream_ref_t ref = responder ? t.refs.responder : t.refs.requester;
        check(&t, false, "signal", peer_signal(s, ref, reset, code), MOQ_OK);
        if (version == MOQ_VERSION_DRAFT_22)
            terminal(&t, s, responder ? t.responder_handle : t.requester_handle,
                     reset ? TS_PEER_RESET : TS_PEER_STOP,
                     responder ? TS_RESPONDER : TS_REQUESTER, code);
        else
            quiet(&t, s, "D18.silent_retirement.event_kind");
        if (version == MOQ_VERSION_DRAFT_22 && !responder && !reset) {
            /* STOP did not terminate the peer's sending direction. */
            check(&t, false, "stop.late_fin", moq_session_on_bidi_stream_bytes(
                s, ref, NULL, 0, true, 1), MOQ_OK);
            quiet(&t, s, "stop.late_fin.duplicate");
        }
        established(&t);
    }
    end_case(&t);
}

static void missing_fin_case(const char *name, moq_version_t version)
{
    test_case_t t;
    if (begin_case(&t, name, version)) {
        check(&t, false, "responder_fin", moq_session_on_bidi_stream_bytes(
            t.requester, t.refs.requester, NULL, 0, true, 1), MOQ_OK);
        if (version == MOQ_VERSION_DRAFT_22) {
            terminal(&t, t.requester, t.requester_handle,
                     TS_MISSING_RESPONSE, TS_REQUESTER, 0);
            established(&t);
        } else {
            /* Legacy behavior, not a cross-profile terminal. */
            t.requester_close_actions = 1;
            check(&t, false, "D18.state", moq_session_state(t.requester), MOQ_SESS_CLOSED);
            unsigned count = 0;
            moq_event_t ev;
            while (next_event(&t, t.requester, &ev, false)) {
                ++count;
                if (check(&t, false, "D18.event_kind", ev.kind, MOQ_EVENT_SESSION_CLOSED))
                    check(&t, false, "D18.close_code", ev.u.closed.code, 3);
                moq_event_cleanup(&ev);
            }
            check(&t, false, "D18.close_event_count", count, 1);
        }
    }
    end_case(&t);
}

static void response_case(const char *name, moq_version_t version,
                          bool requester_fin, bool late_signal, bool reset)
{
    test_case_t t;
    if (begin_case(&t, name, version)) {
        /* Normal requests already carry FIN in current profiles. If that
         * policy changes, still exercise a real requester FIN before deciding. */
        if (requester_fin && !t.refs.requester_fin)
            check(&t, false, "requester_fin", moq_session_on_bidi_stream_bytes(
                t.responder, t.refs.responder, NULL, 0, true, 1), MOQ_OK);
        quiet(&t, t.responder, "requester_fin.unexpected_event");
        moq_accept_track_status_cfg_t cfg;
        moq_accept_track_status_cfg_init(&cfg);
        cfg.has_largest = true;
        cfg.largest_group = 7;
        cfg.largest_object = 9;
        check(&t, false, "accept", moq_session_accept_track_status(
            t.responder, t.responder_handle, &cfg, 1), MOQ_OK);
        check(&t, false, "response.pump",
              moq_simpair_run_until_quiescent(t.pair, 32, NULL), MOQ_OK);
        unsigned count = 0;
        moq_event_t ev;
        while (next_event(&t, t.requester, &ev, false)) {
            ++count;
            if (check(&t, false, "response.kind", ev.kind, MOQ_EVENT_TRACK_STATUS_OK)) {
                check(&t, false, "response.handle",
                      moq_track_status_handle_eq(ev.u.track_status_ok.handle, t.requester_handle), true);
                check(&t, false, "response.has_largest", ev.u.track_status_ok.has_largest, true);
                check(&t, false, "response.group", ev.u.track_status_ok.largest_group, 7);
                check(&t, false, "response.object", ev.u.track_status_ok.largest_object, 9);
            }
            moq_event_cleanup(&ev);
        }
        check(&t, false, "response.event_count", count, 1);
        if (late_signal) {
            check(&t, false, "late.requester", peer_signal(
                t.requester, t.refs.requester, reset, UINT64_MAX), MOQ_OK);
            check(&t, false, "late.responder", peer_signal(
                t.responder, t.refs.responder, reset, UINT64_MAX), MOQ_OK);
        }
        established(&t);
    }
    end_case(&t);
}

/* A single unrelated public request occupies exactly one action/event slot. */
static bool extra_request(test_case_t *t, moq_session_t *s, bool pump)
{
    moq_track_status_cfg_t cfg;
    moq_track_status_cfg_init(&cfg);
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("held")};
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("other");
    moq_track_status_handle_t handle;
    if (!check(t, true, "filler.request",
               moq_session_track_status(s, &cfg, 1, &handle), MOQ_OK)) return false;
    return !pump || check(t, true, "filler.pump",
                          moq_simpair_run_until_quiescent(t->pair, 32, NULL), MOQ_OK);
}

static bool take_filler(test_case_t *t, moq_session_t *s)
{
    moq_event_t ev;
    if (!check(t, true, "filler.present", next_event(t, s, &ev, true), true))
        return false;
    bool ok = check(t, true, "filler.kind", ev.kind, MOQ_EVENT_TRACK_STATUS_REQUEST);
    moq_event_cleanup(&ev);
    return ok;
}

static bool next_action(test_case_t *t, moq_session_t *s, moq_action_t *a,
                        moq_action_kind_t kind)
{
    size_t count = 0;
    if (!check(t, true, "action.poll",
               moq_session_poll_actions_ex(s, a, 1, sizeof(*a), &count), MOQ_OK))
        return false;
    if (!check(t, true, "action.count", count, 1)) return false;
    return check(t, false, "action.kind", a->kind, kind);
}

static void first_cause_case(bool reset)
{
    test_case_t t;
    const char *name = reset ? "d22.blocked.reset0_then_stop_max"
                             : "d22.blocked.stop0_then_reset_max";
    if (begin_case_limits(&t, name, MOQ_VERSION_DRAFT_22, 1, 1) &&
        extra_request(&t, t.requester, true) && extra_request(&t, t.responder, false)) {
        check(&t, false, "first.refused",
              peer_signal(t.responder, t.refs.responder, reset, 0), MOQ_ERR_WOULD_BLOCK);
        check(&t, false, "opposite.refused",
              peer_signal(t.responder, t.refs.responder, !reset, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
        check(&t, false, "repeat.refused",
              peer_signal(t.responder, t.refs.responder, reset, 7), MOQ_ERR_WOULD_BLOCK);
        moq_accept_track_status_cfg_t ac;
        moq_accept_track_status_cfg_init(&ac);
        moq_reject_track_status_cfg_t rc;
        moq_reject_track_status_cfg_init(&rc);
        rc.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        check(&t, false, "failed.accept", moq_session_accept_track_status(
            t.responder, t.responder_handle, &ac, 1), MOQ_ERR_WRONG_STATE);
        check(&t, false, "failed.reject", moq_session_reject_track_status(
            t.responder, t.responder_handle, &rc, 1), MOQ_ERR_WRONG_STATE);
        if (take_filler(&t, t.responder)) {
            terminal(&t, t.responder, t.responder_handle,
                     reset ? TS_PEER_RESET : TS_PEER_STOP, TS_RESPONDER, 0);
            check(&t, false, "notified.accept", moq_session_accept_track_status(
                t.responder, t.responder_handle, &ac, 1), MOQ_ERR_STALE_HANDLE);
            check(&t, false, "notified.reject", moq_session_reject_track_status(
                t.responder, t.responder_handle, &rc, 1), MOQ_ERR_STALE_HANDLE);
            moq_action_t a;
            if (next_action(&t, t.responder, &a, MOQ_ACTION_OPEN_BIDI_STREAM))
                moq_action_cleanup(&a);
            if (next_action(&t, t.responder, &a, MOQ_ACTION_RESET_BIDI_STREAM)) {
                check(&t, false, "cancel.ref", a.u.reset_bidi_stream.stream_ref._v,
                      t.refs.responder._v);
                check(&t, false, "cancel.code", a.u.reset_bidi_stream.error_code, 1);
                moq_action_cleanup(&a);
            }
            size_t count = 99;
            check(&t, false, "cancel.drain", moq_session_poll_actions_ex(
                t.responder, &a, 1, sizeof(a), &count), MOQ_OK);
            check(&t, false, "cancel.no_duplicate", count, 0);
        }
        established(&t);
    }
    end_case(&t);
}

static void retained_response_case(bool reject, bool reset, bool fin)
{
    char name[96];
    snprintf(name, sizeof(name), "d22.retained_%s.%s.%s", reject ? "error" : "ok",
             reset ? "reset" : "stop", fin ? "fin" : "no_fin");
    test_case_t t;
    if (begin_case_limits(&t, name, MOQ_VERSION_DRAFT_22, 1, 0) &&
        extra_request(&t, t.responder, true)) {
        moq_result_t rc;
        if (reject) {
            moq_reject_track_status_cfg_t cfg;
            moq_reject_track_status_cfg_init(&cfg);
            cfg.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
            cfg.reason = MOQ_BYTES_LITERAL("gone");
            cfg.can_retry = true;
            cfg.retry_after_ms = 17;
            rc = moq_session_reject_track_status(t.responder, t.responder_handle, &cfg, 1);
        } else {
            moq_accept_track_status_cfg_t cfg;
            moq_accept_track_status_cfg_init(&cfg);
            cfg.has_largest = true;
            cfg.largest_group = 7;
            cfg.largest_object = 9;
            rc = moq_session_accept_track_status(t.responder, t.responder_handle, &cfg, 1);
        }
        if (check(&t, true, "response.send", rc, MOQ_OK)) {
            moq_action_t a;
            if (next_action(&t, t.responder, &a, MOQ_ACTION_SEND_BIDI_STREAM)) {
                check(&t, true, "response.fin", a.u.send_bidi_stream.fin, true);
                check(&t, false, "response.retained", moq_session_on_bidi_stream_bytes(
                    t.requester, t.refs.requester, a.u.send_bidi_stream.data,
                    a.u.send_bidi_stream.len, fin, 1), MOQ_ERR_WOULD_BLOCK);
                moq_action_cleanup(&a);
                check(&t, false, "response.late_signal", peer_signal(
                    t.requester, t.refs.requester, reset, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
                if (take_filler(&t, t.requester)) {
                    moq_event_t ev;
                    if (check(&t, false, "response.present",
                              next_event(&t, t.requester, &ev, false), true)) {
                        check(&t, false, "response.kind", ev.kind,
                              reject ? MOQ_EVENT_TRACK_STATUS_ERROR : MOQ_EVENT_TRACK_STATUS_OK);
                        if (ev.kind == MOQ_EVENT_TRACK_STATUS_ERROR) {
                            check(&t, false, "response.handle", moq_track_status_handle_eq(
                                ev.u.track_status_error.handle, t.requester_handle), true);
                            check(&t, false, "response.code", ev.u.track_status_error.error_code, 0x10);
                            check(&t, false, "response.retry", ev.u.track_status_error.can_retry, true);
                            check(&t, false, "response.retry_after", ev.u.track_status_error.retry_after_ms, 17);
                            check(&t, false, "response.reason.len", ev.u.track_status_error.reason.len, 4);
                            if (ev.u.track_status_error.reason.len == 4)
                                check(&t, false, "response.reason.bytes",
                                      memcmp(ev.u.track_status_error.reason.data, "gone", 4), 0);
                        } else if (ev.kind == MOQ_EVENT_TRACK_STATUS_OK) {
                            check(&t, false, "response.handle", moq_track_status_handle_eq(
                                ev.u.track_status_ok.handle, t.requester_handle), true);
                            check(&t, false, "response.has_largest", ev.u.track_status_ok.has_largest, true);
                            check(&t, false, "response.group", ev.u.track_status_ok.largest_group, 7);
                            check(&t, false, "response.object", ev.u.track_status_ok.largest_object, 9);
                        }
                        moq_event_cleanup(&ev);
                    }
                    if (!fin && !reset)
                        check(&t, false, "response.final_fin", moq_session_on_bidi_stream_bytes(
                            t.requester, t.refs.requester, NULL, 0, true, 1), MOQ_OK);
                    quiet(&t, t.requester, "response.no_second_outcome");
                }
            }
        }
        established(&t);
    }
    end_case(&t);
}

static void scalar_poll_case(size_t element_size)
{
    char name[80];
    snprintf(name, sizeof(name), "d22.scalar_poll.%zu", element_size);
    test_case_t t;
    if (begin_case(&t, name, MOQ_VERSION_DRAFT_22)) {
        check(&t, false, "scalar.reset", peer_signal(
            t.requester, t.refs.requester, true, UINT64_MAX), MOQ_OK);
        unsigned char storage[176];
        memset(storage, 0xa5, sizeof(storage));
        size_t count = 0;
        check(&t, false, "scalar.poll", moq_session_poll_events_ex(
            t.requester, storage, 1, element_size, &count), MOQ_OK);
        check(&t, false, "scalar.count", count, 1);
        uint32_t kind, detail;
        memcpy(&kind, storage, 4);
        memcpy(&detail, storage + 4, 4);
        check(&t, false, "scalar.kind", kind, 51);
        check(&t, false, "scalar.producer_detail", detail, 24);
        for (size_t i = element_size; i < sizeof(storage); ++i)
            check(&t, false, "scalar.canary", storage[i], 0xa5);
        if (element_size >= 40) {
            uint64_t code;
            memcpy(&code, storage + 32, 8);
            check(&t, false, "scalar.full_code", code, UINT64_MAX);
        }
        /* Scalar cleanup has no ownership even when the caller was short. */
        moq_event_t ev;
        memset(&ev, 0, sizeof(ev));
        memcpy(&ev, storage, element_size < sizeof(ev) ? element_size : sizeof(ev));
        moq_event_cleanup(&ev);
        moq_event_cleanup(&ev);
        established(&t);
    }
    end_case(&t);
}

static void truncated_fin_case(void)
{
    test_case_t t;
    if (begin_case(&t, "d22.truncated_response_fin", MOQ_VERSION_DRAFT_22)) {
        moq_accept_track_status_cfg_t cfg;
        moq_accept_track_status_cfg_init(&cfg);
        if (check(&t, true, "truncated.accept", moq_session_accept_track_status(
            t.responder, t.responder_handle, &cfg, 1), MOQ_OK)) {
            moq_action_t a;
            if (next_action(&t, t.responder, &a, MOQ_ACTION_SEND_BIDI_STREAM)) {
                if (check(&t, true, "truncated.has_frame", a.u.send_bidi_stream.len > 1, true))
                    check(&t, false, "truncated.fin", moq_session_on_bidi_stream_bytes(
                        t.requester, t.refs.requester, a.u.send_bidi_stream.data, 1, true, 1), MOQ_OK);
                moq_action_cleanup(&a);
                terminal(&t, t.requester, t.requester_handle, 4, TS_REQUESTER, 0);
            }
        }
        established(&t);
    }
    end_case(&t);
}

static void retained_goaway_case(bool reset)
{
    test_case_t t;
    if (begin_case_limits(&t, reset ? "d22.retained_goaway.reset" : "d22.retained_goaway.stop",
                          MOQ_VERSION_DRAFT_22, 1, 1) &&
        extra_request(&t, t.responder, true) && extra_request(&t, t.requester, false)) {
        moq_request_goaway_cfg_t cfg;
        moq_request_goaway_cfg_init(&cfg);
        cfg.new_session_uri = MOQ_BYTES_LITERAL("https://next.example/moq");
        cfg.timeout_ms = 37;
        if (check(&t, true, "migration.send", moq_session_request_goaway_track_status(
            t.responder, t.responder_handle, &cfg, 1), MOQ_OK)) {
            moq_action_t a;
            if (next_action(&t, t.responder, &a, MOQ_ACTION_SEND_BIDI_STREAM)) {
                check(&t, true, "migration.fin", a.u.send_bidi_stream.fin, false);
                check(&t, false, "migration.retained", moq_session_on_bidi_stream_bytes(
                    t.requester, t.refs.requester, a.u.send_bidi_stream.data,
                    a.u.send_bidi_stream.len, false, 1), MOQ_ERR_WOULD_BLOCK);
                moq_action_cleanup(&a);
                check(&t, false, "migration.late_signal", peer_signal(
                    t.requester, t.refs.requester, reset, 0), MOQ_ERR_WOULD_BLOCK);
                if (take_filler(&t, t.requester)) {
                    moq_event_t ev;
                    if (check(&t, false, "migration.present", next_event(&t, t.requester, &ev, false), true)) {
                        if (check(&t, false, "migration.kind", ev.kind, MOQ_EVENT_REQUEST_GOAWAY)) {
                            check(&t, false, "migration.family", ev.u.request_goaway.family, MOQ_REQUEST_FAMILY_TRACK_STATUS);
                            check(&t, false, "migration.handle", moq_track_status_handle_eq(
                                ev.u.request_goaway.handle.track_status, t.requester_handle), true);
                            check(&t, false, "migration.timeout", ev.u.request_goaway.timeout_ms, 37);
                            check(&t, false, "migration.uri.len", ev.u.request_goaway.new_session_uri.len,
                                  cfg.new_session_uri.len);
                            if (ev.u.request_goaway.new_session_uri.len == cfg.new_session_uri.len)
                                check(&t, false, "migration.uri.bytes", memcmp(ev.u.request_goaway.new_session_uri.data,
                                      cfg.new_session_uri.data, cfg.new_session_uri.len), 0);
                        }
                        moq_event_cleanup(&ev);
                    }
                    if (next_action(&t, t.requester, &a, MOQ_ACTION_OPEN_BIDI_STREAM))
                        moq_action_cleanup(&a);
                    if (!reset) {
                        if (next_action(&t, t.requester, &a, MOQ_ACTION_STOP_BIDI_STREAM)) {
                            check(&t, false, "migration.stop.ref", a.u.stop_bidi_stream.stream_ref._v, t.refs.requester._v);
                            check(&t, false, "migration.stop.code", a.u.stop_bidi_stream.error_code, 1);
                            moq_action_cleanup(&a);
                        }
                        check(&t, false, "migration.final_fin", moq_session_on_bidi_stream_bytes(
                            t.requester, t.refs.requester, NULL, 0, true, 1), MOQ_OK);
                    }
                    quiet(&t, t.requester, "migration.no_second_outcome");
                }
            }
        }
        established(&t);
    }
    end_case(&t);
}

static void sent_migration_decisions(test_case_t *t, moq_result_t expected)
{
    moq_accept_track_status_cfg_t accept;
    moq_accept_track_status_cfg_init(&accept);
    moq_reject_track_status_cfg_t reject;
    moq_reject_track_status_cfg_init(&reject);
    moq_request_goaway_cfg_t goaway;
    moq_request_goaway_cfg_init(&goaway);
    check(t, false, "sent.accept", moq_session_accept_track_status(
        t->responder, t->responder_handle, &accept, 1), expected);
    check(t, false, "sent.reject", moq_session_reject_track_status(
        t->responder, t->responder_handle, &reject, 1), expected);
    check(t, false, "sent.goaway_again", moq_session_request_goaway_track_status(
        t->responder, t->responder_handle, &goaway, 1), expected);
}

static void sent_migration_action(test_case_t *t, moq_action_kind_t kind)
{
    moq_action_t a;
    size_t count = 0;
    check(t, false, "sent.poll", moq_session_poll_actions_ex(
        t->responder, &a, 1, sizeof(a), &count), MOQ_OK);
    if (!check(t, false, "sent.action_count", count, 1)) return;
    if (check(t, false, "sent.action_kind", a.kind, kind)) {
        if (kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            check(t, false, "sent.goaway.ref", a.u.send_bidi_stream.stream_ref._v,
                  t->refs.responder._v);
            check(t, false, "sent.goaway.open_half", a.u.send_bidi_stream.fin, false);
        } else {
            bool reset = kind == MOQ_ACTION_RESET_BIDI_STREAM;
            check(t, false, "sent.cancel.ref", reset ? a.u.reset_bidi_stream.stream_ref._v
                                                      : a.u.stop_bidi_stream.stream_ref._v,
                  t->refs.responder._v);
            check(t, false, "sent.cancel.code", reset ? a.u.reset_bidi_stream.error_code
                                                       : a.u.stop_bidi_stream.error_code, 1);
        }
    }
    moq_action_cleanup(&a);
}

static void sent_migration_no_actions(test_case_t *t)
{
    moq_action_t a;
    size_t count = 0;
    check(t, false, "sent.empty_poll", moq_session_poll_actions_ex(
        t->responder, &a, 1, sizeof(a), &count), MOQ_OK);
    check(t, false, "sent.no_extra_action", count, 0);
    if (count) moq_action_cleanup(&a);
}

static void sent_goaway_case(bool reset, bool pressure, bool stop_first, bool prior_fin)
{
    char name[112];
    snprintf(name, sizeof(name), "d22.sent_goaway.%s%s.%s.%s",
             prior_fin ? "prior_fin." : "", stop_first ? "stop_then" : "direct",
             reset ? "reset" : "fin", pressure ? "full_action" : "one_action");
    test_case_t t;
    if (begin_case_delivery(&t, name, MOQ_VERSION_DRAFT_22, 1, 1, !prior_fin)) {
        moq_request_goaway_cfg_t cfg;
        moq_request_goaway_cfg_init(&cfg);
        cfg.timeout_ms = 37;
        if (check(&t, true, "sent.send", moq_session_request_goaway_track_status(
                t.responder, t.responder_handle, &cfg, 1), MOQ_OK)) {
            if (!pressure) sent_migration_action(&t, MOQ_ACTION_SEND_BIDI_STREAM);
            sent_migration_decisions(&t, MOQ_ERR_WRONG_STATE);
            check(&t, false, "sent.before_teardown.owner", moq_session_has_transport_stream(
                t.responder, t.refs.responder), true);
            if (stop_first) {
                check(&t, false, "sent.stop", peer_signal(
                    t.responder, t.refs.responder, false, 0), MOQ_OK);
                sent_migration_decisions(&t, MOQ_ERR_STALE_HANDLE);
                quiet(&t, t.responder, "sent.no_terminal_after_stop");
                check(&t, false, "sent.after_stop.owner", moq_session_has_transport_stream(
                    t.responder, t.refs.responder), !prior_fin || pressure);
                if (!pressure) {
                    sent_migration_action(&t, MOQ_ACTION_RESET_BIDI_STREAM);
                    if (!prior_fin) sent_migration_action(&t, MOQ_ACTION_STOP_BIDI_STREAM);
                    sent_migration_no_actions(&t);
                    check(&t, false, "sent.after_output.owner", moq_session_has_transport_stream(
                        t.responder, t.refs.responder), !prior_fin);
                }
            }
            if (!prior_fin) {
                check(&t, false, "sent.peer_receive_end", reset
                    ? peer_signal(t.responder, t.refs.responder, true, UINT64_MAX)
                    : moq_session_on_bidi_stream_bytes(t.responder, t.refs.responder,
                                                       NULL, 0, true, 1), MOQ_OK);
            }
            if (pressure) {
                check(&t, false, "sent.output_still_owed.owner", moq_session_has_transport_stream(
                    t.responder, t.refs.responder), true);
                sent_migration_decisions(&t, MOQ_ERR_STALE_HANDLE);
                sent_migration_action(&t, MOQ_ACTION_SEND_BIDI_STREAM);
            }
            if (pressure || !stop_first)
                sent_migration_action(&t, MOQ_ACTION_RESET_BIDI_STREAM);
            sent_migration_no_actions(&t);
            check(&t, false, "sent.retired.owner", moq_session_has_transport_stream(
                t.responder, t.refs.responder), false);
            sent_migration_decisions(&t, MOQ_ERR_STALE_HANDLE);
            quiet(&t, t.responder, "sent.no_second_outcome");
            established(&t);
        }
    }
    end_case(&t);
}

static void crossed_goaway_case(void)
{
    test_case_t t;
    if (begin_case_delivery(&t, "d22.sent_goaway.first_received_goaway", MOQ_VERSION_DRAFT_22,
                            1, 1, true)) {
        moq_request_goaway_cfg_t cfg;
        moq_request_goaway_cfg_init(&cfg);
        cfg.timeout_ms = 37;
        if (check(&t, true, "crossed.send", moq_session_request_goaway_track_status(
                t.responder, t.responder_handle, &cfg, 1), MOQ_OK)) {
            sent_migration_action(&t, MOQ_ACTION_SEND_BIDI_STREAM);
            /* Section 9.2: our sent GOAWAY is not a received GOAWAY. The
             * one-action queue retains the second cancellation direction. */
            const uint8_t goaway[] = {0x10, 0, 2, 0, 37};
            check(&t, false, "crossed.receive", moq_session_on_bidi_stream_bytes(
                t.responder, t.refs.responder, goaway, sizeof(goaway), false, 1), MOQ_ERR_WOULD_BLOCK);
            moq_event_t ev;
            if (check(&t, false, "crossed.event", next_event(&t, t.responder, &ev, false), true)) {
                if (check(&t, false, "crossed.kind", ev.kind, MOQ_EVENT_REQUEST_GOAWAY)) {
                    check(&t, false, "crossed.family", ev.u.request_goaway.family,
                          MOQ_REQUEST_FAMILY_TRACK_STATUS);
                    check(&t, false, "crossed.handle", moq_track_status_handle_eq(
                        ev.u.request_goaway.handle.track_status, t.responder_handle), true);
                    check(&t, false, "crossed.uri", ev.u.request_goaway.new_session_uri.len, 0);
                    check(&t, false, "crossed.timeout", ev.u.request_goaway.timeout_ms, 37);
                }
                moq_event_cleanup(&ev);
            }
            sent_migration_decisions(&t, MOQ_ERR_STALE_HANDLE);
            sent_migration_action(&t, MOQ_ACTION_RESET_BIDI_STREAM);
            sent_migration_action(&t, MOQ_ACTION_STOP_BIDI_STREAM);
            sent_migration_no_actions(&t);
            check(&t, false, "crossed.peer_owner", moq_session_has_transport_stream(
                t.responder, t.refs.responder), true);
            check(&t, false, "crossed.peer_fin", moq_session_on_bidi_stream_bytes(
                t.responder, t.refs.responder, NULL, 0, true, 1), MOQ_OK);
            check(&t, false, "crossed.retired", moq_session_has_transport_stream(
                t.responder, t.refs.responder), false);
            quiet(&t, t.responder, "crossed.no_second_outcome");
            established(&t);
        }
    }
    end_case(&t);
}

int main(void)
{
    missing_fin_case("d22.missing_responder_fin", MOQ_VERSION_DRAFT_22);
    response_case("d22.requester_fin_then_ok", MOQ_VERSION_DRAFT_22, true, false, false);
    for (unsigned responder = 0; responder < 2; ++responder)
        for (unsigned reset = 0; reset < 2; ++reset)
            for (unsigned max_code = 0; max_code < 2; ++max_code) {
                char name[80];
                snprintf(name, sizeof(name), "d22.%s.peer_%s.%s",
                         responder ? "responder" : "requester",
                         reset ? "reset" : "stop", max_code ? "max" : "zero");
                signal_case(name, MOQ_VERSION_DRAFT_22, responder != 0,
                            reset != 0, max_code ? UINT64_MAX : 0);
            }
    response_case("d22.ok_then_late_reset", MOQ_VERSION_DRAFT_22, false, true, true);
    response_case("d22.ok_then_late_stop", MOQ_VERSION_DRAFT_22, false, true, false);
    signal_case("d18.requester.peer_reset.silent", MOQ_VERSION_DRAFT_18, false, true, 0);
    signal_case("d18.requester.peer_stop.silent", MOQ_VERSION_DRAFT_18, false, false, 0);
    missing_fin_case("d18.missing_responder_fin.closes", MOQ_VERSION_DRAFT_18);
    response_case("d18.requester_fin_then_ok", MOQ_VERSION_DRAFT_18, true, false, false);
    first_cause_case(true);
    first_cause_case(false);
    for (unsigned reject = 0; reject < 2; ++reject)
        for (unsigned reset = 0; reset < 2; ++reset)
            for (unsigned fin = 0; fin < 2; ++fin)
                retained_response_case(reject != 0, reset != 0, fin != 0);
    const size_t scalar_sizes[] = {16, 39, 40, 144, 160};
    for (size_t i = 0; i < sizeof(scalar_sizes) / sizeof(scalar_sizes[0]); ++i)
        scalar_poll_case(scalar_sizes[i]);
    truncated_fin_case();
    retained_goaway_case(true);
    retained_goaway_case(false);
    for (unsigned reset = 0; reset < 2; ++reset)
        for (unsigned pressure = 0; pressure < 2; ++pressure)
            for (unsigned stop = 0; stop < 2; ++stop)
                sent_goaway_case(reset != 0, pressure != 0, stop != 0, false);
    sent_goaway_case(false, false, true, true);
    sent_goaway_case(false, true, true, true);
    crossed_goaway_case();
    printf("SUMMARY cases=%u behavior_failed_cases=%u behavior_failed_checks=%u setup_failed_cases=%u\n",
           cases, failed_cases, failed_checks, setup_cases);
    return setup_cases ? 2 : failed_cases ? 1 : 0;
}
#endif
