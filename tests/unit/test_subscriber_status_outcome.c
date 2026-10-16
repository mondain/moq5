/* Public native ingress, one facade ring, no fabricated session events. */
#include <moq/subscriber.h>
#include <moq/sim.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, setups, cases;
static const char *case_name;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #x); ++failures; \
} } while (0)
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "SETUP_FAIL %s:%d: %s\n", case_name, __LINE__, #x); \
    ++setups; return false; \
} } while (0)

typedef struct fixture {
    moq_simpair_t *pair;
    moq_session_t *client, *server;
    moq_subscriber_t *sub;
    moq_stream_ref_t stream;
    size_t allocations, blocks, bytes;
    bool refuse_allocations;
} fixture_t;

typedef struct request {
    moq_sub_status_req_t *token;
    moq_track_status_handle_t peer;
    moq_stream_ref_t stream;
} request_t;

static void *allocate(size_t size, void *ctx)
{
    fixture_t *f = ctx;
    ++f->allocations;
    if (f->refuse_allocations) return NULL;
    void *p = malloc(size);
    if (p) { ++f->blocks; f->bytes += size; }
    return p;
}

static void deallocate(void *p, size_t size, void *ctx)
{
    fixture_t *f = ctx;
    if (p) {
        CHECK(f->blocks > 0 && f->bytes >= size);
        --f->blocks;
        f->bytes -= size;
    }
    free(p);
}

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM)
        f->stream = r->stream_ref;
}

static bool drain(moq_session_t *session)
{
    for (unsigned i = 0; i < 128; ++i) {
        moq_event_t e;
        size_t count = 0;
        REQUIRE(moq_session_poll_events_ex(session, &e, 1, sizeof(e), &count) == MOQ_OK);
        if (!count) return true;
        moq_event_cleanup(&e);
    }
    REQUIRE(false);
}

static bool start(fixture_t *f, moq_version_t version, uint32_t event_capacity)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.initial_now_us = 1;
    cfg.max_events = event_capacity;
    cfg.client_send_request_capacity = true;
    cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = 128;
    cfg.server_initial_request_capacity = 128;
    cfg.trace_fn = trace;
    cfg.trace_ctx = f;
    REQUIRE(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    f->client = moq_simpair_client(f->pair);
    f->server = moq_simpair_server(f->pair);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    REQUIRE(moq_session_state(f->client) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(f->server) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_version(f->client) == version);
    REQUIRE(drain(f->client) && drain(f->server));
    moq_alloc_t alloc = {.ctx = f, .alloc = allocate, .free = deallocate};
    moq_sub_cfg_t sub_cfg;
    moq_sub_cfg_init_sized(&sub_cfg, sizeof(sub_cfg));
    REQUIRE(moq_sub_create(f->client, &alloc, &sub_cfg, &f->sub) == MOQ_OK);
    return true;
}

static void finish(fixture_t *f)
{
    moq_sub_destroy(f->sub);
    CHECK(f->blocks == 0 && f->bytes == 0);
    if (f->pair) {
        (void)drain(f->client);
        (void)drain(f->server);
    }
    moq_simpair_destroy(f->pair);
}

static moq_result_t request_only(fixture_t *f, moq_sub_status_req_t **token)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("status");
    moq_sub_status_cfg_t cfg;
    moq_sub_status_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("video");
    return moq_sub_track_status(f->sub, &cfg, 1, token);
}

static bool issue(fixture_t *f, request_t *r)
{
    REQUIRE(request_only(f, &r->token) == MOQ_OK && r->token != NULL);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_event_t e;
    size_t count = 0;
    REQUIRE(moq_session_poll_events_ex(f->server, &e, 1, sizeof(e), &count) == MOQ_OK);
    REQUIRE(count == 1);
    bool is_request = e.kind == MOQ_EVENT_TRACK_STATUS_REQUEST;
    if (is_request) r->peer = e.u.track_status_request.handle;
    moq_event_cleanup(&e);
    REQUIRE(is_request);
    r->stream = f->stream;
    REQUIRE(moq_session_poll_events_ex(f->server, &e, 1, sizeof(e), &count) == MOQ_OK);
    if (count) moq_event_cleanup(&e);
    REQUIRE(count == 0);
    return true;
}

/* Presence 0 = absent, 1 = present zero, 2 = present nonzero. D22 has no
 * EXPIRES parameter; legacy profiles retain their native encoding rules. */
static bool respond(fixture_t *f, request_t r, bool error, unsigned presence)
{
    if (error) {
        moq_reject_track_status_cfg_t cfg;
        moq_reject_track_status_cfg_init(&cfg);
        cfg.error_code = presence == 0 && moq_session_version(f->server) != MOQ_VERSION_DRAFT_16
                       ? UINT64_MAX : MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        cfg.can_retry = presence != 0;
        cfg.retry_after_ms = presence == 2 ? 17 : 0;
        cfg.reason = MOQ_BYTES_LITERAL("gone");
        REQUIRE(moq_session_reject_track_status(f->server, r.peer, &cfg, 1) == MOQ_OK);
    } else {
        moq_accept_track_status_cfg_t cfg;
        moq_accept_track_status_cfg_init(&cfg);
        cfg.has_largest = presence != 0;
        cfg.largest_group = presence == 2 ? 7 : 0;
        cfg.largest_object = presence == 2 ? 9 : 0;
        cfg.has_expires = presence != 0 && moq_session_version(f->server) != MOQ_VERSION_DRAFT_22;
        cfg.expires_ms = presence == 2 ? 17 : 0;
        REQUIRE(moq_session_accept_track_status(f->server, r.peer, &cfg, 1) == MOQ_OK);
    }
    return true;
}

static bool bytes_equal(const void *p, size_t n, unsigned char value)
{
    const unsigned char *bytes = p;
    for (size_t i = 0; i < n; ++i) if (bytes[i] != value) return false;
    return true;
}

static void expect_empty(fixture_t *f)
{
    moq_sub_status_outcome_t out;
    memset(&out, 0xa5, sizeof(out));
    size_t calls = f->allocations;
    CHECK(moq_sub_poll_status_outcome(f->sub, &out, sizeof(out)) == MOQ_DONE);
    CHECK(bytes_equal(&out, sizeof(out), 0xa5));
    moq_sub_status_result_t old;
    memset(&old, 0xa5, sizeof(old));
    CHECK(moq_sub_poll_status(f->sub, &old) == MOQ_DONE);
    CHECK(bytes_equal(&old, sizeof(old), 0xa5));
    CHECK(f->allocations == calls);
}

static moq_sub_status_outcome_t expected(request_t r, moq_version_t version,
                                        bool error, unsigned presence)
{
    moq_sub_status_outcome_t out = {0};
    out.struct_size = sizeof(out);
    out.kind = error ? MOQ_SUB_STATUS_OUTCOME_ERROR : MOQ_SUB_STATUS_OUTCOME_OK;
    out.request = r.token;
    if (error) {
        out.error_code = presence == 0 && version != MOQ_VERSION_DRAFT_16
                       ? MOQ_REQUEST_ERROR_INTERNAL_ERROR : MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        if (presence) out.flags = MOQ_SUB_STATUS_OUTCOME_CAN_RETRY;
        out.retry_after_ms = presence == 2 ? 17 : 0;
    } else if (presence) {
        out.flags = MOQ_SUB_STATUS_OUTCOME_HAS_LARGEST;
        out.largest_group = presence == 2 ? 7 : 0;
        out.largest_object = presence == 2 ? 9 : 0;
        if (version != MOQ_VERSION_DRAFT_22 &&
            (version != MOQ_VERSION_DRAFT_16 || presence == 2)) {
            out.flags |= MOQ_SUB_STATUS_OUTCOME_HAS_EXPIRES;
            out.expires_ms = presence == 2 ? 17 : 0;
        }
    }
    return out;
}

static void compare(moq_sub_status_outcome_t got, moq_sub_status_outcome_t want)
{
#define FIELD(field) CHECK(got.field == want.field)
    FIELD(struct_size); FIELD(kind); FIELD(request); FIELD(flags); FIELD(reserved);
    FIELD(largest_group); FIELD(largest_object); FIELD(expires_ms);
    FIELD(error_code); FIELD(retry_after_ms); FIELD(cause); FIELD(local_role);
    FIELD(stream_error_code);
#undef FIELD
}

static void consume(fixture_t *f, moq_sub_status_outcome_t want)
{
    moq_sub_status_outcome_t out;
    memset(&out, 0xa5, sizeof(out));
    size_t calls = f->allocations;
    CHECK(moq_sub_poll_status_outcome(f->sub, &out, sizeof(out)) == MOQ_OK);
    compare(out, want);
    CHECK(f->allocations == calls);
}

static bool terminal_case(fixture_t *f, unsigned variant)
{
    REQUIRE(start(f, MOQ_VERSION_DRAFT_22, 0));
    request_t r;
    REQUIRE(issue(f, &r));
    expect_empty(f);
    const uint64_t code = (variant & 1u) ? UINT64_MAX : 0;
    uint32_t cause;
    if (variant < 4) {
        bool reset = variant < 2;
        cause = reset ? MOQ_TRACK_STATUS_PEER_RESET_STREAM : MOQ_TRACK_STATUS_PEER_STOP_SENDING;
        CHECK((reset ? moq_session_on_bidi_stream_reset(f->client, r.stream, code, 1)
                     : moq_session_on_bidi_stream_stop(f->client, r.stream, code, 1)) == MOQ_OK);
    } else {
        cause = variant == 4 ? MOQ_TRACK_STATUS_MISSING_RESPONSE : MOQ_TRACK_STATUS_TRUNCATED_RESPONSE;
        if (variant == 4) {
            CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, NULL, 0, true, 1) == MOQ_OK);
        } else {
            REQUIRE(respond(f, r, false, 0));
            moq_action_t a;
            size_t count = 0;
            REQUIRE(moq_session_poll_actions_ex(f->server, &a, 1, sizeof(a), &count) == MOQ_OK);
            REQUIRE(count == 1 && a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
            CHECK(a.u.send_bidi_stream.len > 1);
            CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream,
                a.u.send_bidi_stream.data, 1, true, 1) == MOQ_OK);
            moq_action_cleanup(&a);
        }
    }
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    moq_sub_status_result_t old;
    memset(&old, 0xa5, sizeof(old));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(moq_sub_poll_status(f->sub, &old) == MOQ_ERR_UNSUPPORTED);
        CHECK(bytes_equal(&old, sizeof(old), 0xa5));
    }
    moq_sub_status_outcome_t want = {0};
    want.struct_size = sizeof(want);
    want.kind = MOQ_SUB_STATUS_OUTCOME_TERMINATED;
    want.request = r.token;
    want.cause = cause;
    want.local_role = MOQ_TRACK_STATUS_REQUESTER;
    want.stream_error_code = variant < 4 ? code : 0;
    consume(f, want);
    if (variant == 2 || variant == 3)
        CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, NULL, 0, true, 1) == MOQ_OK);
    CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_OK);
    CHECK(moq_session_on_bidi_stream_stop(f->client, r.stream, 0, 1) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    expect_empty(f);
    CHECK(moq_session_state(f->client) == MOQ_SESS_ESTABLISHED);
    return true;
}

static bool response_case(fixture_t *f, unsigned variant)
{
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_22, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_16};
    moq_version_t version = versions[variant / 12];
    bool legacy = (variant % 12) >= 6;
    bool error = (variant % 6) >= 3;
    unsigned presence = variant % 3;
    REQUIRE(start(f, version, 0));
    request_t r;
    REQUIRE(issue(f, &r));
    REQUIRE(respond(f, r, error, presence));
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    moq_sub_status_outcome_t want = expected(r, version, error, presence);
    if (legacy) {
        moq_sub_status_result_t old;
        memset(&old, 0xa5, sizeof(old));
        CHECK(moq_sub_poll_status(f->sub, &old) == MOQ_OK);
        CHECK(old.kind == (error ? MOQ_SUB_STATUS_ERROR : MOQ_SUB_STATUS_OK));
        CHECK(old.request == want.request);
        CHECK(old.has_largest == ((want.flags & MOQ_SUB_STATUS_OUTCOME_HAS_LARGEST) != 0));
        CHECK(old.largest_group == want.largest_group && old.largest_object == want.largest_object);
        CHECK(old.has_expires == ((want.flags & MOQ_SUB_STATUS_OUTCOME_HAS_EXPIRES) != 0));
        CHECK(old.expires_ms == want.expires_ms && old.error_code == want.error_code);
        CHECK(old.can_retry == ((want.flags & MOQ_SUB_STATUS_OUTCOME_CAN_RETRY) != 0));
        CHECK(old.retry_after_ms == want.retry_after_ms);
    } else consume(f, want);
    if (version != MOQ_VERSION_DRAFT_16) {
        CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_OK);
        CHECK(moq_session_on_bidi_stream_stop(f->client, r.stream, 0, 1) == MOQ_OK);
    }
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    expect_empty(f);
    request_t next;
    REQUIRE(issue(f, &next));
    CHECK(next.token == r.token);
    if (version != MOQ_VERSION_DRAFT_16) {
        CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_OK);
        CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
        expect_empty(f);
    }
    REQUIRE(respond(f, next, false, 0));
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    consume(f, expected(next, version, false, 0));
    expect_empty(f);
    return true;
}

static bool response_before_fin_case(fixture_t *f, unsigned variant)
{
    REQUIRE(start(f, MOQ_VERSION_DRAFT_22, 0));
    request_t r;
    REQUIRE(issue(f, &r));
    bool error = (variant & 1u) != 0;
    REQUIRE(respond(f, r, error, 2));
    moq_action_t a;
    size_t count = 0;
    REQUIRE(moq_session_poll_actions_ex(f->server, &a, 1, sizeof(a), &count) == MOQ_OK);
    REQUIRE(count == 1 && a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
    /* Split the real response bytes from its FIN, as a transport can. */
    CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, a.u.send_bidi_stream.data,
        a.u.send_bidi_stream.len, false, 1) == MOQ_OK);
    moq_action_cleanup(&a);
    if (variant < 2)
        CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, NULL, 0, true, 1) == MOQ_OK);
    else
        CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    consume(f, expected(r, MOQ_VERSION_DRAFT_22, error, 2));
    expect_empty(f);
    CHECK(moq_session_state(f->client) == MOQ_SESS_ESTABLISHED);
    return true;
}

static bool output_case(fixture_t *f, unsigned unused)
{
    (void)unused;
    REQUIRE(start(f, MOQ_VERSION_DRAFT_22, 0));
    request_t r;
    REQUIRE(issue(f, &r));
    CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    const size_t floor = MOQ_SUB_STATUS_OUTCOME_V0_SIZE;
    const size_t calls = f->allocations;
    f->refuse_allocations = true;
    moq_sub_status_outcome_t out;
    memset(&out, 0xa5, sizeof(out));
    CHECK(moq_sub_poll_status_outcome(NULL, &out, sizeof(out)) == MOQ_ERR_INVAL);
    CHECK(bytes_equal(&out, sizeof(out), 0xa5));
    CHECK(moq_sub_poll_status_outcome(f->sub, NULL, sizeof(out)) == MOQ_ERR_INVAL);
    CHECK(moq_sub_poll_status(NULL, (moq_sub_status_result_t *)&out) == MOQ_ERR_INVAL);
    CHECK(moq_sub_poll_status(f->sub, NULL) == MOQ_ERR_INVAL);
    const size_t sizes[] = {0, 1, sizeof(uint32_t), floor - 1};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t physical = sizes[i] ? sizes[i] : 1;
        void *short_out = malloc(physical);
        REQUIRE(short_out != NULL);
        memset(short_out, 0xa5, physical);
        CHECK(moq_sub_poll_status_outcome(f->sub, short_out, sizes[i]) == MOQ_ERR_INVAL);
        CHECK(bytes_equal(short_out, physical, 0xa5));
        free(short_out);
    }
    void *exact = malloc(floor);
    REQUIRE(exact != NULL);
    memset(exact, 0xa5, floor);
    CHECK(moq_sub_poll_status_outcome(f->sub, exact, floor) == MOQ_OK);
    memset(&out, 0, sizeof(out));
    memcpy(&out, exact, floor);
    free(exact);
    CHECK(out.struct_size == floor && out.kind == MOQ_SUB_STATUS_OUTCOME_TERMINATED);
    CHECK(out.request == r.token && out.stream_error_code == UINT64_MAX);
    CHECK(f->allocations == calls);
    expect_empty(f);
    f->refuse_allocations = false;
    request_t next;
    REQUIRE(issue(f, &next));
    CHECK(next.token == r.token);
    REQUIRE(respond(f, next, true, 2));
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    _Alignas(moq_sub_status_outcome_t) unsigned char future[sizeof(out) + 19];
    memset(future, 0xa5, sizeof(future));
    CHECK(moq_sub_poll_status_outcome(f->sub, (moq_sub_status_outcome_t *)future,
                                      sizeof(future)) == MOQ_OK);
    memcpy(&out, future, sizeof(out));
    compare(out, expected(next, MOQ_VERSION_DRAFT_22, true, 2));
    CHECK(bytes_equal(future + sizeof(out), sizeof(future) - sizeof(out), 0xa5));
    expect_empty(f);
    return true;
}

static bool pressure_case(fixture_t *f, unsigned variant)
{
    REQUIRE(start(f, MOQ_VERSION_DRAFT_22, 1));
    request_t r;
    REQUIRE(issue(f, &r));
    /* The peer's unrelated request occupies the client's only event slot. */
    moq_track_status_cfg_t cfg;
    moq_track_status_cfg_init(&cfg);
    moq_bytes_t part = MOQ_BYTES_LITERAL("unrelated");
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("other");
    moq_track_status_handle_t filler;
    REQUIRE(moq_session_track_status(f->server, &cfg, 1, &filler) == MOQ_OK);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_sub_status_outcome_t want = {0};
    if (variant < 4) {
        bool error = (variant & 1u) != 0;
        REQUIRE(respond(f, r, error, 2));
        moq_action_t a;
        size_t count = 0;
        REQUIRE(moq_session_poll_actions_ex(f->server, &a, 1, sizeof(a), &count) == MOQ_OK);
        REQUIRE(count == 1 && a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, a.u.send_bidi_stream.data,
            a.u.send_bidi_stream.len, variant >= 2, 1) == MOQ_ERR_WOULD_BLOCK);
        moq_action_cleanup(&a);
        CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_ERR_WOULD_BLOCK);
        want = expected(r, MOQ_VERSION_DRAFT_22, error, 2);
    } else {
        CHECK(moq_session_on_bidi_stream_stop(f->client, r.stream, 0, 1) == MOQ_ERR_WOULD_BLOCK);
        CHECK(moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1) == MOQ_ERR_WOULD_BLOCK);
        want.struct_size = sizeof(want);
        want.kind = MOQ_SUB_STATUS_OUTCOME_TERMINATED;
        want.request = r.token;
        want.cause = MOQ_TRACK_STATUS_PEER_STOP_SENDING;
        want.local_role = MOQ_TRACK_STATUS_REQUESTER;
    }
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    consume(f, want);
    CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    expect_empty(f);
    CHECK(moq_session_state(f->client) == MOQ_SESS_ESTABLISHED);
    return true;
}

static bool full_ring_case(fixture_t *f, unsigned unused)
{
    (void)unused;
    REQUIRE(start(f, MOQ_VERSION_DRAFT_22, 0));
    request_t requests[16];
    for (unsigned i = 0; i < 16; ++i) {
        REQUIRE(issue(f, &requests[i]));
        if (i == 0) {
            CHECK(moq_session_on_bidi_stream_reset(f->client, requests[i].stream, 0, 1) == MOQ_OK);
        } else {
            REQUIRE(respond(f, requests[i], (i & 1u) != 0, 2));
            REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
        }
        CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
    }
    moq_sub_status_req_t *extra = NULL;
    CHECK(request_only(f, &extra) == MOQ_ERR_WOULD_BLOCK && extra == NULL);
    moq_sub_status_result_t old;
    memset(&old, 0xa5, sizeof(old));
    CHECK(moq_sub_poll_status(f->sub, &old) == MOQ_ERR_UNSUPPORTED);
    CHECK(bytes_equal(&old, sizeof(old), 0xa5));
    moq_sub_status_outcome_t out;
    memset(&out, 0xa5, sizeof(out));
    CHECK(moq_sub_poll_status_outcome(f->sub, &out, MOQ_SUB_STATUS_OUTCOME_V0_SIZE - 1) == MOQ_ERR_INVAL);
    CHECK(bytes_equal(&out, sizeof(out), 0xa5));
    CHECK(request_only(f, &extra) == MOQ_ERR_WOULD_BLOCK && extra == NULL);

    fixture_t other = {0};
    bool ready = start(&other, MOQ_VERSION_DRAFT_22, 0);
    request_t independent;
    if (ready) ready = issue(&other, &independent) && respond(&other, independent, false, 2);
    if (ready) {
        CHECK(moq_simpair_run_until_quiescent(other.pair, 32, NULL) == MOQ_OK);
        CHECK(moq_sub_tick(other.sub, 1) == MOQ_OK);
        consume(&other, expected(independent, MOQ_VERSION_DRAFT_22, false, 2));
        expect_empty(&other);
    }
    finish(&other);
    REQUIRE(ready);
    for (unsigned i = 0; i < 16; ++i) {
        moq_sub_status_outcome_t want = expected(requests[i], MOQ_VERSION_DRAFT_22, (i & 1u) != 0, 2);
        if (i == 0) {
            memset(&want, 0, sizeof(want));
            want.struct_size = sizeof(want);
            want.kind = MOQ_SUB_STATUS_OUTCOME_TERMINATED;
            want.request = requests[i].token;
            want.cause = MOQ_TRACK_STATUS_PEER_RESET_STREAM;
            want.local_role = MOQ_TRACK_STATUS_REQUESTER;
        }
        consume(f, want);
        if (i == 0) {
            request_t reused;
            REQUIRE(issue(f, &reused));
            CHECK(reused.token == requests[0].token);
            REQUIRE(respond(f, reused, false, 0));
            REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
            CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
            CHECK(request_only(f, &extra) == MOQ_ERR_WOULD_BLOCK && extra == NULL);
        }
    }
    consume(f, expected(requests[0], MOQ_VERSION_DRAFT_22, false, 0));
    expect_empty(f);
    return true;
}

static bool legacy_terminal_case(fixture_t *f, unsigned variant)
{
    REQUIRE(start(f, MOQ_VERSION_DRAFT_18, 0));
    request_t r;
    REQUIRE(issue(f, &r));
    if (variant < 2) {
        CHECK((variant == 0 ? moq_session_on_bidi_stream_reset(f->client, r.stream, UINT64_MAX, 1)
                            : moq_session_on_bidi_stream_stop(f->client, r.stream, 0, 1)) == MOQ_OK);
        CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
        CHECK(moq_session_state(f->client) == MOQ_SESS_ESTABLISHED);
        request_t next;
        REQUIRE(issue(f, &next));
        CHECK(next.token != r.token); /* Legacy silent retirement does not free the facade slot. */
    } else {
        if (variant == 2) {
            CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream, NULL, 0, true, 1) == MOQ_OK);
        } else {
            REQUIRE(respond(f, r, false, 0));
            moq_action_t a;
            size_t count = 0;
            REQUIRE(moq_session_poll_actions_ex(f->server, &a, 1, sizeof(a), &count) == MOQ_OK);
            REQUIRE(count == 1 && a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
            CHECK(moq_session_on_bidi_stream_bytes(f->client, r.stream,
                a.u.send_bidi_stream.data, 1, true, 1) == MOQ_OK);
            moq_action_cleanup(&a);
        }
        CHECK(moq_session_state(f->client) == MOQ_SESS_CLOSED);
        CHECK(moq_sub_tick(f->sub, 1) == MOQ_OK);
        moq_sub_status_req_t *next = NULL;
        CHECK(request_only(f, &next) == MOQ_ERR_CLOSED && next == NULL);
    }
    expect_empty(f);
    return true;
}

static void run(const char *name, bool (*body)(fixture_t *, unsigned), unsigned variant)
{
    char label[96];
    snprintf(label, sizeof(label), "%s.%u", name, variant);
    case_name = label;
    fixture_t f = {0};
    unsigned before = failures, setup_before = setups;
    (void)body(&f, variant);
    finish(&f);
    ++cases;
    printf("%s %s (checks_failed=%u setups_failed=%u)\n",
           failures == before && setups == setup_before ? "PASS" : "FAIL",
           label, failures - before, setups - setup_before);
}

int main(void)
{
    for (unsigned i = 0; i < 6; ++i) run("terminal", terminal_case, i);
    for (unsigned i = 0; i < 36; ++i) run("response", response_case, i);
    for (unsigned i = 0; i < 4; ++i) run("response_before_fin", response_before_fin_case, i);
    run("physical_outputs", output_case, 0);
    for (unsigned i = 0; i < 5; ++i) run("event_pressure", pressure_case, i);
    run("full_ring_independent_reuse", full_ring_case, 0);
    for (unsigned i = 0; i < 4; ++i) run("legacy_terminal", legacy_terminal_case, i);
    printf("subscriber_status_outcome: %u cases, %u failures, %u setup failures\n", cases, failures, setups);
    return setups ? 2 : failures ? 1 : 0;
}
