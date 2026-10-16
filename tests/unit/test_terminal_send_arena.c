/*
 * A subscription's or publication's terminal (PUBLISH_DONE) and the session's
 * send arena. The arena is reclaimed only once every queued action has been
 * taken, so a terminal that does not fit the occupied tail but fits the
 * arena once its output drains is a retryable shortage (WOULD_BLOCK) that
 * changes nothing; only a terminal that cannot fit even the empty arena is
 * BUFFER. Both terminal calls, on every profile: draft-16 (control channel),
 * draft-18 and draft-22 (request streams).
 *
 * The occupied tail is real output: a namespace announcement queued by the
 * same session, sized so the tail is exactly one byte short of the encoded
 * terminal (the fixture asserts that fill before judging the result).
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <moq/control_d18.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/profile.h"

#include <string.h>

typedef struct pair {
    moq_simpair_t *sp;
    moq_session_t *pub;   /* the side that owes the terminal */
    moq_session_t *peer;
} pair_t;

static void
settle(pair_t *p)
{
    size_t steps = 0;
    (void)moq_simpair_run_until_quiescent(p->sp, 256, &steps);
}

static void
drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static bool
pair_open(pair_t *p, moq_version_t v)
{
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0xA4E7u;
    cfg.version = v;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 64;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 64;
    if (moq_simpair_create(&cfg, &p->sp) != MOQ_OK) return false;
    if (moq_simpair_start(p->sp) != MOQ_OK) return false;
    p->pub = moq_simpair_server(p->sp);
    p->peer = moq_simpair_client(p->sp);
    settle(p);
    drain_events(p->pub);
    drain_events(p->peer);
    return true;
}

/* The publisher side owes a subscription terminal (publication == false: the
 * peer subscribed) or a publication terminal (publication == true). */
static bool
establish(pair_t *p, bool publication, moq_subscription_t *sub,
          moq_publication_t *pubh)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("arena");
    moq_namespace_t ns = { &part, 1 };
    if (publication) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init_sized(&pc, sizeof(pc));
        pc.track_namespace = ns;
        pc.track_name = MOQ_BYTES_LITERAL("t");
        if (moq_session_publish(p->pub, &pc, 1, pubh) != MOQ_OK) return false;
        settle(p);
        moq_event_t ev;
        bool asked = false;
        while (moq_session_poll_events(p->peer, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) {
                moq_accept_publish_cfg_t a;
                moq_accept_publish_cfg_init(&a);
                asked = moq_session_accept_publish(
                            p->peer, ev.u.publish_request.pub, &a, 1) == MOQ_OK;
            }
            moq_event_cleanup(&ev);
        }
        settle(p);
        drain_events(p->pub);
        return asked;
    }
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = ns;
    sc.track_name = MOQ_BYTES_LITERAL("t");
    moq_subscription_t peer_sub;
    if (moq_session_subscribe(p->peer, &sc, 1, &peer_sub) != MOQ_OK) return false;
    settle(p);
    moq_event_t ev;
    bool asked = false;
    while (moq_session_poll_events(p->pub, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            *sub = ev.u.subscribe_request.sub;
            moq_accept_subscribe_cfg_t a;
            moq_accept_subscribe_cfg_init(&a);
            asked = moq_session_accept_subscribe(p->pub, *sub, &a, 1) == MOQ_OK;
        }
        moq_event_cleanup(&ev);
    }
    settle(p);
    drain_events(p->peer);
    return asked;
}

/* Queue a namespace announcement whose single extra part is `len` bytes; the
 * actions it queues stay unpolled, so its bytes occupy the arena. */
static bool
announce(pair_t *p, char tag, size_t len)
{
    static uint8_t big[4096];
    memset(big, tag, sizeof(big));
    moq_bytes_t parts[2] = { MOQ_BYTES_LITERAL("arena"), { big, len } };
    moq_publish_namespace_cfg_t nc;
    moq_publish_namespace_cfg_init(&nc);
    nc.track_namespace = (moq_namespace_t){ parts, 2 };
    moq_announcement_t ann;
    return moq_session_publish_namespace(p->pub, &nc, 1, &ann) == MOQ_OK;
}

/* The terminal this session would send for request `rid`, encoded by its
 * own profile (status TRACK_ENDED, stream count 0, no reason). */
static size_t
terminal_bytes(moq_session_t *s, uint64_t rid, uint8_t *buf, size_t cap)
{
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    moq_finish_publish_encode_args_t args = {
        .request_id = rid, .status_code = 0x2, .stream_count = 0,
        .reason = NULL, .reason_len = 0,
    };
    if (s->profile->encode_publish_done(s, &w, &args) != MOQ_OK) return 0;
    return moq_buf_writer_offset(&w);
}

/* Take every queued action from the publisher side without delivering it
 * (the arena is reclaimed at the next call). With `want` non-NULL, count the
 * actions that carry exactly the profile's own encoding of that terminal
 * (compared byte for byte against it, not decoded independently), sent where
 * the profile sends it: the control channel, or -- with its FIN -- the request
 * stream `ref` of that very request. */
static size_t
take_actions(moq_session_t *s, const uint8_t *want, size_t want_len,
             moq_stream_ref_t ref, int *dones)
{
    size_t n = 0;
    moq_action_t a;
    bool req = moq_session_uses_request_streams(s);
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        n++;
        if (want != NULL) {
            const uint8_t *data = NULL;
            size_t len = 0;
            bool placed = false;
            if (!req && a.kind == MOQ_ACTION_SEND_CONTROL) {
                data = a.u.send_control.data;
                len = a.u.send_control.len;
                placed = true;
            } else if (req && a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
                data = a.u.send_bidi_stream.data;
                len = a.u.send_bidi_stream.len;
                placed = a.u.send_bidi_stream.fin &&
                         a.u.send_bidi_stream.stream_ref._v == ref._v;
            }
            if (placed && len == want_len && memcmp(data, want, len) == 0)
                (*dones)++;
        }
        moq_action_cleanup(&a);
    }
    return n;
}

static moq_result_t
terminal(pair_t *p, bool publication, moq_subscription_t sub,
         moq_publication_t pubh, moq_bytes_t reason)
{
    if (publication) {
        moq_finish_publish_cfg_t fc;
        moq_finish_publish_cfg_init(&fc);
        fc.status_code = 0x2;
        fc.stream_count = 0;
        fc.reason = reason;
        return moq_session_finish_publish(p->pub, pubh, &fc, 2);
    }
    moq_done_subscribe_cfg_t dc;
    moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x2;
    dc.stream_count = 0;
    dc.reason = reason;
    return moq_session_done_subscribe(p->pub, sub, &dc, 2);
}

static size_t
terminal_len(moq_session_t *s, uint64_t request_id)
{
    uint8_t buf[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    moq_finish_publish_encode_args_t args = {
        .request_id = request_id, .status_code = 0x2, .stream_count = 0,
        .reason = NULL, .reason_len = 0,
    };
    if (s->profile->encode_publish_done(s, &w, &args) != MOQ_OK) return 0;
    return moq_buf_writer_offset(&w);
}

static int
case_short_tail(moq_version_t v, bool publication)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(pair_open(&p, v));
    moq_subscription_t sub = { 0 };
    moq_publication_t pubh = { 0 };
    bool est = establish(&p, publication, &sub, &pubh);
    MOQ_TEST_CHECK(est);
    if (!est) {
        moq_simpair_destroy(p.sp);
        return failures;
    }
    uint64_t rid = publication
        ? p.pub->publishes[pub_resolve_handle(p.pub, pubh)].request_id
        : p.pub->subs[sub_resolve_handle(p.pub, sub)].request_id;
    moq_stream_ref_t rref = publication
        ? p.pub->publishes[pub_resolve_handle(p.pub, pubh)].rs.ref
        : p.pub->subs[sub_resolve_handle(p.pub, sub)].rs.ref;
    size_t tlen = terminal_len(p.pub, rid);
    MOQ_TEST_CHECK(tlen > 0);

    /* Probe the announcement's framing, then queue one that leaves the tail
     * exactly tlen - 1 bytes. */
    MOQ_TEST_CHECK(take_actions(p.pub, NULL, 0, (moq_stream_ref_t){0}, NULL) == 0);
    MOQ_TEST_CHECK(announce(&p, 'p', 1000));
    size_t over = p.pub->send_len - 1000;
    (void)take_actions(p.pub, NULL, 0, (moq_stream_ref_t){0}, NULL);
    size_t want_fill = p.pub->send_cap - (tlen - 1);
    MOQ_TEST_CHECK(announce(&p, 'f', want_fill - over));
    MOQ_TEST_CHECK_EQ_SIZE(p.pub->send_len, want_fill);   /* exact fill */
    size_t queued = p.pub->action_tail - p.pub->action_head;

    moq_result_t rc = terminal(&p, publication, sub, pubh, (moq_bytes_t){0});
    printf("  draft %u %s: tail %zu, terminal %zu -> rc %d\n",
           (unsigned)v, publication ? "publication" : "subscription",
           p.pub->send_cap - want_fill, tlen, (int)rc);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_WOULD_BLOCK);
    /* Nothing changed: no action queued, the request still owned. */
    MOQ_TEST_CHECK_EQ_SIZE(p.pub->action_tail - p.pub->action_head, queued);
    MOQ_TEST_CHECK_EQ_SIZE(p.pub->send_len, want_fill);
    if (publication) {
        MOQ_TEST_CHECK(pub_resolve_handle(p.pub, pubh) >= 0);
    } else {
        MOQ_TEST_CHECK(sub_resolve_handle(p.pub, sub) >= 0);
    }
    /* Once the queued output is taken, the same terminal goes out exactly
     * once: its own bytes (request id, status, stream count), on the control
     * channel or as the request stream's final message with FIN. */
    (void)take_actions(p.pub, NULL, 0, (moq_stream_ref_t){0}, NULL);
    MOQ_TEST_CHECK_EQ_INT(
        (int)terminal(&p, publication, sub, pubh, (moq_bytes_t){0}),
        (int)MOQ_OK);
    uint8_t want[64];
    size_t want_len = terminal_bytes(p.pub, rid, want, sizeof(want));
    int dones = 0;
    (void)take_actions(p.pub, want, want_len, rref, &dones);
    MOQ_TEST_CHECK_EQ_INT(dones, 1);
    MOQ_TEST_CHECK_EQ_INT(
        (int)terminal(&p, publication, sub, pubh, (moq_bytes_t){0}),
        (int)MOQ_ERR_STALE_HANDLE);
    MOQ_TEST_CHECK_EQ_INT((int)p.pub->state, (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* A publication terminal that can never be encoded (finish_publish builds it
 * in a fixed 256-byte buffer and this reason is longer, though within the
 * 1024-byte limit) is BUFFER, every time, on an empty arena, and changes
 * nothing: the same publication then ends normally. */
static int
case_never_fits(moq_version_t v, bool publication)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(pair_open(&p, v));
    moq_subscription_t sub = { 0 };
    moq_publication_t pubh = { 0 };
    bool est = establish(&p, publication, &sub, &pubh);
    MOQ_TEST_CHECK(est);
    if (!est) {
        moq_simpair_destroy(p.sp);
        return failures;
    }
    static uint8_t huge[300];
    memset(huge, 'r', sizeof(huge));
    moq_bytes_t reason = { huge, sizeof(huge) };
    (void)take_actions(p.pub, NULL, 0, (moq_stream_ref_t){0}, NULL);
    for (int i = 0; i < 3; i++) {
        MOQ_TEST_CHECK_EQ_INT((int)terminal(&p, publication, sub, pubh, reason),
                              (int)MOQ_ERR_BUFFER);
    }
    MOQ_TEST_CHECK_EQ_SIZE(p.pub->action_tail - p.pub->action_head, 0);
    MOQ_TEST_CHECK(pub_resolve_handle(p.pub, pubh) >= 0);
    uint64_t rid = p.pub->publishes[pub_resolve_handle(p.pub, pubh)].request_id;
    moq_stream_ref_t rref =
        p.pub->publishes[pub_resolve_handle(p.pub, pubh)].rs.ref;
    MOQ_TEST_CHECK_EQ_INT(
        (int)terminal(&p, publication, sub, pubh, (moq_bytes_t){0}),
        (int)MOQ_OK);
    uint8_t want[64];
    size_t want_len = terminal_bytes(p.pub, rid, want, sizeof(want));
    int dones = 0;
    (void)take_actions(p.pub, want, want_len, rref, &dones);
    MOQ_TEST_CHECK_EQ_INT(dones, 1);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* A subscription terminal that cannot fit even the empty arena: a draft-18
 * session configured with a 512-byte send arena, and a 1000-byte reason
 * (within the 1024-byte limit). While other output is queued the shortfall
 * reads WOULD_BLOCK -- which does not promise a fit; once that output has
 * drained it is BUFFER, every time, with the subscription still owned and
 * nothing queued for it; the same subscription then ends with a terminal
 * that fits, sent exactly once. */
static int
case_subscription_never_fits(void)
{
    int failures = 0;
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_SERVER);
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.send_buffer_size = 512;
    moq_session_t *s = NULL;
    MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
    if (s == NULL) return failures;
    MOQ_TEST_CHECK_EQ_SIZE(s->send_cap, 512);
    MOQ_TEST_CHECK(moq_session_start(s, 0) == MOQ_OK);
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
    uint8_t buf[256];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    moq_d18_encode_setup(&w);
    MOQ_TEST_CHECK(moq_session_on_control_bytes(s, buf,
                                                moq_buf_writer_offset(&w),
                                                0) == MOQ_OK);
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
    /* The peer subscribes on a request bidi; the session accepts. */
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("arena") };
    moq_namespace_t ns = { parts, 1 };
    moq_d18_msg_params_t mp = { 0 };
    moq_buf_writer_init(&w, buf, sizeof(buf));
    moq_d18_encode_subscribe(&w, 0, &ns, MOQ_BYTES_LITERAL("t"), &mp);
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0xE001);
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(
                       s, ref, buf, moq_buf_writer_offset(&w), false, 1) ==
                   MOQ_OK);
    moq_subscription_t h = { 0 };
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) h = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t acfg;
    moq_accept_subscribe_cfg_init(&acfg);
    MOQ_TEST_CHECK(moq_session_accept_subscribe(s, h, &acfg, 1) == MOQ_OK);
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);

    static uint8_t reason[1000];
    memset(reason, 'r', sizeof(reason));
    moq_done_subscribe_cfg_t dc;
    moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x2;
    dc.reason = (moq_bytes_t){ reason, sizeof(reason) };
    uint64_t rid = s->subs[sub_resolve_handle(s, h)].request_id;
    moq_stream_ref_t rref = s->subs[sub_resolve_handle(s, h)].rs.ref;

    /* Unrelated output occupies part of the arena. */
    moq_bytes_t aparts[2] = { MOQ_BYTES_LITERAL("arena"),
                              MOQ_BYTES_LITERAL("other") };
    moq_publish_namespace_cfg_t nc;
    moq_publish_namespace_cfg_init(&nc);
    nc.track_namespace = (moq_namespace_t){ aparts, 2 };
    moq_announcement_t ann;
    MOQ_TEST_CHECK(moq_session_publish_namespace(s, &nc, 2, &ann) == MOQ_OK);
    MOQ_TEST_CHECK(s->send_len > 0);
    size_t queued = s->action_tail - s->action_head;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_done_subscribe(s, h, &dc, 2),
                          (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_SIZE(s->action_tail - s->action_head, queued);
    MOQ_TEST_CHECK(sub_resolve_handle(s, h) >= 0);
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);

    for (int i = 0; i < 3; i++) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_done_subscribe(s, h, &dc, 2),
                              (int)MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK_EQ_SIZE(s->action_tail - s->action_head, 0);
        MOQ_TEST_CHECK(sub_resolve_handle(s, h) >= 0);
    }
    dc.reason = (moq_bytes_t){ 0 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_done_subscribe(s, h, &dc, 2),
                          (int)MOQ_OK);
    uint8_t want[64];
    size_t want_len = terminal_bytes(s, rid, want, sizeof(want));
    int dones = 0;
    (void)take_actions(s, want, want_len, rref, &dones);
    MOQ_TEST_CHECK_EQ_INT(dones, 1);
    moq_session_destroy(s);
    return failures;
}

int
main(void)
{
    int failures = 0;
    const moq_version_t vs[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18,
                                 MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < sizeof(vs) / sizeof(vs[0]); i++) {
        for (int pubn = 0; pubn < 2; pubn++) {
            int f = case_short_tail(vs[i], pubn != 0);
            printf("%s: short_tail(draft %u, %s)\n", f ? "FAIL" : "PASS",
                   (unsigned)vs[i], pubn ? "publication" : "subscription");
            failures += f;
            if (pubn != 0) {
                f = case_never_fits(vs[i], true);
                printf("%s: never_fits(draft %u, publication)\n",
                       f ? "FAIL" : "PASS", (unsigned)vs[i]);
                failures += f;
            }
        }
    }
    int f = case_subscription_never_fits();
    printf("%s: never_fits(draft 18, subscription, 512-byte arena)\n",
           f ? "FAIL" : "PASS");
    failures += f;
    return failures == 0 ? 0 : 1;
}
