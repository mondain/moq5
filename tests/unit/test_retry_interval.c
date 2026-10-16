/*
 * REQUEST_ERROR Retry Interval: "The minimum time (in milliseconds) before
 * the request SHOULD be sent again, plus one. If the value is 0, the request
 * SHOULD NOT be retried" (draft-16 §9.8, draft-18 §10.6.2, draft-22 §9.4.2). The
 * public API carries the time itself (can_retry, retry_after_ms) on every
 * draft: a retryable refusal goes on the wire as retry_after_ms + 1, so
 * "retry now" is 1, and a received non-zero value surfaces as value - 1.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <stddef.h>
#include <string.h>

enum { EVENT_BOUND = 64, PUMP_BOUND = 64 };
#define LEGACY_WIRE_MAX UINT64_C(0x3fffffffffffffff)
#define LITERAL(...) { (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }) }

typedef struct {
    const char *name;
    bool can_retry;
    uint64_t ms;
    moq_bytes_t d16, d18;
} retry_case_t;

/* D16 9.8: request id 0, UNAUTHORIZED 1, QUIC varint interval, empty reason.
 * D18 10.6.2 / D22 9.4.2: no request id, vi64 interval, empty reason.
 * In particular 1501 = 0x45dd (QUIC) or 0x85dd (vi64). */
static const retry_case_t cases[] = {
    { "no-retry", false, 777,
      LITERAL(0x05, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00),
      LITERAL(0x05, 0x00, 0x03, 0x01, 0x00, 0x00) },
    { "immediate", true, 0,
      LITERAL(0x05, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00),
      LITERAL(0x05, 0x00, 0x03, 0x01, 0x01, 0x00) },
    { "1500ms", true, 1500,
      LITERAL(0x05, 0x00, 0x05, 0x00, 0x01, 0x45, 0xdd, 0x00),
      LITERAL(0x05, 0x00, 0x04, 0x01, 0x85, 0xdd, 0x00) },
    { "front-door-max", true, LEGACY_WIRE_MAX - 1,
      LITERAL(0x05, 0x00, 0x0b, 0x00, 0x01,
              0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00),
      LITERAL(0x05, 0x00, 0x0b, 0x01,
              0xff, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00) },
};

/* Valid D18 receive intervals beyond the current send front door's range. */
static const retry_case_t wide_cases[] = {
    { "vi64-above-62-bit", true, LEGACY_WIRE_MAX, { NULL, 0 },
      LITERAL(0x05, 0x00, 0x0b, 0x01,
              0xff, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) },
    { "vi64-max", true, UINT64_MAX - 1, { NULL, 0 },
      LITERAL(0x05, 0x00, 0x0b, 0x01,
              0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00) },
};
#undef LITERAL

typedef struct {
    bool request, response, bad, fin;
    moq_stream_ref_t client_ref, server_ref;
    size_t opens, responses, len;
    uint8_t bytes[32];
} trace_t;

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    trace_t *t = ctx;
    if (r->struct_size < offsetof(moq_sim_trace_record_t, fin) + sizeof(r->fin)) {
        t->bad = true;
        return;
    }
    if (t->request && r->kind == MOQ_SIM_TRACE_ACTION &&
        r->from == MOQ_PERSPECTIVE_CLIENT && r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        t->client_ref = r->stream_ref;
        t->opens++;
    }
    if (t->request && r->kind == MOQ_SIM_TRACE_INPUT &&
        r->to == MOQ_PERSPECTIVE_SERVER && r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES)
        t->server_ref = r->stream_ref;
    if (!t->response || r->kind != MOQ_SIM_TRACE_ACTION || r->from != MOQ_PERSPECTIVE_SERVER)
        return;
    moq_bytes_t bytes;
    if (r->action_kind == MOQ_ACTION_SEND_CONTROL) bytes = r->bytes;
    else if (r->action_kind == MOQ_ACTION_SEND_BIDI_STREAM) {
        bytes = r->detail_bytes;
        if (r->stream_ref._v != t->server_ref._v) t->bad = true;
    } else return;
    if (++t->responses != 1 || bytes.len > sizeof(t->bytes) || (bytes.len && !bytes.data)) {
        t->bad = true;
        return;
    }
    if (bytes.len) memcpy(t->bytes, bytes.data, bytes.len);
    t->len = bytes.len;
    t->fin = r->fin;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    bool found = false;
    for (unsigned i = 0; i < EVENT_BOUND; i++) {
        if (moq_session_poll_events(s, &ev, 1) != 1) return found;
        if (ev.kind == MOQ_EVENT_SESSION_CLOSED || (found && ev.kind == kind)) {
            moq_event_cleanup(&ev);
            if (found) moq_event_cleanup(out);
            return false;
        }
        if (ev.kind == kind) { *out = ev; found = true; continue; }
        moq_event_cleanup(&ev);
    }
    if (found) moq_event_cleanup(out);
    return false;
}

static bool drain_setup(moq_session_t *s)
{
    moq_event_t ev;
    for (unsigned i = 0; i < EVENT_BOUND; i++) {
        if (moq_session_poll_events(s, &ev, 1) != 1) return true;
        bool closed = ev.kind == MOQ_EVENT_SESSION_CLOSED;
        moq_event_cleanup(&ev);
        if (closed) return false;
    }
    return false;
}

static bool pump(moq_simpair_t *sp)
{
    return moq_simpair_run_until_quiescent(sp, PUMP_BOUND, NULL) == MOQ_OK &&
           moq_session_state(moq_simpair_client(sp)) == MOQ_SESS_ESTABLISHED &&
           moq_session_state(moq_simpair_server(sp)) == MOQ_SESS_ESTABLISHED;
}

/* A pair whose server holds an unanswered SUBSCRIBE from the client. */
static moq_simpair_t *pending_subscribe(moq_version_t version,
                                        moq_subscription_t *client_h,
                                        moq_subscription_t *server_h, trace_t *t)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.initial_now_us = 1000;
    cfg.trace_fn = trace;
    cfg.trace_ctx = t;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) goto fail;
    if (!pump(sp)) goto fail;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    if (moq_session_version(c) != version || moq_session_version(sv) != version ||
        !drain_setup(c) || !drain_setup(sv)) goto fail;
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_subscribe_cfg_t scfg; moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = MOQ_BYTES_LITERAL("v");
    t->request = true;
    if (moq_session_subscribe(c, &scfg, moq_simpair_now_us(sp), client_h) != MOQ_OK) goto fail;
    if (!pump(sp)) goto fail;
    t->request = false;
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) goto fail;
    *server_h = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    if (!moq_subscription_is_valid(*client_h) || !moq_subscription_is_valid(*server_h) || t->bad)
        goto fail;
    if (version != MOQ_VERSION_DRAFT_16 &&
        (t->opens != 1 || !t->client_ref._v || !t->server_ref._v)) goto fail;
    return sp;
fail:
    moq_simpair_destroy(sp);
    return NULL;
}

static moq_result_t reject(moq_session_t *sv, moq_subscription_t h,
                           bool can_retry, uint64_t ms, uint64_t now_us)
{
    moq_reject_subscribe_cfg_t rc; moq_reject_subscribe_cfg_init(&rc);
    rc.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
    rc.can_retry = can_retry;
    rc.retry_after_ms = ms;
    return moq_session_reject_subscribe(sv, h, &rc, now_us);
}

static moq_bytes_t literal(moq_version_t version, const retry_case_t *tc)
{
    return version == MOQ_VERSION_DRAFT_16 ? tc->d16 : tc->d18;
}

static int check_event(moq_simpair_t *sp, moq_subscription_t ch, const retry_case_t *tc)
{
    int failures = 0;
    moq_session_t *c = moq_simpair_client(sp);
    moq_event_t ev;
    bool found = take(c, MOQ_EVENT_SUBSCRIBE_ERROR, &ev);
    MOQ_TEST_CHECK(found);
    if (found) {
        MOQ_TEST_CHECK(moq_subscription_eq(ev.u.subscribe_error.sub, ch));
        MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_error.error_code, 1);
        MOQ_TEST_CHECK_EQ_INT((int)ev.u.subscribe_error.can_retry, (int)tc->can_retry);
        MOQ_TEST_CHECK_EQ_U64(ev.u.subscribe_error.retry_after_ms, tc->can_retry ? tc->ms : 0);
        MOQ_TEST_CHECK_EQ_SIZE(ev.u.subscribe_error.reason.len, 0);
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(moq_simpair_server(sp)), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

static int send_and_surface(moq_simpair_t *sp, moq_version_t version,
                            moq_subscription_t ch, moq_subscription_t sh,
                            trace_t *t, const retry_case_t *tc)
{
    int failures = 0;
    t->response = true;
    moq_result_t rc = reject(moq_simpair_server(sp), sh, tc->can_retry, tc->ms,
                             moq_simpair_now_us(sp));
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) return failures;
    bool pumped = pump(sp);
    MOQ_TEST_CHECK(pumped);
    if (!pumped) return failures;
    moq_bytes_t expected = literal(version, tc);
    MOQ_TEST_CHECK(!t->bad && t->responses == 1);
    MOQ_TEST_CHECK(t->len == expected.len && memcmp(t->bytes, expected.data, expected.len) == 0);
    MOQ_TEST_CHECK(version == MOQ_VERSION_DRAFT_16 || t->fin);
    failures += check_event(sp, ch, tc);
    return failures;
}

/* Keep the existing round-trip event control, with an independent wire oracle. */
static int surfaced(moq_version_t version, const retry_case_t *tc)
{
    int failures = 0;
    moq_subscription_t ch = MOQ_SUBSCRIPTION_INVALID, sh = MOQ_SUBSCRIPTION_INVALID;
    trace_t t = {0};
    moq_simpair_t *sp = pending_subscribe(version, &ch, &sh, &t);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    failures += send_and_surface(sp, version, ch, sh, &t, tc);
    moq_simpair_destroy(sp);
    return failures;
}

/* No rejection/encoder runs here: a separate session receives fixed literals. */
static int received_literal(moq_version_t version, const retry_case_t *tc)
{
    int failures = 0;
    moq_subscription_t ch = MOQ_SUBSCRIPTION_INVALID, sh = MOQ_SUBSCRIPTION_INVALID;
    trace_t t = {0};
    moq_simpair_t *sp = pending_subscribe(version, &ch, &sh, &t);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    moq_bytes_t bytes = literal(version, tc);
    t.response = true;
    moq_result_t rc = version == MOQ_VERSION_DRAFT_16
        ? moq_session_on_control_bytes(c, bytes.data, bytes.len, moq_simpair_now_us(sp))
        : moq_session_on_bidi_stream_bytes(c, t.client_ref, bytes.data, bytes.len,
                                           true, moq_simpair_now_us(sp));
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    failures += check_event(sp, ch, tc);
    /* The simulated server still has an unanswered request. Do not pump a
     * reciprocal FIN back into that unrelated state after a manual response. */
    size_t closes = 0;
    moq_action_t a;
    for (unsigned i = 0; i < 2; i++) {
        if (moq_session_poll_actions(c, &a, 1) != 1) break;
        MOQ_TEST_CHECK(a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM);
        if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM)
            MOQ_TEST_CHECK(a.u.close_bidi_stream.stream_ref._v == t.client_ref._v);
        closes++;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, version == MOQ_VERSION_DRAFT_16 ? 0 : 1);
    MOQ_TEST_CHECK(!t.bad && t.responses == 0);
done:
    moq_simpair_destroy(sp);
    return failures;
}

static bool no_effects(moq_session_t *s)
{
    moq_action_t a;
    moq_event_t ev;
    bool empty = true;
    if (moq_session_poll_actions(s, &a, 1) == 1) {
        moq_action_cleanup(&a);
        empty = false;
    }
    if (moq_session_poll_events(s, &ev, 1) == 1) {
        moq_event_cleanup(&ev);
        empty = false;
    }
    return empty && moq_session_state(s) == MOQ_SESS_ESTABLISHED;
}

static int boundary_refusals(moq_version_t version)
{
    int failures = 0;
    moq_subscription_t ch = MOQ_SUBSCRIPTION_INVALID, sh = MOQ_SUBSCRIPTION_INVALID;
    trace_t t = {0};
    moq_simpair_t *sp = pending_subscribe(version, &ch, &sh, &t);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    /* First refused value; valid full-width vi64 delay; plus-one overflow. */
    static const uint64_t refused[] = { LEGACY_WIRE_MAX, UINT64_MAX - 1, UINT64_MAX };
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); i++) {
        moq_result_t rc = reject(sv, sh, true, refused[i], moq_simpair_now_us(sp));
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INVAL);
        bool empty = no_effects(c) && no_effects(sv);
        MOQ_TEST_CHECK(empty);
        if (rc != MOQ_ERR_INVAL || !empty) goto done;
    }
    /* Refusal must leave this exact pending handle usable. */
    failures += send_and_surface(sp, version, ch, sh, &t, &cases[1]);
done:
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
    };
    for (size_t i = 0; i < sizeof(versions) / sizeof(*versions); i++) {
        moq_version_t v = versions[i];
        size_t count = v == MOQ_VERSION_DRAFT_22 ? 3 : sizeof(cases) / sizeof(*cases);
        for (size_t j = 0; j < count; j++) {
            int n = surfaced(v, &cases[j]);
            printf("D%u/send/%s %s\n", (unsigned)v, cases[j].name, n ? "FAIL" : "PASS");
            failures += n;
            if (failures) return 1;
            if (v == MOQ_VERSION_DRAFT_22) continue;
            n = received_literal(v, &cases[j]);
            printf("D%u/literal-receive/%s %s\n", (unsigned)v, cases[j].name, n ? "FAIL" : "PASS");
            failures += n;
            if (failures) return 1;
        }
        if (v == MOQ_VERSION_DRAFT_22) continue;
        int n = boundary_refusals(v);
        printf("D%u/front-door-three-refusals-and-valid-retry %s\n", (unsigned)v, n ? "FAIL" : "PASS");
        failures += n;
        if (failures) return 1;
    }
    for (size_t i = 0; i < sizeof(wide_cases) / sizeof(*wide_cases); i++) {
        int n = received_literal(MOQ_VERSION_DRAFT_18, &wide_cases[i]);
        printf("D18/literal-receive/%s %s\n", wide_cases[i].name, n ? "FAIL" : "PASS");
        failures += n;
        if (failures) return 1;
    }
    MOQ_TEST_PASS("retry_interval");
    return failures != 0;
}
