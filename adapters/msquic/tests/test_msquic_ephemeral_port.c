/*
 * A managed MsQuic server created with port 0 listens on a port an IPv4
 * client reaches it on.
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
 * application would (no test-side port selection), and dials it from an IPv4
 * loopback client; every round must reach SETUP. A port an IPv4-only socket
 * holds swallows the client's Initial packets and the round ends in the
 * client's idle timeout instead.
 *
 * Confinement: every moq_session_* call lives inside on_lane_pump; the main
 * thread only waits on terminal predicates. Timed waits are fail-closed hang
 * guards.
 *
 * Usage: test_msquic_ephemeral_port <cert.pem> <key.pem>
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <moq/msquic_managed.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* Enough rounds that, with a fifth or more of the ephemeral range shadowed,
 * a port chosen without regard to IPv4 sockets is all but certain to hit a
 * shadowed port at least once. */
enum { ROUNDS = 24 };

struct client_obs {
    pthread_mutex_t mu;
    int setup;
    bool close_sent;
};

static int client_pump(moq_msquic_managed_t *m,
                       moq_msquic_managed_lane_t *lane, uint64_t now,
                       void *ctx)
{
    struct client_obs *o = ctx;
    moq_session_t *s = moq_msquic_managed_session(m);
    moq_event_t ev;

    (void)lane;
    pthread_mutex_lock(&o->mu);
    while (s != NULL && moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE)
            o->setup++;
        moq_event_cleanup(&ev);
    }
    if (s != NULL && o->setup > 0 && !o->close_sent) {
        o->close_sent = true;
        (void)moq_session_close(s, 0, NULL, now);
    }
    pthread_mutex_unlock(&o->mu);
    return 0;
}

static int server_pump(moq_msquic_managed_t *m,
                       moq_msquic_managed_lane_t *lane, uint64_t now,
                       void *ctx)
{
    moq_event_t ev;

    (void)m;
    (void)now;
    (void)ctx;
    for (moq_msquic_managed_conn_t *c = moq_msquic_lane_next_conn(lane, NULL);
         c != NULL; c = moq_msquic_lane_next_conn(lane, c)) {
        moq_session_t *s = moq_msquic_managed_conn_session(c);
        bool closed = false;

        while (moq_session_poll_events(s, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SESSION_CLOSED)
                closed = true;
            moq_event_cleanup(&ev);
        }
        if (closed)
            (void)moq_msquic_managed_conn_ack_terminal(c);
    }
    return 0;
}

static bool wait_terminal(moq_msquic_managed_t *m, int max_ms)
{
    for (int i = 0; i < max_ms / 10; i++) {
        if (moq_msquic_managed_is_closed(m) ||
            moq_msquic_managed_is_fatal(m))
            return true;
        (void)moq_msquic_managed_wait(m, 10 * 1000);
    }
    return moq_msquic_managed_is_closed(m) ||
           moq_msquic_managed_is_fatal(m);
}

/* One round: a port-0 server, an IPv4 client, SETUP. True when it reached
 * SETUP and closed in order. */
static bool round_reaches(const char *cert, const char *key, int round)
{
    int before = failures;
    struct client_obs obs;
    moq_msquic_managed_t *sv = NULL, *cl = NULL;

    memset(&obs, 0, sizeof(obs));
    pthread_mutex_init(&obs.mu, NULL);

    moq_msquic_managed_cfg_t scfg;
    moq_msquic_managed_cfg_init_sized(&scfg, sizeof(scfg));
    scfg.alloc = moq_alloc_default();
    scfg.perspective = MOQ_PERSPECTIVE_SERVER;
    scfg.host = "127.0.0.1";
    scfg.port = 0;
    scfg.cert_path = cert;
    scfg.key_path = key;
    scfg.on_lane_pump = server_pump;
    CHECK(moq_msquic_managed_create(&scfg, &sv) == MOQ_OK);
    if (sv == NULL) {
        pthread_mutex_destroy(&obs.mu);
        return false;
    }
    uint16_t port = moq_msquic_managed_port(sv);
    CHECK(port != 0);

    moq_msquic_managed_cfg_t ccfg;
    moq_msquic_managed_cfg_init_sized(&ccfg, sizeof(ccfg));
    ccfg.alloc = moq_alloc_default();
    ccfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    ccfg.host = "127.0.0.1";
    ccfg.port = port;
    ccfg.insecure_skip_verify = true;
    /* an unreachable listener ends the round in this bound, not MsQuic's
     * default handshake timeout */
    ccfg.idle_timeout_ms = 3000;
    ccfg.on_lane_pump = client_pump;
    ccfg.on_lane_pump_user = &obs;
    CHECK(moq_msquic_managed_create(&ccfg, &cl) == MOQ_OK);
    if (cl != NULL) {
        CHECK(wait_terminal(cl, 20000));
        CHECK(moq_msquic_managed_is_closed(cl));
        CHECK(!moq_msquic_managed_is_fatal(cl));
        CHECK(moq_msquic_managed_stop(cl) == MOQ_OK);
        moq_msquic_managed_destroy(cl);
    }
    CHECK(moq_msquic_managed_stop(sv) == MOQ_OK);
    moq_msquic_managed_destroy(sv);
    CHECK(obs.setup == 1);
    pthread_mutex_destroy(&obs.mu);
    if (failures != before)
        fprintf(stderr, "  round %d: port %u not reached over IPv4\n",
                round, (unsigned)port);
    return failures == before;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }

    /* one library reference for the whole process, so MsQuic's datapath is
     * not torn down and re-initialised between rounds */
    static const QUIC_API_TABLE *lib_pin;

    if (QUIC_FAILED(MsQuicOpen2(&lib_pin))) {
        fprintf(stderr, "MsQuicOpen2 failed\n");
        return 2;
    }

    /* stop at the first unreachable port: each one costs an idle timeout */
    for (int r = 0; r < ROUNDS && failures == 0; r++)
        (void)round_reaches(argv[1], argv[2], r);

    if (failures == 0)
        printf("PASS: msquic_ephemeral_port (%d rounds)\n", ROUNDS);
    else
        fprintf(stderr, "FAIL: msquic_ephemeral_port (%d)\n", failures);
    return failures;
}
