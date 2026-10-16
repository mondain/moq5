/* Ordinary announcement lifecycle only. Stream refs come from public SimPair
 * traces; FIN/RESET/STOP enter through the public transport-input functions. */
#include <moq/moq.h>
#include <moq/sim.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(MOQ_EVENT_NAMESPACE_TERMINATED == 57u, "typed announcement kind");
_Static_assert(sizeof(moq_namespace_terminated_event_t) == 24, "frozen detail");
_Static_assert(offsetof(moq_namespace_terminated_event_t, ann) == 0, "handle");
_Static_assert(offsetof(moq_namespace_terminated_event_t, cause) == 8, "cause");
_Static_assert(offsetof(moq_namespace_terminated_event_t, local_role) == 12, "role");
_Static_assert(offsetof(moq_namespace_terminated_event_t, stream_error_code) == 16,
               "stream error");
_Static_assert(sizeof(moq_namespace_terminal_cause_t) == 4, "fixed cause width");
_Static_assert(sizeof(moq_namespace_role_t) == 4, "fixed role width");

static unsigned checks, failures, cases, setup_failures;
static const uint8_t namespace_bytes[] = "terminal-0001";

typedef struct {
    unsigned setup, published, accepted, rejected, done, cancelled, terminated;
    unsigned accepted_at, rejected_at, terminal_at, goaway;
    moq_namespace_terminated_event_t terminal;
} observation_t;

typedef struct {
    const char *name;
    const char *phase;
    unsigned failures_before;
    bool setup_ok, capture_request, capture_withdrawal;
    bool expect_rejection, expect_goaway;
    unsigned event_serial;
    moq_simpair_t *pair;
    moq_session_t *session[2];
    moq_announcement_t ann[2];
    moq_stream_ref_t ref[2];
    unsigned opens, request_inputs, withdraw_resets, withdraw_stops;
    uint32_t withdrawal_cause;
    uint64_t withdrawal_code;
    observation_t seen[2];
} fixture_t;

static bool check(fixture_t *f, bool valid, const char *label)
{
    ++checks;
    if (!valid) {
        ++failures;
        fprintf(stderr, "FAIL %s phase=%s check=%s\n", f->name, f->phase, label);
    }
    return valid;
}

static bool value(fixture_t *f, const char *label, uint64_t actual, uint64_t expected)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "FAIL %s phase=%s check=%s actual=%" PRIu64
                " expected=%" PRIu64 "\n", f->name, f->phase, label, actual, expected);
        return false;
    }
    return true;
}

static bool result(fixture_t *f, const char *label, moq_result_t actual,
                   moq_result_t expected)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "FAIL %s phase=%s check=%s actual=%d expected=%d\n",
                f->name, f->phase, label, (int)actual, (int)expected);
        return false;
    }
    return true;
}

static uint64_t now(const fixture_t *f) { return f->pair ? moq_simpair_now_us(f->pair) : 1000; }

#define TRACE_HAS(r, field) \
    ((r)->struct_size >= offsetof(moq_sim_trace_record_t, field) + sizeof((r)->field))

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (f->capture_request && r->kind == MOQ_SIM_TRACE_ACTION &&
        r->from == MOQ_PERSPECTIVE_CLIENT && r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        if (!check(f, TRACE_HAS(r, stream_ref) && TRACE_HAS(r, fin), "open.trace.extent")) return;
        f->ref[0] = r->stream_ref;
        f->opens++;
        check(f, !r->fin, "announcement.request.direction.open");
    }
    if (f->capture_request && r->kind == MOQ_SIM_TRACE_INPUT &&
        r->to == MOQ_PERSPECTIVE_SERVER && r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES) {
        if (!check(f, TRACE_HAS(r, stream_ref), "input.trace.extent")) return;
        if (f->request_inputs)
            value(f, "stable.receiver.ref", r->stream_ref._v, f->ref[1]._v);
        f->ref[1] = r->stream_ref;
        f->request_inputs++;
    }
    if (!f->capture_withdrawal) return;
    if (!check(f, TRACE_HAS(r, stream_ref) && TRACE_HAS(r, error_code),
               "withdrawal.trace.extent")) return;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->stream_ref._v == f->ref[0]._v) {
        if (r->action_kind == MOQ_ACTION_RESET_BIDI_STREAM ||
            r->action_kind == MOQ_ACTION_ABORT_BIDI_STREAM) {
            f->withdraw_resets++;
            value(f, "withdrawal.reset.code", r->error_code, 1);
        }
        if (r->action_kind == MOQ_ACTION_STOP_BIDI_STREAM ||
            r->action_kind == MOQ_ACTION_ABORT_BIDI_STREAM) {
            f->withdraw_stops++;
            value(f, "withdrawal.stop.code", r->error_code, 1);
        }
    }
    if (r->kind == MOQ_SIM_TRACE_INPUT && r->to == MOQ_PERSPECTIVE_SERVER &&
        r->stream_ref._v == f->ref[1]._v && f->withdrawal_cause == 0 &&
        (r->input_kind == MOQ_SIM_INPUT_BIDI_RESET || r->input_kind == MOQ_SIM_INPUT_BIDI_STOP)) {
        f->withdrawal_cause = r->input_kind == MOQ_SIM_INPUT_BIDI_RESET
            ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING;
        f->withdrawal_code = r->error_code;
    }
}

static bool collect(fixture_t *f, unsigned side)
{
    observation_t *o = &f->seen[side];
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t e;
        if (moq_session_poll_events(f->session[side], &e, 1) == 0) return true;
        unsigned serial = ++f->event_serial;
        switch (e.kind) {
        case MOQ_EVENT_SETUP_COMPLETE:
            o->setup++;
            break;
        case MOQ_EVENT_NAMESPACE_PUBLISHED:
            o->published++;
            check(f, side == 1, "published.responder");
            if (check(f, e.detail_size >= sizeof(e.u.namespace_published), "published.extent")) {
                const moq_namespace_t *ns = &e.u.namespace_published.track_namespace;
                if (side == 1) f->ann[1] = e.u.namespace_published.ann;
                if (check(f, ns->count == 1 && ns->parts != NULL, "published.namespace.count") &&
                    check(f, ns->parts[0].len == sizeof(namespace_bytes) - 1 &&
                             ns->parts[0].data != NULL, "published.namespace.length"))
                    check(f, memcmp(ns->parts[0].data, namespace_bytes,
                                    sizeof(namespace_bytes) - 1) == 0, "published.namespace.bytes");
            }
            break;
        case MOQ_EVENT_NAMESPACE_ACCEPTED:
            o->accepted++;
            if (o->accepted == 1) o->accepted_at = serial;
            check(f, side == 0, "accepted.requester");
            if (check(f, e.detail_size >= sizeof(e.u.namespace_accepted), "accepted.extent"))
                check(f, moq_announcement_eq(e.u.namespace_accepted.ann, f->ann[0]), "accepted.identity");
            break;
        case MOQ_EVENT_NAMESPACE_TERMINATED:
            o->terminated++;
            if (o->terminated == 1) o->terminal_at = serial;
            if (value(f, "terminal.detail_size", e.detail_size, 24)) {
                if (o->terminated == 1) o->terminal = e.u.namespace_terminated;
                check(f, moq_announcement_eq(e.u.namespace_terminated.ann, f->ann[side]),
                      "terminal.identity");
            }
            break;
        case MOQ_EVENT_NAMESPACE_DONE:
            o->done++;
            if (value(f, "legacy.done.extent", e.detail_size, sizeof(moq_namespace_done_event_t)))
                check(f, moq_announcement_eq(e.u.namespace_done.ann, f->ann[side]), "legacy.done.identity");
            break;
        case MOQ_EVENT_NAMESPACE_CANCELLED:
            o->cancelled++;
            if (check(f, e.detail_size >= sizeof(moq_namespace_cancelled_event_t), "legacy.cancel.extent"))
                check(f, moq_announcement_eq(e.u.namespace_cancelled.ann, f->ann[side]), "legacy.cancel.identity");
            break;
        case MOQ_EVENT_NAMESPACE_REJECTED:
            o->rejected++;
            if (o->rejected == 1) o->rejected_at = serial;
            if (f->expect_rejection &&
                check(f, e.detail_size >= sizeof(e.u.namespace_rejected), "rejection.extent")) {
                const moq_namespace_rejected_event_t *r = &e.u.namespace_rejected;
                check(f, moq_announcement_eq(r->ann, f->ann[side]), "rejection.identity");
                value(f, "rejection.code", r->error_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
                value(f, "rejection.retry", r->can_retry, true);
                value(f, "rejection.retry_after", r->retry_after_ms, 17);
                if (check(f, r->reason.len == 4 && r->reason.data != NULL, "rejection.reason.extent"))
                    check(f, memcmp(r->reason.data, "gone", 4) == 0, "rejection.reason.bytes");
            }
            break;
        case MOQ_EVENT_REQUEST_GOAWAY:
            o->goaway++;
            check(f, f->expect_goaway, "goaway.expected");
            if (check(f, e.detail_size >= sizeof(e.u.request_goaway), "goaway.extent")) {
                const moq_request_goaway_event_t *g = &e.u.request_goaway;
                static const char uri[] = "https://next.example/moq";
                value(f, "goaway.family", g->family, MOQ_REQUEST_FAMILY_ANNOUNCEMENT);
                check(f, moq_announcement_eq(g->handle.announcement, f->ann[side]), "goaway.identity");
                value(f, "goaway.timeout", g->timeout_ms, 37);
                if (check(f, g->new_session_uri.len == sizeof(uri) - 1 &&
                             g->new_session_uri.data != NULL, "goaway.uri.extent"))
                    check(f, memcmp(g->new_session_uri.data, uri, sizeof(uri) - 1) == 0,
                          "goaway.uri.bytes");
            }
            break;
        default:
            ++checks;
            ++failures;
            fprintf(stderr, "FAIL %s phase=%s check=unexpected.event.kind actual=%" PRIu32 "\n",
                    f->name, f->phase, e.kind);
            break;
        }
        moq_event_cleanup(&e);
    }
    return check(f, false, "finite.event.drain");
}

static bool pump(fixture_t *f)
{
    return result(f, "finite.action.pump",
        moq_simpair_run_until_quiescent(f->pair, 16, NULL), MOQ_OK) &&
        collect(f, 0) && collect(f, 1);
}

static bool progress(fixture_t *f)
{
    for (unsigned i = 0; i < 3; i++) {
        if (!result(f, "virtual.tick", moq_simpair_advance_to(f->pair, now(f) + 1), MOQ_OK) ||
            !pump(f)) return false;
    }
    return true;
}

static void no_peer_rejection(fixture_t *f)
{
    for (unsigned side = 0; side < 2; side++) {
        value(f, "no.fabricated.request.error", f->seen[side].rejected, 0);
        value(f, "session.stays.established", moq_session_state(f->session[side]), MOQ_SESS_ESTABLISHED);
    }
}

static void no_legacy_terminal(fixture_t *f)
{
    for (unsigned side = 0; side < 2; side++) {
        value(f, "no.legacy.done", f->seen[side].done, 0);
        value(f, "no.legacy.cancelled", f->seen[side].cancelled, 0);
    }
    no_peer_rejection(f);
}

static bool accept_initial(fixture_t *f)
{
    moq_accept_namespace_cfg_t cfg;
    moq_accept_namespace_cfg_init(&cfg);
    return result(f, "initial.accept", moq_session_accept_namespace(f->session[1],
                  f->ann[1], &cfg, now(f)), MOQ_OK) && pump(f) &&
        value(f, "initial.accepted.once", f->seen[0].accepted, 1) &&
        value(f, "initial.no.typed.requester", f->seen[0].terminated, 0) &&
        value(f, "initial.no.typed.responder", f->seen[1].terminated, 0);
}

static bool begin_limits(fixture_t *f, const char *name, moq_version_t version,
                         bool accepted, uint32_t max_events, uint32_t max_actions)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->phase = "setup";
    f->failures_before = failures;
    ++cases;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.seed = 3885;
    cfg.initial_now_us = 1000;
    cfg.max_events = max_events;
    cfg.max_actions = max_actions;
    /* Match the public TS fixture's one-action setup: omit optional capacity
     * messages rather than needing two simultaneous setup actions. */
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = max_actions != 1;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 16;
    cfg.trace_fn = trace;
    cfg.trace_ctx = f;
    if (!result(f, "pair.create", moq_simpair_create(&cfg, &f->pair), MOQ_OK)) return false;
    f->session[0] = moq_simpair_client(f->pair);
    f->session[1] = moq_simpair_server(f->pair);
    if (!result(f, "pair.start", moq_simpair_start(f->pair), MOQ_OK) || !pump(f)) return false;
    for (unsigned side = 0; side < 2; side++) {
        if (!value(f, "setup.once", f->seen[side].setup, 1) ||
            !value(f, "setup.established", moq_session_state(f->session[side]),
                   MOQ_SESS_ESTABLISHED)) return false;
    }
    moq_publish_namespace_cfg_t publish;
    moq_publish_namespace_cfg_init(&publish);
    moq_bytes_t part = {namespace_bytes, sizeof(namespace_bytes) - 1};
    publish.track_namespace = (moq_namespace_t){&part, 1};
    f->capture_request = true;
    if (!result(f, "ordinary.publish", moq_session_publish_namespace(f->session[0],
                &publish, now(f), &f->ann[0]), MOQ_OK) || !pump(f)) return false;
    f->capture_request = false;
    if (!value(f, "published.once", f->seen[1].published, 1) ||
        !check(f, moq_announcement_is_valid(f->ann[0]) && moq_announcement_is_valid(f->ann[1]),
               "both.announcement.identities") ||
        !value(f, "no.premature.acceptance", f->seen[0].accepted, 0)) return false;
    if (version != MOQ_VERSION_DRAFT_16) {
        if (!value(f, "one.request.bidi", f->opens, 1) ||
            !check(f, f->request_inputs > 0 && f->ref[0]._v != 0 && f->ref[1]._v != 0,
                   "public.mapped.refs")) return false;
    } else if (!value(f, "d16.control.channel", f->opens, 0)) return false;
    if (accepted && !accept_initial(f)) return false;
    no_peer_rejection(f);
    if (failures != f->failures_before) return false;
    f->setup_ok = true;
    f->phase = "contract";
    return true;
}

static bool begin(fixture_t *f, const char *name, moq_version_t version, bool accepted)
{
    return begin_limits(f, name, version, accepted, 0, 0);
}

static void finish(fixture_t *f)
{
    if (!f->setup_ok) ++setup_failures;
    if (f->pair) moq_simpair_destroy(f->pair);
    else {
        if (f->session[0]) moq_session_destroy(f->session[0]);
        if (f->session[1]) moq_session_destroy(f->session[1]);
    }
    printf("CASE %s: %s (setup %s)\n", f->name,
        failures == f->failures_before ? "PASS" : "FAIL", f->setup_ok ? "PASS" : "FAIL");
}

static void stale(fixture_t *f, unsigned side)
{
    moq_accept_namespace_cfg_t cfg;
    moq_accept_namespace_cfg_init(&cfg);
    /* A still-live requester or established responder would return WRONG_STATE,
     * without changing its state. A committed terminal must instead be STALE. */
    result(f, "committed.handle.stale", moq_session_accept_namespace(f->session[side],
           f->ann[side], &cfg, now(f)), MOQ_ERR_STALE_HANDLE);
}

static void terminal(fixture_t *f, unsigned side, uint32_t cause, uint64_t code)
{
    observation_t *o = &f->seen[side];
    value(f, "typed.terminal.once", o->terminated, 1);
    if (o->terminated) {
        value(f, "typed.cause", o->terminal.cause, cause);
        value(f, "typed.local_role", o->terminal.local_role,
            side == 0 ? MOQ_NAMESPACE_ROLE_REQUESTER : MOQ_NAMESPACE_ROLE_RESPONDER);
        value(f, "typed.stream_error.exact", o->terminal.stream_error_code, code);
    }
    no_legacy_terminal(f);
}

static void response_control(void)
{
    fixture_t f;
    if (!begin(&f, "d22_completed_initial_response", MOQ_VERSION_DRAFT_22, true)) goto out;
    if (!progress(&f)) goto out;
    value(&f, "accepted.once", f.seen[0].accepted, 1);
    value(&f, "response.not.terminal.requester", f.seen[0].terminated, 0);
    value(&f, "response.not.terminal.responder", f.seen[1].terminated, 0);
    no_legacy_terminal(&f);
out:
    finish(&f);
}

static void response_fin(bool initial_response)
{
    fixture_t f;
    if (!begin(&f, initial_response ? "d22_established_responder_fin" : "d22_missing_response_fin",
               MOQ_VERSION_DRAFT_22, initial_response)) goto out;
    if (!result(&f, "peer.response.fin", moq_session_on_bidi_stream_bytes(f.session[0],
                f.ref[0], NULL, 0, true, now(&f)), MOQ_OK)) goto out;
    stale(&f, 0); /* before the terminal event is polled */
    if (!progress(&f)) goto out;
    terminal(&f, 0, initial_response ? MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE
                                   : MOQ_NAMESPACE_TERMINAL_MISSING_RESPONSE, 0);
    value(&f, "response.count", f.seen[0].accepted, initial_response ? 1 : 0);
out:
    finish(&f);
}

static void peer_signal(const char *name, unsigned side, bool stop, uint64_t code)
{
    fixture_t f;
    if (!begin(&f, name, MOQ_VERSION_DRAFT_22, true)) goto out;
    moq_result_t rc = stop
        ? moq_session_on_bidi_stream_stop(f.session[side], f.ref[side], code, now(&f))
        : moq_session_on_bidi_stream_reset(f.session[side], f.ref[side], code, now(&f));
    if (!result(&f, "peer.stream.signal", rc, MOQ_OK)) goto out;
    stale(&f, side); /* before polling: retirement must accompany commitment */
    if (!progress(&f)) goto out;
    terminal(&f, side, stop ? MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING
                           : MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM, code);
    value(&f, "prior.acceptance.preserved", f.seen[0].accepted, 1);
out:
    finish(&f);
}

static void pending_withdrawal(void)
{
    fixture_t f;
    if (!begin(&f, "d22_pending_announcer_withdrawal", MOQ_VERSION_DRAFT_22, false)) goto out;
    f.capture_withdrawal = true;
    if (!result(&f, "pending.withdraw", moq_session_publish_namespace_done(f.session[0],
                f.ann[0], now(&f)), MOQ_OK)) goto out;
    result(&f, "withdrawn.handle.stale", moq_session_publish_namespace_done(f.session[0],
           f.ann[0], now(&f)), MOQ_ERR_STALE_HANDLE);
    if (!progress(&f)) goto out;
    value(&f, "withdraw.remaining.send.half", f.withdraw_resets, 1);
    value(&f, "withdraw.remaining.receive.half", f.withdraw_stops, 1);
    check(&f, f.withdrawal_cause == MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM ||
              f.withdrawal_cause == MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING,
          "withdrawal.public.input.observed");
    value(&f, "withdrawal.public.input.code", f.withdrawal_code, 1);
    terminal(&f, 1, f.withdrawal_cause, f.withdrawal_code);
    if (f.seen[1].terminated == 1) stale(&f, 1);
    value(&f, "local.withdraw.no.peer.terminal", f.seen[0].terminated, 0);
    value(&f, "withdraw.no.acceptance", f.seen[0].accepted, 0);
out:
    finish(&f);
}

static void pending_receiver_control(void)
{
    fixture_t f;
    if (!begin(&f, "d22_pending_receiver_cannot_withdraw", MOQ_VERSION_DRAFT_22, false)) goto out;
    result(&f, "receiver.withdraw.refused", moq_session_publish_namespace_done(f.session[1],
           f.ann[1], now(&f)), MOQ_ERR_WRONG_STATE);
    if (!accept_initial(&f) || !progress(&f)) goto out;
    value(&f, "receiver.refusal.no.terminal", f.seen[1].terminated, 0);
    value(&f, "requester.no.terminal", f.seen[0].terminated, 0);
    no_legacy_terminal(&f);
out:
    finish(&f);
}

static void legacy_control(const char *name, moq_version_t version, bool cancel)
{
    fixture_t f;
    if (!begin(&f, name, version, true)) goto out;
    moq_cancel_namespace_cfg_t cfg;
    moq_cancel_namespace_cfg_init(&cfg);
    cfg.error_code = MOQ_REQUEST_ERROR_INTERNAL_ERROR;
    moq_result_t rc = cancel
        ? moq_session_cancel_namespace(f.session[1], f.ann[1], &cfg, now(&f))
        : moq_session_publish_namespace_done(f.session[0], f.ann[0], now(&f));
    if (!result(&f, "legacy.local.terminal", rc, MOQ_OK) || !progress(&f)) goto out;
    value(&f, "legacy.done.once", f.seen[1].done, cancel ? 0 : 1);
    value(&f, "legacy.cancelled.once", f.seen[0].cancelled, cancel ? 1 : 0);
    value(&f, "legacy.no.requester.done", f.seen[0].done, 0);
    value(&f, "legacy.no.responder.cancelled", f.seen[1].cancelled, 0);
    for (unsigned side = 0; side < 2; side++) value(&f, "legacy.no.typed.terminal", f.seen[side].terminated, 0);
    stale(&f, cancel ? 0 : 1);
    no_peer_rejection(&f);
out:
    finish(&f);
}

/* The following schedules extend the retained ordinary case. All pressure
 * is produced by another public request, never by changing session internals. */
typedef struct {
    unsigned fins, resets, stops;
} cleanup_actions_t;

static bool next_action(fixture_t *f, unsigned side, moq_action_kind_t kind,
                        moq_action_t *a)
{
    size_t count = 0;
    if (!result(f, "action.poll", moq_session_poll_actions_ex(f->session[side],
                a, 1, sizeof(*a), &count), MOQ_OK) ||
        !value(f, "action.count", count, 1)) return false;
    if (!value(f, "action.kind", a->kind, kind)) {
        moq_action_cleanup(a);
        return false;
    }
    return true;
}

static bool drain_ann_actions(fixture_t *f, unsigned side, cleanup_actions_t *counts)
{
    for (unsigned i = 0; i < 16; i++) {
        moq_action_t a;
        size_t count = 0;
        if (!result(f, "cleanup.poll", moq_session_poll_actions_ex(f->session[side],
                    &a, 1, sizeof(a), &count), MOQ_OK)) return false;
        if (!count) return true;
        moq_stream_ref_t ref = moq_stream_ref_from_u64(0);
        uint64_t code = 0;
        bool cancellation = false;
        switch (a.kind) {
        case MOQ_ACTION_CLOSE_BIDI_STREAM:
            ref = a.u.close_bidi_stream.stream_ref;
            counts->fins++;
            break;
        case MOQ_ACTION_SEND_BIDI_STREAM:
            ref = a.u.send_bidi_stream.stream_ref;
            check(f, a.u.send_bidi_stream.fin, "cleanup.send.is_fin");
            value(f, "cleanup.fin.empty", a.u.send_bidi_stream.len, 0);
            counts->fins++;
            break;
        case MOQ_ACTION_RESET_BIDI_STREAM:
            ref = a.u.reset_bidi_stream.stream_ref;
            code = a.u.reset_bidi_stream.error_code;
            counts->resets++;
            cancellation = true;
            break;
        case MOQ_ACTION_STOP_BIDI_STREAM:
            ref = a.u.stop_bidi_stream.stream_ref;
            code = a.u.stop_bidi_stream.error_code;
            counts->stops++;
            cancellation = true;
            break;
        case MOQ_ACTION_ABORT_BIDI_STREAM:
            ref = a.u.abort_bidi_stream.stream_ref;
            code = a.u.abort_bidi_stream.error_code;
            counts->resets++;
            counts->stops++;
            cancellation = true;
            break;
        default:
            check(f, false, "cleanup.unexpected.action");
            break;
        }
        value(f, "cleanup.identity", ref._v, f->ref[side]._v);
        if (cancellation) value(f, "cleanup.cancel.code", code, 1);
        moq_action_cleanup(&a);
    }
    return check(f, false, "finite.cleanup.drain");
}

static bool settle_local(fixture_t *f, unsigned side, cleanup_actions_t *counts)
{
    memset(counts, 0, sizeof(*counts));
    for (unsigned i = 0; i < 3; i++)
        if (!collect(f, side) || !drain_ann_actions(f, side, counts)) return false;
    return collect(f, side);
}

static void action_counts(fixture_t *f, cleanup_actions_t c, unsigned fins,
                          unsigned resets, unsigned stops)
{
    value(f, "cleanup.fins", c.fins, fins);
    value(f, "cleanup.resets", c.resets, resets);
    value(f, "cleanup.stops", c.stops, stops);
}

static bool filler_request(fixture_t *f, unsigned from, bool deliver)
{
    moq_track_status_cfg_t cfg;
    moq_track_status_cfg_init(&cfg);
    moq_bytes_t part = MOQ_BYTES_LITERAL("held");
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("pressure");
    moq_track_status_handle_t handle;
    if (!result(f, "filler.request", moq_session_track_status(f->session[from],
                &cfg, now(f), &handle), MOQ_OK)) return false;
    if (!deliver) return true;
    if (f->pair) return result(f, "filler.deliver",
        moq_simpair_run_until_quiescent(f->pair, 16, NULL), MOQ_OK);
    moq_action_t a;
    if (!next_action(f, from, MOQ_ACTION_OPEN_BIDI_STREAM, &a)) return false;
    /* Adapter-local peer refs, mapped from actual emitted stream opens. */
    moq_stream_ref_t peer = moq_stream_ref_from_u64(a.u.open_bidi_stream.stream_ref._v + 65536);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f->session[1 - from], peer,
        a.u.open_bidi_stream.data, a.u.open_bidi_stream.len, a.u.open_bidi_stream.fin, now(f));
    moq_action_cleanup(&a);
    return result(f, "filler.deliver", rc, MOQ_OK);
}

static bool prepare_pressure(fixture_t *f, unsigned side)
{
    f->phase = "setup";
    f->setup_ok = false;
    if (!filler_request(f, 1 - side, true) || !filler_request(f, side, false)) return false;
    f->setup_ok = true;
    f->phase = "contract";
    return true;
}

static bool take_filler_event(fixture_t *f, unsigned side)
{
    moq_event_t e;
    size_t count = 0;
    if (!result(f, "filler.event.poll", moq_session_poll_events_ex(f->session[side],
                &e, 1, sizeof(e), &count), MOQ_OK) ||
        !value(f, "filler.event.count", count, 1)) return false;
    bool ok = value(f, "filler.event.kind", e.kind, MOQ_EVENT_TRACK_STATUS_REQUEST);
    moq_event_cleanup(&e);
    return ok;
}

static bool take_filler_action(fixture_t *f, unsigned side)
{
    moq_action_t a;
    if (!next_action(f, side, MOQ_ACTION_OPEN_BIDI_STREAM, &a)) return false;
    check(f, a.u.open_bidi_stream.stream_ref._v != f->ref[side]._v, "filler.action.distinct");
    moq_action_cleanup(&a);
    return true;
}

static moq_result_t signal_peer(fixture_t *f, unsigned side, bool reset, uint64_t code)
{
    return reset ? moq_session_on_bidi_stream_reset(f->session[side], f->ref[side], code, now(f))
                 : moq_session_on_bidi_stream_stop(f->session[side], f->ref[side], code, now(f));
}

static bool input_consumed(fixture_t *f, unsigned side, moq_result_t rc)
{
    bool ok = check(f, rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK, "input.accepted.or.retained");
    if (rc == MOQ_ERR_WOULD_BLOCK)
        check(f, moq_session_has_transport_stream(f->session[side], f->ref[side]),
              "input.refusal.has.receive.owner");
    return ok;
}

static bool response_action(fixture_t *f, bool reject, moq_action_t *a)
{
    moq_result_t rc;
    if (reject) {
        moq_reject_namespace_cfg_t cfg;
        moq_reject_namespace_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        cfg.reason = MOQ_BYTES_LITERAL("gone");
        cfg.can_retry = true;
        cfg.retry_after_ms = 17;
        rc = moq_session_reject_namespace(f->session[1], f->ann[1], &cfg, now(f));
    } else {
        moq_accept_namespace_cfg_t cfg;
        moq_accept_namespace_cfg_init(&cfg);
        rc = moq_session_accept_namespace(f->session[1], f->ann[1], &cfg, now(f));
    }
    if (!result(f, "response.send", rc, MOQ_OK) ||
        !next_action(f, 1, MOQ_ACTION_SEND_BIDI_STREAM, a)) return false;
    bool ok = check(f, a->detail_size >= sizeof(a->u.send_bidi_stream), "response.action.extent") &&
        value(f, "response.stream", a->u.send_bidi_stream.stream_ref._v, f->ref[1]._v) &&
        check(f, a->u.send_bidi_stream.data != NULL && a->u.send_bidi_stream.len > 1,
              "response.generated.frame") &&
        value(f, "response.send.fin", a->u.send_bidi_stream.fin, reject);
    if (!ok) moq_action_cleanup(a);
    return ok;
}

static void graceful_fin_actions(void)
{
    fixture_t f;
    if (!begin_limits(&f, "d22_completion_queues_only_fin", MOQ_VERSION_DRAFT_22, true, 1, 1)) goto out;
    if (!result(&f, "completion.peer_fin", moq_session_on_bidi_stream_bytes(f.session[0],
                f.ref[0], NULL, 0, true, now(&f)), MOQ_OK)) goto out;
    stale(&f, 0);
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    terminal(&f, 0, MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE, 0);
    action_counts(&f, actions, 1, 0, 0);
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "completion.receive.retired");
out:
    finish(&f);
}

static void requester_fin_delayed_accept(void)
{
    fixture_t f;
    if (!begin_limits(&f, "d22_requester_fin_delayed_accept", MOQ_VERSION_DRAFT_22, false, 1, 1)) goto out;
    if (!result(&f, "requester.direction.fin", moq_session_on_bidi_stream_bytes(f.session[1],
                f.ref[1], NULL, 0, true, now(&f)), MOQ_OK) || !progress(&f)) goto out;
    value(&f, "requester.fin.not.withdrawal", f.seen[1].terminated, 0);
    if (!accept_initial(&f)) goto out;
    moq_cancel_namespace_cfg_t cfg;
    moq_cancel_namespace_cfg_init(&cfg);
    if (!result(&f, "receiver.cancel.after_peer_fin", moq_session_cancel_namespace(f.session[1],
                f.ann[1], &cfg, now(&f)), MOQ_OK)) goto out;
    stale(&f, 1);
    cleanup_actions_t actions;
    if (!settle_local(&f, 1, &actions)) goto out;
    action_counts(&f, actions, 0, 1, 0);
    value(&f, "local.cancel.no.peer.event", f.seen[1].terminated, 0);
    check(&f, !moq_session_has_transport_stream(f.session[1], f.ref[1]), "peer_fin.receive.retired");
    no_legacy_terminal(&f);
out:
    finish(&f);
}

static void truncated_response(void)
{
    fixture_t f;
    if (!begin(&f, "d22_generated_response_truncated_fin", MOQ_VERSION_DRAFT_22, false)) goto out;
    moq_action_t a;
    if (!response_action(&f, false, &a)) goto out;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len - 1, true, now(&f));
    moq_action_cleanup(&a);
    if (!result(&f, "truncated.generated_prefix.fin", rc, MOQ_OK)) goto out;
    stale(&f, 0);
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    terminal(&f, 0, MOQ_NAMESPACE_TERMINAL_TRUNCATED_MESSAGE, 0);
    value(&f, "truncated.no.acceptance", f.seen[0].accepted, 0);
    action_counts(&f, actions, 0, 1, 0);
out:
    finish(&f);
}

static void crossed_terminal(unsigned side, bool reset)
{
    char name[96];
    snprintf(name, sizeof(name), "d22_%s_blocked_%s0_then_%s_max",
        side ? "responder" : "requester", reset ? "reset" : "stop", reset ? "stop" : "reset");
    fixture_t f;
    if (!begin_limits(&f, name, MOQ_VERSION_DRAFT_22, false, 1, 1) ||
        !prepare_pressure(&f, side)) goto out;
    result(&f, "first.signal.retained", signal_peer(&f, side, reset, 0), MOQ_ERR_WOULD_BLOCK);
    result(&f, "crossed.signal.retained", signal_peer(&f, side, !reset, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
    result(&f, "repeat.signal.retained", signal_peer(&f, side, reset, 7), MOQ_ERR_WOULD_BLOCK);
    if (side == 1) {
        moq_accept_namespace_cfg_t ac;
        moq_accept_namespace_cfg_init(&ac);
        moq_reject_namespace_cfg_t rc;
        moq_reject_namespace_cfg_init(&rc);
        rc.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        result(&f, "known.failure.cannot.accept", moq_session_accept_namespace(f.session[1],
               f.ann[1], &ac, now(&f)), MOQ_ERR_WRONG_STATE);
        result(&f, "known.failure.cannot.reject", moq_session_reject_namespace(f.session[1],
               f.ann[1], &rc, now(&f)), MOQ_ERR_WRONG_STATE);
    } else {
        result(&f, "known.failure.cannot.withdraw", moq_session_publish_namespace_done(f.session[0],
               f.ann[0], now(&f)), MOQ_ERR_WRONG_STATE);
    }
    if (!take_filler_event(&f, side) || !collect(&f, side)) goto out;
    terminal(&f, side, reset ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM
                            : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING, 0);
    stale(&f, side);
    if (!take_filler_action(&f, side)) goto out;
    cleanup_actions_t actions;
    if (!settle_local(&f, side, &actions)) goto out;
    action_counts(&f, actions, 0, 1, 0);
    terminal(&f, side, reset ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM
                            : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING, 0);
    check(&f, !moq_session_has_transport_stream(f.session[side], f.ref[side]),
          "crossed.retired.without.new.packet");
out:
    finish(&f);
}

static void retained_response(bool reject, bool reset, bool fin)
{
    char name[96];
    snprintf(name, sizeof(name), "d22_retained_%s_%s_%s", reject ? "error" : "ok",
             reset ? "reset" : "stop", fin ? "fin" : "no_fin");
    fixture_t f;
    if (!begin_limits(&f, name, MOQ_VERSION_DRAFT_22, false, 1, 1) ||
        !prepare_pressure(&f, 0)) goto out;
    f.expect_rejection = reject;
    moq_action_t a;
    if (!response_action(&f, reject, &a)) goto out;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, fin, now(&f));
    moq_action_cleanup(&a);
    result(&f, "complete.response.retained", rc, MOQ_ERR_WOULD_BLOCK);
    result(&f, "later.signal.retained", signal_peer(&f, 0, reset, UINT64_MAX), MOQ_ERR_WOULD_BLOCK);
    if (!take_filler_event(&f, 0) || !collect(&f, 0)) goto out;
    stale(&f, 0);
    if (!take_filler_action(&f, 0)) goto out;
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    value(&f, "original.acceptance.once", f.seen[0].accepted, reject ? 0 : 1);
    value(&f, "original.rejection.once", f.seen[0].rejected, reject ? 1 : 0);
    if (reject) {
        value(&f, "rejection.no.second.terminal", f.seen[0].terminated, 0);
        value(&f, "rejection.no.legacy.done", f.seen[0].done, 0);
        value(&f, "rejection.no.legacy.cancelled", f.seen[0].cancelled, 0);
    } else {
        terminal(&f, 0, fin ? MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE :
                 reset ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING,
                 fin ? 0 : UINT64_MAX);
        check(&f, f.seen[0].accepted_at != 0 &&
                  f.seen[0].accepted_at < f.seen[0].terminal_at, "response.precedes.terminal");
        /* The FIN outcome precedes the later signal, but cancellation reaches
         * the still-open local half while its FIN output is blocked. D22
         * 6.4.2.3 therefore owes RESET, not a FIN after cancellation. */
        action_counts(&f, actions, 0, 1, !fin && !reset ? 1 : 0);
    }
    if (!fin && !reset) {
        check(&f, moq_session_has_transport_stream(f.session[0], f.ref[0]), "stop.retains.receive.owner");
        result(&f, "retained.response.final_peer_fin", moq_session_on_bidi_stream_bytes(f.session[0],
               f.ref[0], NULL, 0, true, now(&f)), MOQ_OK);
        if (!settle_local(&f, 0, &actions)) goto out;
        action_counts(&f, actions, 0, 0, 0);
    }
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "response.receive.retired");
    value(&f, "response.final.terminal.count", f.seen[0].terminated, reject ? 0 : 1);
    for (unsigned side = 0; side < 2; side++)
        value(&f, "response.session.established", moq_session_state(f.session[side]), MOQ_SESS_ESTABLISHED);
out:
    finish(&f);
}

static void stop_waits_for_peer(bool reset)
{
    fixture_t f;
    if (!begin_limits(&f, reset ? "d22_stop_then_delayed_peer_reset" : "d22_stop_then_delayed_peer_fin",
                      MOQ_VERSION_DRAFT_22, true, 1, 1)) goto out;
    if (!input_consumed(&f, 1, signal_peer(&f, 1, false, UINT64_MAX))) goto out;
    stale(&f, 1);
    cleanup_actions_t actions;
    if (!settle_local(&f, 1, &actions)) goto out;
    terminal(&f, 1, MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING, UINT64_MAX);
    action_counts(&f, actions, 0, 1, 1);
    check(&f, moq_session_has_transport_stream(f.session[1], f.ref[1]), "stop.is.not.peer_fin");
    moq_result_t rc = reset ? signal_peer(&f, 1, true, 0) :
        moq_session_on_bidi_stream_bytes(f.session[1], f.ref[1], NULL, 0, true, now(&f));
    result(&f, "actual.peer.receive.terminal", rc, MOQ_OK);
    if (!settle_local(&f, 1, &actions)) goto out;
    terminal(&f, 1, MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING, UINT64_MAX);
    action_counts(&f, actions, 0, 0, 0);
    check(&f, !moq_session_has_transport_stream(f.session[1], f.ref[1]), "actual.peer.receive.retired");
out:
    finish(&f);
}

static void one_action_withdrawal(void)
{
    fixture_t f;
    if (!begin_limits(&f, "d22_pending_withdraw_one_action_reannounce", MOQ_VERSION_DRAFT_22, false, 1, 1)) goto out;
    f.phase = "setup";
    f.setup_ok = false;
    if (!filler_request(&f, 0, false)) goto out;
    f.setup_ok = true;
    f.phase = "contract";
    result(&f, "full_queue.withdraw.no_effect", moq_session_publish_namespace_done(f.session[0],
           f.ann[0], now(&f)), MOQ_ERR_WOULD_BLOCK);
    moq_accept_namespace_cfg_t ac;
    moq_accept_namespace_cfg_init(&ac);
    result(&f, "refused.withdraw.handle.still_live", moq_session_accept_namespace(f.session[0],
           f.ann[0], &ac, now(&f)), MOQ_ERR_WRONG_STATE);
    if (!take_filler_action(&f, 0)) goto out;
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, 0, 0, 0);
    if (!result(&f, "one_action.withdraw", moq_session_publish_namespace_done(f.session[0],
                f.ann[0], now(&f)), MOQ_OK)) goto out;
    stale(&f, 0);
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, 0, 1, 1);
    value(&f, "withdraw.no.local.peer.event", f.seen[0].terminated, 0);
    check(&f, moq_session_has_transport_stream(f.session[0], f.ref[0]), "withdraw.waits.for.peer");

    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    moq_bytes_t part = {namespace_bytes, sizeof(namespace_bytes) - 1};
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    moq_announcement_t replacement;
    if (!result(&f, "stale_guard.allows.reannounce", moq_session_publish_namespace(f.session[0],
                &cfg, now(&f), &replacement), MOQ_OK)) goto out;
    check(&f, moq_announcement_is_valid(replacement) &&
              !moq_announcement_eq(replacement, f.ann[0]), "replacement.distinct.live_handle");
    moq_action_t a;
    if (!next_action(&f, 0, MOQ_ACTION_OPEN_BIDI_STREAM, &a)) goto out;
    moq_stream_ref_t replacement_ref = a.u.open_bidi_stream.stream_ref;
    check(&f, replacement_ref._v != f.ref[0]._v, "replacement.distinct.stream");
    moq_action_cleanup(&a);
    stale(&f, 0);
    result(&f, "withdraw.peer_reset", signal_peer(&f, 0, true, UINT64_MAX), MOQ_OK);
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, 0, 0, 0);
    value(&f, "withdraw.no.late.terminal", f.seen[0].terminated, 0);
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "withdraw.receive.retired");
    check(&f, moq_session_has_transport_stream(f.session[0], replacement_ref), "replacement.still.owned");
    result(&f, "old.retirement.preserves.replacement", moq_session_accept_namespace(f.session[0],
           replacement, &ac, now(&f)), MOQ_ERR_WRONG_STATE);
    no_legacy_terminal(&f);
out:
    finish(&f);
}

static bool goaway_action(fixture_t *f, moq_action_t *a)
{
    moq_request_goaway_cfg_t cfg;
    moq_request_goaway_cfg_init(&cfg);
    cfg.new_session_uri = MOQ_BYTES_LITERAL("https://next.example/moq");
    cfg.timeout_ms = 37;
    if (!result(f, "goaway.send", moq_session_request_goaway_namespace(f->session[1],
                f->ann[1], &cfg, now(f)), MOQ_OK) ||
        !next_action(f, 1, MOQ_ACTION_SEND_BIDI_STREAM, a)) return false;
    value(f, "goaway.send.ref", a->u.send_bidi_stream.stream_ref._v, f->ref[1]._v);
    check(f, !a->u.send_bidi_stream.fin, "goaway.send.not_fin");
    return true;
}

static void sent_goaway_waits_for_peer(bool reset)
{
    fixture_t f;
    if (!begin_limits(&f, reset ? "d22_sent_goaway_stop_then_reset" : "d22_sent_goaway_stop_then_fin",
                      MOQ_VERSION_DRAFT_22, true, 1, 1)) goto out;
    moq_action_t a;
    if (!goaway_action(&f, &a)) goto out;
    moq_action_cleanup(&a);
    result(&f, "migrated.peer.stop", signal_peer(&f, 1, false, 0), MOQ_OK);
    stale(&f, 1);
    cleanup_actions_t actions;
    if (!settle_local(&f, 1, &actions)) goto out;
    value(&f, "sent.migration.no.terminal", f.seen[1].terminated, 0);
    action_counts(&f, actions, 0, 1, 1);
    check(&f, moq_session_has_transport_stream(f.session[1], f.ref[1]), "sent.migration.retains.receive");
    moq_result_t rc = reset ? signal_peer(&f, 1, true, UINT64_MAX) :
        moq_session_on_bidi_stream_bytes(f.session[1], f.ref[1], NULL, 0, true, now(&f));
    result(&f, "sent.migration.actual.peer_terminal", rc, MOQ_OK);
    if (!settle_local(&f, 1, &actions)) goto out;
    action_counts(&f, actions, 0, 0, 0);
    value(&f, "sent.migration.no.late.terminal", f.seen[1].terminated, 0);
    check(&f, !moq_session_has_transport_stream(f.session[1], f.ref[1]), "sent.migration.receive.retired");
    no_legacy_terminal(&f);
out:
    finish(&f);
}

static void held_ok_then_partial_goaway_fin(void)
{
    fixture_t f;
    if (!begin_limits(&f, "d22_held_ok_then_partial_goaway_fin", MOQ_VERSION_DRAFT_22,
                      false, 1, 1) || !prepare_pressure(&f, 0)) goto out;
    moq_action_t a;
    if (!response_action(&f, false, &a)) goto out;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, false, now(&f));
    moq_action_cleanup(&a);
    result(&f, "held.ok", rc, MOQ_ERR_WOULD_BLOCK);
    if (!goaway_action(&f, &a)) goto out;
    rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len - 1, true, now(&f));
    moq_action_cleanup(&a);
    result(&f, "held.partial.goaway.fin", rc, MOQ_ERR_WOULD_BLOCK);
    if (!take_filler_event(&f, 0) || !collect(&f, 0)) goto out;
    stale(&f, 0);
    if (!take_filler_action(&f, 0)) goto out;
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    value(&f, "held.original.ok.once", f.seen[0].accepted, 1);
    value(&f, "partial.goaway.not.projected", f.seen[0].goaway, 0);
    terminal(&f, 0, MOQ_NAMESPACE_TERMINAL_TRUNCATED_MESSAGE, 0);
    check(&f, f.seen[0].accepted_at < f.seen[0].terminal_at, "ok.precedes.truncated.suffix");
    action_counts(&f, actions, 0, 1, 0);
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "truncated.suffix.retired");
out:
    finish(&f);
}

static void received_goaway_waits_for_peer(void)
{
    fixture_t f;
    if (!begin_limits(&f, "d22_received_goaway_stop_then_fin", MOQ_VERSION_DRAFT_22, true, 1, 1)) goto out;
    f.expect_goaway = true;
    moq_action_t a;
    if (!goaway_action(&f, &a)) goto out;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, false, now(&f));
    moq_action_cleanup(&a);
    if (!input_consumed(&f, 0, rc)) goto out;
    stale(&f, 0);
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    value(&f, "received.migration.once", f.seen[0].goaway, 1);
    value(&f, "received.migration.no.terminal", f.seen[0].terminated, 0);
    check(&f, moq_session_has_transport_stream(f.session[0], f.ref[0]), "received.migration.retains.receive");
    result(&f, "received.migration.peer_stop", signal_peer(&f, 0, false, UINT64_MAX), MOQ_OK);
    if (!settle_local(&f, 0, &actions)) goto out;
    check(&f, moq_session_has_transport_stream(f.session[0], f.ref[0]), "received.migration.stop.not_fin");
    result(&f, "received.migration.peer_fin", moq_session_on_bidi_stream_bytes(f.session[0],
           f.ref[0], NULL, 0, true, now(&f)), MOQ_OK);
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, 0, 0, 0);
    value(&f, "received.migration.still.once", f.seen[0].goaway, 1);
    value(&f, "received.migration.no.late.terminal", f.seen[0].terminated, 0);
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "received.migration.receive.retired");
    no_legacy_terminal(&f);
out:
    finish(&f);
}

/* Public manual transport only for asymmetric scratch limits, which SimPair's
 * public config does not expose. No private owner/layout access. */
static uint32_t capacity_accept_uri(void *ctx, uint32_t present,
                                    moq_bytes_t authority, moq_bytes_t path_query)
{
    (void)ctx;
    static const char target[] = "capacity.invalid";
    if (present != MOQ_BINDING_TARGET_COMPLETE || authority.len != sizeof(target) - 1 ||
        !authority.data || memcmp(authority.data, target, sizeof(target) - 1) != 0)
        return MOQ_URI_INVALID_AUTHORITY;
    return path_query.len == 0 ? MOQ_URI_ACCEPT : MOQ_URI_INVALID_PATH;
}

static bool capacity_begin(fixture_t *f, const char *name, unsigned limited_side,
                           size_t scratch, uint32_t max_events)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->phase = "setup";
    f->failures_before = failures;
    ++cases;
    for (unsigned side = 0; side < 2; ++side) {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
            side ? MOQ_PERSPECTIVE_SERVER : MOQ_PERSPECTIVE_CLIENT);
        cfg.version = MOQ_VERSION_DRAFT_22;
        cfg.initial_request_capacity = 16;
        cfg.send_request_capacity = false;
        cfg.max_actions = 1;
        cfg.max_events = max_events;
        if (side == limited_side) cfg.output_scratch_size = (uint32_t)scratch;
        if (!result(f, "manual.create", moq_session_create(&cfg, now(f), &f->session[side]), MOQ_OK)) return false;
        moq_transport_binding_t binding;
        moq_transport_binding_init(&binding);
        binding.kind = MOQ_BINDING_RAW_QUIC;
        binding.facts_known = MOQ_BINDING_FACTS_ALL;
        binding.facts_supported = MOQ_BINDING_FACTS_ALL;
        if (side == 0) {
            binding.target_present = MOQ_BINDING_TARGET_COMPLETE;
            binding.authority = MOQ_BYTES_LITERAL("capacity.invalid");
        } else {
            binding.accept_uri = capacity_accept_uri;
        }
        if (!result(f, "manual.bind", moq_session_bind_transport(f->session[side],
                    &binding, sizeof(binding)), MOQ_OK) ||
            !result(f, "manual.start", moq_session_start(f->session[side], now(f)), MOQ_OK)) return false;
    }
    for (unsigned side = 0; side < 2; ++side) {
        moq_action_t a;
        if (!next_action(f, side, MOQ_ACTION_OPEN_UNI_CONTROL, &a)) return false;
        moq_result_t rc = moq_session_on_control_bytes(f->session[1 - side],
            a.u.open_uni_control.data, a.u.open_uni_control.len, now(f));
        moq_action_cleanup(&a);
        if (!result(f, "manual.setup.deliver", rc, MOQ_OK)) return false;
    }
    for (unsigned side = 0; side < 2; ++side)
        if (!collect(f, side) || !value(f, "manual.setup.once", f->seen[side].setup, 1) ||
            !value(f, "manual.established", moq_session_state(f->session[side]),
                   MOQ_SESS_ESTABLISHED)) return false;
    f->setup_ok = true;
    f->phase = "contract";
    return true;
}

static bool capacity_publish(fixture_t *f, const moq_auth_token_t *token, moq_result_t expected)
{
    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    moq_bytes_t part = {namespace_bytes, sizeof(namespace_bytes) - 1};
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.auth_tokens = token;
    cfg.auth_token_count = token ? 1 : 0;
    if (!result(f, "capacity.public.publish", moq_session_publish_namespace(f->session[0],
                &cfg, now(f), &f->ann[0]), MOQ_OK)) return false;
    moq_action_t a;
    if (!next_action(f, 0, MOQ_ACTION_OPEN_BIDI_STREAM, &a)) return false;
    f->ref[0] = a.u.open_bidi_stream.stream_ref;
    f->ref[1] = moq_stream_ref_from_u64(f->ref[0]._v + 65536);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f->session[1], f->ref[1],
        a.u.open_bidi_stream.data, a.u.open_bidi_stream.len, false, now(f));
    moq_action_cleanup(&a);
    return result(f, "capacity.publish.deliver", rc, expected);
}

static bool capacity_bytes(fixture_t *f, const char *label, moq_bytes_t got, moq_bytes_t want)
{
    return value(f, label, got.len, want.len) &&
        check(f, !want.len || (got.data && memcmp(got.data, want.data, want.len) == 0), label);
}

static size_t aligned_size(size_t size, size_t alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

static const uint8_t capacity_reason[] = "0123456789abcdef0123456789abcdef0123456789abcdef";
static const uint8_t capacity_uri[] = "https://next.example/moq";

static void capacity_response(bool redirect, bool short_arena, unsigned pressure)
{
    char name[96];
    snprintf(name, sizeof(name), "d22_capacity_%s_%s_pressure%u", redirect ? "redirect" : "error",
             short_arena ? "short" : "exact", pressure);
    size_t needed = sizeof(capacity_reason) - 1;
    if (redirect) needed += aligned_size(sizeof(capacity_uri) - 1, _Alignof(moq_bytes_t)) +
                            sizeof(moq_bytes_t) + sizeof(namespace_bytes) - 1;
    fixture_t f;
    if (!capacity_begin(&f, name, 0, needed - short_arena, pressure == 2 ? 2 : 1) ||
        !capacity_publish(&f, NULL, MOQ_OK) || !collect(&f, 1)) goto out;
    if (pressure && (!filler_request(&f, 1, true) || !filler_request(&f, 0, false))) goto out;
    moq_reject_namespace_cfg_t cfg;
    moq_reject_namespace_cfg_init(&cfg);
    cfg.error_code = redirect ? MOQ_REQUEST_ERROR_REDIRECT : MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
    cfg.can_retry = true;
    cfg.retry_after_ms = 17;
    cfg.reason = (moq_bytes_t){capacity_reason, sizeof(capacity_reason) - 1};
    moq_bytes_t part = {namespace_bytes, sizeof(namespace_bytes) - 1};
    if (redirect) {
        cfg.redirect.connect_uri = (moq_bytes_t){capacity_uri, sizeof(capacity_uri) - 1};
        cfg.redirect.track_namespace = (moq_namespace_t){&part, 1};
    }
    if (!result(&f, "capacity.peer.reject", moq_session_reject_namespace(f.session[1],
                f.ann[1], &cfg, now(&f)), MOQ_OK)) goto out;
    moq_action_t a;
    if (!next_action(&f, 1, MOQ_ACTION_SEND_BIDI_STREAM, &a)) goto out;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
        a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, false, now(&f));
    moq_action_cleanup(&a);
    if (pressure) result(&f, "capacity.complete.response", rc, MOQ_ERR_WOULD_BLOCK);
    else input_consumed(&f, 0, rc);
    if (pressure) {
        /* The complete error/redirect is first; later STOP must not replace it
         * or an already known permanent inability to project it. */
        input_consumed(&f, 0, signal_peer(&f, 0, false, UINT64_MAX));
        if (!take_filler_event(&f, 0)) goto out;
        if (!result(&f, "capacity.scratch.recycle",
                    moq_session_process_pending(f.session[0], now(&f)), MOQ_OK)) goto out;
        if (!take_filler_action(&f, 0)) goto out;
    }
    if (short_arena) {
        if (!collect(&f, 0)) goto out;
        terminal(&f, 0, MOQ_NAMESPACE_TERMINAL_LOCAL_CAPACITY, 0);
        value(&f, "capacity.no.shortened.rejection", f.seen[0].rejected, 0);
    } else {
        moq_event_t ev;
        if (!value(&f, "capacity.original.event", moq_session_poll_events(f.session[0], &ev, 1), 1)) goto out;
        if (value(&f, "capacity.original.kind", ev.kind,
                  redirect ? MOQ_EVENT_REQUEST_REDIRECT : MOQ_EVENT_NAMESPACE_REJECTED)) {
            if (redirect) {
                const moq_request_redirect_event_t *r = &ev.u.request_redirect;
                check(&f, moq_announcement_eq(r->handle.announcement, f.ann[0]), "capacity.redirect.handle");
                value(&f, "capacity.redirect.family", r->family, MOQ_REQUEST_FAMILY_ANNOUNCEMENT);
                capacity_bytes(&f, "capacity.redirect.uri", r->connect_uri, cfg.redirect.connect_uri);
                value(&f, "capacity.redirect.ns.count", r->track_namespace.count, 1);
                if (r->track_namespace.count == 1)
                    capacity_bytes(&f, "capacity.redirect.ns", r->track_namespace.parts[0], part);
                value(&f, "capacity.redirect.track.empty", r->track_name.len, 0);
                capacity_bytes(&f, "capacity.redirect.reason", r->reason, cfg.reason);
                value(&f, "capacity.redirect.retry", r->retry_after_ms, 17);
                value(&f, "capacity.redirect.can_retry", r->can_retry, true);
            } else {
                const moq_namespace_rejected_event_t *r = &ev.u.namespace_rejected;
                check(&f, moq_announcement_eq(r->ann, f.ann[0]), "capacity.error.handle");
                value(&f, "capacity.error.code", r->error_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
                capacity_bytes(&f, "capacity.error.reason", r->reason, cfg.reason);
                value(&f, "capacity.error.retry", r->retry_after_ms, 17);
                value(&f, "capacity.error.can_retry", r->can_retry, true);
            }
        }
        moq_event_cleanup(&ev);
    }
    stale(&f, 0);
    cleanup_actions_t actions;
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, !short_arena && !pressure ? 1 : 0,
                  short_arena || pressure ? 1 : 0, short_arena || pressure ? 1 : 0);
    check(&f, moq_session_has_transport_stream(f.session[0], f.ref[0]), "capacity.receive.retained");
    result(&f, "capacity.actual.peer.fin", moq_session_on_bidi_stream_bytes(f.session[0],
               f.ref[0], NULL, 0, true, now(&f)), MOQ_OK);
    if (!settle_local(&f, 0, &actions)) goto out;
    action_counts(&f, actions, 0, 0, 0);
    value(&f, "capacity.one.outcome", f.seen[0].terminated, short_arena ? 1 : 0);
    check(&f, !moq_session_has_transport_stream(f.session[0], f.ref[0]), "capacity.receive.retired");
    value(&f, "capacity.session.alive", moq_session_state(f.session[0]), MOQ_SESS_ESTABLISHED);
    /* Unrelated public request admission still works after retirement. */
    moq_track_status_cfg_t ts;
    moq_track_status_cfg_init(&ts);
    ts.track_namespace = (moq_namespace_t){&part, 1};
    ts.track_name = MOQ_BYTES_LITERAL("after");
    moq_track_status_handle_t handle;
    result(&f, "capacity.unrelated.request", moq_session_track_status(f.session[0], &ts, now(&f), &handle), MOQ_OK);
    if (next_action(&f, 0, MOQ_ACTION_OPEN_BIDI_STREAM, &a)) moq_action_cleanup(&a);
out:
    finish(&f);
}

static void capacity_incoming(bool token, bool short_arena)
{
    char name[96];
    snprintf(name, sizeof(name), "d22_capacity_incoming_%s_%s", token ? "token" : "namespace",
             short_arena ? "short" : "exact");
    static const uint8_t token_bytes[] = {0xa5, 0x5a, 0x01};
    moq_auth_token_t auth = {.token_type = 7, .token_value = {token_bytes, sizeof(token_bytes)}};
    size_t needed = sizeof(moq_bytes_t) + sizeof(namespace_bytes) - 1;
    if (token) needed = aligned_size(needed + sizeof(token_bytes), _Alignof(moq_resolved_token_t)) +
                        sizeof(moq_resolved_token_t);
    fixture_t f;
    if (!capacity_begin(&f, name, 1, needed - short_arena, 1) ||
        !filler_request(&f, 0, true) || !filler_request(&f, 1, false) ||
        !capacity_publish(&f, token ? &auth : NULL, MOQ_ERR_WOULD_BLOCK)) goto out;
    if (!take_filler_action(&f, 1) || !take_filler_event(&f, 1)) goto out;
    if (!result(&f, "capacity.incoming.empty.retry", moq_session_on_bidi_stream_bytes(f.session[1],
                f.ref[1], NULL, 0, false, now(&f)), MOQ_OK)) goto out;
    if (short_arena) {
        if (!collect(&f, 1)) goto out;
        value(&f, "capacity.no.fabricated.admission", f.seen[1].published, 0);
        value(&f, "capacity.no.pre_app.terminal", f.seen[1].terminated, 0);
        moq_action_t a;
        if (!next_action(&f, 1, MOQ_ACTION_SEND_BIDI_STREAM, &a)) goto out;
        value(&f, "capacity.actual.error.fin", a.u.send_bidi_stream.fin, true);
        moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
            a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, true, now(&f));
        moq_action_cleanup(&a);
        if (!result(&f, "capacity.actual.error.deliver", rc, MOQ_OK)) goto out;
        moq_event_t ev;
        if (!value(&f, "capacity.actual.error.event", moq_session_poll_events(f.session[0], &ev, 1), 1)) goto out;
        if (value(&f, "capacity.actual.error.kind", ev.kind, MOQ_EVENT_NAMESPACE_REJECTED)) {
            value(&f, "capacity.actual.internal_error", ev.u.namespace_rejected.error_code, MOQ_REQUEST_ERROR_INTERNAL_ERROR);
            check(&f, moq_announcement_eq(ev.u.namespace_rejected.ann, f.ann[0]), "capacity.actual.error.handle");
        }
        moq_event_cleanup(&ev);
        cleanup_actions_t actions;
        if (!settle_local(&f, 0, &actions)) goto out;
        action_counts(&f, actions, 1, 0, 0);
        result(&f, "capacity.pre_app.peer.fin", moq_session_on_bidi_stream_bytes(f.session[1],
               f.ref[1], NULL, 0, true, now(&f)), MOQ_OK);
        check(&f, !moq_session_has_transport_stream(f.session[1], f.ref[1]), "capacity.pre_app.drain.retired");
    } else {
        moq_event_t ev;
        if (!value(&f, "capacity.exact.admission", moq_session_poll_events(f.session[1], &ev, 1), 1)) goto out;
        if (value(&f, "capacity.exact.kind", ev.kind, MOQ_EVENT_NAMESPACE_PUBLISHED)) {
            const moq_namespace_published_event_t *p = &ev.u.namespace_published;
            f.ann[1] = p->ann;
            value(&f, "capacity.exact.namespace.count", p->track_namespace.count, 1);
            if (p->track_namespace.count == 1)
                capacity_bytes(&f, "capacity.exact.namespace", p->track_namespace.parts[0],
                    (moq_bytes_t){namespace_bytes, sizeof(namespace_bytes) - 1});
            value(&f, "capacity.exact.tokens", p->token_count, token ? 1 : 0);
            if (token && p->token_count == 1) {
                value(&f, "capacity.exact.token.type", p->tokens[0].token_type, 7);
                capacity_bytes(&f, "capacity.exact.token.value", p->tokens[0].token_value, auth.token_value);
            }
        }
        moq_event_cleanup(&ev);
        moq_accept_namespace_cfg_t cfg;
        moq_accept_namespace_cfg_init(&cfg);
        if (!result(&f, "capacity.exact.accept", moq_session_accept_namespace(f.session[1], f.ann[1],
                    &cfg, now(&f)), MOQ_OK)) goto out;
        moq_action_t a;
        if (!next_action(&f, 1, MOQ_ACTION_SEND_BIDI_STREAM, &a)) goto out;
        moq_result_t rc = moq_session_on_bidi_stream_bytes(f.session[0], f.ref[0],
            a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, false, now(&f));
        moq_action_cleanup(&a);
        result(&f, "capacity.exact.ok.deliver", rc, MOQ_OK);
        collect(&f, 0);
        value(&f, "capacity.exact.accepted.once", f.seen[0].accepted, 1);
    }
    for (unsigned side = 0; side < 2; ++side)
        value(&f, "capacity.incoming.session.alive", moq_session_state(f.session[side]), MOQ_SESS_ESTABLISHED);
out:
    finish(&f);
}

int main(void)
{
    response_control();
    response_fin(true);
    response_fin(false);
    peer_signal("d22_requester_reset_zero", 0, false, 0);
    peer_signal("d22_requester_reset_full64", 0, false, UINT64_MAX);
    peer_signal("d22_requester_stop_zero", 0, true, 0);
    peer_signal("d22_requester_stop_full64", 0, true, UINT64_MAX);
    peer_signal("d22_responder_reset_zero", 1, false, 0);
    peer_signal("d22_responder_reset_full64", 1, false, UINT64_MAX);
    peer_signal("d22_responder_stop_zero", 1, true, 0);
    peer_signal("d22_responder_stop_full64", 1, true, UINT64_MAX);
    pending_withdrawal();
    pending_receiver_control();
    legacy_control("d16_withdraw_done", MOQ_VERSION_DRAFT_16, false);
    legacy_control("d16_receiver_cancelled", MOQ_VERSION_DRAFT_16, true);
    legacy_control("d18_withdraw_done", MOQ_VERSION_DRAFT_18, false);
    legacy_control("d18_receiver_cancelled", MOQ_VERSION_DRAFT_18, true);
    graceful_fin_actions();
    requester_fin_delayed_accept();
    truncated_response();
    for (unsigned side = 0; side < 2; side++) {
        crossed_terminal(side, false);
        crossed_terminal(side, true);
    }
    for (unsigned reset = 0; reset < 2; reset++) {
        retained_response(false, reset != 0, false);
        retained_response(false, reset != 0, true);
        retained_response(true, reset != 0, false);
    }
    stop_waits_for_peer(false);
    stop_waits_for_peer(true);
    one_action_withdrawal();
    sent_goaway_waits_for_peer(false);
    sent_goaway_waits_for_peer(true);
    received_goaway_waits_for_peer();
    held_ok_then_partial_goaway_fin();
    for (unsigned redirect = 0; redirect < 2; ++redirect) {
        for (unsigned short_arena = 0; short_arena < 2; ++short_arena) {
            capacity_response(redirect != 0, short_arena != 0, 0);
            capacity_response(redirect != 0, short_arena != 0, 1);
        }
        capacity_response(redirect != 0, false, 2);
    }
    for (unsigned token = 0; token < 2; ++token) {
        capacity_incoming(token != 0, false);
        capacity_incoming(token != 0, true);
    }
    printf("namespace terminal: cases=%u checks=%u failures=%u setup_failures=%u\n",
           cases, checks, failures, setup_failures);
    return failures ? 1 : 0;
}
