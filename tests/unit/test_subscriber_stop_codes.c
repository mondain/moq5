/*
 * The STOP_SENDING code a subscriber sends on a data stream it gives up
 * because the stream's subscription is gone: CANCELLED (0x1) -- "The stream
 * was cancelled by either endpoint" (draft-18 §3.3.3, draft-22 §12.5, which
 * apply their codes to "resetting or sending STOP_SENDING on any stream");
 * draft-16 §10.4.3 defines CANCELLED as "The subscriber requested
 * cancellation via UNSUBSCRIBE, FETCH_CANCEL or STOP_SENDING". Two
 * situations, on every profile:
 *   - the subscription was cancelled: its open stream is stopped then, and
 *     the bytes the peer had already sent are discarded without another
 *     STOP;
 *   - PUBLISH_DONE named more streams than arrived, and the wait for them
 *     expired: the subscriber deletes the subscription state and stops the
 *     streams still open ("SHOULD send STOP_SENDING on all streams related
 *     to the subscription when it deletes subscription state").
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_version_t g_version;
static moq_bytes_t g_parts[] = { { (const uint8_t *)"live", 4 } };

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    moq_subscription_t csub, ssub;
} pair_t;

static void pump(pair_t *p)
{
    moq_simpair_run_until_quiescent(p->sp, 64, NULL);
    moq_simpair_run_until_quiescent(p->sp, 64, NULL);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static bool mk(pair_t *p)
{
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    scfg.done_wait_timeout_us = 50000;
    if (moq_simpair_create(&scfg, &p->sp) < 0) return false;
    if (moq_simpair_start(p->sp) < 0) return false;
    p->c = moq_simpair_client(p->sp);
    p->sv = moq_simpair_server(p->sp);
    pump(p);
    drain(p->c); drain(p->sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    if (moq_session_subscribe(p->c, &cfg, 1, &p->csub) != MOQ_OK) return false;
    pump(p);
    moq_event_t ev;
    bool got = false;
    while (moq_session_poll_events(p->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) { p->ssub = ev.u.subscribe_request.sub; got = true; }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    if (moq_session_accept_subscribe(p->sv, p->ssub, &ac, 2) != MOQ_OK) return false;
    pump(p);
    drain(p->c); drain(p->sv);
    return true;
}

static moq_result_t write_obj(pair_t *p, moq_subgroup_handle_t h, uint64_t obj)
{
    moq_rcbuf_t *pl = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
    moq_result_t rc = moq_session_write_object(p->sv, h, obj, pl, 3);
    moq_rcbuf_decref(pl);
    return rc;
}

/* The STOP_DATA codes the client queued, any stream; returns how many. */
static size_t take_stops(moq_session_t *s, uint64_t *code)
{
    size_t n = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_STOP_DATA) { n++; *code = a.u.stop_data.error_code; }
        moq_action_cleanup(&a);
    }
    return n;
}

static uint64_t want_code(void)
{
    return MOQ_STREAM_ERROR_CANCELLED;
}

/* The client cancels; the publisher's next object on the open stream finds
 * the subscription gone. */
static int test_cancelled_then_bytes(void)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    moq_subgroup_handle_t h;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(p.sv, p.ssub, &sc, 3, &h), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_obj(&p, h, 0), (int)MOQ_OK);
    pump(&p);
    drain(p.c);
    /* The next object is in flight when the subscriber cancels. */
    MOQ_TEST_CHECK_EQ_INT((int)write_obj(&p, h, 1), (int)MOQ_OK);
    uint8_t buf[128]; size_t n = 0;
    moq_stream_ref_t sref = { 0 };
    moq_action_t a;
    while (moq_session_poll_actions(p.sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            sref = a.u.send_data.stream_ref;
            memcpy(buf + n, a.u.send_data.header, a.u.send_data.header_len);
            n += a.u.send_data.header_len;
            if (a.u.send_data.payload) {
                memcpy(buf + n, moq_rcbuf_data(a.u.send_data.payload),
                       moq_rcbuf_len(a.u.send_data.payload));
                n += moq_rcbuf_len(a.u.send_data.payload);
            }
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(n > 0 && sref._v != 0);
    /* The client-side stream bound to the subscription. */
    moq_stream_ref_t cref = { 0 };
    for (size_t i = 0; i < p.c->rx_cap; i++)
        if (p.c->rx_streams[i].active &&
            moq_subscription_eq(p.c->rx_streams[i].sub, p.csub))
            cref = p.c->rx_streams[i].stream_ref;
    MOQ_TEST_CHECK(cref._v != 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(p.c, p.csub, 4), (int)MOQ_OK);
    uint64_t code = 99;
    MOQ_TEST_CHECK_EQ_SIZE(take_stops(p.c, &code), 1);
    MOQ_TEST_CHECK_EQ_U64(code, want_code());
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_bytes(p.c, cref, buf, n, false, 5),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(take_stops(p.c, &code), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* PUBLISH_DONE names two streams; one never closes; the wait expires. */
static int test_stream_count_expiry(void)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
    moq_subgroup_handle_t h;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(p.sv, p.ssub, &sc, 3, &h), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)write_obj(&p, h, 0), (int)MOQ_OK);
    pump(&p);
    drain(p.c);
    /* The FIN is lost; the publisher then ends the subscription. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(p.sv, h, 3), (int)MOQ_OK);
    moq_action_t a;
    while (moq_session_poll_actions(p.sv, &a, 1) > 0) moq_action_cleanup(&a);
    moq_done_subscribe_cfg_t dc; moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x2; dc.stream_count = 2;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_done_subscribe(p.sv, p.ssub, &dc, 3), (int)MOQ_OK);
    pump(&p);
    uint64_t code = 99;
    (void)take_stops(p.c, &code);
    code = 99;
    uint64_t now = moq_simpair_now_us(p.sp) + 1000000;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(p.c, now), (int)MOQ_OK);
    size_t stops = take_stops(p.c, &code);
    MOQ_TEST_CHECK_EQ_SIZE(stops, 1);
    MOQ_TEST_CHECK_EQ_U64(code, want_code());
    MOQ_TEST_CHECK(moq_session_state(p.c) != MOQ_SESS_CLOSED);
    moq_simpair_destroy(p.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 3; i++) {
        g_version = versions[i];
        int before = failures;
        failures += test_cancelled_then_bytes();
        failures += test_stream_count_expiry();
        if (failures != before)
            fprintf(stderr, "  version 0x%llx: %d failures\n",
                    (unsigned long long)versions[i], failures - before);
    }
    if (failures == 0) MOQ_TEST_PASS("subscriber_stop_codes");
    return failures != 0;
}
