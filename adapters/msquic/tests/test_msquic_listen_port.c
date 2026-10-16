/*
 * The port a managed MsQuic server asks its listener for.
 *
 * Port 0 is a port IPv4 and IPv6 clients both reach, so the facade picks it
 * from the IPv4 allocator and hands MsQuic a concrete port, never 0; a start
 * that fails is retried on a fresh listener and a freshly picked port, a
 * bounded number of times; an explicit port is handed over exactly as
 * configured and never retried. The reachability itself is proven over real
 * sockets by msquic_ephemeral_port_shadow; this pins the facade's side of it.
 *
 * Deterministic and transport-free: the whole facade stands on the fake API
 * table (tests/support/fake_msq_managed.h) with its listener entries wrapped
 * to record and refuse. No doorbell threads.
 *
 * Usage: test_msquic_listen_port
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <moq/msquic_managed.h>

#include "support/fake_msq_managed.h"
#include "support/msq_test_seams.h"

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

enum { MAX_STARTS = 16 };

static struct {
    QUIC_API_TABLE real;   /* the fake's own listener entries */
    int opens, closes, starts;
    uint16_t ports[MAX_STARTS];
    int fail_starts;       /* refuse the next N starts */
    bool ipv4_free[MAX_STARTS]; /* the port was free for IPv4 at start */
} g;

/* Whether an IPv4 socket could take `port` right now. */
static bool ipv4_free(uint16_t port)
{
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok;

    if (fd < 0)
        return false;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);
    ok = bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) == 0;
    close(fd);
    return ok;
}

static QUIC_STATUS QUIC_API w_listener_open(HQUIC reg,
                                            QUIC_LISTENER_CALLBACK_HANDLER cb,
                                            void *ctx, HQUIC *out)
{
    g.opens++;
    return g.real.ListenerOpen(reg, cb, ctx, out);
}

static void QUIC_API w_listener_close(HQUIC h)
{
    g.closes++;
    g.real.ListenerClose(h);
}

static QUIC_STATUS QUIC_API w_listener_start(HQUIC h, const QUIC_BUFFER *alpns,
                                             uint32_t alpn_count,
                                             const QUIC_ADDR *addr)
{
    uint16_t port = QuicAddrGetPort(addr);

    if (g.starts < MAX_STARTS) {
        g.ports[g.starts] = port;
        g.ipv4_free[g.starts] = port != 0 && ipv4_free(port);
    }
    g.starts++;
    if (g.fail_starts > 0) {
        g.fail_starts--;
        return QUIC_STATUS_ADDRESS_IN_USE;
    }
    return g.real.ListenerStart(h, alpns, alpn_count, addr);
}

static int idle_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                     uint64_t now, void *ctx)
{
    (void)m; (void)lane; (void)now; (void)ctx;
    return 0;
}

static fake_mgd_t fake;

static void rig(int fail_starts)
{
    fake_mgd_init(&fake);
    memset(&g, 0, sizeof(g));
    g.real = fake.api;
    fake.api.ListenerOpen = w_listener_open;
    fake.api.ListenerClose = w_listener_close;
    fake.api.ListenerStart = w_listener_start;
    g.fail_starts = fail_starts;
    moq_msq_test_api_override = fake_mgd_table(&fake);
    moq_msq_test_no_doorbell = true;
}

static moq_result_t create_server(uint16_t port, moq_msquic_managed_t **out)
{
    moq_msquic_managed_cfg_t cfg;

    moq_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.cert_path = "unused-by-the-fake-cert.pem";
    cfg.key_path = "unused-by-the-fake-key.pem";
    cfg.on_lane_pump = idle_pump;
    *out = NULL;
    return moq_msquic_managed_create(&cfg, out);
}

static void done(moq_msquic_managed_t *m)
{
    if (m != NULL) {
        (void)moq_msquic_managed_stop(m);
        moq_msquic_managed_destroy(m);
    }
    moq_msq_test_api_override = NULL;
    moq_msq_test_no_doorbell = false;
}

/* Port 0: one start, on a concrete port no IPv4 socket held. */
static void t_zero_picks_ipv4_free_port(void)
{
    int before = failures;
    moq_msquic_managed_t *m = NULL;

    rig(0);
    CHECK(create_server(0, &m) == MOQ_OK);
    CHECK(g.starts == 1);
    CHECK(g.ports[0] != 0);
    CHECK(g.ipv4_free[0]);
    CHECK(g.opens == 1 && g.closes == 0);
    done(m);
    if (failures == before)
        printf("PASS: zero_picks_ipv4_free_port\n");
}

/* A failed start is retried on a fresh listener and a picked port. */
static void t_zero_retries_lost_bind(void)
{
    int before = failures;
    moq_msquic_managed_t *m = NULL;

    rig(3);
    CHECK(create_server(0, &m) == MOQ_OK);
    CHECK(g.starts == 4);
    for (int i = 0; i < 4; i++) {
        CHECK(g.ports[i] != 0);
        CHECK(g.ipv4_free[i]);
    }
    /* each refused listener was closed before the next was opened */
    CHECK(g.opens == 4 && g.closes == 3);
    done(m);
    if (failures == before)
        printf("PASS: zero_retries_lost_bind\n");
}

/* The retry is bounded; exhausting it fails create and leaks no listener. */
static void t_zero_retry_bounded(void)
{
    int before = failures;
    moq_msquic_managed_t *m = NULL;

    rig(MAX_STARTS);
    CHECK(create_server(0, &m) == MOQ_ERR_INTERNAL);
    CHECK(m == NULL);
    CHECK(g.starts == 8);
    for (int i = 0; i < 8; i++)
        CHECK(g.ports[i] != 0);
    CHECK(g.opens == 8 && g.closes == 8);
    CHECK(!fake.listener_open);
    done(NULL);
    if (failures == before)
        printf("PASS: zero_retry_bounded\n");
}

/* An explicit port is handed over as configured, and never retried. */
static void t_explicit_port_as_given(void)
{
    int before = failures;
    moq_msquic_managed_t *m = NULL;

    rig(0);
    CHECK(create_server(4711, &m) == MOQ_OK);
    CHECK(g.starts == 1);
    CHECK(g.ports[0] == 4711);
    done(m);

    rig(1);
    CHECK(create_server(4711, &m) == MOQ_ERR_INTERNAL);
    CHECK(m == NULL);
    CHECK(g.starts == 1);
    CHECK(g.ports[0] == 4711);
    CHECK(g.opens == 1 && g.closes == 1);
    done(NULL);
    if (failures == before)
        printf("PASS: explicit_port_as_given\n");
}

int main(void)
{
    t_zero_picks_ipv4_free_port();
    t_zero_retries_lost_bind();
    t_zero_retry_bounded();
    t_explicit_port_as_given();
    if (failures == 0)
        printf("PASS: msquic_listen_port\n");
    else
        fprintf(stderr, "FAIL: msquic_listen_port (%d)\n", failures);
    return failures;
}
