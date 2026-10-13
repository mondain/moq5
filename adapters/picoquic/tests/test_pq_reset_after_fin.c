/*
 * picoquic local-stream reset after FIN (patches/picoquic/0002) and the
 * reliable-reset prefix boundary (patches/picoquic/0005).
 *
 * RFC 9000 Section 3.1 lets a sender abandon a stream with RESET_STREAM in
 * the "Data Sent" state, after the FIN has been sent but before every byte
 * and the FIN are acknowledged. Once all of it is acknowledged ("Data
 * Recvd", a terminal state) there is nothing left to abandon: a reset
 * requested then is refused, and one still unsent when that point is
 * reached is dropped. The draft-18 subgroup DELIVERY_TIMEOUT relies on this:
 * a subgroup whose FIN is in flight must still be cancellable.
 *
 * A reliable reset (RESET_STREAM_AT) becomes sendable once its prefix,
 * bytes 0 .. reliable_size - 1, is acknowledged (patches/picoquic/0005).
 *
 * Constructed rows set stream and SACK state directly to pin one boundary
 * each. Packet rows drive two real picoquic endpoints over the tls_api
 * simulated links, with virtual time and a loss mask, so the frames really
 * go on the wire.
 */
#include <picoquictest_internal.h>
#include <picoquic_utils.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Provider-internal entry points, used only by the constructed rows. */
int picoquic_process_ack_of_stream_frame(picoquic_cnx_t *, uint8_t *, size_t, size_t *);
uint8_t *picoquic_format_reset_stream_frame(picoquic_stream_head_t *, uint8_t *,
    uint8_t *, int *, int *);

/* Lets the rows build against an unpatched provider, where they then fail
 * on behaviour rather than on compilation. Same value as the patch. */
#ifndef PICOQUIC_ERROR_OFFSET_TOO_BIG
#define PICOQUIC_ERROR_OFFSET_TOO_BIG (PICOQUIC_ERROR_CLASS + 72)
#endif

static int failures;
static const char *row;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL[%s]:%d: %s\n", row, __LINE__, #x); failures++; \
} } while (0)

typedef struct {
    picoquic_test_tls_api_ctx_t *tls;
    uint64_t now, loss, blocked, sid, reset_code;
    unsigned resets, fins;
    size_t received;
    int saw_reset_sent;
} pair_t;

static int callback(picoquic_cnx_t *cnx, uint64_t sid, uint8_t *bytes,
                    size_t length, picoquic_call_back_event_t event,
                    void *context, void *stream_context)
{
    pair_t *p = context;
    (void)bytes;
    (void)stream_context;
    if (event == picoquic_callback_stream_data || event == picoquic_callback_stream_fin) {
        p->received += length;
        if (event == picoquic_callback_stream_fin) p->fins++;
    } else if (event == picoquic_callback_stream_reset) {
        p->resets++;
        CHECK(picoquic_get_remote_stream_error(cnx, sid) == p->reset_code);
    }
    return 0;
}

static int setup(pair_t *p)
{
    memset(p, 0, sizeof(*p));
    picoquic_solution_dir = PICOQUIC_SOURCE_DIR;
    if (tls_api_init_ctx(&p->tls, PICOQUIC_INTERNAL_TEST_VERSION_1,
            PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &p->now, NULL, NULL, 0, 1, 0) != 0)
        return -1;
    picoquic_set_callback(p->tls->cnx_client, callback, p);
    picoquic_set_default_callback(p->tls->qserver, callback, p);
    picoquic_tp_t tp = *picoquic_get_transport_parameters(p->tls->cnx_client, 1);
    tp.is_reset_stream_at_enabled = 1;
    picoquic_set_transport_parameters(p->tls->cnx_client, &tp);
    picoquic_set_default_tp_value(p->tls->qserver, picoquic_tp_reset_stream_at, 1);
    if (picoquic_start_client_cnx(p->tls->cnx_client) != 0 ||
        tls_api_connection_loop(p->tls, &p->loss, 0, &p->now) != 0) return -1;
    return 0;
}

/* Runs the simulation; records whether the client's stream p->sid was ever
 * seen with its reset on the wire. */
static void pump(pair_t *p, uint64_t duration)
{
    uint64_t limit = p->now + duration;
    for (unsigned i = 0; i < 10000 && p->now < limit; i++) {
        int active = 0;
        CHECK(tls_api_one_sim_round(p->tls, &p->now, limit, &active) == 0);
        picoquic_stream_head_t *st = picoquic_find_stream(p->tls->cnx_client, p->sid);
        if (st != NULL && st->reset_sent) p->saw_reset_sent = 1;
    }
}

static void open_stream(pair_t *p, int unidir)
{
    p->sid = picoquic_get_next_local_stream_id(p->tls->cnx_client, unidir);
    CHECK(picoquic_set_app_stream_ctx(p->tls->cnx_client, p->sid, p) == 0);
}

/* A local stream whose five bytes and FIN were sent. fin_offset stays 0:
 * picoquic sets it only from received frames, so it says nothing about
 * what this endpoint sent. A UNI stream is closed once its FIN is sent; a
 * BIDI stream stays open while the peer's direction is. */
static picoquic_stream_head_t *fin_sent_stream(pair_t *p, int unidir)
{
    open_stream(p, unidir);
    picoquic_stream_head_t *st = picoquic_find_stream(p->tls->cnx_client, p->sid);
    CHECK(st != NULL);
    if (st == NULL) return NULL;
    st->sent_offset = 5;
    st->fin_requested = st->fin_sent = 1;
    st->is_closed = unidir ? 1 : 0;
    CHECK(st->fin_offset == 0);
    /* The scheduler drops a stream from the output list once its FIN is sent. */
    picoquic_remove_output_stream(p->tls->cnx_client, st);
    CHECK(!st->is_output_stream);
    return st;
}

/* acked: 0 = nothing, 1 = every byte but not the FIN, 2 = bytes and FIN. */
static void constructed_reset(unsigned acked)
{
    static const char *names[] = {
        "constructed-reset-after-fin-unacked",
        "constructed-reset-after-fin-bytes-acked",
        "constructed-reset-after-fin-acked-refused",
    };
    pair_t p;
    row = names[acked];
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = fin_sent_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    /* The FIN occupies the position after the last byte in the SACK list. */
    if (acked) CHECK(picoquic_update_sack_list(&st->sack_list, 0, acked == 1 ? 4 : 5, 0) == 0);
    int rc = picoquic_reset_stream(cnx, p.sid, 42);
    if (acked == 2) {
        /* Refused without side effects: the application keeps its context. */
        CHECK(rc == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED);
        CHECK(!st->reset_requested && st->app_stream_ctx == &p);
    } else {
        CHECK(rc == 0 && st->reset_requested && st->local_error == 42);
        CHECK(st->app_stream_ctx == NULL);
        /* A repeated reset is accepted and keeps the first error code. */
        CHECK(picoquic_reset_stream(cnx, p.sid, 99) == 0);
        CHECK(st->local_error == 42);
        /* Sending the FIN removed the stream from the output list; the
         * accepted reset must put it back or it is never formatted. */
        CHECK(st->is_output_stream);
    }
    tls_api_delete_ctx(p.tls);
}

/* Stream state with a gap at byte 2; the FIN position (5) is acknowledged. */
static picoquic_stream_head_t *gapped_stream(pair_t *p, int unidir)
{
    picoquic_stream_head_t *st = fin_sent_stream(p, unidir);
    if (st == NULL) return NULL;
    CHECK(picoquic_update_sack_list(&st->sack_list, 0, 1, 0) == 0);
    CHECK(picoquic_update_sack_list(&st->sack_list, 3, 5, 0) == 0);
    (void)picoquic_delete_stream_if_closed(p->tls->cnx_client, st);
    CHECK(picoquic_find_stream(p->tls->cnx_client, p->sid) == st);
    return st;
}

/* STREAM|OFF|LEN at offset 2, one byte: fills the gap. */
static void ack_gap(pair_t *p)
{
    uint8_t gap_ack[] = { 0x0e, (uint8_t)p->sid, 2, 1, 'c' };
    size_t consumed = 0;
    CHECK(picoquic_process_ack_of_stream_frame(p->tls->cnx_client, gap_ack,
        sizeof(gap_ack), &consumed) == 0);
}

static void constructed_empty_ack(void)
{
    pair_t p;
    row = "constructed-empty-ack-range";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = gapped_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    /* STREAM|LEN, offset 0, length 0, no FIN: acknowledges nothing. It must
     * not become the range [0, UINT64_MAX] and retire the stream. */
    uint8_t zero_ack[] = { 0x0a, (uint8_t)p.sid, 0 };
    size_t consumed = 0;
    CHECK(picoquic_process_ack_of_stream_frame(cnx, zero_ack, sizeof(zero_ack), &consumed) == 0);
    CHECK(consumed == sizeof(zero_ack));
    CHECK(picoquic_find_stream(cnx, p.sid) == st);
    if (picoquic_find_stream(cnx, p.sid) == st)
        CHECK(!picoquic_check_sack_list(&st->sack_list, 0, 5));
    tls_api_delete_ctx(p.tls);
}

/* Control: the ACK that completes bytes and FIN retires a stream that has
 * no reset. */
static void constructed_fin_ack_retires(void)
{
    pair_t p;
    row = "constructed-fin-ack-retires";
    CHECK(setup(&p) == 0);
    picoquic_stream_head_t *st = gapped_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    ack_gap(&p);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* An ACK that leaves a gap is not the end of the stream: a reset accepted
 * after the FIN stays pending through it. */
static void constructed_partial_ack_keeps_reset(void)
{
    pair_t p;
    row = "constructed-partial-ack-keeps-reset";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = fin_sent_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    CHECK(picoquic_reset_stream(cnx, p.sid, 12) == 0);
    /* Bytes 3..4 and the FIN: byte 0..2 remain unacknowledged. */
    uint8_t tail_ack[] = { 0x0f, (uint8_t)p.sid, 3, 2, 'd', 'e' };
    size_t consumed = 0;
    CHECK(picoquic_process_ack_of_stream_frame(cnx, tail_ack, sizeof(tail_ack), &consumed) == 0);
    st = picoquic_find_stream(cnx, p.sid);
    CHECK(st != NULL);
    if (st != NULL) {
        CHECK(st->reset_requested && !st->reset_sent && st->local_error == 12);
        CHECK(st->is_output_stream);
    }
    tls_api_delete_ctx(p.tls);
}

/* Every byte and the FIN acknowledged while the reset is still unsent: the
 * stream reached "Data Recvd", so the reset is dropped and nothing more is
 * owed. A UNI stream retires as a completed one. A BIDI stream, whose
 * receive side is still open, keeps no pending reset and refuses a new one. */
static void constructed_fin_acked_drops_unsent_reset(int unidir)
{
    pair_t p;
    row = unidir ? "constructed-fin-acked-drops-unsent-reset"
                 : "constructed-fin-acked-drops-unsent-reset-bidi";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = gapped_stream(&p, unidir);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    CHECK(picoquic_reset_stream(cnx, p.sid, 12) == 0);
    ack_gap(&p);
    if (unidir) {
        CHECK(picoquic_find_stream(cnx, p.sid) == NULL);
    } else {
        st = picoquic_find_stream(cnx, p.sid);
        CHECK(st != NULL);
        if (st != NULL) {
            CHECK(!st->reset_requested && !st->reset_sent);
            CHECK(picoquic_reset_stream(cnx, p.sid, 13) == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED);
            CHECK(!st->reset_requested);
        }
    }
    tls_api_delete_ctx(p.tls);
}

/* A reliable reset whose prefix is not acknowledged is never dropped, even
 * when the FIN position and the rest are. */
static void constructed_prefix_unacked_keeps_reset(void)
{
    pair_t p;
    row = "constructed-prefix-unacked-keeps-reset";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = fin_sent_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    CHECK(picoquic_update_sack_list(&st->sack_list, 3, 5, 0) == 0);
    CHECK(picoquic_reset_stream_at(cnx, p.sid, 12, 3) == 0);
    /* Byte 0 only: bytes 1..2 of the three-byte prefix stay unacknowledged. */
    uint8_t first_ack[] = { 0x0a, (uint8_t)p.sid, 1, 'a' };
    size_t consumed = 0;
    CHECK(picoquic_process_ack_of_stream_frame(cnx, first_ack, sizeof(first_ack), &consumed) == 0);
    st = picoquic_find_stream(cnx, p.sid);
    CHECK(st != NULL);
    if (st != NULL) CHECK(st->reset_requested && st->reliable_size == 3);
    tls_api_delete_ctx(p.tls);
}

/* Once the reset is on the wire, the stream is in "Reset Sent": an ACK that
 * completes the FIN does not retire it before the reset is acknowledged,
 * and a repeated reset is accepted and keeps the first error code. */
static void constructed_repeat_reset_after_fin_ack(void)
{
    pair_t p;
    row = "constructed-repeat-reset-after-fin-ack";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = gapped_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    CHECK(picoquic_reset_stream(cnx, p.sid, 12) == 0);
    uint8_t frame[64];
    int more = 0, pure = 1;
    uint8_t *end = picoquic_format_reset_stream_frame(st, frame, frame + sizeof(frame), &more, &pure);
    CHECK(end != NULL && end > frame && st->reset_sent);
    ack_gap(&p);
    st = picoquic_find_stream(cnx, p.sid);
    CHECK(st != NULL);
    if (st != NULL) {
        CHECK(picoquic_check_sack_list(&st->sack_list, 0, 5));
        CHECK(picoquic_reset_stream(cnx, p.sid, 99) == 0);
        CHECK(st->reset_requested && st->local_error == 12);
    }
    tls_api_delete_ctx(p.tls);
}

/* The Reliable Size may not exceed the Final Size. Bytes queued but not yet
 * sent count toward it: they go out before the reset. */
static void constructed_reliable_size_bound(void)
{
    pair_t p;
    row = "constructed-reliable-size-above-final-refused";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = fin_sent_stream(&p, 1);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    CHECK(picoquic_reset_stream_at(cnx, p.sid, 12, 6) == PICOQUIC_ERROR_OFFSET_TOO_BIG);
    CHECK(!st->reset_requested && st->app_stream_ctx == &p && !st->is_output_stream);
    CHECK(picoquic_reset_stream_at(cnx, p.sid, 12, 5) == 0);
    CHECK(st->reset_requested && st->reliable_size == 5);
    tls_api_delete_ctx(p.tls);

    row = "constructed-reliable-size-counts-queued-bytes";
    CHECK(setup(&p) == 0);
    cnx = p.tls->cnx_client;
    open_stream(&p, 1);
    /* A WebTransport-style preamble: queued, not yet sent. */
    CHECK(picoquic_add_to_stream_with_ctx(cnx, p.sid, (const uint8_t *)"\x40\x54\x00", 3, 0, &p) == 0);
    st = picoquic_find_stream(cnx, p.sid);
    CHECK(st != NULL && st->sent_offset == 0);
    CHECK(picoquic_reset_stream_at(cnx, p.sid, 12, 4) == PICOQUIC_ERROR_OFFSET_TOO_BIG);
    CHECK(picoquic_reset_stream_at(cnx, p.sid, 12, 3) == 0);
    if (st != NULL) CHECK(st->reset_requested && st->reliable_size == 3);
    tls_api_delete_ctx(p.tls);
}

/* A received RESET_STREAM_AT whose Reliable Size exceeds its Final Size is
 * a FRAME_ENCODING_ERROR (draft-ietf-quic-reliable-stream-reset Section 4). */
static void constructed_reset_at_receive_bound(void)
{
    pair_t p;
    row = "constructed-reset-at-reliable-above-final";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *srv = p.tls->cnx_server;
    CHECK(srv != NULL);
    if (srv == NULL) { tls_api_delete_ctx(p.tls); return; }
    /* Client-initiated UNI stream 2: final size 2, reliable size 2 is valid. */
    static const uint8_t ok[] = { 0x24, 0x02, 0x07, 0x02, 0x02 };
    picoquic_state_enum before = srv->cnx_state;
    CHECK(before < picoquic_state_disconnecting);
    CHECK(picoquic_decode_reset_stream_at_frame(srv, ok, ok + sizeof(ok)) == ok + sizeof(ok));
    CHECK(srv->cnx_state == before);
    /* Stream 6: final size 2, reliable size 3. */
    static const uint8_t bad[] = { 0x24, 0x06, 0x07, 0x02, 0x03 };
    CHECK(picoquic_decode_reset_stream_at_frame(srv, bad, bad + sizeof(bad)) == NULL);
    CHECK(srv->cnx_state == picoquic_state_disconnecting);
    CHECK(srv->local_error == PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR);
    tls_api_delete_ctx(p.tls);
}

/* One packet carrying RESET_STREAM_AT for stream A, then STREAM+FIN for
 * stream B. Its acknowledgement must retire A's reset and still read B's
 * frame, so the RESET_STREAM_AT handler has to consume all four fields. */
static void constructed_reset_at_ack(void)
{
    pair_t p;
    row = "constructed-reset-stream-at-ack";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    const uint64_t a = 2, b = 6; /* client-initiated UNI */
    CHECK(picoquic_set_app_stream_ctx(cnx, a, &p) == 0);
    CHECK(picoquic_set_app_stream_ctx(cnx, b, &p) == 0);
    picoquic_stream_head_t *sa = picoquic_find_stream(cnx, a);
    picoquic_stream_head_t *sb = picoquic_find_stream(cnx, b);
    CHECK(sa != NULL && sb != NULL);
    if (sa == NULL || sb == NULL) { tls_api_delete_ctx(p.tls); return; }
    sa->sent_offset = 5;
    sa->local_error = 7;
    sa->reliable_size = 2;
    sa->reset_requested = sa->reset_sent = 1;
    sb->sent_offset = 3;
    sb->fin_requested = sb->fin_sent = 1;
    picoquic_packet_t *pkt = picoquic_create_packet(cnx->quic);
    CHECK(pkt != NULL);
    if (pkt == NULL) { tls_api_delete_ctx(p.tls); return; }
    static const uint8_t frames[] = {
        0x24, 0x02, 0x07, 0x05, 0x02,                 /* RESET_STREAM_AT a */
        0x0f, 0x06, 0x00, 0x03, 'x', 'y', 'z',        /* STREAM b, FIN */
    };
    memcpy(pkt->bytes, frames, sizeof(frames));
    pkt->offset = 0;
    pkt->length = sizeof(frames);
    pkt->ptype = picoquic_packet_1rtt_protected;
    pkt->send_path = NULL;
    picoquic_process_ack_of_frames(cnx, pkt, 0);
    picoquic_recycle_packet(cnx->quic, pkt);
    CHECK(picoquic_find_stream(cnx, a) == NULL);
    CHECK(picoquic_find_stream(cnx, b) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* Control: data and FIN delivered and acknowledged retire the stream. */
static void packet_fin_retires(void)
{
    pair_t p;
    row = "packet-fin-retires";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"abc", 3, 1, &p) == 0);
    pump(&p, 200000);
    CHECK(p.received == 3 && p.fins == 1 && p.resets == 0);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* Bytes and FIN sent but lost, then reset. The reset is accepted and goes
 * on the wire; the stream retires once the reset is acknowledged. */
static void packet_reset_after_fin(void)
{
    pair_t p;
    row = "packet-reset-after-fin";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    p.blocked = UINT64_MAX;
    p.tls->c_to_s_link->loss_mask = &p.blocked;
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"abcde", 5, 1, &p) == 0);
    pump(&p, 50000);
    picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && st->fin_sent && st->sent_offset == 5);
    CHECK(st && !picoquic_check_sack_list(&st->sack_list, 0, 5));
    CHECK(p.received == 0);
    p.reset_code = UINT64_C(0x3000000000000001);
    CHECK(picoquic_reset_stream(p.tls->cnx_client, p.sid, p.reset_code) == 0);
    p.blocked = 0;
    pump(&p, 1000000);
    CHECK(p.saw_reset_sent);
    CHECK(p.resets == 1 && p.fins == 0);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* A reliable reset after FIN while the whole stream is lost: the
 * retransmission then delivers every byte and the FIN, so the stream ends
 * in "Data Recvd" and the reset, never sendable before that, is dropped.
 * The peer gets the complete stream and the stream retires. */
static void packet_reliable_reset_all_delivered(void)
{
    pair_t p;
    row = "packet-reliable-reset-all-delivered";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    p.blocked = UINT64_MAX;
    p.tls->c_to_s_link->loss_mask = &p.blocked;
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"abcde", 5, 1, &p) == 0);
    pump(&p, 50000);
    p.reset_code = UINT64_C(0x3000000000000001);
    CHECK(picoquic_reset_stream_at(p.tls->cnx_client, p.sid, p.reset_code, 2) == 0);
    p.blocked = 0;
    pump(&p, 1000000);
    CHECK(!p.saw_reset_sent);
    CHECK(p.received == 5 && p.fins == 1 && p.resets == 0);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* A reliable reset after FIN whose prefix is acknowledged before the rest
 * of the stream. While the prefix is outstanding the reset is not sendable
 * and the scheduler must keep the stream, although its FIN went out long
 * before; once the prefix is acknowledged the RESET_STREAM_AT goes on the
 * wire, and the stream retires when that is acknowledged. */
static void packet_reliable_reset_prefix_first(void)
{
    pair_t p;
    row = "packet-reliable-reset-prefix-first";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    static uint8_t data[8000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    uint64_t c_to_s = 0, s_to_c = UINT64_MAX;
    p.tls->c_to_s_link->loss_mask = &c_to_s;
    p.tls->s_to_c_link->loss_mask = &s_to_c;
    /* The 1000-byte prefix reaches the peer; its acknowledgement is lost. */
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid, data, 1000, 0, &p) == 0);
    pump(&p, 20000);
    CHECK(p.received == 1000);
    /* The rest and the FIN are lost. */
    c_to_s = UINT64_MAX;
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        data + 1000, sizeof(data) - 1000, 1, &p) == 0);
    pump(&p, 50000);
    picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && st->fin_sent && st->sent_offset == sizeof(data));
    CHECK(st && !picoquic_check_sack_list(&st->sack_list, 0, 999));
    p.reset_code = UINT64_C(0x3000000000000002);
    CHECK(picoquic_reset_stream_at(p.tls->cnx_client, p.sid, p.reset_code, 1000) == 0);
    /* Scheduler passes while the prefix is outstanding. */
    pump(&p, 20000);
    /* Acknowledgements flow again and the first client packet gets through:
     * the repeated prefix is acknowledged, everything after it is not. */
    s_to_c = 0;
    c_to_s = ~UINT64_C(1);
    pump(&p, 300000);
    st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && !picoquic_check_sack_list(&st->sack_list, 0, sizeof(data)));
    c_to_s = 0;
    pump(&p, 2000000);
    CHECK(p.saw_reset_sent);
    /* Repeats already queued before the reset may still complete the
     * stream at the peer; the reset only guarantees the prefix. */
    CHECK(p.received >= 1000);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* A reliable reset whose prefix is the whole stream, with no FIN: the
 * WebTransport preamble-only reset. The prefix check takes an inclusive last
 * offset, so the reset becomes sendable once byte reliable_size - 1 is
 * acknowledged; it reaches the peer, and the stream retires on its ACK.
 * queued = 1 resets while the preamble is still queued, queued = 0 after
 * it is acknowledged. */
static void packet_reliable_reset_whole_prefix(int queued)
{
    pair_t p;
    row = queued ? "packet-reliable-reset-queued-preamble-no-fin"
                 : "packet-reliable-reset-acked-preamble-no-fin";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"\x40\x54\x00", 3, 0, &p) == 0);
    if (!queued) {
        pump(&p, 100000);
        picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
        CHECK(st && st->sent_offset == 3 && picoquic_check_sack_list(&st->sack_list, 0, 2));
        CHECK(p.received == 3);
    }
    p.reset_code = 0x17;
    CHECK(picoquic_reset_stream_at(p.tls->cnx_client, p.sid, p.reset_code, 3) == 0);
    pump(&p, 1000000);
    CHECK(p.saw_reset_sent);
    CHECK(p.received == 3 && p.resets == 1 && p.fins == 0);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    tls_api_delete_ctx(p.tls);
}

/* Reliable-reset prefix boundaries, on constructed stream state with no
 * FIN. prefix = 0: a plain reset is immediately sendable as RESET_STREAM,
 * and the stream cannot be made active again. prefix = 5: with bytes 0..3
 * acknowledged the reset is neither selected nor formatted and the stream
 * may still be activated to send the prefix; once byte 4 (the last prefix
 * byte, not byte 5) is acknowledged the reset is selected, formatted as
 * RESET_STREAM_AT, and activation is refused. */
static void constructed_prefix_boundary(uint64_t prefix)
{
    pair_t p;
    row = prefix == 0 ? "constructed-prefix-empty" : "constructed-prefix-last-byte";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    uint64_t sid = picoquic_get_next_local_stream_id(cnx, 1);
    p.sid = sid;
    CHECK(picoquic_mark_active_stream(cnx, sid, 0, NULL) == 0);
    picoquic_stream_head_t *st = picoquic_find_stream(cnx, sid);
    CHECK(st != NULL);
    if (st == NULL) { tls_api_delete_ctx(p.tls); return; }
    st->sent_offset = prefix;
    CHECK(picoquic_reset_stream_at(cnx, sid, 1, prefix) == 0);
    uint8_t bytes[64];
    int more = 0, pure_ack = 1, active = 0, error = 0;
    if (prefix > 0) {
        CHECK(picoquic_update_sack_list(&st->sack_list, 0, prefix - 2, 0) == 0);
        CHECK(picoquic_mark_active_stream(cnx, sid, 1, NULL) == 0);
        CHECK(picoquic_mark_active_stream(cnx, sid, 0, NULL) == 0);
        CHECK(picoquic_find_ready_stream(cnx) != st);
        CHECK(picoquic_format_stream_frame(cnx, st, bytes, bytes + sizeof(bytes),
              &more, &pure_ack, &active, &error) == bytes);
        CHECK(error == 0 && st->reset_sent == 0);
        CHECK(picoquic_update_sack_list(&st->sack_list, prefix - 1, prefix - 1, 0) == 0);
        CHECK(picoquic_check_sack_list(&st->sack_list, 0, prefix - 1) != 0);
        CHECK(picoquic_check_sack_list(&st->sack_list, 0, prefix) == 0);
    }
    CHECK(picoquic_mark_active_stream(cnx, sid, 1, NULL) == PICOQUIC_ERROR_CANNOT_SET_ACTIVE_STREAM);
    CHECK(picoquic_mark_active_stream(cnx, sid, 0, NULL) == 0);
    CHECK(picoquic_find_ready_stream(cnx) == st);
    more = 0; pure_ack = 1; active = 0; error = 0;
    uint8_t *end = picoquic_format_stream_frame(cnx, st, bytes, bytes + sizeof(bytes),
        &more, &pure_ack, &active, &error);
    CHECK(end != NULL && end > bytes);
    CHECK(error == 0 && pure_ack == 0 && st->reset_sent != 0);
    if (end != NULL && end > bytes) {
        uint64_t type = 0;
        CHECK(picoquic_frames_varint_decode(bytes, end, &type) != NULL);
        CHECK(type == (prefix == 0 ? picoquic_frame_type_reset_stream :
                                     picoquic_frame_type_reset_stream_at));
    }
    tls_api_delete_ctx(p.tls);
}

/* A bidirectional stream outlives its acknowledged FIN while the peer's
 * direction is open. Its send side is then in "Data Recvd": the reset is
 * refused, the application context stays, and nothing reaches the peer. */
static void packet_reset_after_fin_acked(void)
{
    pair_t p;
    row = "packet-reset-after-fin-acked-refused";
    CHECK(setup(&p) == 0);
    open_stream(&p, 0);
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"abc", 3, 1, &p) == 0);
    pump(&p, 200000);
    CHECK(p.received == 3 && p.fins == 1);
    picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && st->fin_sent && picoquic_check_sack_list(&st->sack_list, 0, 3));
    p.reset_code = 5;
    CHECK(picoquic_reset_stream(p.tls->cnx_client, p.sid, 5) == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED);
    st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && !st->reset_requested && st->app_stream_ctx == &p);
    pump(&p, 200000);
    CHECK(p.resets == 0 && !p.saw_reset_sent);
    tls_api_delete_ctx(p.tls);
}

int main(void)
{
    for (unsigned acked = 0; acked < 3; acked++) constructed_reset(acked);
    constructed_empty_ack();
    constructed_fin_ack_retires();
    constructed_partial_ack_keeps_reset();
    constructed_fin_acked_drops_unsent_reset(1);
    constructed_fin_acked_drops_unsent_reset(0);
    constructed_prefix_unacked_keeps_reset();
    constructed_repeat_reset_after_fin_ack();
    constructed_reliable_size_bound();
    constructed_reset_at_receive_bound();
    constructed_reset_at_ack();
    packet_fin_retires();
    packet_reset_after_fin();
    packet_reliable_reset_all_delivered();
    packet_reliable_reset_prefix_first();
    packet_reset_after_fin_acked();
    constructed_prefix_boundary(0);
    constructed_prefix_boundary(5);
    packet_reliable_reset_whole_prefix(1);
    packet_reliable_reset_whole_prefix(0);
    printf("reset-after-fin failures=%d\n", failures);
    return failures != 0;
}
