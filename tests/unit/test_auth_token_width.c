/*
 * Authorization Token Type width follows each draft's integer encoding:
 * draft-16 carries it as a QUIC varint (§9.2.2.1, at most 2^62-1); draft-18
 * (§10.2.2) and draft-22 (§8.9) carry it as vi64, the full uint64 range.
 * Outgoing tokens are checked against the session's own profile: a type the
 * version cannot carry is refused before any effect (no action, no Request
 * ID), and every type it can carry reaches the peer exactly. SETUP tokens,
 * an ordinary request and the shape checks every profile keeps are covered;
 * draft-22 namespace updates are covered in test_d22_namespace_updates.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "test_binding_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/request_stream_internal.h"

static int failures = 0;

#define TYPE_2P62   (UINT64_C(1) << 62)
#define TYPE_D16MAX (TYPE_2P62 - 1)

static const char *vname(moq_version_t v)
{
    return v == MOQ_VERSION_DRAFT_16 ? "d16" : v == MOQ_VERSION_DRAFT_18 ? "d18" : "d22";
}

static bool contains(const uint8_t *hay, size_t n, const uint8_t *needle, size_t m)
{
    for (size_t i = 0; m <= n && i <= n - m; i++)
        if (memcmp(hay + i, needle, m) == 0) return true;
    return false;
}

/* The client's setup bytes, whichever channel the profile opens. */
static bool setup_bytes(const moq_action_t *a, const uint8_t **d, size_t *n)
{
    if (a->kind == MOQ_ACTION_SEND_CONTROL) {
        *d = a->u.send_control.data; *n = a->u.send_control.len; return true;
    }
    if (a->kind == MOQ_ACTION_OPEN_UNI_CONTROL) {
        *d = a->u.open_uni_control.data; *n = a->u.open_uni_control.len; return true;
    }
    return false;
}

static moq_session_t *make(moq_version_t v, moq_perspective_t role,
                           const moq_auth_token_t *tokens, size_t count,
                           moq_result_t *rc_out)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), role);
    cfg.version = v;
    cfg.setup_auth_tokens = tokens;
    cfg.setup_auth_token_count = count;
    moq_session_t *s = NULL;
    *rc_out = moq_session_create(&cfg, 0, &s);
    return s;
}

/* SETUP: a client presenting `type` either is refused at create, or sends it
 * (literal bytes `wire` appear in its setup) and the server's SETUP_COMPLETE
 * reports exactly that type. */
static void setup_row(moq_version_t v, uint64_t type, bool accepted,
                      const uint8_t *wire, size_t wire_len)
{
    static const uint8_t value[] = { 'v' };
    moq_auth_token_t tok = { .token_type = type, .token_value = { value, 1 } };
    moq_result_t rc;
    moq_session_t *c = make(v, MOQ_PERSPECTIVE_CLIENT, &tok, 1, &rc);
    if (!accepted) {
        if (rc != MOQ_ERR_INVAL || c)
            fprintf(stderr, "setup %s type %llu: expected refusal\n", vname(v),
                    (unsigned long long)type);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INVAL);
        MOQ_TEST_CHECK(c == NULL);
        moq_session_destroy(c);
        return;
    }
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    moq_session_t *s = make(v, MOQ_PERSPECTIVE_SERVER, NULL, 0, &rc);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (!c || !s) { moq_session_destroy(c); moq_session_destroy(s); return; }
    MOQ_TEST_CHECK_EQ_INT((int)test_bind_if_required(c), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)test_bind_if_required(s), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(c, 0), (int)MOQ_OK);
    if (v != MOQ_VERSION_DRAFT_16) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(s, 0), (int)MOQ_OK);
        moq_action_t a;
        while (moq_session_poll_actions(s, &a, 1)) moq_action_cleanup(&a);
    }
    moq_action_t a;
    const uint8_t *d = NULL;
    size_t n = 0;
    MOQ_TEST_CHECK(moq_session_poll_actions(c, &a, 1) == 1 && setup_bytes(&a, &d, &n));
    MOQ_TEST_CHECK(d && contains(d, n, wire, wire_len));
    if (d) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_control_bytes(s, d, n, 1), (int)MOQ_OK);
        bool seen = false;
        moq_event_t ev;
        while (moq_session_poll_events(s, &ev, 1)) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) {
                seen = true;
                MOQ_TEST_CHECK_EQ_SIZE(ev.u.setup_complete.token_count, 1);
                if (ev.u.setup_complete.token_count == 1)
                    MOQ_TEST_CHECK_EQ_U64(ev.u.setup_complete.tokens[0].token_type, type);
            }
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK(seen);
    }
    moq_action_cleanup(&a);
    moq_session_destroy(c);
    moq_session_destroy(s);
}

static void test_setup(void)
{
    /* Literal token structures (Alias Type USE_VALUE 3, Token Type, value
     * 'v') behind their option/parameter type and length. */
    static const uint8_t d16_max[] = {          /* QUIC varint 2^62-1 */
        0x03, 0x0a, 0x03, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 'v' };
    static const uint8_t vi64_2p62[] = {        /* vi64 9-byte form */
        0x0b, 0x03, 0xff, 0x40,0,0,0,0,0,0,0, 'v' };
    static const uint8_t vi64_max[] = {
        0x0b, 0x03, 0xff, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 'v' };
    /* The whole draft-18 SETUP: type 0x2F00, Length 13, one token option. */
    static const uint8_t d18_full[] = {
        0xaf, 0x00, 0x00, 0x0d, 0x03, 0x0b, 0x03,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 'v' };
    /* The whole draft-16 CLIENT_SETUP: type 0x20, Length 13, one parameter. */
    static const uint8_t d16_full[] = {
        0x20, 0x00, 0x0d, 0x01, 0x03, 0x0a, 0x03,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 'v' };

    setup_row(MOQ_VERSION_DRAFT_16, TYPE_D16MAX, true, d16_max, sizeof(d16_max));
    setup_row(MOQ_VERSION_DRAFT_16, TYPE_D16MAX, true, d16_full, sizeof(d16_full));
    setup_row(MOQ_VERSION_DRAFT_16, TYPE_2P62, false, NULL, 0);
    setup_row(MOQ_VERSION_DRAFT_16, UINT64_MAX, false, NULL, 0);
    for (int i = 0; i < 2; i++) {
        moq_version_t v = i ? MOQ_VERSION_DRAFT_22 : MOQ_VERSION_DRAFT_18;
        setup_row(v, TYPE_2P62, true, vi64_2p62, sizeof(vi64_2p62));
        setup_row(v, UINT64_MAX, true, vi64_max, sizeof(vi64_max));
    }
    setup_row(MOQ_VERSION_DRAFT_18, UINT64_MAX, true, d18_full, sizeof(d18_full));

    /* Shape refusals hold on every profile. */
    const moq_version_t all[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18,
                                  MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 3; i++) {
        moq_result_t rc;
        moq_auth_token_t bad = { .token_type = 1, .token_value = { NULL, 3 } };
        moq_session_t *s = make(all[i], MOQ_PERSPECTIVE_CLIENT, &bad, 1, &rc);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INVAL);
        moq_session_destroy(s);
        s = make(all[i], MOQ_PERSPECTIVE_CLIENT, NULL, 1, &rc);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INVAL);
        moq_session_destroy(s);
    }
    MOQ_TEST_PASS("auth_token_width setup");
}

/* -- An ordinary request (SUBSCRIBE) -------------------------------- */

static moq_simpair_t *pair(moq_version_t v)
{
    static moq_alloc_t alloc;
    alloc = *moq_alloc_default();
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &alloc;
    cfg.seed = 7;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 16;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 16;
    cfg.version = v;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || !sp) return NULL;
    if (moq_simpair_start(sp) != MOQ_OK) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 8, NULL);
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_client(sp), &ev, 1)) moq_event_cleanup(&ev);
    while (moq_session_poll_events(moq_simpair_server(sp), &ev, 1)) moq_event_cleanup(&ev);
    return sp;
}

static moq_result_t subscribe(moq_simpair_t *sp, const moq_auth_token_t *tok,
                              size_t count)
{
    /* Each request names its own track: a second subscription to one track
     * is refused for its own reason. */
    static const moq_bytes_t parts[] = { { (const uint8_t *)"ns", 2 } };
    static const char names[] = "abcdefghij";
    static unsigned next;
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ (moq_bytes_t *)parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)&names[next++ % 10], 1 };
    cfg.auth_tokens = tok;
    cfg.auth_token_count = count;
    moq_subscription_t sub;
    return moq_session_subscribe(moq_simpair_client(sp), &cfg,
                                 moq_simpair_now_us(sp), &sub);
}

/* The next Request ID this client would use, where the profile keeps it in
 * the shared request-stream state (draft-18 and draft-22). */
static bool next_id(moq_session_t *s, uint64_t *out)
{
    if (moq_session_version(s) == MOQ_VERSION_DRAFT_16) return false;
    *out = ((const moq_request_stream_state_t *)s->profile_state)->next_local_request_id;
    return true;
}

/* A refused SUBSCRIBE leaves nothing behind: no action, the same next
 * Request ID, the session established. */
static void refused_subscribe(moq_simpair_t *sp, const moq_auth_token_t *tok)
{
    moq_session_t *c = moq_simpair_client(sp);
    uint64_t before = 0, after = 0;
    bool ids = next_id(c, &before);
    MOQ_TEST_CHECK_EQ_INT((int)subscribe(sp, tok, 1), (int)MOQ_ERR_INVAL);
    moq_action_t a;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(c, &a, 1), 0);
    if (ids) {
        MOQ_TEST_CHECK(next_id(c, &after));
        MOQ_TEST_CHECK_EQ_U64(after, before);
    }
    MOQ_TEST_CHECK(moq_session_state(c) == MOQ_SESS_ESTABLISHED);
}

/* A SUBSCRIBE carrying `type` reaches the server with exactly that type. */
static void accepted_subscribe(moq_simpair_t *sp, uint64_t type)
{
    static const uint8_t value[] = { 'v' };
    moq_auth_token_t tok = { .token_type = type, .token_value = { value, 1 } };
    MOQ_TEST_CHECK_EQ_INT((int)subscribe(sp, &tok, 1), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 8, NULL);
    int seen = 0;
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_server(sp), &ev, 1)) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            seen++;
            MOQ_TEST_CHECK_EQ_SIZE(ev.u.subscribe_request.token_count, 1);
            if (ev.u.subscribe_request.token_count == 1)
                MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_request.tokens[0].token_type, type);
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_INT(seen, 1);
}

static void test_request(moq_version_t v)
{
    moq_simpair_t *sp = pair(v);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return;
    moq_auth_token_t bad_span = { .token_type = 1, .token_value = { NULL, 3 } };
    refused_subscribe(sp, &bad_span);
    if (v == MOQ_VERSION_DRAFT_16) {
        moq_auth_token_t wide = { .token_type = TYPE_2P62, .token_value = { NULL, 0 } };
        refused_subscribe(sp, &wide);
        wide.token_type = UINT64_MAX;
        refused_subscribe(sp, &wide);
        accepted_subscribe(sp, TYPE_D16MAX);
    } else {
        accepted_subscribe(sp, TYPE_2P62);
        accepted_subscribe(sp, UINT64_MAX);
    }
    /* No token at all is a request like any other. */
    MOQ_TEST_CHECK_EQ_INT((int)subscribe(sp, NULL, 0), (int)MOQ_OK);
    moq_simpair_destroy(sp);
    char name[48];
    snprintf(name, sizeof(name), "auth_token_width request %s", vname(v));
    MOQ_TEST_PASS(name);
}

int main(void)
{
    test_setup();
    test_request(MOQ_VERSION_DRAFT_16);
    test_request(MOQ_VERSION_DRAFT_18);
    test_request(MOQ_VERSION_DRAFT_22);
    return failures ? 1 : 0;
}
