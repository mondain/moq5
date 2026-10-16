/* Draft-22 REQUEST_UPDATE of the three namespace requests (§9.5, §9.5.2):
 * typed send, decision, result and owed queries; legal parameter contexts;
 * PREFIX_OVERLAP spaces; the prefix barrier; pressure, FIN, RESET and
 * pipelining. Expected wire bytes are literal; no product encoder defines an
 * oracle. Two established sessions exchange bidi traffic through a manual
 * pipe so every byte that crosses can be inspected. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

/* -- Pipe ------------------------------------------------------------- */

#define REF_CLIENT_TO_SERVER 0x100000u
#define REF_SERVER_TO_CLIENT 0x200000u

typedef struct pipe {
    moq_simpair_t *pair;
    moq_session_t *c, *s;
    uint64_t now;
    /* Last bidi bytes each side sent, with the FIN they carried. */
    uint8_t c_sent[512];
    size_t c_sent_len;
    bool c_sent_fin;
    uint8_t s_sent[512];
    size_t s_sent_len;
    bool s_sent_fin;
    unsigned c_closes, s_closes, c_resets, s_resets;
    uint64_t c_close_code, s_close_code;
    uint64_t c_last_open;   /* the client's ref of the stream it opened last */
} pipe_t;

/* The advertised token cache of the next pipe (0: none). */
static uint64_t g_auth_token_cache;

static int pipe_open(pipe_t *p, moq_version_t version, uint32_t server_events,
                     uint32_t server_actions)
{
    int failures = 0;
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.seed = 0x5101;
    cfg.max_events = server_events;
    cfg.max_actions = server_actions;
    cfg.auth_token_cache_size = g_auth_token_cache;
    moq_result_t rc = moq_simpair_create(&cfg, &p->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return failures;
    rc = moq_simpair_start(p->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    rc = moq_simpair_run_until_quiescent(p->pair, 64, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    p->c = moq_simpair_client(p->pair);
    p->s = moq_simpair_server(p->pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p->c), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p->s), MOQ_SESS_ESTABLISHED);
    moq_event_t ev;
    while (moq_session_poll_events(p->c, &ev, 1)) moq_event_cleanup(&ev);
    while (moq_session_poll_events(p->s, &ev, 1)) moq_event_cleanup(&ev);
    p->now = 1000;
    return failures;
}

static void pipe_close(pipe_t *p)
{
    moq_simpair_destroy(p->pair);
    memset(p, 0, sizeof(*p));
}

/* Move every queued bidi action of `from` to `to`. A refused delivery is
 * retried with an empty feed until it is taken or the step budget runs out. */
static int pipe_move(pipe_t *p, bool from_client)
{
    int failures = 0;
    moq_session_t *from = from_client ? p->c : p->s;
    moq_session_t *to = from_client ? p->s : p->c;
    uint64_t base = from_client ? REF_CLIENT_TO_SERVER : REF_SERVER_TO_CLIENT;
    uint64_t peer_base = from_client ? REF_SERVER_TO_CLIENT : REF_CLIENT_TO_SERVER;
    moq_action_t a;
    while (moq_session_poll_actions(from, &a, 1)) {
        moq_stream_ref_t ref = {0};
        const uint8_t *data = NULL;
        size_t len = 0;
        bool fin = false, deliver = true;
        switch (a.kind) {
        case MOQ_ACTION_OPEN_BIDI_STREAM:
            ref = a.u.open_bidi_stream.stream_ref;
            if (from_client) p->c_last_open = ref._v;
            data = a.u.open_bidi_stream.data;
            len = a.u.open_bidi_stream.len;
            fin = a.u.open_bidi_stream.fin;
            break;
        case MOQ_ACTION_SEND_BIDI_STREAM:
            ref = a.u.send_bidi_stream.stream_ref;
            data = a.u.send_bidi_stream.data;
            len = a.u.send_bidi_stream.len;
            fin = a.u.send_bidi_stream.fin;
            break;
        case MOQ_ACTION_CLOSE_BIDI_STREAM:
            ref = a.u.close_bidi_stream.stream_ref;
            fin = true;
            break;
        case MOQ_ACTION_RESET_BIDI_STREAM:
        case MOQ_ACTION_ABORT_BIDI_STREAM:
            if (from_client) p->c_resets++; else p->s_resets++;
            deliver = false;
            break;
        case MOQ_ACTION_CLOSE_SESSION:
            if (from_client) {
                p->c_closes++;
                p->c_close_code = a.u.close_session.code;
            } else {
                p->s_closes++;
                p->s_close_code = a.u.close_session.code;
            }
            deliver = false;
            break;
        default:
            deliver = false;
            break;
        }
        if (deliver) {
            uint8_t *log = from_client ? p->c_sent : p->s_sent;
            size_t *log_len = from_client ? &p->c_sent_len : &p->s_sent_len;
            bool *log_fin = from_client ? &p->c_sent_fin : &p->s_sent_fin;
            if (len <= sizeof(p->c_sent)) {
                if (len) memcpy(log, data, len);
                *log_len = len;
            }
            *log_fin = fin;
            /* A stream the peer opened returns on the peer's own ref. */
            uint64_t v = ref._v >= peer_base && ref._v < peer_base + 0x100000u
                ? ref._v - peer_base : base + ref._v;
            moq_stream_ref_t to_ref = moq_stream_ref_from_u64(v);
            moq_result_t rc = moq_session_on_bidi_stream_bytes(to, to_ref, data,
                                                               len, fin, p->now);
            for (unsigned i = 0; rc == MOQ_ERR_WOULD_BLOCK && i < 4; ++i)
                rc = moq_session_on_bidi_stream_bytes(to, to_ref, NULL, 0, false,
                                                      p->now);
            MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        }
        moq_action_cleanup(&a);
    }
    return failures;
}

/* Record what the server sent without delivering it: answers to bytes the
 * test injected in the client's name must not reach the real client. */
static void pipe_take_server(pipe_t *p)
{
    moq_action_t a;
    while (moq_session_poll_actions(p->s, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.len <= sizeof(p->s_sent)) {
            memcpy(p->s_sent, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            p->s_sent_len = a.u.send_bidi_stream.len;
            p->s_sent_fin = a.u.send_bidi_stream.fin;
        } else if (a.kind == MOQ_ACTION_CLOSE_SESSION) {
            p->s_closes++;
            p->s_close_code = a.u.close_session.code;
        }
        moq_action_cleanup(&a);
    }
}

/* Raw bytes from the client onto the server's view of the client's stream. */
static moq_result_t to_server(pipe_t *p, uint64_t client_ref,
                              const uint8_t *data, size_t len, bool fin)
{
    return moq_session_on_bidi_stream_bytes(p->s,
        moq_stream_ref_from_u64(REF_CLIENT_TO_SERVER + client_ref), data, len,
        fin, p->now);
}

/* A missing event reads as kind 0 with no detail, never as stale bytes. */
static bool next_event(moq_session_t *s, moq_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    return moq_session_poll_events(s, ev, 1) == 1;
}

static size_t drain_events(moq_session_t *s)
{
    moq_event_t ev;
    size_t n = 0;
    while (next_event(s, &ev)) {
        moq_event_cleanup(&ev);
        n++;
    }
    return n;
}

static bool bytes_eq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static const uint8_t request_ok[] = {7, 0, 1, 0};
static const moq_bytes_t part_a = { (const uint8_t *)"a", 1 };
static const moq_bytes_t part_b = { (const uint8_t *)"b", 1 };
static const moq_bytes_t part_c = { (const uint8_t *)"c", 1 };
static const moq_bytes_t part_d = { (const uint8_t *)"d", 1 };

/* -- Request setup ----------------------------------------------------- */

typedef struct handles {
    moq_announcement_t ann_c, ann_s;
    moq_ns_sub_handle_t nss_c, nss_s;
    moq_track_sub_handle_t trk_c, trk_s;
    uint64_t c_ref;      /* the client's stream ref for the request */
} handles_t;

static int open_announcement(pipe_t *p, handles_t *h)
{
    int failures = 0;
    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    moq_bytes_t parts[] = {part_a};
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    MOQ_TEST_CHECK_EQ_INT(moq_session_publish_namespace(p->c, &cfg, p->now, &h->ann_c),
                          MOQ_OK);
    failures += pipe_move(p, true);
    h->c_ref = p->c_last_open;
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p->s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_PUBLISHED);
    h->ann_s = ev.u.namespace_published.ann;
    moq_event_cleanup(&ev);
    moq_accept_namespace_cfg_t acfg;
    moq_accept_namespace_cfg_init(&acfg);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_namespace(p->s, h->ann_s, &acfg, p->now),
                          MOQ_OK);
    failures += pipe_move(p, false);
    MOQ_TEST_CHECK(next_event(p->c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_ACCEPTED);
    moq_event_cleanup(&ev);
    return failures;
}

static int open_ns_sub(pipe_t *p, handles_t *h, const moq_bytes_t *parts, size_t count)
{
    int failures = 0;
    moq_subscribe_namespace_cfg_t cfg;
    moq_subscribe_namespace_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){(moq_bytes_t *)parts, count};
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_namespace(p->c, &cfg, p->now, &h->nss_c),
                          MOQ_OK);
    failures += pipe_move(p, true);
    h->c_ref = p->c_last_open;
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p->s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_REQUEST);
    h->nss_s = ev.u.ns_sub_request.handle;
    moq_event_cleanup(&ev);
    moq_accept_ns_sub_cfg_t acfg;
    moq_accept_ns_sub_cfg_init(&acfg);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_ns_sub(p->s, h->nss_s, &acfg, p->now), MOQ_OK);
    failures += pipe_move(p, false);
    MOQ_TEST_CHECK(next_event(p->c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_OK);
    moq_event_cleanup(&ev);
    return failures;
}

static int open_tracks(pipe_t *p, handles_t *h, const moq_bytes_t *parts, size_t count)
{
    int failures = 0;
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){(moq_bytes_t *)parts, count};
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(p->c, &cfg, p->now, &h->trk_c),
                          MOQ_OK);
    failures += pipe_move(p, true);
    h->c_ref = p->c_last_open;
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p->s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST);
    h->trk_s = ev.u.subscribe_tracks_request.handle;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_tracks_cfg_t acfg;
    moq_accept_subscribe_tracks_cfg_init(&acfg);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks(p->s, h->trk_s, &acfg, p->now),
                          MOQ_OK);
    failures += pipe_move(p, false);
    MOQ_TEST_CHECK(next_event(p->c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_OK);
    moq_event_cleanup(&ev);
    return failures;
}

static moq_accept_subscription_update_cfg_t accept_cfg(uint64_t id)
{
    moq_accept_subscription_update_cfg_t cfg;
    moq_accept_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = id;
    return cfg;
}

static moq_reject_subscription_update_cfg_t reject_cfg(uint64_t id, uint64_t code,
                                                       const char *reason)
{
    moq_reject_subscription_update_cfg_t cfg;
    moq_reject_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = id;
    cfg.error_code = code;
    cfg.reason = (moq_bytes_t){(const uint8_t *)reason, reason ? strlen(reason) : 0};
    return cfg;
}

/* -- Authorization updates and update IDs ------------------------- */

/* Token Type is vi64 on draft-22 (§8.9): an update may carry the full
 * uint64 range. The literal update names it in its nine-byte form and the
 * receiver's owed record holds it exactly. */
static int full_width_token_update(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_announcement(&p, &h);
    if (failures) goto done;
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    const uint8_t token[] = {0xaa};
    moq_auth_token_t tok = { .token_type = UINT64_MAX, .token_value = {token, 1} };
    ucfg.auth_tokens = &tok;
    ucfg.auth_token_count = 1;
    uint64_t id = UINT64_MAX;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, &id),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(id, 2);
    failures += pipe_move(&p, true);
    const uint8_t update_wire[] = {2, 0, 15, 2, 1, 3, 11, 3,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xaa};
    MOQ_TEST_CHECK(bytes_eq(p.c_sent, p.c_sent_len, update_wire, sizeof(update_wire)));
    moq_namespace_update_record_t rec;
    MOQ_TEST_CHECK_EQ_INT(moq_session_namespace_owed_update(p.s, h.ann_s, &rec, sizeof(rec)),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(rec.token_count, 1);
    if (rec.token_count == 1)
        MOQ_TEST_CHECK_EQ_U64(rec.tokens[0].token_type, UINT64_MAX);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "full_width_token_update\n");
    return failures;
}

/* An announcement refresh carries only a token. The owed record is
 * observable before and after the event, the answer names the update's own
 * Request ID, and exactly one REQUEST_OK reaches the requester. */
static int announcement_token_refresh(bool reject)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_announcement(&p, &h);
    if (failures) goto done;

    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    const uint8_t token[] = {0xaa};
    moq_auth_token_t tok = { .token_type = 0, .token_value = {token, 1} };
    ucfg.auth_tokens = &tok;
    ucfg.auth_token_count = 1;
    uint64_t id = UINT64_MAX;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, &id),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(id, 2);
    /* A second update waits for the first answer. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, NULL),
                          MOQ_ERR_REQUEST_BLOCKED);
    failures += pipe_move(&p, true);
    const uint8_t update_wire[] = {2, 0, 7, 2, 1, 3, 3, 3, 0, 0xaa};
    MOQ_TEST_CHECK(bytes_eq(p.c_sent, p.c_sent_len, update_wire, sizeof(update_wire)));
    MOQ_TEST_CHECK(!p.c_sent_fin);

    /* Owed before the event is polled. */
    moq_namespace_update_record_t rec;
    memset(&rec, 0xa5, sizeof(rec));
    MOQ_TEST_CHECK_EQ_INT(moq_session_namespace_owed_update(p.s, h.ann_s, &rec,
        MOQ_NAMESPACE_UPDATE_RECORD_V0_SIZE - 1), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_session_namespace_owed_update(p.s, h.ann_s, &rec, sizeof(rec)),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(rec.update_request_id, 2);
    MOQ_TEST_CHECK_EQ_INT(rec.flags, MOQ_NAMESPACE_UPDATE_AUTHORIZATION);
    MOQ_TEST_CHECK_EQ_SIZE(rec.token_count, 1);
    if (rec.token_count == 1) {
        MOQ_TEST_CHECK_EQ_SIZE(rec.tokens[0].token_value.len, 1);
        MOQ_TEST_CHECK(rec.tokens[0].token_value.data[0] == 0xaa);
    }
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_UPDATE_REQUEST);
    if (ev.kind == MOQ_EVENT_NAMESPACE_UPDATE_REQUEST) {
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_update_request.ann._opaque, h.ann_s._opaque);
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_update_request.update_request_id, 2);
        const moq_namespace_update_record_t *u = ev.u.namespace_update_request.update;
        MOQ_TEST_CHECK(u != NULL);
        if (u) {
            MOQ_TEST_CHECK(u->struct_size >= MOQ_NAMESPACE_UPDATE_RECORD_V0_SIZE);
            MOQ_TEST_CHECK_EQ_INT(u->flags, MOQ_NAMESPACE_UPDATE_AUTHORIZATION);
            MOQ_TEST_CHECK_EQ_SIZE(u->token_count, 1);
        }
    }
    moq_event_cleanup(&ev);
    /* Still owed after the event; nothing was answered by observing it. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_namespace_owed_update(p.s, h.ann_s, &rec, sizeof(rec)),
                          MOQ_OK);
    moq_action_t a;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(p.s, &a, 1), 0);

    moq_accept_subscription_update_cfg_t wrong = accept_cfg(4);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_namespace_update(p.s, h.ann_s, &wrong, p.now),
                          MOQ_ERR_STALE_HANDLE);
    if (!reject) {
        moq_accept_subscription_update_cfg_t ok = accept_cfg(2);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_namespace_update(p.s, h.ann_s, &ok, p.now),
                              MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_namespace_update(p.s, h.ann_s, &ok, p.now),
                              MOQ_ERR_WRONG_STATE);
        failures += pipe_move(&p, false);
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, request_ok, sizeof(request_ok)));
        MOQ_TEST_CHECK(!p.s_sent_fin);
    } else {
        moq_reject_subscription_update_cfg_t no = reject_cfg(2, 1, "no");
        MOQ_TEST_CHECK_EQ_INT(moq_session_reject_namespace_update(p.s, h.ann_s, &no, p.now),
                              MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_namespace_owed_update(p.s, h.ann_s, &rec,
                              sizeof(rec)), MOQ_ERR_STALE_HANDLE);
        failures += pipe_move(&p, false);
        const uint8_t error_wire[] = {5, 0, 5, 1, 0, 2, 'n', 'o'};
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, error_wire, sizeof(error_wire)));
        MOQ_TEST_CHECK(p.s_sent_fin);
    }
    MOQ_TEST_CHECK(next_event(p.c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_UPDATE_RESULT);
    if (ev.kind == MOQ_EVENT_NAMESPACE_UPDATE_RESULT) {
        const moq_namespace_update_result_event_t *r = &ev.u.namespace_update_result;
        MOQ_TEST_CHECK_EQ_U64(r->ann._opaque, h.ann_c._opaque);
        MOQ_TEST_CHECK_EQ_U64(r->update_request_id, 2);
        MOQ_TEST_CHECK_EQ_INT(r->local_role, MOQ_REQUEST_ROLE_REQUESTER);
        MOQ_TEST_CHECK_EQ_INT(r->outcome, reject ? MOQ_NAMESPACE_UPDATE_OUTCOME_ERROR
                                                 : MOQ_NAMESPACE_UPDATE_OUTCOME_OK);
        MOQ_TEST_CHECK_EQ_U64(r->error_code, reject ? 1 : 0);
        MOQ_TEST_CHECK_EQ_SIZE(r->reason.len, reject ? 2 : 0);
    }
    moq_event_cleanup(&ev);
    if (reject) {
        /* The responder's FIN rode its refusal: the request completes. */
        MOQ_TEST_CHECK(next_event(p.c, &ev));
        MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_TERMINATED);
        moq_event_cleanup(&ev);
    } else {
        /* A second refresh is possible once the first is answered. */
        MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, &id),
                              MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(id, 4);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.c), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "announcement_token_refresh reject=%d\n", reject);
    return failures;
}

/* An update's tokens go through the session's token cache (§8.9) like any
 * request's: a REGISTER is observed as its resolved value and registers the
 * alias, a later USE_ALIAS resolves to the same value, a DELETE retires it,
 * and an update naming the retired alias is refused UNKNOWN_AUTH_TOKEN_ALIAS
 * by the session itself, ending the announcement. */
static int token_cache_transactions(void)
{
    int failures = 0;
    pipe_t p;
    g_auth_token_cache = 64;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    g_auth_token_cache = 0;
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_announcement(&p, &h);
    if (failures) goto done;
    /* REGISTER alias 5 = (type 0, 0xbb); USE_ALIAS 5; DELETE 5; USE_ALIAS 5. */
    const uint8_t reg[] = {2, 0, 8, 2, 1, 3, 4, 1, 5, 0, 0xbb};
    const uint8_t use[] = {2, 0, 6, 4, 1, 3, 2, 2, 5};
    const uint8_t del[] = {2, 0, 6, 6, 1, 3, 2, 0, 5};
    const uint8_t stale[] = {2, 0, 6, 8, 1, 3, 2, 2, 5};
    const uint8_t *wires[] = {reg, use, del};
    const size_t lens[] = {sizeof(reg), sizeof(use), sizeof(del)};
    /* A DELETE retires an alias and resolves to no token, so the update that
     * carries only it selects no authorization: what is in effect stands. */
    const size_t want_tokens[] = {1, 1, 0};
    const uint32_t want_flags[] = {MOQ_NAMESPACE_UPDATE_AUTHORIZATION,
                                   MOQ_NAMESPACE_UPDATE_AUTHORIZATION, 0};
    for (int i = 0; i < 3; ++i) {
        uint64_t id = 2u + 2u * (uint64_t)i;
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, wires[i], lens[i], false), MOQ_OK);
        moq_event_t ev;
        MOQ_TEST_CHECK(next_event(p.s, &ev));
        MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_UPDATE_REQUEST);
        if (ev.kind == MOQ_EVENT_NAMESPACE_UPDATE_REQUEST) {
            const moq_namespace_update_record_t *u = ev.u.namespace_update_request.update;
            MOQ_TEST_CHECK_EQ_U64(u->update_request_id, id);
            MOQ_TEST_CHECK_EQ_INT(u->flags, want_flags[i]);
            MOQ_TEST_CHECK_EQ_SIZE(u->token_count, want_tokens[i]);
            if (u->token_count == 1) {
                MOQ_TEST_CHECK_EQ_U64(u->tokens[0].token_type, 0);
                MOQ_TEST_CHECK_EQ_SIZE(u->tokens[0].token_value.len, 1);
                MOQ_TEST_CHECK(u->tokens[0].token_value.data[0] == 0xbb);
            }
        }
        moq_event_cleanup(&ev);
        moq_accept_subscription_update_cfg_t ok = accept_cfg(id);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_namespace_update(p.s, h.ann_s, &ok, p.now),
                              MOQ_OK);
        pipe_take_server(&p);
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, request_ok, sizeof(request_ok)));
    }
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, stale, sizeof(stale), false), MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NAMESPACE_UPDATE_RESULT);
    if (ev.kind == MOQ_EVENT_NAMESPACE_UPDATE_RESULT) {
        MOQ_TEST_CHECK_EQ_INT(ev.u.namespace_update_result.outcome,
                              MOQ_NAMESPACE_UPDATE_OUTCOME_REFUSED);
        MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_update_result.update_request_id, 8);
    }
    moq_event_cleanup(&ev);
    pipe_take_server(&p);
    const uint8_t refused[] = {5, 0, 3, 0x17, 0, 0};
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, refused, sizeof(refused)));
    MOQ_TEST_CHECK(p.s_sent_fin);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "token_cache_transactions\n");
    return failures;
}

/* Without the opt-in, an update is still admitted in full before it is
 * refused: a REGISTER it carries commits its alias (draft-22 §8.9: REGISTER
 * applies even when the request is refused), so a later USE_ALIAS resolves;
 * a duplicate REGISTER is the session error it always is; a Range Filter at
 * MAX_FILTER_RANGES 0 is refused INVALID_FILTER, not NOT_SUPPORTED (§3.3.2).
 * Family 0: PUBLISH_NAMESPACE; family 1: SUBSCRIBE_TRACKS. No event, as
 * before updates could be deferred. */
static int refusal_admits(int family, int mode)
{
    int failures = 0;
    pipe_t p;
    g_auth_token_cache = 64;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    g_auth_token_cache = 0;
    if (failures) goto done;
    handles_t h, other;
    memset(&h, 0, sizeof(h));
    memset(&other, 0, sizeof(other));
    moq_bytes_t pa[] = {part_a}, pb[] = {part_b};
    if (family == 0) failures += open_announcement(&p, &h);
    else failures += open_tracks(&p, &h, pa, 1);
    /* A second request the later message is sent on (Request ID 2). */
    failures += open_ns_sub(&p, &other, pb, 1);
    if (failures) goto done;
    if (mode == 0 || mode == 1) {
        /* REGISTER alias 5 = (type 0, 0xbb), refused NOT_SUPPORTED. */
        const uint8_t reg[] = {2, 0, 8, 4, 1, 3, 4, 1, 5, 0, 0xbb};
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, reg, sizeof(reg), false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 0);
        pipe_take_server(&p);
        const uint8_t not_supported[] = {5, 0, 3, 3, 0, 0};
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, not_supported, sizeof(not_supported)));
        MOQ_TEST_CHECK(p.s_sent_fin);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
        if (mode == 0) {
            /* The alias is registered: an update on the other request uses it. */
            MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
            const uint8_t use[] = {2, 0, 6, 6, 1, 3, 2, 2, 5};
            MOQ_TEST_CHECK_EQ_INT(to_server(&p, other.c_ref, use, sizeof(use), false), MOQ_OK);
            moq_event_t ev;
            MOQ_TEST_CHECK(next_event(p.s, &ev));
            MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_UPDATE_REQUEST);
            if (ev.kind == MOQ_EVENT_NS_SUB_UPDATE_REQUEST) {
                const moq_namespace_update_record_t *u = ev.u.ns_sub_update_request.update;
                MOQ_TEST_CHECK_EQ_SIZE(u->token_count, 1);
                if (u->token_count == 1)
                    MOQ_TEST_CHECK(u->tokens[0].token_value.len == 1 &&
                                   u->tokens[0].token_value.data[0] == 0xbb);
            }
            moq_event_cleanup(&ev);
            MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
        } else {
            /* A second REGISTER of alias 5 is a session error. */
            const uint8_t again[] = {2, 0, 8, 6, 1, 3, 4, 1, 5, 0, 0xcc};
            (void)to_server(&p, other.c_ref, again, sizeof(again), false);
            pipe_take_server(&p);
            MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_CLOSED);
        }
    } else if (family == 1) {
        /* TRACK_PROPERTY_FILTER at MAX_FILTER_RANGES 0. */
        const uint8_t update[] = {2, 0, 4, 4, 1, 0x29, 0};
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 0);
        pipe_take_server(&p);
        const uint8_t invalid_filter[] = {5, 0, 3, 0x36, 0, 0};
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, invalid_filter, sizeof(invalid_filter)));
        MOQ_TEST_CHECK(p.s_sent_fin);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
    }
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "refusal_admits family=%d mode=%d\n", family, mode);
    return failures;
}

/* Without the opt-in the established behavior stands: every family's update
 * is refused NOT_SUPPORTED with the stream closed, never acknowledged. */
static int legacy_refusal(int family)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t parts[] = {part_a};
    if (family == 0) failures += open_announcement(&p, &h);
    else if (family == 1) failures += open_ns_sub(&p, &h, parts, 1);
    else failures += open_tracks(&p, &h, parts, 1);
    if (failures) goto done;
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    moq_result_t rc = family == 0 ? moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, NULL)
        : family == 1 ? moq_session_update_ns_sub(p.c, h.nss_c, &ucfg, p.now, NULL)
        : moq_session_update_subscribe_tracks(p.c, h.trk_c, &ucfg, p.now, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    failures += pipe_move(&p, true);
    const uint8_t empty_update[] = {2, 0, 2, 2, 0};
    MOQ_TEST_CHECK(bytes_eq(p.c_sent, p.c_sent_len, empty_update, sizeof(empty_update)));
    failures += pipe_move(&p, false);
    const uint8_t not_supported[] = {5, 0, 3, 3, 0, 0};
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, not_supported, sizeof(not_supported)));
    MOQ_TEST_CHECK(p.s_sent_fin);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 0);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, family == 0 ? MOQ_EVENT_NAMESPACE_UPDATE_RESULT
        : family == 1 ? MOQ_EVENT_NS_SUB_UPDATE_RESULT
        : MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_RESULT);
    MOQ_TEST_CHECK_EQ_INT(ev.u.namespace_update_result.outcome,
                          MOQ_NAMESPACE_UPDATE_OUTCOME_ERROR);
    MOQ_TEST_CHECK_EQ_U64(ev.u.namespace_update_result.error_code,
                          MOQ_REQUEST_ERROR_NOT_SUPPORTED);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "legacy_refusal family=%d\n", family);
    return failures;
}

/* -- Prefix overlap and the prefix barrier ------------------------- */

static int found(moq_session_t *s, moq_event_kind_t kind, const moq_bytes_t *parts,
                 size_t count)
{
    int failures = 0;
    moq_event_t ev;
    bool got = next_event(s, &ev);
    MOQ_TEST_CHECK(got);
    if (!got) return failures;
    MOQ_TEST_CHECK_EQ_INT(ev.kind, kind);
    if (ev.kind != kind) {
        moq_event_cleanup(&ev);
        return failures;
    }
    const moq_namespace_t *ns = kind == MOQ_EVENT_NAMESPACE_FOUND
        ? &ev.u.namespace_found.track_namespace_suffix
        : &ev.u.namespace_gone.track_namespace_suffix;
    MOQ_TEST_CHECK_EQ_SIZE(ns->count, count);
    for (size_t i = 0; i < count && i < ns->count; ++i)
        MOQ_TEST_CHECK(bytes_eq(ns->parts[i].data, ns->parts[i].len,
                                parts[i].data, parts[i].len));
    moq_event_cleanup(&ev);
    return failures;
}

/* Narrowing (a) to (a, b) with a NAMESPACE crossing the update: before the
 * REQUEST_OK the old prefix names suffixes, after it the new one. (a, b)
 * survives as the empty suffix; (a, c) and (a, d) leave the subscriber's set
 * without a NAMESPACE_DONE, so a later DONE for one of them is a violation.
 * The publisher's own set is rebased identically. */
static int ns_sub_prefix_barrier(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    failures += open_ns_sub(&p, &h, pa, 1);
    if (failures) goto done;
    moq_bytes_t sb[] = {part_b}, sc[] = {part_c}, sd[] = {part_d};
    moq_namespace_t nb = {sb, 1}, nc = {sc, 1}, nd = {sd, 1}, empty = {NULL, 0};
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(p.s, h.nss_s, &nb, p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(p.s, h.nss_s, &nc, p.now), MOQ_OK);
    failures += pipe_move(&p, false);
    failures += found(p.c, MOQ_EVENT_NAMESPACE_FOUND, sb, 1);
    failures += found(p.c, MOQ_EVENT_NAMESPACE_FOUND, sc, 1);

    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    moq_bytes_t pab[] = {part_a, part_b};
    ucfg.flags = MOQ_NAMESPACE_UPDATE_PREFIX;
    ucfg.track_namespace_prefix = (moq_namespace_t){pab, 2};
    uint64_t id = 0;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_ns_sub(p.c, h.nss_c, &ucfg, p.now, &id), MOQ_OK);
    failures += pipe_move(&p, true);
    const uint8_t update_wire[] = {2, 0, 8, 2, 1, 0x34, 2, 1, 'a', 1, 'b'};
    MOQ_TEST_CHECK(bytes_eq(p.c_sent, p.c_sent_len, update_wire, sizeof(update_wire)));
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_UPDATE_REQUEST);
    if (ev.kind == MOQ_EVENT_NS_SUB_UPDATE_REQUEST) {
        const moq_namespace_update_record_t *u = ev.u.ns_sub_update_request.update;
        MOQ_TEST_CHECK_EQ_INT(u->flags, MOQ_NAMESPACE_UPDATE_PREFIX);
        MOQ_TEST_CHECK_EQ_SIZE(u->track_namespace_prefix.count, 2);
    }
    moq_event_cleanup(&ev);
    /* A NAMESPACE the publisher sends before answering uses the old prefix. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(p.s, h.nss_s, &nd, p.now), MOQ_OK);
    moq_accept_subscription_update_cfg_t ok = accept_cfg(id);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now), MOQ_OK);
    /* The publisher's set was rebased with the acceptance: (a, b) is the
     * empty suffix now, and (a, c) is no longer reported. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace_done(p.s, h.nss_s, &nc, p.now),
                          MOQ_ERR_WRONG_STATE);
    failures += pipe_move(&p, false);
    failures += found(p.c, MOQ_EVENT_NAMESPACE_FOUND, sd, 1);
    MOQ_TEST_CHECK(next_event(p.c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_UPDATE_RESULT);
    MOQ_TEST_CHECK_EQ_INT(ev.u.ns_sub_update_result.outcome, MOQ_NAMESPACE_UPDATE_OUTCOME_OK);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_sub_update_result.update_request_id, id);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace_done(p.s, h.nss_s, &empty, p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(p.s, h.nss_s, &nc, p.now), MOQ_OK);
    failures += pipe_move(&p, false);
    failures += found(p.c, MOQ_EVENT_NAMESPACE_GONE, NULL, 0);
    failures += found(p.c, MOQ_EVENT_NAMESPACE_FOUND, sc, 1);   /* (a, b, c) */
    /* (a, d) left the subscriber's set at the barrier: its DONE now names
     * nothing the subscriber holds. */
    const uint8_t done_d[] = {0x0e, 0, 3, 1, 1, 'd'};
    (void)moq_session_on_bidi_stream_bytes(p.c,
        moq_stream_ref_from_u64(h.c_ref), done_d, sizeof(done_d), false, p.now);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.c), MOQ_SESS_CLOSED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "ns_sub_prefix_barrier\n");
    return failures;
}

/* -- Parameter contexts and error domains -------------------------- */

typedef struct bad_case {
    const char *name;
    int family;              /* 0 announcement, 1 namespace sub, 2 tracks */
    uint8_t wire[16];
    size_t len;
    uint64_t close_code;     /* session close code; 0 = request refusal */
} bad_case_t;

/* Each family accepts only its own parameters (§9.5); a value outside
 * FORWARD's domain, a reused or wrong-parity Request ID, and a second update
 * beyond the advertised one close the session with the code each rule names.
 * The client's requests used ID 0, so its first update is 2. */
static const bad_case_t bad_cases[] = {
    {"announcement FORWARD", 0, {2, 0, 4, 2, 1, 0x10, 1}, 7, 0x3},
    {"announcement PREFIX", 0, {2, 0, 6, 2, 1, 0x34, 1, 1, 'm'}, 9, 0x3},
    {"namespace FORWARD", 1, {2, 0, 4, 2, 1, 0x10, 0}, 7, 0x3},
    {"tracks FORWARD 2", 2, {2, 0, 4, 2, 1, 0x10, 2}, 7, 0x3},
    {"tracks GROUP_ORDER", 2, {2, 0, 4, 2, 1, 0x22, 1}, 7, 0x3},
    {"tracks reused ID", 2, {2, 0, 2, 0, 0}, 5, 0x4},
    {"tracks odd ID", 2, {2, 0, 2, 3, 0}, 5, 0x4},
};

static int open_family(pipe_t *p, handles_t *h, int family)
{
    moq_bytes_t parts[] = {part_a};
    if (family == 0) return open_announcement(p, h);
    if (family == 1) return open_ns_sub(p, h, parts, 1);
    return open_tracks(p, h, parts, 1);
}

static int malformed_context(const bad_case_t *bc)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_family(&p, &h, bc->family);
    if (failures) goto done;
    (void)to_server(&p, h.c_ref, bc->wire, bc->len, false);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s) > 0 &&
        moq_session_state(p.s) != MOQ_SESS_CLOSED, 0);
    failures += pipe_move(&p, false);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK_EQ_U64(p.s_close_code, bc->close_code);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "malformed_context %s\n", bc->name);
    return failures;
}

/* A second update while the first is owed exceeds the advertised
 * MAX_REQUEST_UPDATES of one (§9.1.7): TOO_MANY_REQUEST_UPDATES. */
static int update_credit(int family)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_family(&p, &h, family);
    if (failures) goto done;
    const uint8_t first[] = {2, 0, 2, 2, 0}, second[] = {2, 0, 2, 4, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, first, sizeof(first), false), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    (void)to_server(&p, h.c_ref, second, sizeof(second), false);
    failures += pipe_move(&p, false);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK_EQ_U64(p.s_close_code, 0x1b);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "update_credit family=%d\n", family);
    return failures;
}

/* A Range Filter at the advertised zero limit is a request-local refusal
 * (§3.3.2), not a session error: INVALID_FILTER, the stream closed, the
 * refusal reported, and an unrelated request untouched. */
static int range_filter_refused(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t other, h;
    memset(&other, 0, sizeof(other));
    memset(&h, 0, sizeof(h));
    moq_bytes_t pz[] = {part_d}, pa[] = {part_a};
    failures += open_ns_sub(&p, &other, pz, 1);
    failures += open_tracks(&p, &h, pa, 1);
    if (failures) goto done;
    /* TRACK_PROPERTY_FILTER with a zero-length value (a clear). */
    const uint8_t update[] = {2, 0, 4, 4, 1, 0x29, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_RESULT);
    if (ev.kind == MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_RESULT) {
        const moq_subscribe_tracks_update_result_event_t *r =
            &ev.u.subscribe_tracks_update_result;
        MOQ_TEST_CHECK_EQ_U64(r->handle._opaque, h.trk_s._opaque);
        MOQ_TEST_CHECK_EQ_U64(r->update_request_id, 4);
        MOQ_TEST_CHECK_EQ_INT(r->outcome, MOQ_NAMESPACE_UPDATE_OUTCOME_REFUSED);
        MOQ_TEST_CHECK_EQ_INT(r->local_role, MOQ_REQUEST_ROLE_RESPONDER);
        MOQ_TEST_CHECK_EQ_U64(r->error_code, MOQ_REQUEST_ERROR_INVALID_FILTER);
    }
    moq_event_cleanup(&ev);
    failures += pipe_move(&p, false);
    const uint8_t invalid_filter[] = {5, 0, 3, 0x36, 0, 0};
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, invalid_filter, sizeof(invalid_filter)));
    MOQ_TEST_CHECK(p.s_sent_fin);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
    moq_accept_subscription_update_cfg_t ok = accept_cfg(4);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks_update(p.s, h.trk_s, &ok, p.now),
                          MOQ_ERR_STALE_HANDLE);
    moq_bytes_t sx[] = {part_b};
    moq_namespace_t nx = {sx, 1};
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(p.s, other.nss_s, &nx, p.now), MOQ_OK);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "range_filter_refused\n");
    return failures;
}

/* -- Overlap spaces and the tracks barrier ------------------------ */

/* Same-type overlap is PREFIX_OVERLAP; the namespace-subscription and
 * track-subscription spaces are independent; a prefix an update would
 * install is claimed until it concludes. */
static int prefix_overlap(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t na, nb, tb;
    memset(&na, 0, sizeof(na));
    memset(&nb, 0, sizeof(nb));
    memset(&tb, 0, sizeof(tb));
    moq_bytes_t pa[] = {part_a}, pb[] = {part_b};
    failures += open_ns_sub(&p, &na, pa, 1);   /* request ID 0 */
    failures += open_ns_sub(&p, &nb, pb, 1);   /* 2 */
    failures += open_tracks(&p, &tb, pb, 1);   /* 4 */
    if (failures) goto done;

    /* The requester refuses an overlapping update of its own locally. */
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    moq_bytes_t pax[] = {part_a, part_c};
    ucfg.flags = MOQ_NAMESPACE_UPDATE_PREFIX;
    ucfg.track_namespace_prefix = (moq_namespace_t){pax, 2};
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_ns_sub(p.c, nb.nss_c, &ucfg, p.now, NULL),
                          MOQ_ERR_INVAL);
    /* The responder refuses one that reaches it: (b) -> (a, c) overlaps (a).
     * Injected under a Request ID the client will not reach in this test. */
    const uint8_t overlap[] = {2, 0, 8, 100, 1, 0x34, 2, 1, 'a', 1, 'c'};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, nb.c_ref, overlap, sizeof(overlap), false), MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_UPDATE_RESULT);
    MOQ_TEST_CHECK_EQ_INT(ev.u.ns_sub_update_result.outcome,
                          MOQ_NAMESPACE_UPDATE_OUTCOME_REFUSED);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_sub_update_result.error_code,
                          MOQ_REQUEST_ERROR_PREFIX_OVERLAP);
    moq_event_cleanup(&ev);
    pipe_take_server(&p);
    const uint8_t prefix_overlap_err[] = {5, 0, 3, 0x30, 0, 0};
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, prefix_overlap_err,
                            sizeof(prefix_overlap_err)));
    MOQ_TEST_CHECK(p.s_sent_fin);
    MOQ_TEST_CHECK_EQ_INT(moq_session_ns_sub_owed_update(p.s, nb.nss_s,
        &(moq_namespace_update_record_t){0}, sizeof(moq_namespace_update_record_t)),
        MOQ_ERR_STALE_HANDLE);

    /* Track discovery to (a) is legal beside a namespace subscription of (a). */
    moq_namespace_update_cfg_init(&ucfg);
    ucfg.flags = MOQ_NAMESPACE_UPDATE_PREFIX;
    ucfg.track_namespace_prefix = (moq_namespace_t){pa, 1};
    uint64_t id = 0;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_subscribe_tracks(p.c, tb.trk_c, &ucfg, p.now, &id),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(id, 6);
    failures += pipe_move(&p, true);
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_REQUEST);
    moq_event_cleanup(&ev);
    /* The owed (a) is claimed: a new SUBSCRIBE_TRACKS of (a, d) overlaps it. */
    const uint8_t tracks_ad[] = {0x51, 0, 7, 102, 2, 1, 'a', 1, 'd', 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, 0x77, tracks_ad, sizeof(tracks_ad), false), MOQ_OK);
    pipe_take_server(&p);
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, prefix_overlap_err,
                            sizeof(prefix_overlap_err)));
    moq_accept_subscription_update_cfg_t ok = accept_cfg(6);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks_update(p.s, tb.trk_s, &ok, p.now),
                          MOQ_OK);
    failures += pipe_move(&p, false);
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, request_ok, sizeof(request_ok)));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "prefix_overlap\n");
    return failures;
}

/* Prefix and FORWARD in one update. PUBLISH_SKIPPED after the REQUEST_OK
 * names a suffix of the new prefix. */
static int tracks_prefix_forward(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    failures += open_tracks(&p, &h, pa, 1);
    if (failures) goto done;
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    moq_bytes_t pb[] = {part_b};
    ucfg.flags = MOQ_NAMESPACE_UPDATE_PREFIX | MOQ_NAMESPACE_UPDATE_FORWARD;
    ucfg.track_namespace_prefix = (moq_namespace_t){pb, 1};
    ucfg.forward = false;
    uint64_t id = 0;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_subscribe_tracks(p.c, h.trk_c, &ucfg, p.now, &id),
                          MOQ_OK);
    failures += pipe_move(&p, true);
    const uint8_t wire[] = {2, 0, 8, 2, 2, 0x10, 0, 0x24, 1, 1, 'b'};
    MOQ_TEST_CHECK(bytes_eq(p.c_sent, p.c_sent_len, wire, sizeof(wire)));
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_REQUEST);
    if (ev.kind == MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_REQUEST) {
        const moq_namespace_update_record_t *u = ev.u.subscribe_tracks_update_request.update;
        MOQ_TEST_CHECK_EQ_INT(u->flags, MOQ_NAMESPACE_UPDATE_PREFIX | MOQ_NAMESPACE_UPDATE_FORWARD);
        MOQ_TEST_CHECK(!u->forward);
        MOQ_TEST_CHECK_EQ_SIZE(u->track_namespace_prefix.count, 1);
        MOQ_TEST_CHECK_EQ_SIZE(u->token_count, 0);
    }
    moq_event_cleanup(&ev);
    moq_accept_subscription_update_cfg_t ok = accept_cfg(id);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks_update(p.s, h.trk_s, &ok, p.now),
                          MOQ_OK);
    moq_bytes_t sx[] = {part_c};
    moq_namespace_t nx = {sx, 1};
    const moq_bytes_t name_v = MOQ_BYTES_LITERAL("v");
    MOQ_TEST_CHECK_EQ_INT(moq_session_send_publish_blocked(p.s, h.trk_s, &nx,
        name_v, p.now), MOQ_OK);
    failures += pipe_move(&p, false);
    MOQ_TEST_CHECK(next_event(p.c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_RESULT);
    MOQ_TEST_CHECK_EQ_INT(ev.u.subscribe_tracks_update_result.outcome,
                          MOQ_NAMESPACE_UPDATE_OUTCOME_OK);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(next_event(p.c, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_PUBLISH_BLOCKED);
    moq_event_cleanup(&ev);
    /* The client's own space now holds (b): (b, x) overlaps it locally. */
    moq_subscribe_tracks_cfg_t tcfg;
    moq_subscribe_tracks_cfg_init(&tcfg);
    moq_bytes_t pbx[] = {part_b, part_c};
    tcfg.track_namespace_prefix = (moq_namespace_t){pbx, 2};
    moq_track_sub_handle_t other;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(p.c, &tcfg, p.now, &other),
                          MOQ_ERR_INVAL);
    tcfg.track_namespace_prefix = (moq_namespace_t){pa, 1};
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(p.c, &tcfg, p.now, &other), MOQ_OK);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "tracks_prefix_forward\n");
    return failures;
}

/* -- Pressure, FIN, RESET, GOAWAY and pipelining ------------------ */

/* A full event queue holds the update before any effect: no Request ID is
 * admitted and nothing is owed until the event can be surfaced. */
static int event_pressure(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 1, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    moq_bytes_t pa[] = {part_a}, pb[] = {part_b};
    failures += open_ns_sub(&p, &a, pa, 1);   /* ID 0 */
    if (failures) goto done;
    /* A second request's event fills the one-slot queue. */
    moq_subscribe_namespace_cfg_t cfg;
    moq_subscribe_namespace_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){pb, 1};
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_namespace(p.c, &cfg, p.now, &b.nss_c), MOQ_OK);
    failures += pipe_move(&p, true);                  /* ID 2 */
    const uint8_t update[] = {2, 0, 2, 4, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, a.c_ref, update, sizeof(update), false),
                          MOQ_ERR_WOULD_BLOCK);
    moq_namespace_update_record_t rec;
    MOQ_TEST_CHECK_EQ_INT(moq_session_ns_sub_owed_update(p.s, a.nss_s, &rec, sizeof(rec)),
                          MOQ_ERR_WRONG_STATE);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, a.c_ref, NULL, 0, false), MOQ_OK);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_NS_SUB_UPDATE_REQUEST);
    MOQ_TEST_CHECK_EQ_U64(ev.u.ns_sub_update_request.update_request_id, 4);
    moq_event_cleanup(&ev);
    /* A further empty feed admits nothing twice. */
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, a.c_ref, NULL, 0, false), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_ns_sub_owed_update(p.s, a.nss_s, &rec, sizeof(rec)),
                          MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(rec.update_request_id, 4);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "event_pressure\n");
    return failures;
}

/* The requester's FIN is directional (§6.4.2.2): the answer stays owed and is
 * still sent. A RESET cancels the request and the answer with it. A GOAWAY
 * this side sent on the request leaves the owed answer to be given. */
static int owed_across_signals(int signal)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    failures += open_ns_sub(&p, &h, pa, 1);
    if (failures) goto done;
    const uint8_t update[] = {2, 0, 2, 2, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    moq_stream_ref_t sref = moq_stream_ref_from_u64(REF_CLIENT_TO_SERVER + h.c_ref);
    if (signal == 0) {
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, NULL, 0, true), MOQ_OK);
    } else if (signal == 1) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_reset(p.s, sref, 7, p.now), MOQ_OK);
        drain_events(p.s);
    } else {
        moq_request_goaway_cfg_t gcfg;
        moq_request_goaway_cfg_init(&gcfg);
        MOQ_TEST_CHECK_EQ_INT(moq_session_request_goaway_ns_sub(p.s, h.nss_s, &gcfg, p.now),
                              MOQ_OK);
        failures += pipe_move(&p, false);
    }
    moq_accept_subscription_update_cfg_t ok = accept_cfg(2);
    moq_result_t rc = moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, signal == 1 ? MOQ_ERR_STALE_HANDLE : MOQ_OK);
    if (signal != 1) {
        failures += pipe_move(&p, false);
        MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, request_ok, sizeof(request_ok)));
        MOQ_TEST_CHECK(!p.s_sent_fin);
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "owed_across_signals signal=%d\n", signal);
    return failures;
}

/* An answer owed for a namespace update (SUBSCRIBE_NAMESPACE, family 0, or
 * SUBSCRIBE_TRACKS, family 1) survives request migration: a GOAWAY this
 * side sent followed by the requester's FIN, the requester's GOAWAY, and
 * both crossing (draft-22 §9.2: a GOAWAY "does not impact subscription
 * state"; §6.4.2.2: FIN is directional and required responses precede it;
 * §9.5: exactly one update response), and an update that crosses this
 * side's GOAWAY (mode 3). The answer is still given, as one REQUEST_OK,
 * with no FIN before it. */
static int owed_across_goaway(int family, int mode)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    if (family == 0) failures += open_ns_sub(&p, &h, pa, 1);
    else failures += open_tracks(&p, &h, pa, 1);
    if (failures) goto done;
    const uint8_t update[] = {2, 0, 2, 2, 0};
    if (mode != 3) {
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    }
    moq_request_goaway_cfg_t gcfg;
    moq_request_goaway_cfg_init(&gcfg);
    if (mode == 0 || mode == 2 || mode == 3) {      /* this side's GOAWAY */
        moq_result_t grc = family == 0
            ? moq_session_request_goaway_ns_sub(p.s, h.nss_s, &gcfg, p.now)
            : moq_session_request_goaway_subscribe_tracks(p.s, h.trk_s, &gcfg, p.now);
        MOQ_TEST_CHECK_EQ_INT(grc, MOQ_OK);
        pipe_take_server(&p);
        MOQ_TEST_CHECK(!p.s_sent_fin);
    }
    if (mode == 1 || mode == 2) {      /* the requester's GOAWAY (empty URI) */
        const uint8_t goaway[] = {0x10, 0, 2, 0, 0};
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, goaway, sizeof(goaway), false), MOQ_OK);
        drain_events(p.s);
        pipe_take_server(&p);
        MOQ_TEST_CHECK(!p.s_sent_fin);
    }
    if (mode == 3) {                   /* the update crosses the GOAWAY */
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    }
    if (mode == 0 || mode == 3) {      /* then the requester's FIN */
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, NULL, 0, true), MOQ_OK);
        drain_events(p.s);
        pipe_take_server(&p);
        MOQ_TEST_CHECK(!p.s_sent_fin);
    }
    moq_accept_subscription_update_cfg_t ok = accept_cfg(2);
    moq_result_t rc = family == 0
        ? moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now)
        : moq_session_accept_subscribe_tracks_update(p.s, h.trk_s, &ok, p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    /* Exactly one answer: the first send after acceptance is REQUEST_OK. */
    bool answered = false, fin_before = false;
    moq_action_t a;
    while (moq_session_poll_actions(p.s, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM && !answered) {
            answered = bytes_eq(a.u.send_bidi_stream.data, a.u.send_bidi_stream.len,
                                request_ok, sizeof(request_ok));
            if (!answered && a.u.send_bidi_stream.fin) fin_before = true;
        } else if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM && !answered) {
            fin_before = true;
        }
        MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(answered);
    MOQ_TEST_CHECK(!fin_before);
    rc = family == 0 ? moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now)
                     : moq_session_accept_subscribe_tracks_update(p.s, h.trk_s, &ok, p.now);
    MOQ_TEST_CHECK(rc != MOQ_OK);      /* answered once */
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_ESTABLISHED);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "owed_across_goaway family=%d mode=%d\n", family, mode);
    return failures;
}

/* An answer the output cannot take now is refused whole and retried. */
static int answer_pressure(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 4);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    failures += open_ns_sub(&p, &h, pa, 1);
    if (failures) goto done;
    const uint8_t update[] = {2, 0, 2, 2, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, h.c_ref, update, sizeof(update), false), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 1);
    moq_bytes_t parts[4][1] = {{part_a}, {part_b}, {part_c}, {part_d}};
    size_t queued = 0;
    for (size_t i = 0; i < 4; ++i) {
        moq_namespace_t n = {parts[i], 1};
        if (moq_session_send_namespace(p.s, h.nss_s, &n, p.now) == MOQ_OK) queued++;
    }
    MOQ_TEST_CHECK(queued > 0);
    moq_accept_subscription_update_cfg_t ok = accept_cfg(2);
    moq_result_t rc = moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
    if (rc == MOQ_ERR_WOULD_BLOCK) {
        moq_namespace_update_record_t rec;
        MOQ_TEST_CHECK_EQ_INT(moq_session_ns_sub_owed_update(p.s, h.nss_s, &rec, sizeof(rec)),
                              MOQ_OK);
        failures += pipe_move(&p, false);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now),
                              MOQ_OK);
    }
    failures += pipe_move(&p, false);
    MOQ_TEST_CHECK(bytes_eq(p.s_sent, p.s_sent_len, request_ok, sizeof(request_ok)));
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "answer_pressure\n");
    return failures;
}

/* An update pipelined behind SUBSCRIBE_TRACKS waits for the answer to the
 * request; a rejected request still consumes the update's Request ID. */
static int pipelined_tracks_update(bool accept)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    const uint8_t wire[] = {0x51, 0, 5, 0, 1, 1, 'a', 0,  2, 0, 2, 2, 0};
    MOQ_TEST_CHECK_EQ_INT(to_server(&p, 0x55, wire, sizeof(wire), false),
                          MOQ_ERR_WOULD_BLOCK);
    moq_event_t ev;
    MOQ_TEST_CHECK(next_event(p.s, &ev));
    MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST);
    moq_track_sub_handle_t h = ev.u.subscribe_tracks_request.handle;
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK_EQ_SIZE(drain_events(p.s), 0);
    if (accept) {
        moq_accept_subscribe_tracks_cfg_t acfg;
        moq_accept_subscribe_tracks_cfg_init(&acfg);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks(p.s, h, &acfg, p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(to_server(&p, 0x55, NULL, 0, false), MOQ_OK);
        MOQ_TEST_CHECK(next_event(p.s, &ev));
        MOQ_TEST_CHECK_EQ_INT(ev.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_REQUEST);
        MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_tracks_update_request.update_request_id, 2);
        moq_event_cleanup(&ev);
        moq_accept_subscription_update_cfg_t ok = accept_cfg(2);
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe_tracks_update(p.s, h, &ok, p.now),
                              MOQ_OK);
        moq_action_t a;
        size_t oks = 0;
        while (moq_session_poll_actions(p.s, &a, 1)) {
            if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
                bytes_eq(a.u.send_bidi_stream.data, a.u.send_bidi_stream.len,
                         request_ok, sizeof(request_ok)))
                oks++;
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK_EQ_SIZE(oks, 2);
    } else {
        moq_reject_subscribe_tracks_cfg_t rcfg;
        moq_reject_subscribe_tracks_cfg_init(&rcfg);
        rcfg.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
        MOQ_TEST_CHECK_EQ_INT(moq_session_reject_subscribe_tracks(p.s, h, &rcfg, p.now), MOQ_OK);
        /* ID 2 was the update's: a new request reusing it is a duplicate. */
        const uint8_t reuse[] = {0x51, 0, 5, 2, 1, 1, 'z', 0};
        (void)to_server(&p, 0x56, reuse, sizeof(reuse), false);
        failures += pipe_move(&p, false);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.s), MOQ_SESS_CLOSED);
        MOQ_TEST_CHECK_EQ_U64(p.s_close_code, 0x4);
    }
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "pipelined_tracks_update accept=%d\n", accept);
    return failures;
}

/* -- Requester-side refusals and the draft gate ---------------------- */

/* A malformed or misdirected update is refused before anything is sent or
 * a Request ID is consumed: no config, a parameter the family does not
 * take, an unknown flag, a short struct_size, the announcement's receiver,
 * and a stale handle. */
static int requester_refusals(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_22, 0, 0);
    if (failures) goto done;
    handles_t h;
    memset(&h, 0, sizeof(h));
    failures += open_announcement(&p, &h);
    if (failures) goto done;
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, NULL, p.now, NULL),
                          MOQ_ERR_INVAL);
    ucfg.flags = MOQ_NAMESPACE_UPDATE_FORWARD;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, NULL),
                          MOQ_ERR_INVAL);
    ucfg.flags = 0x80;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, NULL),
                          MOQ_ERR_INVAL);
    ucfg.flags = 0;
    ucfg.struct_size = MOQ_NAMESPACE_UPDATE_CFG_V0_SIZE - 1;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, NULL),
                          MOQ_ERR_INVAL);
    moq_namespace_update_cfg_init(&ucfg);
    /* The receiver of the announcement cannot update it. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.s, h.ann_s, &ucfg, p.now, NULL),
                          MOQ_ERR_WRONG_STATE);
    moq_announcement_t stale = h.ann_c;
    stale._opaque ^= (uint64_t)1 << 20;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, stale, &ucfg, p.now, NULL),
                          MOQ_ERR_STALE_HANDLE);
    moq_action_t a;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(p.c, &a, 1), 0);
    /* Nothing was consumed: the next update is the next Request ID. */
    uint64_t id = 0;
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_namespace(p.c, h.ann_c, &ucfg, p.now, &id), MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(id, 2);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "requester_refusals\n");
    return failures;
}

/* Draft-18 has no namespace updates: every call is UNSUPPORTED with no
 * output, even with the opt-in set. */
static int legacy_draft(void)
{
    int failures = 0;
    pipe_t p;
    failures += pipe_open(&p, MOQ_VERSION_DRAFT_18, 0, 0);
    if (failures) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(p.s, true), MOQ_OK);
    handles_t h;
    memset(&h, 0, sizeof(h));
    moq_bytes_t pa[] = {part_a};
    failures += open_ns_sub(&p, &h, pa, 1);
    if (failures) goto done;
    moq_namespace_update_cfg_t ucfg;
    moq_namespace_update_cfg_init(&ucfg);
    MOQ_TEST_CHECK_EQ_INT(moq_session_update_ns_sub(p.c, h.nss_c, &ucfg, p.now, NULL),
                          MOQ_ERR_UNSUPPORTED);
    moq_namespace_update_record_t rec;
    MOQ_TEST_CHECK_EQ_INT(moq_session_ns_sub_owed_update(p.s, h.nss_s, &rec, sizeof(rec)),
                          MOQ_ERR_UNSUPPORTED);
    moq_accept_subscription_update_cfg_t ok = accept_cfg(0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_ns_sub_update(p.s, h.nss_s, &ok, p.now),
                          MOQ_ERR_UNSUPPORTED);
    moq_action_t a;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(p.c, &a, 1), 0);
done:
    pipe_close(&p);
    if (failures) fprintf(stderr, "legacy_draft\n");
    return failures;
}

/* -- PREFIX_OVERLAP is per direction --------------------------------- *
 * §4.2: "Within a session, if a publisher receives a SUBSCRIBE_NAMESPACE with
 * a Track Namespace Prefix that shares a common prefix with an established
 * SUBSCRIBE_NAMESPACE, it MUST respond with ... PREFIX_OVERLAP" (§3.6 says the
 * same of SUBSCRIBE_TRACKS). The comparison is between the subscriptions one
 * subscriber holds at one publisher: two endpoints that each discover the
 * other with the empty prefix do not overlap. Each side subscribes with the
 * empty prefix, in both orders, for both families: the local call succeeds,
 * the peer surfaces and accepts the request, and the requester gets its OK.
 * A second request in the same direction is still refused locally, and on
 * draft-22 an update of the second side's prefix to one its peer already
 * subscribed with is sent. */
static int sub_family(moq_session_t *s, int family, const moq_bytes_t *parts,
                      size_t count, uint64_t now, uint64_t *out_handle)
{
    moq_namespace_t prefix = {(moq_bytes_t *)parts, count};
    if (family == 0) {
        moq_subscribe_namespace_cfg_t cfg;
        moq_subscribe_namespace_cfg_init(&cfg);
        cfg.track_namespace_prefix = prefix;
        cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t h;
        moq_result_t rc = moq_session_subscribe_namespace(s, &cfg, now, &h);
        if (out_handle) *out_handle = h._opaque;
        return rc;
    }
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = prefix;
    moq_track_sub_handle_t h;
    moq_result_t rc = moq_session_subscribe_tracks(s, &cfg, now, &h);
    if (out_handle) *out_handle = h._opaque;
    return rc;
}

/* Accept the one request of `family` the responder surfaced; true when it
 * surfaced. */
static bool accept_family(moq_session_t *s, int family, uint64_t now)
{
    bool seen = false;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (family == 0 && ev.kind == MOQ_EVENT_NS_SUB_REQUEST) {
            moq_accept_ns_sub_cfg_t acfg;
            moq_accept_ns_sub_cfg_init(&acfg);
            seen = moq_session_accept_ns_sub(s, ev.u.ns_sub_request.handle,
                                             &acfg, now) == MOQ_OK;
        } else if (family == 1 && ev.kind == MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST) {
            moq_accept_subscribe_tracks_cfg_t acfg;
            moq_accept_subscribe_tracks_cfg_init(&acfg);
            seen = moq_session_accept_subscribe_tracks(s,
                ev.u.subscribe_tracks_request.handle, &acfg, now) == MOQ_OK;
        }
        moq_event_cleanup(&ev);
    }
    return seen;
}

/* The requester's answer: 1 OK, -1 error, 0 none. */
static int family_answer(moq_session_t *s, int family)
{
    int answer = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == (family == 0 ? MOQ_EVENT_NS_SUB_OK : MOQ_EVENT_SUBSCRIBE_TRACKS_OK))
            answer = 1;
        if (ev.kind == (family == 0 ? MOQ_EVENT_NS_SUB_ERROR : MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR))
            answer = -1;
        moq_event_cleanup(&ev);
    }
    return answer;
}

static int opposite_direction_overlap(moq_version_t version, int family, bool server_first)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_simpair_create(&cfg, &sp), MOQ_OK);
    if (!sp) return failures;
    MOQ_TEST_CHECK_EQ_INT(moq_simpair_start(sp), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain_events(c);
    drain_events(sv);
    moq_session_t *first = server_first ? sv : c;
    moq_session_t *second = server_first ? c : sv;
    uint64_t now = 1000;
    static const moq_bytes_t a_only[] = { { (const uint8_t *)"a", 1 } };

    /* The first side subscribes with ["a"], then the second with the empty
     * prefix, which shares a common prefix with it. */
    uint64_t h_first = 0, h_second = 0;
    MOQ_TEST_CHECK_EQ_INT(sub_family(first, family, a_only, 1, now, &h_first), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    MOQ_TEST_CHECK(accept_family(second, family, now));
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    MOQ_TEST_CHECK_EQ_INT(family_answer(first, family), 1);

    MOQ_TEST_CHECK_EQ_INT(sub_family(second, family, NULL, 0, now, &h_second), MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    MOQ_TEST_CHECK(accept_family(first, family, now));
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    MOQ_TEST_CHECK_EQ_INT(family_answer(second, family), 1);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(c), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(sv), MOQ_SESS_ESTABLISHED);

    /* Same direction still overlaps: the second side's own empty prefix
     * covers ["a"]. */
    MOQ_TEST_CHECK_EQ_INT(sub_family(second, family, a_only, 1, now, NULL),
                          MOQ_ERR_INVAL);

    /* Draft-22: the first side moves its prefix to the empty one the second
     * side subscribed with. Only its own request is compared. */
    if (version == MOQ_VERSION_DRAFT_22) {
        moq_namespace_update_cfg_t ucfg;
        moq_namespace_update_cfg_init(&ucfg);
        ucfg.flags = MOQ_NAMESPACE_UPDATE_PREFIX;
        ucfg.track_namespace_prefix = (moq_namespace_t){ NULL, 0 };
        uint64_t uid = 0;
        MOQ_TEST_CHECK_EQ_INT(moq_session_set_defer_namespace_updates(second, true),
                              MOQ_OK);
        moq_result_t urc = family == 0
            ? moq_session_update_ns_sub(first, (moq_ns_sub_handle_t){ h_first },
                                        &ucfg, now, &uid)
            : moq_session_update_subscribe_tracks(first,
                (moq_track_sub_handle_t){ h_first }, &ucfg, now, &uid);
        MOQ_TEST_CHECK_EQ_INT(urc, MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 64, NULL);
        /* The responder's own empty-prefix request is not compared: the
         * update surfaces for an answer instead of a PREFIX_OVERLAP refusal. */
        bool surfaced = false;
        moq_event_t ev;
        while (moq_session_poll_events(second, &ev, 1) > 0) {
            if (ev.kind == (family == 0 ? MOQ_EVENT_NS_SUB_UPDATE_REQUEST
                                        : MOQ_EVENT_SUBSCRIBE_TRACKS_UPDATE_REQUEST))
                surfaced = true;
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK(surfaced);
    }
    moq_simpair_destroy(sp);
    if (failures)
        fprintf(stderr, "opposite_direction_overlap v=%u family=%d server_first=%d\n",
                (unsigned)version, family, (int)server_first);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += announcement_token_refresh(false);
    failures += announcement_token_refresh(true);
    failures += token_cache_transactions();
    failures += full_width_token_update();
    for (int f = 0; f < 3; ++f) failures += legacy_refusal(f);
    for (int f = 0; f < 2; ++f)
        for (int mode = 0; mode < 3; ++mode)
            if (mode < 2 || f == 1) failures += refusal_admits(f, mode);
    failures += ns_sub_prefix_barrier();
    for (size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); ++i)
        failures += malformed_context(&bad_cases[i]);
    for (int f = 0; f < 3; ++f) failures += update_credit(f);
    failures += range_filter_refused();
    failures += prefix_overlap();
    {
        const moq_version_t versions[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18,
                                           MOQ_VERSION_DRAFT_22 };
        for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v)
            for (int family = 0; family < 2; ++family)
                for (int order = 0; order < 2; ++order) {
                    /* Draft-16 has no SUBSCRIBE_TRACKS. */
                    if (versions[v] == MOQ_VERSION_DRAFT_16 && family == 1) continue;
                    failures += opposite_direction_overlap(versions[v], family, order != 0);
                }
    }
    failures += tracks_prefix_forward();
    failures += event_pressure();
    for (int sig = 0; sig < 3; ++sig) failures += owed_across_signals(sig);
    for (int family = 0; family < 2; ++family)
        for (int mode = 0; mode < 4; ++mode) failures += owed_across_goaway(family, mode);
    failures += answer_pressure();
    failures += pipelined_tracks_update(true);
    failures += pipelined_tracks_update(false);
    failures += requester_refusals();
    failures += legacy_draft();
    if (!failures) MOQ_TEST_PASS("D22 namespace updates");
    return failures ? 1 : 0;
}
