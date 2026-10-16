/*
 * Managed pico WT edge cases: config validation, NULL safety, connect
 * failure, certificate-verification failure, stop/wake/wait lifecycle,
 * stop-from-on_pump rejection, and the CONNECT :authority (the URI
 * authority, independent of the TLS SNI).
 *
 * Args: --cert <file> --key <file>
 */

#include <moq/moq.h>
#include <moq/pico_wt_managed.h>
#include "pico_wt_test_seam.h"
#include <moq/picoquic_verify.h>   /* the helper under test */

#include <picoquic.h>
#include <picotls.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int noop_pump(moq_pico_wt_managed_t *m, uint64_t now, void *ctx)
{ (void)m; (void)now; (void)ctx; return 0; }

/* configure_quic hook: short handshake timeout so a failed connect
 * surfaces in ~1s instead of the multi-second default. */
static int short_handshake(picoquic_quic_t *quic, void *ctx)
{
    (void)ctx;
    picoquic_set_default_handshake_timeout(quic, 1000000);  /* 1s */
    return 0;
}

static void base_client_cfg(moq_pico_wt_managed_cfg_t *cfg, int port)
{
    moq_pico_wt_managed_cfg_init(cfg);
    cfg->alloc = moq_alloc_default();
    cfg->perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg->host = "127.0.0.1";
    cfg->port = port;
    cfg->on_pump = noop_pump;
}

/* -- config validation (no network) -------------------------------- */

static void test_cfg_validation(void)
{
    moq_pico_wt_managed_t *m = (moq_pico_wt_managed_t *)1;
    moq_pico_wt_managed_cfg_t cfg;

    CHECK(moq_pico_wt_managed_create(NULL, &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);

    /* missing on_pump */
    moq_pico_wt_managed_cfg_init(&cfg);
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg.host = "127.0.0.1"; cfg.port = 4433;
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);

    /* client without host */
    base_client_cfg(&cfg, 4433); cfg.host = NULL;
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);

    /* server without cert/key */
    moq_pico_wt_managed_cfg_init(&cfg);
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.port = 4433; cfg.on_pump = noop_pump;
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);

    /* invalid perspective */
    base_client_cfg(&cfg, 4433); cfg.perspective = (moq_perspective_t)0;
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);

    /* bad ports */
    base_client_cfg(&cfg, 0);
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);
    base_client_cfg(&cfg, 70000);
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);

    /* missing alloc */
    base_client_cfg(&cfg, 4433); cfg.alloc = NULL;
    CHECK(moq_pico_wt_managed_create(&cfg, &m) == MOQ_ERR_INVAL);
}

/* -- NULL safety --------------------------------------------------- */

static void test_null_safety(void)
{
    CHECK(moq_pico_wt_managed_stop(NULL) == MOQ_ERR_INVAL);
    moq_pico_wt_managed_destroy(NULL);  /* void; must not crash */
    CHECK(moq_pico_wt_managed_session(NULL) == NULL);
    CHECK(!moq_pico_wt_managed_is_fatal(NULL));
    CHECK(!moq_pico_wt_managed_is_closed(NULL));
    CHECK(moq_pico_wt_managed_fatal_code(NULL) == 0);
    CHECK(moq_pico_wt_managed_close_code(NULL) == 0);
    CHECK(moq_pico_wt_managed_local_port(NULL) == 0);
    CHECK(moq_pico_wt_managed_wake(NULL) == MOQ_ERR_INVAL);
    CHECK(moq_pico_wt_managed_wait(NULL, 0) == MOQ_ERR_INVAL);
}

/* -- connect failure (no server) ----------------------------------- */

static void test_connect_failure(void)
{
    /* Nothing listens on port 1: the handshake fails and the managed
     * client must surface a terminal (fatal) state, not hang. */
    moq_pico_wt_managed_cfg_t cfg;
    base_client_cfg(&cfg, 1);
    cfg.insecure_skip_verify = true;
    cfg.configure_quic = short_handshake;
    moq_pico_wt_managed_t *cli = NULL;
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
    if (!cli) return;

    int fatal = 0;
    for (int i = 0; i < 40 && !fatal; i++) {   /* up to ~4s */
        moq_pico_wt_managed_wait(cli, 100000);
        fatal = moq_pico_wt_managed_is_fatal(cli);
    }
    CHECK(fatal);
    CHECK(moq_pico_wt_managed_fatal_code(cli) == 0);   /* transport-level */
    CHECK(moq_pico_wt_managed_session(cli) == NULL);  /* never came up */

    moq_pico_wt_managed_stop(cli);
    moq_pico_wt_managed_destroy(cli);
}


/* -- stop/wake/wait lifecycle -------------------------------------- */

static void test_lifecycle(void)
{
    /* Client to a dead port with the default (long) handshake timeout:
     * it stays connecting (not fatal) for the brief checks below. */
    moq_pico_wt_managed_cfg_t cfg;
    base_client_cfg(&cfg, 1);
    cfg.insecure_skip_verify = true;
    moq_pico_wt_managed_t *cli = NULL;
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
    if (!cli) return;

    CHECK(moq_pico_wt_managed_wake(cli) == MOQ_OK);   /* thread running */
    moq_result_t w = moq_pico_wt_managed_wait(cli, 0);  /* non-blocking */
    CHECK(w == MOQ_OK || w == MOQ_DONE);

    CHECK(moq_pico_wt_managed_stop(cli) == MOQ_OK);
    CHECK(moq_pico_wt_managed_stop(cli) == MOQ_OK);    /* idempotent */
    CHECK(moq_pico_wt_managed_wake(cli) == MOQ_ERR_CLOSED);
    CHECK(moq_pico_wt_managed_wait(cli, 0) == MOQ_ERR_CLOSED);

    moq_pico_wt_managed_destroy(cli);
}

/* -- cert verification failure ------------------------------------- */

static moq_pico_wt_managed_t *make_test_server(const char *cert,
    const char *key, int port)
{
    moq_pico_wt_managed_cfg_t cfg;
    moq_pico_wt_managed_cfg_init(&cfg);
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.cert_path = cert; cfg.key_path = key;
    cfg.port = port;
    cfg.on_pump = noop_pump;
    moq_pico_wt_managed_t *srv = NULL;
    if (moq_pico_wt_managed_create(&cfg, &srv) != MOQ_OK) return NULL;
    for (int i = 0; i < 3; i++) moq_pico_wt_managed_wait(srv, 100000);
    return srv;
}

/* A client's CONNECT waits for the server's SETTINGS
 * (draft-ietf-webtrans-http3-15 Section 4.6), so create succeeds and the
 * CONNECT is submitted once SETTINGS arrive. long_path = 1: a 2048-byte path
 * cannot be prepared, so the CONNECT is still pending after SETTINGS, h3zero
 * fails the connection, and the client ends terminally, at the transport
 * level, with no session. long_path = 0 is the control: the same server, the
 * CONNECT is queued once SETTINGS arrive and the session comes up. The
 * private seam tells these apart from a handshake failure or a refused route,
 * neither of which leaves a CONNECT pending after SETTINGS. It cannot by
 * itself tell a preparation failure from a failure to queue the prepared
 * CONNECT; only the path differs between the two rows. Stop and destroy then
 * release the h3zero context the connection still carries. */
static void test_connect_prepare(const char *cert, const char *key, int long_path)
{
    static char long_path_buf[2049];
    long_path_buf[0] = '/';
    memset(long_path_buf + 1, 'p', sizeof(long_path_buf) - 2);
    long_path_buf[sizeof(long_path_buf) - 1] = '\0';
    int proven = 0;
    static const int ports[] = { 25443, 25587, 25731, 25877 };
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven; i++) {
        moq_pico_wt_managed_t *srv = make_test_server(cert, key, ports[i]);
        if (!srv) continue;                          /* bind — retry */
        moq_pico_wt_managed_cfg_t cfg;
        base_client_cfg(&cfg, moq_pico_wt_managed_local_port(srv));
        cfg.insecure_skip_verify = true;
        cfg.path = long_path ? long_path_buf : "/moq";
        moq_pico_wt_managed_t *cli = NULL;
        CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
        if (cli) {
            int done = 0, has_session = 0;
            for (int k = 0; k < 40 && !done; k++) {
                moq_pico_wt_managed_wait(cli, 100000);
                has_session = moq_pico_wt_managed_session(cli) != NULL;
                done = moq_pico_wt_managed_is_fatal(cli) || has_session;
            }
            int fatal = moq_pico_wt_managed_is_fatal(cli);
            uint64_t fcode = moq_pico_wt_managed_fatal_code(cli);
            moq_pico_wt_managed_stop(cli);   /* joins the counting thread */
            unsigned queued = moq_pico_wt_managed_test_connect_enqueued(cli);
            unsigned pending = moq_pico_wt_managed_test_connect_left_pending(cli);
            if (long_path) {
                CHECK(pending == 1 && queued == 0);
                CHECK(fatal && fcode == 0 && !has_session);
            } else {
                CHECK(queued == 1 && pending == 0);
                CHECK(has_session && !fatal);
            }
            proven = 1;
            moq_pico_wt_managed_destroy(cli);
        }
        moq_pico_wt_managed_stop(srv);
        moq_pico_wt_managed_destroy(srv);
    }
    CHECK(proven);
}

/* A verify-certificate callback that rejects every server cert. Used to
 * exercise the verification-failure path (picoquic's built-in default
 * has no CA store and accepts, so we install an explicit verifier — the
 * configure_quic hook is exactly the documented place for this). */
static atomic_int reject_called;

static int reject_cert_cb(struct st_ptls_verify_certificate_t *self,
    ptls_t *tls, const char *server_name,
    int (**verify_sign)(void *, uint16_t, ptls_iovec_t, ptls_iovec_t),
    void **verify_data, ptls_iovec_t *certs, size_t num_certs)
{
    (void)self; (void)tls; (void)server_name; (void)verify_sign;
    (void)verify_data; (void)certs; (void)num_certs;
    atomic_store(&reject_called, 1);   /* proves verification ran */
    return PTLS_ALERT_BAD_CERTIFICATE;
}
/* Advertise the common TLS 1.3 signature schemes so negotiation
 * reaches certificate verification (the test cert is RSA): rsa_pss_rsae
 * sha256/384/512, ecdsa_secp256r1_sha256, ed25519. */
static uint16_t reject_algos[] = {
    0x0804, 0x0805, 0x0806, 0x0403, 0x0807, UINT16_MAX
};
static ptls_verify_certificate_t reject_verifier =
    { reject_cert_cb, reject_algos };

static int install_reject_verifier(picoquic_quic_t *quic, void *ctx)
{
    (void)ctx;
    picoquic_set_default_handshake_timeout(quic, 1000000);
    picoquic_set_verify_certificate_callback(quic, &reject_verifier, NULL);
    return 0;
}

static void test_cert_failure(const char *cert, const char *key)
{
    /* Retry candidate ports so a busy/unbound server is not mistaken
     * for a cert rejection. "Proven" requires the verify callback to
     * have actually run; only then are fatal + no-session asserted. */
    const int ports[] = { 18483, 19483, 20483 };
    int proven = 0;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven;
         i++) {
        atomic_store(&reject_called, 0);
        moq_pico_wt_managed_t *srv =
            make_test_server(cert, key, ports[i]);
        if (!srv) continue;                          /* bind — retry */

        moq_pico_wt_managed_cfg_t cfg;
        base_client_cfg(&cfg, moq_pico_wt_managed_local_port(srv));
        cfg.insecure_skip_verify = false;            /* verify the cert */
        cfg.configure_quic = install_reject_verifier;
        moq_pico_wt_managed_t *cli = NULL;
        if (moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK && cli) {
            int fatal = 0;
            for (int k = 0; k < 40 && !fatal; k++) {
                moq_pico_wt_managed_wait(cli, 100000);
                fatal = moq_pico_wt_managed_is_fatal(cli);
            }
            int called = atomic_load(&reject_called);
            int has_session = moq_pico_wt_managed_session(cli) != NULL;
            uint64_t fcode = moq_pico_wt_managed_fatal_code(cli);
            moq_pico_wt_managed_stop(cli);
            moq_pico_wt_managed_destroy(cli);
            if (called) {
                CHECK(fatal);            /* rejection → terminal state */
                CHECK(fcode == 0);       /* transport/handshake-level */
                CHECK(!has_session);     /* no session established */
                proven = 1;
            }
            /* else: handshake never reached verification (server may
             * not have bound on this port) — try the next candidate. */
        }
        moq_pico_wt_managed_stop(srv);
        moq_pico_wt_managed_destroy(srv);
    }
    CHECK(proven);   /* the verify callback ran and rejected somewhere */
}

/* -- production helper: moq_picoquic_set_cert_verifier ------------- */

/* configure_quic that installs the libmoq production verifier (system
 * trust store). Also shortens the handshake timeout so a rejected
 * connection surfaces quickly. */
static int install_helper_verifier(picoquic_quic_t *quic, void *ctx)
{
    (void)ctx;
    picoquic_set_default_handshake_timeout(quic, 1000000);  /* 1s */
    return moq_picoquic_set_cert_verifier(quic, NULL);  /* NULL = system */
}

/* Poll a managed client until it goes fatal or exposes a session. */
static int wait_client(moq_pico_wt_managed_t *cli, int *out_has_session)
{
    int fatal = 0;
    for (int k = 0; k < 50 && !fatal; k++) {
        moq_pico_wt_managed_wait(cli, 100000);
        fatal = moq_pico_wt_managed_is_fatal(cli);
        if (moq_pico_wt_managed_session(cli) != NULL) break;
    }
    *out_has_session = moq_pico_wt_managed_session(cli) != NULL;
    return moq_pico_wt_managed_is_fatal(cli);
}

static void test_helper_cert_failure(const char *cert, const char *key)
{
    /* Prove moq_picoquic_set_cert_verifier installs a REAL verifier:
     * against the self-signed test server it rejects (client fatal, no
     * session). Self-contained per port: a rejected client fails at TLS
     * BEFORE WT CONNECT, so it never occupies the one-connection server —
     * an insecure control client then connects to the SAME server and
     * comes up, proving the server was serving and isolating the helper's
     * verifier as the cause of the first client's failure. */
    const int ports[] = { 18493, 19493, 20493 };
    int proven = 0;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven;
         i++) {
        moq_pico_wt_managed_t *srv = make_test_server(cert, key, ports[i]);
        if (!srv) continue;
        int port = moq_pico_wt_managed_local_port(srv);

        /* Client 1: helper (system trust) → must reject the self-signed. */
        moq_pico_wt_managed_cfg_t c1;
        base_client_cfg(&c1, port);
        c1.insecure_skip_verify = false;
        c1.configure_quic = install_helper_verifier;
        moq_pico_wt_managed_t *cli1 = NULL;
        int h_fatal = 0, h_session = 1;
        if (moq_pico_wt_managed_create(&c1, &cli1) == MOQ_OK && cli1)
            h_fatal = wait_client(cli1, &h_session);
        if (cli1) {
            moq_pico_wt_managed_stop(cli1);
            moq_pico_wt_managed_destroy(cli1);
        }

        /* Client 2: insecure control → must come up on the same server. */
        moq_pico_wt_managed_cfg_t c2;
        base_client_cfg(&c2, port);
        c2.insecure_skip_verify = true;
        moq_pico_wt_managed_t *cli2 = NULL;
        int ctrl_session = 0, ctrl_dummy = 0;
        if (moq_pico_wt_managed_create(&c2, &cli2) == MOQ_OK && cli2) {
            (void)wait_client(cli2, &ctrl_dummy);
            ctrl_session = moq_pico_wt_managed_session(cli2) != NULL;
            moq_pico_wt_managed_stop(cli2);
            moq_pico_wt_managed_destroy(cli2);
        }

        moq_pico_wt_managed_stop(srv);
        moq_pico_wt_managed_destroy(srv);

        /* Only assert once the control proves the server served on this
         * port (rules out a busy/unbound-port false positive). */
        if (ctrl_session) {
            CHECK(h_fatal);       /* helper rejected → terminal */
            CHECK(!h_session);    /* no session established */
            proven = 1;
        }
    }
    CHECK(proven);  /* helper's verifier rejected against a serving peer */
}

static void test_default_cert_failure(const char *cert, const char *key)
{
    /* The DEFAULT client config (insecure_skip_verify=false, no configure_quic)
     * must fail closed: against the self-signed test server it rejects (client
     * fatal, no session), proving the managed client installs a real verifier by
     * default. An insecure control client then connects to the SAME server,
     * isolating the default verifier as the cause of the first failure.
     *
     * Guards against a default that installs no verifier: client 1 would then
     * ACCEPT the self-signed cert and come up with a session (h_fatal=0,
     * h_session=1). */
    const int ports[] = { 18495, 19495, 20495 };
    int proven = 0;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven;
         i++) {
        moq_pico_wt_managed_t *srv = make_test_server(cert, key, ports[i]);
        if (!srv) continue;
        int port = moq_pico_wt_managed_local_port(srv);

        /* Client 1: default verifier (no configure_quic) → must reject. An
         * explicit sni is required to verify against a bare-IP host; the
         * self-signed cert still fails chain validation regardless of name. */
        moq_pico_wt_managed_cfg_t c1;
        base_client_cfg(&c1, port);
        c1.insecure_skip_verify = false;   /* default; verifier installed */
        c1.sni = "localhost";              /* expected name (IP host) */
        moq_pico_wt_managed_t *cli1 = NULL;
        int d_fatal = 0, d_session = 1;
        if (moq_pico_wt_managed_create(&c1, &cli1) == MOQ_OK && cli1)
            d_fatal = wait_client(cli1, &d_session);
        if (cli1) {
            moq_pico_wt_managed_stop(cli1);
            moq_pico_wt_managed_destroy(cli1);
        }

        /* Client 2: insecure control → must come up on the same server. */
        moq_pico_wt_managed_cfg_t c2;
        base_client_cfg(&c2, port);
        c2.insecure_skip_verify = true;
        moq_pico_wt_managed_t *cli2 = NULL;
        int ctrl_session = 0, ctrl_dummy = 0;
        if (moq_pico_wt_managed_create(&c2, &cli2) == MOQ_OK && cli2) {
            (void)wait_client(cli2, &ctrl_dummy);
            ctrl_session = moq_pico_wt_managed_session(cli2) != NULL;
            moq_pico_wt_managed_stop(cli2);
            moq_pico_wt_managed_destroy(cli2);
        }

        moq_pico_wt_managed_stop(srv);
        moq_pico_wt_managed_destroy(srv);

        if (ctrl_session) {
            CHECK(d_fatal);       /* default verifier rejected → terminal */
            CHECK(!d_session);    /* no session established */
            proven = 1;
        }
    }
    CHECK(proven);  /* default fails closed against a serving peer */
}

static void test_ip_host_sni_policy(void)
{
    /* IP-literal host SNI policy. Drives only _create (+ stop/destroy) to a
     * dead port — no server or successful handshake is needed, though _create
     * does start the network thread.
     *  - insecure + IP host + no sni: must still CREATE (regression: a bare IP
     *    is not a valid SNI, so we must not feed it to picoquic). Historically
     *    SNI defaulted to "localhost"; that path must keep working.
     *  - verified + IP host + no sni: must FAIL CLOSED (MOQ_ERR_INVAL) — we
     *    cannot do hostname verification against a bare IP without an expected
     *    name, and must not send an invalid IP SNI or verify a placeholder.
     *  - verified + IP host + explicit sni: must CREATE (name supplied). */
    moq_pico_wt_managed_cfg_t cfg;
    moq_pico_wt_managed_t *cli = NULL;

    base_client_cfg(&cfg, 1);             /* host = 127.0.0.1, dead port 1 */
    cfg.insecure_skip_verify = true;      /* no sni */
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
    CHECK(cli != NULL);
    if (cli) { moq_pico_wt_managed_stop(cli); moq_pico_wt_managed_destroy(cli); }

    cli = NULL;
    base_client_cfg(&cfg, 1);
    cfg.insecure_skip_verify = false;     /* verified, no sni, IP host */
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_ERR_INVAL);
    CHECK(cli == NULL);

    cli = NULL;
    base_client_cfg(&cfg, 1);
    cfg.insecure_skip_verify = false;
    cfg.sni = "example.test";             /* verified, explicit name, IP host */
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
    CHECK(cli != NULL);
    if (cli) { moq_pico_wt_managed_stop(cli); moq_pico_wt_managed_destroy(cli); }
}

/* -- CONNECT identity --------------------------------------------- */

/* One CONNECT row: what the client is configured with and what the server
 * must observe. target_authority NULL leaves the target to be derived from
 * host/port and cfg_path. */
typedef struct {
    const char *host, *sni;
    const char *target_authority, *target_path_query;
    const char *cfg_path;        /* NULL: not configured */
    const char *server_path;     /* the server's route */
    const char *want_authority_fmt, *want_path, *want_sni;
} connect_row_t;

static void set_target(moq_connection_target_t *t, const char *authority,
                       const char *path_query)
{
    memset(t, 0, sizeof(*t));
    t->struct_size = (uint32_t)sizeof(*t);
    t->target_present = MOQ_BINDING_TARGET_COMPLETE;
    t->authority.data = (const uint8_t *)authority;
    t->authority.len = strlen(authority);
    t->path_query.data = (const uint8_t *)path_query;
    t->path_query.len = strlen(path_query);
}

static void client_cfg(moq_pico_wt_managed_cfg_t *cfg, const char *host,
                       int port)
{
    moq_pico_wt_managed_cfg_init_sized(cfg, sizeof(*cfg));
    cfg->alloc = moq_alloc_default();
    cfg->perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg->host = host;
    cfg->port = port;
    cfg->on_pump = noop_pump;
    cfg->insecure_skip_verify = true;
}

/* One client against a fresh server routing `server_path`: the server
 * records the CONNECT's :authority and :path and the connection's TLS SNI.
 * Returns 1 once a session came up and the observation was checked, 0 when
 * the server never served (busy port), so the caller tries the next one. */
static int connect_row(const char *cert, const char *key, int port,
                       const connect_row_t *row)
{
    moq_pico_wt_managed_cfg_t scfg;
    moq_pico_wt_managed_cfg_init(&scfg);
    scfg.alloc = moq_alloc_default();
    scfg.perspective = MOQ_PERSPECTIVE_SERVER;
    scfg.cert_path = cert; scfg.key_path = key;
    scfg.port = port;
    scfg.path = row->server_path;
    scfg.on_pump = noop_pump;
    moq_pico_wt_managed_t *srv = NULL;
    if (moq_pico_wt_managed_create(&scfg, &srv) != MOQ_OK) return 0;
    for (int i = 0; i < 3; i++) moq_pico_wt_managed_wait(srv, 100000);
    int bound = moq_pico_wt_managed_local_port(srv);

    moq_connection_target_t target;
    if (row->target_authority)
        set_target(&target, row->target_authority, row->target_path_query);
    moq_pico_wt_managed_cfg_t cfg;
    client_cfg(&cfg, row->host, bound);
    cfg.sni = row->sni;
    cfg.path = row->cfg_path;
    cfg.target = row->target_authority ? &target : NULL;

    int served = 0;
    moq_pico_wt_managed_t *cli = NULL;
    CHECK(moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK);
    if (cli) {
        for (int k = 0; k < 40 && !served; k++) {
            moq_pico_wt_managed_wait(cli, 100000);
            served = moq_pico_wt_managed_session(cli) != NULL;
            if (moq_pico_wt_managed_is_fatal(cli)) break;
        }
        moq_pico_wt_managed_stop(cli);
        moq_pico_wt_managed_destroy(cli);
    }
    moq_pico_wt_managed_stop(srv);          /* joins the recording thread */
    if (served) {
        char want[300];
        snprintf(want, sizeof(want), row->want_authority_fmt, bound);
        const char *got = moq_pico_wt_managed_test_connect_authority(srv);
        const char *got_path = moq_pico_wt_managed_test_connect_path(srv);
        const char *got_sni = moq_pico_wt_managed_test_connect_sni(srv);
        if (strcmp(got, want) != 0 || strcmp(got_path, row->want_path) != 0 ||
            strcmp(got_sni, row->want_sni) != 0)
            fprintf(stderr, "  connect row host=%s: :authority \"%s\" "
                    "(want \"%s\"), :path \"%s\" (want \"%s\"), SNI \"%s\" "
                    "(want \"%s\")\n", row->host, got, want, got_path,
                    row->want_path, got_sni, row->want_sni);
        CHECK(strcmp(got, want) == 0);
        CHECK(strcmp(got_path, row->want_path) == 0);
        CHECK(strcmp(got_sni, row->want_sni) == 0);
    }
    moq_pico_wt_managed_destroy(srv);
    return served;
}

/* The CONNECT dereferences the client's one identity (RFC 9114 Section
 * 4.3.1; draft-ietf-moq-transport-22 Section 6.2.1): :authority is the
 * target's authority exactly as spelled, or host:port (IPv6 bracketed) when
 * derived; :path is the target's path/query exactly as spelled -- percent
 * escapes kept -- with "/" in front of an empty or query-only one. A path
 * configured alongside the target either matches that projection or is
 * refused. The TLS SNI is independent: a bare name, never the port. */
static void test_connect_identity(const char *cert, const char *key)
{
    static const int ports[] = { 26443, 26587, 26731, 26877 };
    static const connect_row_t rows[] = {
        /* derived: https://127.0.0.1:<port>/moq */
        { "127.0.0.1", "test.example.com", NULL, NULL, NULL, "/moq",
          "127.0.0.1:%d", "/moq", "test.example.com" },
        /* derived, IPv6 literal: https://[::1]:<port>/moq */
        { "::1", "test.example.com", NULL, NULL, NULL, "/moq",
          "[::1]:%d", "/moq", "test.example.com" },
        /* derived, default SNI from a DNS host stays the bare name */
        { "localhost", NULL, NULL, NULL, NULL, "/moq",
          "localhost:%d", "/moq", "localhost" },
        /* https://relay.example:4433/moq dialed at a loopback address */
        { "127.0.0.1", "relay.example", "relay.example:4433", "/moq", NULL,
          "/moq", "relay.example:4433", "/moq", "relay.example" },
        /* https://relay.example/moq: the default port stays elided */
        { "127.0.0.1", "relay.example", "relay.example", "/moq", NULL,
          "/moq", "relay.example", "/moq", "relay.example" },
        /* https://[::1]:4433/moq spelled by the caller */
        { "127.0.0.1", "test.example.com", "[::1]:4433", "/moq", NULL,
          "/moq", "[::1]:4433", "/moq", "test.example.com" },
        /* a percent-encoded path and query: sent as spelled, no path set */
        { "127.0.0.1", "relay.example", "relay.example:4433",
          "/tenant%2Fone?token=a%26b", NULL, "/tenant%2Fone",
          "relay.example:4433", "/tenant%2Fone?token=a%26b", "relay.example" },
        /* the same target with a matching configured path */
        { "127.0.0.1", "relay.example", "relay.example:4433",
          "/tenant%2Fone?token=a%26b", "/tenant%2Fone?token=a%26b",
          "/tenant%2Fone", "relay.example:4433",
          "/tenant%2Fone?token=a%26b", "relay.example" },
        /* https://relay.example: no path component sends "/" */
        { "127.0.0.1", "relay.example", "relay.example", "", NULL, "/",
          "relay.example", "/", "relay.example" },
        /* https://relay.example?q=1: the query follows that "/" */
        { "127.0.0.1", "relay.example", "relay.example", "?q=1", "/?q=1",
          "/", "relay.example", "/?q=1", "relay.example" },
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        int proven = 0;
        for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven;
             i++)
            proven = connect_row(cert, key, ports[i], &rows[r]);
        if (!proven)
            fprintf(stderr, "  connect row %zu never served\n", r);
        CHECK(proven);
    }
}

/* Refused identities fail create before any transport effect: no facade
 * exists, so nothing was dialed. */
static void expect_refused(const char *what, moq_pico_wt_managed_cfg_t *cfg)
{
    moq_pico_wt_managed_t *cli = (moq_pico_wt_managed_t *)1;
    moq_result_t rc = moq_pico_wt_managed_create(cfg, &cli);
    if (rc != MOQ_ERR_INVAL || cli != NULL)
        fprintf(stderr, "  %s: create %d, facade %p\n", what, (int)rc,
                (void *)cli);
    CHECK(rc == MOQ_ERR_INVAL);
    CHECK(cli == NULL);
    if (rc == MOQ_OK && cli) {
        moq_pico_wt_managed_stop(cli);
        moq_pico_wt_managed_destroy(cli);
    }
}

/* Accepted identities are kept whole: the client's owned target is the one
 * it will bind. Dials a dead port; only create's result is examined. */
static void expect_target(const char *what, moq_pico_wt_managed_cfg_t *cfg,
                          const char *authority, const char *path_query)
{
    moq_pico_wt_managed_t *cli = NULL;
    CHECK(moq_pico_wt_managed_create(cfg, &cli) == MOQ_OK);
    if (!cli) {
        fprintf(stderr, "  %s: create refused\n", what);
        return;
    }
    const moq_connection_target_t *t = moq_pico_wt_managed_test_target(cli);
    int ok = t && t->target_present == MOQ_BINDING_TARGET_COMPLETE &&
             t->authority.len == strlen(authority) &&
             memcmp(t->authority.data, authority, t->authority.len) == 0 &&
             t->path_query.len == strlen(path_query) &&
             (!t->path_query.len ||
              memcmp(t->path_query.data, path_query, t->path_query.len) == 0);
    if (!ok)
        fprintf(stderr, "  %s: target present=%u authority %zu path %zu "
                "(want %zu, %zu)\n", what, t ? t->target_present : 0u,
                t ? t->authority.len : (size_t)0,
                t ? t->path_query.len : (size_t)0, strlen(authority),
                strlen(path_query));
    CHECK(ok);
    moq_pico_wt_managed_stop(cli);
    moq_pico_wt_managed_destroy(cli);
}

/* The identity is one value or a create error, never two resources and
 * never an absent target: a configured path that conflicts with the target
 * is refused, a long derived path keeps its target, and the binding's bound
 * (authority plus path/query at most 65535 bytes) holds on both sides. */
static void test_connect_identity_limits(void)
{
    moq_pico_wt_managed_cfg_t cfg;
    moq_connection_target_t t;

    /* a path that names another resource than the target */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.example:4433", "/tenant%2Fone?token=a%26b");
    cfg.path = "/moq";
    cfg.target = &t;
    expect_refused("conflicting path", &cfg);

    /* the target's :path projection, not its raw spelling, is what a
     * configured path is compared with */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.example", "?q=1");
    cfg.path = "?q=1";
    cfg.target = &t;
    expect_refused("unprojected query-only path", &cfg);

    /* an incomplete target, or one carrying a URI policy */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.example", "/moq");
    t.target_present = MOQ_BINDING_AUTHORITY_PRESENT;
    cfg.target = &t;
    expect_refused("incomplete target", &cfg);
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "", "/moq");
    cfg.target = &t;
    expect_refused("empty target authority", &cfg);

    /* embedded NUL in either component */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.example", "/moq");
    t.authority.data = (const uint8_t *)"relay\0.example";
    t.authority.len = 14;
    cfg.target = &t;
    expect_refused("NUL in authority", &cfg);
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.example", "/moq");
    t.path_query.data = (const uint8_t *)"/m\0q";
    t.path_query.len = 4;
    cfg.target = &t;
    expect_refused("NUL in path", &cfg);

    /* derived: "/" + 1100 'a' with host localhost:443 keeps its target */
    static char long_path[1102];
    long_path[0] = '/';
    memset(long_path + 1, 'a', 1100);
    long_path[1101] = '\0';
    client_cfg(&cfg, "localhost", 443);
    cfg.path = long_path;
    expect_target("derived long path", &cfg, "localhost:443", long_path);

    /* the binding bound, derived: "localhost:1" is 11 bytes */
    static char edge[65536];
    edge[0] = '/';
    memset(edge + 1, 'b', sizeof(edge) - 2);
    edge[65535 - 11] = '\0';                     /* 65524-byte path */
    client_cfg(&cfg, "localhost", 1);
    cfg.path = edge;
    expect_target("derived at the bound", &cfg, "localhost:1", edge);
    edge[65535 - 11] = 'b';
    edge[65535 - 11 + 1] = '\0';                 /* one byte over */
    client_cfg(&cfg, "localhost", 1);
    cfg.path = edge;
    expect_refused("derived over the bound", &cfg);

    /* the binding bound, explicit target */
    edge[65535 - 11] = '\0';
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.ex:1", edge);           /* 10 + 65524 bytes */
    cfg.target = &t;
    expect_target("target under the bound", &cfg, "relay.ex:1", edge);
    edge[65535 - 11] = 'b';                       /* 10 + 65525 = 65535 */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.ex:1", edge);
    cfg.target = &t;
    expect_target("target at the bound", &cfg, "relay.ex:1", edge);
    edge[65535 - 11 + 1] = 'b';
    edge[65535 - 11 + 2] = '\0';                  /* 10 + 65526 */
    client_cfg(&cfg, "127.0.0.1", 1);
    set_target(&t, "relay.ex:1", edge);
    cfg.target = &t;
    expect_refused("target over the bound", &cfg);
}

/* -- stop() from on_pump must be rejected -------------------------- */

typedef struct { atomic_int done; int stop_rc; } stopcheck_app_t;

static int stopcheck_pump(moq_pico_wt_managed_t *m, uint64_t now, void *ctx)
{
    (void)now;
    stopcheck_app_t *a = (stopcheck_app_t *)ctx;
    if (!atomic_load(&a->done)) {
        a->stop_rc = (int)moq_pico_wt_managed_stop(m);  /* network thread */
        atomic_store(&a->done, 1);
    }
    return 0;
}

static void test_stop_from_pump(const char *cert, const char *key)
{
    /* Needs the client to attach (so on_pump runs), which needs a bound
     * server — retry candidate ports; only assert once on_pump ran. */
    const int ports[] = { 18484, 19484, 20484 };
    int proven = 0;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !proven;
         i++) {
        moq_pico_wt_managed_t *srv =
            make_test_server(cert, key, ports[i]);
        if (!srv) continue;

        stopcheck_app_t app; memset(&app, 0, sizeof(app));
        atomic_init(&app.done, 0);
        app.stop_rc = 0;

        moq_pico_wt_managed_cfg_t cfg;
        base_client_cfg(&cfg, moq_pico_wt_managed_local_port(srv));
        cfg.insecure_skip_verify = true;
        cfg.on_pump = stopcheck_pump;
        cfg.on_pump_ctx = &app;
        moq_pico_wt_managed_t *cli = NULL;
        if (moq_pico_wt_managed_create(&cfg, &cli) == MOQ_OK && cli) {
            uint64_t waited = 0;
            while (!atomic_load(&app.done) && waited < 5000) {
                if (moq_pico_wt_managed_wait(cli, 200000) == MOQ_ERR_CLOSED)
                    break;
                waited += 200;
            }
            int done = atomic_load(&app.done);
            int rc = app.stop_rc;
            moq_pico_wt_managed_stop(cli);
            moq_pico_wt_managed_destroy(cli);
            if (done) {
                CHECK(rc == MOQ_ERR_WRONG_STATE);    /* stop() refused */
                proven = 1;
            }
            /* else: client never attached (server may not have bound) —
             * try the next candidate port. */
        }
        moq_pico_wt_managed_stop(srv);
        moq_pico_wt_managed_destroy(srv);
    }
    CHECK(proven);   /* on_pump ran and stop() was rejected */
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cert") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: %s --cert <f> --key <f>\n", argv[0]);
        return 2;
    }

    test_cfg_validation();
    test_null_safety();
    test_ip_host_sni_policy();
    test_connect_failure();
    test_connect_prepare(cert, key, 0);
    test_connect_prepare(cert, key, 1);
    test_lifecycle();
    test_cert_failure(cert, key);
    test_helper_cert_failure(cert, key);
    test_default_cert_failure(cert, key);
    test_stop_from_pump(cert, key);
    test_connect_identity(cert, key);
    test_connect_identity_limits();

    if (failures == 0)
        printf("test_pico_wt_managed_edges: PASS\n");
    else
        fprintf(stderr, "test_pico_wt_managed_edges: %d failure(s)\n",
                failures);
    return failures ? 1 : 0;
}
