/* Pinned provider terminal contract. Packet rows use tls_api's simulated
 * QUIC links; constructed rows explicitly manufacture stream/SACK state. */
#include <picoquictest_internal.h>
#include <picoquic_utils.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Existing provider-internal entry point, used only for constructed ACK rows. */
int picoquic_process_ack_of_stream_frame(picoquic_cnx_t *, uint8_t *, size_t, size_t *);

static int failures;
static const char *row;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL[%s]:%d: %s\n", row, __LINE__, #x); failures++; \
} } while (0)

typedef struct {
    picoquic_test_tls_api_ctx_t *tls;
    h3zero_callback_ctx_t *h3;
    uint64_t now, loss, blocked, sid, stop_code, reset_code;
    unsigned commits, stops, resets, fins, frees, reenter;
    size_t received;
} pair_t;

/* Baseline uses only old public APIs so missing support fails behaviourally.
 * These fallbacks are test controls, never production compatibility shims. */
static int observe(picoquic_cnx_t *cnx, uint64_t sid)
{
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    return picoquic_enable_stream_send_commit(cnx, sid);
#else
    (void)cnx; (void)sid;
    return 0;
#endif
}

static int stop_error(picoquic_cnx_t *cnx, uint64_t sid, uint64_t *out)
{
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    return picoquic_get_remote_stop_error(cnx, sid, out);
#else
    *out = picoquic_get_remote_stream_error(cnx, sid);
    return 0;
#endif
}

static int callback(picoquic_cnx_t *cnx, uint64_t sid, uint8_t *bytes,
                    size_t length, picoquic_call_back_event_t event,
                    void *context, void *stream_context)
{
    pair_t *p = context;
    (void)bytes;
    if (event == picoquic_callback_stream_data || event == picoquic_callback_stream_fin) {
        p->received += length;
        if (event == picoquic_callback_stream_fin) p->fins++;
    } else if (event == picoquic_callback_stop_sending) {
        p->stops++;
        uint64_t code = UINT64_MAX;
        CHECK(stop_error(cnx, sid, &code) == 0);
        CHECK(code == p->stop_code);
        CHECK(picoquic_get_remote_stream_error(cnx, sid) != code || code == 0);
        /* The FIN-loss phase ends at the causal STOP callback. */
        p->blocked = 0;
        CHECK(picoquic_reset_stream(cnx, sid, code) == 0);
    } else if (event == picoquic_callback_stream_reset) {
        p->resets++;
        CHECK(picoquic_get_remote_stream_error(cnx, sid) == p->reset_code);
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    } else if (event == picoquic_callback_stream_send_committed) {
        CHECK(sid == p->sid);
        CHECK(stream_context == p);
        p->commits++;
        picoquic_stream_head_t *st = picoquic_find_stream(cnx, sid);
        CHECK(st != NULL && st->fin_sent);
        CHECK(st && picoquic_check_sack_list(&st->sack_list, 0, st->sent_offset));
        if (p->reenter) {
            CHECK(picoquic_reset_stream(cnx, sid, 123) == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED);
            CHECK(st->app_stream_ctx == p);
            CHECK(observe(cnx, sid) != 0);
            picoquic_unlink_app_stream_ctx(cnx, sid);
            CHECK(picoquic_close(cnx, 0) == 0);
        }
#endif
    }
    (void)stream_context;
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

static void pump(pair_t *p, uint64_t duration)
{
    uint64_t limit = p->now + duration;
    for (unsigned i = 0; i < 10000 && p->now < limit; i++) {
        int active = 0;
        CHECK(tls_api_one_sim_round(p->tls, &p->now, limit, &active) == 0);
    }
}

static void cleanup(pair_t *p)
{
    if (p->h3) {
        h3zero_callback_delete_context(p->tls->cnx_client, p->h3);
        picoquic_set_callback(p->tls->cnx_client, callback, p);
    }
    tls_api_delete_ctx(p->tls);
}

static void open_stream(pair_t *p, int opt_in)
{
    p->sid = picoquic_get_next_local_stream_id(p->tls->cnx_client, 1);
    CHECK(picoquic_set_app_stream_ctx(p->tls->cnx_client, p->sid, p) == 0);
    if (opt_in) CHECK(observe(p->tls->cnx_client, p->sid) == 0);
}

static void packet_ack(int empty, int opt_in, int reenter)
{
    pair_t p;
    row = !opt_in ? "packet-no-opt-in" : empty ? "packet-empty-fin" : "packet-data-fin";
    CHECK(setup(&p) == 0);
    open_stream(&p, opt_in);
    p.reenter = (unsigned)reenter;
    if (!empty) {
        CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
            (const uint8_t *)"abc", 3, 0, &p) == 0);
        pump(&p, 100000);
        CHECK(p.received == 3 && p.commits == 0);
        picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
        CHECK(st && picoquic_check_sack_list(&st->sack_list, 0, 2));
        CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
            (const uint8_t *)"de", 2, 0, &p) == 0);
        pump(&p, 100000);
        CHECK(p.received == 5 && p.commits == 0);
        st = picoquic_find_stream(p.tls->cnx_client, p.sid);
        CHECK(st && picoquic_check_sack_list(&st->sack_list, 0, 4));
    }
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid, NULL, 0, 1, &p) == 0);
    pump(&p, 200000);
    CHECK(p.commits == (unsigned)opt_in);
    CHECK(p.fins == 1);
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    cleanup(&p);
}

static void packet_reset(int reliable)
{
    pair_t p;
    row = reliable ? "packet-pending-prefix-reset-after-fin" : "packet-timer-reset-after-fin";
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
    CHECK(p.commits == 0 && p.received == 0);
    p.reset_code = UINT64_C(0x3000000000000001);
    CHECK(picoquic_reset_stream_at(p.tls->cnx_client, p.sid, p.reset_code, reliable ? 2 : 0) == 0);
    p.blocked = 0;
    pump(&p, 1000000);
    CHECK(p.commits == 0);
    if (!reliable) CHECK(p.resets == 1 && p.fins == 0);
    /* For reliable reset the original packet may deliver FIN first. Native
     * retirement still requires the RESET acknowledgement, not that FIN ACK. */
    CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    cleanup(&p);
}

static void packet_stop(uint64_t code, int after_fin)
{
    pair_t p;
    row = after_fin ? "packet-stop-after-fin" : "packet-stop-before-fin";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    p.stop_code = p.reset_code = code;
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"abcde", 5, 0, &p) == 0);
    pump(&p, 100000);
    if (after_fin) {
        p.blocked = UINT64_MAX;
        p.tls->c_to_s_link->loss_mask = &p.blocked;
        CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid, NULL, 0, 1, &p) == 0);
        pump(&p, 30000);
        picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
        CHECK(st && st->fin_sent);
    }
    CHECK(picoquic_stop_sending(p.tls->cnx_server, p.sid, code) == 0);
    pump(&p, 30000);
    p.blocked = 0;
    pump(&p, 300000);
    printf("%s code=%" PRIu64 " stop=%u reset=%u commit=%u fin=%u\n",
        row, code, p.stops, p.resets, p.commits, p.fins);
    CHECK(p.stops == 1 && p.commits == 0 && p.resets == 1);
    cleanup(&p);
}

static void packet_close_winner(void)
{
    pair_t p;
    row = "packet-teardown-before-fin-ack";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    p.blocked = UINT64_MAX;
    p.tls->s_to_c_link->loss_mask = &p.blocked;
    CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
        (const uint8_t *)"data", 4, 1, &p) == 0);
    pump(&p, 50000);
    picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    CHECK(st && st->fin_sent && !picoquic_check_sack_list(&st->sack_list, 0, 4));
    CHECK(p.fins == 1 && p.commits == 0);
    CHECK(picoquic_close(p.tls->cnx_client, 0) == 0);
    p.blocked = 0;
    pump(&p, 200000);
    cleanup(&p);
    CHECK(p.commits == 0);
}

static void constructed_reset(unsigned mode)
{
    pair_t p;
    row = "constructed-reset-boundary";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = picoquic_find_stream(cnx, p.sid);
    st->sent_offset = 5;
    st->fin_requested = st->fin_sent = st->is_closed = 1;
    CHECK(st->fin_offset == 0);
    if (mode) CHECK(picoquic_update_sack_list(&st->sack_list, 0, mode == 1 ? 4 : 5, 0) == 0);
    int rc = picoquic_reset_stream(cnx, p.sid, 42);
    if (mode == 2) {
        CHECK(rc == PICOQUIC_ERROR_STREAM_ALREADY_CLOSED);
        CHECK(!st->reset_requested && st->app_stream_ctx == &p);
    } else {
        CHECK(rc == 0 && st->reset_requested && st->local_error == 42);
        CHECK(picoquic_reset_stream(cnx, p.sid, 99) == 0);
        CHECK(st->local_error == 42);
        /* In credit, the reset is formatted and queued as soon as it is
         * accepted (provider queue/format state, not wire transmission). */
        CHECK(st->reset_sent);
    }
    cleanup(&p);
}

static void constructed_getter(void)
{
    pair_t p;
    row = "constructed-getter-absence-and-wrong-field";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    uint64_t out = UINT64_C(0xbeef);
    CHECK(stop_error(p.tls->cnx_client, p.sid, &out) != 0);
    CHECK(out == UINT64_C(0xbeef));
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    CHECK(stop_error(NULL, p.sid, &out) != 0);
    CHECK(stop_error(p.tls->cnx_client, p.sid, NULL) != 0);
#endif
    picoquic_stream_head_t *st = picoquic_find_stream(p.tls->cnx_client, p.sid);
    st->remote_error = 99;
    st->remote_stop_error = 0;
    st->stop_sending_received = 1;
    CHECK(stop_error(p.tls->cnx_client, p.sid, &out) == 0 && out == 0);
    st->remote_stop_error = UINT64_C(0x3fffffffffffffff);
    CHECK(stop_error(p.tls->cnx_client, p.sid, &out) == 0 && out == UINT64_C(0x3fffffffffffffff));
    cleanup(&p);
}

static void constructed_ack(unsigned cancel)
{
    pair_t p;
    row = "constructed-partial-gap-stale-ack-and-cancellation";
    CHECK(setup(&p) == 0);
    open_stream(&p, 1);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    picoquic_stream_head_t *st = picoquic_find_stream(cnx, p.sid);
    st->sent_offset = 5;
    st->fin_requested = st->fin_sent = st->is_closed = 1;
    CHECK(picoquic_update_sack_list(&st->sack_list, 0, 1, 0) == 0);
    CHECK(picoquic_update_sack_list(&st->sack_list, 3, 5, 0) == 0);
    (void)picoquic_delete_stream_if_closed(cnx, st);
    CHECK(p.commits == 0 && picoquic_find_stream(cnx, p.sid) == st);
    /* A zero-length non-FIN ACK must not underflow to an all-covering range. */
    uint8_t zero_ack[] = { 0x0a, (uint8_t)p.sid, 0 };
    size_t consumed = 0;
    CHECK(picoquic_process_ack_of_stream_frame(cnx, zero_ack, sizeof(zero_ack), &consumed) == 0);
    CHECK(p.commits == 0 && picoquic_find_stream(cnx, p.sid) == st);
    if (picoquic_find_stream(cnx, p.sid) == NULL) { cleanup(&p); return; }
    if (cancel == 1) CHECK(picoquic_reset_stream(cnx, p.sid, 12) == 0);
    if (cancel == 2) CHECK(picoquic_close(cnx, 0) == 0);
    if (cancel == 3) st->stop_sending_received = 1;
    uint8_t gap_ack[] = { 0x0e, (uint8_t)p.sid, 2, 1, 'c' };
    CHECK(picoquic_process_ack_of_stream_frame(cnx, gap_ack, sizeof(gap_ack), &consumed) == 0);
    CHECK(p.commits == (cancel == 0 ? 1u : 0u));
    CHECK(picoquic_process_ack_of_stream_frame(cnx, gap_ack, sizeof(gap_ack), &consumed) == 0);
    CHECK(p.commits == (cancel == 0 ? 1u : 0u));
    if (cancel == 1) {
        /* This in-credit reset was formatted and queued as soon as it was
         * accepted (reset_sent is the provider's queue/format state, not
         * proof that a packet left). This provider keeps a reset stream
         * until the reset itself is acknowledged, so the ACK that completes
         * the bytes and the FIN does not retire it; RFC 9000 §13.3 would
         * also let a sender stop at that ACK, so this pins the provider's
         * policy rather than an RFC requirement. Either way the reset
         * suppressed the committed callback, on both ACKs. */
        picoquic_stream_head_t *kept = picoquic_find_stream(cnx, p.sid);
        CHECK(kept == st);
        if (kept != NULL)
            CHECK(kept->reset_requested && kept->reset_sent);
    }
    cleanup(&p);
}

static void constructed_opt_in(void)
{
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    pair_t p;
    row = "constructed-opt-in-validation";
    CHECK(setup(&p) == 0);
    picoquic_cnx_t *cnx = p.tls->cnx_client;
    CHECK(observe(NULL, 2) != 0);
    CHECK(observe(cnx, 2) != 0);
    CHECK(picoquic_set_app_stream_ctx(cnx, 0, &p) == 0);
    CHECK(observe(cnx, 0) != 0);
    open_stream(&p, 1);
    CHECK(observe(cnx, p.sid) == 0);
    picoquic_stream_head_t *st = picoquic_find_stream(cnx, p.sid);
    st->sent_offset = 1;
    CHECK(observe(cnx, p.sid) != 0 && st->app_stream_ctx == &p);
    cleanup(&p);
#endif
}

static int h3_path(picoquic_cnx_t *cnx, uint8_t *bytes, size_t length,
                   picohttp_call_back_event_t event, h3zero_stream_ctx_t *sc,
                   void *context)
{
    pair_t *p = context;
    (void)bytes; (void)length;
    if (event == picohttp_callback_free) p->frees++;
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
    if (event == picohttp_callback_stream_send_committed) {
        p->commits++;
        CHECK(sc->stream_id == p->sid);
        CHECK(h3zero_find_stream(p->h3, p->sid) == sc);
        CHECK(picoquic_find_stream(cnx, p->sid) != NULL);
        if (p->reenter) h3zero_delete_stream(cnx, p->h3, sc);
    }
#endif
    (void)cnx; (void)sc;
    return 0;
}

static void packet_h3_churn(void)
{
    pair_t p;
    row = "packet-h3-forwarding-cleanup-churn";
    CHECK(setup(&p) == 0);
    p.h3 = h3zero_callback_create_context(NULL);
    CHECK(p.h3 != NULL);
    p.h3->no_print = 1;
    picoquic_set_callback(p.tls->cnx_client, h3zero_callback, p.h3);
    unsigned expected_commits = 0;
    for (unsigned i = 0; i < 32; i++) {
        h3zero_stream_ctx_t *sc = picowt_create_local_stream(p.tls->cnx_client, 0, p.h3, 0);
        CHECK(sc != NULL);
        if (!sc) break;
        p.sid = sc->stream_id;
        sc->path_callback = h3_path;
        sc->path_callback_ctx = &p;
        CHECK(observe(p.tls->cnx_client, p.sid) == 0);
        p.reenter = i & 1u;
        if (i % 3 == 0) {
            /* H3 owns its application context; explicit cancellation releases
             * it after the native reset accepts. Generic free is not ACK. */
            CHECK(picoquic_reset_stream(p.tls->cnx_client, p.sid, 0) == 0);
            h3zero_delete_stream(p.tls->cnx_client, p.h3, sc);
        } else {
            CHECK(picoquic_add_to_stream_with_ctx(p.tls->cnx_client, p.sid,
                (const uint8_t *)"x", 1, 1, sc) == 0);
            expected_commits++;
        }
        pump(&p, 150000);
        CHECK(p.commits == expected_commits && p.frees == i + 1);
        CHECK(p.h3->h3_stream_tree.size == 0);
        CHECK(picoquic_find_stream(p.tls->cnx_client, p.sid) == NULL);
    }
    h3zero_stream_ctx_t *sc = picowt_create_local_stream(p.tls->cnx_client, 0, p.h3, 0);
    CHECK(sc != NULL);
    if (sc) {
        sc->path_callback = h3_path;
        sc->path_callback_ctx = &p;
        CHECK(observe(p.tls->cnx_client, sc->stream_id) == 0);
    }
    cleanup(&p);
    CHECK(p.commits == expected_commits && p.frees == 33);
}

int main(void)
{
    for (unsigned mode = 0; mode < 3; mode++) constructed_reset(mode);
    constructed_getter();
    constructed_opt_in();
    for (unsigned cancel = 0; cancel < 4; cancel++) constructed_ack(cancel);
    packet_ack(0, 0, 0);
    packet_ack(1, 1, 0);
    packet_ack(0, 1, 0);
    packet_ack(0, 1, 1);
    packet_reset(0);
    packet_reset(1);
    packet_stop(0, 0);
    packet_stop(17, 0);
    packet_stop(UINT64_C(0x3fffffffffffffff), 0);
    packet_stop(17, 1);
    packet_close_winner();
    packet_h3_churn();
    printf("terminal-contract failures=%d\n", failures);
    return failures != 0;
}
