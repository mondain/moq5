/*
 * Draft-22 profile: the handshake establishes both sides over the simulator,
 * GOAWAY drains, the data plane is draft-18's, the request-error registry is
 * the draft's own, and the version is registered for explicit selection.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <moq/control_d18.h>
#include "test_support.h"
#include "test_binding_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include <string.h>
#include <stdlib.h>

static bool bytes_equal(moq_bytes_t b, const char *s)
{
    return b.len == strlen(s) && (b.len == 0 || memcmp(b.data, s, b.len) == 0);
}

static moq_bytes_t private_peer_implementation(const moq_session_t *s)
{
    return (moq_bytes_t){s->peer_implementation, s->peer_implementation_len};
}

/* Decode the Setup Options of the SETUP a freshly started client queued on
 * its control channel. */
static bool decode_queued_setup(moq_session_t *s, moq_d22_setup_options_t *out)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind != MOQ_ACTION_OPEN_UNI_CONTROL) { moq_action_cleanup(&a); continue; }
        moq_buf_reader_t r;
        moq_buf_reader_init(&r, a.u.open_uni_control.data, a.u.open_uni_control.len);
        moq_control_envelope_t env;
        bool ok = moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                  env.msg_type == MOQ_D22_SETUP &&
                  moq_d22_decode_setup_options(env.payload, env.payload_len, out) == MOQ_OK;
        moq_action_cleanup(&a);
        return ok;
    }
    return false;
}

static int count_kind(moq_session_t *s, moq_event_kind_t kind)
{
    moq_event_t ev; int n = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) { if (ev.kind == kind) n++; moq_event_cleanup(&ev); }
    return n;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    bool found = false;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (!found && ev.kind == kind) { *out = ev; found = true; continue; }
        moq_event_cleanup(&ev);
    }
    return found;
}

/* Feed the server a draft-22 SUBSCRIBE for live/v on a fresh request stream. */
static moq_result_t feed_subscribe(moq_session_t *sv, uint64_t ref, uint64_t request_id)
{
    uint8_t ns_buf[16], buf[64];
    moq_buf_writer_t nw;
    moq_buf_writer_init(&nw, ns_buf, sizeof(ns_buf));
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { parts, 1 };
    if (moq_d22_encode_namespace(&nw, &ns) != MOQ_OK) return MOQ_ERR_INVAL;
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_SUBSCRIBE;
    m.request_id = request_id;
    m.ns.encoded.data = ns_buf;
    m.ns.encoded.len = moq_buf_writer_offset(&nw);
    m.ns.count = 1;
    m.track_name = MOQ_BYTES_LITERAL("v");
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    if (moq_d22_encode_message(&w, &m, NULL, 0, &ctx) != MOQ_OK) return MOQ_ERR_INVAL;
    return moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(ref), buf,
                                            moq_buf_writer_offset(&w), false, 10);
}

/* The REQUEST_ERROR code the server queued on `ref`, or UINT64_MAX. */
static uint64_t queued_error_code(moq_session_t *sv, uint64_t ref)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.request_type = MOQ_D22_SUBSCRIBE;
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                env.msg_type == MOQ_D22_REQUEST_ERROR &&
                moq_d22_decode_message(MOQ_D22_REQUEST_ERROR, env.payload,
                                       env.payload_len, &ctx, &m) == MOQ_OK)
                code = m.code;
        }
        moq_action_cleanup(&a);
    }
    return code;
}

/* A second SUBSCRIBE to a track this side already serves is a concurrent
 * subscription (§3.1): it surfaces as its own request, nothing is refused
 * on its stream, and the session stays up. */
static bool second_subscribe_admitted(void)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = MOQ_VERSION_DRAFT_22;
    scfg.client_send_request_capacity = true; scfg.client_initial_request_capacity = 32;
    scfg.server_send_request_capacity = true; scfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return false;
    bool admitted = false;
    if (moq_simpair_start(sp) < 0) goto out;
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *sv = moq_simpair_server(sp);
    (void)count_kind(sv, MOQ_EVENT_SETUP_COMPLETE);
    moq_event_t ev;
    if (feed_subscribe(sv, 0x7000, 0) != MOQ_OK) goto out;
    if (!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) goto out;
    moq_event_cleanup(&ev);
    (void)queued_error_code(sv, 0x7000);
    if (feed_subscribe(sv, 0x7004, 2) != MOQ_OK) goto out;
    if (!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) goto out;
    moq_event_cleanup(&ev);
    admitted = queued_error_code(sv, 0x7004) == UINT64_MAX &&
               moq_session_state(sv) == MOQ_SESS_ESTABLISHED;
out:
    moq_simpair_destroy(sp);
    return admitted;
}

static bool all_zero(const void *storage, size_t size)
{
    const unsigned char *bytes = storage;
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != 0) return false;
    return true;
}

typedef struct setup_alloc_state {
    size_t calls;
    size_t fail_at;
    size_t live;
    moq_session_t *session;
    bool saw_pool_transaction;
} setup_alloc_state_t;

static void *setup_alloc(size_t size, void *ctx)
{
    setup_alloc_state_t *a = ctx;
    a->calls++;
    if (a->session && a->session->ns_subs[0].auth_txn.ov.count > 0)
        a->saw_pool_transaction = true;
    if (a->calls == a->fail_at) return NULL;
    void *p = malloc(size);
    if (p) a->live++;
    return p;
}

static void *setup_realloc(void *p, size_t old_size, size_t new_size, void *ctx)
{
    setup_alloc_state_t *a = ctx;
    (void)old_size;
    if (!p) return setup_alloc(new_size, ctx);
    if (new_size == 0) {
        free(p);
        a->live--;
        return NULL;
    }
    a->calls++;
    if (a->calls == a->fail_at) return NULL;
    void *copy = realloc(p, new_size);
    return copy;
}

static void setup_free(void *p, size_t size, void *ctx)
{
    setup_alloc_state_t *a = ctx;
    (void)size;
    if (p) a->live--;
    free(p);
}

static moq_session_t *setup_peer(moq_version_t version, const moq_alloc_t *alloc,
                                  uint32_t events, uint32_t scratch,
                                  uint64_t cache)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), alloc, MOQ_PERSPECTIVE_SERVER);
    cfg.version = version;
    cfg.max_namespace_subscriptions = 1;
    cfg.max_track_subscriptions = 1;
    cfg.max_events = events;
    cfg.output_scratch_size = scratch;
    cfg.send_auth_token_cache_size = true;
    cfg.auth_token_cache_size = cache;
    moq_session_t *s = NULL;
    if (moq_session_create(&cfg, 0, &s) != MOQ_OK) return NULL;
    if (test_bind_if_required(s) != MOQ_OK || moq_session_start(s, 0) != MOQ_OK) {
        moq_session_destroy(s);
        return NULL;
    }
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
    return s;
}

/* SETUP's token-option encoding is the same on drafts 18 and 22. */
static size_t setup_with_tokens(uint8_t *buf, size_t cap,
                                 const moq_d22_kvp_t *tokens, size_t count,
                                 bool implementation)
{
    moq_d22_kvp_t options[MOQ_DECODED_MAX_TOKENS + 1];
    if (count > MOQ_DECODED_MAX_TOKENS) return 0;
    if (count) memcpy(options, tokens, count * sizeof(*tokens));
    if (implementation) {
        memset(&options[count], 0, sizeof(options[count]));
        options[count].type = 7;
        options[count].bytes = MOQ_BYTES_LITERAL("remote");
        count++;
    }
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    if (moq_d22_encode_setup(&w, options, count) != MOQ_OK) return 0;
    return moq_buf_writer_offset(&w);
}

static uint64_t setup_close_code(moq_session_t *s)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION) code = a.u.close_session.code;
        moq_action_cleanup(&a);
    }
    return code;
}

static bool setup_event_token(moq_session_t *s, size_t count,
                               uint64_t type, const char *value)
{
    moq_event_t ev;
    if (moq_session_poll_events(s, &ev, 1) != 1) return false;
    bool ok = ev.kind == MOQ_EVENT_SETUP_COMPLETE &&
              ev.u.setup_complete.token_count == count;
    if (ok && count > 0) {
        const moq_resolved_token_t *t = ev.u.setup_complete.tokens;
        ok = t && t[0].token_type == type && bytes_equal(t[0].token_value, value);
    }
    moq_event_cleanup(&ev);
    return ok;
}

static void seed_setup_pool_members(moq_session_t *s)
{
    moq_ns_sub_entry_t *ns = &s->ns_subs[0];
    moq_track_sub_entry_t *tracks = &s->track_subs[0];
    for (size_t i = 0; i < MOQ_DECODED_MAX_TOKENS; i++)
        ns->resolved_tokens[i].token_type = 0x5a;
    ns->auth_txn.ov.count = 1;
    ns->auth_txn.ov.cache = &s->peer_token_cache;
    ns->auth_txn.ov.entries[0].kind = MOQ_OV_DELETE;
    ns->auth_txn.ov.entries[0].alias = 65;
    tracks->auth_txn = ns->auth_txn;
}

static bool setup_pool_members_clear(const moq_session_t *s)
{
    const moq_ns_sub_entry_t *ns = &s->ns_subs[0];
    return ns->token_count == 0 &&
           all_zero(ns->resolved_tokens, sizeof(ns->resolved_tokens)) &&
           all_zero(ns->token_staged, sizeof(ns->token_staged)) &&
           all_zero(&ns->auth_txn, sizeof(ns->auth_txn)) &&
           all_zero(&s->track_subs[0].auth_txn,
                    sizeof(s->track_subs[0].auth_txn));
}

static int test_setup_pool_ownership(void)
{
    int failures = 0;
    const size_t counts[] = { 0, 1, MOQ_DECODED_MAX_TOKENS };
    printf("SETUP layout: namespace=%zu/%zu track=%zu/%zu\n",
           sizeof(moq_ns_sub_entry_t), _Alignof(moq_ns_sub_entry_t),
           sizeof(moq_track_sub_entry_t), _Alignof(moq_track_sub_entry_t));
    for (size_t c = 0; c < 2 * (sizeof(counts) / sizeof(counts[0])); c++) {
        size_t count = counts[c % (sizeof(counts) / sizeof(counts[0]))];
        setup_alloc_state_t a = {0};
        moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                             .realloc = setup_realloc, .free = setup_free };
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), &alloc,
                                   MOQ_PERSPECTIVE_SERVER);
        cfg.version = c < 3 ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_22;
        cfg.max_namespace_subscriptions = 1;
        cfg.max_track_subscriptions = 1;
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        if (!s) continue;
        MOQ_TEST_CHECK(test_bind_if_required(s) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_start(s, 0) == MOQ_OK);
        MOQ_TEST_CHECK(s->ns_sub_cap == 1 && s->track_sub_cap == 1);
        s->ns_subs[0].generation = 91;
        s->track_subs[0].generation = 103;
        uint8_t *ns_recv = s->ns_subs[0].recv_buf;
        uint8_t *track_recv = s->track_subs[0].rs.recv_buf;
        size_t ns_cap = s->ns_subs[0].recv_cap;
        size_t track_cap = s->track_subs[0].rs.recv_cap;
        seed_setup_pool_members(s);
        size_t calls = a.calls;
        a.fail_at = calls + 1;

        uint8_t values[MOQ_DECODED_MAX_TOKENS][3];
        moq_d22_kvp_t options[MOQ_DECODED_MAX_TOKENS];
        memset(options, 0, sizeof(options));
        for (size_t i = 0; i < count; i++) {
            values[i][0] = MOQ_AUTH_OP_USE_VALUE;
            values[i][1] = 4;
            values[i][2] = (uint8_t)('a' + i);
            options[i].type = 3;
            options[i].bytes = (moq_bytes_t){ values[i], sizeof(values[i]) };
        }
        uint8_t message[256];
        moq_buf_writer_t w;
        moq_buf_writer_init(&w, message, sizeof(message));
        MOQ_TEST_CHECK(moq_d22_encode_setup(&w, options, count) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message,
                           moq_buf_writer_offset(&w), 1) == MOQ_OK);
        memset(message, 0, sizeof(message));
        MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK(a.calls == calls);
        MOQ_TEST_CHECK(setup_pool_members_clear(s));
        MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_FREE);
        MOQ_TEST_CHECK(s->track_subs[0].state == MOQ_TRACK_SUB_FREE);
        MOQ_TEST_CHECK(s->ns_subs[0].generation == 91);
        MOQ_TEST_CHECK(s->track_subs[0].generation == 103);
        MOQ_TEST_CHECK(s->ns_subs[0].recv_buf == ns_recv &&
                       s->ns_subs[0].recv_cap == ns_cap);
        MOQ_TEST_CHECK(s->track_subs[0].rs.recv_buf == track_recv &&
                       s->track_subs[0].rs.recv_cap == track_cap);
        moq_event_t ev = {0};
        int polled = moq_session_poll_events(s, &ev, 1);
        MOQ_TEST_CHECK(polled == 1);
        if (polled != 1) { moq_session_destroy(s); continue; }
        MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_SETUP_COMPLETE);
        MOQ_TEST_CHECK(ev.u.setup_complete.token_count == count);
        for (size_t i = 0; i < ev.u.setup_complete.token_count; i++) {
            const moq_resolved_token_t *t = &ev.u.setup_complete.tokens[i];
            MOQ_TEST_CHECK(t->token_type == 4 && t->token_value.len == 1);
            MOQ_TEST_CHECK(t->token_value.data != NULL);
            if (t->token_value.data)
                MOQ_TEST_CHECK(t->token_value.data[0] == (uint8_t)('a' + i));
            MOQ_TEST_CHECK((uintptr_t)t->token_value.data >= (uintptr_t)s->event_scratch &&
                           (uintptr_t)t->token_value.data < (uintptr_t)s->event_scratch +
                                                              s->event_scratch_len);
        }
        moq_event_cleanup(&ev);
        moq_session_destroy(s);
        MOQ_TEST_CHECK(a.live == 0);
    }
    return failures;
}

static int test_setup_failures_and_pressure(void)
{
    int failures = 0;
    const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    static const uint8_t alpha[] = { MOQ_AUTH_OP_REGISTER, 1, 4, 'a', 'l', 'p', 'h', 'a' };
    static const uint8_t bravo[] = { MOQ_AUTH_OP_REGISTER, 2, 5, 'b', 'r', 'a', 'v', 'o' };
    moq_d22_kvp_t options[] = {
        { .type = 3, .bytes = { alpha, sizeof(alpha) } },
        { .type = 3, .bytes = { bravo, sizeof(bravo) } }
    };
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        /* Failure at each REGISTER copy, then (draft 22) the implementation
         * copy. Empty public ingress retries the retained control bytes. */
        size_t fail_points = versions[v] == MOQ_VERSION_DRAFT_22 ? 3 : 2;
        for (size_t fail = 1; fail <= fail_points; fail++) {
            setup_alloc_state_t a = {0};
            moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                                 .realloc = setup_realloc, .free = setup_free };
            moq_session_t *s = setup_peer(versions[v], &alloc, 8, 2048, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            a.session = s;
            size_t live = a.live;
            size_t calls = a.calls;
            a.fail_at = calls + fail;
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), options, 2,
                                          versions[v] == MOQ_VERSION_DRAFT_22);
            MOQ_TEST_CHECK(n != 0);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_ERR_NOMEM);
            MOQ_TEST_CHECK(a.saw_pool_transaction && a.calls == calls + fail);
            MOQ_TEST_CHECK(a.live == live);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_SETUP_SENT && s->recv_len == n);
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 0);
            MOQ_TEST_CHECK(s->peer_implementation == NULL && s->event_scratch_len == 0);
            MOQ_TEST_CHECK(s->event_head == s->event_tail);
            MOQ_TEST_CHECK(setup_pool_members_clear(s));
            MOQ_TEST_CHECK(s->ns_subs[0].generation == 0 && s->track_subs[0].generation == 0);
            a.fail_at = 0;
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, NULL, 0, 2) == MOQ_OK);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED && s->recv_len == 0);
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 42);
            MOQ_TEST_CHECK(setup_pool_members_clear(s));
            memset(message, 0, sizeof(message));
            MOQ_TEST_CHECK(setup_event_token(s, 2, 4, "alpha"));
            if (versions[v] == MOQ_VERSION_DRAFT_22)
                MOQ_TEST_CHECK(bytes_equal(private_peer_implementation(s), "remote"));
            a.session = NULL;
            moq_session_destroy(s);
            MOQ_TEST_CHECK(a.live == 0);
        }

        /* A natural event-queue refusal precedes borrowing either pool. */
        {
            moq_session_t *s = setup_peer(versions[v], moq_alloc_default(), 1, 2048, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            moq_event_t blocker = {0};
            blocker.kind = MOQ_EVENT_GOAWAY;
            MOQ_TEST_CHECK(push_event(s, &blocker) == MOQ_OK);
            seed_setup_pool_members(s);
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), options, 1, false);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_ERR_WOULD_BLOCK);
            MOQ_TEST_CHECK(s->ns_subs[0].resolved_tokens[0].token_type == 0x5a);
            MOQ_TEST_CHECK(s->ns_subs[0].auth_txn.ov.count == 1);
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 0 && s->recv_len == n);
            moq_event_t ev;
            MOQ_TEST_CHECK(moq_session_poll_events(s, &ev, 1) == 1);
            moq_event_cleanup(&ev);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, NULL, 0, 2) == MOQ_OK);
            MOQ_TEST_CHECK(setup_pool_members_clear(s));
            MOQ_TEST_CHECK(setup_event_token(s, 1, 4, "alpha"));
            moq_session_destroy(s);
        }

        /* White-box transient arena pressure: normal pre-establishment
         * public ingress has an empty arena, so exercise the real profile
         * dispatcher directly while retaining its explicit consumed count. */
        {
            setup_alloc_state_t a = {0};
            moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                                 .realloc = setup_realloc, .free = setup_free };
            moq_session_t *s = setup_peer(versions[v], &alloc, 8, 64, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            size_t live = a.live;
            s->event_scratch_len = 60;
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), options, 1, false);
            size_t consumed = SIZE_MAX;
            MOQ_TEST_CHECK(s->profile->process_control_data(s, message, n,
                               &consumed) == MOQ_ERR_WOULD_BLOCK);
            MOQ_TEST_CHECK(consumed == 0 && s->event_scratch_len == 60);
            MOQ_TEST_CHECK(a.live == live && s->peer_token_cache.used_bytes == 0);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_SETUP_SENT && setup_pool_members_clear(s));
            s->event_scratch_len = 0;
            MOQ_TEST_CHECK(s->profile->process_control_data(s, message, n,
                               &consumed) == MOQ_OK);
            MOQ_TEST_CHECK(consumed == n && setup_pool_members_clear(s));
            MOQ_TEST_CHECK(setup_event_token(s, 1, 4, "alpha"));
            moq_session_destroy(s);
            MOQ_TEST_CHECK(a.live == 0);
        }

        /* An empty arena too small for the token event must close, abort the
         * REGISTER and leave both scratch owners clear. */
        {
            setup_alloc_state_t a = {0};
            moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                                 .realloc = setup_realloc, .free = setup_free };
            moq_session_t *s = setup_peer(versions[v], &alloc, 8, 16, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), options, 1, false);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_OK);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED && setup_close_code(s) == 0x1);
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 0 && setup_pool_members_clear(s));
            moq_session_destroy(s);
            MOQ_TEST_CHECK(a.live == 0);
        }

        /* Preserve cumulative REGISTER downgrade: one fits, the second is
         * USE_VALUE, but both exact resolved values reach event scratch. */
        {
            moq_session_t *s = setup_peer(versions[v], moq_alloc_default(), 8, 2048, 21);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), options, 2, false);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_OK);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED && setup_pool_members_clear(s));
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 21);
            MOQ_TEST_CHECK(moq_token_cache_lookup(&s->peer_token_cache, 1, NULL, NULL, NULL) == MOQ_TOKEN_OK);
            MOQ_TEST_CHECK(moq_token_cache_lookup(&s->peer_token_cache, 2, NULL, NULL, NULL) == MOQ_TOKEN_ERR_UNKNOWN);
            memset(message, 0, sizeof(message));
            moq_event_t ev;
            int polled = moq_session_poll_events(s, &ev, 1);
            MOQ_TEST_CHECK(polled == 1);
            if (polled == 1) {
                MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_SETUP_COMPLETE &&
                               ev.u.setup_complete.token_count == 2);
                if (ev.kind == MOQ_EVENT_SETUP_COMPLETE && ev.u.setup_complete.token_count == 2) {
                    const moq_resolved_token_t *t = ev.u.setup_complete.tokens;
                    MOQ_TEST_CHECK(t[0].token_type == 4 && bytes_equal(t[0].token_value, "alpha"));
                    MOQ_TEST_CHECK(t[1].token_type == 5 && bytes_equal(t[1].token_value, "bravo"));
                }
                moq_event_cleanup(&ev);
            }
            moq_session_destroy(s);
        }

        /* Token values are opaque (§8.9: "The contents and serialization
         * of this payload are defined by the Token Type"): a REGISTER whose
         * value is one 0x00 byte and a USE_VALUE with an empty value both
         * establish the session, the REGISTER commits, and SETUP_COMPLETE
         * carries both values byte for byte. */
        {
            static const uint8_t zero_value[] = { MOQ_AUTH_OP_REGISTER, 2, 4, 0 };
            static const uint8_t empty_value[] = { MOQ_AUTH_OP_USE_VALUE, 5 };
            moq_session_t *s = setup_peer(versions[v], moq_alloc_default(), 8, 2048, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            moq_d22_kvp_t binary[2] = { options[0], options[0] };
            binary[0].bytes = (moq_bytes_t){ zero_value, sizeof(zero_value) };
            binary[1].bytes = (moq_bytes_t){ empty_value, sizeof(empty_value) };
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), binary, 2, false);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_OK);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED && setup_pool_members_clear(s));
            MOQ_TEST_CHECK(moq_token_cache_lookup(&s->peer_token_cache, 2, NULL, NULL, NULL) == MOQ_TOKEN_OK);
            moq_event_t ev;
            int polled = moq_session_poll_events(s, &ev, 1);
            MOQ_TEST_CHECK(polled == 1);
            if (polled == 1) {
                MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_SETUP_COMPLETE &&
                               ev.u.setup_complete.token_count == 2);
                if (ev.kind == MOQ_EVENT_SETUP_COMPLETE && ev.u.setup_complete.token_count == 2) {
                    const moq_resolved_token_t *t = ev.u.setup_complete.tokens;
                    MOQ_TEST_CHECK(t[0].token_type == 4 && t[0].token_value.len == 1 &&
                                   t[0].token_value.data[0] == 0);
                    MOQ_TEST_CHECK(t[1].token_type == 5 && t[1].token_value.len == 0);
                }
                moq_event_cleanup(&ev);
            }
            moq_session_destroy(s);
        }

        /* Syntax and alias failures keep their existing close codes; pending
         * registrations are never accidentally committed. */
        static const uint8_t bad_op[] = { MOQ_AUTH_OP_USE_ALIAS, 1 };
        for (size_t kind = 0; kind < 3; kind++) {
            setup_alloc_state_t a = {0};
            moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                                 .realloc = setup_realloc, .free = setup_free };
            moq_session_t *s = setup_peer(versions[v], &alloc, 8, 2048, 256);
            MOQ_TEST_CHECK(s != NULL);
            if (!s) continue;
            moq_d22_kvp_t bad[2] = { options[0], options[0] };
            size_t count = 2;
            uint64_t code = 0x14;
            if (kind == 1) {
                bad[0].bytes = (moq_bytes_t){ bad_op, sizeof(bad_op) };
                count = 1;
                code = 0x3;
            } else if (kind == 2) {
                static const uint8_t duplicate[] = { MOQ_AUTH_OP_USE_VALUE, 4, 'a' };
                bad[0].bytes = bad[1].bytes = (moq_bytes_t){ duplicate, sizeof(duplicate) };
                code = 0x16;
            }
            uint8_t message[256];
            size_t n = setup_with_tokens(message, sizeof(message), bad, count, false);
            MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message, n, 1) == MOQ_OK);
            MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED && setup_close_code(s) == code);
            MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 0 && setup_pool_members_clear(s));
            moq_session_destroy(s);
            MOQ_TEST_CHECK(a.live == 0);
        }
    }
    return failures;
}

typedef struct setup_uri_probe {
    moq_session_t *session;
    uint32_t verdict;
    size_t calls;
    bool untouched;
} setup_uri_probe_t;

static uint32_t setup_uri_before_borrow(void *ctx, uint32_t present,
                                       moq_bytes_t authority, moq_bytes_t path)
{
    setup_uri_probe_t *probe = ctx;
    const moq_session_t *s = probe->session;
    probe->calls++;
    probe->untouched = present == MOQ_BINDING_TARGET_COMPLETE &&
        bytes_equal(authority, "test.invalid") && bytes_equal(path, "/live") &&
        s->ns_subs[0].state == MOQ_NS_SUB_FREE &&
        s->track_subs[0].state == MOQ_TRACK_SUB_FREE &&
        s->ns_subs[0].resolved_tokens[0].token_type == 0x5a &&
        s->ns_subs[0].auth_txn.ov.count == 1 &&
        s->track_subs[0].auth_txn.ov.count == 1;
    return probe->verdict;
}

static int test_setup_uri_borrow_order(void)
{
    int failures = 0;
    for (size_t reject = 0; reject < 2; reject++) {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_SERVER);
        cfg.version = MOQ_VERSION_DRAFT_22;
        cfg.max_namespace_subscriptions = cfg.max_track_subscriptions = 1;
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        if (!s) continue;
        setup_uri_probe_t probe = { .session = s,
            .verdict = reject ? MOQ_URI_INVALID_PATH : MOQ_URI_ACCEPT };
        moq_transport_binding_t binding;
        moq_transport_binding_init(&binding);
        binding.kind = MOQ_BINDING_RAW_QUIC;
        binding.facts_known = binding.facts_supported = MOQ_BINDING_FACTS_ALL;
        binding.accept_uri = setup_uri_before_borrow;
        binding.accept_uri_ctx = &probe;
        MOQ_TEST_CHECK(moq_session_bind_transport(s, &binding, sizeof(binding)) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_start(s, 0) == MOQ_OK);
        seed_setup_pool_members(s);
        static const uint8_t value[] = { MOQ_AUTH_OP_USE_VALUE, 4, 'a' };
        moq_d22_kvp_t options[] = {
            { .type = 1, .bytes = MOQ_BYTES_LITERAL("/live") },
            { .type = 3, .bytes = { value, sizeof(value) } },
            { .type = 5, .bytes = MOQ_BYTES_LITERAL("test.invalid") }
        };
        uint8_t message[128];
        moq_buf_writer_t w;
        moq_buf_writer_init(&w, message, sizeof(message));
        MOQ_TEST_CHECK(moq_d22_encode_setup(&w, options, 3) == MOQ_OK);
        MOQ_TEST_CHECK(moq_session_on_control_bytes(s, message,
                           moq_buf_writer_offset(&w), 1) == MOQ_OK);
        MOQ_TEST_CHECK(probe.calls == 1 && probe.untouched);
        if (reject) {
            MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED && setup_close_code(s) == 0x8);
            MOQ_TEST_CHECK(s->ns_subs[0].resolved_tokens[0].token_type == 0x5a);
            MOQ_TEST_CHECK(s->ns_subs[0].auth_txn.ov.count == 1);
        } else {
            MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED && setup_pool_members_clear(s));
            MOQ_TEST_CHECK(setup_event_token(s, 1, 4, "a"));
        }
        moq_session_destroy(s);
    }
    return failures;
}

static size_t setup_early_request(uint8_t *buf, size_t cap,
                                  moq_version_t version, bool tracks,
                                  uint64_t id, bool reuse)
{
    moq_bytes_t part = reuse ? MOQ_BYTES_LITERAL("next") : MOQ_BYTES_LITERAL("live");
    moq_namespace_t prefix = { &part, 1 };
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    if (version == MOQ_VERSION_DRAFT_18) {
        moq_d18_msg_params_t params = {0};
        params.auth_token_count = reuse ? 1 : 2;
        params.auth_tokens[0].alias_type = reuse ? MOQ_AUTH_OP_USE_VALUE : MOQ_AUTH_OP_USE_ALIAS;
        params.auth_tokens[0].alias = 1;
        params.auth_tokens[0].token_type = 4;
        params.auth_tokens[0].token_value = MOQ_BYTES_LITERAL("next");
        params.auth_tokens[1] = (moq_d18_auth_token_t){
            .alias_type = MOQ_AUTH_OP_REGISTER, .alias = 2, .token_type = 5,
            .token_value = MOQ_BYTES_LITERAL("bravo") };
        moq_result_t rc = tracks
            ? moq_d18_encode_subscribe_tracks(&w, id, &prefix, &params)
            : moq_d18_encode_subscribe_namespace(&w, id, &prefix, &params);
        return rc == MOQ_OK ? moq_buf_writer_offset(&w) : 0;
    }
    uint8_t ns_buf[16];
    moq_buf_writer_t nw;
    moq_buf_writer_init(&nw, ns_buf, sizeof(ns_buf));
    if (moq_d22_encode_namespace(&nw, &prefix) != MOQ_OK) return 0;
    moq_d22_message_t message = {0};
    message.type = tracks ? MOQ_D22_SUBSCRIBE_TRACKS : MOQ_D22_SUBSCRIBE_NAMESPACE;
    message.request_id = id;
    message.ns.encoded = (moq_bytes_t){ ns_buf, moq_buf_writer_offset(&nw) };
    message.ns.count = 1;
    moq_d22_param_t params[2] = {
        { .type = 3, .value.token = {
            .alias_type = reuse ? MOQ_AUTH_OP_USE_VALUE : MOQ_AUTH_OP_USE_ALIAS,
            .alias = 1, .type = 4, .value = MOQ_BYTES_LITERAL("next") } },
        { .type = 3, .value.token = { .alias_type = MOQ_AUTH_OP_REGISTER,
            .alias = 2, .type = 5, .value = MOQ_BYTES_LITERAL("bravo") } }
    };
    moq_d22_message_context_t context = {0};
    if (moq_d22_encode_message(&w, &message, params, reuse ? 1 : 2,
                               &context) != MOQ_OK) return 0;
    return moq_buf_writer_offset(&w);
}

static bool setup_request_event(moq_session_t *s, bool tracks, bool reuse,
                                 uint64_t *handle)
{
    moq_event_t ev;
    if (moq_session_poll_events(s, &ev, 1) != 1) return false;
    bool ok = ev.kind == (tracks ? MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST : MOQ_EVENT_NS_SUB_REQUEST);
    if (ok) {
        moq_namespace_t prefix = tracks ? ev.u.subscribe_tracks_request.track_namespace_prefix
                                        : ev.u.ns_sub_request.track_namespace_prefix;
        size_t count = tracks ? ev.u.subscribe_tracks_request.token_count
                               : ev.u.ns_sub_request.token_count;
        const moq_resolved_token_t *tokens = tracks ? ev.u.subscribe_tracks_request.tokens
                                                    : ev.u.ns_sub_request.tokens;
        *handle = tracks ? ev.u.subscribe_tracks_request.handle._opaque
                          : ev.u.ns_sub_request.handle._opaque;
        ok = prefix.count == 1 && prefix.parts &&
             bytes_equal(prefix.parts[0], reuse ? "next" : "live") &&
             count == (reuse ? 1u : 2u) && tokens &&
             tokens[0].token_type == 4 && bytes_equal(tokens[0].token_value, reuse ? "next" : "alpha");
        if (ok && !reuse)
            ok = tokens[1].token_type == 5 && bytes_equal(tokens[1].token_value, "bravo");
        if (ok) {
            for (size_t i = 0; i < count; i++)
                ok = ok && (uintptr_t)tokens[i].token_value.data >= (uintptr_t)s->event_scratch &&
                     (uintptr_t)tokens[i].token_value.data <
                         (uintptr_t)s->event_scratch + s->event_scratch_len;
        }
    }
    moq_event_cleanup(&ev);
    return ok;
}

static moq_result_t setup_reject_owner(moq_session_t *s, bool tracks, uint64_t handle)
{
    if (tracks) {
        moq_reject_subscribe_tracks_cfg_t cfg;
        moq_reject_subscribe_tracks_cfg_init(&cfg);
        return moq_session_reject_subscribe_tracks(s, (moq_track_sub_handle_t){ handle }, &cfg, 5);
    }
    moq_reject_ns_sub_cfg_t cfg;
    moq_reject_ns_sub_cfg_init(&cfg);
    return moq_session_reject_ns_sub(s, (moq_ns_sub_handle_t){ handle }, &cfg, 5);
}

static int test_setup_early_owner_reuse(void)
{
    int failures = 0;
    size_t cases = 0;
    const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t v = 0; v < 2; v++) {
        for (size_t tracks = 0; tracks < 2; tracks++) {
            for (size_t pressure = 0; pressure < 2; pressure++) {
                for (size_t fin = 0; fin < 2; fin++) {
                    setup_alloc_state_t a = {0};
                    moq_alloc_t alloc = { .ctx = &a, .alloc = setup_alloc,
                                         .realloc = setup_realloc, .free = setup_free };
                    moq_session_t *s = setup_peer(versions[v], &alloc,
                                                  pressure ? 1 : 8, 2048, 256);
                    MOQ_TEST_CHECK(s != NULL);
                    if (!s) continue;
                    cases++;
                    moq_subscribe_namespace_cfg_t ns_cfg;
                    moq_subscribe_namespace_cfg_init(&ns_cfg);
                    moq_ns_sub_handle_t ns_handle = {0};
                    MOQ_TEST_CHECK(moq_session_subscribe_namespace(s, &ns_cfg, 0,
                                       &ns_handle) == MOQ_ERR_WRONG_STATE);
                    moq_subscribe_tracks_cfg_t track_cfg;
                    moq_subscribe_tracks_cfg_init(&track_cfg);
                    moq_track_sub_handle_t track_handle = {0};
                    MOQ_TEST_CHECK(moq_session_subscribe_tracks(s, &track_cfg, 0,
                                       &track_handle) == MOQ_ERR_WRONG_STATE);
                    MOQ_TEST_CHECK(s->ns_sub_cap == 1 && s->track_sub_cap == 1);
                    uint8_t *ns_recv = s->ns_subs[0].recv_buf;
                    uint8_t *track_recv = s->track_subs[0].rs.recv_buf;
                    uint8_t request[128];
                    size_t n = setup_early_request(request, sizeof(request), versions[v],
                                                   tracks != 0, 0, false);
                    MOQ_TEST_CHECK(n > 0);
                    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x4000);
                    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, request,
                                       n, fin != 0, 1) == MOQ_OK);
                    memset(request, 0, sizeof(request));
                    moq_request_endpoint_t carrier = request_registry_find_by_streamref(s, ref);
                    MOQ_TEST_CHECK(carrier.kind == MOQ_REQ_SUBSCRIPTION && carrier.slot >= 0);
                    if (carrier.kind != MOQ_REQ_SUBSCRIPTION || carrier.slot < 0 ||
                        (size_t)carrier.slot >= s->sub_cap) {
                        moq_session_destroy(s);
                        MOQ_TEST_CHECK(a.live == 0);
                        continue;
                    }
                    if (carrier.slot >= 0) {
                        const moq_sub_entry_t *sub = &s->subs[carrier.slot];
                        MOQ_TEST_CHECK(sub->state == MOQ_SUB_RECVING_REQUEST &&
                                       sub->rs.recv_len == n && sub->rs.recv_fin == (fin != 0));
                    }
                    MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_FREE &&
                                   s->track_subs[0].state == MOQ_TRACK_SUB_FREE);
                    static const uint8_t alpha[] = {
                        MOQ_AUTH_OP_REGISTER, 1, 4, 'a', 'l', 'p', 'h', 'a' };
                    moq_d22_kvp_t option = { .type = 3, .bytes = { alpha, sizeof(alpha) } };
                    uint8_t setup[128];
                    size_t sn = setup_with_tokens(setup, sizeof(setup), &option, 1, false);
                    MOQ_TEST_CHECK(moq_session_on_control_bytes(s, setup, sn, 2) == MOQ_OK);
                    memset(setup, 0, sizeof(setup));
                    MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED);
                    /* Inspect the actual new owner BEFORE polling or any advancing
                     * call. Caller-after-completion cleanup would erase this auth. */
                    if (!tracks && pressure) {
                        const moq_ns_sub_entry_t *ns = &s->ns_subs[0];
                        MOQ_TEST_CHECK(ns->state == MOQ_NS_SUB_RECVING_PUBLISHER &&
                                       ns->parse_complete && ns->auth_processed);
                        MOQ_TEST_CHECK(ns->token_count == 2 && ns->token_staged[0] &&
                                       !ns->token_staged[1]);
                        MOQ_TEST_CHECK(ns->auth_txn.ov.count == 1 &&
                                       ns->auth_txn.ov.entries[0].preowned != NULL);
                        MOQ_TEST_CHECK(ns->stream_ref._v == ref._v &&
                                       ns->handoff_fin_pending == (fin != 0));
                        MOQ_TEST_CHECK(bytes_equal(ns->resolved_tokens[0].token_value, "alpha") &&
                                       bytes_equal(ns->resolved_tokens[1].token_value, "bravo"));
                        MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 21);
                    } else if (tracks && pressure) {
                        MOQ_TEST_CHECK(s->track_subs[0].state == MOQ_TRACK_SUB_FREE);
                        MOQ_TEST_CHECK(s->subs[carrier.slot].state == MOQ_SUB_RECVING_REQUEST &&
                                       s->subs[carrier.slot].rs.recv_fin == (fin != 0));
                        MOQ_TEST_CHECK(setup_pool_members_clear(s));
                    } else if (!(fin && v == 0)) {
                        if (tracks) {
                            MOQ_TEST_CHECK(s->track_subs[0].state == MOQ_TRACK_SUB_PENDING_PUBLISHER &&
                                           s->track_subs[0].rs.ref._v == ref._v &&
                                           track_sub_peer_fin_observed(&s->track_subs[0]) == (fin != 0));
                        } else {
                            MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_PENDING_PUBLISHER &&
                                           s->ns_subs[0].stream_ref._v == ref._v &&
                                           s->ns_subs[0].token_count == 2 &&
                                           s->ns_subs[0].auth_committed &&
                                           ns_sub_fin_observed(&s->ns_subs[0], false) == (fin != 0));
                        }
                        MOQ_TEST_CHECK(s->peer_token_cache.used_bytes == 42);
                    }
                    MOQ_TEST_CHECK(setup_event_token(s, 1, 4, "alpha"));
                    if (pressure && !tracks) {
                        /* Namespace ownership has left the subscription carrier.
                         * The documented empty-bidi retry resumes that new owner;
                         * draining SETUP alone currently does not sweep ns_subs. */
                        MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_RECVING_PUBLISHER &&
                                       s->ns_subs[0].token_count == 2 &&
                                       s->ns_subs[0].handoff_fin_pending == (fin != 0));
                        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, NULL, 0,
                                           false, 3) == MOQ_OK);
                    }
                    if (pressure && tracks) {
                        /* Event-drain refeed commits the carrier's track owner. */
                        MOQ_TEST_CHECK(s->track_subs[0].state == MOQ_TRACK_SUB_PENDING_PUBLISHER &&
                                       s->track_subs[0].rs.ref._v == ref._v &&
                                       track_sub_peer_fin_observed(&s->track_subs[0]) == (fin != 0));
                    }
                    uint64_t handle = 0;
                    MOQ_TEST_CHECK(setup_request_event(s, tracks != 0, false, &handle));
                    MOQ_TEST_CHECK(handle != 0 && s->peer_token_cache.used_bytes == 42);
                    if (fin && v == 0 && tracks) {
                        /* Draft-18 FIN cancels. A one-event queue retains the
                         * handed FIN until this documented empty retry. */
                        if (pressure)
                            MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, NULL,
                                               0, false, 4) == MOQ_OK);
                        moq_event_t cancelled;
                        int polled = moq_session_poll_events(s, &cancelled, 1);
                        MOQ_TEST_CHECK(polled == 1);
                        if (polled == 1) {
                            MOQ_TEST_CHECK(cancelled.kind == MOQ_EVENT_SUBSCRIBE_TRACKS_CANCELLED &&
                                           cancelled.u.subscribe_tracks_cancelled.handle._opaque == handle);
                            moq_event_cleanup(&cancelled);
                        }
                    } else if (!(fin && v == 0)) {
                        MOQ_TEST_CHECK(setup_reject_owner(s, tracks != 0, handle) == MOQ_OK);
                    }
                    MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_FREE &&
                                   s->track_subs[0].state == MOQ_TRACK_SUB_FREE);
                    MOQ_TEST_CHECK(s->ns_subs[0].recv_buf == ns_recv &&
                                   s->track_subs[0].rs.recv_buf == track_recv);
                    uint32_t generation = tracks ? s->track_subs[0].generation
                                                  : s->ns_subs[0].generation;
                    MOQ_TEST_CHECK(generation == 2);
                    moq_action_t action;
                    while (moq_session_poll_actions(s, &action, 1) > 0) {
                        MOQ_TEST_CHECK(action.kind != MOQ_ACTION_CLOSE_SESSION &&
                                       action.kind != MOQ_ACTION_ABORT_BIDI_STREAM);
                        moq_action_cleanup(&action);
                    }
                    moq_stream_ref_t next_ref = moq_stream_ref_from_u64(0x4004);
                    n = setup_early_request(request, sizeof(request), versions[v], tracks != 0, 2, true);
                    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, next_ref, request,
                                       n, false, 6) == MOQ_OK);
                    memset(request, 0, sizeof(request));
                    if (tracks) {
                        MOQ_TEST_CHECK(s->track_subs[0].state == MOQ_TRACK_SUB_PENDING_PUBLISHER &&
                                       s->track_subs[0].generation == 3 &&
                                       s->track_subs[0].rs.ref._v == next_ref._v &&
                                       !track_sub_peer_fin_observed(&s->track_subs[0]));
                        moq_request_endpoint_t owner = request_registry_find_by_streamref(s, next_ref);
                        MOQ_TEST_CHECK(owner.kind == MOQ_REQ_SUBSCRIBE_TRACKS && owner.slot == 0);
                    } else {
                        MOQ_TEST_CHECK(s->ns_subs[0].state == MOQ_NS_SUB_PENDING_PUBLISHER &&
                                       s->ns_subs[0].generation == 3 &&
                                       s->ns_subs[0].stream_ref._v == next_ref._v &&
                                       !ns_sub_fin_observed(&s->ns_subs[0], false) &&
                                       s->ns_subs[0].token_count == 1);
                        MOQ_TEST_CHECK(moq_index_find(s->idx_ns_by_ref, s->idx_ns_mask, next_ref._v) == 0);
                    }
                    uint64_t next_handle = 0;
                    MOQ_TEST_CHECK(setup_request_event(s, tracks != 0, true, &next_handle));
                    MOQ_TEST_CHECK(next_handle != 0 && next_handle != handle);
                    MOQ_TEST_CHECK(setup_reject_owner(s, tracks != 0, next_handle) == MOQ_OK);
                    /* Normal namespace free is selective, not SETUP release:
                     * old non-owning spans may remain, but no staging survives. */
                    MOQ_TEST_CHECK(s->ns_subs[0].token_count == 0 &&
                                   all_zero(s->ns_subs[0].token_staged,
                                            sizeof(s->ns_subs[0].token_staged)));
                    for (size_t i = 0; i < s->ns_subs[0].auth_txn.ov.count; i++)
                        MOQ_TEST_CHECK(s->ns_subs[0].auth_txn.ov.entries[i].preowned == NULL);
                    MOQ_TEST_CHECK(all_zero(&s->track_subs[0].auth_txn,
                                            sizeof(s->track_subs[0].auth_txn)));
                    MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED);
                    moq_session_destroy(s);
                    MOQ_TEST_CHECK(a.live == 0);
                }
            }
        }
    }
    MOQ_TEST_CHECK(cases == 16);
    printf("SETUP early-owner cases: %zu\n", cases);
    return failures;
}

int main(void)
{
    int failures = 0;

    failures += test_setup_pool_ownership();
    failures += test_setup_failures_and_pressure();
    failures += test_setup_uri_borrow_order();
    failures += test_setup_early_owner_reuse();

    /* == Registry: present, consistent, offered ======================== */
    {
        const moq_profile_ops_t *p = moq_profile_lookup(MOQ_VERSION_DRAFT_22);
        MOQ_TEST_CHECK(p != NULL);
        if (p) {
            MOQ_TEST_CHECK_EQ_INT((int)p->version, (int)MOQ_VERSION_DRAFT_22);
            MOQ_TEST_CHECK(p->wire_ready);
            MOQ_TEST_CHECK(p->uses_request_streams && p->uses_uni_control_channel);
            MOQ_TEST_CHECK(p->request_fin_is_not_cancel && p->premature_fin_fails_request);
            /* Concurrent subscriptions to one track (§3.1); drafts 16 and
             * 18 allow one per role. */
            MOQ_TEST_CHECK(p->allows_concurrent_subscriptions);
            MOQ_TEST_CHECK(!moq_profile_lookup(MOQ_VERSION_DRAFT_18)->allows_concurrent_subscriptions);
            MOQ_TEST_CHECK(!moq_profile_lookup(MOQ_VERSION_DRAFT_16)->allows_concurrent_subscriptions);
            /* Fill fetch streams are served; the publisher's unknown stream
             * count is the full-width maximum on this wire. */
            MOQ_TEST_CHECK(p->supports_fill);
            MOQ_TEST_CHECK(p->stream_count_unknown == UINT64_MAX);
            MOQ_TEST_CHECK(p->redirect_on_any_request);
            /* One unanswered update per request stream; one error may answer
             * a coalesced run. Draft-18 advertises no bound; draft-16 has no
             * coalesced error. */
            MOQ_TEST_CHECK_EQ_U64(p->request_updates_max, 1);
            MOQ_TEST_CHECK_EQ_U64(p->request_updates_exceeded_error, 0x1b);
            MOQ_TEST_CHECK(p->request_update_errors_coalesce);
            MOQ_TEST_CHECK_EQ_U64(moq_profile_lookup(MOQ_VERSION_DRAFT_18)->request_updates_max, 0);
            MOQ_TEST_CHECK(moq_profile_lookup(MOQ_VERSION_DRAFT_18)->request_update_errors_coalesce);
            MOQ_TEST_CHECK_EQ_U64(moq_profile_lookup(MOQ_VERSION_DRAFT_16)->request_updates_max, 0);
            MOQ_TEST_CHECK(!moq_profile_lookup(MOQ_VERSION_DRAFT_16)->request_update_errors_coalesce);
            MOQ_TEST_CHECK(!p->supports_joining_fetch && !p->publish_ok_carries_params);
            MOQ_TEST_CHECK(!p->publish_done_subscription_ended && p->subgroup_timer_at_fin);
            MOQ_TEST_CHECK(p->request_codec != NULL);
            /* Data-plane framing is draft-18's. */
            const moq_profile_ops_t *d18 = moq_profile_lookup(MOQ_VERSION_DRAFT_18);
            MOQ_TEST_CHECK(p->decode_subgroup_header == d18->decode_subgroup_header);
            MOQ_TEST_CHECK(p->decode_object_datagram == d18->decode_object_datagram);
            /* The fetch object decoder adds the End of Timed-Out Range marker
             * in front of draft-18's framing. */
            MOQ_TEST_CHECK(p->decode_fetch_object != d18->decode_fetch_object);
            MOQ_TEST_CHECK(p->encode_fetch_range != d18->encode_fetch_range);
            MOQ_TEST_CHECK(p->classify_uni_stream == d18->classify_uni_stream);
        }
        /* The request-error registry is draft-22's own (§16.11.2): every
         * registered code is surfaced as sent, and a code the draft does not
         * register -- including draft-18's DUPLICATE_SUBSCRIPTION and
         * INVALID_JOINING_REQUEST_ID -- is INTERNAL_ERROR (§13). */
        if (p) {
            static const uint64_t registered[] = {
                0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x9, 0x10, 0x11, 0x12,
                0x20, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36,
            };
            MOQ_TEST_CHECK_EQ_SIZE(sizeof(registered) / sizeof(*registered), 18);
            for (size_t i = 0; i < sizeof(registered) / sizeof(*registered); i++)
                MOQ_TEST_CHECK_EQ_U64((uint64_t)p->semantic_request_error(registered[i]),
                                      registered[i]);
            MOQ_TEST_CHECK_EQ_U64(MOQ_REQUEST_ERROR_CONFLICTING_FILTERS, 0x35);
            static const uint64_t unknown[] = {
                0x19, 0x32, 0x7, 0x13, 0x37, 0x7f + 0x9d, UINT64_MAX,
            };
            for (size_t i = 0; i < sizeof(unknown) / sizeof(*unknown); i++)
                MOQ_TEST_CHECK_EQ_U64((uint64_t)p->semantic_request_error(unknown[i]),
                                      MOQ_REQUEST_ERROR_INTERNAL_ERROR);
            /* The profile's refusal code for a second subscription (never
             * sent on this wire) is still one the draft registers; draft-18
             * keeps its own registered code. */
            MOQ_TEST_CHECK_EQ_U64(p->duplicate_subscription_error,
                                  MOQ_REQUEST_ERROR_NOT_SUPPORTED);
            MOQ_TEST_CHECK_EQ_U64((uint64_t)p->semantic_request_error(
                                      p->duplicate_subscription_error),
                                  p->duplicate_subscription_error);
            const moq_profile_ops_t *d18 = moq_profile_lookup(MOQ_VERSION_DRAFT_18);
            MOQ_TEST_CHECK_EQ_U64(d18->duplicate_subscription_error,
                                  MOQ_REQUEST_ERROR_DUPLICATE_SUBSCRIPTION);
            MOQ_TEST_CHECK_EQ_U64((uint64_t)d18->semantic_request_error(0x19), 0x19);
            MOQ_TEST_CHECK_EQ_U64((uint64_t)d18->semantic_request_error(0x36),
                                  MOQ_REQUEST_ERROR_INTERNAL_ERROR);
        }
        MOQ_TEST_CHECK(moq_version_supported(MOQ_VERSION_DRAFT_22));
        /* Newest first: the registry's order places it ahead of every other
         * offered version. */
        moq_version_t vs[8];
        size_t n = moq_versions_supported(vs, 8);
        MOQ_TEST_CHECK(n >= 1 && vs[0] == MOQ_VERSION_DRAFT_22);
        /* SETUP on this wire depends on how the session is carried, so the
         * profile requires a transport binding before it starts. */
        MOQ_TEST_CHECK(moq_version_binding_requirements(MOQ_VERSION_DRAFT_22) == MOQ_BINDING_FACTS_ALL);
    }

    /* == A second subscription to a track: admitted, the session stays up */
    MOQ_TEST_CHECK(second_subscribe_admitted());

    /* == Handshake over the simulator =================================== */
    {
        moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
        scfg.alloc = moq_alloc_default();
        scfg.version = MOQ_VERSION_DRAFT_22;
        moq_simpair_t *sp = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_simpair_create(&scfg, &sp), (int)MOQ_OK);
        moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_version(c), (int)MOQ_VERSION_DRAFT_22);
        MOQ_TEST_CHECK_EQ_INT((int)moq_simpair_start(sp), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_SETUP_COMPLETE), 1);
        MOQ_TEST_CHECK_EQ_INT(count_kind(sv, MOQ_EVENT_SETUP_COMPLETE), 1);
        /* Local privacy policy omits optional implementation identification. */
        MOQ_TEST_CHECK(!c->peer_setup.has_implementation);
        MOQ_TEST_CHECK(!sv->peer_setup.has_implementation);
        /* The peer's limits were recorded. */
        /* GOAWAY from the server drains the client. */
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_goaway(sv, NULL, 0, moq_simpair_now_us(sp)), (int)MOQ_OK);
        moq_simpair_run_until_quiescent(sp, 16, NULL);
        MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_GOAWAY), 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_DRAINING);
        moq_simpair_destroy(sp);
    }

    /* == Local identification is omitted, with full and old-sized configs. */
    for (int old = 0; old < 2; old++) {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_CLIENT);
        cfg.version = MOQ_VERSION_DRAFT_22;
        if (old)
            cfg.struct_size = (uint32_t)(offsetof(moq_session_cfg_t, version) + sizeof(cfg.version));
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_create(&cfg, 0, &s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)test_bind_if_required(s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(s, 1), (int)MOQ_OK);
        moq_d22_setup_options_t opts;
        MOQ_TEST_CHECK(decode_queued_setup(s, &opts));
        MOQ_TEST_CHECK(!opts.has_implementation);
        /* The native-QUIC client still names its target around it. */
        MOQ_TEST_CHECK(opts.has_path && opts.has_authority);
        /* The bound on unanswered updates the session enforces is the one
         * it advertises. */
        MOQ_TEST_CHECK(opts.has_max_request_updates);
        MOQ_TEST_CHECK_EQ_U64(opts.max_request_updates, s->profile->request_updates_max);
        /* Nothing received yet: the private peer snapshot is empty. */
        MOQ_TEST_CHECK(private_peer_implementation(s).len == 0 &&
                       private_peer_implementation(s).data == NULL);
        moq_session_destroy(s);
    }

    /* == A peer may omit the identifier; a server omits its local identity. */
    {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_SERVER);
        cfg.version = MOQ_VERSION_DRAFT_22;
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_create(&cfg, 0, &s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)test_bind_if_required(s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(s, 1), (int)MOQ_OK);
        moq_d22_setup_options_t opts;
        MOQ_TEST_CHECK(decode_queued_setup(s, &opts));
        MOQ_TEST_CHECK(!opts.has_implementation);
        /* A bare SETUP (no options) from the peer. */
        static const uint8_t setup[] = { 0xaf, 0x00, 0x00, 0x00 };
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_control_bytes(s, setup, sizeof(setup), 2), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(s), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK(private_peer_implementation(s).len == 0);
        moq_session_destroy(s);
    }

    /* == A duplicate SETUP is a protocol violation ===================== */
    {
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_SERVER);
        cfg.version = MOQ_VERSION_DRAFT_22;
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_create(&cfg, 0, &s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)test_bind_if_required(s), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(s, 1), (int)MOQ_OK);
        static const uint8_t setup[] = { 0xaf, 0x00, 0x00, 0x00 };
        moq_stream_ref_t ref = moq_stream_ref_from_u64(7);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_control_bytes(s, setup, sizeof(setup), 2), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(s), (int)MOQ_SESS_ESTABLISHED);
        (void)moq_session_on_control_bytes(s, setup, sizeof(setup), 3);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(s), (int)MOQ_SESS_CLOSED);
        (void)ref;
        moq_session_destroy(s);
    }

    MOQ_TEST_PASS("d22_profile");
    return failures != 0;
}
