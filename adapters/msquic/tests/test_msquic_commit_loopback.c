/* Real selected MsQuic, owned loopback. The clock controls only MoQ timers,
 * never QUIC ACKs. For expiry, advance it after native FIN acceptance while
 * still on the serialized connection callback: the RESET must follow that
 * FIN at the real provider before another native callback can run. */
#ifndef QUIC_API_ENABLE_PREVIEW_FEATURES
#define QUIC_API_ENABLE_PREVIEW_FEATURES 1
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <moq/msquic_managed.h>
#include "msquic_internal.h"
#include "msq_loopback_port.h"

extern moq_msquic_conn_t *moq_msq_test_managed_conn_adapter(moq_msquic_managed_conn_t *conn);

static const QUIC_API_TABLE *api;
static _Atomic uint64_t test_time;
static uint64_t now_us(void) { return atomic_load(&test_time); }

enum phase { WAIT_SUB, WAIT_PREFIX, WAIT_FIN, COMMITTED };
struct run {
    pthread_mutex_t mu;
    moq_version_t version;
    bool data_fin, expire;
    _Atomic unsigned failures;
    enum phase phase;
    bool subscribed, have_sub, wrapped, have_id;
    bool fin_accepted, prefix_acked, bytes_acked_without_fin, timer_cleared;
    bool timeout_injected, reliable_prefix_reset;
    unsigned objects, object_mask, peer_resets, peer_fins;
    unsigned native_data_fin, native_empty_fin;
    unsigned native_abort, commit_reports;
    uint64_t native_abort_code, id;
    moq_subscription_t sub;
    moq_subgroup_handle_t sg;
    moq_rcbuf_t *payload;
    moq_msquic_conn_t *conn;
    moq_transport_endpoint_ops_t original_ops;
    moq_msquic_hook_fn original_hook;
    void *original_hook_user;
    QUIC_API_TABLE wrapped_api;
};
static struct run *active;

#define CHECK(r, x) do { if (!(x)) { \
    fprintf(stderr, "FAIL d%u/%s/%s:%d: %s\n", (unsigned)(r)->version, \
        (r)->data_fin ? "data-fin" : "empty-fin", (r)->expire ? "timeout" : "ack", \
        __LINE__, #x); ++(r)->failures; } } while (0)

static QUIC_STATUS QUIC_API native_send(HQUIC stream, const QUIC_BUFFER *buffers,
    uint32_t count, QUIC_SEND_FLAGS flags, void *ctx)
{
    QUIC_STATUS rc = api->StreamSend(stream, buffers, count, flags, ctx);
    struct run *r = active;
    pthread_mutex_lock(&r->mu);
    struct moq_msq_stream *ms = moq_msquic_test_stream_find(r->conn, r->id);
    if (QUIC_SUCCEEDED(rc) && ms && ms->stream == stream && (flags & QUIC_SEND_FLAG_FIN))
        ++r->native_data_fin;
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static QUIC_STATUS QUIC_API native_shutdown(HQUIC stream,
    QUIC_STREAM_SHUTDOWN_FLAGS flags, QUIC_UINT62 code)
{
    QUIC_STATUS rc = api->StreamShutdown(stream, flags, code);
    struct run *r = active;
    pthread_mutex_lock(&r->mu);
    struct moq_msq_stream *ms = moq_msquic_test_stream_find(r->conn, r->id);
    if (QUIC_SUCCEEDED(rc) && ms && ms->stream == stream) {
        if (flags == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL) ++r->native_empty_fin;
        if (flags == QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) {
            ++r->native_abort;
            r->native_abort_code = code;
            CHECK(r, ms->fin_sent);
            CHECK(r, !ms->commit_reported);
            CHECK(r, r->conn->test_stream_commit_reports == 0);
        }
    }
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static moq_transport_result_t open_uni(void *ctx, uint64_t *id)
{
    struct run *r = active;
    moq_transport_result_t rc = r->original_ops.open_uni(ctx, id);
    pthread_mutex_lock(&r->mu);
    if (rc == MOQ_TRANSPORT_OK && r->phase == WAIT_PREFIX) {
        CHECK(r, !r->have_id);
        r->have_id = true;
        r->id = *id;
    }
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static void accepted_fin(void *ctx, uint64_t id, bool fin, moq_transport_result_t rc)
{
    struct run *r = active;
    if (!fin || rc != MOQ_TRANSPORT_OK || !r->have_id || id != r->id) return;
    moq_msquic_conn_t *c = ctx;
    pthread_mutex_lock(&r->mu);
    struct moq_msq_stream *ms = moq_msquic_test_stream_find(c, id);
    CHECK(r, ms && ms->fin_sent && ms->stream != NULL);
    CHECK(r, !r->fin_accepted);
    r->fin_accepted = true;
    CHECK(r, c->test_stream_commit_reports == 0);
    CHECK(r, r->prefix_acked);
    CHECK(r, c->on_worker > 0);
    uint64_t deadline = moq_session_next_deadline_us(c->session);
    if (r->version == MOQ_VERSION_DRAFT_16) {
        CHECK(r, deadline == UINT64_MAX);
    } else {
        CHECK(r, deadline == now_us() + 100000);
    }
    if (r->expire) {
        CHECK(r, deadline != UINT64_MAX);
        atomic_store(&test_time, deadline);
        CHECK(r, moq_session_tick(c->session, deadline) == MOQ_OK);
        r->timeout_injected = true;
    }
    pthread_mutex_unlock(&r->mu);
}

static moq_transport_result_t write_bytes(void *ctx, uint64_t id,
    const uint8_t *data, size_t len, bool fin)
{
    moq_transport_result_t rc = active->original_ops.write(ctx, id, data, len, fin);
    accepted_fin(ctx, id, fin, rc);
    return rc;
}

static moq_transport_result_t write_payload(void *ctx, uint64_t id,
    moq_rcbuf_t *buf, bool fin)
{
    moq_transport_result_t rc = active->original_ops.write_payload(ctx, id, buf, fin);
    accepted_fin(ctx, id, fin, rc);
    return rc;
}

static moq_transport_result_t reset_at(void *ctx, uint64_t id, uint64_t code, uint64_t size)
{
    struct run *r = active;
    moq_transport_result_t rc = r->original_ops.reset_stream_at(ctx, id, code, size);
    pthread_mutex_lock(&r->mu);
    if (id == r->id) {
        struct moq_msq_stream *ms = moq_msquic_test_stream_find(r->conn, id);
        CHECK(r, rc == MOQ_TRANSPORT_OK);
        CHECK(r, r->conn->reliable_reset && size > 0);
        CHECK(r, ms && ms->acked_total >= size && ms->reset_sent);
        CHECK(r, code == MOQ_RESET_DELIVERY_TIMEOUT);
        r->reliable_prefix_reset = rc == MOQ_TRANSPORT_OK && size > 0 &&
            ms && ms->acked_total >= size && ms->reset_sent;
    }
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static int client_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
    uint64_t now, void *ctx)
{
    (void)lane;
    struct run *r = ctx;
    moq_session_t *s = moq_msquic_managed_session(m);
    pthread_mutex_lock(&r->mu);
    moq_event_t ev;
    while (s && moq_session_poll_events(s, &ev, 1)) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE && !r->subscribed) {
            moq_subscribe_cfg_t cfg;
            moq_subscribe_cfg_init(&cfg);
            moq_bytes_t part = MOQ_BYTES_LITERAL("commit");
            cfg.track_namespace = (moq_namespace_t){&part, 1};
            cfg.track_name = MOQ_BYTES_LITERAL("v");
            cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
            moq_subscription_t sub;
            CHECK(r, moq_session_subscribe(s, &cfg, now, &sub) == MOQ_OK);
            r->subscribed = true;
        } else if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            CHECK(r, ev.u.object_received.group_id == 7);
            CHECK(r, ev.u.object_received.object_id < 2);
            CHECK(r, ev.u.object_received.payload && moq_rcbuf_len(ev.u.object_received.payload) == 7);
            if (ev.u.object_received.payload && moq_rcbuf_len(ev.u.object_received.payload) == 7)
                CHECK(r, memcmp(moq_rcbuf_data(ev.u.object_received.payload), "payload", 7) == 0);
            if (ev.u.object_received.object_id < 2) {
                unsigned bit = 1u << (unsigned)ev.u.object_received.object_id;
                CHECK(r, !(r->object_mask & bit));
                r->object_mask |= bit;
            }
            ++r->objects;
        } else if (ev.kind == MOQ_EVENT_SUBGROUP_RESET) {
            CHECK(r, r->expire);
            CHECK(r, ev.u.subgroup_reset.group_id == 7);
            CHECK(r, ev.u.subgroup_reset.error_code == MOQ_RESET_DELIVERY_TIMEOUT);
            ++r->peer_resets;
        } else if (ev.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
            CHECK(r, ev.u.subgroup_finished.group_id == 7);
            CHECK(r, r->objects == (r->data_fin ? 2u : 1u));
            ++r->peer_fins;
        }
        moq_event_cleanup(&ev);
    }
    pthread_mutex_unlock(&r->mu);
    return 0;
}

/* The managed application pump is coalesced on its doorbell. Observe the
 * attach hook instead, preserving the managed hook: this sees SEND_COMPLETE
 * before SEND_SHUTDOWN_COMPLETE and submits the FIN on the native worker. */
static void observe_service(moq_msquic_conn_t *c, void *ctx)
{
    struct run *r = ctx;
    r->original_hook(c, r->original_hook_user);
    pthread_mutex_lock(&r->mu);
    moq_session_t *s = c->session;
    struct moq_msq_stream *ms = r->have_id ? moq_msquic_test_stream_find(c, r->id) : NULL;
    if (r->phase == WAIT_PREFIX && ms && ms->sent_total > 0 &&
        ms->acked_total == ms->sent_total && ms->inflight_bytes == 0) {
        CHECK(r, c->on_worker > 0);
        r->prefix_acked = true;
        CHECK(r, !ms->fin_sent && c->test_stream_commit_reports == 0);
        if (r->data_fin)
            CHECK(r, moq_session_write_object(s, r->sg, 1, r->payload, now_us()) == MOQ_OK);
        CHECK(r, moq_session_close_subgroup(s, r->sg, now_us()) == MOQ_OK);
        r->phase = WAIT_FIN;
    } else if (r->phase == WAIT_FIN) {
        r->commit_reports = c->test_stream_commit_reports;
        if (r->commit_reports) {
            CHECK(r, !r->expire && r->commit_reports == 1);
            CHECK(r, c->test_last_committed_stream == r->id);
            CHECK(r, moq_session_state(s) == MOQ_SESS_ESTABLISHED);
            CHECK(r, moq_session_next_deadline_us(s) == UINT64_MAX);
            r->timer_cleared = true;
            r->phase = COMMITTED;
        } else if (!r->expire && ms && ms->fin_sent &&
                   ms->acked_total == ms->sent_total && ms->inflight_bytes == 0) {
            r->bytes_acked_without_fin = true;
            if (r->version != MOQ_VERSION_DRAFT_16)
                CHECK(r, moq_session_next_deadline_us(s) == now_us() + 100000);
        }
    }
    pthread_mutex_unlock(&r->mu);
}

static int server_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
    uint64_t now, void *ctx)
{
    (void)m;
    struct run *r = ctx;
    moq_msquic_managed_conn_t *child = moq_msquic_lane_next_conn(lane, NULL);
    moq_session_t *s = child ? moq_msquic_managed_conn_session(child) : NULL;
    if (!s) return 0;
    pthread_mutex_lock(&r->mu);
    if (!r->wrapped) {
        r->conn = moq_msq_test_managed_conn_adapter(child);
        r->original_hook = r->conn->hook;
        r->original_hook_user = r->conn->hook_user;
        r->conn->hook = observe_service;
        r->conn->hook_user = r;
        r->original_ops = r->conn->ops;
        r->wrapped_api = *r->conn->api;
        r->wrapped_api.StreamSend = native_send;
        r->wrapped_api.StreamShutdown = native_shutdown;
        r->conn->api = &r->wrapped_api;
        r->conn->ops.open_uni = open_uni;
        r->conn->ops.write = write_bytes;
        r->conn->ops.write_payload = write_payload;
        r->conn->ops.reset_stream_at = reset_at;
        r->wrapped = true;
    }
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1)) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            r->sub = ev.u.subscribe_request.sub;
            r->have_sub = true;
        }
        moq_event_cleanup(&ev);
    }
    if (r->phase == WAIT_SUB && r->have_sub) {
        static const uint8_t d16[] = {0x02, 0x40, 0x64};
        static const uint8_t later[] = {0x06, 0x64};
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        ac.track_properties = r->version == MOQ_VERSION_DRAFT_16
            ? (moq_bytes_t){d16, sizeof(d16)} : (moq_bytes_t){later, sizeof(later)};
        CHECK(r, moq_session_accept_subscribe(s, r->sub, &ac, now) == MOQ_OK);
        moq_subgroup_cfg_t cfg;
        moq_subgroup_cfg_init(&cfg);
        cfg.group_id = 7;
        CHECK(r, moq_session_open_subgroup(s, r->sub, &cfg, now, &r->sg) == MOQ_OK);
        CHECK(r, moq_session_write_object(s, r->sg, 0, r->payload, now) == MOQ_OK);
        r->phase = WAIT_PREFIX;
    }
    pthread_mutex_unlock(&r->mu);
    return 0;
}

static unsigned run_once(const char *cert, const char *key, moq_version_t version,
                          bool data_fin, bool expire)
{
    struct run r;
    memset(&r, 0, sizeof(r));
    atomic_init(&r.failures, 0);
    pthread_mutex_init(&r.mu, NULL);
    r.version = version;
    r.data_fin = data_fin;
    r.expire = expire;
    atomic_store(&test_time, 1000000);
    active = &r;
    CHECK(&r, moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"payload", 7,
                              &r.payload) == MOQ_OK);
    moq_msquic_managed_t *server = NULL, *client = NULL;
    moq_msquic_managed_cfg_t cfg;
    moq_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.host = "127.0.0.1";
    cfg.version = version;
    cfg.cert_path = cert;
    cfg.key_path = key;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 16;
    cfg.on_lane_pump = server_pump;
    cfg.on_lane_pump_user = &r;
    CHECK(&r, msqt_server_create(&cfg, &server) == MOQ_OK);
    if (server) {
        cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
        cfg.port = moq_msquic_managed_port(server);
        cfg.cert_path = cfg.key_path = NULL;
        cfg.insecure_skip_verify = true;
        cfg.on_lane_pump = client_pump;
        CHECK(&r, moq_msquic_managed_create(&cfg, &client) == MOQ_OK);
    }
    bool complete = false;
    if (client) {
        for (unsigned i = 0; i < 500; ++i) {
            pthread_mutex_lock(&r.mu);
            complete = expire ? r.peer_resets + r.peer_fins == 1 &&
                r.native_abort == 1 && r.reliable_prefix_reset :
                r.phase == COMMITTED && r.peer_fins == 1 &&
                r.objects == (data_fin ? 2u : 1u);
            unsigned failed = r.failures;
            pthread_mutex_unlock(&r.mu);
            if (complete || failed || moq_msquic_managed_is_fatal(client) ||
                moq_msquic_managed_is_fatal(server)) break;
            (void)moq_msquic_managed_wait(client, 10000);
        }
        pthread_mutex_lock(&r.mu);
        CHECK(&r, complete && r.fin_accepted && r.prefix_acked);
        CHECK(&r, r.native_data_fin == (data_fin ? 1u : 0u));
        CHECK(&r, r.native_empty_fin == (data_fin ? 0u : 1u));
        if (expire) {
            CHECK(&r, r.native_abort == 1 && r.native_abort_code == MOQ_RESET_DELIVERY_TIMEOUT);
            CHECK(&r, r.timeout_injected && r.reliable_prefix_reset);
            /* A receiver may already have consumed FIN before the sender
             * processes its ACK. Selected MsQuic ignores RESET after that
             * RemoteCloseAcked winner. Require an actual peer terminal, not
             * a guaranteed RESET replacing an already delivered FIN. */
            CHECK(&r, r.peer_resets + r.peer_fins == 1);
            CHECK(&r, r.commit_reports == 0);
        } else {
            CHECK(&r, r.commit_reports == 1 && r.timer_cleared);
            CHECK(&r, r.native_abort == 0 && r.peer_resets == 0 && r.peer_fins == 1);
            if (data_fin) CHECK(&r, r.bytes_acked_without_fin);
        }
        pthread_mutex_unlock(&r.mu);
        CHECK(&r, !moq_msquic_managed_is_fatal(client));
        CHECK(&r, !moq_msquic_managed_is_fatal(server));
        CHECK(&r, moq_msquic_managed_stop(client) == MOQ_OK);
        moq_msquic_managed_destroy(client);
    }
    if (server) {
        CHECK(&r, moq_msquic_managed_stop(server) == MOQ_OK);
        moq_msquic_managed_destroy(server);
    }
    CHECK(&r, moq_rcbuf_refcount(r.payload) == 1);
    moq_rcbuf_decref(r.payload);
    pthread_mutex_destroy(&r.mu);
    active = NULL;
    printf("%s native/d%u/%s/%s: commits=%u native-aborts=%u peer-resets=%u peer-fins=%u\n",
        r.failures ? "FAIL" : "PASS", (unsigned)version,
        data_fin ? "data-fin" : "empty-fin", expire ? "timeout" : "ack",
        r.commit_reports, r.native_abort, r.peer_resets, r.peer_fins);
    return r.failures;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s cert.pem key.pem\n", argv[0]);
        return 2;
    }
    /* Keep the same process-lifetime library pin as existing owned loopbacks. */
    if (QUIC_FAILED(MsQuicOpen2(&api))) return 2;
    moq_msq_test_now_us = now_us;
    unsigned failures = 0;
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i)
        for (unsigned data = 0; data < 2; ++data) {
            failures += run_once(argv[1], argv[2], versions[i], data != 0, false);
            if (versions[i] != MOQ_VERSION_DRAFT_16)
                failures += run_once(argv[1], argv[2], versions[i], data != 0, true);
        }
    moq_msq_test_now_us = NULL;
    printf("10 native cases, %u failures\n", failures);
    return failures ? 1 : 0;
}
