/*
 * The version offer that reaches the wire follows what the picoquic family
 * can carry.
 *
 * A plain picoquic + h3zero server on loopback records what each client
 * connection actually offers -- the TLS ALPN list for raw QUIC, the
 * WT-Available-Protocols header of the WebTransport CONNECT -- and then
 * refuses it. The endpoint client offers draft-22 only when this build's
 * picoquic reports exact stream terminal outcomes
 * (PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION >= 1); otherwise AUTO leaves it
 * out, a preference only orders the rest, an explicit list keeps its order,
 * and moq_endpoint_connect refuses an EXACT draft-22 (it resolves the offer
 * before starting any connection, so none is attempted; this test checks the
 * refusal, not the absence of packets). The picoquic and pico_wt facades
 * refuse a draft-22 offer at create on such a build. The same expectations
 * hold with the contract present, with draft-22 included. Only the offer is
 * checked: the server refuses every connection, so no session is
 * established.
 *
 *   test_endpoint_offer_wire --cert <pem> --key <pem>
 */
#include "test_support.h"

#include <moq/endpoint.h>
#include <moq/picoquic_threaded.h>
#include <moq/pico_wt_managed.h>

#include <h3zero.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>
#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION) && \
    PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION >= 1
#define HOSTS_D22 1
#else
#define HOSTS_D22 0
#endif

static int failures = 0;

/* -- what the server saw (written on its network thread) -------------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_alpn[256];      /* comma-joined ALPN offer of the last client */
static bool g_alpn_seen;
static char g_wt[256];        /* WT-Available-Protocols tokens, comma-joined */
static bool g_wt_seen;

static void reset_seen(void)
{
    pthread_mutex_lock(&g_mu);
    g_alpn[0] = g_wt[0] = '\0';
    g_alpn_seen = g_wt_seen = false;
    pthread_mutex_unlock(&g_mu);
}

/* Record the client's ALPN offer. Select "h3" when offered, so a
 * WebTransport client reaches its CONNECT; refuse anything else. */
static size_t capture_alpn(picoquic_quic_t *quic, picoquic_iovec_t *list,
                           size_t count)
{
    (void)quic;
    size_t pick = count;
    pthread_mutex_lock(&g_mu);
    g_alpn[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        size_t used = strlen(g_alpn);
        snprintf(g_alpn + used, sizeof(g_alpn) - used, "%s%.*s", i ? "," : "",
                 (int)list[i].len, (const char *)list[i].base);
        if (list[i].len == 2 && memcmp(list[i].base, "h3", 2) == 0) pick = i;
    }
    g_alpn_seen = true;
    pthread_mutex_unlock(&g_mu);
    return pick;
}

/* Record the CONNECT's WT-Available-Protocols as bare tokens, then refuse
 * the CONNECT. */
static int capture_connect(picoquic_cnx_t *cnx, uint8_t *bytes, size_t length,
                           picohttp_call_back_event_t event,
                           h3zero_stream_ctx_t *stream_ctx, void *app_ctx)
{
    (void)cnx; (void)bytes; (void)length; (void)app_ctx;
    if (event != picohttp_callback_connect) return 0;
    const uint8_t *p = stream_ctx->ps.stream_state.header.wt_available_protocols;
    size_t n = stream_ctx->ps.stream_state.header.wt_available_protocols_length;
    pthread_mutex_lock(&g_mu);
    size_t o = 0;
    for (size_t i = 0; p && i < n && o + 1 < sizeof(g_wt); i++) {
        if (p[i] == '"' || p[i] == ' ' || p[i] == '\t') continue;
        g_wt[o++] = (char)p[i];
    }
    g_wt[o] = '\0';
    g_wt_seen = true;
    pthread_mutex_unlock(&g_mu);
    return -1;
}

static int loop_cb(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum ev,
                   void *ctx, void *arg)
{
    (void)quic; (void)ev; (void)ctx; (void)arg;
    return 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Wait until the server recorded the offer of interest, at most 5 s. */
static bool wait_seen(bool wt)
{
    uint64_t deadline = now_ms() + 5000u;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        bool seen = wt ? g_wt_seen : g_alpn_seen;
        pthread_mutex_unlock(&g_mu);
        if (seen) return true;
        if (now_ms() >= deadline) return false;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static int g_port;

static moq_endpoint_cfg_t client_cfg(char *url, size_t url_len, bool wt)
{
    if (wt) snprintf(url, url_len, "https://127.0.0.1:%d/moq", g_port);
    else    snprintf(url, url_len, "moqt://127.0.0.1:%d", g_port);
    moq_endpoint_cfg_t c;
    moq_endpoint_cfg_init_sized(&c, sizeof(c));
    c.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    /* The server's certificate is not the point here: the offer is. */
    c.insecure_skip_verify = true;
    return c;
}

/* Connect with `c`, return what the server recorded (ALPN or WT tokens). */
static moq_result_t offer_of(const moq_endpoint_cfg_t *c, bool wt, char *out,
                             size_t out_len)
{
    reset_seen();
    out[0] = '\0';
    moq_endpoint_t *ep = NULL;
    moq_result_t rc = moq_endpoint_connect(c, &ep);
    if (rc != MOQ_OK) return rc;
    bool seen = wait_seen(wt);
    moq_endpoint_stop(ep);
    moq_endpoint_destroy(ep);
    pthread_mutex_lock(&g_mu);
    snprintf(out, out_len, "%s", wt ? g_wt : g_alpn);
    pthread_mutex_unlock(&g_mu);
    return seen ? MOQ_OK : MOQ_ERR_INTERNAL;   /* nothing reached the server */
}

static void expect_offer(const char *name, moq_endpoint_cfg_t *c, bool wt,
                         const char *want)
{
    char got[256];
    moq_result_t rc = offer_of(c, wt, got, sizeof(got));
    printf("%-34s %s offered: %s\n", name, wt ? "WT-Available-Protocols" : "ALPN",
           got[0] ? got : "(nothing)");
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: offered '%s', expected '%s'\n", name, got, want);
        failures++;
    }
}

static int pq_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                   uint64_t now_us, void *user)
{
    (void)t; (void)lane; (void)now_us; (void)user;
    return 0;
}

static int wt_pump(moq_pico_wt_managed_t *m, uint64_t now_us, void *ctx)
{
    (void)m; (void)now_us; (void)ctx;
    return 0;
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--cert") == 0) cert = argv[i + 1];
        else if (strcmp(argv[i], "--key") == 0) key = argv[i + 1];
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: %s --cert <pem> --key <pem>\n", argv[0]);
        return 2;
    }
    printf("picoquic stream-terminal contract: %s\n", HOSTS_D22 ? "present" : "absent");

    /* A loopback UDP port free at this moment. */
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t al = sizeof(a);
        MOQ_TEST_CHECK(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
                       getsockname(s, (struct sockaddr *)&a, &al) == 0);
        g_port = ntohs(a.sin_port);
        close(s);
    }

    picohttp_server_path_item_t path_table[1];
    path_table[0] = (picohttp_server_path_item_t){
        .path = "/moq", .path_length = 4,
        .path_callback = capture_connect, .path_app_ctx = NULL,
        .connect_protocol = H3ZERO_WEBTRANSPORT_H3_PROTOCOL,
        .connect_protocol_length = sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1 };
    picohttp_server_parameters_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.path_table = path_table;
    sp.path_table_nb = 1;

    picoquic_quic_t *quic = picoquic_create(4, cert, key, NULL, NULL,
        h3zero_callback, &sp, NULL, NULL, NULL, picoquic_current_time(),
        NULL, NULL, NULL, 0);
    MOQ_TEST_CHECK(quic != NULL);
    if (!quic) return 1;
    picoquic_set_alpn_select_fn_v2(quic, capture_alpn);
    picowt_set_default_transport_parameters(quic);
    picoquic_packet_loop_param_t lp;
    memset(&lp, 0, sizeof(lp));
    lp.local_port = (uint16_t)g_port;
    lp.local_af = AF_INET;
    int thread_ret = 0;
    picoquic_network_thread_ctx_t *nt =
        picoquic_start_network_thread(quic, &lp, loop_cb, NULL, &thread_ret);
    MOQ_TEST_CHECK(nt != NULL);
    if (!nt) { picoquic_free(quic); return 1; }

    static const moq_version_t d22[1] = { MOQ_VERSION_DRAFT_22 };
    static const moq_version_t d18_d16[2] = { MOQ_VERSION_DRAFT_18,
                                              MOQ_VERSION_DRAFT_16 };
    char url[96];

    /* == Raw QUIC ======================================================= */
    {
        moq_endpoint_cfg_t c = client_cfg(url, sizeof(url), false);
        expect_offer("raw AUTO", &c, false,
                     HOSTS_D22 ? "moqt-22,moqt-18,moqt-16" : "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), false);
        c.preferred_version = MOQ_VERSION_DRAFT_16;
        expect_offer("raw AUTO preferring draft-16", &c, false,
                     HOSTS_D22 ? "moqt-16,moqt-22,moqt-18" : "moqt-16,moqt-18");

        c = client_cfg(url, sizeof(url), false);
        c.preferred_version = MOQ_VERSION_DRAFT_22;
        expect_offer("raw AUTO preferring draft-22", &c, false,
                     HOSTS_D22 ? "moqt-22,moqt-18,moqt-16" : "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), false);
        c.versions = (moq_version_offer_t){ sizeof(moq_version_offer_t),
            MOQ_VERSION_POLICY_LIST, d18_d16, 2 };
        expect_offer("raw LIST {18,16}", &c, false, "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), false);
        c.versions = (moq_version_offer_t){ sizeof(moq_version_offer_t),
            MOQ_VERSION_POLICY_EXACT, d22, 1 };
        if (HOSTS_D22) {
            expect_offer("raw EXACT 22", &c, false, "moqt-22");
        } else {
            reset_seen();
            moq_endpoint_t *ep = NULL;
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&c, &ep),
                                  (int)MOQ_ERR_UNSUPPORTED);
            MOQ_TEST_CHECK(ep == NULL);
            printf("%-34s refused at connect: MOQ_ERR_UNSUPPORTED\n", "raw EXACT 22");
        }
    }

    /* == WebTransport =================================================== */
    {
        moq_endpoint_cfg_t c = client_cfg(url, sizeof(url), true);
        expect_offer("WT AUTO", &c, true,
                     HOSTS_D22 ? "moqt-22,moqt-18,moqt-16" : "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), true);
        c.preferred_version = MOQ_VERSION_DRAFT_22;
        expect_offer("WT AUTO preferring draft-22", &c, true,
                     HOSTS_D22 ? "moqt-22,moqt-18,moqt-16" : "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), true);
        c.versions = (moq_version_offer_t){ sizeof(moq_version_offer_t),
            MOQ_VERSION_POLICY_LIST, d18_d16, 2 };
        expect_offer("WT LIST {18,16}", &c, true, "moqt-18,moqt-16");

        c = client_cfg(url, sizeof(url), true);
        c.versions = (moq_version_offer_t){ sizeof(moq_version_offer_t),
            MOQ_VERSION_POLICY_EXACT, d22, 1 };
        if (HOSTS_D22) {
            expect_offer("WT EXACT 22", &c, true, "moqt-22");
        } else {
            moq_endpoint_t *ep = NULL;
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&c, &ep),
                                  (int)MOQ_ERR_UNSUPPORTED);
            MOQ_TEST_CHECK(ep == NULL);
            printf("%-34s refused at connect: MOQ_ERR_UNSUPPORTED\n", "WT EXACT 22");
        }
    }

    /* == The facades, used directly ===================================== */
    {
        static const char *const alpn22[1] = { "moqt-22" };
        moq_pq_threaded_cfg_t pc;
        moq_pq_threaded_cfg_init_sized(&pc, sizeof(pc));
        pc.alloc = moq_alloc_default();
        pc.perspective = MOQ_PERSPECTIVE_CLIENT;
        pc.host = "127.0.0.1";
        pc.port = g_port;
        pc.insecure_skip_verify = true;
        pc.on_lane_pump = pq_pump;
        pc.alpn_list = alpn22;
        pc.alpn_count = 1;
        moq_pq_threaded_t *t = NULL;
        moq_result_t rc = moq_pq_threaded_create(&pc, &t);
        printf("%-34s create: %d\n", "picoquic facade ALPN {moqt-22}", (int)rc);
        MOQ_TEST_CHECK_EQ_INT((int)rc, HOSTS_D22 ? (int)MOQ_OK : (int)MOQ_ERR_UNSUPPORTED);
        if (t) { moq_pq_threaded_stop(t); moq_pq_threaded_destroy(t); }

        moq_pico_wt_managed_cfg_t wc;
        moq_pico_wt_managed_cfg_init_sized(&wc, sizeof(wc));
        wc.alloc = moq_alloc_default();
        wc.perspective = MOQ_PERSPECTIVE_CLIENT;
        wc.host = "127.0.0.1";
        wc.port = g_port;
        wc.insecure_skip_verify = true;
        wc.on_pump = wt_pump;
        wc.wt_protocols = "moqt-22";
        moq_pico_wt_managed_t *m = NULL;
        rc = moq_pico_wt_managed_create(&wc, &m);
        printf("%-34s create: %d\n", "pico_wt facade WT {moqt-22}", (int)rc);
        MOQ_TEST_CHECK_EQ_INT((int)rc, HOSTS_D22 ? (int)MOQ_OK : (int)MOQ_ERR_UNSUPPORTED);
        if (m) { moq_pico_wt_managed_stop(m); moq_pico_wt_managed_destroy(m); }
    }

    picoquic_delete_network_thread(nt);
    picoquic_free(quic);
    if (failures) {
        fprintf(stderr, "test_endpoint_offer_wire: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: endpoint_offer_wire\n");
    return 0;
}
