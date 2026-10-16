/*
 * A managed wtquic-MsQuic server created with port 0 listens on a port an
 * IPv4 client reaches it on.
 *
 * MsQuic binds every listener to the dual-stack IPv6 wildcard, and some
 * kernels (macOS) do not check IPv4-only sockets when they pick a port for a
 * dual-stack socket. Port 0 must still mean a port that IPv4 and IPv6 clients
 * both reach this listener on, so the facade chooses it itself rather than
 * leaving the choice to MsQuic's bind.
 *
 * Run it under tests/support/port_shadow.c, which holds a large share of the
 * ephemeral range with IPv4-only sockets in another process, as a system
 * daemon would. Each round creates a fresh server with port 0, exactly as an
 * application would (no test-side port selection), and connects a raw
 * wtquic client to 127.0.0.1; every round must establish its WebTransport
 * session. A port an IPv4-only socket holds swallows the client's Initial
 * packets and the round ends in the client's idle timeout instead.
 *
 * Usage: test_wtquic_msquic_managed_ephemeral_port <cert.pem> <key.pem>
 */
#include <moq/wtquic_msquic_managed.h>

#include <wtquic/wtquic.h>
#include <wtquic/wtquic_msquic.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* Enough rounds that, with a fifth or more of the ephemeral range shadowed,
 * a port chosen without regard to IPv4 sockets is all but certain to hit a
 * shadowed port at least once. */
enum { ROUNDS = 24 };

/* The client's idle timeout, and the hang guard above it. */
#define CLIENT_IDLE_MS 3000u
#define WAIT_SECS 10

static int idle_pump(moq_wtquic_msquic_managed_t *m,
                     moq_wtquic_msquic_managed_lane_t *lane, uint64_t now_us,
                     void *user)
{
    (void)m;
    (void)lane;
    (void)now_us;
    (void)user;
    return 0;
}

struct raw_cli {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int established;
};

static void raw_established(wtq_session_t *s, wtq_str_t sub, void *user)
{
    struct raw_cli *r = user;

    (void)s;
    (void)sub;
    pthread_mutex_lock(&r->mu);
    r->established = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

static bool raw_wait_established(struct raw_cli *r)
{
    struct timespec dl;

    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += WAIT_SECS;
    pthread_mutex_lock(&r->mu);
    bool ok = true;
    while (r->established == 0 && ok)
        ok = pthread_cond_timedwait(&r->cv, &r->mu, &dl) == 0;
    bool set = r->established != 0;
    pthread_mutex_unlock(&r->mu);
    return set;
}

static void round_reaches(const char *cert, const char *key, int round)
{
    int before = failures;
    const char *offers[] = { "moqt-18" };
    moq_wtquic_msquic_managed_cfg_t cfg;

    moq_wtquic_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.cert_path = cert;
    cfg.key_path = key;
    cfg.wt_path = "/moq";
    cfg.wt_protocols = offers;
    cfg.wt_protocol_count = 1;
    cfg.max_connections = 1;
    cfg.on_lane_pump = idle_pump;

    moq_wtquic_msquic_managed_t *srv = NULL;
    CHECK(moq_wtquic_msquic_managed_create(&cfg, &srv) == MOQ_OK);
    if (srv == NULL)
        return;
    uint16_t port = moq_wtquic_msquic_managed_port(srv);
    CHECK(port != 0);

    struct raw_cli rc;
    memset(&rc, 0, sizeof(rc));
    pthread_mutex_init(&rc.mu, NULL);
    pthread_cond_init(&rc.cv, NULL);

    wtq_msquic_env_cfg_t ecfg = WTQ_MSQUIC_ENV_CFG_INIT;
    ecfg.tuning.idle_timeout_ms = CLIENT_IDLE_MS;
    wtq_msquic_env_t *cenv = NULL;
    wtq_session_t *cs = NULL;

    CHECK(wtq_msquic_env_open(&ecfg, &cenv) == WTQ_OK);
    if (cenv != NULL && port != 0) {
        const char *const cprotos[] = { offers[0] };
        wtq_session_events_t cev;

        wtq_session_events_init(&cev);
        cev.on_established = raw_established;
        wtq_connect_config_t connect = WTQ_CONNECT_CONFIG_INIT;
        connect.authority = "localhost";
        connect.path = "/moq";
        connect.subprotocols = cprotos;
        connect.subprotocol_count = 1;
        wtq_msquic_client_cfg_t cli = WTQ_MSQUIC_CLIENT_CFG_INIT;
        cli.server_name = "127.0.0.1";
        cli.port = port;
        cli.insecure_skip_verify = true;
        cli.connect = &connect;
        cli.events = &cev;
        cli.user = &rc;
        CHECK(wtq_msquic_client_connect(cenv, &cli, &cs) == WTQ_OK);
        CHECK(raw_wait_established(&rc));
    }

    CHECK(moq_wtquic_msquic_managed_stop(srv) == MOQ_OK);
    moq_wtquic_msquic_managed_destroy(srv);
    if (cenv != NULL)
        wtq_msquic_env_close(cenv);
    if (cs != NULL)
        wtq_session_release(cs);
    pthread_cond_destroy(&rc.cv);
    pthread_mutex_destroy(&rc.mu);
    if (failures != before)
        fprintf(stderr, "  round %d: port %u not reached over IPv4\n",
                round, (unsigned)port);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }

    /* stop at the first unreachable port: each one costs an idle timeout */
    for (int r = 0; r < ROUNDS && failures == 0; r++)
        round_reaches(argv[1], argv[2], r);

    if (failures == 0)
        printf("PASS: wtquic_msquic_managed_ephemeral_port (%d rounds)\n",
               ROUNDS);
    else
        fprintf(stderr, "FAIL: wtquic_msquic_managed_ephemeral_port (%d)\n",
                failures);
    return failures;
}
