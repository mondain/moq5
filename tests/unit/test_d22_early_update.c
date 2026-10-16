/* Literal actual ingress; --controls retains separately characterized D18 behavior. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "../../core/src/session/request_stream_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    const char *name;
    moq_simpair_t *pair;
    moq_session_t *s;
    moq_alloc_t alloc;
    size_t live;
    bool fail_alloc;
    size_t alloc_calls;
    size_t fail_at;
    uint32_t max_actions;
    uint64_t cache_size;
    int failures;
    bool publish;
    uint64_t handle;
    moq_stream_ref_t ref;
    uint8_t id;
    uint64_t now;
} fixture_t;

static void *count_alloc(size_t n, void *ctx)
{
    fixture_t *f = ctx;
    f->alloc_calls++;
    if (f->fail_alloc || (f->fail_at && f->alloc_calls == f->fail_at)) return NULL;
    void *p = malloc(n);
    if (p) f->live++;
    return p;
}

static void *count_realloc(void *p, size_t old, size_t n, void *ctx)
{
    fixture_t *f = ctx;
    (void)old;
    if (!p) return count_alloc(n, ctx);
    if (!n) { free(p); f->live--; return NULL; }
    f->alloc_calls++;
    if (f->fail_alloc || (f->fail_at && f->alloc_calls == f->fail_at)) return NULL;
    return realloc(p, n);
}

static void count_free(void *p, size_t n, void *ctx)
{
    fixture_t *f = ctx;
    (void)n;
    if (p) f->live--;
    free(p);
}

static bool check(fixture_t *f, bool ok, const char *tag)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s\n", f->name, tag);
        f->failures++;
    }
    return ok;
}

static bool drain_events(fixture_t *f, moq_session_t *s)
{
    for (;;) {
        moq_event_t e = {0};
        size_t n = 0;
        moq_result_t rc = moq_session_poll_events_ex(s, &e, 1, sizeof(e), &n);
        if (!check(f, rc == MOQ_OK, "poll.events")) return false;
        if (!n) return true;
        moq_event_cleanup(&e);
    }
}

static bool drain_actions(fixture_t *f)
{
    for (;;) {
        moq_action_t a = {0};
        size_t n = 0;
        moq_result_t rc = moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n);
        if (!check(f, rc == MOQ_OK, "poll.actions")) return false;
        if (!n) return true;
        moq_action_cleanup(&a);
    }
}

static bool open_fixture(fixture_t *f, moq_version_t version, bool peer_server)
{
    f->alloc = (moq_alloc_t){ .ctx = f, .alloc = count_alloc,
                             .realloc = count_realloc, .free = count_free };
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &f->alloc;
    cfg.version = version;
    cfg.auth_token_cache_size = f->cache_size ? f->cache_size : 256;
    cfg.max_actions = f->max_actions;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    if (!check(f, moq_simpair_create(&cfg, &f->pair) == MOQ_OK && f->pair,
               "pair.create")) return false;
    if (!check(f, moq_simpair_start(f->pair) == MOQ_OK, "pair.start")) return false;
    if (!check(f, moq_simpair_run_until_quiescent(f->pair, 16, NULL) == MOQ_OK,
               "pair.pump")) return false;
    moq_session_t *c = moq_simpair_client(f->pair);
    moq_session_t *s = moq_simpair_server(f->pair);
    if (!check(f, moq_session_version(c) == version && moq_session_version(s) == version &&
               moq_session_state(c) == MOQ_SESS_ESTABLISHED &&
               moq_session_state(s) == MOQ_SESS_ESTABLISHED, "pair.actual.version.state"))
        return false;
    f->s = peer_server ? c : s;
    f->id = peer_server ? 1 : 0;
    f->ref = moq_stream_ref_from_u64(0x7800);
    f->now = 100;
    return drain_events(f, c) && drain_events(f, s) && drain_actions(f);
}

static int close_fixture(fixture_t *f)
{
    if (f->pair) moq_simpair_destroy(f->pair);
    check(f, f->live == 0, "allocator.balance.zero");
    return f->failures;
}

/* Namespace n, track v; REGISTER alias1/type0/value a. Both drafts share these
 * particular initial/auth encodings. No product encoder supplies an oracle. */
static size_t initial_wire(const fixture_t *f, uint8_t *out)
{
    static const uint8_t sub[] = {
        3, 0, 13, 0, 1, 1, 'n', 1, 'v', 1, 3, 4, 1, 1, 0, 'a'
    };
    static const uint8_t pub[] = {
        0x1d, 0, 14, 0, 1, 1, 'n', 1, 'v', 1, 1, 3, 4, 1, 1, 0, 'a'
    };
    size_t n = f->publish ? sizeof(pub) : sizeof(sub);
    memcpy(out, f->publish ? pub : sub, n);
    out[3] = f->id;
    return n;
}

/* REGISTER alias2/type0/value b, then priority17. KVP type delta 0x20-3=29. */
static size_t update_wire(const fixture_t *f, uint8_t *out)
{
    static const uint8_t update[] = { 2, 0, 10, 0, 2, 3, 4, 1, 2, 0, 'b', 29, 17 };
    memcpy(out, update, sizeof(update));
    out[3] = (uint8_t)(f->id + 2);
    return sizeof(update);
}

static moq_result_t ingress(fixture_t *f, const uint8_t *p, size_t n, bool fin)
{
    return moq_session_on_bidi_stream_bytes(f->s, f->ref, p, n, fin, f->now++);
}

static bool token_is(const moq_resolved_token_t *t, char value)
{
    return t && t->token_type == 0 && t->token_value.len == 1 &&
           t->token_value.data && t->token_value.data[0] == (uint8_t)value;
}

static bool initial_event(fixture_t *f)
{
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_events_ex(f->s, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "initial.event.present")) return false;
    bool ok;
    if (f->publish) {
        ok = e.kind == MOQ_EVENT_PUBLISH_REQUEST;
        if (ok) {
            f->handle = e.u.publish_request.pub._opaque;
            ok = e.u.publish_request.token_count == 1 &&
                 token_is(e.u.publish_request.tokens, 'a');
        }
    } else {
        ok = e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST;
        if (ok) {
            f->handle = e.u.subscribe_request.sub._opaque;
            ok = e.u.subscribe_request.token_count == 1 &&
                 token_is(e.u.subscribe_request.tokens, 'a');
        }
    }
    moq_event_cleanup(&e);
    return check(f, ok && f->handle != 0, "initial.event.kind.tokens.handle");
}

static moq_result_t answer(fixture_t *f, bool reject)
{
    if (f->publish) {
        moq_publication_t h = { f->handle };
        if (reject) {
            moq_reject_publish_cfg_t cfg;
            moq_reject_publish_cfg_init(&cfg);
            cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
            return moq_session_reject_publish(f->s, h, &cfg, f->now++);
        }
        moq_accept_publish_cfg_t cfg;
        moq_accept_publish_cfg_init_sized(&cfg, sizeof(cfg));
        return moq_session_accept_publish(f->s, h, &cfg, f->now++);
    }
    moq_subscription_t h = { f->handle };
    if (reject) {
        moq_reject_subscribe_cfg_t cfg;
        moq_reject_subscribe_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
        return moq_session_reject_subscribe(f->s, h, &cfg, f->now++);
    }
    moq_accept_subscribe_cfg_t cfg;
    moq_accept_subscribe_cfg_init(&cfg);
    cfg.track_alias = 1;
    return moq_session_accept_subscribe(f->s, h, &cfg, f->now++);
}

static bool wire_action(fixture_t *f, const uint8_t *bytes, size_t len, bool fin)
{
    moq_action_t a = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "wire.action.present")) return false;
    bool ok = a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
        a.u.send_bidi_stream.stream_ref._v == f->ref._v &&
        a.u.send_bidi_stream.fin == fin && a.u.send_bidi_stream.len == len &&
        memcmp(a.u.send_bidi_stream.data, bytes, len) == 0;
    if (!ok) {
        fprintf(stderr, "WIRE %s kind=%d", f->name, (int)a.kind);
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            fprintf(stderr, " fin=%d bytes=", a.u.send_bidi_stream.fin);
            for (size_t i = 0; i < a.u.send_bidi_stream.len; i++)
                fprintf(stderr, "%02x", a.u.send_bidi_stream.data[i]);
        }
        fputc('\n', stderr);
    }
    moq_action_cleanup(&a);
    return check(f, ok, "wire.exact.bytes.ref.fin");
}

static bool no_actions(fixture_t *f)
{
    moq_action_t a = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK,
               "no.actions.poll")) return false;
    if (n) moq_action_cleanup(&a);
    return check(f, n == 0, "no.actions");
}

static bool no_events(fixture_t *f)
{
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_events_ex(f->s, &e, 1, sizeof(e), &n) == MOQ_OK,
               "no.events.poll")) return false;
    if (n) moq_event_cleanup(&e);
    return check(f, n == 0, "no.events");
}

static bool protocol_close(fixture_t *f, uint64_t code)
{
    moq_action_t a = {0};
    size_t n = 0;
    if (!check(f, moq_session_state(f->s) == MOQ_SESS_CLOSED &&
               moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "protocol.close.present")) return false;
    bool ok = a.kind == MOQ_ACTION_CLOSE_SESSION && a.u.close_session.code == code;
    moq_action_cleanup(&a);
    return check(f, ok, "protocol.close.exact.code") && no_actions(f);
}

static bool stop_action(fixture_t *f)
{
    moq_action_t a = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "cancel.stop.present")) return false;
    bool ok = a.kind == MOQ_ACTION_STOP_BIDI_STREAM &&
              a.u.stop_bidi_stream.stream_ref._v == f->ref._v &&
              a.u.stop_bidi_stream.error_code == 1;
    moq_action_cleanup(&a);
    return check(f, ok, "cancel.stop.ref.code1");
}

static bool publish_finished(fixture_t *f)
{
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_events_ex(f->s, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "publish.finished.present")) return false;
    bool ok_event = e.kind == MOQ_EVENT_PUBLISH_FINISHED &&
                    e.u.publish_finished.pub._opaque == f->handle &&
                    e.u.publish_finished.status_code == 0 &&
                    e.u.publish_finished.stream_count == 0;
    moq_event_cleanup(&e);
    if (!check(f, ok_event, "publish.finished.same.handle.status.count")) return false;
    moq_action_t a = {0};
    n = 0;
    if (!check(f, moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "publish.reciprocal.fin.present")) return false;
    bool ok_fin = a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM &&
                  a.u.close_bidi_stream.stream_ref._v == f->ref._v;
    moq_action_cleanup(&a);
    return check(f, ok_fin, "publish.reciprocal.fin.same.ref");
}

static bool pressure(fixture_t *f, unsigned which)
{
    if (which == 1) {
        moq_action_t a = {0};
        a.kind = MOQ_ACTION_SEND_CONTROL;
        while (!action_queue_full(f->s))
            if (!check(f, push_action(f->s, &a) == MOQ_OK, "pressure.action.push"))
                return false;
    } else if (which == 2) {
        moq_event_t e = {0};
        e.kind = MOQ_EVENT_GOAWAY;
        while (!event_queue_full(f->s))
            if (!check(f, push_event(f->s, &e) == MOQ_OK, "pressure.event.push"))
                return false;
    }
    return true;
}

static bool clear_pressure(fixture_t *f, unsigned which)
{
    return which == 1 ? drain_actions(f) : which == 2 ? drain_events(f, f->s) : true;
}

static bool queued_pressure_actions(fixture_t *f, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        moq_action_t a = {0};
        size_t n = 0;
        if (!check(f, moq_session_poll_actions_ex(f->s, &a, 1, sizeof(a), &n) == MOQ_OK &&
                   n == 1, "progress.earlier.action.present")) return false;
        bool ok = a.kind == MOQ_ACTION_SEND_CONTROL &&
                  a.u.send_control.data == NULL && a.u.send_control.len == 0;
        moq_action_cleanup(&a);
        if (!check(f, ok, "progress.earlier.queued.actions.preserved.in.order")) return false;
    }
    return true;
}

static bool ids_cache(fixture_t *f, bool updated)
{
    const moq_request_stream_state_t *rs = f->s->profile_state;
    return check(f, rs->peer_next_request_id == (uint64_t)f->id + (updated ? 4 : 2) &&
                 rs->peer_seen_count == 0, "peer.id.commit.order") &&
        check(f, f->s->peer_token_cache.used_bytes == (updated ? 34u : 17u),
              "cache.commit.order") &&
        check(f, moq_token_cache_lookup(&f->s->peer_token_cache, 2, NULL, NULL, NULL) ==
                  (updated ? MOQ_TOKEN_OK : MOQ_TOKEN_ERR_UNKNOWN), "alias2.commit.order");
}

static bool pending_retained(fixture_t *f, const uint8_t *bytes, size_t len, bool fin)
{
    moq_request_endpoint_t ep = request_registry_find_by_streamref(f->s, f->ref);
    const moq_request_stream_t *rs = NULL;
    bool pending = false;
    bool observed_fin = false;
    if (f->publish && ep.kind == MOQ_REQ_PUBLISH && ep.slot >= 0 &&
        (size_t)ep.slot < f->s->pub_cap) {
        const moq_pub_entry_t *p = &f->s->publishes[ep.slot];
        rs = &p->rs;
        pending = p->state == MOQ_PUB_PENDING_SUBSCRIBER;
        observed_fin = p->rs.recv_fin || p->handoff_fin_pending;
        for (size_t i = 0; i < f->s->sub_cap; i++)
            if (!check(f, f->s->subs[i].state != MOQ_SUB_RECVING_REQUEST ||
                       f->s->subs[i].rs.ref._v != f->ref._v,
                       "publish.staging.released")) return false;
    } else if (!f->publish && ep.kind == MOQ_REQ_SUBSCRIPTION && ep.slot >= 0 &&
               (size_t)ep.slot < f->s->sub_cap) {
        rs = &f->s->subs[ep.slot].rs;
        pending = f->s->subs[ep.slot].state == MOQ_SUB_PENDING_PUBLISHER;
        observed_fin = rs->recv_fin;
    }
    return check(f, rs && pending && rs->recv_len == len && observed_fin == fin &&
                 memcmp(rs->recv_buf, bytes, len) == 0 &&
                 moq_session_has_transport_stream(f->s, f->ref),
                 "pending.actual.owner.bytes.fin");
}

static void observation(fixture_t *f, const char *step, moq_result_t rc)
{
    const moq_request_stream_state_t *rs = f->s->profile_state;
    moq_request_endpoint_t ep = request_registry_find_by_streamref(f->s, f->ref);
    size_t len = 0;
    bool fin = false;
    if (ep.slot >= 0 && ep.kind == MOQ_REQ_SUBSCRIPTION && (size_t)ep.slot < f->s->sub_cap) {
        len = f->s->subs[ep.slot].rs.recv_len;
        fin = f->s->subs[ep.slot].rs.recv_fin;
    } else if (ep.slot >= 0 && ep.kind == MOQ_REQ_PUBLISH && (size_t)ep.slot < f->s->pub_cap) {
        len = f->s->publishes[ep.slot].rs.recv_len;
        fin = f->s->publishes[ep.slot].rs.recv_fin || f->s->publishes[ep.slot].handoff_fin_pending;
    }
    printf("OBS %s step=%s rc=%d session=%d owner=%d retained=%zu fin=%d id=%llu cache=%zu\n",
           f->name, step, rc, (int)f->s->state, (int)ep.kind, len, fin,
           (unsigned long long)rs->peer_next_request_id, f->s->peer_token_cache.used_bytes);
}

static bool update_event(fixture_t *f)
{
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(f, moq_session_poll_events_ex(f->s, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "update.event.present")) return false;
    bool ok = false;
    if (f->publish && e.kind == MOQ_EVENT_PUBLISH_UPDATED) {
        ok = e.u.publish_updated.pub._opaque == f->handle &&
             e.u.publish_updated.has_subscriber_priority && e.u.publish_updated.subscriber_priority == 17 &&
             e.u.publish_updated.token_count == 1 && token_is(e.u.publish_updated.tokens, 'b');
    } else if (!f->publish && e.kind == MOQ_EVENT_SUBSCRIBE_UPDATED) {
        ok = e.u.subscribe_updated.sub._opaque == f->handle &&
             e.u.subscribe_updated.has_subscriber_priority && e.u.subscribe_updated.subscriber_priority == 17 &&
             e.u.subscribe_updated.token_count == 1 && token_is(e.u.subscribe_updated.tokens, 'b');
    }
    moq_event_cleanup(&e);
    return check(f, ok, "update.event.exact.tokens.priority.handle");
}

static bool initial_response(fixture_t *f, bool reject)
{
    static const uint8_t sub_ok[] = { 4, 0, 2, 1, 0 };
    static const uint8_t pub_ok[] = { 7, 0, 1, 0 };
    /* Error3, retry interval0, empty reason; no redirect unless REDIRECT. */
    static const uint8_t error[] = { 5, 0, 3, 3, 0, 0 };
    return reject ? wire_action(f, error, sizeof(error), true) : f->publish
        ? wire_action(f, pub_ok, sizeof(pub_ok), false)
        : wire_action(f, sub_ok, sizeof(sub_ok), false);
}

typedef struct early_case {
    const char *name;
    unsigned arrival; /* 0 coalesced, 1 whole update separately, 2 split update */
    bool fin;
    bool reject;
    unsigned pressure;
} early_case_t;

static int early_probe(bool publish, bool peer_server, const early_case_t *c)
{
    char name[96];
    snprintf(name, sizeof(name), "%s.peer-%s.%s", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client", c->name);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32], all[80];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    /* A publisher must send PUBLISH_DONE before FIN; no objects/streams opened. */
    static const uint8_t done[] = { 0x0b, 0, 3, 0, 0, 0 };
    memcpy(all, initial, ni);
    memcpy(all + ni, update, nu);
    size_t total = ni + nu;
    if (c->fin && publish) { memcpy(all + total, done, sizeof(done)); total += sizeof(done); }
    moq_result_t rc;
    if (c->arrival == 0) {
        if (!pressure(&f, c->pressure)) goto out;
        rc = ingress(&f, all, total, c->fin);
    } else {
        if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
            !initial_event(&f) || !ids_cache(&f, false) || !no_actions(&f) ||
            !pressure(&f, c->pressure)) goto out;
        size_t part = c->arrival == 2 ? 2 : total - ni;
        rc = ingress(&f, all + ni, part, c->arrival == 1 && c->fin);
        observation(&f, "update.first", rc);
        if (!check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED,
                   "legal.early.update.must.not.close")) goto out;
        if (c->arrival == 2) rc = ingress(&f, all + ni + part, total - ni - part, c->fin);
    }
    observation(&f, "update.complete", rc);
    if (!check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED,
               "legal.early.update.must.not.close")) goto out;
    if (rc == MOQ_ERR_WOULD_BLOCK && c->pressure == 2 && c->arrival == 0) {
        if (!clear_pressure(&f, c->pressure)) goto out;
        rc = ingress(&f, NULL, 0, false);
        observation(&f, "initial.empty.retry", rc);
        if (!check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED,
                   "retry.must.not.close")) goto out;
    }
    if (!check(&f, rc == MOQ_ERR_WOULD_BLOCK, "complete.pending.update.retained.block")) goto out;
    if (c->arrival == 0 && !initial_event(&f)) goto out;
    if (!ids_cache(&f, false) ||
        !pending_retained(&f, all + ni, total - ni, c->fin)) goto out;
    memset(all, 0, sizeof(all));
    if (c->pressure == 1) {
        if (!check(&f, answer(&f, c->reject) == MOQ_ERR_WOULD_BLOCK,
                   "initial.answer.action.refusal") ||
            !ids_cache(&f, false) || !clear_pressure(&f, 1)) goto out;
    } else if (c->pressure == 2 && !pressure(&f, 2)) goto out;
    /* This remains the app's decision; no automatic initial response is allowed. */
    if (!no_actions(&f) || !check(&f, answer(&f, c->reject) == MOQ_OK, "app.initial.answer") ||
        !initial_response(&f, c->reject)) goto out;
    if (c->reject) {
        if (!ids_cache(&f, true) || (!c->fin && !stop_action(&f)) ||
            !no_actions(&f) || !no_events(&f)) goto out;
        moq_request_endpoint_t ep = request_registry_find_by_streamref(f.s, f.ref);
        if (!check(&f, ep.slot < 0, "rejected.owner.retired") ||
            !check(&f, answer(&f, true) == MOQ_ERR_STALE_HANDLE,
                   "rejected.answer.not.repeated")) goto out;
        if (!c->fin && !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK,
                             "cancel.peer.fin.drained")) goto out;
        check(&f, !moq_session_has_transport_stream(f.s, f.ref), "cancel.ref.retired");
    } else {
        if (c->pressure == 1 && !pressure(&f, 1)) goto out;
        if (c->pressure) {
            if (!check(&f, ingress(&f, NULL, 0, false) == MOQ_ERR_WOULD_BLOCK,
                       "accepted.initial.update.capacity.refusal") ||
                !ids_cache(&f, false) || !clear_pressure(&f, c->pressure)) goto out;
        }
        rc = ingress(&f, NULL, 0, false);
        static const uint8_t ok[] = { 7, 0, 1, 0 };
        if (!check(&f, rc == MOQ_OK, "update.empty.retry") ||
            !wire_action(&f, ok, sizeof(ok), false) || !update_event(&f) ||
            !ids_cache(&f, true)) goto out;
        if (publish && c->fin && !publish_finished(&f)) goto out;
        if (!no_actions(&f) || !no_events(&f)) goto out;
    }
    check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "early.final.session.live");
out:
    return close_fixture(&f);
}

static int control(bool publish, bool peer_server, bool reject, unsigned capacity)
{
    char name[96];
    snprintf(name, sizeof(name), "control.%s.peer-%s.%s.pressure%u", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client", reject ? "reject" : "accept", capacity);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) || !ids_cache(&f, false) || !no_actions(&f)) goto out;
    if (capacity == 1) {
        if (!pressure(&f, 1) || !check(&f, answer(&f, reject) == MOQ_ERR_WOULD_BLOCK,
                                     "app.answer.action.refusal") ||
            !ids_cache(&f, false) || !clear_pressure(&f, 1)) goto out;
    }
    if (!check(&f, answer(&f, reject) == MOQ_OK, "app.initial.answer") ||
        !initial_response(&f, reject) || !no_actions(&f)) goto out;
    if (!reject && !pressure(&f, capacity)) goto out;
    moq_result_t rc = ingress(&f, update, nu, false);
    observation(&f, "established.update", rc);
    if (!check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "control.session.live")) goto out;
    if (reject) {
        if (!check(&f, rc == MOQ_OK, "terminal.inflight.drained") ||
            !ids_cache(&f, false) || !no_actions(&f)) goto out;
    } else {
        if (capacity) {
            if (!check(&f, rc == MOQ_ERR_WOULD_BLOCK, "update.capacity.refusal") ||
                !ids_cache(&f, false) || !clear_pressure(&f, capacity)) goto out;
            rc = ingress(&f, NULL, 0, false);
        }
        static const uint8_t ok[] = { 7, 0, 1, 0 };
        if (!check(&f, rc == MOQ_OK, "update.retry.result") ||
            !wire_action(&f, ok, sizeof(ok), false) || !update_event(&f) ||
            !ids_cache(&f, true) || !no_actions(&f)) goto out;
    }
out:
    return close_fixture(&f);
}

static int old18_control(bool publish, bool peer_server)
{
    char name[64];
    snprintf(name, sizeof(name), "D18.characterization.%s.peer-%s", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client");
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_18, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f)) goto out;
    moq_result_t rc = ingress(&f, update, nu, false);
    observation(&f, "old.pending.update", rc);
    if (!check(&f, rc == MOQ_OK && moq_session_state(f.s) == MOQ_SESS_CLOSED,
               "old18.current.behavior.close")) goto out;
    moq_action_t a = {0};
    size_t n = 0;
    if (!check(&f, moq_session_poll_actions_ex(f.s, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "old18.close.present")) goto out;
    bool ok = a.kind == MOQ_ACTION_CLOSE_SESSION && a.u.close_session.code == 3;
    moq_action_cleanup(&a);
    check(&f, ok, "old18.close.code3");
out:
    return close_fixture(&f);
}

static int fin_control(bool publish, bool peer_server)
{
    char name[80];
    snprintf(name, sizeof(name), "control.%s.peer-%s.fin.action", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client");
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], tail[32];
    size_t ni = initial_wire(&f, initial), nt = update_wire(&f, tail);
    if (publish) {
        static const uint8_t done[] = { 0x0b, 0, 3, 0, 0, 0 };
        memcpy(tail + nt, done, sizeof(done));
        nt += sizeof(done);
    }
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) || !check(&f, answer(&f, false) == MOQ_OK, "app.accept") ||
        !initial_response(&f, false) || !pressure(&f, 1)) goto out;
    moq_result_t rc = ingress(&f, tail, nt, true);
    observation(&f, "fin.refusal", rc);
    if (!check(&f, rc == MOQ_ERR_WOULD_BLOCK, "fin.capacity.refusal") ||
        !ids_cache(&f, false)) goto out;
    moq_request_endpoint_t ep = request_registry_find_by_streamref(f.s, f.ref);
    bool retained = ep.slot >= 0 &&
        (publish ? ep.kind == MOQ_REQ_PUBLISH && (size_t)ep.slot < f.s->pub_cap &&
                   f.s->publishes[ep.slot].rs.recv_len == nt && f.s->publishes[ep.slot].rs.recv_fin
                 : ep.kind == MOQ_REQ_SUBSCRIPTION && (size_t)ep.slot < f.s->sub_cap &&
                   f.s->subs[ep.slot].rs.recv_len == nt && f.s->subs[ep.slot].rs.recv_fin);
    if (!check(&f, retained, "fin.retained.owner.bytes") || !clear_pressure(&f, 1)) goto out;
    memset(tail, 0, sizeof(tail));
    static const uint8_t ok[] = { 7, 0, 1, 0 };
    if (!check(&f, ingress(&f, NULL, 0, false) == MOQ_OK, "fin.empty.retry") ||
        !wire_action(&f, ok, sizeof(ok), false) || !update_event(&f) || !ids_cache(&f, true)) goto out;
    if (publish && !publish_finished(&f)) goto out;
    check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "fin.session.live");
    no_actions(&f);
out:
    return close_fixture(&f);
}

static int partial_answer_control(bool publish, bool peer_server, bool reject)
{
    char name[80];
    snprintf(name, sizeof(name), "control.%s.peer-%s.partial.%s", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client",
             reject ? "reject" : "accept");
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) || !check(&f, ingress(&f, update, 2, false) == MOQ_OK,
                                   "partial.pending.ingress") ||
        !ids_cache(&f, false) || !no_actions(&f)) goto out;
    observation(&f, "partial.before.answer", MOQ_OK);
    if (!pressure(&f, 1) || !check(&f, answer(&f, reject) == MOQ_ERR_WOULD_BLOCK,
                                  "partial.answer.refusal") ||
        !ids_cache(&f, false) || !clear_pressure(&f, 1) ||
        !check(&f, answer(&f, reject) == MOQ_OK, "partial.answer.retry") ||
        !initial_response(&f, reject)) goto out;
    if (!check(&f, ingress(&f, update + 2, nu - 2, false) == MOQ_OK,
               "partial.final.fragment")) goto out;
    if (reject) {
        ids_cache(&f, false);
        no_actions(&f);
    } else {
        static const uint8_t ok[] = { 7, 0, 1, 0 };
        if (!wire_action(&f, ok, sizeof(ok), false) || !update_event(&f) ||
            !ids_cache(&f, true) || !no_actions(&f)) goto out;
    }
    check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "partial.session.live");
out:
    return close_fixture(&f);
}

static bool settle_early(fixture_t *f, bool reject)
{
    if (!check(f, answer(f, reject) == MOQ_OK, "early.answer.commit") ||
        !initial_response(f, reject)) return false;
    if (reject) {
        return ids_cache(f, true) && stop_action(f) && no_actions(f) && no_events(f) &&
            check(f, ingress(f, NULL, 0, true) == MOQ_OK, "early.cancel.peer.fin") &&
            check(f, !moq_session_has_transport_stream(f->s, f->ref), "early.cancel.retired");
    }
    static const uint8_t ok[] = { 7, 0, 1, 0 };
    return ids_cache(f, false) && no_actions(f) &&
        check(f, ingress(f, NULL, 0, false) == MOQ_OK, "early.accept.empty.retry") &&
        wire_action(f, ok, sizeof(ok), false) && update_event(f) &&
        ids_cache(f, true) && no_actions(f) && no_events(f) &&
        check(f, ingress(f, NULL, 0, false) == MOQ_OK, "early.no.duplicate.retry") &&
        no_actions(f) && no_events(f);
}

static int bytewise_probe(bool publish, bool peer_server, bool reject)
{
    char name[96];
    snprintf(name, sizeof(name), "bytewise.%s.peer-%s.%s", publish ? "PUBLISH" : "SUBSCRIBE",
             peer_server ? "server" : "client", reject ? "reject" : "accept");
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f)) goto out;
    for (size_t i = 0; i < nu; i++) {
        moq_result_t rc = ingress(&f, update + i, 1, false);
        if (!check(&f, rc == (i + 1 == nu ? MOQ_ERR_WOULD_BLOCK : MOQ_OK),
                   "bytewise.partial.or.complete.result") ||
            !pending_retained(&f, update, i + 1, false) ||
            !ids_cache(&f, false) || !no_actions(&f) || !no_events(&f)) goto out;
    }
    memset(update, 0, sizeof(update));
    settle_early(&f, reject);
out:
    return close_fixture(&f);
}

static int reject_refusal_probe(bool publish, bool peer_server, unsigned which)
{
    char name[96];
    static const char *const modes[] = {
        "one-spare", "drain-capacity", "send-bytes", "allocation", "full-actions"
    };
    snprintf(name, sizeof(name), "reject.transaction.%s.peer-%s.%s",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", modes[which - 1]);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) ||
        !check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
               "early.pending.retained")) goto out;
    if (which == 1 || which == 5) {
        if (!pressure(&f, 1)) goto out;
        if (which == 1 && !queued_pressure_actions(&f, 1)) goto out;
    } else if (which == 2) {
        for (size_t i = 0; i < f.s->drain_ref_cap; i++)
            if (!check(&f, drain_ref_add(f.s, moq_stream_ref_from_u64(0x100000 + i)),
                       "reject.drain.pressure")) goto out;
    } else if (which == 3) {
        size_t cap = f.s->send_cap;
        uint8_t *bytes = f.alloc.alloc(cap, f.alloc.ctx);
        if (!check(&f, bytes != NULL, "reject.send.pressure.allocate")) goto out;
        memset(bytes, 0, cap);
        moq_result_t rc = queue_send_bidi(f.s, moq_stream_ref_from_u64(0x7001),
                                         bytes, cap, false);
        f.alloc.free(bytes, cap, f.alloc.ctx);
        if (!check(&f, rc == MOQ_OK, "reject.send.pressure.queue")) goto out;
    } else {
        f.fail_alloc = true;
    }
    size_t live = f.live, send_len = f.s->send_len, drains = f.s->drain_ref_count;
    size_t head = f.s->action_head, tail = f.s->action_tail;
    moq_result_t rc = answer(&f, true);
    f.fail_alloc = false;
    if (which == 1) {
        if (!check(&f, rc == MOQ_OK && f.s->action_head == head &&
                   f.s->action_tail == tail + 1 && f.s->drain_ref_count == drains + 1 &&
                   moq_session_has_transport_stream(f.s, f.ref),
                   "reject.one.spare.commits.error.and.bounded.drain") ||
            !ids_cache(&f, true)) goto out;
        size_t calls = f.alloc_calls;
        if (!queued_pressure_actions(&f, tail - head) || !ids_cache(&f, true) ||
            !initial_response(&f, true) || !ids_cache(&f, true) ||
            !stop_action(&f) || !ids_cache(&f, true) || !no_actions(&f) || !no_events(&f) ||
            !check(&f, f.alloc_calls == calls, "reject.one.spare.poll.progress.has.no.allocation") ||
            !check(&f, answer(&f, true) == MOQ_ERR_STALE_HANDLE,
                   "reject.one.spare.answer.not.repeated") ||
            !ids_cache(&f, true) || !no_actions(&f) ||
            !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK &&
                       !moq_session_has_transport_stream(f.s, f.ref),
                   "reject.one.spare.final.fin.retires.drain")) goto out;
        check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "reject.one.spare.session.live");
        goto out;
    }
    if (!check(&f, rc == (which == 4 ? MOQ_ERR_NOMEM : MOQ_ERR_WOULD_BLOCK),
               "reject.transaction.refused") ||
        !check(&f, f.live == live && f.s->send_len == send_len &&
                   f.s->action_head == head && f.s->action_tail == tail &&
                   f.s->drain_ref_count == drains, "reject.refusal.inventory.unchanged") ||
        !pending_retained(&f, update, nu, false) || !ids_cache(&f, false) ||
        !no_events(&f)) goto out;
    if (which == 2) {
        for (size_t i = 0; i < f.s->drain_ref_cap; i++)
            if (!check(&f, drain_ref_remove(f.s, moq_stream_ref_from_u64(0x100000 + i)),
                       "reject.drain.pressure.release")) goto out;
    } else if (which != 4 && !drain_actions(&f)) goto out;
    if (!no_actions(&f)) goto out;
    settle_early(&f, true);
out:
    return close_fixture(&f);
}

static int alias_after_reject_probe(bool publish, bool peer_server, bool auth_failure)
{
    char name[96];
    snprintf(name, sizeof(name), "reject.alias.%s.peer-%s.auth-failure%d",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", auth_failure);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (auth_failure) {
        /* REGISTER2 remains mandatory even when subsequent USE_ALIAS3 fails. */
        static const uint8_t failed[] = {
            2, 0, 14, 0, 3, 3, 4, 1, 2, 0, 'b', 0, 2, 2, 3, 29, 17
        };
        memcpy(update, failed, sizeof(failed));
        update[3] = (uint8_t)(f.id + 2);
        nu = sizeof(failed);
    }
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) ||
        !check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
               "early.pending.retained") || !settle_early(&f, true)) goto out;
    /* A new request uses alias2, not the original UPDATE's borrowed bytes. */
    uint8_t use[] = { 3, 0, 11, 0, 1, 1, 'n', 1, 'w', 1, 3, 2, 2, 2 };
    use[3] = (uint8_t)(f.id + 4);
    f.ref = moq_stream_ref_from_u64(0x7900);
    if (!check(&f, ingress(&f, use, sizeof(use), false) == MOQ_OK,
               "next.request.use.alias2")) goto out;
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(&f, moq_session_poll_events_ex(f.s, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "alias.next.request.event")) goto out;
    bool ok = e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST &&
              e.u.subscribe_request.token_count == 1 &&
              token_is(e.u.subscribe_request.tokens, 'b');
    if (e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) f.handle = e.u.subscribe_request.sub._opaque;
    moq_event_cleanup(&e);
    if (!check(&f, ok, "alias2.resolves.to.registered.b")) goto out;
    const moq_request_stream_state_t *rs = f.s->profile_state;
    if (!check(&f, rs->peer_next_request_id == (uint64_t)f.id + 6 &&
               rs->peer_seen_count == 0 && f.s->peer_token_cache.used_bytes == 34,
               "next.request.id.and.cache.exact") || !no_actions(&f) || !no_events(&f)) goto out;
    f.publish = false;
    if (!check(&f, answer(&f, true) == MOQ_OK, "alias.next.request.reject") ||
        !initial_response(&f, true) || !no_actions(&f) ||
        !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK, "alias.next.request.retire")) goto out;
    /* Reuse of the cancelled UPDATE ID on a different bidi must still close. */
    use[3] = (uint8_t)(f.id + 2);
    f.ref = moq_stream_ref_from_u64(0x7a00);
    if (!check(&f, ingress(&f, use, sizeof(use), false) == MOQ_OK,
               "cancelled.update.id.reuse.ingress") || !protocol_close(&f, 4)) goto out;
    check(&f, f.s->peer_token_cache.used_bytes == 34 &&
               rs->peer_next_request_id == (uint64_t)f.id + 6,
               "duplicate.id.no.new.cache.or.id.effects");
out:
    return close_fixture(&f);
}

static int reject_invalid_probe(bool publish, bool peer_server, unsigned which)
{
    char name[96];
    snprintf(name, sizeof(name), "reject.invalid.%s.peer-%s.kind%u",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", which);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f)) goto out;
    if (which == 0) update[3] = f.id;         /* duplicate request ID */
    else if (which == 1) update[8] = 1;      /* duplicate REGISTER alias */
    else update[7] = 7;                    /* unrecognized Alias Type */
    if (!check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
               "invalid.complete.update.retained") || !ids_cache(&f, false) ||
        !check(&f, answer(&f, true) == MOQ_OK, "invalid.update.session.close.result") ||
        !protocol_close(&f, which == 0 ? 4 : which == 1 ? 0x14 : 6)) goto out;
    ids_cache(&f, false);
out:
    return close_fixture(&f);
}

static int two_updates_probe(bool publish, bool peer_server, bool coalesced)
{
    char name[96];
    snprintf(name, sizeof(name), "two.pending.%s.peer-%s.coalesced%d",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", coalesced);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32], all[96];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (coalesced) {
        memcpy(all, initial, ni);
        memcpy(all + ni, update, nu);
        memcpy(all + ni + nu, update, nu);
        all[ni + nu + 3] = (uint8_t)(f.id + 4);
        if (!check(&f, ingress(&f, all, ni + 2 * nu, false) == MOQ_OK,
                   "two.coalesced.ingress.close.result")) goto out;
    } else {
        if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
            !initial_event(&f) ||
            !check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
                   "first.update.retained")) goto out;
        update[3] = (uint8_t)(f.id + 4);
        if (!check(&f, ingress(&f, update, nu, false) == MOQ_OK,
                   "second.update.ingress.close.result")) goto out;
    }
    if (!protocol_close(&f, 0x1b)) goto out;
    ids_cache(&f, false);
out:
    return close_fixture(&f);
}

/* The public requester cannot initiate a pending UPDATE; only that peer's legal
 * wire input is literal. Both initial request owners and terminal handling are
 * real, public-API-created endpoints, with the simulator's actual stream refs. */
static bool native_request(fixture_t *f, moq_session_t **out_peer,
                           moq_stream_ref_t *out_ref, uint64_t *out_handle)
{
    moq_session_t *peer = f->s == moq_simpair_client(f->pair)
        ? moq_simpair_server(f->pair) : moq_simpair_client(f->pair);
    moq_bytes_t part = MOQ_BYTES_LITERAL("n");
    moq_auth_token_t auth = { .token_type = 0, .token_value = MOQ_BYTES_LITERAL("a") };
    int peer_slot;
    int local_slot;
    if (f->publish) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = (moq_namespace_t){ &part, 1 };
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.auth_tokens = &auth;
        cfg.auth_token_count = 1;
        moq_publication_t h = MOQ_PUBLICATION_INVALID;
        if (!check(f, moq_session_publish(peer, &cfg, f->now++, &h) == MOQ_OK,
                   "native.publish.request")) return false;
        *out_handle = h._opaque;
        peer_slot = pub_resolve_handle(peer, h);
    } else {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){ &part, 1 };
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.auth_tokens = &auth;
        cfg.auth_token_count = 1;
        moq_subscription_t h = MOQ_SUBSCRIPTION_INVALID;
        if (!check(f, moq_session_subscribe(peer, &cfg, f->now++, &h) == MOQ_OK,
                   "native.subscribe.request")) return false;
        *out_handle = h._opaque;
        peer_slot = sub_resolve_handle(peer, h);
    }
    if (!check(f, peer_slot >= 0, "native.requester.owner.present") ||
        !check(f, moq_simpair_run_until_quiescent(f->pair, 16, NULL) == MOQ_OK,
               "native.initial.pump") || !initial_event(f)) return false;
    if (f->publish) {
        local_slot = pub_resolve_handle(f->s, (moq_publication_t){ f->handle });
        if (!check(f, local_slot >= 0, "native.responder.publish.owner")) return false;
        f->ref = f->s->publishes[local_slot].rs.ref;
        *out_ref = peer->publishes[peer_slot].rs.ref;
    } else {
        local_slot = sub_resolve_handle(f->s, (moq_subscription_t){ f->handle });
        if (!check(f, local_slot >= 0, "native.responder.subscribe.owner")) return false;
        f->ref = f->s->subs[local_slot].rs.ref;
        *out_ref = peer->subs[peer_slot].rs.ref;
    }
    *out_peer = peer;
    return check(f, f->ref._v != 0 && out_ref->_v != 0, "native.actual.stream.refs") &&
        no_actions(f) && no_events(f);
}

static int native_reject_probe(bool publish, bool peer_server, bool peer_pressure)
{
    char name[96];
    snprintf(name, sizeof(name), "native.reject.%s.peer-%s.event-pressure%d",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", peer_pressure);
    fixture_t f = { .name = name, .publish = publish };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    moq_session_t *peer = NULL;
    moq_stream_ref_t peer_ref = moq_stream_ref_from_u64(0);
    uint64_t peer_handle = 0;
    if (!native_request(&f, &peer, &peer_ref, &peer_handle)) goto out;
    uint8_t update[32];
    size_t nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
               "native.early.update.retained") ||
        !pending_retained(&f, update, nu, false) ||
        !check(&f, answer(&f, true) == MOQ_OK, "native.reject.commit") ||
        !initial_response(&f, true) || !stop_action(&f) ||
        !no_actions(&f) || !no_events(&f)) goto out;
    const moq_request_stream_state_t *rs = f.s->profile_state;
    if (!check(&f, rs->peer_next_request_id == (uint64_t)f.id + 4 &&
               f.s->peer_token_cache.used_bytes == 17 &&
               moq_token_cache_lookup(&f.s->peer_token_cache, 2, NULL, NULL, NULL) == MOQ_TOKEN_OK,
               "native.reject.id.register.commit")) goto out;
    moq_session_t *responder = f.s;
    moq_stream_ref_t responder_ref = f.ref;
    f.s = peer;
    f.ref = peer_ref;
    if (peer_pressure && !pressure(&f, 2)) goto out;
    /* Replay the independently asserted literal, not an encoder/decoder oracle. */
    static const uint8_t error[] = { 5, 0, 3, 3, 0, 0 };
    moq_result_t rc = ingress(&f, error, sizeof(error), true);
    if (peer_pressure) {
        if (!check(&f, rc == MOQ_ERR_WOULD_BLOCK, "native.requester.response.refused") ||
            !drain_events(&f, peer)) goto out;
        rc = ingress(&f, NULL, 0, false);
    }
    if (!check(&f, rc == MOQ_OK, "native.requester.error.fin.accepted")) goto out;
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(&f, moq_session_poll_events_ex(peer, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "native.requester.error.event")) goto out;
    bool ok = publish ? e.kind == MOQ_EVENT_PUBLISH_ERROR &&
                        e.u.publish_error.pub._opaque == peer_handle &&
                        e.u.publish_error.error_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED
                      : e.kind == MOQ_EVENT_SUBSCRIBE_ERROR &&
                        e.u.subscribe_error.sub._opaque == peer_handle &&
                        e.u.subscribe_error.error_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED;
    moq_event_cleanup(&e);
    if (!check(&f, ok, "native.requester.error.same.handle.code") ||
        !check(&f, moq_session_on_bidi_stream_stop(peer, peer_ref, 1, f.now++) == MOQ_OK,
               "native.requester.following.stop.accepted") || !no_events(&f)) goto out;
    moq_action_t a = {0};
    n = 0;
    if (!check(&f, moq_session_poll_actions_ex(peer, &a, 1, sizeof(a), &n) == MOQ_OK &&
               n == 1, "native.requester.reciprocal.fin")) goto out;
    ok = a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM &&
         a.u.close_bidi_stream.stream_ref._v == peer_ref._v;
    moq_action_cleanup(&a);
    if (!check(&f, ok, "native.requester.reciprocal.fin.ref") || !no_actions(&f) ||
        !check(&f, !moq_session_has_transport_stream(peer, peer_ref),
               "native.requester.owner.retired")) goto out;
    f.s = responder;
    f.ref = responder_ref;
    if (!check(&f, ingress(&f, NULL, 0, true) == MOQ_OK, "native.reciprocal.fin.delivered") ||
        !check(&f, !moq_session_has_transport_stream(responder, responder_ref),
               "native.responder.owner.retired") || !no_events(&f) || !no_actions(&f)) goto out;
    check(&f, moq_session_state(peer) == MOQ_SESS_ESTABLISHED &&
               moq_session_state(responder) == MOQ_SESS_ESTABLISHED,
               "native.both.sessions.live");
out:
    return close_fixture(&f);
}

static int opposite_publish_probe(bool peer_server)
{
    char name[96];
    snprintf(name, sizeof(name), "PUBLISH.opposite.pending.peer-%s", peer_server ? "server" : "client");
    fixture_t f = { .name = name, .publish = true };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    moq_session_t *peer = NULL;
    moq_stream_ref_t peer_ref = moq_stream_ref_from_u64(0);
    uint64_t peer_handle = 0;
    if (!native_request(&f, &peer, &peer_ref, &peer_handle)) goto out;
    uint8_t update[32];
    size_t nu = update_wire(&f, update);
    update[3] = f.id ^ 1; /* The subscriber's first ID has opposite parity. */
    f.s = peer;
    f.ref = peer_ref;
    if (!check(&f, ingress(&f, update, nu, false) == MOQ_OK,
               "opposite.pending.close.result") || !protocol_close(&f, 3)) goto out;
    check(&f, f.s->peer_token_cache.used_bytes == 0, "opposite.pending.no.register.effects");
out:
    return close_fixture(&f);
}

static int head_done_probe(bool peer_server, bool coalesced)
{
    char name[96];
    snprintf(name, sizeof(name), "PUBLISH.head-DONE.peer-%s.coalesced%d",
             peer_server ? "server" : "client", coalesced);
    fixture_t f = { .name = name, .publish = true };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], all[48];
    size_t ni = initial_wire(&f, initial);
    static const uint8_t done[] = { 0x0b, 0, 3, 0, 0, 0 };
    moq_result_t rc;
    if (coalesced) {
        memcpy(all, initial, ni);
        memcpy(all + ni, done, sizeof(done));
        rc = ingress(&f, all, ni + sizeof(done), true);
    } else {
        if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
            !initial_event(&f)) goto out;
        rc = ingress(&f, done, sizeof(done), true);
    }
    if (!check(&f, rc == MOQ_OK, "head.done.not.deferred") ||
        (coalesced && !initial_event(&f)) || !publish_finished(&f) ||
        !no_actions(&f) || !no_events(&f) || !ids_cache(&f, false)) goto out;
    check(&f, !moq_session_has_transport_stream(f.s, f.ref) &&
               answer(&f, false) == MOQ_ERR_STALE_HANDLE &&
               moq_session_state(f.s) == MOQ_SESS_ESTABLISHED,
               "head.done.retired.without.initial.answer");
out:
    return close_fixture(&f);
}

static int one_action_cap_probe(bool publish, bool peer_server, bool fin)
{
    char name[96];
    snprintf(name, sizeof(name), "one-action-cap.%s.peer-%s.fin%d",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", fin);
    fixture_t f = { .name = name, .publish = publish, .max_actions = 1 };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    static const uint8_t done[] = { 0x0b, 0, 3, 0, 0, 0 };
    if (publish && fin) { memcpy(update + nu, done, sizeof(done)); nu += sizeof(done); }
    if (!check(&f, f.s->action_cap == 1, "one.action.real.constructor.cap") ||
        !check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) ||
        !check(&f, ingress(&f, update, nu, fin) == MOQ_ERR_WOULD_BLOCK,
               "one.action.pending.retained")) goto out;
    if (!check(&f, answer(&f, true) == MOQ_OK, "one.action.reject.commits") ||
        !ids_cache(&f, true) ||
        !check(&f, moq_session_has_transport_stream(f.s, f.ref) == !fin,
               "one.action.terminal.owner.only.open.peer.drain")) goto out;
    size_t calls = f.alloc_calls;
    if (!initial_response(&f, true) || !ids_cache(&f, true) ||
        (!fin && !stop_action(&f)) || !ids_cache(&f, true) ||
        !no_actions(&f) || !no_events(&f) ||
        !check(&f, f.alloc_calls == calls && f.s->action_cap == 1,
               "one.action.poll.only.progress.no.allocation.or.queue.growth") ||
        !check(&f, answer(&f, true) == MOQ_ERR_STALE_HANDLE,
               "one.action.answer.not.repeated") || !ids_cache(&f, true)) goto out;
    if (!fin && (!check(&f, ingress(&f, NULL, 0, false) == MOQ_OK,
                       "one.action.committed.empty.retry") ||
                 !no_actions(&f) || !no_events(&f) ||
                 !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK,
                       "one.action.peer.fin.retires.drain"))) goto out;
    check(&f, !moq_session_has_transport_stream(f.s, f.ref), "one.action.terminal.owner.retired");
    no_actions(&f);
    no_events(&f);
    check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "one.action.session.live");
out:
    return close_fixture(&f);
}

static moq_result_t answer_deferred(fixture_t *f, bool reject)
{
    moq_subscription_t h = { f->handle };
    if (reject) {
        moq_reject_subscription_update_cfg_t cfg;
        moq_reject_subscription_update_cfg_init(&cfg);
        cfg.update_request_id = (uint64_t)f->id + 2;
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
        return moq_session_reject_subscription_update(f->s, h, &cfg, f->now++);
    }
    moq_accept_subscription_update_cfg_t cfg;
    moq_accept_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = (uint64_t)f->id + 2;
    return moq_session_accept_subscription_update(f->s, h, &cfg, f->now++);
}

static int app_deferearly_probe(bool peer_server, bool reject)
{
    char name[96];
    snprintf(name, sizeof(name), "SUBSCRIBE.app-deferred.peer-%s.%s",
             peer_server ? "server" : "client", reject ? "reject-update" : "accept-update");
    fixture_t f = { .name = name };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    if (!check(&f, moq_session_set_defer_subscription_updates(f.s, true) == MOQ_OK,
               "application.defer.update.policy")) goto out;
    uint8_t initial[32], update[32];
    size_t ni = initial_wire(&f, initial), nu = update_wire(&f, update);
    if (!check(&f, ingress(&f, initial, ni, false) == MOQ_OK, "initial.ingress") ||
        !initial_event(&f) ||
        !check(&f, ingress(&f, update, nu, false) == MOQ_ERR_WOULD_BLOCK,
               "early.pending.retained") ||
        !check(&f, answer(&f, false) == MOQ_OK, "initial.accept.commits.independently") ||
        !initial_response(&f, false) || !no_actions(&f) || !ids_cache(&f, false) ||
        !check(&f, ingress(&f, NULL, 0, false) == MOQ_OK, "deferred.update.empty.retry")) goto out;
    moq_event_t e = {0};
    size_t n = 0;
    if (!check(&f, moq_session_poll_events_ex(f.s, &e, 1, sizeof(e), &n) == MOQ_OK &&
               n == 1, "deferred.update.event")) goto out;
    bool ok = e.kind == MOQ_EVENT_SUBSCRIBE_UPDATED &&
              e.u.subscribe_updated.sub._opaque == f.handle &&
              e.u.subscribe_updated.answer_owed && e.u.subscribe_updated.update_count == 1 &&
              e.u.subscribe_updated.update_request_id == (uint64_t)f.id + 2 &&
              e.u.subscribe_updated.has_subscriber_priority &&
              e.u.subscribe_updated.subscriber_priority == 17 &&
              e.u.subscribe_updated.token_count == 1 && token_is(e.u.subscribe_updated.tokens, 'b');
    moq_event_cleanup(&e);
    if (!check(&f, ok, "deferred.update.exact.owed.event") || !ids_cache(&f, true) ||
        !no_actions(&f) || !no_events(&f)) goto out;
    int slot = sub_resolve_handle(f.s, (moq_subscription_t){ f.handle });
    if (!check(&f, slot >= 0 && f.s->subs[slot].subscriber_priority == 128,
               "deferred.settings.not.applied.before.answer") ||
        !pressure(&f, 1) ||
        !check(&f, answer_deferred(&f, reject) == MOQ_ERR_WOULD_BLOCK,
               "deferred.answer.action.refusal") ||
        !ids_cache(&f, true) || !drain_actions(&f) ||
        !check(&f, answer_deferred(&f, reject) == MOQ_OK, "deferred.answer.retry")) goto out;
    static const uint8_t error[] = { 5, 0, 3, 3, 0, 0 };
    static const uint8_t ok_wire[] = { 7, 0, 1, 0 };
    if (!wire_action(&f, reject ? error : ok_wire,
                     reject ? sizeof(error) : sizeof(ok_wire), false) ||
        !check(&f, !f.s->subs[slot].owed_update.owed &&
                   f.s->subs[slot].subscriber_priority == (reject ? 128 : 17),
                   "deferred.answer.settings.and.owed.settled") ||
        !check(&f, answer_deferred(&f, reject) ==
                   (reject ? MOQ_ERR_STALE_HANDLE : MOQ_ERR_WRONG_STATE),
               "deferred.answer.not.repeated") || !ids_cache(&f, true)) goto out;
    if (reject) {
        static const uint8_t failed_done[] = { 0x0b, 0, 3, 8, 0, 0 };
        if (!wire_action(&f, failed_done, sizeof(failed_done), true) ||
            !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK,
                   "deferred.reject.peer.fin.drained") ||
            !check(&f, !moq_session_has_transport_stream(f.s, f.ref),
                   "deferred.reject.owner.retired")) goto out;
    }
    if (!no_actions(&f) || !no_events(&f)) goto out;
    check(&f, moq_session_state(f.s) == MOQ_SESS_ESTABLISHED, "deferred.session.live");
out:
    return close_fixture(&f);
}

static size_t automatic_initial_wire(const fixture_t *f, uint8_t *out)
{
    size_t n = initial_wire(f, out);
    static const uint8_t unknown[] = { 0, 2, 2, 3 };
    out[2] += sizeof(unknown);
    out[f->publish ? 10 : 9] = 2;
    memcpy(out + n, unknown, sizeof(unknown));
    return n + sizeof(unknown);
}

static bool automatic_error(fixture_t *f)
{
    static const uint8_t error[] = { 5, 0, 3, 0x17, 0, 0 };
    return wire_action(f, error, sizeof(error), true);
}

static bool automatic_staging(fixture_t *f, const uint8_t *bytes, size_t len,
                               bool fin, size_t cache, size_t ranges)
{
    const moq_request_stream_state_t *state = f->s->profile_state;
    moq_request_endpoint_t ep = request_registry_find_by_streamref(f->s, f->ref);
    return check(f, ep.kind == MOQ_REQ_SUBSCRIPTION && ep.slot >= 0 &&
        f->s->subs[ep.slot].state == MOQ_SUB_RECVING_REQUEST &&
        f->s->subs[ep.slot].rs.recv_len == len &&
        f->s->subs[ep.slot].rs.recv_fin == fin &&
        memcmp(f->s->subs[ep.slot].rs.recv_buf, bytes, len) == 0 &&
        state->peer_next_request_id == f->id && state->peer_seen_count == ranges &&
        f->s->peer_token_cache.used_bytes == cache,
        "automatic.refusal.retains.raw.staging.fin.ids.cache");
}

/* Actual refused requests create seven disjoint ranges in an eight-range
 * allocation. The composed pair needs nine ranges, not one spare slot. */
static bool automatic_seed_ranges(fixture_t *f)
{
    moq_stream_ref_t ref = f->ref;
    for (unsigned i = 0; i < 7; i++) {
        uint8_t bytes[32];
        size_t n = automatic_initial_wire(f, bytes);
        bytes[3] = (uint8_t)(f->id + 4 + 4 * i);
        bytes[f->publish ? 14 : 13] = (uint8_t)(8 + i);
        f->ref = moq_stream_ref_from_u64(ref._v + 1 + i);
        if (!check(f, ingress(f, bytes, n, true) == MOQ_OK,
                   "automatic.range.seed.actual.ingress") ||
            !automatic_error(f) || !no_actions(f) || !no_events(f)) return false;
    }
    f->ref = ref;
    const moq_request_stream_state_t *state = f->s->profile_state;
    return check(f, state->peer_next_request_id == f->id &&
                   state->peer_seen_count == 7 && state->peer_seen_cap == 8 &&
                   f->s->peer_token_cache.used_bytes == 119,
                 "automatic.range.seed.real.capacity");
}

typedef struct automatic_case {
    const char *name;
    unsigned suffix; /* 0 none, 1 REGISTER, 2 USE_ALIAS, 3 duplicate REGISTER,
                      * 4 DELETE/re-register, 5 overflow, 6 duplicate ID,
                      * 7 parity, 8 disjoint IDs, 9 message-level suffix reject,
                      * 10 USE_ALIAS then REGISTER, 11 initial DELETE/re-register */
    bool fin;
    unsigned pressure; /* 1 full actions, 2 one spare, 3 send bytes, 4 drains,
                        * 5 initial NOMEM, 6 range NOMEM, 7 suffix NOMEM */
    bool fragmented;
} automatic_case_t;

static int automatic_refusal_probe(bool publish, bool peer_server,
                                    const automatic_case_t *c)
{
    char name[128];
    snprintf(name, sizeof(name), "automatic-refusal.%s.peer-%s.%s",
             publish ? "PUBLISH" : "SUBSCRIBE", peer_server ? "server" : "client", c->name);
    fixture_t f = { .name = name, .publish = publish,
        .cache_size = c->suffix == 5 ? 24 :
                      c->suffix == 2 || c->suffix == 4 || c->suffix == 11 ? 17 : 256 };
    if (!open_fixture(&f, MOQ_VERSION_DRAFT_22, peer_server)) goto out;
    if (c->suffix == 8 && !automatic_seed_ranges(&f)) goto out;
    if (c->suffix == 11) {
        uint8_t seed[32];
        size_t len = automatic_initial_wire(&f, seed);
        moq_stream_ref_t ref = f.ref;
        f.ref = moq_stream_ref_from_u64(ref._v + 1);
        if (!check(&f, ingress(&f, seed, len, true) == MOQ_OK,
                   "automatic.prefix.delete.seed.actual.ingress") ||
            !automatic_error(&f) || !no_actions(&f) || !no_events(&f)) goto out;
        f.ref = ref;
        f.id += 2;
    }
    if (!check(&f, ingress(&f, NULL, 0, false) == MOQ_OK,
               "automatic.advance.reclaims.drained.setup.output")) goto out;
    size_t prior_cache = c->suffix == 8 ? 119 : c->suffix == 11 ? 17 : 0;
    size_t prior_ranges = c->suffix == 8 ? 7 : 0;
    uint8_t bytes[96], update[40];
    size_t ni = automatic_initial_wire(&f, bytes), nu = 0;
    if (c->suffix) nu = update_wire(&f, update);
    if (c->suffix == 2 || c->suffix == 11) {
        static const uint8_t use[] = { 2, 0, 8, 0, 2, 3, 2, 2, 1, 29, 17 };
        memcpy(update, use, sizeof(use));
        nu = sizeof(use);
        update[3] = (uint8_t)(f.id + 2);
        if (c->suffix == 11) {
            size_t at = publish ? 11 : 10;
            static const uint8_t delete[] = { 3, 2, 0, 1 };
            memmove(bytes + at + sizeof(delete), bytes + at, ni - at);
            memcpy(bytes + at, delete, sizeof(delete));
            bytes[at + sizeof(delete)] = 0; /* REGISTER type delta after DELETE */
            bytes[at + sizeof(delete) + 5] = 'b';
            bytes[2] += sizeof(delete);
            bytes[publish ? 10 : 9]++;
            ni += sizeof(delete);
        }
    } else if (c->suffix == 3) {
        update[8] = 1;
    } else if (c->suffix == 4) {
        static const uint8_t replace[] = {
            2, 0, 14, 0, 3, 3, 2, 0, 1, 0, 4, 1, 1, 0, 'b', 29, 17
        };
        memcpy(update, replace, sizeof(replace));
        nu = sizeof(replace);
        update[3] = (uint8_t)(f.id + 2);
    } else if (c->suffix == 6) {
        update[3] = f.id;
    } else if (c->suffix == 7) {
        update[3] = (uint8_t)(f.id + 3);
    } else if (c->suffix == 8) {
        bytes[3] = (uint8_t)(f.id + 40);
        update[3] = (uint8_t)(f.id + 44);
    } else if (c->suffix == 9) {
        /* REGISTER still settles after the UPDATE's unknown alias refusal. */
        static const uint8_t unknown[] = { 0, 2, 2, 4 };
        memmove(update + 11 + sizeof(unknown), update + 11, 2);
        memcpy(update + 11, unknown, sizeof(unknown));
        update[2] += sizeof(unknown);
        update[4]++;
        nu += sizeof(unknown);
    } else if (c->suffix == 10) {
        static const uint8_t use_register[] = {
            2, 0, 14, 0, 3, 3, 2, 2, 1, 0, 4, 1, 2, 0, 'b', 29, 17
        };
        memcpy(update, use_register, sizeof(use_register));
        nu = sizeof(use_register);
        update[3] = (uint8_t)(f.id + 2);
    }
    memcpy(bytes + ni, update, nu);
    size_t n = ni + nu;
    if (publish && c->fin && c->suffix) {
        static const uint8_t done[] = { 0x0b, 0, 3, 0, 0, 0 };
        memcpy(bytes + n, done, sizeof(done));
        n += sizeof(done);
    }
    if (c->pressure == 1 || c->pressure == 2) {
        if (!pressure(&f, 1)) goto out;
        if (c->pressure == 2) {
            moq_action_t a = {0};
            size_t count = 0;
            if (!check(&f, moq_session_poll_actions_ex(f.s, &a, 1, sizeof(a), &count) == MOQ_OK &&
                       count == 1 && a.kind == MOQ_ACTION_SEND_CONTROL,
                       "automatic.exactly.one.spare.action")) goto out;
            moq_action_cleanup(&a);
        }
    } else if (c->pressure == 3) {
        uint8_t *payload = f.alloc.alloc(f.s->send_cap, f.alloc.ctx);
        if (!check(&f, payload != NULL, "automatic.send.pressure.allocate")) goto out;
        memset(payload, 0x5a, f.s->send_cap);
        moq_result_t rc = queue_send_bidi(f.s, moq_stream_ref_from_u64(0x7001),
                                         payload, f.s->send_cap, false);
        f.alloc.free(payload, f.s->send_cap, f.alloc.ctx);
        if (!check(&f, rc == MOQ_OK, "automatic.send.pressure.queue")) goto out;
    } else if (c->pressure == 4) {
        for (size_t i = 0; i < f.s->drain_ref_cap; i++)
            if (!check(&f, drain_ref_add(f.s, moq_stream_ref_from_u64(0x100000 + i)),
                       "automatic.drain.pressure")) goto out;
    } else if (c->pressure >= 5) {
        f.fail_at = f.alloc_calls + c->pressure - 4;
    }
    size_t head = f.s->action_head, tail = f.s->action_tail;
    size_t send_len = f.s->send_len, drains = f.s->drain_ref_count;
    const moq_request_stream_state_t *before_state = f.s->profile_state;
    size_t prior_cap = before_state->peer_seen_cap, prior_live = f.live;
    size_t final_calls = f.alloc_calls;
    size_t final_cap = prior_cap;
    size_t first = c->fragmented ? ni + nu / 2 : n;
    moq_result_t rc = ingress(&f, bytes, first, c->fin && first == n);
    uint64_t close_code = c->suffix == 3 ? 0x14 : c->suffix == 5 ? 0x13 :
                          c->suffix == 6 || c->suffix == 7 ? 4 : 0;
    if (c->pressure && c->pressure != 2 && !close_code) {
        moq_result_t expected = c->pressure >= 5 ? MOQ_ERR_NOMEM : MOQ_ERR_WOULD_BLOCK;
        if (!check(&f, rc == expected, "automatic.preparation.or.output.refused") ||
            !check(&f, head == f.s->action_head && tail == f.s->action_tail &&
                       send_len == f.s->send_len && drains == f.s->drain_ref_count,
                       "automatic.refusal.preserves.queued.actions.bytes.drains") ||
            !automatic_staging(&f, bytes, first, c->fin && first == n,
                               prior_cache, prior_ranges) || !no_events(&f)) goto out;
        const moq_request_stream_state_t *prepared = f.s->profile_state;
        if (!check(&f, f.live == prior_live + (!prior_cap && prepared->peer_seen_cap ? 1 : 0),
                   "automatic.aborted.preowned.and.staging.balance")) goto out;
        if (c->pressure == 3) {
            for (size_t i = 0; i < send_len; i++)
                if (!check(&f, f.s->send_buf[i] == 0x5a,
                           "automatic.borrowed.queued.payload.unchanged")) goto out;
        }
        if (c->suffix == 11) {
            const uint8_t *value = NULL;
            size_t len = 0;
            if (!check(&f, moq_token_cache_lookup(&f.s->peer_token_cache, 1, NULL, &value, &len) == MOQ_TOKEN_OK &&
                       len == 1 && value[0] == 'a',
                       "automatic.aborted.prefix.delete.leaves.original.value")) goto out;
        }
        f.fail_at = 0;
        if (c->fragmented) {
            if (!check(&f, ingress(&f, NULL, 0, false) == MOQ_ERR_WOULD_BLOCK,
                       "automatic.fragment.empty.retry") ||
                !automatic_staging(&f, bytes, first, false, prior_cache, prior_ranges) ||
                !check(&f, ingress(&f, bytes + first, n - first, c->fin) == MOQ_ERR_WOULD_BLOCK,
                       "automatic.fragment.completed.while.blocked") ||
                !automatic_staging(&f, bytes, n, c->fin, prior_cache, prior_ranges)) goto out;
        }
        if (c->pressure == 4) {
            for (size_t i = 0; i < f.s->drain_ref_cap; i++)
                if (!check(&f, drain_ref_remove(f.s, moq_stream_ref_from_u64(0x100000 + i)),
                           "automatic.drain.pressure.release")) goto out;
        } else if (c->pressure <= 3 && !drain_actions(&f)) goto out;
        final_calls = f.alloc_calls;
        final_cap = prepared->peer_seen_cap;
        rc = ingress(&f, NULL, 0, false);
    }
    if (close_code) {
        const moq_request_stream_state_t *state = f.s->profile_state;
        check(&f, rc == MOQ_OK && moq_session_state(f.s) == MOQ_SESS_CLOSED,
              "automatic.invalid.composition.session.error.not.local.refusal");
        check(&f, state->peer_next_request_id == f.id &&
                  state->peer_seen_count == prior_ranges &&
                  f.s->peer_token_cache.used_bytes == prior_cache,
              "automatic.session.error.does.not.commit.either.prepared.message");
        protocol_close(&f, close_code);
        goto out;
    }
    if (!check(&f, rc == MOQ_OK && moq_session_state(f.s) == MOQ_SESS_ESTABLISHED,
               "automatic.refusal.must.not.turn.legal.update.into.session.error")) goto out;
    if (c->pressure == 2 &&
        !check(&f, f.s->action_head == head && f.s->action_tail == tail + 1 &&
                   f.s->drain_ref_count == drains + (c->fin ? 0 : 1) &&
                   moq_session_has_transport_stream(f.s, f.ref) == !c->fin,
               "automatic.one.spare.commits.error.and.bounded.drain")) goto out;
    const moq_request_stream_state_t *state = f.s->profile_state;
    size_t cache = c->suffix == 11 ? 17 :
        prior_cache + (c->suffix && c->suffix != 2 && c->suffix != 4 ? 34 : 17);
    uint64_t next = c->suffix == 8 ? f.id : (uint64_t)f.id + (c->suffix ? 4 : 2);
    const uint8_t *value = NULL;
    size_t value_len = 0;
    if (!check(&f, state->peer_next_request_id == next &&
               state->peer_seen_count == (c->suffix == 8 ? 9u : 0u) &&
               f.s->peer_token_cache.used_bytes == cache &&
               moq_token_cache_lookup(&f.s->peer_token_cache, 1, NULL, &value, &value_len) == MOQ_TOKEN_OK &&
               value_len == 1 && value[0] == (c->suffix == 4 || c->suffix == 11 ? 'b' : 'a'),
               "complete.update.id.and.register.must.be.settled.before.abandonment") ||
        !check(&f, f.alloc_calls - final_calls ==
                   (c->suffix == 8 && final_cap < 9 ? 3u : c->suffix == 10 ? 3u : c->suffix ? 2u : 1u),
                   "automatic.commit.has.no.hidden.allocation")) goto out;
    size_t calls = f.alloc_calls;
    if ((c->pressure == 2 && !queued_pressure_actions(&f, tail - head)) ||
        !automatic_error(&f) || (c->suffix && !c->fin && !stop_action(&f)) ||
        !no_actions(&f) || !no_events(&f)) goto out;
    if (!check(&f, state->peer_next_request_id == next &&
               state->peer_seen_count == (c->suffix == 8 ? 9u : 0u) &&
               f.s->peer_token_cache.used_bytes == cache && f.alloc_calls == calls,
               "automatic.poll.only.progress.ids.auth.once.no.allocation")) goto out;
    const uint8_t *alias2_value = NULL;
    size_t alias2_len = 0;
    bool alias2 = c->suffix && c->suffix != 2 && c->suffix != 4 && c->suffix != 11;
    int alias2_rc = moq_token_cache_lookup(&f.s->peer_token_cache, 2, NULL, &alias2_value, &alias2_len);
    if (!check(&f, alias2 ? alias2_rc == MOQ_TOKEN_OK && alias2_len == 1 && alias2_value[0] == 'b'
                          : alias2_rc == MOQ_TOKEN_ERR_UNKNOWN,
               "automatic.settled.alias2.exact.value.or.absence")) goto out;
    if (!c->fin) {
        if (!check(&f, ingress(&f, NULL, 0, false) == MOQ_OK,
                   "automatic.committed.empty.retry.no.duplicate.response") ||
            !no_actions(&f) || !no_events(&f) ||
            !check(&f, ingress(&f, NULL, 0, true) == MOQ_OK,
                   "automatic.peer.fin.retires.drain")) goto out;
    }
    check(&f, !moq_session_has_transport_stream(f.s, f.ref), "automatic.terminal.owner.retired");
    no_actions(&f);
    no_events(&f);
out:
    return close_fixture(&f);
}

int main(int argc, char **argv)
{
    bool controls = argc == 1 || (argc == 2 && strcmp(argv[1], "--controls") == 0);
    bool early_probes = argc == 1 || (argc == 2 && strcmp(argv[1], "--early-probes") == 0);
    bool extra = argc == 1 || (argc == 2 && strcmp(argv[1], "--extra") == 0);
    bool automatic = argc == 1 || (argc == 2 && strcmp(argv[1], "--automatic") == 0);
    if (!controls && !early_probes && !extra && !automatic) return 2;
    static const automatic_case_t auto_cases[] = {
        { "initial.only", 0, false, 0, false },
        { "initial.only.fin", 0, true, 0, false },
        { "initial.only.pressure", 0, false, 1, false },
        { "register", 1, false, 0, false },
        { "register.fin", 1, true, 0, false },
        { "register.full.actions", 1, false, 1, false },
        { "register.one.spare", 1, false, 2, false },
        { "register.send.pressure", 1, false, 3, false },
        { "register.drain.pressure", 1, false, 4, false },
        { "register.initial.nomem", 1, false, 5, false },
        { "register.suffix.nomem", 1, false, 6, false },
        { "register.fragment.empty.retry", 1, false, 1, true },
        { "register.fragment.fin.retry", 1, true, 1, true },
        { "use.prepared.alias", 2, false, 0, false },
        { "use.prepared.alias.nomem", 2, false, 6, false },
        { "duplicate.prepared.alias", 3, false, 0, false },
        { "duplicate.prepared.alias.full.actions", 3, false, 1, false },
        { "delete.prepared.alias.reregister", 4, false, 0, false },
        { "combined.cache.limit", 5, false, 0, false },
        { "combined.cache.limit.full.actions", 5, false, 1, false },
        { "duplicate.prepared.id", 6, false, 0, false },
        { "duplicate.prepared.id.full.actions", 6, false, 1, false },
        { "wrong.parity.id", 7, false, 0, false },
        { "wrong.parity.id.full.actions", 7, false, 1, false },
        { "disjoint.prepared.id.capacity", 8, false, 0, false },
        { "disjoint.prepared.id.nomem", 8, false, 6, false },
        { "disjoint.prepared.id.one.spare", 8, false, 2, false },
        { "suffix.message.error.register", 9, false, 0, false },
        { "use.register.preowned.nomem", 10, false, 7, false },
        { "use.register", 10, false, 0, false },
        { "initial.delete.reregister.use", 11, false, 0, false },
        { "initial.delete.reregister.one.spare", 11, false, 2, false },
        { "initial.delete.reregister.full.actions", 11, false, 1, false },
        { "initial.delete.reregister.suffix.nomem", 11, false, 6, false }
    };
    static const early_case_t cases[] = {
        { "coalesced.accept", 0, false, false, 0 },
        { "whole.reject", 1, false, true, 0 },
        { "split.event.accept", 2, false, false, 2 },
        { "split.fin.action.accept", 2, true, false, 1 },
        { "coalesced.fin.reject", 0, true, true, 0 },
        { "coalesced.event.accept", 0, false, false, 2 }
    };
    int failures = 0;
    size_t count = 0;
    for (unsigned pub = 0; pub < 2; pub++) {
        for (unsigned role = 0; role < 2; role++) {
            if (automatic) {
                for (size_t i = 0; i < sizeof(auto_cases) / sizeof(auto_cases[0]); i++) {
                    failures += automatic_refusal_probe(pub != 0, role != 0, &auto_cases[i]);
                    count++;
                }
            }
            if (early_probes) {
                for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                    failures += early_probe(pub != 0, role != 0, &cases[i]);
                    count++;
                }
            }
            if (controls) {
                for (unsigned p = 0; p < 3; p++) {
                    failures += control(pub != 0, role != 0, false, p);
                    count++;
                }
                failures += control(pub != 0, role != 0, true, 1);
                failures += old18_control(pub != 0, role != 0);
                failures += fin_control(pub != 0, role != 0);
                count += 3;
                if (pub) {
                    failures += partial_answer_control(true, role != 0, false);
                    failures += partial_answer_control(true, role != 0, true);
                    count += 2;
                }
            }
            if (extra) {
                for (unsigned reject = 0; reject < 2; reject++) {
                    failures += bytewise_probe(pub != 0, role != 0, reject != 0);
                    failures += alias_after_reject_probe(pub != 0, role != 0, reject != 0);
                    failures += two_updates_probe(pub != 0, role != 0, reject != 0);
                    failures += native_reject_probe(pub != 0, role != 0, reject != 0);
                    failures += one_action_cap_probe(pub != 0, role != 0, reject != 0);
                    count += 5;
                    if (!pub) {
                        failures += partial_answer_control(false, role != 0, reject != 0);
                        failures += app_deferearly_probe(role != 0, reject != 0);
                        count += 2;
                    } else {
                        failures += head_done_probe(role != 0, reject != 0);
                        count++;
                    }
                }
                for (unsigned refusal = 1; refusal <= 5; refusal++) {
                    failures += reject_refusal_probe(pub != 0, role != 0, refusal);
                    count++;
                }
                for (unsigned invalid = 0; invalid < 3; invalid++) {
                    failures += reject_invalid_probe(pub != 0, role != 0, invalid);
                    count++;
                }
                if (pub) {
                    failures += opposite_publish_probe(role != 0);
                    count++;
                }
            }
        }
    }
    printf("%s cases=%zu failures=%d\n", argc == 1 ? "ALL" : automatic ? "AUTOMATIC" : extra ? "EXTENDED" :
           controls ? "CONTROLS" : "EARLY_CASES", count, failures);
    return failures ? 1 : 0;
}
