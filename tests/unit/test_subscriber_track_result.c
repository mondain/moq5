#include <moq/subscriber.h>
#include <stddef.h>

#ifdef __cplusplus
#define RESULT_ASSERT static_assert
#define RESULT_ALIGNOF alignof
#else
#define RESULT_ASSERT _Static_assert
#define RESULT_ALIGNOF _Alignof
#endif

typedef struct result_u64_alignment { uint64_t value; } result_u64_alignment_t;

RESULT_ASSERT(sizeof(((moq_sub_track_result_t *)0)->state) == 4, "fixed state");
RESULT_ASSERT(sizeof(((moq_sub_track_result_t *)0)->flags) == 4, "fixed flags");
RESULT_ASSERT(offsetof(moq_sub_track_result_t, flags) == 8, "flags offset");
RESULT_ASSERT(MOQ_SUB_TRACK_RESULT_V0_SIZE ==
    (sizeof(void *) == 8 ? 56u : (RESULT_ALIGNOF(result_u64_alignment_t) == 8 ? 48u : 44u)),
    "frozen result floor");
RESULT_ASSERT(sizeof(moq_sub_track_result_t) == MOQ_SUB_TRACK_RESULT_V0_SIZE,
    "complete v0 result");

#ifndef MOQ_TRACK_RESULT_LAYOUT_ONLY
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include "test_support.h"
#include <stdlib.h>
#include <string.h>

static int failures;

/* Application-range properties, including an opaque byte value. */
static const uint8_t properties[] = { 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb };
static const uint8_t properties_d16[] = { 0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb };

typedef struct ledger {
    size_t calls;
    size_t blocks;
    size_t bytes;
    bool fail;
} ledger_t;

static void *ledger_alloc(size_t n, void *ctx)
{
    ledger_t *l = ctx;
    l->calls++;
    if (l->fail) return NULL;
    void *p = malloc(n);
    if (p) { l->blocks++; l->bytes += n; }
    return p;
}

static void ledger_free(void *p, size_t n, void *ctx)
{
    ledger_t *l = ctx;
    if (p) {
        MOQ_TEST_CHECK(l->blocks > 0 && l->bytes >= n);
        l->blocks--;
        l->bytes -= n;
    }
    free(p);
}

typedef struct fixture {
    moq_simpair_t *pair;
    moq_subscriber_t *sub;
    ledger_t ledger;
    unsigned subscribed;
    unsigned errors;
    unsigned closed;
    unsigned done;
    moq_stream_ref_t last_request_stream;
    moq_sub_track_result_t callback_result;
    bool capture_control;
    unsigned control_count;
    size_t control_len;
    uint8_t control_bytes[64];
} fixture_t;

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM)
        f->last_request_stream = r->stream_ref;
    if (f->capture_control && r->kind == MOQ_SIM_TRACE_ACTION &&
        r->from == MOQ_PERSPECTIVE_SERVER && r->action_kind == MOQ_ACTION_SEND_CONTROL) {
        f->control_count++;
        f->control_len = r->bytes.len;
        MOQ_TEST_CHECK(r->bytes.len <= sizeof(f->control_bytes));
        if (r->bytes.len <= sizeof(f->control_bytes))
            memcpy(f->control_bytes, r->bytes.data, r->bytes.len);
    }
}

static void on_subscribed(void *ctx, moq_sub_track_t *track)
{
    fixture_t *f = ctx;
    f->subscribed++;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f->sub, track,
        &f->callback_result, sizeof(f->callback_result)) == MOQ_OK);
    MOQ_TEST_CHECK(f->callback_result.state == MOQ_SUB_TRACK_ACTIVE);
    MOQ_TEST_CHECK(f->callback_result.flags & MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN);
}

static void on_error(void *ctx, moq_sub_track_t *track,
                     moq_request_error_t code, moq_bytes_t reason)
{
    fixture_t *f = ctx;
    (void)track; (void)code; (void)reason;
    f->errors++;
}

static void on_closed(void *ctx, uint64_t code)
{
    fixture_t *f = ctx;
    (void)code;
    f->closed++;
}

static void on_done(void *ctx, moq_sub_track_t *track, uint64_t status)
{
    fixture_t *f = ctx;
    MOQ_TEST_CHECK(status == 0);
    MOQ_TEST_CHECK(moq_sub_track_get_state(track) == MOQ_SUB_TRACK_DONE);
    f->done++;
}

static void pump(fixture_t *f)
{
    MOQ_TEST_CHECK(moq_simpair_run_until_quiescent(f->pair, 16, NULL) == MOQ_OK);
}

static void fixture_init(fixture_t *f, moq_version_t version, size_t cap)
{
    memset(f, 0, sizeof(*f));
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = moq_alloc_default();
    pc.version = version;
    pc.trace_fn = trace;
    pc.trace_ctx = f;
    pc.client_send_request_capacity = true;
    pc.server_send_request_capacity = true;
    pc.client_initial_request_capacity = 32;
    pc.server_initial_request_capacity = 32;
    MOQ_TEST_CHECK(moq_simpair_create(&pc, &f->pair) == MOQ_OK);
    MOQ_TEST_CHECK(moq_simpair_start(f->pair) == MOQ_OK);
    pump(f);
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_client(f->pair), &ev, 1))
        moq_event_cleanup(&ev);
    while (moq_session_poll_events(moq_simpair_server(f->pair), &ev, 1))
        moq_event_cleanup(&ev);
    moq_alloc_t alloc = { .ctx = &f->ledger, .alloc = ledger_alloc,
                         .free = ledger_free };
    moq_sub_cfg_t sc;
    moq_sub_cfg_init_sized(&sc, sizeof(sc));
    sc.max_tracks = 2;
    sc.max_queued_object_bytes = cap;
    sc.callbacks.ctx = f;
    sc.callbacks.on_subscribed = on_subscribed;
    sc.callbacks.on_subscribe_error = on_error;
    sc.callbacks.on_closed = on_closed;
    sc.on_subscribe_done = on_done;
    MOQ_TEST_CHECK(moq_sub_create(moq_simpair_client(f->pair), &alloc,
                                  &sc, &f->sub) == MOQ_OK);
}

static void fixture_destroy(fixture_t *f)
{
    moq_sub_destroy(f->sub);
    moq_simpair_destroy(f->pair);
    MOQ_TEST_CHECK(f->ledger.blocks == 0 && f->ledger.bytes == 0);
}

static moq_sub_track_t *subscribe(fixture_t *f, const char *name,
                                 moq_subscription_t *peer)
{
    moq_sub_track_cfg_t cfg;
    moq_sub_track_cfg_init(&cfg);
    moq_bytes_t ns[] = { MOQ_BYTES_LITERAL("result") };
    cfg.track_namespace = (moq_namespace_t){ ns, 1 };
    cfg.track_name = moq_bytes_cstr(name);
    moq_sub_track_t *track = NULL;
    MOQ_TEST_CHECK(moq_sub_subscribe(f->sub, &cfg, 0, &track) == MOQ_OK);
    pump(f);
    *peer = MOQ_SUBSCRIPTION_INVALID;
    moq_event_t ev;
    while (moq_session_poll_events(moq_simpair_server(f->pair), &ev, 1)) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) *peer = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(*peer));
    return track;
}

static void accept(fixture_t *f, moq_subscription_t peer,
                   const moq_accept_subscribe_cfg_t *cfg)
{
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_subscribe(moq_simpair_server(f->pair),
                                                      peer, cfg, 0), MOQ_OK);
    pump(f);
}

static void release(fixture_t *f, moq_sub_track_t *track)
{
    MOQ_TEST_CHECK(moq_sub_unsubscribe(f->sub, track, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_release_track(f->sub, track) == MOQ_OK);
    pump(f);
}

static void send_object(fixture_t *f, moq_subscription_t peer)
{
    moq_subgroup_cfg_t sg;
    moq_subgroup_cfg_init(&sg);
    sg.group_id = 1;
    moq_subgroup_handle_t handle;
    moq_session_t *sv = moq_simpair_server(f->pair);
    MOQ_TEST_CHECK(moq_session_open_subgroup(sv, peer, &sg, 0, &handle) == MOQ_OK);
    moq_rcbuf_t *payload = NULL;
    MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"body", 4,
                                    &payload) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_write_object(sv, handle, 2, payload, 0) == MOQ_OK);
    moq_rcbuf_decref(payload);
    MOQ_TEST_CHECK(moq_session_close_subgroup(sv, handle, 0) == MOQ_OK);
    pump(f);
    MOQ_TEST_CHECK(moq_sub_tick(f->sub, 0) == MOQ_OK);
}

static void expect_refusal(const moq_subscriber_t *sub, const moq_sub_track_t *track,
                           size_t size, moq_result_t expected)
{
    moq_sub_track_result_t out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    MOQ_TEST_CHECK(moq_sub_get_track_result(sub, track, &out, size) == expected);
    MOQ_TEST_CHECK(memcmp(saved, &out, sizeof(out)) == 0);
}

static void test_initial_and_guards(moq_version_t version)
{
    fixture_t f, other;
    fixture_init(&f, version, 0);
    fixture_init(&other, version, 0);
    moq_subscription_t peer;
    moq_sub_track_t *t = subscribe(&f, "empty", &peer);
    moq_sub_track_result_t out;
    size_t calls = f.ledger.calls;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.struct_size == sizeof(out));
    MOQ_TEST_CHECK(out.state == MOQ_SUB_TRACK_PENDING && out.flags == 0);
    MOQ_TEST_CHECK(out.largest_group == 0 && out.largest_object == 0);
    MOQ_TEST_CHECK(out.expires_ms == 0 && out.track_properties.len == 0);
    MOQ_TEST_CHECK(out.track_properties.data == NULL && calls == f.ledger.calls);
    for (size_t n = 0; n < MOQ_SUB_TRACK_RESULT_V0_SIZE; n++)
        expect_refusal(f.sub, t, n, MOQ_ERR_INVAL);
    expect_refusal(NULL, t, sizeof(out), MOQ_ERR_INVAL);
    expect_refusal(f.sub, NULL, sizeof(out), MOQ_ERR_INVAL);
    expect_refusal(other.sub, t, sizeof(out), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, NULL, sizeof(out)) == MOQ_ERR_INVAL);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    accept(&f, peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.subscribed == 1);
    MOQ_TEST_CHECK(f.callback_result.flags == MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN);
    MOQ_TEST_CHECK(f.callback_result.track_properties.data == NULL);
    MOQ_TEST_CHECK(f.ledger.calls == calls);
    struct { moq_sub_track_result_t value; uint8_t tail[16]; } future;
    memset(&future, 0xa5, sizeof(future));
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &future.value,
                                           sizeof(future)) == MOQ_OK);
    MOQ_TEST_CHECK(future.value.struct_size == sizeof(out));
    for (size_t i = 0; i < sizeof(future.tail); i++) MOQ_TEST_CHECK(future.tail[i] == 0xa5);
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &out, SIZE_MAX) == MOQ_OK);
    release(&f, t);
    expect_refusal(f.sub, t, sizeof(out), MOQ_ERR_INVAL);
    t = subscribe(&f, "reuse", &peer);
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.flags == 0 && out.state == MOQ_SUB_TRACK_PENDING);
    fixture_destroy(&other);
    fixture_destroy(&f);
}

static void test_retention(moq_version_t version, uint64_t largest, uint64_t expiry)
{
    fixture_t f;
    fixture_init(&f, version, 0);
    moq_subscription_t peer;
    moq_sub_track_t *t = subscribe(&f, "retained", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true;
    ac.largest_group = largest;
    ac.largest_object = largest;
    ac.has_expires = true;
    ac.expires_ms = expiry;
    moq_bytes_t props = version == MOQ_VERSION_DRAFT_16 ?
        (moq_bytes_t){ properties_d16, sizeof(properties_d16) } :
        (moq_bytes_t){ properties, sizeof(properties) };
    ac.track_properties = props;
    accept(&f, peer, &ac);
    size_t blocks = f.ledger.blocks, bytes = f.ledger.bytes, allocation = 0;
    MOQ_TEST_CHECK(moq_rcbuf_allocation_size(props.len, &allocation) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.subscribed == 1 && f.errors == 0);
    MOQ_TEST_CHECK(f.ledger.blocks == blocks + 1 && f.ledger.bytes == bytes + allocation);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, t) == MOQ_ERR_WRONG_STATE);
    MOQ_TEST_CHECK(f.ledger.blocks == blocks + 1 && f.ledger.bytes == bytes + allocation);
    moq_sub_track_result_t before = f.callback_result, after;
    uint32_t expected_flags = MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN |
        MOQ_SUB_TRACK_RESULT_HAS_LARGEST;
    if (version != MOQ_VERSION_DRAFT_16 || expiry != 0)
        expected_flags |= MOQ_SUB_TRACK_RESULT_HAS_EXPIRES;
    MOQ_TEST_CHECK(before.flags == expected_flags);
    MOQ_TEST_CHECK(before.largest_group == largest && before.largest_object == largest);
    MOQ_TEST_CHECK(before.expires_ms == expiry);
    MOQ_TEST_CHECK(before.track_properties.len == props.len);
    if (!before.track_properties.data) { fixture_destroy(&f); return; }
    MOQ_TEST_CHECK(memcmp(before.track_properties.data, props.data, props.len) == 0);
    moq_sub_stats_t stats;
    MOQ_TEST_CHECK(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    MOQ_TEST_CHECK(stats.bytes_queued == 0 && stats.subscribe_ok == 1);

    /* Ordinary later work changes the session borrow epoch and live Largest. */
    if (largest == 0) {
        send_object(&f, peer);
        moq_sub_object_t obj;
        MOQ_TEST_CHECK(moq_sub_poll_object(f.sub, &obj) == MOQ_OK);
        MOQ_TEST_CHECK(obj.group_id == 1 && obj.object_id == 2);
        moq_sub_object_cleanup(&obj);
    }
    moq_subscription_t second_peer;
    moq_sub_track_t *second = subscribe(&f, "second", &second_peer);
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ props.data,
        version == MOQ_VERSION_DRAFT_16 ? 3u : 2u };
    accept(&f, second_peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    size_t calls = f.ledger.calls;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &after, sizeof(after)) == MOQ_OK);
    MOQ_TEST_CHECK(f.ledger.calls == calls);
    MOQ_TEST_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    MOQ_TEST_CHECK(memcmp(before.track_properties.data, props.data, props.len) == 0);
    release(&f, second);
    MOQ_TEST_CHECK(f.ledger.blocks == blocks + 1 && f.ledger.bytes == bytes + allocation);
    MOQ_TEST_CHECK(moq_sub_unsubscribe(f.sub, t, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &after, sizeof(after)) == MOQ_OK);
    MOQ_TEST_CHECK(after.state == MOQ_SUB_TRACK_DONE && after.flags == before.flags);
    MOQ_TEST_CHECK(after.track_properties.data == before.track_properties.data);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, t) == MOQ_OK);
    MOQ_TEST_CHECK(f.ledger.blocks == blocks && f.ledger.bytes == bytes);
    fixture_destroy(&f);
}

static void test_d16_ack_parameter_order(void)
{
    /* Draft-16 sections 1.4.2 and 9.10: request 0, alias 3, counted
     * parameters, then properties. EXPIRES (8) must precede LARGEST (9),
     * whose delta is 1. These literals are independent of our encoder. */
    static const uint8_t neither[] = {
        0x04, 0x00, 0x0b, 0x00, 0x03, 0x00,
        0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    static const uint8_t expires_only[] = {
        0x04, 0x00, 0x0d, 0x00, 0x03, 0x01, 0x08, 0x11,
        0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    static const uint8_t largest_only[] = {
        0x04, 0x00, 0x0f, 0x00, 0x03, 0x01, 0x09, 0x02, 0x00, 0x00,
        0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    static const uint8_t both[] = {
        0x04, 0x00, 0x11, 0x00, 0x03, 0x02, 0x08, 0x11, 0x01, 0x02, 0x00, 0x00,
        0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    static const uint8_t full_width[] = {
        0x04, 0x00, 0x26, 0x00, 0x03, 0x02, 0x08,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x01, 0x10,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    const struct {
        const char *name;
        bool has_largest, has_expires;
        uint64_t largest, expiry;
        moq_bytes_t wire;
    } cases[] = {
        { "neither", false, false, 0, 0, { neither, sizeof(neither) } },
        { "expires-only", false, true, 0, 17, { expires_only, sizeof(expires_only) } },
        { "largest-only", true, false, 0, 0, { largest_only, sizeof(largest_only) } },
        { "both", true, true, 0, 17, { both, sizeof(both) } },
        { "zero-expiry-omitted", true, true, 0, 0,
          { largest_only, sizeof(largest_only) } },
        { "full-width", true, true, UINT64_C(0x3fffffffffffffff),
          UINT64_C(0x3fffffffffffffff), { full_width, sizeof(full_width) } }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fprintf(stderr, "D16 ACK literal: %s\n", cases[i].name);
        fixture_t f;
        fixture_init(&f, MOQ_VERSION_DRAFT_16, 0);
        moq_subscription_t peer;
        moq_sub_track_t *track = subscribe(&f, cases[i].name, &peer);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        ac.has_track_alias = true;
        ac.track_alias = 3;
        ac.has_largest = cases[i].has_largest;
        ac.largest_group = cases[i].largest;
        ac.largest_object = cases[i].largest;
        ac.has_expires = cases[i].has_expires;
        ac.expires_ms = cases[i].expiry;
        ac.track_properties = (moq_bytes_t){ properties_d16, sizeof(properties_d16) };
        f.capture_control = true;
        moq_result_t rc = moq_session_accept_subscribe(moq_simpair_server(f.pair),
                                                      peer, &ac, 0);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) { fixture_destroy(&f); continue; }
        pump(&f);
        f.capture_control = false;
        MOQ_TEST_CHECK(f.control_count == 1);
        MOQ_TEST_CHECK(f.control_len == cases[i].wire.len);
        if (f.control_len == cases[i].wire.len)
            MOQ_TEST_CHECK(memcmp(f.control_bytes, cases[i].wire.data,
                                  cases[i].wire.len) == 0);
        MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
        MOQ_TEST_CHECK(f.subscribed == 1 && f.errors == 0 && f.closed == 0);
        moq_sub_track_result_t out;
        MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, track, &out, sizeof(out)) == MOQ_OK);
        uint32_t flags = MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN;
        if (cases[i].has_largest) flags |= MOQ_SUB_TRACK_RESULT_HAS_LARGEST;
        if (cases[i].has_expires && cases[i].expiry != 0)
            flags |= MOQ_SUB_TRACK_RESULT_HAS_EXPIRES;
        MOQ_TEST_CHECK(out.flags == flags);
        MOQ_TEST_CHECK(out.largest_group == cases[i].largest &&
                       out.largest_object == cases[i].largest);
        MOQ_TEST_CHECK(out.expires_ms == cases[i].expiry);
        MOQ_TEST_CHECK(out.track_properties.len == sizeof(properties_d16));
        if (out.track_properties.data)
            MOQ_TEST_CHECK(memcmp(out.track_properties.data, properties_d16,
                                  sizeof(properties_d16)) == 0);
        release(&f, track);
        fixture_destroy(&f);
    }
}

static void test_terminal_before_tick(bool rejected)
{
    fixture_t f;
    fixture_init(&f, MOQ_VERSION_DRAFT_22, 0);
    moq_subscription_t peer;
    moq_sub_track_t *t = subscribe(&f, "terminal", &peer);
    if (rejected) {
        moq_reject_subscribe_cfg_t cfg;
        moq_reject_subscribe_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        MOQ_TEST_CHECK(moq_session_reject_subscribe(moq_simpair_server(f.pair),
                                                   peer, &cfg, 0) == MOQ_OK);
        pump(&f);
    } else {
        moq_accept_subscribe_cfg_t cfg;
        moq_accept_subscribe_cfg_init(&cfg);
        cfg.track_properties = (moq_bytes_t){ properties, sizeof(properties) };
        accept(&f, peer, &cfg);
        MOQ_TEST_CHECK(moq_sub_unsubscribe(f.sub, t, 0) == MOQ_OK);
    }
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    moq_sub_track_result_t out;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.state == (rejected ? MOQ_SUB_TRACK_ERROR : MOQ_SUB_TRACK_DONE));
    MOQ_TEST_CHECK(out.flags == (rejected ? 0u : MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN));
    MOQ_TEST_CHECK(out.track_properties.len == (rejected ? 0u : sizeof(properties)));
    MOQ_TEST_CHECK(f.subscribed == 0 && f.errors == (rejected ? 1u : 0u));
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, t) == MOQ_OK);
    fixture_destroy(&f);
}

static void test_object_and_ack_credit(bool exact_fit)
{
    fixture_t f;
    fixture_init(&f, MOQ_VERSION_DRAFT_22, 2 * sizeof(properties) + (exact_fit ? 4 : 3));
    moq_subscription_t peer;
    moq_sub_track_t *first = subscribe(&f, "credit-first", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ properties, sizeof(properties) };
    accept(&f, peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    send_object(&f, peer);
    moq_sub_track_t *second = subscribe(&f, "credit-second", &peer);
    accept(&f, peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == (exact_fit ? MOQ_OK : MOQ_ERR_BUFFER));
    moq_sub_stats_t stats;
    MOQ_TEST_CHECK(moq_sub_get_stats(f.sub, &stats, sizeof(stats)) == MOQ_OK);
    MOQ_TEST_CHECK(stats.bytes_queued == 4 && stats.objects_queued == 1);
    moq_sub_object_t obj;
    MOQ_TEST_CHECK(moq_sub_poll_object(f.sub, &obj) == MOQ_OK);
    MOQ_TEST_CHECK(obj.track == first);
    moq_sub_object_cleanup(&obj);
    if (!exact_fit) {
        expect_refusal(f.sub, second, sizeof(moq_sub_track_result_t), MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK(moq_sub_unsubscribe(f.sub, second, 0) == MOQ_OK);
        MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
        expect_refusal(f.sub, second, sizeof(moq_sub_track_result_t), MOQ_ERR_BUFFER);
        MOQ_TEST_CHECK(moq_sub_release_track(f.sub, second) == MOQ_OK);
    }
    fixture_destroy(&f);
}

static void test_full_width_wire_ack(void)
{
    fixture_t f;
    fixture_init(&f, MOQ_VERSION_DRAFT_22, 0);
    moq_subscription_t peer;
    moq_sub_track_t *t = subscribe(&f, "full-width", &peer);
    /* D22 9.7/9.20: alias1, two delta-typed parameters (EXPIRES8,
     * LARGEST_OBJECT9), then application properties120/121. Each maximum
     * vi64 is nine 0xff bytes. The public sender's separate expiry ceiling
     * must not limit this inbound observation test. */
    static const uint8_t bytes[] = {
        0x04, 0x00, 0x26, 0x01, 0x02, 0x08,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x01,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
    };
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(moq_simpair_client(f.pair),
        f.last_request_stream, bytes, sizeof(bytes), false, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    moq_sub_track_result_t out;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, t, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.flags == (MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN |
        MOQ_SUB_TRACK_RESULT_HAS_LARGEST | MOQ_SUB_TRACK_RESULT_HAS_EXPIRES));
    MOQ_TEST_CHECK(out.largest_group == UINT64_MAX && out.largest_object == UINT64_MAX);
    MOQ_TEST_CHECK(out.expires_ms == UINT64_MAX);
    MOQ_TEST_CHECK(out.track_properties.len == sizeof(properties));
    if (out.track_properties.data)
        MOQ_TEST_CHECK(memcmp(out.track_properties.data, properties, sizeof(properties)) == 0);
    fixture_destroy(&f);
}

static void test_retention_failure(bool allocation_failure)
{
    fixture_t f;
    fixture_init(&f, MOQ_VERSION_DRAFT_22, sizeof(properties));
    moq_subscription_t peer;
    moq_sub_track_t *first = subscribe(&f, "first", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ properties, sizeof(properties) };
    accept(&f, peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    size_t blocks = f.ledger.blocks, bytes = f.ledger.bytes;
    if (allocation_failure) release(&f, first);
    moq_sub_track_t *failed = subscribe(&f, "failure", &peer);
    accept(&f, peer, &ac);
    f.ledger.fail = allocation_failure;
    moq_result_t expected = allocation_failure ? MOQ_ERR_NOMEM : MOQ_ERR_BUFFER;
    size_t calls = f.ledger.calls;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == expected);
    MOQ_TEST_CHECK(f.ledger.calls == calls + (allocation_failure ? 1u : 0u));
    MOQ_TEST_CHECK(moq_sub_track_get_state(failed) == MOQ_SUB_TRACK_ACTIVE);
    expect_refusal(f.sub, failed, sizeof(moq_sub_track_result_t), expected);
    f.ledger.fail = false;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == expected);
    MOQ_TEST_CHECK(f.subscribed == 1 && f.errors == 0 && f.closed == 0);
    if (!allocation_failure)
        MOQ_TEST_CHECK(f.ledger.blocks == blocks && f.ledger.bytes == bytes);
    release(&f, failed);
    if (!allocation_failure) release(&f, first);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    /* The session and exact ACK credit both remain usable after release. */
    moq_sub_track_t *again = subscribe(&f, "again", &peer);
    accept(&f, peer, &ac);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.subscribed == 2);
    (void)again;
    fixture_destroy(&f);
}

static void queue_terminal(fixture_t *f, moq_subscription_t peer, bool closed)
{
    if (closed) {
        MOQ_TEST_CHECK(moq_session_on_transport_close(
            moq_simpair_client(f->pair), 0, 0) == MOQ_OK);
    } else {
        moq_done_subscribe_cfg_t dc;
        moq_done_subscribe_cfg_init(&dc);
        MOQ_TEST_CHECK(moq_session_done_subscribe(moq_simpair_server(f->pair),
                                                  peer, &dc, 0) == MOQ_OK);
        pump(f);
    }
}

static void test_retention_terminal(moq_version_t version, bool fail_copy,
                                    bool closed, bool already_queued)
{
    fprintf(stderr, "terminal version=%d copy_failure=%d close=%d queued=%d\n",
            (int)version, fail_copy, closed, already_queued);
    fixture_t f;
    fixture_init(&f, version, 0);
    moq_subscription_t peer;
    moq_sub_track_t *track = subscribe(&f, "terminal-copy", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = version == MOQ_VERSION_DRAFT_16 ?
        (moq_bytes_t){ properties_d16, sizeof(properties_d16) } :
        (moq_bytes_t){ properties, sizeof(properties) };
    accept(&f, peer, &ac);
    if (already_queued) queue_terminal(&f, peer, closed);
    f.ledger.fail = fail_copy;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == (fail_copy ? MOQ_ERR_NOMEM : MOQ_OK));
    f.ledger.fail = false;
    MOQ_TEST_CHECK(f.subscribed == (fail_copy ? 0u : 1u) && f.errors == 0);
    if (!already_queued) queue_terminal(&f, peer, closed);
    size_t calls = f.ledger.calls;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_track_get_state(track) == MOQ_SUB_TRACK_DONE);
    MOQ_TEST_CHECK(f.closed == (closed ? 1u : 0u));
    MOQ_TEST_CHECK(f.done == (closed ? 0u : 1u));
    if (fail_copy) {
        expect_refusal(f.sub, track, sizeof(moq_sub_track_result_t), MOQ_ERR_NOMEM);
    } else {
        moq_sub_track_result_t out;
        MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, track, &out, sizeof(out)) == MOQ_OK);
        MOQ_TEST_CHECK(out.state == MOQ_SUB_TRACK_DONE);
        MOQ_TEST_CHECK(out.track_properties.len == ac.track_properties.len);
        if (out.track_properties.data)
            MOQ_TEST_CHECK(memcmp(out.track_properties.data, ac.track_properties.data,
                                  ac.track_properties.len) == 0);
    }
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.closed == (closed ? 1u : 0u));
    MOQ_TEST_CHECK(f.done == (closed ? 0u : 1u));
    MOQ_TEST_CHECK(f.ledger.calls == calls);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, track) == MOQ_OK);
    expect_refusal(f.sub, track, sizeof(moq_sub_track_result_t), MOQ_ERR_INVAL);
    fixture_destroy(&f);
}

static void test_failed_track_allows_other_progress(moq_version_t version)
{
    fprintf(stderr, "unrelated progress version=%d\n", (int)version);
    fixture_t f;
    fixture_init(&f, version, 0);
    moq_subscription_t peer, other_peer;
    moq_sub_track_t *failed = subscribe(&f, "failed", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = version == MOQ_VERSION_DRAFT_16 ?
        (moq_bytes_t){ properties_d16, sizeof(properties_d16) } :
        (moq_bytes_t){ properties, sizeof(properties) };
    accept(&f, peer, &ac);
    f.ledger.fail = true;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_ERR_NOMEM);
    f.ledger.fail = false;
    moq_sub_track_t *other = subscribe(&f, "healthy", &other_peer);
    moq_accept_subscribe_cfg_init(&ac);
    accept(&f, other_peer, &ac);
    size_t calls = f.ledger.calls;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_ERR_NOMEM);
    MOQ_TEST_CHECK(f.subscribed == 1 && f.errors == 0);
    MOQ_TEST_CHECK(moq_sub_track_get_state(other) == MOQ_SUB_TRACK_ACTIVE);
    moq_sub_track_result_t out;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, other, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.flags == MOQ_SUB_TRACK_RESULT_RESPONSE_SEEN);
    expect_refusal(f.sub, failed, sizeof(out), MOQ_ERR_NOMEM);
    queue_terminal(&f, other_peer, false);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_ERR_NOMEM);
    MOQ_TEST_CHECK(f.done == 1);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, other) == MOQ_OK);
    queue_terminal(&f, peer, false);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.done == 2 && f.closed == 0);
    expect_refusal(f.sub, failed, sizeof(out), MOQ_ERR_NOMEM);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, failed) == MOQ_OK);
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.done == 2 && f.subscribed == 1);
    MOQ_TEST_CHECK(f.ledger.calls == calls);
    fixture_destroy(&f);
}

static void test_close_discards_unobserved_ack(moq_version_t version)
{
    fprintf(stderr, "close before ACK observation version=%d\n", (int)version);
    fixture_t f;
    fixture_init(&f, version, 0);
    moq_subscription_t peer;
    moq_sub_track_t *track = subscribe(&f, "unobserved", &peer);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = version == MOQ_VERSION_DRAFT_16 ?
        (moq_bytes_t){ properties_d16, sizeof(properties_d16) } :
        (moq_bytes_t){ properties, sizeof(properties) };
    accept(&f, peer, &ac);
    /* Core transport close discards the unobserved ACK before queuing close.
     * This is a no-copy control, not a retention-failure recovery schedule. */
    queue_terminal(&f, peer, true);
    size_t calls = f.ledger.calls;
    f.ledger.fail = true;
    MOQ_TEST_CHECK(moq_sub_tick(f.sub, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.ledger.calls == calls);
    MOQ_TEST_CHECK(f.subscribed == 0 && f.errors == 0 && f.done == 0 && f.closed == 1);
    moq_sub_track_result_t out;
    MOQ_TEST_CHECK(moq_sub_get_track_result(f.sub, track, &out, sizeof(out)) == MOQ_OK);
    MOQ_TEST_CHECK(out.state == MOQ_SUB_TRACK_DONE && out.flags == 0);
    MOQ_TEST_CHECK(out.track_properties.data == NULL && out.track_properties.len == 0);
    MOQ_TEST_CHECK(moq_sub_release_track(f.sub, track) == MOQ_OK);
    fixture_destroy(&f);
}

int main(void)
{
    test_initial_and_guards(MOQ_VERSION_DRAFT_16);
    test_initial_and_guards(MOQ_VERSION_DRAFT_18);
    test_initial_and_guards(MOQ_VERSION_DRAFT_22);
    test_retention(MOQ_VERSION_DRAFT_16, 0, 0);
    test_retention(MOQ_VERSION_DRAFT_18, 0, 0);
    test_retention(MOQ_VERSION_DRAFT_22, 0, 0);
    test_retention(MOQ_VERSION_DRAFT_22, UINT64_MAX, 1);
    test_d16_ack_parameter_order();
    test_full_width_wire_ack();
    test_terminal_before_tick(false);
    test_terminal_before_tick(true);
    test_object_and_ack_credit(false);
    test_object_and_ack_credit(true);
    test_retention_failure(false);
    test_retention_failure(true);
    const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22
    };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        for (unsigned copy = 0; copy < 2; copy++)
            for (unsigned closed = 0; closed < 2; closed++)
                for (unsigned queued = 0; queued < (closed ? 1u : 2u); queued++)
                    test_retention_terminal(versions[i], copy != 0,
                                            closed != 0, queued != 0);
        test_failed_track_allows_other_progress(versions[i]);
        test_close_discards_unobserved_ack(versions[i]);
    }
    MOQ_TEST_PASS("subscriber initial ACK result");
    return failures != 0;
}
#endif
