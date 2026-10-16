/* Namespace-subscription lifecycle, using public sessions and SimPair.
 * No manufactured handles/stream refs, private layout, or fabricated events. */
#include <moq/moq.h>
#include <moq/sim.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { NS_TERMINATED = 59, NS_DETAIL_SIZE = 24, DRAIN_ROUNDS = 32 };
_Static_assert(MOQ_EVENT_NAMESPACE_SUBSCRIPTION_TERMINATED == NS_TERMINATED,
               "namespace subscription has its own event kind");
_Static_assert(sizeof(moq_ns_subscription_terminated_event_t) == NS_DETAIL_SIZE &&
               offsetof(moq_ns_subscription_terminated_event_t, handle) == 0 &&
               offsetof(moq_ns_subscription_terminated_event_t, cause) == 8 &&
               offsetof(moq_ns_subscription_terminated_event_t, local_role) == 12 &&
               offsetof(moq_ns_subscription_terminated_event_t, stream_error_code) == 16,
               "typed scalar terminal layout");
_Static_assert(offsetof(moq_event_t, u) == 16, "event prefix");
_Static_assert(MOQ_REQUEST_TERMINAL_NONE == 0 &&
               MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM == 1 &&
               MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING == 2 &&
               MOQ_REQUEST_TERMINAL_MISSING_RESPONSE == 3 &&
               MOQ_REQUEST_TERMINAL_TRUNCATED_RESPONSE == 4 &&
               MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE == 5,
               "request terminal causes are append-only");

typedef enum signal_kind { SIGNAL_FIN, SIGNAL_RESET, SIGNAL_STOP } signal_kind_t;

typedef struct trace_refs {
    bool armed, short_record, requester_fin;
    unsigned opens, inputs;
    moq_stream_ref_t requester, responder;
} trace_refs_t;

typedef struct test_case {
    const char *name;
    moq_simpair_t *pair;
    moq_session_t *requester, *responder;
    moq_ns_sub_handle_t requester_handle, responder_handle;
    moq_publication_t publisher, subscriber;
    trace_refs_t refs;
    unsigned setup_failures, failures, masked, held;
    moq_stream_ref_t watch_ref;
    unsigned resets, stops, closes;
} test_case_t;

typedef struct observations {
    moq_ns_sub_handle_t handle;
    unsigned expected_gone, expected_terminals;
    unsigned gone, gone_mask, terminals, full_terminals;
    uint32_t cause, role;
    uint64_t code;
    bool cause_known;
} observations_t;

static unsigned cases, passed_cases, failed_cases, setup_cases, failed_checks, masked_checks, held_checks;

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

static bool result(test_case_t *t, bool setup, const char *field,
                   moq_result_t actual, moq_result_t expected)
{
    if (actual == expected) return true;
    if (setup) ++t->setup_failures;
    else ++t->failures;
    printf("%s %s: %s expected=%d actual=%d\n",
           setup ? "SETUP_FAIL" : "FAIL", t->name, field, (int)expected, (int)actual);
    return false;
}

static void mask(test_case_t *t, const char *field, unsigned count)
{
    t->masked += count;
    printf("MASKED %s: %s (%u checks)\n", t->name, field, count);
}

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    trace_refs_t *refs = ctx;
    if (!refs->armed) return;
    if (r->struct_size < offsetof(moq_sim_trace_record_t, fin) + sizeof(r->fin)) {
        refs->short_record = true;
        return;
    }
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        ++refs->opens;
        refs->requester = r->stream_ref;
    }
    if (r->kind == MOQ_SIM_TRACE_INPUT && r->to == MOQ_PERSPECTIVE_SERVER &&
        r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES) {
        ++refs->inputs;
        refs->responder = r->stream_ref;
        refs->requester_fin = r->fin;
    }
}

static bool poll_event(test_case_t *t, moq_session_t *s, moq_event_t *ev, bool setup)
{
    size_t count = 0;
    memset(ev, 0, sizeof(*ev));
    if (!result(t, setup, "poll_events",
                moq_session_poll_events_ex(s, ev, 1, sizeof(*ev), &count), MOQ_OK))
        return false;
    return count != 0;
}

static bool take_event(test_case_t *t, moq_session_t *s, moq_event_kind_t kind,
                       size_t detail_size, bool setup, moq_event_t *ev)
{
    if (!check(t, setup, "event.present", poll_event(t, s, ev, setup), true)) return false;
    bool ok = check(t, setup, "event.kind", ev->kind, kind);
    ok = check(t, setup, "event.detail_covered", ev->detail_size >= detail_size, true) && ok;
    if (!ok) moq_event_cleanup(ev);
    return ok;
}

static void quiet(test_case_t *t, moq_session_t *s, bool setup, bool discard)
{
    for (unsigned i = 0; i < DRAIN_ROUNDS; ++i) {
        moq_event_t ev;
        if (!poll_event(t, s, &ev, setup)) return;
        if (!discard) check(t, setup, "unexpected_event", ev.kind, 0);
        moq_event_cleanup(&ev);
    }
    check(t, setup, "event_drain_bounded", false, true);
}

static bool pump(test_case_t *t, bool setup)
{
    return result(t, setup, "pair.pump",
                  moq_simpair_run_until_quiescent(t->pair, DRAIN_ROUNDS, NULL), MOQ_OK);
}

static bool start_limited_case(test_case_t *t, const char *name, moq_version_t version,
                               uint32_t max_events, uint32_t max_actions)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.initial_now_us = 1;
    cfg.max_events = max_events;
    cfg.max_actions = max_actions;
    if (version == MOQ_VERSION_DRAFT_16) {
        cfg.client_send_request_capacity = true;
        cfg.client_initial_request_capacity = 32;
        cfg.server_send_request_capacity = true;
        cfg.server_initial_request_capacity = 32;
    }
    cfg.trace_fn = trace;
    cfg.trace_ctx = &t->refs;
    if (!result(t, true, "pair.create", moq_simpair_create(&cfg, &t->pair), MOQ_OK) ||
        !check(t, true, "pair.present", t->pair != NULL, true)) return false;
    t->requester = moq_simpair_client(t->pair);
    t->responder = moq_simpair_server(t->pair);
    if (!result(t, true, "pair.start", moq_simpair_start(t->pair), MOQ_OK) ||
        !pump(t, true)) return false;
    moq_session_t *sides[] = {t->requester, t->responder};
    for (size_t i = 0; i < 2; ++i) {
        check(t, true, "selected_version", moq_session_version(sides[i]), version);
        check(t, true, "established", moq_session_state(sides[i]), MOQ_SESS_ESTABLISHED);
        quiet(t, sides[i], true, true);
    }
    return t->setup_failures == 0;
}

static bool start_case(test_case_t *t, const char *name, moq_version_t version,
                       uint32_t max_events)
{
    return start_limited_case(t, name, version, max_events, 0);
}

static bool subscribe_request_named(test_case_t *t, const char *name)
{
    moq_subscribe_namespace_cfg_t cfg;
    moq_subscribe_namespace_cfg_init(&cfg);
    moq_bytes_t prefix[] = {{(const uint8_t *)name, strlen(name)}};
    cfg.track_namespace_prefix = (moq_namespace_t){prefix, 1};
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    t->refs.armed = true;
    moq_result_t rc = moq_session_subscribe_namespace(t->requester, &cfg, 1,
                                                     &t->requester_handle);
    if (!result(t, true, "subscribe", rc, MOQ_OK) || !pump(t, true)) {
        t->refs.armed = false;
        return false;
    }
    t->refs.armed = false;
    check(t, true, "trace.complete", t->refs.short_record, false);
    check(t, true, "trace.opens", t->refs.opens, 1);
    check(t, true, "trace.inputs", t->refs.inputs, 1);
    check(t, true, "trace.request_has_no_fin", t->refs.requester_fin, false);
    check(t, true, "trace.requester_ref", t->refs.requester._v != 0, true);
    check(t, true, "trace.responder_ref", t->refs.responder._v != 0, true);
    check(t, true, "requester.handle", moq_ns_sub_handle_is_valid(t->requester_handle), true);
    moq_event_t ev;
    if (!take_event(t, t->responder, MOQ_EVENT_NS_SUB_REQUEST,
                    sizeof(moq_ns_sub_request_event_t), true, &ev)) return false;
    t->responder_handle = ev.u.ns_sub_request.handle;
    check(t, true, "responder.handle", moq_ns_sub_handle_is_valid(t->responder_handle), true);
    const moq_namespace_t *ns = &ev.u.ns_sub_request.track_namespace_prefix;
    bool match = ns->count == 1 && ns->parts && ns->parts[0].len == strlen(name) &&
                 ns->parts[0].data && memcmp(ns->parts[0].data, name, strlen(name)) == 0;
    check(t, true, "request.prefix", match, true);
    moq_event_cleanup(&ev);
    quiet(t, t->responder, true, false);
    quiet(t, t->requester, true, false);
    printf("TRACE %s: requester=%" PRIu64 " responder=%" PRIu64 "\n",
           t->name, t->refs.requester._v, t->refs.responder._v);
    return t->setup_failures == 0;
}

static bool subscribe_request(test_case_t *t)
{
    return subscribe_request_named(t, "live");
}

static bool accept_request(test_case_t *t, bool setup)
{
    moq_accept_ns_sub_cfg_t cfg;
    moq_accept_ns_sub_cfg_init(&cfg);
    if (!result(t, setup, "accept", moq_session_accept_ns_sub(
            t->responder, t->responder_handle, &cfg, 1), MOQ_OK) || !pump(t, setup))
        return false;
    moq_event_t ev;
    if (!take_event(t, t->requester, MOQ_EVENT_NS_SUB_OK,
                    sizeof(moq_ns_sub_ok_event_t), setup, &ev)) return false;
    bool ok = check(t, setup, "ok.handle",
                    moq_ns_sub_handle_eq(ev.u.ns_sub_ok.handle, t->requester_handle), true);
    moq_event_cleanup(&ev);
    quiet(t, t->requester, setup, false);
    quiet(t, t->responder, setup, false);
    ok = check(t, false, "post_ok.requester_live", moq_session_has_transport_stream(
        t->requester, t->refs.requester), true) && ok;
    ok = check(t, false, "post_ok.responder_live", moq_session_has_transport_stream(
        t->responder, t->refs.responder), true) && ok;
    return ok;
}

static moq_namespace_t suffix(unsigned index, moq_bytes_t *part)
{
    *part = index == 0 ? MOQ_BYTES_LITERAL("a") : MOQ_BYTES_LITERAL("b");
    return (moq_namespace_t){part, 1};
}

static unsigned suffix_bit(const moq_namespace_t *ns)
{
    if (ns->count != 1 || !ns->parts || ns->parts[0].len != 1 || !ns->parts[0].data)
        return 0;
    if (ns->parts[0].data[0] == 'a') return 1;
    if (ns->parts[0].data[0] == 'b') return 2;
    return 0;
}

static bool announce_suffixes(test_case_t *t, unsigned count, bool setup)
{
    for (unsigned i = 0; i < count; ++i) {
        moq_bytes_t part;
        moq_namespace_t ns = suffix(i, &part);
        if (!result(t, setup, "send_namespace", moq_session_send_namespace(
                t->responder, t->responder_handle, &ns, 1), MOQ_OK) || !pump(t, setup))
            return false;
        moq_event_t ev;
        if (!take_event(t, t->requester, MOQ_EVENT_NAMESPACE_FOUND,
                        sizeof(moq_namespace_found_event_t), setup, &ev)) return false;
        check(t, setup, "found.handle", moq_ns_sub_handle_eq(
            ev.u.namespace_found.handle, t->requester_handle), true);
        check(t, setup, "found.suffix",
              suffix_bit(&ev.u.namespace_found.track_namespace_suffix), 1u << i);
        moq_event_cleanup(&ev);
        quiet(t, t->requester, setup, false);
        quiet(t, t->responder, setup, false);
    }
    return t->setup_failures == 0;
}

/* A real PUBLISH remains established on its own stream during NS teardown. */
static bool unrelated_setup(test_case_t *t)
{
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
    moq_bytes_t prefix[] = {MOQ_BYTES_LITERAL("other")};
    cfg.track_namespace = (moq_namespace_t){prefix, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("video");
    if (!result(t, true, "unrelated.publish", moq_session_publish(
            t->requester, &cfg, 1, &t->publisher), MOQ_OK) || !pump(t, true)) return false;
    moq_event_t ev;
    if (!take_event(t, t->responder, MOQ_EVENT_PUBLISH_REQUEST,
                    sizeof(moq_publish_request_event_t), true, &ev)) return false;
    t->subscriber = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t accept;
    moq_accept_publish_cfg_init_sized(&accept, sizeof(accept));
    if (!result(t, true, "unrelated.accept", moq_session_accept_publish(
            t->responder, t->subscriber, &accept, 1), MOQ_OK) || !pump(t, true)) return false;
    if (!take_event(t, t->requester, MOQ_EVENT_PUBLISH_OK,
                    sizeof(moq_publish_ok_event_t), true, &ev)) return false;
    check(t, true, "unrelated.ok_handle",
          moq_publication_eq(ev.u.publish_ok.pub, t->publisher), true);
    moq_event_cleanup(&ev);
    quiet(t, t->requester, true, false);
    quiet(t, t->responder, true, false);
    return t->setup_failures == 0;
}

static void unrelated_survives(test_case_t *t)
{
    moq_finish_publish_cfg_t cfg;
    moq_finish_publish_cfg_init(&cfg);
    cfg.stream_count = 0;
    if (!result(t, false, "unrelated.finish", moq_session_finish_publish(
            t->requester, t->publisher, &cfg, 1), MOQ_OK) || !pump(t, false)) return;
    moq_event_t ev;
    if (take_event(t, t->responder, MOQ_EVENT_PUBLISH_FINISHED,
                   sizeof(moq_publish_finished_event_t), false, &ev)) {
        check(t, false, "unrelated.finished_handle",
              moq_publication_eq(ev.u.publish_finished.pub, t->subscriber), true);
        check(t, false, "unrelated.finished_stream_count", ev.u.publish_finished.stream_count, 0);
        check(t, false, "unrelated.finished_cause", ev.u.publish_finished.cause,
              MOQ_REQUEST_TERMINAL_NONE);
        moq_event_cleanup(&ev);
    }
    quiet(t, t->requester, false, false);
    quiet(t, t->responder, false, false);
}

static moq_result_t signal_peer(moq_session_t *s, moq_stream_ref_t ref,
                                signal_kind_t kind, uint64_t code)
{
    if (kind == SIGNAL_FIN)
        return moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, true, 1);
    if (kind == SIGNAL_RESET) return moq_session_on_bidi_stream_reset(s, ref, code, 1);
    return moq_session_on_bidi_stream_stop(s, ref, code, 1);
}

static bool poll_action(test_case_t *t, moq_session_t *s)
{
    moq_action_t a;
    size_t count = 0;
    if (!result(t, false, "poll_actions",
                moq_session_poll_actions_ex(s, &a, 1, sizeof(a), &count), MOQ_OK) || !count)
        return false;
    check(t, false, "no_session_close_action", a.kind == MOQ_ACTION_CLOSE_SESSION, false);
    if (t->watch_ref._v) {
        if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM &&
            a.u.close_bidi_stream.stream_ref._v == t->watch_ref._v)
            ++t->closes;
        if (a.kind == MOQ_ACTION_RESET_BIDI_STREAM &&
            a.u.reset_bidi_stream.stream_ref._v == t->watch_ref._v) {
            ++t->resets;
            check(t, false, "cancel.reset_code", a.u.reset_bidi_stream.error_code,
                  MOQ_STREAM_ERROR_CANCELLED);
        }
        if (a.kind == MOQ_ACTION_STOP_BIDI_STREAM &&
            a.u.stop_bidi_stream.stream_ref._v == t->watch_ref._v) {
            ++t->stops;
            check(t, false, "cancel.stop_code", a.u.stop_bidi_stream.error_code,
                  MOQ_STREAM_ERROR_CANCELLED);
        }
        check(t, false, "cancel.no_whole_bidi_abort",
              a.kind == MOQ_ACTION_ABORT_BIDI_STREAM, false);
    }
    moq_action_cleanup(&a);
    return true;
}

static void observe(test_case_t *t, observations_t *o, const moq_event_t *ev)
{
    if (ev->kind == MOQ_EVENT_NAMESPACE_GONE) {
        ++o->gone;
        check(t, false, "gone.before_terminal", o->terminals, 0);
        if (!check(t, false, "gone.detail_covered",
                   ev->detail_size >= sizeof(moq_namespace_gone_event_t), true)) {
            mask(t, "short GONE masks handle/suffix", 2);
            return;
        }
        check(t, false, "gone.handle", moq_ns_sub_handle_eq(
            ev->u.namespace_gone.handle, o->handle), true);
        unsigned bit = suffix_bit(&ev->u.namespace_gone.track_namespace_suffix);
        check(t, false, "gone.known_suffix", bit != 0, true);
        check(t, false, "gone.once", (o->gone_mask & bit) != 0, false);
        o->gone_mask |= bit;
        printf("OBS %s: GONE suffix_bit=%u\n", t->name, bit);
        return;
    }
    if (ev->kind != NS_TERMINATED) {
        check(t, false, "terminal.unexpected_kind", ev->kind, NS_TERMINATED);
        return;
    }
    ++o->terminals;
    check(t, false, "terminal.after_all_gone", o->gone, o->expected_gone);
    if (!check(t, false, "terminal.detail_covered", ev->detail_size >= NS_DETAIL_SIZE, true)) {
        mask(t, "short terminal masks typed payload", o->cause_known ? 4 : 3);
        return;
    }
    ++o->full_terminals;
    const moq_ns_subscription_terminated_event_t *d = &ev->u.ns_subscription_terminated;
    check(t, false, "terminal.handle", moq_ns_sub_handle_eq(d->handle, o->handle), true);
    check(t, false, "terminal.role", d->local_role, o->role);
    check(t, false, "terminal.stream_error_code", d->stream_error_code, o->code);
    if (o->cause_known) check(t, false, "terminal.cause", d->cause, o->cause);
    printf("OBS %s: TERMINAL cause=%u role=%u code=%" PRIu64 "\n",
           t->name, d->cause, d->local_role, d->stream_error_code);
}

/* Polling alone must settle retained terminal work; no signal re-feed/timer. */
static void drain(test_case_t *t, moq_session_t *s, observations_t *o)
{
    for (unsigned i = 0; i < DRAIN_ROUNDS; ++i) {
        moq_event_t ev;
        bool had_event = poll_event(t, s, &ev, false);
        if (had_event) {
            observe(t, o, &ev);
            moq_event_cleanup(&ev);
        }
        bool had_action = poll_action(t, s);
        if (i + 1 == DRAIN_ROUNDS)
            check(t, false, "terminal_drain_bounded", had_event || had_action, false);
    }
}

static void observations_done(test_case_t *t, observations_t *o)
{
    check(t, false, "gone.count", o->gone, o->expected_gone);
    check(t, false, "gone.complete_set", o->gone_mask, o->expected_gone == 2 ? 3 : 0);
    check(t, false, "terminal.count", o->terminals, o->expected_terminals);
    if (o->expected_terminals && !o->terminals)
        mask(t, "missing terminal masks typed payload", o->cause_known ? 4 : 3);
}

static bool stale(test_case_t *t, bool requester)
{
    if (requester)
        return result(t, false, "terminal.stale_requester", moq_session_cancel_namespace_sub(
            t->requester, t->requester_handle, 1), MOQ_ERR_STALE_HANDLE);
    moq_bytes_t part = MOQ_BYTES_LITERAL("stale");
    moq_namespace_t ns = {&part, 1};
    return result(t, false, "terminal.stale_responder", moq_session_send_namespace(
        t->responder, t->responder_handle, &ns, 1), MOQ_ERR_STALE_HANDLE);
}

static void end_case(test_case_t *t)
{
    if (t->pair) {
        moq_session_t *sides[] = {t->requester, t->responder};
        for (size_t side = 0; side < 2; ++side) {
            if (!t->setup_failures)
                check(t, false, "end.session_established",
                      moq_session_state(sides[side]), MOQ_SESS_ESTABLISHED);
            for (unsigned i = 0; i < DRAIN_ROUNDS; ++i) {
                if (!poll_action(t, sides[side])) break;
                if (i + 1 == DRAIN_ROUNDS)
                    check(t, false, "action_drain_bounded", false, true);
            }
            quiet(t, sides[side], t->setup_failures != 0, t->setup_failures != 0);
        }
        moq_simpair_destroy(t->pair);
    }
    ++cases;
    if (!t->setup_failures && !t->failures && !t->held) ++passed_cases;
    if (t->setup_failures) ++setup_cases;
    if (t->failures) ++failed_cases;
    failed_checks += t->failures;
    masked_checks += t->masked;
    held_checks += t->held;
    printf("%s %s (behavior_failures=%u setup_failures=%u masked=%u held=%u)\n",
           t->setup_failures ? "SETUP_FAIL" : t->failures ? "FAIL" : "PASS",
           t->name, t->failures, t->setup_failures, t->masked, t->held);
}

static void ordinary_case(moq_version_t version, bool requester, signal_kind_t kind,
                          unsigned suffixes, uint64_t code)
{
    char name[128];
    const char *signal_name = kind == SIGNAL_FIN ? "fin" : kind == SIGNAL_RESET ? "reset" : "stop";
    const char *draft = version == MOQ_VERSION_DRAFT_22 ? "d22" :
                        version == MOQ_VERSION_DRAFT_18 ? "d18" : "d16";
    snprintf(name, sizeof(name), "%s.%s.%s.%s.suffix%u", draft,
             requester ? "requester" : "responder", signal_name, code ? "max" : "zero", suffixes);
    test_case_t t;
    if (start_case(&t, name, version, 0) && subscribe_request(&t) &&
        accept_request(&t, true) && announce_suffixes(&t, suffixes, true) && unrelated_setup(&t)) {
        bool d22 = version == MOQ_VERSION_DRAFT_22;
        moq_session_t *s = requester ? t.requester : t.responder;
        moq_stream_ref_t ref = requester ? t.refs.requester : t.refs.responder;
        observations_t o = {
            .handle = requester ? t.requester_handle : t.responder_handle,
            .expected_gone = requester && (d22 || kind != SIGNAL_STOP) ? suffixes : 0,
            .expected_terminals = d22 ? 1 : 0,
            .cause = kind == SIGNAL_FIN ? MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE :
                     kind == SIGNAL_RESET ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM :
                     MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING,
            .role = requester ? MOQ_REQUEST_ROLE_REQUESTER : MOQ_REQUEST_ROLE_RESPONDER,
            .code = code,
            .cause_known = true
        };
        result(&t, false, "peer.signal", signal_peer(s, ref, kind, code), MOQ_OK);
        drain(&t, s, &o);
        observations_done(&t, &o);
        bool committed = o.terminals == 1 && o.full_terminals == 1;
        if (!d22 || committed) (void)stale(&t, requester);
        else mask(&t, "no terminal commitment masks stale-after-terminal", 1);
        if (d22 && kind == SIGNAL_STOP) {
            bool retained = moq_session_has_transport_stream(s, ref);
            check(&t, false, "stop.retains_peer_receive", retained, true);
            if (retained && committed) {
                result(&t, false, "stop.later_peer_end", signal_peer(s, ref,
                    code == 0 ? SIGNAL_FIN : SIGNAL_RESET, UINT64_MAX), MOQ_OK);
                observations_t after = o;
                after.expected_gone = 0;
                after.expected_terminals = 0;
                after.gone = after.gone_mask = after.terminals = after.full_terminals = 0;
                drain(&t, s, &after);
                observations_done(&t, &after);
                check(&t, false, "stop.peer_end_retires", moq_session_has_transport_stream(s, ref), false);
                (void)stale(&t, requester);
            } else {
                mask(&t, "no retained committed owner masks late FIN/RESET, retirement and no duplicate", 3);
            }
        } else {
            check(&t, false, "peer_end.retires_transport_owner",
                  moq_session_has_transport_stream(s, ref), false);
        }
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void requester_fin_case(bool before_accept)
{
    test_case_t t;
    const char *name = before_accept ? "d22.requester_fin.delayed_response" :
                                      "d22.requester_fin.established";
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 0) && subscribe_request(&t) &&
        (before_accept || accept_request(&t, true)) && unrelated_setup(&t)) {
        result(&t, false, "requester.fin", signal_peer(t.responder, t.refs.responder, SIGNAL_FIN, 0), MOQ_OK);
        observations_t o = {.handle = t.responder_handle};
        drain(&t, t.responder, &o);
        observations_done(&t, &o);
        bool live = check(&t, false, "requester_fin.keeps_response_owner",
                          moq_session_has_transport_stream(t.responder, t.refs.responder), true);
        if (live && (!before_accept || accept_request(&t, false)))
            (void)announce_suffixes(&t, 2, false);
        else mask(&t, "lost response owner masks delayed OK and namespace observations", before_accept ? 3 : 2);
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void missing_response_case(bool truncated)
{
    test_case_t t;
    const char *name = truncated ? "d22.truncated_initial_response.fin" :
                                  "d22.missing_initial_response.fin";
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 0) &&
        subscribe_request(&t) && unrelated_setup(&t)) {
        observations_t o = {
            .handle = t.requester_handle, .expected_terminals = 1,
            .cause = truncated ? MOQ_REQUEST_TERMINAL_TRUNCATED_RESPONSE :
                                 MOQ_REQUEST_TERMINAL_MISSING_RESPONSE,
            .role = MOQ_REQUEST_ROLE_REQUESTER, .cause_known = true
        };
        /* The first byte of a two-byte message-type varint is incomplete. */
        const uint8_t partial[] = {0x40};
        result(&t, false, "missing_response.fin", moq_session_on_bidi_stream_bytes(
            t.requester, t.refs.requester, truncated ? partial : NULL,
            truncated ? sizeof(partial) : 0, true, 1), MOQ_OK);
        drain(&t, t.requester, &o);
        observations_done(&t, &o);
        if (o.terminals == 1 && o.full_terminals == 1) (void)stale(&t, true);
        else mask(&t, "missing terminal masks stale-after-terminal", 1);
        check(&t, false, "missing_response.peer_end_retires",
              moq_session_has_transport_stream(t.requester, t.refs.requester), false);
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void crossed_case(bool reset_first)
{
    test_case_t t;
    const char *name = reset_first ? "d22.one_event.reset0_stop_max.suffix2" :
                                    "d22.one_event.stop0_reset_max.suffix2";
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 1) && subscribe_request(&t) &&
        accept_request(&t, true) && announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        observations_t o = {
            .handle = t.requester_handle, .expected_gone = 2, .expected_terminals = 1,
            .cause = reset_first ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM : MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING,
            .role = MOQ_REQUEST_ROLE_REQUESTER, .cause_known = true
        };
        result(&t, false, "first_signal.pending_suffix_work", signal_peer(t.requester,
            t.refs.requester, reset_first ? SIGNAL_RESET : SIGNAL_STOP, 0), MOQ_ERR_WOULD_BLOCK);
        moq_result_t crossed = signal_peer(t.requester, t.refs.requester,
                                          reset_first ? SIGNAL_STOP : SIGNAL_RESET, UINT64_MAX);
        check(&t, false, "crossed_signal.accepted_or_retained",
              crossed == MOQ_OK || crossed == MOQ_ERR_WOULD_BLOCK, true);
        drain(&t, t.requester, &o);
        observations_done(&t, &o);
        if (o.terminals == 1 && o.full_terminals == 1) (void)stale(&t, true);
        else mask(&t, "missing terminal masks stale-after-terminal", 1);
        check(&t, false, "crossed_reset.retires_transport_owner",
              moq_session_has_transport_stream(t.requester, t.refs.requester), false);
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void action_pressure_case(bool requester, bool cross_reset)
{
    char name[96];
    snprintf(name, sizeof(name), "d22.action_pressure.%s.%s",
             requester ? "requester" : "responder", cross_reset ? "cross_reset" : "stop_fin");
    test_case_t t;
    if (start_limited_case(&t, name, MOQ_VERSION_DRAFT_22, 0, 1) &&
        subscribe_request(&t) && accept_request(&t, true) &&
        announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        moq_session_t *s = requester ? t.requester : t.responder;
        moq_stream_ref_t ref = requester ? t.refs.requester : t.refs.responder;
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        moq_bytes_t prefix = MOQ_BYTES_LITERAL("pressure");
        cfg.track_namespace = (moq_namespace_t){&prefix, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("pending");
        moq_publication_t pending;
        if (result(&t, true, "fill_action_queue", moq_session_publish(s, &cfg, 1, &pending), MOQ_OK)) {
            t.watch_ref = ref;
            observations_t o = {
                .handle = requester ? t.requester_handle : t.responder_handle,
                .expected_gone = requester ? 2 : 0, .expected_terminals = 1,
                .cause = MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING,
                .role = requester ? MOQ_REQUEST_ROLE_REQUESTER : MOQ_REQUEST_ROLE_RESPONDER,
                .cause_known = true
            };
            result(&t, false, "stop.owed_local_output", signal_peer(s, ref, SIGNAL_STOP, 0),
                   MOQ_ERR_WOULD_BLOCK);
            (void)stale(&t, requester); /* terminal committed, action slot still full */
            check(&t, false, "output_pressure.retains_owner", moq_session_has_transport_stream(s, ref), true);
            if (cross_reset) {
                result(&t, false, "reset.local_output_still_owed",
                       signal_peer(s, ref, SIGNAL_RESET, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
                check(&t, false, "reset.retains_owed_output_owner",
                      moq_session_has_transport_stream(s, ref), true);
            }
            drain(&t, s, &o);
            observations_done(&t, &o);
            check(&t, false, "cancel.reset_once", t.resets, 1);
            check(&t, false, "cancel.stop_only_if_receive_open", t.stops, cross_reset ? 0 : 1);
            check(&t, false, "receive_owner.after_output", moq_session_has_transport_stream(s, ref), !cross_reset);
            if (!cross_reset) {
                const uint8_t discarded[] = {0xff};
                result(&t, false, "stop.discards_late_bytes", moq_session_on_bidi_stream_bytes(
                    s, ref, discarded, sizeof(discarded), false, 1), MOQ_OK);
                result(&t, false, "stop.later_fin", signal_peer(s, ref, SIGNAL_FIN, 0), MOQ_OK);
            }
            /* A late duplicate/crossed signal must not recreate a terminal. */
            result(&t, false, "late.stop", signal_peer(s, ref, SIGNAL_STOP, UINT64_MAX), MOQ_OK);
            result(&t, false, "late.reset", signal_peer(s, ref, SIGNAL_RESET, 0), MOQ_OK);
            observations_t after = {.handle = o.handle};
            drain(&t, s, &after);
            observations_done(&t, &after);
            check(&t, false, "physical_end.retires_owner", moq_session_has_transport_stream(s, ref), false);
            (void)stale(&t, requester);
            t.watch_ref._v = 0;
            unrelated_survives(&t);
        }
    }
    end_case(&t);
}

static void reuse_case(bool retained_receive)
{
    test_case_t t;
    const char *name = retained_receive ? "d22.stop.retained_prefix_replacement" :
                                         "d22.reset_stop.slot_reuse";
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 0) &&
        subscribe_request(&t) && accept_request(&t, true) &&
        announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        moq_ns_sub_handle_t old[] = {t.requester_handle, t.responder_handle};
        moq_session_t *sides[] = {t.requester, t.responder};
        moq_stream_ref_t refs[] = {t.refs.requester, t.refs.responder};
        for (unsigned i = 0; i < 2; ++i) {
            observations_t o = {
                .handle = old[i], .expected_gone = i == 0 ? 2 : 0, .expected_terminals = 1,
                .cause = retained_receive ? MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING :
                                            MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM,
                .role = i == 0 ? MOQ_REQUEST_ROLE_REQUESTER : MOQ_REQUEST_ROLE_RESPONDER,
                .cause_known = true
            };
            result(&t, false, "old.cancel", signal_peer(sides[i], refs[i],
                retained_receive ? SIGNAL_STOP : SIGNAL_RESET, 0), MOQ_OK);
            drain(&t, sides[i], &o);
            observations_done(&t, &o);
            check(&t, false, "old.receive_owner", moq_session_has_transport_stream(sides[i], refs[i]),
                  retained_receive);
        }
        memset(&t.refs, 0, sizeof(t.refs));
        if (subscribe_request(&t) && accept_request(&t, false) && announce_suffixes(&t, 2, false)) {
            check(&t, false, "reuse.requester_generation", moq_ns_sub_handle_eq(old[0], t.requester_handle), false);
            check(&t, false, "reuse.responder_generation", moq_ns_sub_handle_eq(old[1], t.responder_handle), false);
            result(&t, false, "reuse.old_requester_stale",
                   moq_session_cancel_namespace_sub(t.requester, old[0], 1), MOQ_ERR_STALE_HANDLE);
            moq_bytes_t part = MOQ_BYTES_LITERAL("old");
            moq_namespace_t ns = {&part, 1};
            result(&t, false, "reuse.old_responder_stale",
                   moq_session_send_namespace(t.responder, old[1], &ns, 1), MOQ_ERR_STALE_HANDLE);
            if (retained_receive) {
                for (unsigned i = 0; i < 2; ++i) {
                    result(&t, false, "old.late_reset", signal_peer(sides[i], refs[i], SIGNAL_RESET, UINT64_MAX), MOQ_OK);
                    observations_t after = {0};
                    drain(&t, sides[i], &after);
                    observations_done(&t, &after);
                    check(&t, false, "old.receive_owner_retires", moq_session_has_transport_stream(sides[i], refs[i]), false);
                }
            }
            observations_t o = {
                .handle = t.requester_handle, .expected_gone = 2, .expected_terminals = 1,
                .cause = MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING,
                .role = MOQ_REQUEST_ROLE_REQUESTER, .code = UINT64_MAX, .cause_known = true
            };
            result(&t, false, "reuse.fresh_stop", signal_peer(t.requester, t.refs.requester,
                                                            SIGNAL_STOP, UINT64_MAX), MOQ_OK);
            drain(&t, t.requester, &o);
            observations_done(&t, &o);
            (void)stale(&t, true);
        }
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void event_pressure_case(bool requester, bool reset_first)
{
    char name[96];
    snprintf(name, sizeof(name), "d22.full_event.%s.%s", requester ? "requester" : "responder",
             reset_first ? "reset0_stop_max" : "stop0_reset_max");
    test_case_t t;
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 1) && subscribe_request(&t) &&
        accept_request(&t, true) && announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        moq_session_t *s = requester ? t.requester : t.responder;
        moq_session_t *peer = requester ? t.responder : t.requester;
        moq_stream_ref_t ref = requester ? t.refs.requester : t.refs.responder;
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        moq_bytes_t prefix = MOQ_BYTES_LITERAL("event-pressure");
        cfg.track_namespace = (moq_namespace_t){&prefix, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("pending");
        moq_publication_t pending;
        if (result(&t, true, "blocker.publish", moq_session_publish(peer, &cfg, 1, &pending), MOQ_OK) &&
            pump(&t, true)) {
            result(&t, false, "first_signal.full_event", signal_peer(s, ref,
                reset_first ? SIGNAL_RESET : SIGNAL_STOP, 0), MOQ_ERR_WOULD_BLOCK);
            if (requester) {
                result(&t, false, "uncommitted.handle_not_stale", moq_session_cancel_namespace_sub(
                    s, t.requester_handle, 1), MOQ_ERR_WRONG_STATE);
            } else {
                moq_bytes_t part;
                moq_namespace_t ns = suffix(0, &part);
                result(&t, false, "uncommitted.handle_not_stale", moq_session_send_namespace(
                    s, t.responder_handle, &ns, 1), MOQ_ERR_WRONG_STATE);
            }
            result(&t, false, "crossed_signal.full_event", signal_peer(s, ref,
                reset_first ? SIGNAL_STOP : SIGNAL_RESET, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
            moq_event_t blocker;
            if (take_event(&t, s, MOQ_EVENT_PUBLISH_REQUEST, sizeof(moq_publish_request_event_t),
                           false, &blocker)) moq_event_cleanup(&blocker);
            observations_t o = {
                .handle = requester ? t.requester_handle : t.responder_handle,
                .expected_gone = requester ? 2 : 0, .expected_terminals = 1,
                .cause = reset_first ? MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM :
                                      MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING,
                .role = requester ? MOQ_REQUEST_ROLE_REQUESTER : MOQ_REQUEST_ROLE_RESPONDER,
                .cause_known = true
            };
            drain(&t, s, &o);
            observations_done(&t, &o);
            (void)stale(&t, requester);
            check(&t, false, "crossed.physical_owner_retires", moq_session_has_transport_stream(s, ref), false);
            unrelated_survives(&t);
        }
    }
    end_case(&t);
}

static void migration_case(bool requester, bool fin)
{
    test_case_t t;
    const char *name = fin ? (requester ? "d22.migrated_requester.fin" : "d22.migrated_responder.fin") :
        (requester ? "d22.migrated_requester.stop_reset" : "d22.migrated_responder.stop_reset");
    if (start_case(&t, name, MOQ_VERSION_DRAFT_22, 0) && subscribe_request(&t) &&
        accept_request(&t, true) && announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        moq_session_t *s = requester ? t.requester : t.responder;
        moq_stream_ref_t ref = requester ? t.refs.requester : t.refs.responder;
        moq_ns_sub_handle_t handle = requester ? t.requester_handle : t.responder_handle;
        moq_request_goaway_cfg_t cfg;
        moq_request_goaway_cfg_init(&cfg);
        result(&t, false, "own.goaway", moq_session_request_goaway_ns_sub(s, handle, &cfg, 1), MOQ_OK);
        check(&t, false, "own.goaway_action", poll_action(&t, s), true);
        result(&t, false, "migrated.peer_signal", signal_peer(s, ref, fin ? SIGNAL_FIN : SIGNAL_STOP, 0), MOQ_OK);
        observations_t o = {.handle = handle};
        drain(&t, s, &o);
        observations_done(&t, &o);
        (void)stale(&t, requester);
        check(&t, false, "migrated.receive_owner", moq_session_has_transport_stream(s, ref), !fin);
        result(&t, false, "migrated.reset", signal_peer(s, ref, SIGNAL_RESET, UINT64_MAX), MOQ_OK);
        drain(&t, s, &o);
        observations_done(&t, &o);
        check(&t, false, "migrated.reset_retires", moq_session_has_transport_stream(s, ref), false);
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void rejection_case(bool fin)
{
    test_case_t t;
    if (start_case(&t, fin ? "d22.rejected.split_fin.complete" : "d22.rejected.split_fin.stop_reset",
                   MOQ_VERSION_DRAFT_22, 0) &&
        subscribe_request(&t) && unrelated_setup(&t)) {
        moq_reject_ns_sub_cfg_t cfg;
        moq_reject_ns_sub_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
        result(&t, false, "reject", moq_session_reject_ns_sub(t.responder, t.responder_handle, &cfg, 1), MOQ_OK);
        moq_action_t a;
        size_t n = 0;
        result(&t, false, "reject.poll_action", moq_session_poll_actions_ex(
            t.responder, &a, 1, sizeof(a), &n), MOQ_OK);
        if (check(&t, false, "reject.action_present", n, 1)) {
            if (check(&t, false, "reject.send_bidi", a.kind, MOQ_ACTION_SEND_BIDI_STREAM)) {
                check(&t, false, "reject.has_fin", a.u.send_bidi_stream.fin, true);
                result(&t, false, "reject.deliver_without_fin", moq_session_on_bidi_stream_bytes(
                    t.requester, t.refs.requester, a.u.send_bidi_stream.data,
                    a.u.send_bidi_stream.len, false, 1), MOQ_OK);
            }
            moq_action_cleanup(&a);
        }
        moq_event_t ev;
        if (take_event(&t, t.requester, MOQ_EVENT_NS_SUB_ERROR, sizeof(moq_ns_sub_error_event_t), false, &ev))
            moq_event_cleanup(&ev);
        moq_session_t *sides[] = {t.requester, t.responder};
        moq_stream_ref_t refs[] = {t.refs.requester, t.refs.responder};
        for (unsigned i = 0; i < 2; ++i) {
            result(&t, false, "rejected.peer_signal", signal_peer(sides[i], refs[i], fin ? SIGNAL_FIN : SIGNAL_STOP, 0), MOQ_OK);
            observations_t o = {0};
            drain(&t, sides[i], &o);
            observations_done(&t, &o);
            check(&t, false, "rejected.receive_owner", moq_session_has_transport_stream(sides[i], refs[i]), !fin);
            result(&t, false, "rejected.reset", signal_peer(sides[i], refs[i], SIGNAL_RESET, UINT64_MAX), MOQ_OK);
            drain(&t, sides[i], &o);
            observations_done(&t, &o);
            check(&t, false, "rejected.reset_retires", moq_session_has_transport_stream(sides[i], refs[i]), false);
        }
        unrelated_survives(&t);
    }
    end_case(&t);
}

static void fin_pressure_case(unsigned suffixes, bool full_event, signal_kind_t crossed)
{
    char name[128];
    snprintf(name, sizeof(name), "d22.fin_pressure.suffix%u.event%u.%s", suffixes, full_event,
             crossed == SIGNAL_FIN ? "graceful" : crossed == SIGNAL_RESET ? "reset" : "stop");
    test_case_t t;
    if (start_limited_case(&t, name, MOQ_VERSION_DRAFT_22, full_event ? 1 : 0, 1) &&
        subscribe_request(&t) && accept_request(&t, true) &&
        announce_suffixes(&t, suffixes, true) && unrelated_setup(&t)) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        moq_bytes_t part = MOQ_BYTES_LITERAL("fin-pressure");
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("pending");
        moq_publication_t pending;
        bool ready = true;
        if (full_event)
            ready = result(&t, true, "blocker.publish", moq_session_publish(
                t.responder, &cfg, 1, &pending), MOQ_OK) && pump(&t, true);
        if (ready && result(&t, true, "fill_action_queue", moq_session_publish(
                t.requester, &cfg, 1, &pending), MOQ_OK)) {
            t.watch_ref = t.refs.requester;
            observations_t o = {
                .handle = t.requester_handle, .expected_gone = suffixes, .expected_terminals = 1,
                .cause = MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE,
                .role = MOQ_REQUEST_ROLE_REQUESTER, .cause_known = true
            };
            result(&t, false, "fin.output_pending", signal_peer(t.requester,
                t.refs.requester, SIGNAL_FIN, 0), MOQ_ERR_WOULD_BLOCK);
            result(&t, false, "fin.handle_commit_boundary", moq_session_cancel_namespace_sub(
                t.requester, t.requester_handle, 1), full_event ? MOQ_ERR_WRONG_STATE : MOQ_ERR_STALE_HANDLE);
            check(&t, false, "fin.retained_output_owner", moq_session_has_transport_stream(
                t.requester, t.refs.requester), true);
            if (crossed != SIGNAL_FIN)
                result(&t, false, "crossed.output_pending", signal_peer(t.requester,
                    t.refs.requester, crossed, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
            if (full_event) {
                moq_event_t blocker;
                if (take_event(&t, t.requester, MOQ_EVENT_PUBLISH_REQUEST,
                        sizeof(moq_publish_request_event_t), false, &blocker))
                    moq_event_cleanup(&blocker);
            }
            drain(&t, t.requester, &o);
            observations_done(&t, &o);
            (void)stale(&t, true);
            check(&t, false, "fin.local_close_once", t.closes, crossed == SIGNAL_FIN ? 1 : 0);
            check(&t, false, "fin.reset_only_after_cancellation", t.resets, crossed == SIGNAL_FIN ? 0 : 1);
            check(&t, false, "fin.no_stop_closed_peer", t.stops, 0);
            check(&t, false, "fin.transport_retired", moq_session_has_transport_stream(
                t.requester, t.refs.requester), false);
            result(&t, false, "late.stop", signal_peer(t.requester, t.refs.requester, SIGNAL_STOP, 0), MOQ_OK);
            result(&t, false, "late.reset", signal_peer(t.requester, t.refs.requester, SIGNAL_RESET, UINT64_MAX), MOQ_OK);
            observations_t after = {0};
            drain(&t, t.requester, &after);
            observations_done(&t, &after);
            t.watch_ref._v = 0;
            unrelated_survives(&t);
        }
    }
    end_case(&t);
}

static void fin_reuse_case(void)
{
    test_case_t t;
    if (start_case(&t, "d22.fin.requester_slot_reuse", MOQ_VERSION_DRAFT_22, 0) &&
        subscribe_request(&t) && accept_request(&t, true) &&
        announce_suffixes(&t, 2, true) && unrelated_setup(&t)) {
        const moq_ns_sub_handle_t old = t.requester_handle;
        const moq_stream_ref_t old_ref = t.refs.requester;
        observations_t o = {
            .handle = old, .expected_gone = 2, .expected_terminals = 1,
            .cause = MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE,
            .role = MOQ_REQUEST_ROLE_REQUESTER, .cause_known = true
        };
        result(&t, false, "old.fin", signal_peer(t.requester, old_ref, SIGNAL_FIN, 0), MOQ_OK);
        drain(&t, t.requester, &o);
        observations_done(&t, &o);
        (void)stale(&t, true);
        memset(&t.refs, 0, sizeof(t.refs));
        if (subscribe_request_named(&t, "replacement") && accept_request(&t, false) &&
            announce_suffixes(&t, 2, false)) {
            check(&t, false, "reuse.generation", moq_ns_sub_handle_eq(old, t.requester_handle), false);
            result(&t, false, "reuse.old_stale", moq_session_cancel_namespace_sub(t.requester, old, 1),
                   MOQ_ERR_STALE_HANDLE);
            result(&t, false, "reuse.old_reset", signal_peer(t.requester, old_ref, SIGNAL_RESET, UINT64_MAX), MOQ_OK);
            result(&t, false, "reuse.old_stop", signal_peer(t.requester, old_ref, SIGNAL_STOP, 0), MOQ_OK);
            observations_t after = {0};
            drain(&t, t.requester, &after);
            observations_done(&t, &after);
            o = (observations_t){
                .handle = t.requester_handle, .expected_gone = 2, .expected_terminals = 1,
                .cause = MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM, .code = UINT64_MAX,
                .role = MOQ_REQUEST_ROLE_REQUESTER, .cause_known = true
            };
            result(&t, false, "reuse.new_reset", signal_peer(t.requester, t.refs.requester,
                SIGNAL_RESET, UINT64_MAX), MOQ_OK);
            drain(&t, t.requester, &o);
            observations_done(&t, &o);
            (void)stale(&t, true);
        }
        unrelated_survives(&t);
    }
    end_case(&t);
}

int main(int argc, char **argv)
{
    bool full = argc == 1;
    bool focused = argc == 2 && strcmp(argv[1], "--reset-stop") == 0;
    bool legacy_only = argc == 2 && strcmp(argv[1], "--legacy") == 0;
    bool fin_only = argc == 2 && strcmp(argv[1], "--fin") == 0;
    if (!full && !focused && !legacy_only && !fin_only) {
        fprintf(stderr, "usage: %s [--reset-stop|--legacy|--fin]\n", argv[0]);
        return 2;
    }
    if (!legacy_only) {
        requester_fin_case(true);
        requester_fin_case(false);
        missing_response_case(false);
        missing_response_case(true);
    }
    for (unsigned suffixes = 0; suffixes <= 2; suffixes += 2) {
        if (full || fin_only) ordinary_case(MOQ_VERSION_DRAFT_22, true, SIGNAL_FIN, suffixes, 0);
        if (!legacy_only && !fin_only)
        for (unsigned requester = 0; requester < 2; ++requester)
            for (unsigned reset = 0; reset < 2; ++reset)
                for (unsigned maximum = 0; maximum < 2; ++maximum)
                    ordinary_case(MOQ_VERSION_DRAFT_22, requester != 0,
                                  reset ? SIGNAL_RESET : SIGNAL_STOP, suffixes,
                                  maximum ? UINT64_MAX : 0);
        const moq_version_t legacy[] = {MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18};
        for (size_t v = 0; !focused && !fin_only && v < sizeof(legacy) / sizeof(legacy[0]); ++v) {
            ordinary_case(legacy[v], true, SIGNAL_FIN, suffixes, 0);
            ordinary_case(legacy[v], true, SIGNAL_RESET, suffixes, UINT64_MAX);
            ordinary_case(legacy[v], true, SIGNAL_STOP, suffixes, 0);
        }
    }
    if (full || focused) {
        crossed_case(true);
        crossed_case(false);
        for (unsigned requester = 0; requester < 2; ++requester) {
            action_pressure_case(requester != 0, false);
            action_pressure_case(requester != 0, true);
            event_pressure_case(requester != 0, false);
            event_pressure_case(requester != 0, true);
            migration_case(requester != 0, false);
        }
        reuse_case(false);
        reuse_case(true);
        rejection_case(false);
    }
    if (full || fin_only) {
        for (unsigned suffixes = 0; suffixes <= 2; suffixes += 2)
            for (unsigned full_event = 0; full_event < 2; ++full_event)
                for (unsigned crossed = SIGNAL_FIN; crossed <= SIGNAL_STOP; ++crossed)
                    fin_pressure_case(suffixes, full_event != 0, (signal_kind_t)crossed);
        fin_reuse_case();
        migration_case(false, true);
        migration_case(true, true);
        rejection_case(true);
    }
    printf("SUMMARY cases=%u passed=%u behavior_failed_cases=%u setup_failed_cases=%u "
           "behavior_failures=%u masked_checks=%u held_checks=%u\n",
           cases, passed_cases, failed_cases, setup_cases,
           failed_checks, masked_checks, held_checks);
    unsigned expected = full ? 65 : focused ? 35 : fin_only ? 22 : 12;
    if (cases != expected) {
        fprintf(stderr, "case inventory mismatch: expected=%u actual=%u\n", expected, cases);
        return 2;
    }
    return failed_cases || setup_cases || held_checks ? 1 : 0;
}
