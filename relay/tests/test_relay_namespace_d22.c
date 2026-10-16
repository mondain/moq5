/*
 * Draft-22 namespace discovery, namespace updates and rendezvous through the
 * production relay: SimPair peers attached to the shard runtime's bindings
 * (one shard, or two under the deterministic round barrier), driven with
 * supplied time only. Each case states the invariant it pins.
 *
 * Track discovery (§3.6, §9.18): a SUBSCRIBE_TRACKS is admitted, and every
 * track under its prefix that the subscriber is authorized for -- now and
 * later -- reaches it as a PUBLISH carrying the request's FORWARD and
 * GROUP_ORDER, never its tokens, and never for a track the subscriber itself
 * publishes. A track that cannot get a subscription is reported with
 * PUBLISH_SKIPPED once for its source. Updates change later offers only.
 *
 * Namespace updates (§9.5): authorized as REQUEST_UPDATE and the original
 * namespace action before an acceptance; a refusal ends the request and the
 * relay stops serving it.
 *
 * Rendezvous (§9.20.6): a SUBSCRIBE with a positive RENDEZVOUS_TIMEOUT and no
 * publisher is held without an answer until a publisher arrives or one
 * deadline, taken at first receipt and clamped to the relay's ceiling,
 * passes (TIMEOUT).
 */

#include <moq/relay/moqr_shards.h>
#include <moq/relay/moqr_bind.h>
#include <moq/relay/relay.h>

#include <moq/moq.h>
#include <moq/sim.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tests/unit/test_support.h"

static moq_bytes_t
B(const char *s)
{
    return (moq_bytes_t){ .data = (const uint8_t *)s, .len = strlen(s) };
}

static bool
bytes_is(moq_bytes_t b, const char *s)
{
    return b.len == strlen(s) && (b.len == 0 || memcmp(b.data, s, b.len) == 0);
}

/* -- authorization policy ------------------------------------------------- */

typedef struct policy {
    /* SUBSCRIBE of a track named "secret" needs a token whose value is
     * "key". */
    bool     guard_secret;
    /* REQUEST_UPDATE carrying a token whose value is "bad" is denied. */
    bool     deny_bad_update;
    /* DEFER this action (once each time it is armed). */
    bool     defer_armed;
    uint32_t defer_action;
    uint64_t next_ticket;
    uint64_t deferred_ticket;
    int      calls[MOQR_AUTH_ACTION__COUNT];
    /* SUBSCRIBE of a track with this name is deferred, every time. */
    const char *defer_name;
    int      defer_name_calls;
    int      update_calls_with_token;
    /* With defer_name: defer only a request carrying this token, and deny
     * one carrying deny_token. */
    const char *defer_token;
    const char *deny_token;
    /* An ALLOWed SUBSCRIBE of lease_name lasts lease_us before it is asked
     * again; a SUBSCRIBE of deny_name is denied. */
    const char *lease_name;
    uint64_t lease_us;
    const char *deny_name;
    /* SUBSCRIBE_TRACKS (namespace authorization) is denied. */
    bool     deny_tracks;
    int      lease_name_calls;
} policy_t;

static bool
has_token(const moqr_auth_request_t *req, const char *value)
{
    for (size_t i = 0; i < req->token_count; i++) {
        if (bytes_is(req->tokens[i].token_value, value)) return true;
    }
    return false;
}

static void
policy_hook(void *ctx, const moqr_auth_request_t *req, moqr_auth_verdict_t *out)
{
    policy_t *p = ctx;
    if (req->action < MOQR_AUTH_ACTION__COUNT) p->calls[req->action]++;
    out->decision = MOQR_AUTH_ALLOW;
    if (req->action == MOQR_AUTH_REQUEST_UPDATE && req->token_count > 0) {
        p->update_calls_with_token++;
    }
    if (p->deny_tracks && req->action == MOQR_AUTH_SUBSCRIBE_NAMESPACE) {
        out->decision = MOQR_AUTH_DENY;
        out->error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
        return;
    }
    if (req->action == MOQR_AUTH_SUBSCRIBE && p->lease_name != NULL &&
        bytes_is(req->name, p->lease_name)) {
        p->lease_name_calls++;
        if (p->deny_name != NULL && bytes_is(req->name, p->deny_name)) {
            out->decision = MOQR_AUTH_DENY;
            out->error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
            return;
        }
        if (p->defer_name == NULL || !bytes_is(req->name, p->defer_name)) {
            out->revalidate_after_us = p->lease_us;
            return;
        }
    }
    if (p->defer_name != NULL && req->action == MOQR_AUTH_SUBSCRIBE &&
        bytes_is(req->name, p->defer_name) && p->deny_token != NULL &&
        has_token(req, p->deny_token)) {
        p->defer_name_calls++;
        out->decision = MOQR_AUTH_DENY;
        out->error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
        return;
    }
    if (p->defer_name != NULL && req->action == MOQR_AUTH_SUBSCRIBE &&
        bytes_is(req->name, p->defer_name) &&
        (p->defer_token == NULL || has_token(req, p->defer_token))) {
        p->defer_name_calls++;
        out->decision = MOQR_AUTH_DEFER;
        out->ticket = ++p->next_ticket;
        p->deferred_ticket = out->ticket;
        return;
    }
    if (p->defer_armed && req->action == p->defer_action) {
        p->defer_armed = false;
        out->decision = MOQR_AUTH_DEFER;
        out->ticket = ++p->next_ticket;
        p->deferred_ticket = out->ticket;
        return;
    }
    if (p->guard_secret && req->action == MOQR_AUTH_SUBSCRIBE &&
        bytes_is(req->name, "secret") && !has_token(req, "key")) {
        out->decision = MOQR_AUTH_DENY;
        out->error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
        return;
    }
    if (p->deny_bad_update && req->action == MOQR_AUTH_REQUEST_UPDATE &&
        has_token(req, "bad")) {
        out->decision = MOQR_AUTH_DENY;
        out->error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
    }
}

/* -- the rig -------------------------------------------------------------- */

#define R_MAX_CONNS 4
#define R_MAX_SEEN  8

typedef struct obs {
    int      setup;
    int      closed;
    /* track discovery, subscriber side */
    int      tracks_ok, tracks_err;
    uint64_t tracks_err_code;
    int      publish_req;
    char     pub_name[R_MAX_SEEN][16];
    bool     pub_forward[R_MAX_SEEN];
    size_t   pub_tokens[R_MAX_SEEN];
    uint32_t pub_order_flags[R_MAX_SEEN];
    uint32_t pub_order[R_MAX_SEEN];
    uint64_t pub_raw[R_MAX_SEEN];
    bool     pub_has_largest[R_MAX_SEEN];
    uint64_t pub_largest_group[R_MAX_SEEN];
    int      publish_blocked;
    char     blocked_name[R_MAX_SEEN][16];
    uint32_t blocked_suffix_count[R_MAX_SEEN];
    int      objects;
    uint8_t  last_byte;
    uint64_t last_group;
    int      fill_objects;
    uint8_t  fill_last_byte;
    int      fill_terminals;
    uint32_t fill_reason;
    /* namespace updates, requester side */
    int      upd_ok, upd_err;
    uint64_t upd_err_code;
    uint64_t upd_id;
    /* namespace subscription / announcement */
    int      ns_sub_ok, ns_sub_err;
    int      ns_found, ns_gone;
    char     found[R_MAX_SEEN][16];
    int      ann_ok, ann_err, ann_terminated;
    /* subscriptions */
    int      sub_ok, sub_err;
    uint64_t sub_err_code;
    bool     sub_err_can_retry;
    uint64_t sub_err_retry_ms;
    /* publisher side */
    int      publish_ok, publish_err;
    int      pub_done;       /* an accepted PUBLISH ended by the relay   */
    uint64_t pub_done_code;
    int      sub_request;
    int      other_terminal;
} obs_t;

typedef struct rconn {
    bool               used;
    uint16_t           shard;
    moq_simpair_t     *sp;
    moq_session_t     *peer;
    moq_session_t     *rsess;
    obs_t              o;
    bool               accept_publish;   /* accept the relay's PUBLISHes   */
    bool               accept_subscribe; /* accept the relay's SUBSCRIBEs  */
    moq_subscription_t up_sub;           /* last relay SUBSCRIBE received  */
    bool               raw;              /* the test drives the relay's
                                          * session for this peer itself   */
} rconn_t;

typedef struct rig {
    moqr_shards_t *s;
    rconn_t        conns[R_MAX_CONNS];
    uint64_t       now;
    uint16_t       shards;
    policy_t       policy;
    int            failures;
} rig_t;

#define RIG_OR_FAIL(call)                                                   \
    do {                                                                    \
        if (!(call)) {                                                      \
            printf("FAIL: %s:%d: rig create\n", __FILE__, __LINE__);        \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define R_CHECK(r, expr)                                                    \
    do {                                                                    \
        if (!(expr)) {                                                      \
            printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
            (r)->failures++;                                                \
        }                                                                   \
    } while (0)

/* The core's subscription pool for the next rig (0: the default). */
static uint32_t g_rig_max_subs;
/* The core's revalidation grant pool for the next rig (0: the default). */
static uint32_t g_rig_max_grants;
/* The relay's allocator for the next rig (NULL: the default). */
static const moq_alloc_t *g_rig_alloc;

static bool
rig_create(rig_t *r, uint16_t shards, bool with_policy, uint32_t max_dsubs)
{
    memset(r, 0, sizeof(*r));
    r->now = 1000;
    r->shards = shards;
    moqr_shards_cfg_t cfg;
    moqr_shards_cfg_init_sized(&cfg, sizeof(cfg),
                               g_rig_alloc != NULL ? g_rig_alloc
                                                   : moq_alloc_default());
    cfg.shards = shards;
    cfg.admit_remote_demand = true;
    cfg.core_cfg.log_budget.max_groups = 4;
    cfg.core_cfg.log_budget.max_bytes = 1u << 20;
    if (with_policy) {
        cfg.core_cfg.authorize = policy_hook;
        cfg.core_cfg.authorize_ctx = &r->policy;
    }
    if (max_dsubs != 0) {
        cfg.bind_cfg.max_downstream_subs = max_dsubs;
    }
    if (g_rig_max_subs != 0) {
        cfg.core_cfg.max_subs = g_rig_max_subs;
    }
    if (g_rig_max_grants != 0) {
        cfg.core_cfg.max_grants = g_rig_max_grants;
    }
    return moqr_shards_create(&cfg, &r->s) == MOQR_OK;
}

static void
rig_destroy(rig_t *r)
{
    moqr_shards_destroy(r->s);
    for (int i = 0; i < R_MAX_CONNS; i++) {
        if (r->conns[i].used) moq_simpair_destroy(r->conns[i].sp);
    }
    memset(r, 0, sizeof(*r));
}

static rconn_t *
rig_connect(rig_t *r, uint16_t shard, moq_version_t version,
            uint64_t client_grant)
{
    for (int i = 0; i < R_MAX_CONNS; i++) {
        rconn_t *c = &r->conns[i];
        if (c->used) continue;
        memset(c, 0, sizeof(*c));
        moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
        cfg.alloc = moq_alloc_default();
        cfg.seed = 0xD22 + (uint64_t)i;
        cfg.version = version;
        cfg.client_send_request_capacity = true;
        cfg.client_initial_request_capacity = client_grant;
        cfg.server_send_request_capacity = true;
        cfg.server_initial_request_capacity = 1024;
        if (moq_simpair_create(&cfg, &c->sp) != MOQ_OK) return NULL;
        c->peer = moq_simpair_client(c->sp);
        c->rsess = moq_simpair_server(c->sp);
        if (moq_simpair_start(c->sp) != MOQ_OK ||
            moqr_bind_conn_open(moqr_shards_bind(r->s, shard), c->rsess,
                                version) != MOQR_OK) {
            moq_simpair_destroy(c->sp);
            return NULL;
        }
        c->shard = shard;
        c->used = true;
        c->accept_publish = true;
        c->accept_subscribe = true;
        return c;
    }
    return NULL;
}

static void
peer_drain(rig_t *r, rconn_t *c)
{
    moq_event_t ev;
    while (moq_session_poll_events(c->peer, &ev, 1) == 1) {
        obs_t *o = &c->o;
        switch (ev.kind) {
        case MOQ_EVENT_SETUP_COMPLETE: o->setup++; break;
        case MOQ_EVENT_SESSION_CLOSED: o->closed++; break;
        case MOQ_EVENT_SUBSCRIBE_TRACKS_OK: o->tracks_ok++; break;
        case MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR:
            o->tracks_err++;
            o->tracks_err_code = ev.u.subscribe_tracks_error.error_code;
            break;
        case MOQ_EVENT_PUBLISH_REQUEST: {
            const moq_publish_request_event_t *pr = &ev.u.publish_request;
            int k = o->publish_req++;
            if (k < R_MAX_SEEN) {
                size_t n = pr->track_name.len < 15 ? pr->track_name.len : 15;
                memcpy(o->pub_name[k], pr->track_name.data, n);
                o->pub_name[k][n] = '\0';
                o->pub_forward[k] = pr->forward;
                o->pub_tokens[k] = pr->token_count;
                o->pub_raw[k] = pr->pub._opaque;
                o->pub_has_largest[k] = pr->has_largest;
                o->pub_largest_group[k] = pr->largest_group;
                moq_publish_initial_parameters_t ip;
                memset(&ip, 0, sizeof(ip));
                if (moq_session_get_publish_initial_parameters(
                        c->peer, pr->pub, &ip, sizeof(ip)) == MOQ_OK) {
                    o->pub_order_flags[k] = ip.flags;
                    o->pub_order[k] = ip.group_order;
                }
            }
            if (c->accept_publish) {
                moq_accept_publish_cfg_t a;
                moq_accept_publish_cfg_init(&a);
                R_CHECK(r, moq_session_accept_publish(c->peer, pr->pub, &a,
                                                      r->now) == MOQ_OK);
            } else {
                moq_reject_publish_cfg_t rj;
                moq_reject_publish_cfg_init(&rj);
                rj.error_code = MOQ_REQUEST_ERROR_UNINTERESTED;
                R_CHECK(r, moq_session_reject_publish(c->peer, pr->pub, &rj,
                                                      r->now) == MOQ_OK);
            }
            break;
        }
        case MOQ_EVENT_PUBLISH_BLOCKED: {
            const moq_publish_blocked_event_t *pb = &ev.u.publish_blocked;
            int k = o->publish_blocked++;
            if (k < R_MAX_SEEN) {
                size_t n = pb->track_name.len < 15 ? pb->track_name.len : 15;
                memcpy(o->blocked_name[k], pb->track_name.data, n);
                o->blocked_name[k][n] = '\0';
                o->blocked_suffix_count[k] =
                    (uint32_t)pb->track_namespace_suffix.count;
            }
            break;
        }
        case MOQ_EVENT_FILL_OBJECT:
            o->fill_objects++;
            if (ev.u.fill_object.payload != NULL &&
                moq_rcbuf_len(ev.u.fill_object.payload) > 0) {
                o->fill_last_byte = moq_rcbuf_data(ev.u.fill_object.payload)[0];
            }
            break;
        case MOQ_EVENT_FILL_TERMINAL:
            o->fill_terminals++;
            o->fill_reason = ev.u.fill_terminal.reason;
            break;
        case MOQ_EVENT_OBJECT_RECEIVED:
            o->objects++;
            o->last_group = ev.u.object_received.group_id;
            if (ev.u.object_received.payload != NULL &&
                moq_rcbuf_len(ev.u.object_received.payload) > 0) {
                o->last_byte = moq_rcbuf_data(ev.u.object_received.payload)[0];
            }
            break;
        case MOQ_EVENT_NAMESPACE_UPDATE_RESULT:
        case MOQ_EVENT_NS_SUB_UPDATE_RESULT:
        case MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_RESULT: {
            const moq_namespace_update_result_event_t *ur =
                &ev.u.namespace_update_result;
            o->upd_id = ur->update_request_id;
            if (ur->outcome == MOQ_NAMESPACE_UPDATE_OUTCOME_OK) {
                o->upd_ok++;
            } else {
                o->upd_err++;
                o->upd_err_code = ur->error_code;
            }
            break;
        }
        case MOQ_EVENT_NS_SUB_OK: o->ns_sub_ok++; break;
        case MOQ_EVENT_NS_SUB_ERROR: o->ns_sub_err++; break;
        case MOQ_EVENT_NAMESPACE_FOUND: {
            int k = o->ns_found++;
            const moq_namespace_t *sfx =
                &ev.u.namespace_found.track_namespace_suffix;
            if (k < R_MAX_SEEN && sfx->count > 0) {
                size_t n = sfx->parts[0].len < 15 ? sfx->parts[0].len : 15;
                memcpy(o->found[k], sfx->parts[0].data, n);
                o->found[k][n] = '\0';
            }
            break;
        }
        case MOQ_EVENT_NAMESPACE_GONE: o->ns_gone++; break;
        case MOQ_EVENT_NAMESPACE_ACCEPTED: o->ann_ok++; break;
        case MOQ_EVENT_NAMESPACE_REJECTED: o->ann_err++; break;
        case MOQ_EVENT_NAMESPACE_TERMINATED: o->ann_terminated++; break;
        case MOQ_EVENT_SUBSCRIBE_OK: o->sub_ok++; break;
        case MOQ_EVENT_SUBSCRIBE_ERROR:
            o->sub_err++;
            o->sub_err_code = ev.u.subscribe_error.error_code;
            o->sub_err_can_retry = ev.u.subscribe_error.can_retry;
            o->sub_err_retry_ms = ev.u.subscribe_error.retry_after_ms;
            break;
        case MOQ_EVENT_PUBLISH_OK: o->publish_ok++; break;
        case MOQ_EVENT_PUBLISH_FINISHED:
            o->pub_done++;
            o->pub_done_code = ev.u.publish_finished.status_code;
            break;
        case MOQ_EVENT_PUBLISH_ERROR: o->publish_err++; break;
        case MOQ_EVENT_SUBSCRIBE_REQUEST:
            o->sub_request++;
            c->up_sub = ev.u.subscribe_request.sub;
            if (c->accept_subscribe) {
                moq_accept_subscribe_cfg_t a;
                moq_accept_subscribe_cfg_init(&a);
                R_CHECK(r, moq_session_accept_subscribe(
                               c->peer, ev.u.subscribe_request.sub, &a,
                               r->now) == MOQ_OK);
            }
            break;
        default:
            break;
        }
        moq_event_cleanup(&ev);
    }
}

/* One deterministic cycle at `step_us` past the previous one. */
static void
rig_cycle_by(rig_t *r, uint64_t step_us)
{
    r->now += step_us;
    for (int i = 0; i < R_MAX_CONNS; i++) {
        rconn_t *c = &r->conns[i];
        if (!c->used || c->raw) continue;
        R_CHECK(r, moq_simpair_advance_to(c->sp, r->now) == MOQ_OK);
        size_t steps = 0;
        R_CHECK(r, moq_simpair_run_until_quiescent(c->sp, 64, &steps) ==
                       MOQ_OK);
    }
    R_CHECK(r, moqr_shards_step(r->s, r->now) == MOQR_OK);
    for (int i = 0; i < R_MAX_CONNS; i++) {
        rconn_t *c = &r->conns[i];
        if (!c->used || c->raw) continue;
        size_t steps = 0;
        R_CHECK(r, moq_simpair_run_until_quiescent(c->sp, 64, &steps) ==
                       MOQ_OK);
        peer_drain(r, c);
    }
}

static void
rig_pump(rig_t *r, int cycles)
{
    for (int i = 0; i < cycles; i++) rig_cycle_by(r, 1000);
}

/* What the relay sent on a raw-driven session: responses on the test's
 * request stream (REQUEST_OK type 7 / REQUEST_ERROR type 5) and the request
 * streams it opened (its PUBLISHes). */
typedef struct raw_obs {
    int ok, error, opens, other;
} raw_obs_t;

static void
raw_drain(rconn_t *c, raw_obs_t *o)
{
    memset(o, 0, sizeof(*o));
    moq_action_t a;
    while (moq_session_poll_actions(c->rsess, &a, 1) == 1) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM && a.u.send_bidi_stream.len > 0) {
            uint8_t t = a.u.send_bidi_stream.data[0];
            if (t == 7) o->ok++;
            else if (t == 5) o->error++;
            else o->other++;
        } else if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            o->opens++;
        } else {
            o->other++;
        }
        moq_action_cleanup(&a);
    }
}

/* -- peer requests -------------------------------------------------------- */

static moq_track_sub_handle_t
peer_tracks(rig_t *r, rconn_t *c, const char *prefix, bool forward,
            moq_group_order_t order, const char *token)
{
    moq_bytes_t part = B(prefix);
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){ &part, 1 };
    cfg.has_forward = true;
    cfg.forward = forward;
    moq_auth_token_t tok = { .token_type = 0, .token_value = B(token ? token : "") };
    if (token != NULL) {
        cfg.auth_tokens = &tok;
        cfg.auth_token_count = 1;
    }
    if (order != MOQ_GROUP_ORDER_DEFAULT) {
        cfg.options_flags = MOQ_SUBSCRIBE_TRACKS_GROUP_ORDER;
        cfg.group_order = order;
    }
    moq_track_sub_handle_t h = { 0 };
    R_CHECK(r, moq_session_subscribe_tracks(c->peer, &cfg, r->now, &h) ==
                   MOQ_OK);
    return h;
}

static moq_publication_t
peer_publish(rig_t *r, rconn_t *c, const char *ns, const char *name)
{
    moq_bytes_t part = B(ns);
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    cfg.track_name = B(name);
    moq_publication_t pub = { 0 };
    R_CHECK(r, moq_session_publish(c->peer, &cfg, r->now, &pub) == MOQ_OK);
    return pub;
}

static moq_announcement_t
peer_announce_tok(rig_t *r, rconn_t *c, const char *ns, const char *token)
{
    moq_bytes_t part = B(ns);
    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    moq_auth_token_t tok = { .token_type = 0, .token_value = B(token ? token : "") };
    if (token != NULL) {
        cfg.auth_tokens = &tok;
        cfg.auth_token_count = 1;
    }
    moq_announcement_t ann = { 0 };
    R_CHECK(r, moq_session_publish_namespace(c->peer, &cfg, r->now, &ann) ==
                   MOQ_OK);
    return ann;
}

static moq_announcement_t
peer_announce(rig_t *r, rconn_t *c, const char *ns)
{
    return peer_announce_tok(r, c, ns, NULL);
}

static moq_ns_sub_handle_t
peer_ns_sub(rig_t *r, rconn_t *c, const char *prefix)
{
    moq_bytes_t part = B(prefix);
    moq_subscribe_namespace_cfg_t cfg;
    moq_subscribe_namespace_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){ &part, 1 };
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t h = { 0 };
    R_CHECK(r, moq_session_subscribe_namespace(c->peer, &cfg, r->now, &h) ==
                   MOQ_OK);
    return h;
}

static moq_subscription_t
peer_subscribe(rig_t *r, rconn_t *c, const char *ns, const char *name,
               bool rendezvous, uint64_t rendezvous_ms)
{
    moq_bytes_t part = B(ns);
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    cfg.track_name = B(name);
    cfg.has_rendezvous_timeout = rendezvous;
    cfg.rendezvous_timeout_ms = rendezvous_ms;
    moq_subscription_t sub = { 0 };
    R_CHECK(r, moq_session_subscribe(c->peer, &cfg, r->now, &sub) == MOQ_OK);
    return sub;
}

static void
peer_write(rig_t *r, rconn_t *c, moq_publication_t pub, uint64_t group,
           uint8_t byte)
{
    moq_subgroup_cfg_t sgc;
    moq_subgroup_cfg_init(&sgc);
    sgc.group_id = group;
    sgc.publisher_priority = 100;
    moq_subgroup_handle_t sgh;
    R_CHECK(r, moq_session_open_pub_subgroup(c->peer, pub, &sgc, r->now,
                                             &sgh) == MOQ_OK);
    uint8_t body[8];
    memset(body, byte, sizeof(body));
    moq_rcbuf_t *pl = NULL;
    R_CHECK(r, moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &pl) ==
                   MOQ_OK);
    R_CHECK(r, moq_session_write_object(c->peer, sgh, 0, pl, r->now) == MOQ_OK);
    moq_rcbuf_decref(pl);
    R_CHECK(r, moq_session_close_subgroup(c->peer, sgh, r->now) == MOQ_OK);
}

static moq_namespace_update_cfg_t
update_cfg(const char *prefix, moq_bytes_t *part, int forward,
           moq_auth_token_t *tok, const char *token)
{
    moq_namespace_update_cfg_t u;
    moq_namespace_update_cfg_init(&u);
    if (prefix != NULL) {
        *part = B(prefix);
        u.flags |= MOQ_NAMESPACE_UPDATE_PREFIX;
        u.track_namespace_prefix = (moq_namespace_t){ part, 1 };
    }
    if (forward >= 0) {
        u.flags |= MOQ_NAMESPACE_UPDATE_FORWARD;
        u.forward = forward != 0;
    }
    if (token != NULL) {
        *tok = (moq_auth_token_t){ .token_type = 0, .token_value = B(token) };
        u.auth_tokens = tok;
        u.auth_token_count = 1;
    }
    return u;
}

static bool
saw_publish(const obs_t *o, const char *name)
{
    for (int i = 0; i < o->publish_req && i < R_MAX_SEEN; i++) {
        if (strcmp(o->pub_name[i], name) == 0) return true;
    }
    return false;
}

/* -- cases ---------------------------------------------------------------- */

/* A SUBSCRIBE_TRACKS is admitted (OK, no NOT_SUPPORTED) and the matching
 * track another peer publishes reaches the subscriber as one PUBLISH with the
 * requested FORWARD and GROUP_ORDER and none of the request's tokens; the
 * subscription it creates carries the track's objects. The publisher's own
 * SUBSCRIBE_TRACKS never offers it its own track (§3.6). */
static int
test_discovery_publishes_matching_track(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    moq_publication_t pub = peer_publish(&r, p, "a", "t1");
    rig_pump(&r, 3);
    R_CHECK(&r, p->o.publish_ok == 1);
    peer_write(&r, p, pub, 4, 0x11);   /* the track has a Largest Object */
    rig_pump(&r, 3);

    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DESCENDING, "tok");
    (void)peer_tracks(&r, p, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.tracks_ok == 1 && s->o.tracks_err == 0);
    R_CHECK(&r, p->o.tracks_ok == 1 && p->o.tracks_err == 0);
    R_CHECK(&r, s->o.publish_req == 1);
    R_CHECK(&r, strcmp(s->o.pub_name[0], "t1") == 0);
    R_CHECK(&r, s->o.pub_forward[0]);
    R_CHECK(&r, s->o.pub_tokens[0] == 0);   /* discovery tokens never copied */
    R_CHECK(&r, (s->o.pub_order_flags[0] & MOQ_PUBLISH_INITIAL_HAS_GROUP_ORDER) != 0);
    R_CHECK(&r, s->o.pub_order[0] == MOQ_GROUP_ORDER_DESCENDING);
    R_CHECK(&r, p->o.publish_req == 0);     /* own track excluded */
    /* A PUBLISH names the Largest Object once objects exist (§9.20.17). */
    R_CHECK(&r, s->o.pub_has_largest[0] && s->o.pub_largest_group[0] == 4);

    peer_write(&r, p, pub, 5, 0x5a);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.objects == 1 && s->o.last_byte == 0x5a);

    /* Nothing is offered twice for the same source. */
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.publish_req == 1);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_publishes_matching_track");
    return failures;
}

/* Tracks that appear after admission are offered too; a FORWARD update
 * changes only the PUBLISHes after its acceptance, and a prefix update brings
 * in tracks under the new prefix that already exist, without touching the
 * subscriptions already made (§9.5.2). */
static int
test_discovery_updates_affect_later_offers(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    moq_track_sub_handle_t h =
        peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 2);
    R_CHECK(&r, s->o.tracks_ok == 1 && s->o.publish_req == 0);

    (void)peer_publish(&r, p, "a", "t1");
    (void)peer_publish(&r, p, "b", "t9");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1 && strcmp(s->o.pub_name[0], "t1") == 0);
    R_CHECK(&r, s->o.pub_forward[0]);

    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg(NULL, &part, 0, &tok, NULL);
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_subscribe_tracks(s->peer, h, &u, r.now,
                                                    &id) == MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.upd_ok == 1 && s->o.upd_err == 0 && s->o.upd_id == id);

    (void)peer_publish(&r, p, "a", "t2");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 2 && strcmp(s->o.pub_name[1], "t2") == 0);
    R_CHECK(&r, !s->o.pub_forward[1]);    /* the new FORWARD */

    u = update_cfg("b", &part, -1, &tok, NULL);
    R_CHECK(&r, moq_session_update_subscribe_tracks(s->peer, h, &u, r.now,
                                                    &id) == MOQ_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.upd_ok == 2 && s->o.upd_id == id);
    R_CHECK(&r, s->o.publish_req == 3 && strcmp(s->o.pub_name[2], "t9") == 0);
    /* a/t3 is outside the new prefix. */
    (void)peer_publish(&r, p, "a", "t3");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 3 && !saw_publish(&s->o, "t3"));
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_updates_affect_later_offers");
    return failures;
}

/* A REQUEST_UPDATE following PUBLISH_OK on a discovery publication is an
 * ordinary subscription update (§3.6.2, §3.5): its FILL_PARAMETERS are served
 * from what the relay holds, and its Location Filter governs what the
 * subscription delivers afterwards. */
static int
test_discovery_update_location_and_fill(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    moq_publication_t pub = peer_publish(&r, p, "a", "t1");
    rig_pump(&r, 3);
    peer_write(&r, p, pub, 1, 0x31);   /* retained before discovery */
    rig_pump(&r, 3);
    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1);
    moq_publication_t spub = { ._opaque = s->o.pub_raw[0] };

    moq_location_filter_cfg_t loc, floc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = 5;
    moq_location_filter_cfg_init(&floc);
    floc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;   /* {1,0} .. Largest */
    floc.start_group = 1;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &floc;
    moq_publication_update_cfg_t u;
    moq_publication_update_cfg_init(&u);
    u.location = &loc;
    u.fill = &fill;
    R_CHECK(&r, moq_session_update_publication(s->peer, spub, &u, r.now) ==
                    MOQ_OK);
    rig_pump(&r, 8);
    R_CHECK(&r, s->o.fill_objects == 1 && s->o.fill_last_byte == 0x31);
    R_CHECK(&r, s->o.fill_terminals == 1 &&
                    s->o.fill_reason == MOQ_FILL_COMPLETE);

    /* After the update, the subscription delivers from group 5 on. */
    peer_write(&r, p, pub, 3, 0x33);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.objects == 0);
    peer_write(&r, p, pub, 5, 0x35);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.objects == 1 && s->o.last_group == 5 &&
                    s->o.last_byte == 0x35);
    R_CHECK(&r, s->o.closed == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_update_location_and_fill");
    return failures;
}

/* A discovered track whose authorization the verifier defers is held: not
 * offered, not skipped and not asked about again while the decision is
 * pending. ALLOW then offers it exactly once; DENY declines it. Either way
 * the ticket is resolved once and gone; a connection that goes away first
 * retires it. */
static int
test_discovery_deferred_authorization(int verdict)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    r.policy.defer_name = "slow";
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, p, "a", "open");
    (void)peer_publish(&r, p, "a", "slow");
    rig_pump(&r, 3);
    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1 && saw_publish(&s->o, "open"));
    R_CHECK(&r, r.policy.defer_name_calls == 1);
    uint64_t ticket = r.policy.deferred_ticket;
    R_CHECK(&r, ticket != 0);
    rig_pump(&r, 6);   /* held: no offer, no skip, no second question */
    R_CHECK(&r, s->o.publish_req == 1 && s->o.publish_blocked == 0);
    R_CHECK(&r, r.policy.defer_name_calls == 1);

    moqr_bind_t *bind = moqr_shards_bind(r.s, 0);
    moqr_auth_verdict_t v;
    memset(&v, 0, sizeof(v));
    if (verdict == 2) {
        /* The subscriber leaves first: its connection retires the ticket. */
        R_CHECK(&r, moqr_bind_conn_close(bind, s->rsess) == MOQR_OK);
        v.decision = MOQR_AUTH_ALLOW;
        R_CHECK(&r, moqr_bind_auth_resolve(bind, ticket, &v, r.now) != MOQR_OK);
        int failures = r.failures;
        rig_destroy(&r);
        if (failures == 0) MOQ_TEST_PASS("discovery_deferred_authorization conn_gone");
        return failures;
    }
    v.decision = verdict == 0 ? MOQR_AUTH_ALLOW : MOQR_AUTH_DENY;
    v.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
    R_CHECK(&r, moqr_bind_auth_resolve(bind, ticket, &v, r.now) == MOQR_OK);
    rig_pump(&r, 6);
    if (verdict == 0) {
        R_CHECK(&r, s->o.publish_req == 2 && saw_publish(&s->o, "slow"));
    } else {
        R_CHECK(&r, s->o.publish_req == 1 && !saw_publish(&s->o, "slow"));
    }
    R_CHECK(&r, s->o.publish_blocked == 0);
    R_CHECK(&r, r.policy.defer_name_calls == 1);   /* decided once */
    /* The ticket is gone: resolving it again finds nothing. */
    R_CHECK(&r, moqr_bind_auth_resolve(bind, ticket, &v, r.now) != MOQR_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.publish_req == (verdict == 0 ? 2 : 1));
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: discovery_deferred_authorization %s\n",
               verdict == 0 ? "allow" : "deny");
    return failures;
}

/* A track that cannot get a subscription (here: the binding has room for
 * one downstream subscription) is reported with PUBLISH_SKIPPED naming its
 * suffix and name, once, and is not offered again for that source when room
 * frees up (§3.6.3). */
static int
test_discovery_skipped_not_replayed(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 1));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, p, "a", "t1");
    (void)peer_publish(&r, p, "a", "t2");
    rig_pump(&r, 3);
    s->accept_publish = false;   /* the refusal frees the one slot */
    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.tracks_ok == 1);
    R_CHECK(&r, s->o.publish_req == 1);
    R_CHECK(&r, s->o.publish_blocked == 1);
    R_CHECK(&r, strcmp(s->o.blocked_name[0], s->o.pub_name[0]) != 0);
    R_CHECK(&r, strcmp(s->o.blocked_name[0], "t1") == 0 ||
                    strcmp(s->o.blocked_name[0], "t2") == 0);
    R_CHECK(&r, s->o.blocked_suffix_count[0] == 0);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1 && s->o.publish_blocked == 1);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_skipped_not_replayed");
    return failures;
}

/* A track the relay cannot subscribe to -- here the core's whole
 * subscription pool (one) is taken by another connection, while the
 * subscriber's binding has room -- is SKIPPED, never offered with a PUBLISH
 * that then ends for an internal error, and is not offered again for that
 * source (§3.6.3). */
static int
test_discovery_skipped_when_core_full(void)
{
    rig_t r;
    g_rig_max_subs = 1;
    bool ok = rig_create(&r, 1, false, 0);
    g_rig_max_subs = 0;
    RIG_OR_FAIL(ok);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *x = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && x != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, p, "a", "t1");
    rig_pump(&r, 3);
    R_CHECK(&r, p->o.publish_ok == 1);
    (void)peer_subscribe(&r, x, "a", "t1", false, 0);   /* takes the pool */
    rig_pump(&r, 3);
    R_CHECK(&r, x->o.sub_ok == 1);
    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.tracks_ok == 1);
    R_CHECK(&r, s->o.publish_req == 0);
    R_CHECK(&r, s->o.publish_blocked == 1);
    R_CHECK(&r, s->o.publish_blocked < 1 || strcmp(s->o.blocked_name[0], "t1") == 0);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 0 && s->o.publish_blocked == 1);
    R_CHECK(&r, s->o.closed == 0 && x->o.sub_ok == 1);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_skipped_when_core_full");
    return failures;
}

/* Each discovered track is authorized as a SUBSCRIBE with the track
 * subscription's tokens: a track the subscriber may not read is not offered,
 * and an accepted token update offers it (§3.6, §9.5). */
static int
test_discovery_authorized_per_track(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    r.policy.guard_secret = true;
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, p, "a", "open");
    (void)peer_publish(&r, p, "a", "secret");
    rig_pump(&r, 3);
    moq_track_sub_handle_t h =
        peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, "nope");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1 && saw_publish(&s->o, "open"));
    R_CHECK(&r, !saw_publish(&s->o, "secret"));
    R_CHECK(&r, s->o.publish_blocked == 0);   /* not offered, not skipped */

    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg(NULL, &part, -1, &tok, "key");
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_subscribe_tracks(s->peer, h, &u, r.now,
                                                    &id) == MOQ_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.upd_ok == 1);
    R_CHECK(&r, r.policy.update_calls_with_token >= 1);
    R_CHECK(&r, s->o.publish_req == 2 && saw_publish(&s->o, "secret"));
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_authorized_per_track");
    return failures;
}

/* A denied announcement update is refused and ends the announcement: the
 * relay withdraws it, so a namespace subscriber is told it is gone. An
 * allowed one is acknowledged once, naming its Request ID. */
static int
test_announce_update_authorization(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    r.policy.deny_bad_update = true;
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    moq_announcement_t ann = peer_announce(&r, p, "n");
    (void)peer_ns_sub(&r, s, "n");
    rig_pump(&r, 4);
    R_CHECK(&r, p->o.ann_ok == 1 && s->o.ns_sub_ok == 1 && s->o.ns_found == 1);

    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg(NULL, &part, -1, &tok, "good");
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_namespace(p->peer, ann, &u, r.now, &id) ==
                    MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, p->o.upd_ok == 1 && p->o.upd_err == 0 && p->o.upd_id == id);
    R_CHECK(&r, r.policy.calls[MOQR_AUTH_REQUEST_UPDATE] == 1);
    R_CHECK(&r, s->o.ns_gone == 0);

    u = update_cfg(NULL, &part, -1, &tok, "bad");
    R_CHECK(&r, moq_session_update_namespace(p->peer, ann, &u, r.now, &id) ==
                    MOQ_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, p->o.upd_ok == 1 && p->o.upd_err == 1 && p->o.upd_id == id);
    R_CHECK(&r, p->o.upd_err_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
    R_CHECK(&r, s->o.ns_gone == 1);
    R_CHECK(&r, p->o.closed == 0 && s->o.closed == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("announce_update_authorization");
    return failures;
}

/* An update that carries no token is authorized against the tokens the
 * request carries (§9.5: an omitted parameter keeps its value): one whose
 * announcement carried a token the policy refuses for updates is refused,
 * one whose announcement carried none is accepted. */
static int
test_update_without_tokens_keeps_request_tokens(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    r.policy.deny_bad_update = true;
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL);
    rig_pump(&r, 2);
    moq_announcement_t marked = peer_announce_tok(&r, p, "m", "bad");
    moq_announcement_t plain = peer_announce(&r, p, "q");
    rig_pump(&r, 4);
    R_CHECK(&r, p->o.ann_ok == 2);
    moq_namespace_update_cfg_t u;
    moq_namespace_update_cfg_init(&u);
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_namespace(p->peer, plain, &u, r.now, &id) ==
                    MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, p->o.upd_ok == 1 && p->o.upd_err == 0 && p->o.upd_id == id);
    R_CHECK(&r, moq_session_update_namespace(p->peer, marked, &u, r.now, &id) ==
                    MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, p->o.upd_ok == 1 && p->o.upd_err == 1 && p->o.upd_id == id);
    R_CHECK(&r, p->o.upd_err_code == MOQ_REQUEST_ERROR_UNAUTHORIZED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("update_without_tokens_keeps_request_tokens");
    return failures;
}

/* A deferred update authorization holds the answer: nothing is acknowledged
 * until the verifier decides, then exactly one acceptance follows. */
static int
test_update_deferred_authorization(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_announce(&r, p, "b");
    moq_ns_sub_handle_t h = peer_ns_sub(&r, s, "a");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.ns_sub_ok == 1 && s->o.ns_found == 0);

    r.policy.defer_armed = true;
    r.policy.defer_action = MOQR_AUTH_SUBSCRIBE_NAMESPACE;
    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg("b", &part, -1, &tok, NULL);
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_ns_sub(s->peer, h, &u, r.now, &id) ==
                    MOQ_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, r.policy.deferred_ticket != 0);
    R_CHECK(&r, s->o.upd_ok == 0 && s->o.upd_err == 0 && s->o.ns_found == 0);

    moqr_auth_verdict_t v;
    memset(&v, 0, sizeof(v));
    v.decision = MOQR_AUTH_ALLOW;
    R_CHECK(&r, moqr_bind_auth_resolve(moqr_shards_bind(r.s, 0),
                                       r.policy.deferred_ticket, &v, r.now) ==
                    MOQR_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.upd_ok == 1 && s->o.upd_id == id);
    /* The announced namespace under the new prefix follows the acceptance,
     * as a suffix of the new prefix (empty: the namespace is the prefix). */
    R_CHECK(&r, s->o.ns_found == 1);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("update_deferred_authorization");
    return failures;
}

/* A SUBSCRIBE asking to wait for a publisher is held without any answer; a
 * PUBLISH_NAMESPACE that covers it -- on this shard or another -- resumes
 * ordinary admission: the relay subscribes upstream and the SUBSCRIBE_OK
 * follows the upstream acceptance, never before it, and no TIMEOUT follows
 * once the deadline passes. */
static int
test_rendezvous_announce_resumes_admission(uint16_t shards)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, shards, false, 0));
    uint16_t sshard = (uint16_t)(shards - 1);
    rconn_t *s = rig_connect(&r, sshard, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_subscribe(&r, s, "r", "v", true, 5000);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.sub_ok == 0 && s->o.sub_err == 0);
    R_CHECK(&r, moqr_bind_next_deadline_us(moqr_shards_bind(r.s, sshard)) !=
                    UINT64_MAX);

    p->accept_subscribe = false;
    (void)peer_announce(&r, p, "r");
    rig_pump(&r, 6);
    R_CHECK(&r, p->o.ann_ok == 1);
    R_CHECK(&r, p->o.sub_request == 1);
    R_CHECK(&r, s->o.sub_ok == 0 && s->o.sub_err == 0);   /* not early */
    moq_accept_subscribe_cfg_t a;
    moq_accept_subscribe_cfg_init(&a);
    R_CHECK(&r, moq_session_accept_subscribe(p->peer, p->up_sub, &a, r.now) ==
                    MOQ_OK);
    rig_pump(&r, 6);
    R_CHECK(&r, s->o.sub_ok == 1 && s->o.sub_err == 0);
    rig_cycle_by(&r, 6000 * 1000);   /* well past the old deadline */
    rig_pump(&r, 2);
    R_CHECK(&r, s->o.sub_ok == 1 && s->o.sub_err == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: rendezvous_announce_resumes_admission shards=%u\n",
               (unsigned)shards);
    return failures;
}

/* A source that arrives at or after a held SUBSCRIBE's deadline does not
 * resume it: the wait ends with TIMEOUT first (§9.20.6), and only the
 * subscription still within its deadline proceeds. Two subscribers wait
 * 5 ms and 50 ms; the PUBLISH reaches the relay in the same pass that finds
 * the clock past 5 ms (`exact`: at exactly 5 ms). */
static int
test_rendezvous_late_source_times_out(bool exact)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s1 = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s2 = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s1 != NULL && s2 != NULL && p != NULL);
    rig_pump(&r, 2);
    (void)peer_subscribe(&r, s1, "r", "v", true, 5);
    (void)peer_subscribe(&r, s2, "r", "v", true, 50);
    rig_cycle_by(&r, 1);   /* both first received at this cycle's time */
    uint64_t received = r.now;
    rig_pump(&r, 0);
    R_CHECK(&r, s1->o.sub_ok == 0 && s1->o.sub_err == 0);
    (void)peer_publish(&r, p, "r", "v");
    rig_cycle_by(&r, received + 5000u + (exact ? 0u : 1000u) - r.now);
    rig_pump(&r, 3);
    R_CHECK(&r, s1->o.sub_err == 1 && s1->o.sub_ok == 0);
    R_CHECK(&r, s1->o.sub_err_code == MOQ_REQUEST_ERROR_TIMEOUT);
    R_CHECK(&r, s2->o.sub_ok == 1 && s2->o.sub_err == 0);
    R_CHECK(&r, p->o.publish_ok == 1);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: rendezvous_late_source_times_out %s\n", exact ? "exact" : "past");
    return failures;
}

/* With no publisher, a held SUBSCRIBE ends with TIMEOUT exactly at its one
 * deadline: first receipt plus the requested wait, clamped to the relay's
 * 10,000 ms ceiling (here the request asks for UINT64_MAX ms). Nothing is
 * answered a microsecond earlier. */
static int
test_rendezvous_timeout_clamped(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s != NULL);
    rig_pump(&r, 2);
    (void)peer_subscribe(&r, s, "r", "v", true, UINT64_MAX);
    rig_pump(&r, 1);   /* the relay first receives it at this cycle's time */
    uint64_t received = r.now;
    moqr_bind_t *bind = moqr_shards_bind(r.s, 0);
    R_CHECK(&r, moqr_bind_next_deadline_us(bind) == received + 10000u * 1000u);
    rig_pump(&r, 2);
    R_CHECK(&r, s->o.sub_ok == 0 && s->o.sub_err == 0);
    rig_cycle_by(&r, received + 10000u * 1000u - 1u - r.now);
    R_CHECK(&r, s->o.sub_ok == 0 && s->o.sub_err == 0);
    rig_cycle_by(&r, 1);
    rig_pump(&r, 1);
    R_CHECK(&r, s->o.sub_err == 1 && s->o.sub_ok == 0);
    R_CHECK(&r, s->o.sub_err_code == MOQ_REQUEST_ERROR_TIMEOUT);
    R_CHECK(&r, moqr_bind_next_deadline_us(bind) == UINT64_MAX);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("rendezvous_timeout_clamped");
    return failures;
}

/* A PUBLISH for the awaited track resolves the held SUBSCRIBE (SUBSCRIBE_OK,
 * then the track's objects) and is not also offered to the same subscriber
 * through its SUBSCRIBE_TRACKS (§7.6). */
static int
test_rendezvous_publish_resolves_without_discovery(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_tracks(&r, s, "r", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    (void)peer_subscribe(&r, s, "r", "v", true, 5000);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.tracks_ok == 1 && s->o.sub_ok == 0 && s->o.sub_err == 0);
    moq_publication_t pub = peer_publish(&r, p, "r", "v");
    rig_pump(&r, 4);
    R_CHECK(&r, p->o.publish_ok == 1);
    R_CHECK(&r, s->o.sub_ok == 1 && s->o.sub_err == 0);
    R_CHECK(&r, s->o.publish_req == 0);
    peer_write(&r, p, pub, 1, 0x77);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.objects == 1 && s->o.last_byte == 0x77);
    R_CHECK(&r, s->o.publish_req == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("rendezvous_publish_resolves_without_discovery");
    return failures;
}

/* A wait the relay has no room to hold is refused for load (§12.3
 * EXCESSIVE_LOAD, retryable), not as an internal error or DOES_NOT_EXIST,
 * and the wait already held is untouched. */
static int
test_rendezvous_capacity_is_load(void)
{
    rig_t r;
    g_rig_max_subs = 1;
    bool ok = rig_create(&r, 1, false, 0);
    g_rig_max_subs = 0;
    RIG_OR_FAIL(ok);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s2 = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s != NULL && s2 != NULL);
    rig_pump(&r, 2);
    (void)peer_subscribe(&r, s, "r", "v", true, 5000);
    rig_pump(&r, 2);
    (void)peer_subscribe(&r, s2, "r", "w", true, 5000);
    rig_pump(&r, 2);
    R_CHECK(&r, s2->o.sub_err == 1 && s2->o.sub_ok == 0);
    R_CHECK(&r, s2->o.sub_err_code == MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
    R_CHECK(&r, s2->o.sub_err_can_retry);
    R_CHECK(&r, s->o.sub_err == 0 && s->o.sub_ok == 0);   /* still held */
    R_CHECK(&r, moqr_bind_next_deadline_us(moqr_shards_bind(r.s, 0)) !=
                    UINT64_MAX);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("rendezvous_capacity_is_load");
    return failures;
}

/* Cancelling a held SUBSCRIBE ends the wait: no TIMEOUT later, no deadline
 * left. */
static int
test_rendezvous_cancel_ends_wait(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s != NULL);
    rig_pump(&r, 2);
    moq_subscription_t sub = peer_subscribe(&r, s, "r", "v", true, 50);
    rig_pump(&r, 2);
    moqr_bind_t *bind = moqr_shards_bind(r.s, 0);
    R_CHECK(&r, moqr_bind_next_deadline_us(bind) != UINT64_MAX);
    R_CHECK(&r, moq_session_unsubscribe(s->peer, sub, r.now) == MOQ_OK);
    rig_pump(&r, 2);
    R_CHECK(&r, moqr_bind_next_deadline_us(bind) == UINT64_MAX);
    rig_cycle_by(&r, 100 * 1000);
    rig_pump(&r, 2);
    R_CHECK(&r, s->o.sub_err == 0 && s->o.sub_ok == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("rendezvous_cancel_ends_wait");
    return failures;
}

/* The wait is authorized first, and a deferred authorization does not renew
 * the deadline: it still runs from first receipt, so a decision that comes
 * after it is answered with TIMEOUT at once. */
static int
test_rendezvous_deferred_keeps_deadline(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s != NULL);
    rig_pump(&r, 2);
    r.policy.defer_armed = true;
    r.policy.defer_action = MOQR_AUTH_SUBSCRIBE;
    (void)peer_subscribe(&r, s, "r", "v", true, 5);
    rig_pump(&r, 2);
    R_CHECK(&r, r.policy.deferred_ticket != 0);
    R_CHECK(&r, r.policy.calls[MOQR_AUTH_SUBSCRIBE] == 1);
    rig_cycle_by(&r, 20 * 1000);   /* past first receipt + 5 ms */
    R_CHECK(&r, s->o.sub_err == 0 && s->o.sub_ok == 0);
    moqr_auth_verdict_t v;
    memset(&v, 0, sizeof(v));
    v.decision = MOQR_AUTH_ALLOW;
    R_CHECK(&r, moqr_bind_auth_resolve(moqr_shards_bind(r.s, 0),
                                       r.policy.deferred_ticket, &v, r.now) ==
                    MOQR_OK);
    rig_pump(&r, 2);
    R_CHECK(&r, s->o.sub_err == 1 && s->o.sub_ok == 0);
    R_CHECK(&r, s->o.sub_err_code == MOQ_REQUEST_ERROR_TIMEOUT);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("rendezvous_deferred_keeps_deadline");
    return failures;
}

/* Cancelling a SUBSCRIBE_TRACKS (the subscriber resets its request stream)
 * ends discovery: a track published afterwards is not offered, while the
 * subscription discovery already made stays (§3.6). Driven at the wire on
 * the relay's session for the subscriber. */
static int
test_tracks_cancel_stops_discovery(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    s->raw = true;   /* from here on the test owns this session's wire */
    static const uint8_t tracks_sub[] = {0x80, 0x51, 0, 5, 0, 1, 1, 'a', 0};
    const moq_stream_ref_t ref = {UINT64_C(0x7900)};
    R_CHECK(&r, moq_session_on_bidi_stream_bytes(s->rsess, ref, tracks_sub,
                                                 sizeof(tracks_sub), false,
                                                 r.now) == MOQ_OK);
    rig_pump(&r, 2);
    raw_obs_t ro;
    raw_drain(s, &ro);
    R_CHECK(&r, ro.ok == 1 && ro.error == 0 && ro.opens == 0);
    (void)peer_publish(&r, p, "a", "t1");
    rig_pump(&r, 3);
    raw_drain(s, &ro);
    R_CHECK(&r, ro.opens == 1);   /* the PUBLISH for a/t1 */
    R_CHECK(&r, moq_session_on_bidi_stream_reset(s->rsess, ref, 0x1, r.now) ==
                    MOQ_OK);
    rig_pump(&r, 2);
    raw_drain(s, &ro);
    (void)peer_publish(&r, p, "a", "t2");
    rig_pump(&r, 4);
    raw_drain(s, &ro);
    R_CHECK(&r, ro.opens == 0);   /* no PUBLISH after the cancellation */
    R_CHECK(&r, moq_session_state(s->rsess) == MOQ_SESS_ESTABLISHED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("tracks_cancel_stops_discovery");
    return failures;
}

/* An acceptance the relay's session cannot take yet (its action queue is
 * full) is held, not dropped and not repeated: nothing is answered while the
 * queue is full, and exactly one REQUEST_OK follows once it drains. Driven at
 * the wire on the relay's session for the announcer. */
static int
test_update_answer_held_under_pressure(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL);
    rig_pump(&r, 2);
    p->raw = true;
    raw_obs_t ro;
    raw_drain(p, &ro);
    const moq_stream_ref_t ann_ref = {UINT64_C(0x7900)};
    static const uint8_t announce[] = {6, 0, 5, 0, 1, 1, 'n', 0};
    R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, ann_ref, announce,
                                                 sizeof(announce), false,
                                                 r.now) == MOQ_OK);
    rig_pump(&r, 1);
    /* Fill the relay session's 64-entry action queue: the announcement's OK
     * plus one refusal for each of 63 SUBSCRIBEs of unknown tracks. */
    for (uint8_t i = 1; i <= 63; i++) {
        const uint8_t sub[] = {3, 0, 7, (uint8_t)(2u * i), 1, 1, 'x', 1, 'v', 0};
        const moq_stream_ref_t ref = {UINT64_C(0x8000) + i};
        R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, ref, sub,
                                                     sizeof(sub), false,
                                                     r.now) == MOQ_OK);
        rig_pump(&r, 1);
    }
    R_CHECK(&r, moq_session_action_capacity(p->rsess) == 0);
    /* Request ID 128 (two-byte vi64), no parameters: a token-free refresh. */
    static const uint8_t update[] = {2, 0, 3, 0x80, 0x80, 0};
    R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, ann_ref, update,
                                                 sizeof(update), false,
                                                 r.now) == MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, moq_session_action_capacity(p->rsess) == 0);
    raw_drain(p, &ro);
    R_CHECK(&r, ro.ok == 1);          /* the announcement's OK only */
    R_CHECK(&r, ro.error == 63);
    rig_pump(&r, 2);
    raw_drain(p, &ro);
    R_CHECK(&r, ro.ok == 1 && ro.error == 0 && ro.opens == 0);
    rig_pump(&r, 2);
    raw_drain(p, &ro);
    R_CHECK(&r, ro.ok == 0 && ro.error == 0);   /* never repeated */
    R_CHECK(&r, moq_session_state(p->rsess) == MOQ_SESS_ESTABLISHED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("update_answer_held_under_pressure");
    return failures;
}

/* A discovered track authorized for a limited time is offered under a
 * revalidation grant, as a SUBSCRIBE is: when the lease runs out the hook is
 * asked again, and a DENY ends the subscription the offer created; no later
 * object reaches the subscriber. Holds for a synchronous ALLOW and for a
 * deferred one resolved with a lease. When the grant cannot be kept (the
 * pool is taken), the track is not offered at all (fail closed), and it is
 * not reported as SKIPPED either. */
static int
test_discovery_lease_revalidated(bool deferred, bool grants_full)
{
    rig_t r;
    g_rig_max_grants = grants_full ? 1 : 0;
    bool ok = rig_create(&r, 1, true, 0);
    g_rig_max_grants = 0;
    RIG_OR_FAIL(ok);
    r.policy.lease_name = "t1";
    r.policy.lease_us = 20000;
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *x = grants_full ? rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024)
                             : NULL;
    R_CHECK(&r, p != NULL && s != NULL && (!grants_full || x != NULL));
    rig_pump(&r, 2);
    moq_publication_t pub = peer_publish(&r, p, "a", "t1");
    rig_pump(&r, 3);
    if (grants_full) {
        /* Another subscriber's leased SUBSCRIBE holds the one grant. */
        (void)peer_subscribe(&r, x, "a", "t1", false, 0);
        rig_pump(&r, 3);
        R_CHECK(&r, x->o.sub_ok == 1);
    }
    if (deferred) r.policy.defer_name = "t1";
    (void)peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.tracks_ok == 1);
    if (deferred) {
        R_CHECK(&r, s->o.publish_req == 0);
        uint64_t ticket = r.policy.deferred_ticket;
        R_CHECK(&r, ticket != 0);
        r.policy.defer_name = NULL;
        moqr_auth_verdict_t v;
        memset(&v, 0, sizeof(v));
        v.decision = MOQR_AUTH_ALLOW;
        v.revalidate_after_us = r.policy.lease_us;
        R_CHECK(&r, moqr_bind_auth_resolve(moqr_shards_bind(r.s, 0), ticket,
                                           &v, r.now) == MOQR_OK);
        rig_pump(&r, 4);
    }
    if (grants_full) {
        R_CHECK(&r, s->o.publish_req == 0 && s->o.publish_blocked == 0);
        rig_pump(&r, 4);
        R_CHECK(&r, s->o.publish_req == 0 && s->o.publish_blocked == 0);
        R_CHECK(&r, s->o.closed == 0 && x->o.sub_err == 0);
        int failures = r.failures;
        rig_destroy(&r);
        if (failures == 0)
            printf("PASS: discovery_lease_revalidated %s grants_full\n",
                   deferred ? "deferred" : "sync");
        return failures;
    }
    R_CHECK(&r, s->o.publish_req == 1 && saw_publish(&s->o, "t1"));
    peer_write(&r, p, pub, 0, 0x11);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.objects == 1 && s->o.last_byte == 0x11);
    R_CHECK(&r, s->o.pub_done == 0);
    int calls = r.policy.lease_name_calls;
    r.policy.deny_name = "t1";
    rig_cycle_by(&r, 25000);   /* past the lease */
    rig_pump(&r, 4);
    R_CHECK(&r, r.policy.lease_name_calls > calls);   /* asked again */
    R_CHECK(&r, s->o.pub_done == 1);
    int objects = s->o.objects;
    peer_write(&r, p, pub, 1, 0x22);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.objects == objects);
    R_CHECK(&r, s->o.publish_req == 1);   /* not offered again */
    R_CHECK(&r, s->o.closed == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: discovery_lease_revalidated %s\n",
               deferred ? "deferred" : "sync");
    return failures;
}

/* A deferred per-track decision counts only for the authorization inputs it
 * was asked under: once a token update has replaced them, an ALLOW that comes
 * back for the old ones offers nothing; the track is decided again with the
 * new tokens (here: denied), and established offers are untouched (§3.6,
 * §9.5). */
static int
test_discovery_deferred_stale_inputs(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    r.policy.defer_name = "slow";
    r.policy.defer_token = "ta";
    r.policy.deny_token = "tb";
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, p != NULL && s != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, p, "a", "open");
    (void)peer_publish(&r, p, "a", "slow");
    rig_pump(&r, 3);
    moq_track_sub_handle_t h =
        peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, "ta");
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.publish_req == 1 && saw_publish(&s->o, "open"));
    uint64_t ticket = r.policy.deferred_ticket;
    R_CHECK(&r, ticket != 0 && r.policy.defer_name_calls == 1);

    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg(NULL, &part, -1, &tok, "tb");
    uint64_t id = 0;
    R_CHECK(&r, moq_session_update_subscribe_tracks(s->peer, h, &u, r.now,
                                                    &id) == MOQ_OK);
    rig_pump(&r, 4);
    R_CHECK(&r, s->o.upd_ok == 1);
    R_CHECK(&r, r.policy.defer_name_calls == 2);   /* decided again: DENY */
    R_CHECK(&r, s->o.publish_req == 1);

    moqr_auth_verdict_t v;
    memset(&v, 0, sizeof(v));
    v.decision = MOQR_AUTH_ALLOW;
    (void)moqr_bind_auth_resolve(moqr_shards_bind(r.s, 0), ticket, &v, r.now);
    rig_pump(&r, 6);
    R_CHECK(&r, s->o.publish_req == 1 && !saw_publish(&s->o, "slow"));
    R_CHECK(&r, s->o.publish_blocked == 0 && s->o.pub_done == 0);
    R_CHECK(&r, s->o.closed == 0);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("discovery_deferred_stale_inputs");
    return failures;
}

/* -- an allocator that refuses one allocation of a given size -------------- */

typedef struct fail_alloc {
    moq_alloc_t vt;
    size_t      size;      /* the allocation size to refuse         */
    int         nth;       /* refuse the nth one of that size, 1..  */
    int         seen;
    bool        armed;
    int         refused;
} fail_alloc_t;

static void *
fa_alloc(size_t n, void *ctx)
{
    fail_alloc_t *f = ctx;
    if (f->armed && n == f->size && ++f->seen == f->nth) {
        f->refused++;
        return NULL;
    }
    return malloc(n);
}

static void *
fa_realloc(void *p, size_t o, size_t n, void *ctx)
{
    (void)o;
    (void)ctx;
    return realloc(p, n);
}

static void
fa_free(void *p, size_t n, void *ctx)
{
    (void)n;
    (void)ctx;
    free(p);
}

/* 77 bytes: its stored prefix (a count, a length and the bytes) is 85. */
#define LONG_PART \
    "ppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp"
#define LONG_PREFIX_BYTES 85u

/* A prefix update is accepted only once everything applying it needs is
 * held: an allocation the new prefix cannot get refuses that update alone
 * (EXCESSIVE_LOAD) before any acknowledgement, never acknowledges it and then
 * closes the session; and an update whose allocation succeeded applies
 * without allocating again after its acknowledgement. Holds for namespace
 * and for track subscriptions; `nth` picks which allocation of the prefix's
 * size is refused. */
static int
test_prefix_update_allocates_before_ack(bool tracks, int nth)
{
    fail_alloc_t fa;
    memset(&fa, 0, sizeof(fa));
    fa.vt.ctx = &fa;
    fa.vt.alloc = fa_alloc;
    fa.vt.realloc = fa_realloc;
    fa.vt.free = fa_free;
    fa.size = LONG_PREFIX_BYTES;
    fa.nth = nth;
    rig_t r;
    g_rig_alloc = &fa.vt;
    bool ok = rig_create(&r, 1, false, 0);
    g_rig_alloc = NULL;
    RIG_OR_FAIL(ok);
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, s != NULL);
    rig_pump(&r, 2);
    R_CHECK(&r, strlen(LONG_PART) == 77);
    moq_bytes_t part;
    moq_auth_token_t tok;
    moq_namespace_update_cfg_t u = update_cfg(LONG_PART, &part, -1, &tok, NULL);
    uint64_t id = 0;
    if (tracks) {
        moq_track_sub_handle_t h =
            peer_tracks(&r, s, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
        rig_pump(&r, 3);
        R_CHECK(&r, s->o.tracks_ok == 1);
        fa.armed = true;
        R_CHECK(&r, moq_session_update_subscribe_tracks(s->peer, h, &u, r.now,
                                                        &id) == MOQ_OK);
    } else {
        moq_ns_sub_handle_t h = peer_ns_sub(&r, s, "a");
        rig_pump(&r, 3);
        R_CHECK(&r, s->o.ns_sub_ok == 1);
        fa.armed = true;
        R_CHECK(&r, moq_session_update_ns_sub(s->peer, h, &u, r.now, &id) ==
                        MOQ_OK);
    }
    rig_pump(&r, 4);
    fa.armed = false;
    if (nth == 1) {
        R_CHECK(&r, fa.refused == 1);
        R_CHECK(&r, s->o.upd_ok == 0 && s->o.upd_err == 1);
        R_CHECK(&r, s->o.upd_err_code == MOQ_REQUEST_ERROR_EXCESSIVE_LOAD);
    } else {
        R_CHECK(&r, fa.refused == 0);   /* nothing allocated after the OK */
        R_CHECK(&r, s->o.upd_ok == 1 && s->o.upd_err == 0);
    }
    R_CHECK(&r, s->o.closed == 0);
    R_CHECK(&r, moq_session_state(s->rsess) == MOQ_SESS_ESTABLISHED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: prefix_update_allocates_before_ack %s nth=%d\n",
               tracks ? "tracks" : "namespace", nth);
    return failures;
}

/* What the relay sent on a raw-driven session, in order: the answers on one
 * request stream, and whether a PUBLISH (a stream it opened) came before
 * the first of them. */
typedef struct raw_seq {
    int  ok, error, opens;
    bool open_before_answer;
} raw_seq_t;

static void
raw_seq_drain(rconn_t *c, moq_stream_ref_t req, raw_seq_t *o)
{
    moq_action_t a;
    while (moq_session_poll_actions(c->rsess, &a, 1) == 1) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.len > 0 &&
            a.u.send_bidi_stream.stream_ref._v == req._v) {
            uint8_t t = a.u.send_bidi_stream.data[0];
            if (t == 7) o->ok++;
            else if (t == 5) o->error++;
        } else if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            if (o->ok + o->error == 0) o->open_before_answer = true;
            o->opens++;
        }
        moq_action_cleanup(&a);
    }
}

/* The bytes of a SUBSCRIBE_TRACKS for prefix "a" as a draft-22 peer encodes
 * them, renumbered to Request ID 128 (the raw sequence's next one): type,
 * 16-bit length, then the Request ID, which the peer's first request
 * encodes in one byte. */
static size_t
tracks_request_bytes(rig_t *r, rconn_t *c, uint8_t *out, size_t cap)
{
    (void)peer_tracks(r, c, "a", true, MOQ_GROUP_ORDER_DEFAULT, NULL);
    uint8_t enc[256];
    size_t n = 0;
    moq_action_t a;
    while (moq_session_poll_actions(c->peer, &a, 1) == 1) {
        if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM && n == 0 &&
            a.u.open_bidi_stream.len <= sizeof(enc)) {
            memcpy(enc, a.u.open_bidi_stream.data, a.u.open_bidi_stream.len);
            n = a.u.open_bidi_stream.len;
        }
        moq_action_cleanup(&a);
    }
    if (n < 4 || n + 1 > cap || enc[3] != 0) return 0;
    size_t body = ((size_t)enc[1] << 8 | enc[2]) + 1;
    out[0] = enc[0];
    out[1] = (uint8_t)(body >> 8);
    out[2] = (uint8_t)body;
    out[3] = 0x80;
    out[4] = 0x80;
    memcpy(out + 5, enc + 4, n - 4);
    return n + 1;
}

/* A track subscription's own answer is never lost to output pressure: with
 * the relay session's action queue full, the acceptance (or the refusal) is
 * held and sent exactly once when room frees, whether authorization was
 * synchronous or deferred; nothing is offered for the subscription before
 * its acceptance, and an accepted one then offers the matching track.
 * mode: 0 allow, 1 deny, 2 deferred allow. */
static int
test_tracks_answer_held_under_pressure(int mode)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, true, 0));
    rconn_t *q = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    rconn_t *p = rig_connect(&r, 0, MOQ_VERSION_DRAFT_22, 1024);
    R_CHECK(&r, q != NULL && p != NULL);
    rig_pump(&r, 2);
    (void)peer_publish(&r, q, "a", "t1");
    rig_pump(&r, 3);
    p->raw = true;
    raw_obs_t ro;
    raw_drain(p, &ro);
    uint8_t req[256];
    size_t req_len = tracks_request_bytes(&r, p, req, sizeof(req));
    R_CHECK(&r, req_len > 0);
    const moq_stream_ref_t ann_ref = {UINT64_C(0x7900)};
    static const uint8_t announce[] = {6, 0, 5, 0, 1, 1, 'n', 0};
    R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, ann_ref, announce,
                                                 sizeof(announce), false,
                                                 r.now) == MOQ_OK);
    rig_pump(&r, 1);
    /* Fill the relay session's 64-entry action queue: the announcement's OK
     * plus one refusal for each of 63 SUBSCRIBEs of unknown tracks. */
    for (uint8_t i = 1; i <= 63; i++) {
        const uint8_t sub[] = {3, 0, 7, (uint8_t)(2u * i), 1, 1, 'x', 1, 'v', 0};
        const moq_stream_ref_t ref = {UINT64_C(0x8000) + i};
        R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, ref, sub,
                                                     sizeof(sub), false,
                                                     r.now) == MOQ_OK);
        rig_pump(&r, 1);
    }
    R_CHECK(&r, moq_session_action_capacity(p->rsess) == 0);
    if (mode == 1) r.policy.deny_tracks = true;
    if (mode == 2) {
        r.policy.defer_armed = true;
        r.policy.defer_action = MOQR_AUTH_SUBSCRIBE_NAMESPACE;
    }
    const moq_stream_ref_t tracks_ref = {UINT64_C(0x7a00)};
    R_CHECK(&r, moq_session_on_bidi_stream_bytes(p->rsess, tracks_ref, req,
                                                 req_len, false,
                                                 r.now) == MOQ_OK);
    rig_pump(&r, 3);
    if (mode == 2) {
        uint64_t ticket = r.policy.deferred_ticket;
        R_CHECK(&r, ticket != 0);
        moqr_auth_verdict_t v;
        memset(&v, 0, sizeof(v));
        v.decision = MOQR_AUTH_ALLOW;
        R_CHECK(&r, moqr_bind_auth_resolve(moqr_shards_bind(r.s, 0), ticket,
                                           &v, r.now) == MOQR_OK);
        rig_pump(&r, 3);
    }
    R_CHECK(&r, moq_session_action_capacity(p->rsess) == 0);
    raw_seq_t seq;
    memset(&seq, 0, sizeof(seq));
    raw_seq_drain(p, tracks_ref, &seq);   /* the queue as it was when full */
    R_CHECK(&r, seq.ok == 0 && seq.error == 0 && seq.opens == 0);
    for (int k = 0; k < 4; k++) {
        rig_pump(&r, 1);
        raw_seq_drain(p, tracks_ref, &seq);
    }
    if (mode == 1) {
        R_CHECK(&r, seq.ok == 0 && seq.error == 1 && seq.opens == 0);
    } else {
        R_CHECK(&r, seq.ok == 1 && seq.error == 0);
        R_CHECK(&r, !seq.open_before_answer);
        R_CHECK(&r, seq.opens == 1);   /* t1, offered after the OK */
    }
    rig_pump(&r, 3);
    raw_seq_drain(p, tracks_ref, &seq);
    R_CHECK(&r, seq.ok + seq.error == 1);   /* never repeated */
    R_CHECK(&r, moq_session_state(p->rsess) == MOQ_SESS_ESTABLISHED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0)
        printf("PASS: tracks_answer_held_under_pressure %s\n",
               mode == 0 ? "allow" : mode == 1 ? "deny" : "deferred");
    return failures;
}

/* Draft-18 SUBSCRIBE_TRACKS stays refused with NOT_SUPPORTED. */
static int
test_d18_tracks_not_supported(void)
{
    rig_t r;
    RIG_OR_FAIL(rig_create(&r, 1, false, 0));
    rconn_t *s = rig_connect(&r, 0, MOQ_VERSION_DRAFT_18, 1024);
    R_CHECK(&r, s != NULL);
    rig_pump(&r, 2);
    moq_bytes_t part = B("a");
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){ &part, 1 };
    moq_track_sub_handle_t h;
    R_CHECK(&r, moq_session_subscribe_tracks(s->peer, &cfg, r.now, &h) ==
                    MOQ_OK);
    rig_pump(&r, 3);
    R_CHECK(&r, s->o.tracks_ok == 0 && s->o.tracks_err == 1);
    R_CHECK(&r, s->o.tracks_err_code == MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    int failures = r.failures;
    rig_destroy(&r);
    if (failures == 0) MOQ_TEST_PASS("d18_tracks_not_supported");
    return failures;
}

int
main(void)
{
    int failures = 0;
    failures += test_discovery_publishes_matching_track();
    failures += test_discovery_updates_affect_later_offers();
    failures += test_discovery_update_location_and_fill();
    failures += test_discovery_skipped_not_replayed();
    failures += test_discovery_skipped_when_core_full();
    failures += test_discovery_deferred_authorization(0);
    failures += test_discovery_deferred_authorization(1);
    failures += test_discovery_deferred_authorization(2);
    failures += test_discovery_authorized_per_track();
    failures += test_announce_update_authorization();
    failures += test_update_without_tokens_keeps_request_tokens();
    failures += test_update_deferred_authorization();
    failures += test_rendezvous_announce_resumes_admission(1);
    failures += test_rendezvous_announce_resumes_admission(2);
    failures += test_rendezvous_timeout_clamped();
    failures += test_rendezvous_late_source_times_out(false);
    failures += test_rendezvous_late_source_times_out(true);
    failures += test_rendezvous_publish_resolves_without_discovery();
    failures += test_rendezvous_cancel_ends_wait();
    failures += test_rendezvous_capacity_is_load();
    failures += test_rendezvous_deferred_keeps_deadline();
    failures += test_tracks_cancel_stops_discovery();
    failures += test_update_answer_held_under_pressure();
    failures += test_d18_tracks_not_supported();
    failures += test_discovery_lease_revalidated(false, false);
    failures += test_discovery_lease_revalidated(true, false);
    failures += test_discovery_lease_revalidated(false, true);
    failures += test_discovery_lease_revalidated(true, true);
    failures += test_discovery_deferred_stale_inputs();
    failures += test_prefix_update_allocates_before_ack(false, 1);
    failures += test_prefix_update_allocates_before_ack(false, 2);
    failures += test_prefix_update_allocates_before_ack(true, 1);
    failures += test_prefix_update_allocates_before_ack(true, 2);
    failures += test_tracks_answer_held_under_pressure(0);
    failures += test_tracks_answer_held_under_pressure(1);
    failures += test_tracks_answer_held_under_pressure(2);
    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d failure(s)\n", failures);
    return 1;
}
