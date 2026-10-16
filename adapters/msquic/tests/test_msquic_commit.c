/* Real sessions and bridge over a deterministic fake MsQuic table.
 * SEND_COMPLETE returns buffers, not FIN acknowledgement. Only a graceful
 * SEND_SHUTDOWN_COMPLETE may commit the matching local UNI. No wire proof. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msquic_internal.h"
#include "support/fake_msq_table.h"
#include "../../../core/src/bridge/transport_bridge_internal.h"
#include "../../../tests/unit/test_alloc_support.h"

static unsigned failures, prerequisites, cases;
static const char *case_name;
static uint64_t clock_us;
static uint64_t fake_now(void) { return clock_us; }

#define NEED(x) do { if (!(x)) { \
    fprintf(stderr, "PREREQUISITE %s:%d: %s\n", case_name, __LINE__, #x); \
    ++prerequisites; return false; } } while (0)
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #x); \
    ++failures; } } while (0)

struct side {
    fake_msq_t fake;
    moq_session_t *session;
    moq_msquic_conn_t *conn;
    int relayed;
};

struct fixture {
    test_alloc_state_t memory;
    moq_alloc_t alloc;
    struct side sides[2];
    struct side *sender, *receiver;
    moq_connection_target_t target;
    moq_subscription_t sub;
    moq_rcbuf_t *payload;
};

static void conn_event(struct side *s, QUIC_CONNECTION_EVENT *ev)
{
    (void)moq_msquic_conn_callback()(fake_msq_conn_handle(&s->fake), s->conn, ev);
}

static void stream_event(fake_msq_stream_t *st, QUIC_STREAM_EVENT_TYPE type,
                         bool graceful)
{
    QUIC_STREAM_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.Type = type;
    if (type == QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE)
        ev.SEND_SHUTDOWN_COMPLETE.Graceful = graceful ? TRUE : FALSE;
    if (type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED)
        ev.PEER_RECEIVE_ABORTED.ErrorCode = 0x77;
    (void)st->cb((HQUIC)st, st->ctx, &ev);
}

static void commit(fake_msq_stream_t *st)
{
    stream_event(st, QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE, true);
}

static bridge_stream_entry_t *mapping(struct side *s, uint64_t id)
{
    moq_transport_bridge_t *b = moq_msquic_test_bridge(s->conn);
    for (uint32_t i = 0; i < b->max_streams; ++i)
        if (b->streams[i].active && b->streams[i].transport_id == id)
            return &b->streams[i];
    return NULL;
}

static bool take(struct side *s, moq_event_kind_t kind, moq_event_t *ev)
{
    NEED(moq_session_poll_events(s->session, ev, 1) == 1);
    if (ev->kind != kind) {
        moq_event_cleanup(ev);
        NEED(ev->kind == kind);
    }
    return true;
}

static fake_msq_stream_t *peer_stream(struct side *s, uint64_t id, bool uni)
{
    for (int i = 0; i < s->fake.stream_count; ++i)
        if (s->fake.streams[i].id == id)
            return &s->fake.streams[i];
    fake_msq_stream_t *st = fake_msq_peer_stream(&s->fake, id, uni);
    if (st == NULL) return NULL;
    QUIC_CONNECTION_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.Type = QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED;
    ev.PEER_STREAM_STARTED.Stream = (HQUIC)st;
    ev.PEER_STREAM_STARTED.Flags = uni ? QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL
                                      : QUIC_STREAM_OPEN_FLAG_NONE;
    conn_event(s, &ev);
    return st;
}

static bool relay(struct side *from, struct side *to)
{
    while (from->relayed < from->fake.send_count) {
        fake_msq_send_t *send = &from->fake.sends[from->relayed++];
        NEED(send->bytes_len == send->total);
        fake_msq_stream_t *st = peer_stream(to, send->stream->id, send->stream->uni);
        NEED(st != NULL);
        fake_msq_deliver_receive(st, send->bytes, send->bytes_len,
                                (send->flags & QUIC_SEND_FLAG_FIN) != 0);
        NEED(!fake_msq_stream_recv_disabled(st));
        NEED(fake_msq_deliver_send_complete(&from->fake, false));
    }
    return true;
}

static bool pump(struct fixture *f)
{
    for (unsigned i = 0; i < 16; ++i) {
        moq_msquic_conn_service(f->sides[0].conn);
        moq_msquic_conn_service(f->sides[1].conn);
        if (!relay(&f->sides[0], &f->sides[1]) ||
            !relay(&f->sides[1], &f->sides[0])) return false;
        if (f->sides[0].relayed == f->sides[0].fake.send_count &&
            f->sides[1].relayed == f->sides[1].fake.send_count) return true;
    }
    NEED(false);
}

static bool setup(struct fixture *f, moq_version_t version, bool client_sender)
{
    f->alloc = test_allocator(&f->memory);
    f->sender = &f->sides[client_sender ? 0 : 1];
    f->receiver = &f->sides[client_sender ? 1 : 0];
    f->target.struct_size = sizeof(f->target);
    f->target.target_present = MOQ_BINDING_TARGET_COMPLETE;
    f->target.authority = MOQ_BYTES_LITERAL("test.invalid");
    clock_us = 1000000;
    for (unsigned i = 0; i < 2; ++i) {
        struct side *s = &f->sides[i];
        fake_msq_init(&s->fake, i == 0);
        moq_session_cfg_t sc;
        moq_session_cfg_init_sized(&sc, sizeof(sc), &f->alloc,
            i == 0 ? MOQ_PERSPECTIVE_CLIENT : MOQ_PERSPECTIVE_SERVER);
        sc.version = version;
        sc.send_request_capacity = true;
        sc.initial_request_capacity = 16;
        NEED(moq_session_create(&sc, clock_us, &s->session) == MOQ_OK);
        moq_msquic_conn_cfg_t ac;
        moq_msquic_conn_cfg_init_sized(&ac, sizeof(ac));
        ac.alloc = &f->alloc;
        ac.session = s->session;
        ac.api = fake_msq_table(&s->fake);
        ac.target = &f->target;
        NEED(moq_msquic_conn_create(&ac, &s->conn) == MOQ_OK);
        NEED(moq_msquic_conn_bind(s->conn, fake_msq_conn_handle(&s->fake)) == MOQ_OK);
    }
    for (unsigned i = 0; i < 2; ++i) {
        QUIC_CONNECTION_EVENT ev;
        memset(&ev, 0, sizeof(ev));
        ev.Type = QUIC_CONNECTION_EVENT_CONNECTED;
        conn_event(&f->sides[i], &ev);
    }
    if (!pump(f)) return false;
    for (unsigned i = 0; i < 2; ++i) {
        moq_event_t ev;
        NEED(moq_session_state(f->sides[i].session) == MOQ_SESS_ESTABLISHED);
        if (!take(&f->sides[i], MOQ_EVENT_SETUP_COMPLETE, &ev)) return false;
        moq_event_cleanup(&ev);
    }
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    moq_bytes_t part = MOQ_BYTES_LITERAL("commit");
    sc.track_namespace = (moq_namespace_t){&part, 1};
    sc.track_name = MOQ_BYTES_LITERAL("v");
    sc.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t sub;
    NEED(moq_session_subscribe(f->receiver->session, &sc, clock_us, &sub) == MOQ_OK);
    if (!pump(f)) return false;
    moq_event_t ev;
    if (!take(f->sender, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) return false;
    f->sub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    static const uint8_t d16[] = {0x02, 0x40, 0x64};
    static const uint8_t later[] = {0x06, 0x64};
    moq_accept_subscribe_cfg_t accept;
    moq_accept_subscribe_cfg_init(&accept);
    accept.track_properties = version == MOQ_VERSION_DRAFT_16
        ? (moq_bytes_t){d16, sizeof(d16)} : (moq_bytes_t){later, sizeof(later)};
    NEED(moq_session_accept_subscribe(f->sender->session, f->sub, &accept, clock_us) == MOQ_OK);
    if (!pump(f) || !take(f->receiver, MOQ_EVENT_SUBSCRIBE_OK, &ev)) return false;
    moq_event_cleanup(&ev);
    NEED(moq_rcbuf_create(&f->alloc, (const uint8_t *)"payload", 7, &f->payload) == MOQ_OK);
    NEED(moq_session_next_deadline_us(f->sender->session) == UINT64_MAX);
    NEED(fake_msq_pending_sends(&f->sender->fake) == 0);
    return true;
}

static void finish(struct fixture *f, unsigned before, unsigned pre)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct side *s = &f->sides[i];
        if (s->conn) {
            QUIC_CONNECTION_EVENT ev;
            memset(&ev, 0, sizeof(ev));
            ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER;
            conn_event(s, &ev);
            while (fake_msq_deliver_send_complete(&s->fake, true)) {}
            for (int j = 0; j < s->fake.stream_count; ++j) {
                fake_msq_stream_t *st = &s->fake.streams[j];
                if (!st->closed)
                    stream_event(st, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE, false);
            }
            ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE;
            conn_event(s, &ev);
            moq_msquic_conn_destroy(s->conn);
        }
        if (s->session) moq_session_destroy(s->session);
    }
    if (f->payload) {
        CHECK(moq_rcbuf_refcount(f->payload) == 1);
        moq_rcbuf_decref(f->payload);
    }
    CHECK(f->memory.balance == 0);
    printf("%s %s\n", failures == before && prerequisites == pre ? "PASS" : "FAIL", case_name);
    ++cases;
    free(f);
}

static bool group(struct fixture *f, uint64_t group_id, bool data_fin,
                   moq_subgroup_handle_t *sg, fake_msq_stream_t **out)
{
    struct side *s = f->sender;
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group_id;
    int n = s->fake.stream_count;
    NEED(moq_session_open_subgroup(s->session, f->sub, &cfg, clock_us, sg) == MOQ_OK);
    moq_msquic_conn_service(s->conn);
    NEED(s->fake.stream_count == n + 1);
    *out = &s->fake.streams[n];
    fake_msq_deliver_start_complete(*out, QUIC_STATUS_SUCCESS);
    NEED(moq_session_write_object(s->session, *sg, 0, f->payload, clock_us) == MOQ_OK);
    if (!data_fin) moq_msquic_conn_service(s->conn);
    clock_us += 1000;
    NEED(moq_session_close_subgroup(s->session, *sg, clock_us) == MOQ_OK);
    moq_msquic_conn_service(s->conn);
    if (data_fin) {
        NEED((s->fake.sends[s->fake.send_count - 1].flags & QUIC_SEND_FLAG_FIN) != 0);
    } else {
        NEED((*out)->last_shutdown_flags == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);
    }
    return true;
}

static void pending(struct side *s, fake_msq_stream_t *st, uint64_t deadline)
{
    bridge_stream_entry_t *m = mapping(s, st->id);
    CHECK(m != NULL && m->awaiting_commit);
    CHECK(moq_msquic_test_stream_find(s->conn, st->id) != NULL);
    CHECK(!st->closed);
    CHECK(moq_session_next_deadline_us(s->session) == deadline);
}

enum mode { ACK, TIMEOUT, RESET_FIRST, CANCEL, ABORT, PEER_STOP, CLOSE,
            GENERIC, WRONG, REENTRANT, REFUSE, MODE_COUNT };
static const char *const names[] = {"ack", "timeout", "reset-first", "cancel",
    "abort", "peer-stop", "close", "generic", "wrong", "reentrant", "refuse"};

struct reentrant { fake_msq_stream_t *stream; unsigned calls; };
static void reenter(moq_msquic_conn_t *conn, void *ctx)
{
    struct reentrant *r = ctx;
    if (r->calls++) return;
    commit(r->stream);
    moq_msquic_conn_service(conn);
}

static void run(moq_version_t version, bool client, bool data_fin, enum mode mode)
{
    char label[96];
    snprintf(label, sizeof(label), "d%u/%s/%s/%s", (unsigned)version,
             client ? "client" : "server", data_fin ? "data-fin" : "empty-fin", names[mode]);
    case_name = label;
    unsigned before = failures, pre = prerequisites;
    struct fixture *f = calloc(1, sizeof(*f));
    if (!f) { ++prerequisites; return; }
    if (!setup(f, version, client)) goto done;
    struct side *s = f->sender;
    const moq_transport_endpoint_ops_t *ops = moq_msquic_test_ops(s->conn);
    moq_subgroup_handle_t sg;
    fake_msq_stream_t *st;
    if (!group(f, 7, data_fin, &sg, &st)) goto done;
    uint64_t deadline = version == MOQ_VERSION_DRAFT_16 ? UINT64_MAX : clock_us + 100000;
    CHECK((ops->capabilities & MOQ_TRANSPORT_CAP_COMMIT_REPORT) != 0);
    CHECK(s->conn->test_stream_commit_reports == 0);
    pending(s, st, deadline);

    if (mode == CANCEL) {
        CHECK(fake_msq_deliver_send_complete(&s->fake, true));
    } else {
        CHECK(fake_msq_deliver_send_complete(&s->fake, false));
        pending(s, st, deadline); /* only the first (header) send ACKed */
    }
    while (fake_msq_deliver_send_complete(&s->fake, false)) {}
    CHECK(moq_rcbuf_refcount(f->payload) == 1);
    CHECK(s->conn->test_stream_commit_reports == 0);
    pending(s, st, deadline); /* all data ACKed; FIN is still outstanding */

    if (mode == TIMEOUT) {
        s->conn->reliable_reset = true;
        clock_us = deadline - 1;
        moq_msquic_conn_service(s->conn);
        CHECK(st->last_shutdown_flags != QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND);
        clock_us = deadline;
        moq_msquic_conn_service(s->conn);
        CHECK(st->last_shutdown_flags == QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND);
        CHECK(st->last_shutdown_code == MOQ_RESET_DELIVERY_TIMEOUT);
        CHECK(s->conn->reliable_resets == 1);
        CHECK(mapping(s, st->id) == NULL);
        int shutdowns = st->shutdown_calls;
        commit(st);
        stream_event(st, QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE, false);
        CHECK(st->shutdown_calls == shutdowns);
        CHECK(moq_session_next_deadline_us(s->session) == UINT64_MAX);
    } else if (mode == RESET_FIRST || mode == REFUSE) {
        if (mode == REFUSE) {
            s->fake.stream_shutdown_status = QUIC_STATUS_INVALID_STATE;
            CHECK(ops->reset_stream(s->conn, st->id, 0x456) == MOQ_TRANSPORT_ERROR);
            s->fake.stream_shutdown_status = QUIC_STATUS_ABORTED;
            CHECK(ops->reset_stream(s->conn, st->id, 0x456) == MOQ_TRANSPORT_ERROR);
            s->fake.stream_shutdown_status = QUIC_STATUS_SUCCESS;
            commit(st); /* neither failed attempt may win the terminal race */
            CHECK(mapping(s, st->id) == NULL);
            CHECK(moq_session_next_deadline_us(s->session) == UINT64_MAX);
        } else {
            CHECK(ops->reset_stream(s->conn, st->id, 0x456) == MOQ_TRANSPORT_OK);
            CHECK(st->last_shutdown_code == 0x456);
            CHECK(st->last_shutdown_flags == QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND);
            commit(st);
            pending(s, st, deadline); /* direct op: no session terminal was fed */
        }
    } else if (mode == CANCEL || mode == ABORT) {
        if (mode == ABORT) stream_event(st, QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE, false);
        commit(st);
        pending(s, st, deadline);
    } else if (mode == PEER_STOP) {
        stream_event(st, QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED, false);
        int shutdowns = st->shutdown_calls;
        CHECK(st->last_shutdown_flags == QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND);
        CHECK(st->last_shutdown_code == 0x77);
        commit(st);
        CHECK(st->shutdown_calls == shutdowns);
        CHECK(mapping(s, st->id) == NULL);
    } else if (mode == CLOSE) {
        QUIC_CONNECTION_EVENT ev;
        memset(&ev, 0, sizeof(ev));
        ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER;
        conn_event(s, &ev);
        commit(st);
        CHECK(moq_transport_bridge_is_terminal(moq_msquic_test_bridge(s->conn)));
    } else if (mode == GENERIC) {
        stream_event(st, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE, false);
        CHECK(st->closed);
        CHECK(moq_msquic_test_stream_find(s->conn, st->id) == NULL);
        CHECK(moq_session_next_deadline_us(s->session) == deadline);
        /* MsQuic promises this is the last stream callback. Never fabricate
         * another callback using its freed context. New streams are distinct. */
        moq_subgroup_handle_t other_sg;
        fake_msq_stream_t *other;
        if (!group(f, 8, true, &other_sg, &other)) goto done;
        while (fake_msq_deliver_send_complete(&s->fake, false)) {}
        commit(other);
        CHECK(moq_session_next_deadline_us(s->session) == deadline);
    } else {
        if (mode == WRONG) {
            QUIC_STREAM_EVENT ev;
            memset(&ev, 0, sizeof(ev));
            ev.Type = QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE;
            ev.SEND_SHUTDOWN_COMPLETE.Graceful = TRUE;
            (void)st->cb(fake_msq_conn_handle(&s->fake), st->ctx, &ev);
            pending(s, st, deadline);
            uint64_t bidi_id;
            CHECK(ops->open_bidi(s->conn, &bidi_id) == MOQ_TRANSPORT_OK);
            fake_msq_stream_t *bidi = &s->fake.streams[s->fake.stream_count - 1];
            CHECK(ops->write(s->conn, bidi_id, NULL, 0, true) == MOQ_TRANSPORT_OK);
            commit(bidi);
            fake_msq_stream_t *peer = peer_stream(s, client ? 0x103 : 0x102, true);
            CHECK(peer != NULL);
            if (peer) commit(peer);
            uint64_t uni_id;
            CHECK(ops->open_uni(s->conn, &uni_id) == MOQ_TRANSPORT_OK);
            commit(&s->fake.streams[s->fake.stream_count - 1]); /* no FIN */
            pending(s, st, deadline);
        }
        struct reentrant re = {st, 0};
        if (mode == REENTRANT) {
            s->conn->hook = reenter;
            s->conn->hook_user = &re;
        }
        commit(st);
        s->conn->hook = NULL;
        if (mode == REENTRANT) CHECK(re.calls > 0);
        CHECK(mapping(s, st->id) == NULL);
        CHECK(s->conn->test_stream_commit_reports == 1);
        CHECK(s->conn->test_last_committed_stream == st->id);
        CHECK(moq_session_next_deadline_us(s->session) == UINT64_MAX);
        moq_subgroup_handle_t other_sg;
        fake_msq_stream_t *other;
        if (!group(f, 8, !data_fin, &other_sg, &other)) goto done;
        uint64_t other_deadline = version == MOQ_VERSION_DRAFT_16
            ? UINT64_MAX : clock_us + 100000;
        commit(st); /* duplicate/late A cannot retire B */
        pending(s, other, other_deadline);
        int shutdowns = st->shutdown_calls;
        CHECK(ops->reset_stream(s->conn, st->id, 0x456) == MOQ_TRANSPORT_OK);
        CHECK(st->shutdown_calls == shutdowns); /* ACK won; no native abort */
        while (fake_msq_deliver_send_complete(&s->fake, false)) {}
        commit(other);
        CHECK(mapping(s, other->id) == NULL);
        CHECK(moq_session_next_deadline_us(s->session) == UINT64_MAX);
    }
    CHECK(s->conn->test_stream_commit_reports ==
          (mode == ACK || mode == WRONG || mode == REENTRANT ? 2u :
           mode == REFUSE || mode == GENERIC ? 1u : 0u));
done:
    finish(f, before, pre);
}

int main(void)
{
    moq_msq_test_now_us = fake_now;
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v)
        for (unsigned client = 0; client < 2; ++client)
            for (unsigned data = 0; data < 2; ++data)
                for (enum mode m = ACK; m < MODE_COUNT; ++m)
                    run(versions[v], client != 0, data != 0, m);
    run(MOQ_VERSION_DRAFT_16, false, false, ACK);
    run(MOQ_VERSION_DRAFT_16, false, true, ACK);
    moq_msq_test_now_us = NULL;
    printf("%u cases, %u behavioral failures, %u prerequisite failures\n",
           cases, failures, prerequisites);
    return failures || prerequisites ? 1 : 0;
}
