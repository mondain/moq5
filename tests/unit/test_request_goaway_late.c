/*
 * Messages after a request-stream GOAWAY (draft-18 §10.4, draft-22 §9.2).
 *
 * A GOAWAY on a request stream does not end the request: "The GOAWAY message
 * does not impact subscription state", and the sender may still owe messages
 * on that stream -- the answer to a REQUEST_UPDATE already in flight (§9.5:
 * exactly one REQUEST_OK or REQUEST_ERROR), or PUBLISH_DONE before its FIN
 * when it closes the old request stream. The receiver migrates the request
 * and discards whatever else arrives there, whether in the GOAWAY's own read
 * or a later one, and however the reads split it. Only a second GOAWAY
 * received on the same stream closes the session with PROTOCOL_VIOLATION; a
 * GOAWAY received after one this endpoint sent on the same stream is the
 * first received there and migrates the request as usual. Covered on a
 * subscription's and a namespace subscription's stream, and on the
 * publisher side of a namespace subscription it migrated itself.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_version_t g_version;
static moq_bytes_t g_ns_parts[] = { { (const uint8_t *)"live", 4 } };

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    moq_subscription_t csub, ssub;
    moq_stream_ref_t cref;   /* the client's request bidi of the subscription */
} pair_t;

/* A first step may move nothing while actions are still queued on both
 * sides, so run to quiescence twice. */
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

static size_t count_kind(moq_session_t *s, moq_event_kind_t kind)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == kind) n++;
        moq_event_cleanup(&ev);
    }
    return n;
}

static bool mk(pair_t *p)
{
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    if (moq_simpair_create(&scfg, &p->sp) < 0) return false;
    if (moq_simpair_start(p->sp) < 0) return false;
    pump(p);
    p->c = moq_simpair_client(p->sp);
    p->sv = moq_simpair_server(p->sp);
    drain(p->c); drain(p->sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
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
    int cs = sub_resolve_handle(p->c, p->csub);
    if (cs < 0) return false;
    p->cref = p->c->subs[cs].rs.ref;
    return p->cref._v != 0;
}

/* Encoded messages the publisher could send on the subscription stream. */
static size_t enc_goaway(moq_session_t *s, uint8_t *buf, size_t cap)
{
    moq_buf_writer_t w; moq_buf_writer_init(&w, buf, cap);
    if (s->profile->encode_request_goaway(s, &w, NULL, 0, 50) < 0) return 0;
    return moq_buf_writer_offset(&w);
}

static size_t enc_publish_done(moq_session_t *s, uint64_t request_id, uint8_t *buf, size_t cap)
{
    moq_buf_writer_t w; moq_buf_writer_init(&w, buf, cap);
    moq_finish_publish_encode_args_t a = {
        .request_id = request_id, .status_code = 0x4, .stream_count = 0,
        .reason = (const uint8_t *)"moving", .reason_len = 6 };
    if (s->profile->encode_publish_done(s, &w, &a) < 0) return 0;
    return moq_buf_writer_offset(&w);
}

/* Our own publisher answers an update that was in flight when it sent its
 * GOAWAY; the subscriber migrates and keeps the session. The race is driven
 * by hand: the update reaches the publisher after its GOAWAY was queued, and
 * the subscriber then reads the GOAWAY and the answer behind it. */
static size_t take_bidi_bytes(moq_session_t *s, moq_stream_ref_t ref, uint8_t *out,
                              size_t cap, size_t *count)
{
    size_t n = 0;
    *count = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref._v &&
            n + a.u.send_bidi_stream.len <= cap) {
            memcpy(out + n, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            n += a.u.send_bidi_stream.len;
            (*count)++;
        }
        moq_action_cleanup(&a);
    }
    return n;
}

static int test_update_answered_after_goaway(void)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    int ss = sub_resolve_handle(p.sv, p.ssub);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss < 0) { moq_simpair_destroy(p.sp); return failures; }
    moq_stream_ref_t sref = p.sv->subs[ss].rs.ref;

    moq_subscription_update_cfg_t up;
    moq_subscription_update_cfg_init_sized(&up, sizeof(up));
    up.has_subscriber_priority = true; up.subscriber_priority = 7;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_subscription(p.c, p.csub, &up, 3),
                          (int)MOQ_OK);
    uint8_t upd[128]; size_t nmsg = 0;
    size_t upd_len = take_bidi_bytes(p.c, p.cref, upd, sizeof(upd), &nmsg);
    MOQ_TEST_CHECK(upd_len > 0);

    moq_request_goaway_cfg_t gc; moq_request_goaway_cfg_init(&gc);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_request_goaway_subscribe(p.sv, p.ssub, &gc, 3),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(p.sv, sref, upd, upd_len,
                                                                false, 4), (int)MOQ_OK);
    uint8_t out[256];
    size_t out_len = take_bidi_bytes(p.sv, sref, out, sizeof(out), &nmsg);
    MOQ_TEST_CHECK_EQ_SIZE(nmsg, 2);             /* the GOAWAY, then the answer */

    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(p.c, p.cref, out, out_len,
                                                                false, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.c, MOQ_EVENT_REQUEST_GOAWAY), 1);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* GOAWAY then PUBLISH_DONE and FIN, delivered in `split` reads: every split
 * point from one read to one byte at a time. */
static int test_publish_done_after_goaway(size_t split)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    uint8_t buf[256];
    size_t n = enc_goaway(p.sv, buf, sizeof(buf));
    int ss = sub_resolve_handle(p.sv, p.ssub);
    size_t m = enc_publish_done(p.sv, p.sv->subs[ss].request_id, buf + n, sizeof(buf) - n);
    MOQ_TEST_CHECK(n > 0 && m > 0);
    size_t total = n + m;
    for (size_t off = 0; off < total; ) {
        size_t chunk = split ? split : total;
        if (chunk > total - off) chunk = total - off;
        bool last = off + chunk == total;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(p.c, p.cref, buf + off,
                                  chunk, last, 4), (int)MOQ_OK);
        off += chunk;
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.c, MOQ_EVENT_REQUEST_GOAWAY), 1);
    MOQ_TEST_CHECK(!drain_ref_contains(p.c, p.cref));
    moq_simpair_destroy(p.sp);
    return failures;
}

/* GOAWAY, an unrelated message, then a second GOAWAY, in `split` reads:
 * the second closes with PROTOCOL_VIOLATION wherever the reads split. */
static int test_second_goaway_closes(size_t split, bool with_between)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    uint8_t buf[256];
    size_t n = enc_goaway(p.sv, buf, sizeof(buf));
    if (with_between) {
        int ss = sub_resolve_handle(p.sv, p.ssub);
        n += enc_publish_done(p.sv, p.sv->subs[ss].request_id, buf + n, sizeof(buf) - n);
    }
    n += enc_goaway(p.sv, buf + n, sizeof(buf) - n);
    for (size_t off = 0; off < n && moq_session_state(p.c) != MOQ_SESS_CLOSED; ) {
        size_t chunk = split ? split : n;
        if (chunk > n - off) chunk = n - off;
        (void)moq_session_on_bidi_stream_bytes(p.c, p.cref, buf + off, chunk, false, 4);
        off += chunk;
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c), (int)MOQ_SESS_CLOSED);
    bool close_3 = false;
    moq_action_t a;
    while (moq_session_poll_actions(p.c, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION && a.u.close_session.code == 0x3)
            close_3 = true;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK(close_3);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* Both ends migrate the request at once: the GOAWAY each receives is the
 * first it received on the stream. */
static int test_crossing_goaways(void)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    moq_request_goaway_cfg_t gc; moq_request_goaway_cfg_init(&gc);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_request_goaway_subscribe(p.sv, p.ssub, &gc, 3),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_request_goaway_subscribe(p.c, p.csub, &gc, 3),
                          (int)MOQ_OK);
    pump(&p);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.c, MOQ_EVENT_REQUEST_GOAWAY), 1);
    MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.sv, MOQ_EVENT_REQUEST_GOAWAY), 1);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* The same on a namespace subscription's stream: a message behind the
 * GOAWAY in the same read is discarded; a second GOAWAY closes. */
static int test_ns_sub(bool second_goaway)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    moq_subscribe_namespace_cfg_t cfg; moq_subscribe_namespace_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe_namespace(p.c, &cfg, 3, &ch),
                          (int)MOQ_OK);
    pump(&p);
    moq_event_t ev;
    moq_ns_sub_handle_t sh = { 0 };
    while (moq_session_poll_events(p.sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_NS_SUB_REQUEST) sh = ev.u.ns_sub_request.handle;
        moq_event_cleanup(&ev);
    }
    moq_accept_ns_sub_cfg_t ac; moq_accept_ns_sub_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_ns_sub(p.sv, sh, &ac, 3), (int)MOQ_OK);
    pump(&p);
    drain(p.c);
    moq_stream_ref_t ref = { 0 };
    for (size_t i = 0; i < p.c->ns_sub_cap; i++)
        if (p.c->ns_subs[i].state == MOQ_NS_SUB_ESTABLISHED) ref = p.c->ns_subs[i].stream_ref;
    MOQ_TEST_CHECK(ref._v != 0);
    uint8_t buf[256];
    size_t n = enc_goaway(p.sv, buf, sizeof(buf));
    n += second_goaway ? enc_goaway(p.sv, buf + n, sizeof(buf) - n)
                       : enc_publish_done(p.sv, 0, buf + n, sizeof(buf) - n);
    (void)moq_session_on_bidi_stream_bytes(p.c, ref, buf, n, false, 4);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.c),
                          (int)(second_goaway ? MOQ_SESS_CLOSED : MOQ_SESS_ESTABLISHED));
    if (!second_goaway)
        MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.c, MOQ_EVENT_REQUEST_GOAWAY), 1);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* The publisher side of a namespace subscription it migrated with its own
 * GOAWAY: what the subscriber still sends there is discarded, the
 * subscriber's own GOAWAY (the first received on the stream) migrates the
 * request, and only a second one closes the session. `mode`: 0 a message,
 * 1 a GOAWAY, 2 two GOAWAYs; split into reads of `split` bytes (0 = one). */
static int test_ns_sub_publisher_after_our_goaway(int mode, size_t split)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p));
    if (!p.sp) return failures;
    moq_subscribe_namespace_cfg_t cfg; moq_subscribe_namespace_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe_namespace(p.c, &cfg, 3, &ch),
                          (int)MOQ_OK);
    pump(&p);
    moq_event_t ev;
    moq_ns_sub_handle_t sh = { 0 };
    while (moq_session_poll_events(p.sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_NS_SUB_REQUEST) sh = ev.u.ns_sub_request.handle;
        moq_event_cleanup(&ev);
    }
    moq_accept_ns_sub_cfg_t ac; moq_accept_ns_sub_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_ns_sub(p.sv, sh, &ac, 3), (int)MOQ_OK);
    pump(&p);
    drain(p.c); drain(p.sv);
    moq_request_goaway_cfg_t gc; moq_request_goaway_cfg_init(&gc);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_request_goaway_ns_sub(p.sv, sh, &gc, 4),
                          (int)MOQ_OK);
    moq_stream_ref_t ref = { 0 };
    for (size_t i = 0; i < p.sv->ns_sub_cap; i++)
        if (p.sv->ns_subs[i].goaway_sent) ref = p.sv->ns_subs[i].stream_ref;
    MOQ_TEST_CHECK(ref._v != 0);
    uint8_t buf[256];
    size_t n = 0;
    if (mode == 0) n = enc_publish_done(p.c, 0, buf, sizeof(buf));   /* any framed message */
    if (mode >= 1) n = enc_goaway(p.c, buf, sizeof(buf));
    if (mode == 2) n += enc_goaway(p.c, buf + n, sizeof(buf) - n);
    for (size_t off = 0; off < n && moq_session_state(p.sv) != MOQ_SESS_CLOSED; ) {
        size_t chunk = split ? split : n;
        if (chunk > n - off) chunk = n - off;
        (void)moq_session_on_bidi_stream_bytes(p.sv, ref, buf + off, chunk, false, 5);
        off += chunk;
    }
    if (mode == 2) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_CLOSED);
    } else {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_SIZE(count_kind(p.sv, MOQ_EVENT_REQUEST_GOAWAY), mode == 1 ? 1 : 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(p.sv, ref, NULL, 0, true, 6),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_ESTABLISHED);
    }
    moq_simpair_destroy(p.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        g_version = versions[i];
        int before = failures;
        failures += test_update_answered_after_goaway();
        for (size_t split = 0; split <= 4; split++)
            failures += test_publish_done_after_goaway(split);
        for (size_t split = 0; split <= 3; split++) {
            failures += test_second_goaway_closes(split, false);
            failures += test_second_goaway_closes(split, true);
        }
        failures += test_crossing_goaways();
        failures += test_ns_sub(false);
        failures += test_ns_sub(true);
        for (size_t split = 0; split <= 3; split++)
            for (int mode = 0; mode < 3; mode++)
                failures += test_ns_sub_publisher_after_our_goaway(mode, split);
        if (failures != before)
            fprintf(stderr, "  version 0x%llx: %d failures\n",
                    (unsigned long long)versions[i], failures - before);
    }
    if (failures == 0) MOQ_TEST_PASS("request_goaway_late");
    return failures != 0;
}
