/*
 * A loopback pair reaches its listener even when IPv4-only sockets hold much
 * of the ephemeral port range.
 *
 * MsQuic binds a listener to the dual-stack IPv6 wildcard, and on some kernels
 * (macOS) the port picked for such a socket is not checked against IPv4-only
 * sockets. An IPv4 socket on that port (a system daemon's udp4 *:P) receives
 * every IPv4 datagram the client sends to 127.0.0.1:P, so the handshake never
 * reaches the listener. This test holds IPv4 sockets on a large share of the
 * range and runs pairs back to back: every one must complete MoQ setup on both
 * sides, which holds only if the pair's listener port is one no IPv4 socket
 * holds.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wtquic_contract_pair.h"

#define HOLD_WANTED 4000
#define PAIRS 24
#define SETUP_WAIT_SECS 30

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* Hold IPv4 UDP sockets on ephemeral ports; returns how many are held. */
static int hold_ipv4_ports(int *fds, int want)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t need = (rlim_t)want + 256;
        if (rl.rlim_cur < need) {
            rl.rlim_cur = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= need
                              ? need : rl.rlim_max;
            (void)setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    int n = 0;
    while (n < want) {
        struct sockaddr_in sin;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);

        if (fd < 0)
            break;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) != 0) {
            close(fd);
            break;
        }
        fds[n++] = fd;
    }
    return n;
}

/* Wait until the client completed setup or its establishment failed. */
static bool wait_client_settled(wtqc_side_t *sd)
{
    struct timespec deadline;
    bool ok = true;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SETUP_WAIT_SECS;
    pthread_mutex_lock(&sd->mu);
    while (sd->obs.setup == 0 && !sd->obs.establishment_failed && ok)
        ok = pthread_cond_timedwait(&sd->cv, &sd->mu, &deadline) == 0;
    bool settled = sd->obs.setup > 0 || sd->obs.establishment_failed;
    pthread_mutex_unlock(&sd->mu);
    return settled;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    static int fds[HOLD_WANTED];
    int held = hold_ipv4_ports(fds, HOLD_WANTED);
    printf("port_shadow: %d IPv4 ports held\n", held);
    CHECK(held >= 1000);

    for (int i = 0; i < PAIRS && failures == 0; i++) {
        wtqc_pair_t p;
        wtqc_pair_cfg_t cfg;

        memset(&p, 0, sizeof(p));
        memset(&cfg, 0, sizeof(cfg));
        cfg.cert = argv[1];
        cfg.key = argv[2];
        cfg.client_offer = "moqt-16";
        cfg.server_serve = "moqt-16";
        if (wtqc_pair_setup(&p, &cfg) != 0) {
            CHECK(0 && "pair setup");
            wtqc_pair_teardown(&p);
            break;
        }
        CHECK(wait_client_settled(&p.client));
        pthread_mutex_lock(&p.client.mu);
        bool reached = p.client.obs.setup > 0;
        pthread_mutex_unlock(&p.client.mu);
        if (reached)
            CHECK(wtqc_wait_count(&p.server, &p.server.obs.setup, 1));
        wtqc_pair_teardown(&p);
        CHECK(!p.client.obs.establishment_failed);
        CHECK(p.client.obs.setup == 1);
        CHECK(p.server.obs.setup == 1);
        if (failures != 0)
            fprintf(stderr, "FAIL: pair %d did not reach its listener\n", i);
    }

    for (int i = 0; i < held; i++)
        close(fds[i]);
    if (failures == 0)
        printf("PASS: wtquic_port_shadow\n");
    return failures == 0 ? 0 : 1;
}
