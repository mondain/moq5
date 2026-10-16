/* Modern request IDs identify requests; they are not D16 request credit.
 * Four live requests per round exercise family/role routing before retirement. */
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SUB, NS, FAMILIES, ROUNDS = 12 };
enum { REJECT, CANCEL, WITHDRAW, OUTCOMES };
enum { SETUP, OPEN, ANSWER, TERMINAL, RETIRE };
_Static_assert(ROUNDS * FAMILIES * 2 < 128, "observed Request IDs fit one vi64 byte");

typedef struct summary {
    uint64_t hash;
    uint64_t outcomes[2][FAMILIES][OUTCOMES];
    uint64_t requests, responses, terminals, retired;
} summary_t;

typedef struct request {
    char name[32];
    uint64_t id, out_ref, in_ref;
    union { moq_subscription_t sub; moq_announcement_t ann; } sender, receiver;
    unsigned outcome, opens, inputs, answers, request_events, response_events;
    unsigned terminal_events;
    unsigned namespace_reset_actions[2], namespace_reset_inputs[2];
    moq_sim_trace_input_kind_t namespace_first_input[2];
    uint64_t namespace_first_code[2];
} request_t;

typedef struct state {
    moq_simpair_t *pair;
    moq_session_t *session[2];
    moq_version_t version;
    uint64_t seed, rng, serial[2];
    unsigned round, phase;
    bool failed;
    int64_t allocations;
    request_t req[2][FAMILIES];
    summary_t result;
} state_t;

static bool check(state_t *s, const char *name, bool ok)
{
    if (!ok && !s->failed) {
        fprintf(stderr, "FAIL d%u seed=%llu round=%u phase=%u: %s\n",
            (unsigned)s->version, (unsigned long long)s->seed, s->round, s->phase, name);
        s->failed = true;
    }
    return ok;
}

static void *alloc_bytes(size_t n, void *ctx)
{
    state_t *s = ctx;
    void *p = malloc(n);
    if (p) s->allocations++;
    return p;
}

static void free_bytes(void *p, size_t n, void *ctx)
{
    (void)n;
    state_t *s = ctx;
    if (p) s->allocations--;
    free(p);
}

static void *realloc_bytes(void *p, size_t old, size_t n, void *ctx)
{
    if (!p) return alloc_bytes(n, ctx);
    if (!n) { free_bytes(p, old, ctx); return NULL; }
    return realloc(p, n);
}

static uint64_t random_next(state_t *s)
{
    uint64_t z = (s->rng += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static void logical(state_t *s, uint64_t value)
{
    s->result.hash = (s->result.hash ^ value) * UINT64_C(0x100000001b3);
}

static unsigned side(moq_perspective_t role)
{
    return role == MOQ_PERSPECTIVE_CLIENT ? 0u : 1u;
}

/* D18 section10.1 and D22 section6.4.2.1: both messages begin with Request ID.
 * All selected message types/IDs fit one-byte vi64; no production decoder or
 * private next-ID counter participates in the expectation. */
static bool frame(state_t *s, moq_bytes_t bytes)
{
    return check(s, "wire.complete.frame", bytes.data && bytes.len >= 4 &&
        bytes.len == 3 + (((size_t)bytes.data[1] << 8) | bytes.data[2]));
}

static void observe(state_t *s, unsigned from, moq_bytes_t bytes,
                    uint64_t ref, bool outgoing)
{
    if (!frame(s, bytes)) return;
    unsigned family = bytes.data[0] == 0x03 ? SUB : NS;
    if (!check(s, "wire.request.family", bytes.data[0] == 0x03 || bytes.data[0] == 0x06)) return;
    request_t *q = &s->req[from][family];
    const uint64_t expected = q->id;
    if (!check(s, "wire.request.id.sequence", bytes.data[3] == expected)) return;
    if (!check(s, "wire.request.id.parity", (bytes.data[3] & 1u) == from)) return;
    if (outgoing) {
        check(s, "wire.request.single.open", ++q->opens == 1);
        q->out_ref = ref;
        s->result.requests++;
    } else {
        check(s, "wire.request.single.input", ++q->inputs == 1);
        q->in_ref = ref;
    }
}

#define TRACE_HAS(r, field) \
    ((r)->struct_size >= offsetof(moq_sim_trace_record_t, field) + sizeof((r)->field))

static void trace_namespace_terminal(state_t *s, const moq_sim_trace_record_t *r)
{
    bool action = r->kind == MOQ_SIM_TRACE_ACTION &&
        (r->action_kind == MOQ_ACTION_RESET_BIDI_STREAM ||
         r->action_kind == MOQ_ACTION_STOP_BIDI_STREAM ||
         r->action_kind == MOQ_ACTION_ABORT_BIDI_STREAM);
    bool input = r->kind == MOQ_SIM_TRACE_INPUT &&
        (r->input_kind == MOQ_SIM_INPUT_BIDI_RESET || r->input_kind == MOQ_SIM_INPUT_BIDI_STOP);
    if (!action && !input) return;
    if (!check(s, "trace.terminal.detail.size",
               TRACE_HAS(r, stream_ref) && TRACE_HAS(r, error_code))) return;

    unsigned local = side(action ? r->from : r->to);
    request_t *found = NULL;
    unsigned family = FAMILIES;
    /* INPUT names the target's local ref, never the originating action's ref. */
    for (unsigned origin = 0; origin < 2; ++origin)
        for (unsigned f = 0; f < FAMILIES; ++f) {
            request_t *q = &s->req[origin][f];
            uint64_t ref = local == origin ? q->out_ref : q->in_ref;
            if (ref && ref == r->stream_ref._v) {
                if (!check(s, "trace.terminal.unique.stream", found == NULL)) return;
                found = q;
                family = f;
            }
        }
    if (!check(s, "trace.terminal.mapped.stream", found != NULL) || family != NS) return;
    if (!check(s, "trace.namespace.nonrejected", found->outcome != REJECT) ||
        !check(s, "trace.namespace.cancel.code", r->error_code == MOQ_STREAM_ERROR_CANCELLED)) return;
    if (action) {
        if (r->action_kind != MOQ_ACTION_STOP_BIDI_STREAM)
            check(s, "trace.namespace.single.reset.action", ++found->namespace_reset_actions[local] == 1);
    } else {
        check(s, "trace.namespace.peer.direction", side(r->from) == 1 - local);
        if (!found->namespace_first_input[local]) {
            found->namespace_first_input[local] = r->input_kind;
            found->namespace_first_code[local] = r->error_code;
        }
        if (r->input_kind == MOQ_SIM_INPUT_BIDI_RESET)
            check(s, "trace.namespace.single.reset.input", ++found->namespace_reset_inputs[local] == 1);
    }
}

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    state_t *s = ctx;
    if (s->failed) return;
    if (r->kind == MOQ_SIM_TRACE_INPUT && !check(s, "transport.input.result", r->result == MOQ_OK)) return;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->action_kind == MOQ_ACTION_CLOSE_SESSION) {
        check(s, "transport.unexpected.close", false); return;
    }
    if (s->phase == SETUP || s->phase == RETIRE) return;
    if (!check(s, "trace.detail.size", TRACE_HAS(r, stream_ref) &&
               TRACE_HAS(r, detail_bytes) && TRACE_HAS(r, fin))) return;
    unsigned from = side(r->from);
    if (s->phase == OPEN && r->kind == MOQ_SIM_TRACE_ACTION &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        observe(s, from, r->detail_bytes, r->stream_ref._v, true);
    } else if (s->phase == OPEN && r->kind == MOQ_SIM_TRACE_INPUT &&
        r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES) {
        observe(s, from, r->bytes, r->stream_ref._v, false);
    } else if (s->phase == ANSWER && r->kind == MOQ_SIM_TRACE_ACTION &&
        r->action_kind == MOQ_ACTION_SEND_BIDI_STREAM) {
        request_t *found = NULL;
        unsigned found_family = FAMILIES;
        for (unsigned f = 0; f < FAMILIES; ++f) {
            request_t *q = &s->req[1 - from][f];
            if (q->in_ref == r->stream_ref._v && q->inputs == 1) {
                if (!check(s, "wire.response.unique.stream", !found)) return;
                found = q;
                found_family = f;
            }
        }
        if (!check(s, "wire.response.mapped.stream", found != NULL) || !frame(s, r->detail_bytes)) return;
        check(s, "wire.response.family.outcome", r->detail_bytes.data[0] ==
            (found->outcome == REJECT ? 0x05 : found_family == SUB ? 0x04 : 0x07));
        check(s, "wire.response.single", ++found->answers == 1);
        s->result.responses++;
    }
    if (s->phase == TERMINAL) trace_namespace_terminal(s, r);
}

static bool pump(state_t *s)
{
    moq_result_t rc = moq_simpair_run_until_quiescent(s->pair, 32, NULL);
    if (!check(s, "transport.pump.result", rc == MOQ_OK) || s->failed) return false;
    return check(s, "session.client.established", moq_session_state(s->session[0]) == MOQ_SESS_ESTABLISHED) &&
        check(s, "session.server.established", moq_session_state(s->session[1]) == MOQ_SESS_ESTABLISHED);
}

static bool bytes_equal(moq_bytes_t a, const char *b)
{
    size_t n = strlen(b);
    return a.len == n && (!n || (a.data && !memcmp(a.data, b, n)));
}

static bool namespace_equal(moq_namespace_t ns, const char *name)
{
    return ns.count == 2 && ns.parts && bytes_equal(ns.parts[0], "identity") &&
        bytes_equal(ns.parts[1], name);
}

static bool namespace_terminal_trace(state_t *s, const request_t *q, unsigned receiver)
{
    return check(s, "trace.namespace.initiating.reset", q->namespace_reset_actions[1 - receiver] == 1) &&
        check(s, "trace.namespace.received.reset", q->namespace_reset_inputs[receiver] == 1) &&
        check(s, "trace.namespace.first.cause", q->namespace_first_input[receiver] == MOQ_SIM_INPUT_BIDI_RESET) &&
        check(s, "trace.namespace.first.code", q->namespace_first_code[receiver] == MOQ_STREAM_ERROR_CANCELLED);
}

static bool poll(state_t *s, unsigned receiver, unsigned phase)
{
    for (unsigned n = 0; n < 16; ++n) {
        moq_event_t e;
        size_t got = 0;
        moq_result_t rc = moq_session_poll_events_ex(s->session[receiver], &e, 1, sizeof(e), &got);
        if (!check(s, "event.poll.result", rc == MOQ_OK)) return false;
        if (!got) return !s->failed;
        bool ok = true;
        if (phase == SETUP) ok = check(s, "event.setup.only", e.kind == MOQ_EVENT_SETUP_COMPLETE);
        else if (phase == OPEN) {
            unsigned family = e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST ? SUB : NS;
            request_t *q = &s->req[1 - receiver][family];
            ok = check(s, "event.request.family", e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST ||
                e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED);
            if (ok && family == SUB) {
                ok = check(s, "event.subscribe.namespace", namespace_equal(e.u.subscribe_request.track_namespace, q->name)) &&
                    check(s, "event.subscribe.name", bytes_equal(e.u.subscribe_request.track_name, "video")) &&
                    check(s, "event.subscribe.valid.handle", moq_subscription_is_valid(e.u.subscribe_request.sub));
                q->receiver.sub = e.u.subscribe_request.sub;
            } else if (ok) {
                ok = check(s, "event.namespace.name", namespace_equal(e.u.namespace_published.track_namespace, q->name)) &&
                    check(s, "event.namespace.valid.handle", moq_announcement_is_valid(e.u.namespace_published.ann));
                q->receiver.ann = e.u.namespace_published.ann;
            }
            if (ok) ok = check(s, "event.request.single", ++q->request_events == 1);
        } else if (phase == ANSWER) {
            bool sub = e.kind == MOQ_EVENT_SUBSCRIBE_OK || e.kind == MOQ_EVENT_SUBSCRIBE_ERROR;
            request_t *q = &s->req[receiver][sub ? SUB : NS];
            moq_event_kind_t expected = sub ? (q->outcome == REJECT ? MOQ_EVENT_SUBSCRIBE_ERROR : MOQ_EVENT_SUBSCRIBE_OK) :
                (q->outcome == REJECT ? MOQ_EVENT_NAMESPACE_REJECTED : MOQ_EVENT_NAMESPACE_ACCEPTED);
            ok = check(s, "event.response.family.outcome", e.kind == expected);
            if (ok && sub) {
                moq_subscription_t h = q->outcome == REJECT ? e.u.subscribe_error.sub : e.u.subscribe_ok.sub;
                ok = check(s, "event.subscribe.response.handle", moq_subscription_eq(h, q->sender.sub));
                if (ok && q->outcome == REJECT) ok = check(s, "event.subscribe.reject.code", e.u.subscribe_error.error_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED) &&
                    check(s, "event.subscribe.reject.reason", bytes_equal(e.u.subscribe_error.reason, "declined"));
            } else if (ok) {
                moq_announcement_t h = q->outcome == REJECT ? e.u.namespace_rejected.ann : e.u.namespace_accepted.ann;
                ok = check(s, "event.namespace.response.handle", moq_announcement_eq(h, q->sender.ann));
                if (ok && q->outcome == REJECT) ok = check(s, "event.namespace.reject.code", e.u.namespace_rejected.error_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED) &&
                    check(s, "event.namespace.reject.reason", bytes_equal(e.u.namespace_rejected.reason, "declined"));
            }
            if (ok) ok = check(s, "event.response.single", ++q->response_events == 1);
        } else if (phase == TERMINAL) {
            request_t *q;
            bool namespace_terminal = false;
            if (e.kind == MOQ_EVENT_UNSUBSCRIBED) {
                q = &s->req[1 - receiver][SUB];
                ok = check(s, "event.unsubscribe.handle", moq_subscription_eq(e.u.unsubscribed.sub, q->receiver.sub)) &&
                    check(s, "event.unsubscribe.outcome", q->outcome == CANCEL) &&
                    check(s, "event.terminal.cause", e.u.unsubscribed.cause == MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM) &&
                    check(s, "event.terminal.code", e.u.unsubscribed.stream_error_code == MOQ_STREAM_ERROR_CANCELLED);
            } else if (e.kind == MOQ_EVENT_NAMESPACE_DONE) {
                namespace_terminal = true;
                q = &s->req[1 - receiver][NS];
                ok = check(s, "event.namespace.legacy.version", s->version == MOQ_VERSION_DRAFT_18) &&
                    check(s, "event.withdraw.handle", moq_announcement_eq(e.u.namespace_done.ann, q->receiver.ann)) &&
                    check(s, "event.withdraw.outcome", q->outcome == WITHDRAW);
            } else if (e.kind == MOQ_EVENT_NAMESPACE_CANCELLED) {
                namespace_terminal = true;
                q = &s->req[receiver][NS];
                ok = check(s, "event.namespace.legacy.version", s->version == MOQ_VERSION_DRAFT_18) &&
                    check(s, "event.cancel.handle", moq_announcement_eq(e.u.namespace_cancelled.ann, q->sender.ann)) &&
                    check(s, "event.cancel.outcome", q->outcome == CANCEL);
            } else if (e.kind == MOQ_EVENT_NAMESPACE_TERMINATED) {
                namespace_terminal = true;
                q = NULL;
                ok = check(s, "event.namespace.typed.version", s->version == MOQ_VERSION_DRAFT_22) &&
                    check(s, "event.namespace.typed.extent", e.detail_size >= sizeof(moq_namespace_terminated_event_t));
                if (ok) {
                    const moq_namespace_terminated_event_t *t = &e.u.namespace_terminated;
                    bool requester = t->local_role == MOQ_NAMESPACE_ROLE_REQUESTER;
                    q = &s->req[requester ? receiver : 1 - receiver][NS];
                    moq_announcement_t expected = requester ? q->sender.ann : q->receiver.ann;
                    moq_namespace_terminal_cause_t first =
                        q->namespace_first_input[receiver] == MOQ_SIM_INPUT_BIDI_RESET
                            ? MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM : MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING;
                    ok = check(s, "event.namespace.typed.role", t->local_role == MOQ_NAMESPACE_ROLE_REQUESTER ||
                               t->local_role == MOQ_NAMESPACE_ROLE_RESPONDER) &&
                        check(s, "event.namespace.typed.outcome", q->outcome == (requester ? CANCEL : WITHDRAW)) &&
                        check(s, "event.namespace.typed.handle", moq_announcement_eq(t->ann, expected)) &&
                        check(s, "event.namespace.typed.first.cause", t->cause == first) &&
                        check(s, "event.namespace.typed.first.code", t->stream_error_code == q->namespace_first_code[receiver]);
                }
            } else { q = NULL; ok = check(s, "event.terminal.family", false); }
            if (ok && namespace_terminal) ok = namespace_terminal_trace(s, q, receiver);
            if (ok) ok = check(s, "event.terminal.single", ++q->terminal_events == 1);
            if (ok) s->result.terminals++;
        } else ok = check(s, "event.unexpected.after.retirement", false);
        if (ok) logical(s, ((uint64_t)receiver << 32) | e.kind);
        moq_event_cleanup(&e);
        if (!ok || s->failed) return false;
    }
    return check(s, "event.inventory.limit", false);
}

static bool issue(state_t *s, unsigned origin, unsigned family)
{
    request_t *q = &s->req[origin][family];
    q->id = 2 * s->serial[origin] + origin;
    snprintf(q->name, sizeof(q->name), "%c-%u-%u", origin ? 's' : 'c', s->round, family);
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("identity"), moq_bytes_cstr(q->name)};
    moq_result_t rc;
    uint64_t now = moq_simpair_now_us(s->pair);
    if (family == SUB) {
        moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){parts, 2}; cfg.track_name = MOQ_BYTES_LITERAL("video");
        rc = moq_session_subscribe(s->session[origin], &cfg, now, &q->sender.sub);
    } else {
        moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){parts, 2};
        rc = moq_session_publish_namespace(s->session[origin], &cfg, now, &q->sender.ann);
    }
    if (!check(s, "producer.issue.result", rc == MOQ_OK)) return false;
    s->serial[origin]++;
    logical(s, ((uint64_t)origin << 32) | ((uint64_t)family << 16) | q->id);
    return true;
}

static bool answer(state_t *s, unsigned origin, unsigned family)
{
    request_t *q = &s->req[origin][family];
    moq_session_t *peer = s->session[1 - origin];
    uint64_t now = moq_simpair_now_us(s->pair);
    moq_result_t rc;
    if (family == SUB && q->outcome == REJECT) {
        moq_reject_subscribe_cfg_t cfg; moq_reject_subscribe_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED; cfg.reason = MOQ_BYTES_LITERAL("declined");
        rc = moq_session_reject_subscribe(peer, q->receiver.sub, &cfg, now);
    } else if (family == SUB) {
        moq_accept_subscribe_cfg_t cfg; moq_accept_subscribe_cfg_init(&cfg);
        rc = moq_session_accept_subscribe(peer, q->receiver.sub, &cfg, now);
    } else if (q->outcome == REJECT) {
        moq_reject_namespace_cfg_t cfg; moq_reject_namespace_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED; cfg.reason = MOQ_BYTES_LITERAL("declined");
        rc = moq_session_reject_namespace(peer, q->receiver.ann, &cfg, now);
    } else {
        moq_accept_namespace_cfg_t cfg; moq_accept_namespace_cfg_init(&cfg);
        rc = moq_session_accept_namespace(peer, q->receiver.ann, &cfg, now);
    }
    logical(s, ((uint64_t)origin << 32) | ((uint64_t)family << 16) | q->outcome);
    return check(s, "producer.answer.result", rc == MOQ_OK);
}

static bool terminate(state_t *s, unsigned origin, unsigned family)
{
    request_t *q = &s->req[origin][family];
    if (q->outcome == REJECT) return true;
    uint64_t now = moq_simpair_now_us(s->pair);
    moq_result_t rc;
    if (family == SUB) rc = moq_session_unsubscribe(s->session[origin], q->sender.sub, now);
    else if (q->outcome == WITHDRAW) rc = moq_session_publish_namespace_done(s->session[origin], q->sender.ann, now);
    else {
        moq_cancel_namespace_cfg_t cfg; moq_cancel_namespace_cfg_init(&cfg);
        rc = moq_session_cancel_namespace(s->session[1 - origin], q->receiver.ann, &cfg, now);
    }
    return check(s, "producer.terminal.result", rc == MOQ_OK);
}

static bool retirement(state_t *s, unsigned origin, unsigned family)
{
    request_t *q = &s->req[origin][family];
    uint64_t now = moq_simpair_now_us(s->pair);
    moq_result_t a, b;
    if (family == SUB) {
        moq_accept_subscribe_cfg_t cfg; moq_accept_subscribe_cfg_init(&cfg);
        a = moq_session_unsubscribe(s->session[origin], q->sender.sub, now);
        b = moq_session_accept_subscribe(s->session[1 - origin], q->receiver.sub, &cfg, now);
    } else {
        moq_accept_namespace_cfg_t cfg; moq_accept_namespace_cfg_init(&cfg);
        a = moq_session_publish_namespace_done(s->session[origin], q->sender.ann, now);
        b = moq_session_accept_namespace(s->session[1 - origin], q->receiver.ann, &cfg, now);
    }
    if (!check(s, "retirement.sender.stale", a == MOQ_ERR_STALE_HANDLE) ||
        !check(s, "retirement.receiver.stale", b == MOQ_ERR_STALE_HANDLE)) return false;
    s->result.retired++;
    s->result.outcomes[origin][family][q->outcome]++;
    return true;
}

static bool run_round(state_t *s)
{
    memset(s->req, 0, sizeof(s->req));
    for (unsigned role = 0; role < 2; ++role) {
        s->req[role][SUB].outcome = s->round < 3 ? (s->round == 1 ? REJECT : CANCEL) : (unsigned)(random_next(s) % 2);
        s->req[role][NS].outcome = s->round < 3 ? (s->round == 0 ? WITHDRAW : s->round == 1 ? REJECT : CANCEL) :
            (unsigned)(random_next(s) % OUTCOMES);
    }
    unsigned order[] = {0, 1, 2, 3};
    if (s->round >= 3)
        for (unsigned i = 3; i > 0; --i) {
            unsigned j = (unsigned)(random_next(s) % (i + 1));
            unsigned tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
    s->phase = OPEN;
    for (unsigned i = 0; i < 4; ++i)
        if (!issue(s, order[i] / FAMILIES, order[i] % FAMILIES)) return false;
    if (!pump(s) || !poll(s, 0, OPEN) || !poll(s, 1, OPEN)) return false;
    for (unsigned role = 0; role < 2; ++role)
        for (unsigned f = 0; f < FAMILIES; ++f) {
            request_t *q = &s->req[role][f];
            if (!check(s, "request.inventory.exact", q->opens == 1 && q->inputs == 1 && q->request_events == 1)) return false;
        }
    s->phase = ANSWER;
    for (unsigned i = 4; i > 0; --i)
        if (!answer(s, order[i - 1] / FAMILIES, order[i - 1] % FAMILIES)) return false;
    if (!pump(s) || !poll(s, 0, ANSWER) || !poll(s, 1, ANSWER)) return false;
    for (unsigned role = 0; role < 2; ++role)
        for (unsigned f = 0; f < FAMILIES; ++f) {
            request_t *q = &s->req[role][f];
            if (!check(s, "response.inventory.exact", q->answers == 1 && q->response_events == 1)) return false;
        }
    s->phase = TERMINAL;
    for (unsigned i = 0; i < 4; ++i)
        if (!terminate(s, order[i] / FAMILIES, order[i] % FAMILIES)) return false;
    if (!pump(s) || !poll(s, 0, TERMINAL) || !poll(s, 1, TERMINAL)) return false;
    s->phase = RETIRE;
    for (unsigned role = 0; role < 2; ++role)
        for (unsigned f = 0; f < FAMILIES; ++f) {
            request_t *q = &s->req[role][f];
            if (!check(s, "terminal.inventory.exact", q->terminal_events == (q->outcome == REJECT ? 0u : 1u)) ||
                !retirement(s, role, f)) return false;
        }
    return pump(s) && poll(s, 0, RETIRE) && poll(s, 1, RETIRE);
}

static bool run(moq_version_t version, uint64_t seed, summary_t *result)
{
    state_t s = {0};
    s.version = version; s.seed = seed; s.rng = seed;
    s.result.hash = UINT64_C(0xcbf29ce484222325);
    moq_alloc_t allocator = {.ctx = &s, .alloc = alloc_bytes, .realloc = realloc_bytes, .free = free_bytes};
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &allocator; cfg.version = version; cfg.seed = seed;
    cfg.max_actions = 32; cfg.max_events = 16;
    cfg.trace_fn = trace; cfg.trace_ctx = &s;
    if (!check(&s, "pair.create.result", moq_simpair_create(&cfg, &s.pair) == MOQ_OK)) goto done;
    s.session[0] = moq_simpair_client(s.pair); s.session[1] = moq_simpair_server(s.pair);
    if (!check(&s, "pair.start.result", moq_simpair_start(s.pair) == MOQ_OK) || !pump(&s)) goto done;
    if (!check(&s, "version.client.actual", moq_session_version(s.session[0]) == version) ||
        !check(&s, "version.server.actual", moq_session_version(s.session[1]) == version) ||
        !poll(&s, 0, SETUP) || !poll(&s, 1, SETUP)) goto done;
    for (s.round = 0; s.round < ROUNDS; ++s.round)
        if (!run_round(&s)) goto done;
    if (!check(&s, "summary.request.count", s.result.requests == ROUNDS * 4) ||
        !check(&s, "summary.response.count", s.result.responses == s.result.requests) ||
        !check(&s, "summary.retirement.count", s.result.retired == s.result.requests)) goto done;
    for (unsigned role = 0; role < 2; ++role)
        for (unsigned f = 0; f < FAMILIES; ++f)
            if (!check(&s, "summary.accept.reject.coverage", s.result.outcomes[role][f][REJECT] > 0 &&
                s.result.outcomes[role][f][CANCEL] > 0 && (f != NS || s.result.outcomes[role][f][WITHDRAW] > 0))) goto done;
done:
    moq_simpair_destroy(s.pair);
    /* Cleanup is void; the per-pair allocator is its observable balance gate. */
    if (s.allocations != 0) {
        fprintf(stderr, "FAIL d%u seed=%llu: cleanup.allocator.balance=%lld\n",
            (unsigned)version, (unsigned long long)seed, (long long)s.allocations);
        s.failed = true;
    }
    if (s.failed) fprintf(stderr, "CLEANUP d%u seed=%llu balance=%lld\n",
        (unsigned)version, (unsigned long long)seed, (long long)s.allocations);
    if (!s.failed) *result = s.result;
    return !s.failed;
}

static bool same(const summary_t *a, const summary_t *b)
{
    return a->hash == b->hash && a->requests == b->requests && a->responses == b->responses &&
        a->terminals == b->terminals && a->retired == b->retired &&
        !memcmp(a->outcomes, b->outcomes, sizeof(a->outcomes));
}

int main(void)
{
    const char *selected = getenv("MOQ_SIM_VERSION");
    if (selected && strcmp(selected, "18") && strcmp(selected, "22")) {
        fprintf(stderr, "FAIL version.selection: MOQ_SIM_VERSION=%s unsupported; require18 or22\n", selected);
        return 1;
    }
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    const uint64_t seeds[] = {1, UINT64_C(0x12345678), UINT64_C(0xdeadbeef), UINT64_MAX};
    unsigned pairs = 0;
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v) {
        if (selected && (unsigned)versions[v] != (unsigned)strtoul(selected, NULL, 10)) continue;
        for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
            summary_t a, b;
            if (!run(versions[v], seeds[i], &a) || !run(versions[v], seeds[i], &b)) return 1;
            pairs += 2;
            if (!same(&a, &b)) { fprintf(stderr, "FAIL twin.logical.summary: d%u seed=%llu\n",
                (unsigned)versions[v], (unsigned long long)seeds[i]); return 1; }
            printf("d%u seed=%llu requests=%llu responses=%llu terminals=%llu retired=%llu hash=%016llx\n",
                (unsigned)versions[v], (unsigned long long)seeds[i], (unsigned long long)a.requests,
                (unsigned long long)a.responses, (unsigned long long)a.terminals,
                (unsigned long long)a.retired, (unsigned long long)a.hash);
            for (unsigned role = 0; role < 2; ++role)
                printf("  %s sub.reject=%llu sub.cancel=%llu ns.reject=%llu ns.cancel=%llu ns.withdraw=%llu\n",
                    role ? "server" : "client", (unsigned long long)a.outcomes[role][SUB][REJECT],
                    (unsigned long long)a.outcomes[role][SUB][CANCEL],
                    (unsigned long long)a.outcomes[role][NS][REJECT],
                    (unsigned long long)a.outcomes[role][NS][CANCEL],
                    (unsigned long long)a.outcomes[role][NS][WITHDRAW]);
        }
    }
    printf("PASS request_bidi_identity pairs=%u balanced=%u\n", pairs, pairs);
    return 0;
}
