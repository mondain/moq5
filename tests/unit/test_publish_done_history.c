/* D16 9.15 / D18 10.11: automatic DONE counts all successfully opened
 * streams, including empty, closed, reset and retired/reused subgroups. */
#include <moq/moq.h>
#include <moq/sim.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EVENT_BOUND = 64, STREAM_BOUND = 4, PUMP_BOUND = 64, RESET_CODE = 17 };
enum history { ZERO, EMPTY_FIN, OBJECT_FIN, RESET, STOP, REUSE, AUTO_RESET, HISTORY_COUNT };
static const char *const history_names[] = {
    "zero", "empty-fin", "object-fin", "reset", "peer-stop", "slot-reuse", "automatic-reset"
};
static unsigned failures;
static moq_version_t row_version;
static bool row_publication;
static const char *row_history;

static bool check(bool value, const char *expr, int line)
{
    if (!value) {
        fprintf(stderr, "FAIL D%u/%s/%s:%d %s\n", (unsigned)row_version,
                row_publication ? "publish" : "subscribe", row_history, line, expr);
        failures++;
    }
    return value;
}
#define CHECK(expr) check((expr), #expr, __LINE__)
#define REQUIRE(expr) do { if (!CHECK(expr)) return false; } while (0)

typedef union allocation_header { max_align_t alignment; size_t size; } allocation_header_t;
typedef struct { size_t blocks, bytes; } balance_t;

static void *count_alloc(size_t size, void *ctx)
{
    balance_t *b = ctx;
    if (size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
    allocation_header_t *h = malloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->size = size;
    b->blocks++;
    b->bytes += size;
    return h + 1;
}

static void count_free(void *ptr, size_t size, void *ctx)
{
    if (!ptr) return;
    balance_t *b = ctx;
    allocation_header_t *h = (allocation_header_t *)ptr - 1;
    CHECK(h->size == size && b->blocks > 0 && b->bytes >= h->size);
    b->blocks--;
    b->bytes -= h->size;
    free(h);
}

static void *count_realloc(void *ptr, size_t old_size, size_t new_size, void *ctx)
{
    if (!ptr) return count_alloc(new_size, ctx);
    balance_t *b = ctx;
    allocation_header_t *h = (allocation_header_t *)ptr - 1;
    if (!CHECK(h->size == old_size)) return NULL;
    if (!new_size) { count_free(ptr, old_size, ctx); return NULL; }
    if (new_size > SIZE_MAX - sizeof(*h)) return NULL;
    h = realloc(h, sizeof(*h) + new_size);
    if (!h) return NULL;
    h->size = new_size;
    b->bytes = b->bytes - old_size + new_size;
    return h + 1;
}

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *client, *server;
    moq_alloc_t alloc;
    balance_t balance;
    moq_version_t version;
    bool publication, watch_done;
    moq_subscription_t client_sub, server_sub;
    moq_publication_t client_pub, server_pub;
    moq_stream_ref_t streams[STREAM_BOUND];
    size_t stream_count, expected_count, wire_dones, wire_errors, wire_resets;
    size_t objects, fins, resets, terminals;
    uint64_t next_group;
} pair_t;

static uint64_t now(const pair_t *p) { return moq_simpair_now_us(p->sp); }

/* Only small, ordinary response envelopes are observed. Expected DONE bytes
 * are literal spec framing, independent of the library's encoder/decoder. */
static void observe_responses(pair_t *p, const uint8_t *data, size_t len, bool fin)
{
    size_t pos = 0, frames = 0;
    while (pos < len) {
        if (!CHECK(++frames <= 8 && len - pos >= 3)) return;
        uint8_t type = data[pos];
        size_t body = ((size_t)data[pos + 1] << 8) | data[pos + 2];
        if (!CHECK(type < 64 && body <= len - pos - 3)) return;
        size_t frame_len = body + 3;
        if (type == 0x05) p->wire_errors++;
        if (type == 0x0b) {
            uint8_t d16[] = { 0x0b, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00 };
            uint8_t d18[] = { 0x0b, 0x00, 0x03, 0x08, 0x00, 0x00 };
            d16[3] = p->publication ? 1 : 0;
            d16[5] = d18[4] = (uint8_t)p->expected_count;
            const uint8_t *expected = p->version == MOQ_VERSION_DRAFT_16 ? d16 : d18;
            size_t expected_len = p->version == MOQ_VERSION_DRAFT_16 ? sizeof(d16) : sizeof(d18);
            CHECK(frame_len == expected_len && memcmp(data + pos, expected, expected_len) == 0);
            CHECK(p->wire_errors == 1);
            CHECK(p->version == MOQ_VERSION_DRAFT_16 || (fin && pos + frame_len == len));
            p->wire_dones++;
        }
        pos += frame_len;
    }
}

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    pair_t *p = ctx;
    if (r->kind != MOQ_SIM_TRACE_ACTION || r->from != MOQ_PERSPECTIVE_SERVER) return;
    if (!CHECK(r->struct_size >= offsetof(moq_sim_trace_record_t, fin) + sizeof(r->fin))) return;
    if (r->action_kind == MOQ_ACTION_SEND_DATA) {
        bool found = false;
        for (size_t i = 0; i < p->stream_count; i++)
            if (p->streams[i]._v == r->stream_ref._v) found = true;
        if (!found) {
            if (!CHECK(p->stream_count < STREAM_BOUND)) return;
            p->streams[p->stream_count++] = r->stream_ref;
        }
    } else if (r->action_kind == MOQ_ACTION_RESET_DATA) {
        CHECK(p->wire_dones == 0);
        p->wire_resets++;
    }
    if (!p->watch_done) return;
    if (r->action_kind == MOQ_ACTION_SEND_CONTROL)
        observe_responses(p, r->bytes.data, r->bytes.len, false);
    else if (r->action_kind == MOQ_ACTION_SEND_BIDI_STREAM)
        observe_responses(p, r->detail_bytes.data, r->detail_bytes.len, r->fin);
}

static bool pump(pair_t *p)
{
    REQUIRE(moq_simpair_run_until_quiescent(p->sp, PUMP_BOUND, NULL) == MOQ_OK);
    REQUIRE(moq_session_state(p->client) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(p->server) == MOQ_SESS_ESTABLISHED);
    return true;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    for (unsigned i = 0; i < EVENT_BOUND; i++) {
        if (moq_session_poll_events(s, &ev, 1) != 1) return false;
        if (ev.kind == kind) { *out = ev; return true; }
        CHECK(ev.kind != MOQ_EVENT_SESSION_CLOSED && ev.kind != MOQ_EVENT_SUBSCRIBE_DONE &&
              ev.kind != MOQ_EVENT_PUBLISH_FINISHED);
        moq_event_cleanup(&ev);
    }
    CHECK(false);
    return false;
}

static bool handles_match(const pair_t *p, moq_subscription_t sub, moq_publication_t pub)
{
    return moq_subscription_eq(sub, p->client_sub) && moq_publication_eq(pub, p->client_pub);
}

static bool collect(pair_t *p)
{
    moq_event_t ev;
    for (unsigned i = 0; i < EVENT_BOUND; i++) {
        if (moq_session_poll_events(p->client, &ev, 1) != 1) return true;
        bool ok = true;
        switch (ev.kind) {
        case MOQ_EVENT_OBJECT_RECEIVED:
            p->objects++;
            ok = handles_match(p, ev.u.object_received.sub, ev.u.object_received.pub) &&
                ev.u.object_received.payload && moq_rcbuf_len(ev.u.object_received.payload) == 1 &&
                moq_rcbuf_data(ev.u.object_received.payload)[0] == 0x5a;
            break;
        case MOQ_EVENT_SUBGROUP_FINISHED:
            p->fins++;
            ok = handles_match(p, ev.u.subgroup_finished.sub, ev.u.subgroup_finished.pub);
            break;
        case MOQ_EVENT_SUBGROUP_RESET:
            p->resets++;
            ok = handles_match(p, ev.u.subgroup_reset.sub, ev.u.subgroup_reset.pub);
            break;
        case MOQ_EVENT_SUBSCRIBE_DONE:
            p->terminals++;
            ok = !p->publication && moq_subscription_eq(ev.u.subscribe_done.sub, p->client_sub) &&
                ev.u.subscribe_done.status_code == 8 &&
                ev.u.subscribe_done.stream_count == p->expected_count &&
                p->fins + p->resets == p->expected_count;
            break;
        case MOQ_EVENT_PUBLISH_FINISHED:
            p->terminals++;
            ok = p->publication && moq_publication_eq(ev.u.publish_finished.pub, p->client_pub) &&
                ev.u.publish_finished.status_code == 8 &&
                ev.u.publish_finished.stream_count == p->expected_count &&
                p->fins + p->resets == p->expected_count;
            break;
        case MOQ_EVENT_SESSION_CLOSED:
            ok = false;
            break;
        default:
            break;
        }
        moq_event_cleanup(&ev);
        REQUIRE(ok);
    }
    REQUIRE(false);
    return false;
}

static bool establish(pair_t *p)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &p->alloc;
    cfg.version = p->version;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 64;
    cfg.server_max_open_subgroups = 1;
    cfg.client_max_data_streams = 1;
    cfg.server_defer_subscription_updates = true;
    cfg.trace_fn = trace;
    cfg.trace_ctx = p;
    REQUIRE(moq_simpair_create(&cfg, &p->sp) == MOQ_OK && p->sp);
    p->client = moq_simpair_client(p->sp);
    p->server = moq_simpair_server(p->sp);
    REQUIRE(moq_simpair_start(p->sp) == MOQ_OK);
    if (!pump(p)) return false;
    REQUIRE(moq_session_version(p->client) == p->version &&
            moq_session_version(p->server) == p->version);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("history") };
    moq_event_t ev;
    bool ok;
    if (p->publication) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init(&pc);
        pc.track_namespace = (moq_namespace_t){ parts, 1 };
        pc.track_name = MOQ_BYTES_LITERAL("published");
        REQUIRE(moq_session_publish(p->server, &pc, now(p), &p->server_pub) == MOQ_OK);
        if (!pump(p)) return false;
        REQUIRE(take(p->client, MOQ_EVENT_PUBLISH_REQUEST, &ev));
        p->client_pub = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
        REQUIRE(moq_publication_is_valid(p->client_pub));
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init(&ac);
        REQUIRE(moq_session_accept_publish(p->client, p->client_pub, &ac, now(p)) == MOQ_OK);
        if (!pump(p)) return false;
        REQUIRE(take(p->server, MOQ_EVENT_PUBLISH_OK, &ev));
        ok = moq_publication_eq(ev.u.publish_ok.pub, p->server_pub) && ev.u.publish_ok.send_allowed;
    } else {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = (moq_namespace_t){ parts, 1 };
        sc.track_name = MOQ_BYTES_LITERAL("subscribed");
        REQUIRE(moq_session_subscribe(p->client, &sc, now(p), &p->client_sub) == MOQ_OK);
        if (!pump(p)) return false;
        REQUIRE(take(p->server, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
        p->server_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        REQUIRE(moq_subscription_is_valid(p->server_sub));
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        REQUIRE(moq_session_accept_subscribe(p->server, p->server_sub, &ac, now(p)) == MOQ_OK);
        if (!pump(p)) return false;
        REQUIRE(take(p->client, MOQ_EVENT_SUBSCRIBE_OK, &ev));
        ok = moq_subscription_eq(ev.u.subscribe_ok.sub, p->client_sub);
    }
    moq_event_cleanup(&ev);
    REQUIRE(ok);
    return collect(p);
}

static bool stream_lifecycle(pair_t *p, enum history h, moq_subgroup_handle_t *last)
{
    size_t before = p->stream_count;
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = p->next_group++;
    cfg.subgroup_id = 3;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    moq_result_t rc = p->publication
        ? moq_session_open_pub_subgroup(p->server, p->server_pub, &cfg, now(p), &sg)
        : moq_session_open_subgroup(p->server, p->server_sub, &cfg, now(p), &sg);
    REQUIRE(rc == MOQ_OK && moq_subgroup_is_valid(sg));
    REQUIRE(!moq_subgroup_eq(sg, *last));
    *last = sg;
    if (h != EMPTY_FIN) {
        static const uint8_t byte = 0x5a;
        moq_rcbuf_t *payload = NULL;
        REQUIRE(moq_rcbuf_create(&p->alloc, &byte, 1, &payload) == MOQ_OK);
        rc = moq_session_write_object(p->server, sg, 0, payload, now(p));
        moq_rcbuf_decref(payload);
        REQUIRE(rc == MOQ_OK);
    }
    if (!pump(p) || !collect(p)) return false;
    REQUIRE(p->stream_count == before + 1 && p->terminals == 0);
    if (h == AUTO_RESET) return true;
    if (h == RESET)
        rc = moq_session_reset_subgroup(p->server, sg, RESET_CODE, now(p));
    else if (h == STOP)
        rc = moq_session_on_data_stop(p->server, p->streams[before], RESET_CODE, now(p));
    else
        rc = moq_session_close_subgroup(p->server, sg, now(p));
    REQUIRE(rc == MOQ_OK);
    if (!pump(p) || !collect(p)) return false;
    REQUIRE(p->fins + p->resets == p->stream_count && p->terminals == 0);
    /* Public stale-handle proof of retirement, followed by reuse of a pool
     * configured with one slot on the next call. No private inventory oracle. */
    REQUIRE(moq_session_close_subgroup(p->server, sg, now(p)) == MOQ_ERR_STALE_HANDLE);
    return true;
}

static bool reject_update(pair_t *p)
{
    moq_result_t rc;
    if (p->publication) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init(&cfg);
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 9;
        rc = moq_session_update_publication(p->client, p->client_pub, &cfg, now(p));
    } else {
        moq_subscription_update_cfg_t cfg;
        moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 9;
        rc = moq_session_update_subscription(p->client, p->client_sub, &cfg, now(p));
    }
    REQUIRE(rc == MOQ_OK);
    if (!pump(p)) return false;
    moq_event_t ev;
    REQUIRE(take(p->server, p->publication ? MOQ_EVENT_PUBLISH_UPDATED : MOQ_EVENT_SUBSCRIBE_UPDATED, &ev));
    uint64_t id = p->publication ? ev.u.publish_updated.update_request_id : ev.u.subscribe_updated.update_request_id;
    bool owed = p->publication ? ev.u.publish_updated.answer_owed : ev.u.subscribe_updated.answer_owed;
    bool handle_ok = p->publication ? moq_publication_eq(ev.u.publish_updated.pub, p->server_pub)
                                   : moq_subscription_eq(ev.u.subscribe_updated.sub, p->server_sub);
    moq_event_cleanup(&ev);
    REQUIRE(owed && handle_ok);
    moq_reject_subscription_update_cfg_t cfg;
    moq_reject_subscription_update_cfg_init(&cfg);
    cfg.update_request_id = id;
    cfg.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
    p->watch_done = true;
    rc = p->publication ? moq_session_reject_publication_update(p->server, p->server_pub, &cfg, now(p))
                        : moq_session_reject_subscription_update(p->server, p->server_sub, &cfg, now(p));
    REQUIRE(rc == MOQ_OK);
    if (!pump(p) || !collect(p)) return false;
    REQUIRE(p->wire_dones == 1 && p->wire_errors == 1 && p->terminals == 1);
    return true;
}

static bool scenario(pair_t *p, enum history h)
{
    if (!establish(p)) return false;
    moq_subgroup_handle_t last = MOQ_SUBGROUP_INVALID;
    if (h == REUSE) {
        static const enum history steps[] = { EMPTY_FIN, OBJECT_FIN, RESET, STOP };
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
            if (!stream_lifecycle(p, steps[i], &last)) return false;
    } else if (h != ZERO && !stream_lifecycle(p, h, &last)) return false;
    REQUIRE(p->stream_count == p->expected_count);
    if (!reject_update(p)) return false;
    size_t expected_objects = h == ZERO || h == EMPTY_FIN ? 0 : h == REUSE ? 3 : 1;
    size_t expected_fins = h == EMPTY_FIN || h == OBJECT_FIN ? 1 : h == REUSE ? 2 : 0;
    REQUIRE(p->objects == expected_objects && p->fins == expected_fins);
    REQUIRE(p->resets == p->expected_count - expected_fins && p->wire_resets == p->resets);
    for (unsigned i = 0; i < 3; i++) {
        if (!pump(p) || !collect(p)) return false;
        REQUIRE(p->wire_dones == 1 && p->wire_errors == 1 && p->terminals == 1);
        REQUIRE(p->fins + p->resets == p->expected_count);
    }
    return true;
}

int main(void)
{
    const moq_version_t versions[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18 };
    unsigned rows = 0;
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        for (unsigned publication = 0; publication < 2; publication++) {
            for (enum history h = ZERO; h < HISTORY_COUNT; h++) {
                unsigned before = failures;
                row_version = versions[v]; row_publication = publication != 0;
                row_history = history_names[h];
                pair_t p = {0};
                p.version = row_version; p.publication = row_publication; p.next_group = 10;
                p.expected_count = h == ZERO ? 0 : h == REUSE ? 4 : 1;
                p.alloc = (moq_alloc_t){ .ctx = &p.balance, .alloc = count_alloc,
                    .realloc = count_realloc, .free = count_free };
                bool ok = scenario(&p, h);
                if (p.sp) moq_simpair_destroy(p.sp);
                CHECK(p.balance.blocks == 0 && p.balance.bytes == 0);
                printf("D%u/%s/%s opened=%zu fin=%zu reset=%zu wire-done=%zu event=%zu balance=%zu/%zu %s\n",
                       (unsigned)p.version, publication ? "publish" : "subscribe", row_history,
                       p.stream_count, p.fins, p.resets, p.wire_dones, p.terminals,
                       p.balance.blocks, p.balance.bytes, ok && failures == before ? "PASS" : "FAIL");
                rows++;
                if (!ok || failures != before) {
                    fprintf(stderr, "STOP: product/fixture discrepancy; remaining rows not executed\n");
                    return 1;
                }
            }
        }
    }
    printf("publish_done_history: %u rows, %u failures; D16 request-bidi FIN N/A (control channel)\n",
           rows, failures);
    return failures ? 1 : 0;
}
