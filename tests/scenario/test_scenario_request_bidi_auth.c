/* D18 10.1/10.2.2 and D22 6.4.2.1/8.9: literal request-bidi tokens,
 * with independent per-sender alias state. No legacy control-stream ingress. */
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DELETE_TOKEN, REGISTER_TOKEN, USE_ALIAS, USE_VALUE, OPS, ALIASES = 3 };
enum { VALUE_CAP = 8, TOKENS = 3, RANDOM_REQUESTS = 12 };
enum { DUPLICATE_VALUES, DUPLICATE_ALIAS, DUPLICATE_KINDS };

typedef struct value {
    uint64_t type;
    size_t len;
    uint8_t bytes[VALUE_CAP];
} value_t;

typedef struct token {
    unsigned op, alias;
    value_t value;
} token_t;

typedef struct alias {
    bool live;
    value_t value;
} alias_t;

typedef struct summary {
    uint64_t hash, requests, events, accepted, unauthorized, unknown, retired;
    uint64_t operations[2][OPS], pressures[2], duplicate_closes[2];
    uint64_t duplicate_rejects[2][DUPLICATE_KINDS];
    uint64_t cache_progress[2][DUPLICATE_KINDS], distinct_types[2];
} summary_t;

typedef struct state {
    moq_simpair_t *pair;
    moq_session_t *session[2];
    alias_t aliases[2][ALIASES];
    moq_version_t version;
    uint64_t seed, rng, serial[2], now;
    unsigned role, step;
    int64_t allocations;
    bool failed;
    summary_t result;
} state_t;

typedef struct request {
    unsigned role;
    moq_stream_ref_t ref;
    alias_t next[ALIASES];
    value_t resolved[TOKENS];
    size_t count;
    uint64_t error;
    bool fatal, deny, fragmented;
} request_t;

static bool check(state_t *s, const char *name, bool ok)
{
    if (!ok && !s->failed) {
        fprintf(stderr, "FAIL d%u seed=%llu peer=%s step=%u: %s\n",
            (unsigned)s->version, (unsigned long long)s->seed,
            s->role ? "server" : "client", s->step, name);
        s->failed = true;
    }
    return ok;
}

static void *alloc_bytes(size_t n, void *ctx)
{
    state_t *s = ctx;
    void *p = malloc(n);
    if (p) ++s->allocations;
    return p;
}

static void free_bytes(void *p, size_t n, void *ctx)
{
    (void)n;
    state_t *s = ctx;
    if (p) --s->allocations;
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

static void logical(state_t *s, uint64_t n)
{
    s->result.hash = (s->result.hash ^ n) * UINT64_C(0x100000001b3);
}

static token_t token(state_t *s, unsigned op, unsigned alias)
{
    token_t t = {0};
    t.op = op; t.alias = alias;
    t.value.type = random_next(s) % 8;
    t.value.len = 1 + (size_t)(random_next(s) % VALUE_CAP);
    for (size_t i = 0; i < t.value.len; ++i)
        t.value.bytes[i] = (uint8_t)('a' + random_next(s) % 26);
    return t;
}

static bool equal_value(const value_t *a, const value_t *b)
{
    return a->type == b->type && a->len == b->len &&
        !memcmp(a->bytes, b->bytes, a->len);
}

/* Apply in wire order, including REGISTER/DELETE after a request-level
 * rejection. The plan is committed only after the public ingress completes. */
static void predict(state_t *s, request_t *q, const token_t *tokens, size_t count)
{
    memcpy(q->next, s->aliases[q->role], sizeof(q->next));
    for (size_t i = 0; i < count; ++i) {
        const token_t *t = &tokens[i];
        s->result.operations[q->role][t->op]++;
        logical(s, ((uint64_t)q->role << 32) | ((uint64_t)t->op << 16) | t->alias);
        const value_t *v = &t->value;
        if (t->op == REGISTER_TOKEN) {
            if (q->next[t->alias].live) { q->fatal = true; return; }
            q->next[t->alias].live = true;
            q->next[t->alias].value = *v;
        } else if (t->op == DELETE_TOKEN || t->op == USE_ALIAS) {
            if (t->alias >= ALIASES || !q->next[t->alias].live) {
                if (!q->error) q->error = 0x17;
                continue;
            }
            if (t->op == DELETE_TOKEN) {
                q->next[t->alias].live = false;
                continue;
            }
            v = &q->next[t->alias].value;
        }
        if (q->error) continue;
        for (size_t j = 0; j < q->count; ++j)
            if (equal_value(v, &q->resolved[j])) q->error = 0x4;
        if (!q->error) q->resolved[q->count++] = *v;
    }
}

/* Both modern drafts use the 0..127 one-byte vi64 range. Every enumerated
 * field fits that range; only the frame body length uses a fixed uint16. */
static size_t wire_request(uint8_t out[128], uint8_t id,
                           const token_t *tokens, size_t count)
{
    size_t n = 0;
    out[n++] = 3; out[n++] = 0; out[n++] = 0;
    out[n++] = id;
    out[n++] = 1; out[n++] = 4;
    memcpy(out + n, "auth", 4); n += 4;
    out[n++] = 1; out[n++] = 'v';
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; ++i) {
        const token_t *t = &tokens[i];
        out[n++] = i ? 0 : 3;
        const size_t length_at = n++;
        const size_t start = n;
        out[n++] = (uint8_t)t->op;
        if (t->op != USE_VALUE) out[n++] = (uint8_t)t->alias;
        if (t->op == REGISTER_TOKEN || t->op == USE_VALUE) {
            out[n++] = (uint8_t)t->value.type;
            memcpy(out + n, t->value.bytes, t->value.len); n += t->value.len;
        }
        out[length_at] = (uint8_t)(n - start);
    }
    out[2] = (uint8_t)(n - 3);
    return n;
}

static bool no_events(state_t *s, moq_session_t *session)
{
    moq_event_t e;
    size_t n = 0;
    moq_result_t rc = moq_session_poll_events_ex(session, &e, 1, sizeof(e), &n);
    bool ok = check(s, "event.empty.poll.result", rc == MOQ_OK) &&
        check(s, "event.no.extra", n == 0);
    if (n) moq_event_cleanup(&e);
    return ok;
}

static bool no_actions(state_t *s, moq_session_t *session)
{
    moq_action_t a;
    size_t n = 0;
    moq_result_t rc = moq_session_poll_actions_ex(session, &a, 1, sizeof(a), &n);
    bool ok = check(s, "action.empty.poll.result", rc == MOQ_OK) &&
        check(s, "action.no.extra", n == 0);
    if (n) moq_action_cleanup(&a);
    return ok;
}

static bool response(state_t *s, const request_t *q, uint64_t error)
{
    moq_session_t *session = s->session[1 - q->role];
    const uint8_t ok[] = {4, 0, 2, 1, 0};
    const uint8_t refused[] = {5, 0, 3, (uint8_t)error, 0, 0};
    const uint8_t *expected = error ? refused : ok;
    const size_t len = error ? sizeof(refused) : sizeof(ok);
    moq_action_t a;
    size_t n = 0;
    moq_result_t rc = moq_session_poll_actions_ex(session, &a, 1, sizeof(a), &n);
    bool result = check(s, "response.poll.result", rc == MOQ_OK) &&
        check(s, "response.single", n == 1);
    if (n) {
        result = check(s, "response.bidi.only", a.kind == MOQ_ACTION_SEND_BIDI_STREAM) && result;
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            const moq_send_bidi_stream_action_t *send = &a.u.send_bidi_stream;
            result = check(s, "response.ref", send->stream_ref._v == q->ref._v) && result;
            result = check(s, "response.fin", send->fin == (error != 0)) && result;
            result = check(s, "response.literal.bytes", send->len == len && send->data &&
                !memcmp(send->data, expected, len)) && result;
        }
        moq_action_cleanup(&a);
    }
    logical(s, error);
    return no_actions(s, session) && result;
}

static bool issue(state_t *s, request_t *q, unsigned role,
                  const token_t *tokens, size_t count, bool deny,
                  bool fragmented, bool pressure)
{
    memset(q, 0, sizeof(*q));
    q->role = role; q->deny = deny; q->fragmented = fragmented;
    s->role = role; ++s->step;
    const uint64_t id = s->serial[role]++ * 2 + role;
    q->ref = moq_stream_ref_from_u64(UINT64_C(0xa000) + id);
    if (!check(s, "literal.bounds", id < 128 && count <= TOKENS)) return false;
    predict(s, q, tokens, count);
    uint8_t wire[128] = {0};
    const size_t len = wire_request(wire, (uint8_t)id, tokens, count);
    moq_session_t *session = s->session[1 - role];
    for (size_t offset = 0; offset < len;) {
        const size_t take = fragmented ? 1 : len;
        const bool last = offset + take == len;
        moq_result_t rc = moq_session_on_bidi_stream_bytes(session, q->ref,
            wire + offset, take, false, ++s->now);
        if (!check(s, "ingress.result", rc == (pressure && last ? MOQ_ERR_WOULD_BLOCK : MOQ_OK)))
            return false;
        offset += take;
        if (!last && !pressure && (!no_events(s, session) || !no_actions(s, session))) return false;
    }
    memset(wire, 0, sizeof(wire));
    s->result.requests++;
    logical(s, id);
    if (!q->fatal && !check(s, "session.after.input.established",
        moq_session_state(session) == MOQ_SESS_ESTABLISHED)) return false;
    return true;
}

static bool observe_request(state_t *s, const request_t *q, moq_subscription_t *handle)
{
    moq_session_t *session = s->session[1 - q->role];
    moq_event_t e;
    size_t n = 0;
    moq_result_t rc = moq_session_poll_events_ex(session, &e, 1, sizeof(e), &n);
    bool ok = check(s, "request.event.poll.result", rc == MOQ_OK) &&
        check(s, "request.event.single", n == 1);
    if (n) {
        ok = check(s, "request.event.kind", e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) && ok;
        if (e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            const moq_subscribe_request_event_t *r = &e.u.subscribe_request;
            *handle = r->sub;
            ok = check(s, "request.handle.valid", moq_subscription_is_valid(r->sub)) && ok;
            ok = check(s, "request.namespace.literal", r->track_namespace.count == 1 &&
                r->track_namespace.parts && r->track_namespace.parts[0].len == 4 &&
                r->track_namespace.parts[0].data &&
                !memcmp(r->track_namespace.parts[0].data, "auth", 4)) && ok;
            ok = check(s, "request.name.literal", r->track_name.len == 1 &&
                r->track_name.data && r->track_name.data[0] == 'v') && ok;
            ok = check(s, "request.token.count", r->token_count == q->count) && ok;
            if (r->token_count == q->count) {
                ok = check(s, "request.token.pointer", !q->count || r->tokens) && ok;
                if (r->tokens) for (size_t i = 0; i < q->count; ++i) {
                    const value_t *v = &q->resolved[i];
                    const moq_resolved_token_t *actual = &r->tokens[i];
                    ok = check(s, "request.token.type", actual->token_type == v->type) && ok;
                    ok = check(s, "request.token.literal.value", actual->token_value.len == v->len &&
                        actual->token_value.data && !memcmp(actual->token_value.data, v->bytes, v->len)) && ok;
                    logical(s, actual->token_type);
                    logical(s, actual->token_value.len);
                    if (ok) for (size_t j = 0; j < v->len; ++j) logical(s, actual->token_value.data[j]);
                }
            }
        }
        moq_event_cleanup(&e);
    }
    if (ok) s->result.events++;
    return no_events(s, session) && ok;
}

static bool finish(state_t *s, const request_t *q, unsigned duplicate_kind)
{
    moq_session_t *session = s->session[1 - q->role];
    moq_subscription_t handle = MOQ_SUBSCRIPTION_INVALID;
    if (q->error) {
        if (duplicate_kind < DUPLICATE_KINDS) {
            if (!check(s, "model.reject.is.resolved.duplicate", q->error == 0x4)) return false;
        } else if (!check(s, "model.reject.is.unknown", q->error == 0x17)) return false;
        if (!response(s, q, q->error) || !no_events(s, session)) return false;
        if (duplicate_kind < DUPLICATE_KINDS)
            s->result.duplicate_rejects[q->role][duplicate_kind]++;
        else
            s->result.unknown++;
    } else {
        if (!no_actions(s, session) || !observe_request(s, q, &handle)) return false;
        if (q->deny) {
            moq_reject_subscribe_cfg_t cfg; moq_reject_subscribe_cfg_init(&cfg);
            cfg.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
            if (!check(s, "application.reject.result", moq_session_reject_subscribe(
                session, handle, &cfg, ++s->now) == MOQ_OK) || !response(s, q, 1)) return false;
            s->result.unauthorized++;
        } else {
            moq_accept_subscribe_cfg_t cfg; moq_accept_subscribe_cfg_init(&cfg);
            cfg.has_track_alias = true; cfg.track_alias = 1;
            if (!check(s, "application.accept.result", moq_session_accept_subscribe(
                session, handle, &cfg, ++s->now) == MOQ_OK) || !response(s, q, 0)) return false;
            s->result.accepted++;
        }
    }
    memcpy(s->aliases[q->role], q->next, sizeof(q->next));
    if (q->error || q->deny) {
        if (!check(s, "refused.receive.fin", moq_session_on_bidi_stream_bytes(
            session, q->ref, NULL, 0, true, ++s->now) == MOQ_OK)) return false;
    } else {
        if (!check(s, "accepted.peer.reset", moq_session_on_bidi_stream_reset(
            session, q->ref, 1, ++s->now) == MOQ_OK)) return false;
        moq_event_t e;
        size_t n = 0;
        moq_result_t rc = moq_session_poll_events_ex(session, &e, 1, sizeof(e), &n);
        bool ok = check(s, "terminal.poll.result", rc == MOQ_OK) && check(s, "terminal.single", n == 1);
        if (n) {
            ok = check(s, "terminal.kind", e.kind == MOQ_EVENT_UNSUBSCRIBED) && ok;
            if (e.kind == MOQ_EVENT_UNSUBSCRIBED)
                ok = check(s, "terminal.handle.cause.code", moq_subscription_eq(e.u.unsubscribed.sub, handle) &&
                    e.u.unsubscribed.cause == MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM &&
                    e.u.unsubscribed.stream_error_code == 1) && ok;
            moq_event_cleanup(&e);
        }
        if (!ok) return false;
    }
    if (!no_events(s, session) || !no_actions(s, session) ||
        !check(s, "retirement.stream.released", !moq_session_has_transport_stream(session, q->ref)) ||
        !check(s, "retirement.session.established", moq_session_state(session) == MOQ_SESS_ESTABLISHED)) return false;
    if (moq_subscription_is_valid(handle)) {
        moq_accept_subscribe_cfg_t cfg; moq_accept_subscribe_cfg_init(&cfg);
        if (!check(s, "retirement.handle.stale", moq_session_accept_subscribe(
            session, handle, &cfg, ++s->now) == MOQ_ERR_STALE_HANDLE)) return false;
    }
    s->result.retired++;
    return true;
}

static bool ordinary(state_t *s, unsigned role, const token_t *tokens,
                     size_t count, bool deny, bool fragmented)
{
    request_t q;
    return issue(s, &q, role, tokens, count, deny, fragmented, false) &&
        check(s, "ordinary.not.session.error", !q.fatal) && finish(s, &q, DUPLICATE_KINDS);
}

static bool pressure(state_t *s, unsigned role)
{
    token_t holder_token = token(s, USE_VALUE, 0);
    token_t registration = token(s, REGISTER_TOKEN, 0);
    request_t holder, blocked;
    if (!issue(s, &holder, role, &holder_token, 1, false, false, false) ||
        !issue(s, &blocked, role, &registration, 1, false, true, true)) return false;
    moq_session_t *session = s->session[1 - role];
    if (!check(s, "pressure.owned", moq_session_has_transport_stream(session, blocked.ref)) ||
        !check(s, "pressure.empty.retry.blocked", moq_session_on_bidi_stream_bytes(
            session, blocked.ref, NULL, 0, false, ++s->now) == MOQ_ERR_WOULD_BLOCK) ||
        !no_actions(s, session) || !finish(s, &holder, DUPLICATE_KINDS)) return false;
    if (!check(s, "pressure.empty.retry.complete", moq_session_on_bidi_stream_bytes(
        session, blocked.ref, NULL, 0, false, ++s->now) == MOQ_OK) ||
        !finish(s, &blocked, DUPLICATE_KINDS)) return false;
    s->result.pressures[role]++;
    token_t use = token(s, USE_ALIAS, 0), del = token(s, DELETE_TOKEN, 0);
    return ordinary(s, role, &use, 1, false, true) && ordinary(s, role, &del, 1, false, false);
}

static bool sequence(state_t *s, unsigned role)
{
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        unsigned a = (unsigned)(random_next(s) % ALIASES);
        unsigned b = (a + 1) % ALIASES;
        token_t t[2];
        t[0] = token(s, REGISTER_TOKEN, a);
        if (!ordinary(s, role, t, 1, true, cycle == 0)) return false;
        t[0] = token(s, USE_ALIAS, a);
        if (!ordinary(s, role, t, 1, false, cycle != 0)) return false;
        t[1] = token(s, USE_VALUE, 0);
        /* Same value with a different type remains a distinct authorization. */
        t[1].value = s->aliases[role][a].value;
        t[1].value.type = (t[1].value.type + 1) % 8;
        if (!ordinary(s, role, t, 2, false, true)) return false;
        s->result.distinct_types[role]++;
        t[0] = token(s, DELETE_TOKEN, a); t[1] = token(s, REGISTER_TOKEN, a);
        t[1].value = s->aliases[role][a].value;
        t[1].value.bytes[0] = t[1].value.bytes[0] == 'z' ? 'a' : (uint8_t)(t[1].value.bytes[0] + 1);
        if (!ordinary(s, role, t, 2, false, false)) return false;
        t[0] = token(s, USE_ALIAS, a);
        if (!ordinary(s, role, t, 1, false, true)) return false;
        t[0] = token(s, DELETE_TOKEN, a);
        if (!ordinary(s, role, t, 1, false, false)) return false;
        t[0] = token(s, USE_ALIAS, a);
        if (!ordinary(s, role, t, 1, false, true)) return false;
        t[0] = token(s, DELETE_TOKEN, a);
        if (!ordinary(s, role, t, 1, false, false)) return false;
        t[0] = token(s, USE_ALIAS, 99); t[1] = token(s, REGISTER_TOKEN, b);
        if (!ordinary(s, role, t, 2, false, true)) return false;
        t[0] = token(s, USE_ALIAS, b);
        if (!ordinary(s, role, t, 1, false, false)) return false;
        t[0] = token(s, DELETE_TOKEN, b);
        if (!ordinary(s, role, t, 1, false, true)) return false;
    }
    if (!pressure(s, role)) return false;
    for (unsigned i = 0; i < RANDOM_REQUESTS; ++i) {
        unsigned a = (unsigned)(random_next(s) % ALIASES);
        unsigned op = (unsigned)(random_next(s) % OPS);
        if (op == REGISTER_TOKEN && s->aliases[role][a].live) op = DELETE_TOKEN;
        token_t t = token(s, op, a);
        bool deny = (random_next(s) & 1) != 0;
        bool fragmented = (random_next(s) & 1) != 0;
        if (!ordinary(s, role, &t, 1, deny, fragmented)) return false;
    }
    return true;
}

/* D18 10.2.2 / D22 8.9: resolved duplicates reject only the request. A
 * REGISTER after the offending pair still commits; exercise its wire use
 * only after observing the rejection response on the registering stream. */
static bool duplicate_resolved(state_t *s, unsigned role, unsigned kind)
{
    for (unsigned fragment = 0; fragment < 2; ++fragment) {
        token_t value = token(s, USE_VALUE, 0);
        if (kind == DUPLICATE_ALIAS) {
            token_t registration = value;
            registration.op = REGISTER_TOKEN;
            if (!ordinary(s, role, &registration, 1, false, false)) return false;
        }
        token_t t[TOKENS] = {value, value, token(s, REGISTER_TOKEN, 1)};
        if (kind == DUPLICATE_ALIAS) t[0].op = USE_ALIAS;
        t[2].value.type = (value.value.type + 1) % 8;
        request_t q;
        if (!issue(s, &q, role, t, TOKENS, false, fragment != 0, false) ||
            !check(s, "duplicate.resolved.model", !q.fatal && q.error == 0x4) ||
            !finish(s, &q, kind)) return false;

        token_t use = token(s, USE_ALIAS, 1);
        const uint64_t accepted = s->result.accepted;
        if (!ordinary(s, role, &use, 1, false, fragment == 0) ||
            !check(s, "duplicate.postreject.cache.accepted", s->result.accepted == accepted + 1)) return false;
        s->result.cache_progress[role][kind]++;
        token_t del = token(s, DELETE_TOKEN, 1);
        if (!ordinary(s, role, &del, 1, false, false) ||
            !ordinary(s, role, &use, 1, false, true)) return false;

        t[1].value.type = (value.value.type + 1) % 8;
        if (!ordinary(s, role, t, 2, false, fragment != 0)) return false;
        s->result.distinct_types[role]++;
        if (kind == DUPLICATE_ALIAS) {
            del.alias = 0;
            if (!ordinary(s, role, &del, 1, false, false)) return false;
        }
    }
    return true;
}

static bool duplicate(state_t *s, unsigned role)
{
    token_t t = token(s, REGISTER_TOKEN, 1);
    if (!ordinary(s, role, &t, 1, false, true)) return false;
    t.value.bytes[0] = t.value.bytes[0] == 'z' ? 'a' : (uint8_t)(t.value.bytes[0] + 1);
    request_t q;
    if (!issue(s, &q, role, &t, 1, false, false, false) ||
        !check(s, "duplicate.model.fatal", q.fatal)) return false;
    moq_session_t *session = s->session[1 - role];
    if (!check(s, "duplicate.session.closed", moq_session_state(session) == MOQ_SESS_CLOSED)) return false;
    moq_event_t e;
    size_t n = 0;
    moq_result_t rc = moq_session_poll_events_ex(session, &e, 1, sizeof(e), &n);
    bool ok = check(s, "duplicate.event.poll.result", rc == MOQ_OK) &&
        check(s, "duplicate.event.single", n == 1);
    if (n) {
        ok = check(s, "duplicate.event.code", e.kind == MOQ_EVENT_SESSION_CLOSED && e.u.closed.code == 0x14) && ok;
        moq_event_cleanup(&e);
    }
    moq_action_t action;
    n = 0;
    rc = moq_session_poll_actions_ex(session, &action, 1, sizeof(action), &n);
    ok = check(s, "duplicate.action.poll.result", rc == MOQ_OK) && ok;
    ok = check(s, "duplicate.action.single", n == 1) && ok;
    if (n) {
        ok = check(s, "duplicate.action.code", action.kind == MOQ_ACTION_CLOSE_SESSION &&
            action.u.close_session.code == 0x14) && ok;
        moq_action_cleanup(&action);
    }
    if (!ok || !no_events(s, session) || !no_actions(s, session)) return false;
    s->result.duplicate_closes[role]++;
    logical(s, 0x14);
    return true;
}

static bool run(moq_version_t version, uint64_t seed, unsigned mode, summary_t *result)
{
    state_t s = {0};
    s.version = version; s.seed = seed; s.rng = seed; s.now = 1;
    s.result.hash = UINT64_C(0xcbf29ce484222325);
    moq_alloc_t allocator = {.ctx = &s, .alloc = alloc_bytes, .realloc = realloc_bytes, .free = free_bytes};
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &allocator; cfg.version = version; cfg.seed = seed;
    cfg.max_events = 1; cfg.max_actions = 16; cfg.auth_token_cache_size = 1024;
    if (!check(&s, "pair.create.result", moq_simpair_create(&cfg, &s.pair) == MOQ_OK)) goto done;
    s.session[0] = moq_simpair_client(s.pair); s.session[1] = moq_simpair_server(s.pair);
    if (!check(&s, "pair.start.result", moq_simpair_start(s.pair) == MOQ_OK) ||
        !check(&s, "pair.handshake.result", moq_simpair_run_until_quiescent(s.pair, 32, NULL) == MOQ_OK)) goto done;
    s.now = moq_simpair_now_us(s.pair);
    for (unsigned side = 0; side < 2; ++side) {
        if (!check(&s, "version.actual", moq_session_version(s.session[side]) == version) ||
            !check(&s, "session.established", moq_session_state(s.session[side]) == MOQ_SESS_ESTABLISHED) ||
            !check(&s, "peer.cache.advertised", moq_session_peer_auth_token_cache_size(s.session[side]) == 1024)) goto done;
        moq_event_t e;
        size_t n = 0;
        moq_result_t rc = moq_session_poll_events_ex(s.session[side], &e, 1, sizeof(e), &n);
        bool ok = check(&s, "setup.poll.result", rc == MOQ_OK) && check(&s, "setup.event.single", n == 1);
        if (n) { ok = check(&s, "setup.event.kind", e.kind == MOQ_EVENT_SETUP_COMPLETE) && ok; moq_event_cleanup(&e); }
        if (!ok || !no_events(&s, s.session[side]) || !no_actions(&s, s.session[side])) goto done;
    }
    if (mode >= 3) {
        const unsigned role = (mode - 3) % 2;
        const unsigned kind = (mode - 3) / 2;
        if (!check(&s, "duplicate.kind.valid", kind < DUPLICATE_KINDS) ||
            !duplicate_resolved(&s, role, kind)) goto done;
        if (!check(&s, "summary.duplicate.class", s.result.duplicate_rejects[role][kind] == 2) ||
            !check(&s, "summary.postreject.cache", s.result.cache_progress[role][kind] == 2) ||
            !check(&s, "summary.distinct.types", s.result.distinct_types[role] == 2) ||
            !check(&s, "summary.unknown.distinct", s.result.unknown == 2) ||
            !check(&s, "summary.duplicate.requests", s.result.requests == (kind == DUPLICATE_ALIAS ? 14u : 10u)) ||
            !check(&s, "summary.duplicate.events", s.result.events == s.result.accepted) ||
            !check(&s, "summary.duplicate.outcomes", s.result.accepted + s.result.unknown + 2 == s.result.requests) ||
            !check(&s, "summary.duplicate.retired", s.result.retired == s.result.requests)) goto done;
    } else if (mode) {
        if (!duplicate(&s, mode - 1)) goto done;
    } else {
        if (!sequence(&s, 0) || !sequence(&s, 1)) goto done;
        if (!check(&s, "summary.requests", s.result.requests == 76) ||
            !check(&s, "summary.retired", s.result.retired == s.result.requests) ||
            !check(&s, "summary.outcomes", s.result.accepted && s.result.unauthorized && s.result.unknown &&
                s.result.accepted + s.result.unauthorized + s.result.unknown == s.result.requests)) goto done;
        for (unsigned role = 0; role < 2; ++role) {
            if (!check(&s, "summary.pressure.per.role", s.result.pressures[role] == 1) ||
                !check(&s, "summary.distinct.types", s.result.distinct_types[role] == 2)) goto done;
            for (unsigned op = 0; op < OPS; ++op)
                if (!check(&s, "summary.operation.nonzero", s.result.operations[role][op] > 0)) goto done;
        }
    }
done:
    moq_simpair_destroy(s.pair);
    if (!check(&s, "cleanup.balance", s.allocations == 0) || s.failed) {
        fprintf(stderr, "CLEANUP d%u seed=%llu mode=%u balance=%lld\n", (unsigned)version,
            (unsigned long long)seed, mode, (long long)s.allocations);
        return false;
    }
    *result = s.result;
    return true;
}

static bool same(const summary_t *a, const summary_t *b)
{
    return a->hash == b->hash && a->requests == b->requests && a->events == b->events &&
        a->accepted == b->accepted && a->unauthorized == b->unauthorized &&
        a->unknown == b->unknown && a->retired == b->retired &&
        !memcmp(a->operations, b->operations, sizeof(a->operations)) &&
        !memcmp(a->pressures, b->pressures, sizeof(a->pressures)) &&
        !memcmp(a->duplicate_closes, b->duplicate_closes, sizeof(a->duplicate_closes)) &&
        !memcmp(a->duplicate_rejects, b->duplicate_rejects, sizeof(a->duplicate_rejects)) &&
        !memcmp(a->cache_progress, b->cache_progress, sizeof(a->cache_progress)) &&
        !memcmp(a->distinct_types, b->distinct_types, sizeof(a->distinct_types));
}

int main(void)
{
    const char *selected = getenv("MOQ_SIM_VERSION");
    if (selected && strcmp(selected, "18") && strcmp(selected, "22")) {
        fprintf(stderr, "FAIL version.selection: require18 or22, got%s\n", selected);
        return 1;
    }
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    const uint64_t seeds[] = {1, UINT64_C(0x12345678), UINT64_C(0xdeadbeef), UINT64_MAX};
    unsigned pairs = 0, failures = 0;
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v) {
        if (selected && (unsigned)versions[v] != (unsigned)strtoul(selected, NULL, 10)) continue;
        for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
            for (unsigned mode = 0; mode < 7; ++mode) {
                summary_t a, b;
                bool first = run(versions[v], seeds[i], mode, &a);
                bool second = run(versions[v], seeds[i], mode, &b);
                pairs += 2;
                if (!first || !second) { failures++; continue; }
                if (!same(&a, &b)) { fprintf(stderr, "FAIL twin.summary\n"); return 1; }
                printf("d%u seed=%llu mode=%u requests=%llu events=%llu accepted=%llu unauthorized=%llu unknown=%llu retired=%llu hash=%016llx balance=0\n",
                    (unsigned)versions[v], (unsigned long long)seeds[i], mode,
                    (unsigned long long)a.requests, (unsigned long long)a.events,
                    (unsigned long long)a.accepted, (unsigned long long)a.unauthorized,
                    (unsigned long long)a.unknown, (unsigned long long)a.retired,
                    (unsigned long long)a.hash);
                for (unsigned role = 0; role < 2; ++role)
                    printf("  peer=%s delete=%llu register=%llu alias=%llu value=%llu pressure=%llu duplicate.close=%llu "
                        "duplicate.values=%llu duplicate.alias=%llu cache.values=%llu cache.alias=%llu distinct.types=%llu\n",
                        role ? "server" : "client", (unsigned long long)a.operations[role][DELETE_TOKEN],
                        (unsigned long long)a.operations[role][REGISTER_TOKEN],
                        (unsigned long long)a.operations[role][USE_ALIAS],
                        (unsigned long long)a.operations[role][USE_VALUE],
                        (unsigned long long)a.pressures[role], (unsigned long long)a.duplicate_closes[role],
                        (unsigned long long)a.duplicate_rejects[role][DUPLICATE_VALUES],
                        (unsigned long long)a.duplicate_rejects[role][DUPLICATE_ALIAS],
                        (unsigned long long)a.cache_progress[role][DUPLICATE_VALUES],
                        (unsigned long long)a.cache_progress[role][DUPLICATE_ALIAS],
                        (unsigned long long)a.distinct_types[role]);
            }
        }
    }
    if (failures) {
        fprintf(stderr, "FAIL request-bidi authorization failed.twins=%u pairs=%u\n", failures, pairs);
        return 1;
    }
    printf("PASS request-bidi authorization pairs=%u\n", pairs);
    return 0;
}
