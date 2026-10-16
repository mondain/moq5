/*
 * moq-interop-client — MoQ interop test runner.
 *
 * Connects to a relay via the libmoq service tier (moq_endpoint_t) and runs
 * named test cases. Outputs TAP on stdout; logs on stderr.
 *
 * The endpoint tier owns URL parsing, transport selection (raw QUIC for
 * moqt://, H3 WebTransport for https://), version negotiation (offers every
 * draft this build supports and reads back the negotiated one), TLS/SNI, and
 * the network thread. The test cases drive the sans-I/O core directly via the
 * endpoint's post() executor, which runs moq_session_* calls on the network
 * thread where they are legal. This is the protocol/conformance dogfood of the
 * service tier; application integrators should start with
 * examples/service/ instead.
 */

#include <moq/endpoint.h>
#include <moq/session.h>
#include <moq/rcbuf.h>

#include "endpoint_error.h"
#include "object_relay.h"
#include "sub_answers.h"
#include "url_policy.h"

#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Monotonic clock                                                     */
/* ------------------------------------------------------------------ */

static uint64_t now_us_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ------------------------------------------------------------------ */
/* Test modes                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    TEST_SETUP_ONLY,
    TEST_ANNOUNCE_ONLY,
    TEST_PUBLISH_NAMESPACE_DONE,
    TEST_SUBSCRIBE_ERROR,
} test_mode_t;

/* ------------------------------------------------------------------ */
/* Shared test state (mutex-protected)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t     mu;
    bool                verbose;
    test_mode_t         mode;

    /* State flags (written by pump task on the net thread, read by app). */
    bool                setup_complete;
    bool                session_closed;

    /* announce-only / publish-namespace-done */
    bool                req_publish_ns;
    bool                publish_ns_sent;
    bool                publish_ns_failed;
    int                 publish_ns_rc;
    bool                ns_accepted;
    bool                ns_rejected;
    moq_announcement_t  ann;

    bool                req_publish_ns_done;
    bool                publish_ns_done_sent;
    bool                publish_ns_done_failed;
    int                 publish_ns_done_rc;

    /* subscribe-error / dual-session subscriber side */
    bool                req_subscribe;
    bool                subscribe_sent;
    bool                subscribe_failed;
    int                 subscribe_rc;
    bool                subscribe_ok;
    bool                subscribe_error;
    moq_subscription_t  sub;
    moq_bytes_t        *subscribe_ns;
    size_t              subscribe_ns_count;

    /* dual-session publisher side: accept the test track's SUBSCRIBE and
     * refuse every other forwarded one (sub_answers.h) */
    bool                req_accept_sub;
    interop_sub_answers_t answers;
    bool                accept_sub_sent;
    bool                accept_sub_failed;
    int                 accept_sub_rc;

    int                 last_state;
    const char         *log_prefix;
} test_ctx_t;

static void ctx_init(test_ctx_t *c, test_mode_t mode, bool verbose,
                      const char *prefix)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    c->verbose = verbose;
    c->mode = mode;
    c->log_prefix = prefix ? prefix : "";
}

static void ctx_destroy(test_ctx_t *c)
{
    pthread_mutex_destroy(&c->mu);
}

/* ------------------------------------------------------------------ */
/* Shared constants                                                    */
/* ------------------------------------------------------------------ */

static moq_bytes_t ns_interop_parts[] = {
    { (const uint8_t *)"moq-test", 8 },
    { (const uint8_t *)"interop", 7 },
    { NULL, 0 },   /* optional per-case label; see set_case_ns() */
};
static size_t ns_interop_count = 2;

/* Run-all runs every case in one process against one relay. If they all shared
 * the "moq-test/interop" namespace, a case that announces it and then tears the
 * session down (currently without a graceful CONNECTION_CLOSE) leaves the relay
 * holding that namespace until its idle timeout, so the NEXT announce-case
 * collides ("namespace rejected"). Giving each case a distinct 3rd label keeps
 * them independent regardless of teardown timing. NULL/empty -> 2-part (the
 * single-case default, unchanged). */
static void set_case_ns(const char *tag)
{
    if (tag && *tag) {
        ns_interop_parts[2].data = (const uint8_t *)tag;
        ns_interop_parts[2].len  = strlen(tag);
        ns_interop_count = 3;
    } else {
        ns_interop_parts[2].data = NULL;
        ns_interop_parts[2].len  = 0;
        ns_interop_count = 2;
    }
}

static moq_bytes_t ns_nonexistent_parts[] = {
    { (const uint8_t *)"nonexistent", 11 },
    { (const uint8_t *)"namespace", 9 },
};

/* ------------------------------------------------------------------ */
/* Pump task — posted onto the endpoint's network thread each cycle.    */
/*                                                                      */
/* Runs EXACTLY ONCE per moq_endpoint_post() call (with session, or     */
/* session == NULL on terminal drain). It performs any pending session  */
/* calls and drains queued session events into the shared ctx. The app  */
/* thread re-posts it each wait cycle to keep events flowing — the       */
/* endpoint's own pump does not consume session events.                 */
/* ------------------------------------------------------------------ */

static moq_result_t interop_pump_task(moq_endpoint_t *ep, moq_session_t *sess,
                                      uint64_t now, void *ctx)
{
    (void)ep;
    test_ctx_t *c = (test_ctx_t *)ctx;
    if (!sess) {
        /* Terminal drain marker: session gone. */
        pthread_mutex_lock(&c->mu);
        c->session_closed = true;
        pthread_mutex_unlock(&c->mu);
        return MOQ_OK;
    }

    /* Track session state -> setup_complete. */
    moq_session_state_t state = moq_session_state(sess);
    pthread_mutex_lock(&c->mu);
    if ((int)state != c->last_state) {
        if (c->verbose)
            fprintf(stderr, "  [%s]  session state %d \xe2\x86\x92 %d\n",
                c->log_prefix, c->last_state, (int)state);
        c->last_state = (int)state;
    }
    if (state == MOQ_SESS_ESTABLISHED && !c->setup_complete)
        c->setup_complete = true;

    /* Refusals queued by earlier cycles that the session could not take. */
    interop_sub_answers_flush(&c->answers, sess, now);

    /* Snapshot requested actions. */
    bool do_publish_ns = c->req_publish_ns && !c->publish_ns_sent;
    bool do_publish_ns_done = c->req_publish_ns_done && !c->publish_ns_done_sent;
    bool do_subscribe = c->req_subscribe && !c->subscribe_sent;
    bool do_accept_sub = c->req_accept_sub && c->answers.have_test
                         && !c->accept_sub_sent;
    moq_announcement_t ann_copy = c->ann;
    moq_subscription_t incoming_sub_copy = c->answers.test_sub;
    pthread_mutex_unlock(&c->mu);

    if (do_publish_ns) {
        moq_publish_namespace_cfg_t ncfg;
        moq_publish_namespace_cfg_init(&ncfg);
        ncfg.track_namespace.parts = ns_interop_parts;
        ncfg.track_namespace.count = ns_interop_count;
        moq_announcement_t ann;
        moq_result_t rc = moq_session_publish_namespace(sess, &ncfg, now, &ann);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) {
            c->publish_ns_sent = true;
            c->ann = ann;
            if (c->verbose)
                fprintf(stderr, "  [%s] publish_namespace sent\n", c->log_prefix);
        } else if (rc == MOQ_ERR_WOULD_BLOCK) {
            /* Leave the request flag set; the next posted cycle retries. */
        } else {
            c->publish_ns_sent = true;
            c->publish_ns_failed = true;
            c->publish_ns_rc = (int)rc;
            if (c->verbose)
                fprintf(stderr, "  [%s] publish_namespace failed: %d\n", c->log_prefix, rc);
        }
        pthread_mutex_unlock(&c->mu);
    }

    if (do_publish_ns_done) {
        moq_result_t rc = moq_session_publish_namespace_done(sess, ann_copy, now);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) {
            c->publish_ns_done_sent = true;
            if (c->verbose)
                fprintf(stderr, "  [%s] publish_namespace_done sent\n", c->log_prefix);
        } else if (rc == MOQ_ERR_WOULD_BLOCK) {
            /* retry next cycle */
        } else {
            c->publish_ns_done_sent = true;
            c->publish_ns_done_failed = true;
            c->publish_ns_done_rc = (int)rc;
            if (c->verbose)
                fprintf(stderr, "  [%s] publish_namespace_done failed: %d\n", c->log_prefix, rc);
        }
        pthread_mutex_unlock(&c->mu);
    }

    if (do_subscribe) {
        moq_subscribe_cfg_t scfg;
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace.parts = c->subscribe_ns
            ? c->subscribe_ns : ns_nonexistent_parts;
        scfg.track_namespace.count = c->subscribe_ns
            ? c->subscribe_ns_count : 2;
        scfg.track_name = (moq_bytes_t){ (const uint8_t *)"test-track", 10 };
        scfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        moq_subscription_t sub;
        moq_result_t rc = moq_session_subscribe(sess, &scfg, now, &sub);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) {
            c->subscribe_sent = true;
            c->sub = sub;
            if (c->verbose)
                fprintf(stderr, "  [%s] subscribe sent\n", c->log_prefix);
        } else if (rc == MOQ_ERR_WOULD_BLOCK) {
            /* retry next cycle */
        } else {
            c->subscribe_sent = true;
            c->subscribe_failed = true;
            c->subscribe_rc = (int)rc;
            if (c->verbose)
                fprintf(stderr, "  [%s] subscribe failed: %d\n", c->log_prefix, rc);
        }
        pthread_mutex_unlock(&c->mu);
    }

    if (do_accept_sub) {
        moq_accept_subscribe_cfg_t acfg;
        moq_accept_subscribe_cfg_init(&acfg);
        moq_result_t rc = moq_session_accept_subscribe(
            sess, incoming_sub_copy, &acfg, now);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) {
            c->accept_sub_sent = true;
            if (c->verbose)
                fprintf(stderr, "  [%s] accept_subscribe sent\n", c->log_prefix);
        } else if (rc == MOQ_ERR_WOULD_BLOCK) {
            /* retry next cycle */
        } else {
            c->accept_sub_sent = true;
            c->accept_sub_failed = true;
            c->accept_sub_rc = (int)rc;
            if (c->verbose)
                fprintf(stderr, "  [%s] accept_subscribe failed: %d\n",
                    c->log_prefix, rc);
        }
        pthread_mutex_unlock(&c->mu);
    }

    /* Drain events. */
    moq_event_t ev;
    while (moq_session_poll_events(sess, &ev, 1) == 1) {
        if (c->verbose)
            fprintf(stderr, "  [%s] event kind=%u\n", c->log_prefix, ev.kind);
        pthread_mutex_lock(&c->mu);
        switch (ev.kind) {
        case MOQ_EVENT_SESSION_CLOSED:
            c->session_closed = true;
            break;
        case MOQ_EVENT_NAMESPACE_ACCEPTED:
            c->ns_accepted = true;
            break;
        case MOQ_EVENT_NAMESPACE_REJECTED:
            c->ns_rejected = true;
            break;
        case MOQ_EVENT_SUBSCRIBE_OK:
            c->subscribe_ok = true;
            break;
        case MOQ_EVENT_SUBSCRIBE_ERROR:
            c->subscribe_error = true;
            break;
        case MOQ_EVENT_SUBSCRIBE_REQUEST:
            /* The test track's first request is accepted on the next cycle
             * (when req_accept_sub); every other one is refused below. */
            interop_sub_answers_on_request(&c->answers, sess,
                                           &ev.u.subscribe_request);
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&c->mu);
        moq_event_cleanup(&ev);
    }

    pthread_mutex_lock(&c->mu);
    interop_sub_answers_flush(&c->answers, sess, now);
    pthread_mutex_unlock(&c->mu);

    return MOQ_OK;
}

/* ------------------------------------------------------------------ */
/* Transport label (for diagnostics): derived from the URL scheme.     */
/* ------------------------------------------------------------------ */

static const char *transport_label(const char *url)
{
    if (strncmp(url, "https://", 8) == 0) return "webtransport";
    return "quic"; /* moqt:// / moq:// */
}

/* ------------------------------------------------------------------ */
/* CLI / env                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *relay_url;
    const char *test_name;
    bool        tls_disable_verify;
    bool        verbose;
    int         draft;   /* 0 = auto (offer every draft); 16/18/22 = pin exactly */
    moq_transport_backend_t backend;   /* 0 = AUTO (picoquic); see --backend */
    const char *backend_name;          /* the selected name, for diagnostics */
    uint32_t wt_profile;               /* moq_wt_profile_t; 0 = backend default */
    const char *wt_profile_name;       /* the selected name, for diagnostics */
} cli_opts_t;

/* Transport backend selection (INTERNAL testing knob, default AUTO=picoquic):
 * drives cfg.backend so the same interop suite can exercise libmoq's own
 * adapters. Some backends are transport-restricted; parse_cli cross-checks the
 * name against the URL scheme. Not part of the interop-runner interface. */
static const struct {
    const char             *name;
    moq_transport_backend_t backend;
    bool                    raw_quic;      /* usable with moqt:// */
    bool                    webtransport;  /* usable with https:// */
} g_backends[] = {
    { "auto",          MOQ_TRANSPORT_BACKEND_AUTO,           true,  true  },
    { "picoquic",      MOQ_TRANSPORT_BACKEND_PICOQUIC,       true,  true  },
    { "msquic",        MOQ_TRANSPORT_BACKEND_MSQUIC,         true,  false },
    { "mvfst",         MOQ_TRANSPORT_BACKEND_MVFST,          true,  false },
    { "proxygen",      MOQ_TRANSPORT_BACKEND_PROXYGEN,       false, true  },
    /* WebTransport over MsQuic (cross-platform). */
    { "wtquic-msquic", MOQ_TRANSPORT_BACKEND_WTQUIC_MSQUIC,  false, true  },
    /* WebTransport over Apple Network.framework -- EXPERIMENTAL: its public
     * NF QUIC multiplex client drops an eager H3 server's control stream, so
     * establishment against third-party relays is unreliable (see the wtquic
     * COMPATIBILITY notes). Selectable for evidence only, never a default.
     * There is deliberately NO bare "wtquic" alias: bare wtquic names no
     * upstream transport and is always rejected. */
    { "wtquic-network", MOQ_TRANSPORT_BACKEND_WTQUIC_NETWORK, false, true  },
};

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --relay URL --test NAME [options]\n"
        "\n"
        "  --relay URL             moqt://host:port (raw QUIC) or\n"
        "                          https://host:port[/path] (WebTransport;\n"
        "                          no path means the root mount \"/\")\n"
        "  --test NAME             test case name (default: run all cases)\n"
        "  --draft N               pin the offered draft to 16, 18 or 22\n"
        "                          (default: offer every draft, negotiate one)\n"
        "  --backend NAME          transport backend (testing): auto|picoquic|\n"
        "                          msquic|mvfst|proxygen|wtquic-msquic|\n"
        "                          wtquic-network (default: auto). wtquic-network\n"
        "                          is EXPERIMENTAL; bare \"wtquic\" is not accepted.\n"
        "  --wt-profile NAME       WebTransport-over-H3 dialect, wtquic-msquic ONLY:\n"
        "                          current (:protocol=webtransport-h3, default) or\n"
        "                          d13-14 (:protocol=webtransport, proxygen/moxygen/\n"
        "                          moqx/h3zero family). Rejected for any other\n"
        "                          backend (fixed dialect / no WebTransport).\n"
        "  --tls-disable-verify    skip TLS certificate verification\n"
        "  --verbose               print session state and events to stderr\n"
        "  --help                  show this help\n"
        "\n"
        "Environment fallback (CLI wins):\n"
        "  RELAY_URL, TESTCASE, MOQT_DRAFT, MOQT_BACKEND, MOQT_WT_PROFILE,\n"
        "  TLS_DISABLE_VERIFY=1\n"
        "\n"
        "Transport (raw QUIC vs WebTransport) is selected automatically from the\n"
        "URL scheme. The draft version is auto-negotiated (the endpoint offers\n"
        "every draft this build supports) unless pinned with --draft/MOQT_DRAFT.\n"
        "\n"
        "Supported: setup-only, announce-only, publish-namespace-done,\n"
        "  subscribe-error, announce-subscribe, subscribe-before-announce\n"
        "Named only (not in the run-all suite): object-relay -- opaque object\n"
        "  conformance through a relay (MOQ_INTEROP_OBJECT_RELAY_FAULT=\n"
        "  corrupt-byte|drop-object|extra-object injects a publisher fault)\n",
        prog);
}

/* Strict draft parse: NULL/empty -> 0 (auto), exactly "16", "18" or "22" -> that
 * draft, anything else -> -1 (rejected). atoi would silently accept "16junk"
 * as 16 and "abc" as 0 (auto), falsifying per-draft interop results. */
static int parse_draft(const char *s)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || (v != 16 && v != 18 && v != 22)) return -1;
    return (int)v;
}

static cli_opts_t parse_cli(int argc, char **argv)
{
    cli_opts_t opts = {0};
    const char *draft_str = NULL;
    const char *backend_str = NULL;
    const char *wt_profile_str = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc)
            opts.relay_url = argv[++i];
        else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc)
            opts.test_name = argv[++i];
        else if (strcmp(argv[i], "--draft") == 0 && i + 1 < argc)
            draft_str = argv[++i];
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            backend_str = argv[++i];
        else if (strcmp(argv[i], "--wt-profile") == 0 && i + 1 < argc)
            wt_profile_str = argv[++i];
        else if (strcmp(argv[i], "--tls-disable-verify") == 0)
            opts.tls_disable_verify = true;
        else if (strcmp(argv[i], "--verbose") == 0)
            opts.verbose = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "unknown flag: %s (try --help)\n", argv[i]);
            exit(1);
        }
    }
    if (!opts.relay_url) opts.relay_url = getenv("RELAY_URL");
    if (!opts.test_name) opts.test_name = getenv("TESTCASE");
    /* No test named (bare `make test`, which sets RELAY_URL but no TESTCASE):
     * leave test_name NULL so main() runs the whole suite, like the other
     * interop clients (moq-rs, imquic, ...). */
    if (!opts.tls_disable_verify) {
        const char *env = getenv("TLS_DISABLE_VERIFY");
        if (env && strcmp(env, "1") == 0) opts.tls_disable_verify = true;
    }
    /* Draft pin: --draft wins, else MOQT_DRAFT, else auto (offer all). */
    if (!draft_str) {
        const char *env = getenv("MOQT_DRAFT");
        if (env && *env) draft_str = env;
    }
    opts.draft = parse_draft(draft_str);
    if (opts.draft < 0) {
        fprintf(stderr, "error: --draft/MOQT_DRAFT must be exactly 16, 18 or 22 "
                "(got \"%s\"); omit for auto-negotiation\n", draft_str);
        exit(1);
    }
    /* Backend: --backend wins, else MOQT_BACKEND, else AUTO (picoquic). */
    if (!backend_str) {
        const char *env = getenv("MOQT_BACKEND");
        if (env && *env) backend_str = env;
    }
    if (backend_str) {
        int bi = -1;
        for (size_t k = 0; k < sizeof(g_backends) / sizeof(g_backends[0]); k++)
            if (strcmp(backend_str, g_backends[k].name) == 0) { bi = (int)k; break; }
        if (bi < 0) {
            fprintf(stderr, "error: --backend/MOQT_BACKEND unknown \"%s\" "
                    "(auto|picoquic|msquic|mvfst|proxygen|wtquic-msquic|"
                    "wtquic-network; bare \"wtquic\" is not accepted)\n",
                    backend_str);
            exit(1);
        }
        opts.backend = g_backends[bi].backend;
        opts.backend_name = g_backends[bi].name;
        /* Backends are transport-restricted; catch a scheme mismatch here with
         * a clear message rather than a bare MOQ_ERR_UNSUPPORTED at connect. */
        if (opts.relay_url) {
            bool is_wt = strncmp(opts.relay_url, "https://", 8) == 0;
            if (is_wt && !g_backends[bi].webtransport) {
                fprintf(stderr, "error: backend \"%s\" is raw-QUIC only; "
                        "use a moqt:// relay URL\n", backend_str);
                exit(1);
            }
            if (!is_wt && !g_backends[bi].raw_quic) {
                fprintf(stderr, "error: backend \"%s\" is WebTransport only; "
                        "use an https:// relay URL\n", backend_str);
                exit(1);
            }
        }
    }
    /* WebTransport wire profile: --wt-profile wins, else MOQT_WT_PROFILE. It is
     * a dialect knob honored ONLY by the wtquic-msquic backend; every other
     * backend has a FIXED WebTransport dialect (picoquic/proxygen/wtquic-network)
     * or no WebTransport at all (raw QUIC). The value forwarded to the library
     * defaults to BACKEND_DEFAULT (no explicit selection; wtquic-msquic maps it
     * to CURRENT), but the REQUEST IDENTITY (below) is derived from the backend,
     * so a run can never certify a "current"/"d13-14" dialect the transport did
     * not actually speak. */
    opts.wt_profile = (uint32_t)MOQ_WT_PROFILE_BACKEND_DEFAULT;
    if (!wt_profile_str) {
        const char *env = getenv("MOQT_WT_PROFILE");
        if (env && *env) wt_profile_str = env;
    }
    bool profile_selectable =
        (opts.backend == MOQ_TRANSPORT_BACKEND_WTQUIC_MSQUIC);
    if (wt_profile_str) {
        /* An explicit profile is accepted ONLY where it is actually applied.
         * Accepting it for a backend that ignores it would let the run claim a
         * dialect it never used -- reject instead. */
        if (!profile_selectable) {
            fprintf(stderr, "error: --wt-profile/MOQT_WT_PROFILE applies only "
                    "to the wtquic-msquic backend; backend \"%s\" has a fixed "
                    "WebTransport dialect or no WebTransport\n",
                    opts.backend_name ? opts.backend_name : "auto");
            exit(1);
        }
        if (strcmp(wt_profile_str, "current") == 0) {
            opts.wt_profile = (uint32_t)MOQ_WT_PROFILE_CURRENT;
        } else if (strcmp(wt_profile_str, "d13-14") == 0) {
            opts.wt_profile = (uint32_t)MOQ_WT_PROFILE_D13_14_COMPAT;
        } else {
            fprintf(stderr, "error: --wt-profile/MOQT_WT_PROFILE must be "
                    "\"current\" or \"d13-14\" (got \"%s\")\n", wt_profile_str);
            exit(1);
        }
    }
    /* Truthful request identity: the wtquic-msquic backend reports the selected
     * profile; any other WebTransport backend reports its fixed dialect as
     * "native"; a raw-QUIC transport reports "n/a" (no WebTransport at all). */
    bool is_wt = opts.relay_url != NULL &&
                 strncmp(opts.relay_url, "https://", 8) == 0;
    if (profile_selectable)
        opts.wt_profile_name =
            opts.wt_profile == (uint32_t)MOQ_WT_PROFILE_D13_14_COMPAT
                ? "d13-14" : "current";
    else if (is_wt)
        opts.wt_profile_name = "native";
    else
        opts.wt_profile_name = "n/a";
    return opts;
}

/* ------------------------------------------------------------------ */
/* TAP output                                                          */
/* ------------------------------------------------------------------ */

/* Draft to pin for this run: 0 = auto (offer every draft this build supports),
 * 16/18/22 = offer exactly that one. Set once from the CLI/env in main(). */
static int g_draft_pin = 0;

/* Transport backend for this run (default AUTO = picoquic); set from
 * --backend / MOQT_BACKEND in main(). */
static moq_transport_backend_t g_backend = MOQ_TRANSPORT_BACKEND_AUTO;

/* The selected backend's name, for TAP/diagnostics (default "auto"). */
static const char *g_backend_name = "auto";

/* WebTransport wire profile for this run; set from --wt-profile /
 * MOQT_WT_PROFILE in main(). Only the wtquic-msquic backend applies the value;
 * the identity string is the backend's truthful dialect ("current"/"d13-14" for
 * wtquic-msquic, "native" for a fixed-dialect WebTransport backend, "n/a" for
 * raw QUIC) -- never a dialect the transport did not speak. */
static uint32_t g_wt_profile = (uint32_t)MOQ_WT_PROFILE_BACKEND_DEFAULT;
static const char *g_wt_profile_name = "n/a";

static void tap_plan(int n)
{
    printf("TAP version 14\n");
    printf("1..%d\n", n);
    /* Name the requested backend + draft pin up front, so a connect FAILURE
     * (which never reaches the negotiated line) still records exactly which
     * backend/draft combination was exercised. */
    printf("# request: backend=%s wt-profile=%s draft=%s\n", g_backend_name,
           g_wt_profile_name,
           g_draft_pin == 16 ? "16" : g_draft_pin == 18 ? "18" :
           g_draft_pin == 22 ? "22" : "auto");
    fflush(stdout);
}

static void tap_result(int num, bool ok, const char *desc,
                        uint64_t duration_ms, const char *message)
{
    printf("%s %d - %s\n", ok ? "ok" : "not ok", num, desc);
    printf("  ---\n");
    printf("  duration_ms: %" PRIu64 "\n", duration_ms);
    if (message)
        printf("  message: \"%s\"\n", message);
    printf("  ...\n");
    /* Stream each result: a relay that hangs a later sub-test, or a kill on
     * the runner's per-test timeout, then still leaves partial TAP captured. */
    fflush(stdout);
}

/* Emit a planned step that could not run because an earlier step failed, as a
 * TAP SKIP (`ok N - desc # SKIP reason`). This keeps the plan complete without
 * inflating one root failure into several: the real failure stays the single
 * `not ok`, and the unreached steps are skips, not failures. */
static void tap_result_skip(int num, const char *desc, const char *reason)
{
    printf("ok %d - %s # SKIP %s\n", num, desc, reason);
    fflush(stdout);
}

static void tap_skip(const char *test_name, const char *reason)
{
    printf("TAP version 14\n");
    printf("1..1\n");
    printf("ok 1 - %s # SKIP %s\n", test_name, reason);
}

/* Emit a TAP comment naming the negotiated draft + transport, once setup is
 * complete. Comments are ignored by harness pass/fail accounting. */
static void tap_negotiated(moq_endpoint_t *ep, const char *url)
{
    printf("# negotiated: draft-%u over %s (backend: %s)\n",
        (unsigned)moq_endpoint_negotiated_version(ep), transport_label(url),
        g_backend_name);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Endpoint helpers                                                     */
/* ------------------------------------------------------------------ */

/* Create + start an endpoint. Transport, draft offer, and TLS are all owned
 * by the endpoint: AUTO protocol derives the transport from the URL scheme;
 * the zero-init version offer means "offer every draft this build supports"
 * unless a specific draft was pinned (--draft / MOQT_DRAFT). */
/* Why the last make_endpoint call failed (main thread only). */
static moq_result_t g_endpoint_rc = MOQ_OK;

/* The TAP message for a step whose endpoint could not be created: the
 * connect's result and, for an unsupported pinned draft, a hint. Valid until
 * the next call. */
static const char *endpoint_failure(const char *what)
{
    static char buf[256];
    return interop_endpoint_error(buf, sizeof(buf), what, g_endpoint_rc,
                                  (uint32_t)g_draft_pin);
}

static moq_endpoint_t *make_endpoint(const char *url, bool insecure)
{
    moq_endpoint_cfg_t cfg;
    /* Sized init: we set wt_profile, a struct_size-gated tail field, so the
     * pointer-only init (which stamps only the v0 floor) would drop it. */
    moq_endpoint_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    cfg.protocol = MOQ_TRANSPORT_PROTOCOL_AUTO;
    cfg.backend = g_backend;
    cfg.wt_profile = g_wt_profile;
    /* cfg.versions left zero-init == MOQ_VERSION_POLICY_AUTO (offer all). */
    if (g_draft_pin != 0) {
        /* EXACT offer: propose only the pinned draft; a peer that can't speak
         * it fails the connect (that is the point of a pinned interop run).
         * The offer is copied by connect, so a static single-element array is
         * a safe backing store for the borrowed pointer. */
        static moq_version_t pinned;
        pinned = (moq_version_t)g_draft_pin;
        cfg.versions.struct_size  = sizeof(moq_version_offer_t);
        cfg.versions.policy       = MOQ_VERSION_POLICY_EXACT;
        cfg.versions.versions     = &pinned;
        cfg.versions.version_count = 1;
    }
    cfg.insecure_skip_verify = insecure;
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    /* This tool's URL conventions (port 4433 by default, the "moq" scheme,
     * the wt_path override below) are the endpoint's LEGACY reading. */
    cfg.uri_mode = MOQ_ENDPOINT_URI_LEGACY;
    /* cfg.sni empty -> defaults to URL host; cfg.wt_path empty -> URL path,
     * which the endpoint then defaults to "/moq". A relay URL with no path at
     * all asked for the root mount instead, which is where interop relays
     * commonly publish WebTransport; an explicit path still wins. */
    {
        moq_bytes_t wt_path_override;
        if (interop_resolve_wt_path(url, &wt_path_override))
            cfg.wt_path = wt_path_override;
    }

    moq_endpoint_t *ep = NULL;
    g_endpoint_rc = moq_endpoint_connect(&cfg, &ep);
    if (g_endpoint_rc != MOQ_OK) return NULL;
    return ep;
}

static void close_endpoint(moq_endpoint_t *ep)
{
    if (!ep) return;
    moq_endpoint_stop(ep);
    moq_endpoint_destroy(ep);
}

/* One pump cycle: post the drain/execute task onto the net thread, then block
 * up to rem_us for it to run / the endpoint to wake.
 *
 * Concurrency invariant (do not break): the posted task (interop_pump_task)
 * holds c->mu only to read/write ctx fields and releases it before every
 * moq_session_* call; the app thread holds c->mu only to read/write ctx and
 * never while calling a moq_endpoint_* function. That ordering is what keeps
 * the endpoint's internal lock and c->mu from forming an ABBA cycle. */
static moq_result_t ep_cycle(moq_endpoint_t *ep, test_ctx_t *c, uint64_t rem_us)
{
    /* post() returning terminal means the endpoint is already closed; the
     * wait() below then returns MOQ_ERR_CLOSED and the caller's loop exits. */
    moq_endpoint_post(ep, interop_pump_task, c);
    return moq_endpoint_wait(ep, rem_us);
}

/* Wait until *flag is set, or the session/endpoint goes terminal, or timeout. */
static bool wait_for_flag(moq_endpoint_t *ep, test_ctx_t *c, bool *flag,
                           uint64_t timeout_us, const char **out_reason)
{
    uint64_t deadline = now_us_mono() + timeout_us;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        bool done = *flag;
        bool closed = c->session_closed;
        pthread_mutex_unlock(&c->mu);
        if (done) return true;
        if (closed || moq_endpoint_is_closed(ep)) {
            *out_reason = "session closed"; return false;
        }
        if (moq_endpoint_is_fatal(ep)) { *out_reason = "fatal"; return false; }
        uint64_t now = now_us_mono();
        if (now >= deadline) { *out_reason = "timeout"; return false; }
        uint64_t rem = deadline - now;
        if (rem > 100000) rem = 100000;
        moq_result_t wr = ep_cycle(ep, c, rem);
        if (wr == MOQ_ERR_CLOSED) { *out_reason = "connection closed"; return false; }
    }
}

/* ------------------------------------------------------------------ */
/* Test: setup-only                                                    */
/* ------------------------------------------------------------------ */

static int run_setup_only(const char *url, bool insecure, bool verbose)
{
    test_ctx_t c;
    ctx_init(&c, TEST_SETUP_ONLY, verbose, NULL);

    moq_endpoint_t *ep = make_endpoint(url, insecure);
    if (!ep) {
        tap_plan(1);
        tap_result(1, false, "setup handshake", 0, endpoint_failure("endpoint"));
        ctx_destroy(&c);
        return 1;
    }

    if (verbose) fprintf(stderr, "connecting to %s...\n", url);

    uint64_t t0 = now_us_mono();
    const char *reason = NULL;
    bool ok = wait_for_flag(ep, &c, &c.setup_complete, 3000000, &reason);
    uint64_t ms = (now_us_mono() - t0) / 1000;

    tap_plan(1);
    if (ok) tap_negotiated(ep, url);
    tap_result(1, ok, "setup handshake", ms,
        ok ? "MoQ setup complete" : reason);

    close_endpoint(ep);
    ctx_destroy(&c);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Test: announce-only                                                 */
/* ------------------------------------------------------------------ */

static int run_announce_only(const char *url, bool insecure, bool verbose)
{
    test_ctx_t c;
    ctx_init(&c, TEST_ANNOUNCE_ONLY, verbose, NULL);
    int exit_code = 0;

    moq_endpoint_t *ep = make_endpoint(url, insecure);
    if (!ep) {
        tap_plan(2);
        tap_result(1, false, "setup", 0, endpoint_failure("endpoint"));
        tap_result_skip(2, "namespace accepted", "setup failed");
        ctx_destroy(&c);
        return 1;
    }

    if (verbose) fprintf(stderr, "connecting to %s...\n", url);

    uint64_t t0 = now_us_mono();
    const char *reason = NULL;
    bool setup_ok = wait_for_flag(ep, &c, &c.setup_complete, 3000000, &reason);
    uint64_t ms1 = (now_us_mono() - t0) / 1000;

    tap_plan(2);
    if (setup_ok) tap_negotiated(ep, url);
    tap_result(1, setup_ok, "setup", ms1,
        setup_ok ? "MoQ setup complete" : reason);
    if (!setup_ok) {
        /* Setup failed: the later step cannot run, but the plan promised it --
         * emit it as a SKIP so the TAP stays complete (plan == results) without
         * turning one setup failure into two failures. */
        tap_result_skip(2, "namespace accepted", "setup failed");
        exit_code = 1; goto done;
    }

    /* Request publish_namespace. */
    pthread_mutex_lock(&c.mu);
    c.req_publish_ns = true;
    pthread_mutex_unlock(&c.mu);

    uint64_t t1 = now_us_mono();
    uint64_t ns_deadline = t1 + 3000000;
    const char *ns_reason = "timeout";
    while (true) {
        pthread_mutex_lock(&c.mu);
        bool accepted = c.ns_accepted;
        bool rejected = c.ns_rejected;
        bool failed = c.publish_ns_failed;
        bool closed = c.session_closed;
        pthread_mutex_unlock(&c.mu);
        if (accepted || rejected || failed) break;
        if (closed || moq_endpoint_is_closed(ep)) { ns_reason = "session closed"; break; }
        if (moq_endpoint_is_fatal(ep)) { ns_reason = "fatal"; break; }
        uint64_t now = now_us_mono();
        if (now >= ns_deadline) break;
        uint64_t rem = ns_deadline - now;
        if (rem > 100000) rem = 100000;
        moq_result_t wr = ep_cycle(ep, &c, rem);
        if (wr == MOQ_ERR_CLOSED) { ns_reason = "connection closed"; break; }
    }
    uint64_t ms2 = (now_us_mono() - t1) / 1000;

    pthread_mutex_lock(&c.mu);
    bool ns_accepted = c.ns_accepted;
    bool ns_rejected = c.ns_rejected;
    bool ns_failed = c.publish_ns_failed;
    int ns_rc = c.publish_ns_rc;
    pthread_mutex_unlock(&c.mu);

    if (ns_failed) {
        char msg[64];
        snprintf(msg, sizeof(msg), "publish_namespace failed: %d", ns_rc);
        tap_result(2, false, "namespace accepted", ms2, msg);
        exit_code = 1;
    } else if (ns_rejected) {
        tap_result(2, false, "namespace accepted", ms2,
            "namespace rejected by relay");
        exit_code = 1;
    } else {
        tap_result(2, ns_accepted, "namespace accepted", ms2,
            ns_accepted ? "NAMESPACE_ACCEPTED received" : ns_reason);
        if (!ns_accepted) exit_code = 1;
    }

done:
    close_endpoint(ep);
    ctx_destroy(&c);
    return exit_code;
}

/* ------------------------------------------------------------------ */
/* Test: publish-namespace-done                                        */
/* ------------------------------------------------------------------ */

static int run_publish_namespace_done(const char *url, bool insecure,
                                       bool verbose)
{
    test_ctx_t c;
    ctx_init(&c, TEST_PUBLISH_NAMESPACE_DONE, verbose, NULL);
    int exit_code = 0;

    moq_endpoint_t *ep = make_endpoint(url, insecure);
    if (!ep) {
        tap_plan(3);
        tap_result(1, false, "setup", 0, endpoint_failure("endpoint"));
        tap_result_skip(2, "namespace accepted", "setup failed");
        tap_result_skip(3, "namespace done sent", "setup failed");
        ctx_destroy(&c);
        return 1;
    }

    if (verbose) fprintf(stderr, "connecting to %s...\n", url);

    uint64_t t0 = now_us_mono();
    const char *reason = NULL;
    bool setup_ok = wait_for_flag(ep, &c, &c.setup_complete, 3000000, &reason);
    uint64_t ms1 = (now_us_mono() - t0) / 1000;

    tap_plan(3);
    if (setup_ok) tap_negotiated(ep, url);
    tap_result(1, setup_ok, "setup", ms1,
        setup_ok ? "MoQ setup complete" : reason);
    if (!setup_ok) {
        /* Keep the TAP complete: the two unreached steps are skips, not
         * failures, so one setup failure is not counted as three. */
        tap_result_skip(2, "namespace accepted", "setup failed");
        tap_result_skip(3, "namespace done sent", "setup failed");
        exit_code = 1; goto done;
    }

    /* Publish namespace. */
    pthread_mutex_lock(&c.mu);
    c.req_publish_ns = true;
    pthread_mutex_unlock(&c.mu);

    uint64_t t1 = now_us_mono();
    uint64_t ns_deadline = t1 + 3000000;
    const char *ns_reason = "timeout";
    while (true) {
        pthread_mutex_lock(&c.mu);
        bool accepted = c.ns_accepted;
        bool rejected = c.ns_rejected;
        bool failed = c.publish_ns_failed;
        bool closed = c.session_closed;
        pthread_mutex_unlock(&c.mu);
        if (accepted || rejected || failed) break;
        if (closed || moq_endpoint_is_closed(ep)) { ns_reason = "session closed"; break; }
        if (moq_endpoint_is_fatal(ep)) { ns_reason = "fatal"; break; }
        uint64_t now = now_us_mono();
        if (now >= ns_deadline) break;
        uint64_t rem = ns_deadline - now;
        if (rem > 100000) rem = 100000;
        moq_result_t wr = ep_cycle(ep, &c, rem);
        if (wr == MOQ_ERR_CLOSED) { ns_reason = "connection closed"; break; }
    }
    uint64_t ms2 = (now_us_mono() - t1) / 1000;

    pthread_mutex_lock(&c.mu);
    bool ns_accepted2 = c.ns_accepted;
    bool ns_failed2 = c.publish_ns_failed;
    int ns_rc2 = c.publish_ns_rc;
    pthread_mutex_unlock(&c.mu);

    if (ns_failed2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "publish_namespace failed: %d", ns_rc2);
        tap_result(2, false, "namespace accepted", ms2, msg);
        tap_result_skip(3, "namespace done sent", "namespace not accepted");
        exit_code = 1; goto done;
    }
    tap_result(2, ns_accepted2, "namespace accepted", ms2,
        ns_accepted2 ? "NAMESPACE_ACCEPTED received" : ns_reason);
    if (!ns_accepted2) {
        tap_result_skip(3, "namespace done sent", "namespace not accepted");
        exit_code = 1; goto done;
    }

    /* Send publish_namespace_done. */
    pthread_mutex_lock(&c.mu);
    c.req_publish_ns_done = true;
    pthread_mutex_unlock(&c.mu);

    uint64_t t2 = now_us_mono();
    reason = NULL;
    bool done_ok = wait_for_flag(ep, &c, &c.publish_ns_done_sent, 3000000, &reason);
    uint64_t ms3 = (now_us_mono() - t2) / 1000;

    pthread_mutex_lock(&c.mu);
    bool done_failed = c.publish_ns_done_failed;
    int done_rc = c.publish_ns_done_rc;
    pthread_mutex_unlock(&c.mu);

    if (done_failed) {
        char msg[64];
        snprintf(msg, sizeof(msg), "publish_namespace_done failed: %d", done_rc);
        tap_result(3, false, "namespace done sent", ms3, msg);
        exit_code = 1;
    } else {
        tap_result(3, done_ok, "namespace done sent", ms3,
            done_ok ? "PUBLISH_NAMESPACE_DONE sent" : reason);
        if (!done_ok) exit_code = 1;
    }

done:
    close_endpoint(ep);
    ctx_destroy(&c);
    return exit_code;
}

/* ------------------------------------------------------------------ */
/* Test: subscribe-error                                               */
/* ------------------------------------------------------------------ */

static int run_subscribe_error(const char *url, bool insecure, bool verbose)
{
    test_ctx_t c;
    ctx_init(&c, TEST_SUBSCRIBE_ERROR, verbose, NULL);
    int exit_code = 0;

    moq_endpoint_t *ep = make_endpoint(url, insecure);
    if (!ep) {
        tap_plan(2);
        tap_result(1, false, "setup", 0, endpoint_failure("endpoint"));
        tap_result_skip(2, "subscribe error or timeout", "setup failed");
        ctx_destroy(&c);
        return 1;
    }

    if (verbose) fprintf(stderr, "connecting to %s...\n", url);

    uint64_t t0 = now_us_mono();
    const char *reason = NULL;
    bool setup_ok = wait_for_flag(ep, &c, &c.setup_complete, 3000000, &reason);
    uint64_t ms1 = (now_us_mono() - t0) / 1000;

    tap_plan(2);
    if (setup_ok) tap_negotiated(ep, url);
    tap_result(1, setup_ok, "setup", ms1,
        setup_ok ? "MoQ setup complete" : reason);
    if (!setup_ok) {
        tap_result_skip(2, "subscribe error or timeout", "setup failed");
        exit_code = 1; goto done;
    }

    /* Subscribe to nonexistent namespace. */
    pthread_mutex_lock(&c.mu);
    c.req_subscribe = true;
    pthread_mutex_unlock(&c.mu);

    uint64_t t1 = now_us_mono();
    uint64_t sub_deadline = t1 + 3000000;
    bool got_error = false;
    bool got_ok = false;
    bool sub_call_failed = false;
    bool sub_was_sent = false;
    while (true) {
        pthread_mutex_lock(&c.mu);
        got_error = c.subscribe_error;
        got_ok = c.subscribe_ok;
        sub_call_failed = c.subscribe_failed;
        sub_was_sent = c.subscribe_sent && !c.subscribe_failed;
        bool closed = c.session_closed;
        pthread_mutex_unlock(&c.mu);
        if (got_error || got_ok || sub_call_failed) break;
        if (closed || moq_endpoint_is_closed(ep) || moq_endpoint_is_fatal(ep)) break;
        uint64_t now = now_us_mono();
        if (now >= sub_deadline) break;
        uint64_t rem = sub_deadline - now;
        if (rem > 100000) rem = 100000;
        moq_result_t wr = ep_cycle(ep, &c, rem);
        if (wr == MOQ_ERR_CLOSED) break;
    }
    uint64_t ms2 = (now_us_mono() - t1) / 1000;

    /* Re-read final state after loop exit. */
    pthread_mutex_lock(&c.mu);
    got_error = c.subscribe_error;
    got_ok = c.subscribe_ok;
    sub_call_failed = c.subscribe_failed;
    sub_was_sent = c.subscribe_sent && !c.subscribe_failed;
    bool sub_closed = c.session_closed;
    pthread_mutex_unlock(&c.mu);
    if (moq_endpoint_is_closed(ep) || moq_endpoint_is_fatal(ep)) sub_closed = true;

    if (sub_call_failed) {
        pthread_mutex_lock(&c.mu);
        int src = c.subscribe_rc;
        pthread_mutex_unlock(&c.mu);
        char msg[64];
        snprintf(msg, sizeof(msg), "subscribe call failed: %d", src);
        tap_result(2, false, "subscribe error or timeout", ms2, msg);
        exit_code = 1;
    } else if (got_ok) {
        tap_result(2, false, "subscribe error or timeout", ms2,
            "unexpected SUBSCRIBE_OK for nonexistent namespace");
        exit_code = 1;
    } else if (got_error) {
        tap_result(2, true, "subscribe error or timeout", ms2,
            "SUBSCRIBE_ERROR received");
    } else if (sub_closed) {
        tap_result(2, false, "subscribe error or timeout", ms2,
            "session closed before response");
        exit_code = 1;
    } else if (sub_was_sent) {
        tap_result(2, true, "subscribe error or timeout", ms2,
            "timeout after SUBSCRIBE sent (acceptable per interop spec)");
    } else {
        tap_result(2, false, "subscribe error or timeout", ms2,
            "SUBSCRIBE never sent");
        exit_code = 1;
    }

done:
    close_endpoint(ep);
    ctx_destroy(&c);
    return exit_code;
}

/* ------------------------------------------------------------------ */
/* Dual-session: common pub+sub test logic                             */
/* ------------------------------------------------------------------ */

static int run_dual_session(const char *url, bool insecure, bool verbose,
                            bool subscribe_first)
{
    test_ctx_t pub_ctx, sub_ctx;
    ctx_init(&pub_ctx, TEST_ANNOUNCE_ONLY, verbose, "pub");
    ctx_init(&sub_ctx, TEST_SUBSCRIBE_ERROR, verbose, "sub");
    sub_ctx.subscribe_ns = ns_interop_parts;
    sub_ctx.subscribe_ns_count = ns_interop_count;
    pub_ctx.req_accept_sub = true;
    interop_sub_answers_init(&pub_ctx.answers,
                             (moq_namespace_t){ ns_interop_parts, ns_interop_count },
                             (moq_bytes_t){ (const uint8_t *)"test-track", 10 });

    moq_endpoint_t *pub_ep = NULL, *sub_ep = NULL;

    static const char *names[5] = {
        "publisher setup",
        "publisher namespace accepted",
        "subscriber setup",
        "publisher accepted subscribe",
        "subscriber got SubscribeOk",
    };
    bool     ok[5]  = { false, false, false, false, false };
    uint64_t ms[5]  = { 0 };
    char     msg[5][128];
    for (int i = 0; i < 5; i++) snprintf(msg[i], sizeof(msg[i]), "skipped");
    bool announced_version = false;

#define FAIL(n, ...) do { \
    ok[n] = false; \
    snprintf(msg[n], sizeof(msg[n]), __VA_ARGS__); \
} while (0)
#define PASS(n, ...) do { \
    ok[n] = true; \
    snprintf(msg[n], sizeof(msg[n]), __VA_ARGS__); \
} while (0)

    /* -- subscribe-before-announce: subscriber connects first -- */
    if (subscribe_first) {
        sub_ep = make_endpoint(url, insecure);
        if (!sub_ep) { FAIL(2, "%s", endpoint_failure("subscriber")); goto emit; }
        if (verbose) fprintf(stderr, "[sub] connecting to %s...\n", url);

        uint64_t t0 = now_us_mono();
        const char *r = NULL;
        bool s = wait_for_flag(sub_ep, &sub_ctx, &sub_ctx.setup_complete, 3000000, &r);
        ms[2] = (now_us_mono() - t0) / 1000;
        if (s) { PASS(2, "MoQ setup complete"); }
        else   { FAIL(2, "%s", r); goto emit; }

        pthread_mutex_lock(&sub_ctx.mu);
        sub_ctx.req_subscribe = true;
        pthread_mutex_unlock(&sub_ctx.mu);
        /* drive one cycle so the SUBSCRIBE is sent before the publisher exists */
        ep_cycle(sub_ep, &sub_ctx, 50000);

        struct timespec sl = { 0, 500000000 };
        nanosleep(&sl, NULL);

        pub_ep = make_endpoint(url, insecure);
        if (!pub_ep) { FAIL(0, "%s", endpoint_failure("publisher")); goto emit; }
        if (verbose) fprintf(stderr, "[pub] connecting to %s...\n", url);
    } else {
        pub_ep = make_endpoint(url, insecure);
        if (!pub_ep) { FAIL(0, "%s", endpoint_failure("publisher")); goto emit; }
        if (verbose) fprintf(stderr, "[pub] connecting to %s...\n", url);
    }

    /* 1: publisher setup */
    {
        uint64_t t0 = now_us_mono();
        const char *r = NULL;
        bool s = wait_for_flag(pub_ep, &pub_ctx, &pub_ctx.setup_complete, 3000000, &r);
        ms[0] = (now_us_mono() - t0) / 1000;
        if (s) {
            PASS(0, "MoQ setup complete");
            if (!announced_version) { tap_negotiated(pub_ep, url); announced_version = true; }
        } else { FAIL(0, "%s", r); goto emit; }
    }

    /* 2: publisher namespace accepted */
    {
        pthread_mutex_lock(&pub_ctx.mu);
        pub_ctx.req_publish_ns = true;
        pthread_mutex_unlock(&pub_ctx.mu);

        uint64_t t0 = now_us_mono();
        uint64_t dl = t0 + 3000000;
        const char *nr = "timeout";
        while (true) {
            pthread_mutex_lock(&pub_ctx.mu);
            bool accepted = pub_ctx.ns_accepted;
            bool rejected = pub_ctx.ns_rejected;
            bool failed = pub_ctx.publish_ns_failed;
            bool closed = pub_ctx.session_closed;
            pthread_mutex_unlock(&pub_ctx.mu);
            if (accepted || rejected || failed) break;
            if (closed || moq_endpoint_is_closed(pub_ep)) { nr = "session closed"; break; }
            if (moq_endpoint_is_fatal(pub_ep)) { nr = "fatal"; break; }
            uint64_t now = now_us_mono();
            if (now >= dl) break;
            uint64_t rem = dl - now;
            if (rem > 100000) rem = 100000;
            moq_result_t wr = ep_cycle(pub_ep, &pub_ctx, rem);
            if (wr == MOQ_ERR_CLOSED) { nr = "connection closed"; break; }
        }
        ms[1] = (now_us_mono() - t0) / 1000;

        pthread_mutex_lock(&pub_ctx.mu);
        bool na = pub_ctx.ns_accepted;
        bool nf = pub_ctx.publish_ns_failed;
        int nrc = pub_ctx.publish_ns_rc;
        bool nj = pub_ctx.ns_rejected;
        pthread_mutex_unlock(&pub_ctx.mu);

        if (nf)       { FAIL(1, "publish_namespace failed: %d", nrc); goto emit; }
        else if (nj)  { FAIL(1, "namespace rejected by relay"); goto emit; }
        else if (na)  { PASS(1, "NAMESPACE_ACCEPTED received"); }
        else          { FAIL(1, "%s", nr); goto emit; }
    }

    /* announce-subscribe: subscriber connects after announce. */
    if (!subscribe_first) {
        sub_ep = make_endpoint(url, insecure);
        if (!sub_ep) { FAIL(2, "%s", endpoint_failure("subscriber")); goto emit; }
        if (verbose) fprintf(stderr, "[sub] connecting to %s...\n", url);

        uint64_t t0 = now_us_mono();
        const char *r = NULL;
        bool s = wait_for_flag(sub_ep, &sub_ctx, &sub_ctx.setup_complete, 3000000, &r);
        ms[2] = (now_us_mono() - t0) / 1000;
        if (s) { PASS(2, "MoQ setup complete"); }
        else   { FAIL(2, "%s", r); goto emit; }

        pthread_mutex_lock(&sub_ctx.mu);
        sub_ctx.req_subscribe = true;
        pthread_mutex_unlock(&sub_ctx.mu);
    }

    /* 4: publisher accepted subscribe. Both endpoints must be pumped: the
     * subscriber's SUBSCRIBE has to reach the relay and be forwarded to the
     * publisher, whose pump task then accepts it. */
    {
        uint64_t t0 = now_us_mono();
        uint64_t dl = t0 + 5000000;
        const char *ar = "timeout";
        while (true) {
            pthread_mutex_lock(&pub_ctx.mu);
            bool sent = pub_ctx.accept_sub_sent;
            bool afail = pub_ctx.accept_sub_failed;
            bool closed = pub_ctx.session_closed;
            /* Accepting the test SUBSCRIBE is not enough: every other
             * forwarded request must be answered too (a queued refusal keeps
             * the wait going until it is sent or the deadline passes). */
            interop_verdict_t av = interop_publisher_verdict(sent, &pub_ctx.answers, NULL);
            bool unanswered = pub_ctx.answers.unanswered;
            pthread_mutex_unlock(&pub_ctx.mu);
            if (afail || unanswered || av == INTEROP_VERDICT_PASS) break;
            if (subscribe_first) {
                /* Premature SUBSCRIBE rejected by the relay -> the publisher
                 * will never see one to accept; stop waiting (that rejection
                 * is the expected outcome, resolved below). */
                pthread_mutex_lock(&sub_ctx.mu);
                bool serr_early = sub_ctx.subscribe_error;
                pthread_mutex_unlock(&sub_ctx.mu);
                if (serr_early) break;
            }
            if (closed || moq_endpoint_is_closed(pub_ep)) { ar = "session closed"; break; }
            if (moq_endpoint_is_fatal(pub_ep)) { ar = "fatal"; break; }
            uint64_t now = now_us_mono();
            if (now >= dl) break;
            /* pump the subscriber too, so its SUBSCRIBE flows */
            if (sub_ep) ep_cycle(sub_ep, &sub_ctx, 5000);
            moq_result_t wr = ep_cycle(pub_ep, &pub_ctx, 45000);
            if (wr == MOQ_ERR_CLOSED) { ar = "connection closed"; break; }
        }
        ms[3] = (now_us_mono() - t0) / 1000;

        pthread_mutex_lock(&pub_ctx.mu);
        bool asent = pub_ctx.accept_sub_sent;
        bool afail = pub_ctx.accept_sub_failed;
        int arc = pub_ctx.accept_sub_rc;
        const char *answer_why = NULL;
        interop_verdict_t av = interop_publisher_verdict(true, &pub_ctx.answers,
                                                         &answer_why);
        pthread_mutex_unlock(&pub_ctx.mu);

        /* subscribe-before-announce: a relay that rejected the premature
         * SUBSCRIBE correctly leaves the publisher with nothing to accept.
         * Treat that as the expected pass; step 5 then confirms the
         * SUBSCRIBE_ERROR the subscriber saw. */
        bool sub_rejected = false;
        if (subscribe_first) {
            pthread_mutex_lock(&sub_ctx.mu);
            sub_rejected = sub_ctx.subscribe_error;
            pthread_mutex_unlock(&sub_ctx.mu);
        }

        if (afail)             { FAIL(3, "accept_subscribe failed: %d", arc); goto emit; }
        else if (av == INTEROP_VERDICT_FAIL) { FAIL(3, "%s", answer_why); goto emit; }
        else if (av == INTEROP_VERDICT_WAIT) {
            FAIL(3, "a forwarded SUBSCRIBE refusal was still queued at the deadline");
            goto emit;
        }
        else if (asent)        { PASS(3, "SUBSCRIBE accepted"); }
        else if (sub_rejected) { PASS(3, "relay rejected premature SUBSCRIBE (expected)"); }
        else                   { FAIL(3, "%s", ar); goto emit; }
    }

    /* 5: subscriber got SubscribeOk */
    {
        uint64_t t0 = now_us_mono();
        uint64_t dl = t0 + 3000000;
        const char *sr = "timeout";
        while (true) {
            pthread_mutex_lock(&sub_ctx.mu);
            bool sok = sub_ctx.subscribe_ok;
            bool serr = sub_ctx.subscribe_error;
            bool sfail = sub_ctx.subscribe_failed;
            bool closed = sub_ctx.session_closed;
            pthread_mutex_unlock(&sub_ctx.mu);
            if (sok || serr || sfail) break;
            if (closed || moq_endpoint_is_closed(sub_ep)) { sr = "session closed"; break; }
            if (moq_endpoint_is_fatal(sub_ep)) { sr = "fatal"; break; }
            uint64_t now = now_us_mono();
            if (now >= dl) break;
            uint64_t rem = dl - now;
            if (rem > 100000) rem = 100000;
            moq_result_t wr = ep_cycle(sub_ep, &sub_ctx, rem);
            if (wr == MOQ_ERR_CLOSED) { sr = "connection closed"; break; }
        }
        ms[4] = (now_us_mono() - t0) / 1000;

        pthread_mutex_lock(&sub_ctx.mu);
        bool sok = sub_ctx.subscribe_ok;
        bool serr = sub_ctx.subscribe_error;
        bool sfail = sub_ctx.subscribe_failed;
        int src = sub_ctx.subscribe_rc;
        pthread_mutex_unlock(&sub_ctx.mu);

        if (sfail)      { FAIL(4, "subscribe failed: %d", src); }
        else if (subscribe_first) {
            /* subscribe-before-announce: the subscriber issued SUBSCRIBE before
             * the namespace existed. A relay that rejects it (SUBSCRIBE_ERROR)
             * is behaving correctly -- that is the expected pass. A relay that
             * instead holds the request and delivers SUBSCRIBE_OK once the
             * publisher announces is also valid. Either outcome passes; only a
             * transport failure or timeout fails. (announce-subscribe below
             * still requires SUBSCRIBE_OK.) */
            if (serr)      { PASS(4, "SUBSCRIBE_ERROR (expected before announce)"); }
            else if (sok)  { PASS(4, "SUBSCRIBE_OK received"); }
            else           { FAIL(4, "%s", sr); }
        }
        else if (serr)  { FAIL(4, "SUBSCRIBE_ERROR received"); }
        else if (sok)   { PASS(4, "SUBSCRIBE_OK received"); }
        else            { FAIL(4, "%s", sr); }
    }

emit:
    /* A forwarded request that went unanswered at any point fails the
     * publisher's step, also after it first passed. */
    {
        const char *late_why = NULL;
        pthread_mutex_lock(&pub_ctx.mu);
        interop_verdict_t lv = interop_publisher_verdict(ok[3], &pub_ctx.answers,
                                                         &late_why);
        pthread_mutex_unlock(&pub_ctx.mu);
        if (ok[3] && lv != INTEROP_VERDICT_PASS) {
            ok[3] = false;
            snprintf(msg[3], sizeof(msg[3]), "%s", late_why ? late_why
                     : "a forwarded SUBSCRIBE refusal was still queued");
        }
    }
    close_endpoint(sub_ep);
    close_endpoint(pub_ep);

    tap_plan(5);
    int exit_code = 0;
    for (int i = 0; i < 5; i++) {
        tap_result(i + 1, ok[i], names[i], ms[i], msg[i]);
        if (!ok[i]) exit_code = 1;
    }

    ctx_destroy(&sub_ctx);
    ctx_destroy(&pub_ctx);
    return exit_code;

#undef FAIL
#undef PASS
}

static int run_announce_subscribe(const char *url, bool insecure, bool verbose)
{
    return run_dual_session(url, insecure, verbose, false);
}

static int run_subscribe_before_announce(const char *url, bool insecure,
                                          bool verbose)
{
    return run_dual_session(url, insecure, verbose, true);
}

/* ------------------------------------------------------------------ */
/* Test: object-relay (opaque object conformance through a relay)      */
/* ------------------------------------------------------------------ */

/* A publisher session and a subscriber session, each its own connection to
 * the relay, exchange the fixed Object sequence of object_relay.h through it:
 * the publisher announces a namespace unique to this run, the subscriber
 * subscribes to a track in it through the relay, the publisher accepts the
 * forwarded SUBSCRIBE and, once the subscriber holds SUBSCRIBE_OK, writes
 * ORL_GROUPS subgroups of ORL_OBJECTS objects, closes each subgroup with a FIN
 * and ends the subscription with PUBLISH_DONE TRACK_ENDED. The subscriber
 * checks every identity, length and byte against the sequence and requires
 * every FIN and the done after the tail. Payloads are opaque: this is object
 * transport conformance, not catalog discovery, codec parsing or playback.
 *
 * Every wait is bounded; a timeout is a failure. MOQ_INTEROP_OBJECT_RELAY_FAULT
 * (corrupt-byte | drop-object | extra-object) makes the PUBLISHER deviate from
 * the sequence so the unchanged verifier can be shown to reject it; it is a
 * test-only knob and never part of a passing run. */

#define ORL_SETUP_US      3000000u
#define ORL_SUBSCRIBE_US  5000000u
#define ORL_DELIVERY_US  10000000u

typedef enum {
    ORL_FAULT_NONE,
    ORL_FAULT_CORRUPT_BYTE,   /* g=1 o=2: one payload byte flipped */
    ORL_FAULT_DROP_OBJECT,    /* g=1 o=3: never written */
    ORL_FAULT_EXTRA_OBJECT,   /* g=2: an object o=ORL_OBJECTS after the tail */
} orl_fault_t;

typedef struct {
    pthread_mutex_t     mu;
    bool                verbose;
    const char         *log_prefix;
    const moq_bytes_t  *ns;
    size_t              ns_count;
    moq_bytes_t         track;

    bool                setup_complete;
    bool                session_closed;

    /* publisher */
    bool                req_publish_ns;
    bool                publish_ns_sent;
    bool                publish_ns_failed;
    int                 publish_ns_rc;
    bool                ns_accepted;
    bool                ns_rejected;
    interop_sub_answers_t answers;   /* the test track's SUBSCRIBE; others refused */
    moq_subscription_t  up_sub;
    bool                accept_sent;
    bool                accept_failed;
    int                 accept_rc;
    bool                go;
    orl_fault_t         fault;
    uint64_t            g_next;
    uint64_t            o_next;
    bool                sg_open;
    moq_subgroup_handle_t sg;
    uint64_t            streams_opened;
    uint64_t            objects_written;
    uint64_t            bytes_written;
    uint64_t            done_delay_us;
    uint64_t            tail_closed_us;
    bool                done_sent;
    bool                publish_failed;
    int                 publish_rc;
    const char         *publish_step;

    /* subscriber */
    bool                req_subscribe;
    bool                subscribe_sent;
    bool                subscribe_failed;
    int                 subscribe_rc;
    bool                subscribe_ok;
    bool                subscribe_error;
    moq_subscription_t  sub;
    orl_verifier_t      ver;
    bool                chunk_open;
    bool                chunk_own;
    uint64_t            chunk_group, chunk_subgroup, chunk_object, chunk_status;
    size_t              chunk_len;
    uint8_t             chunk_buf[ORL_MAX_PAYLOAD];
} orl_ctx_t;

static void orl_ctx_init(orl_ctx_t *c, bool verbose, const char *prefix,
                         const moq_bytes_t *ns, size_t ns_count,
                         moq_bytes_t track)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    c->verbose = verbose;
    c->log_prefix = prefix;
    c->ns = ns;
    c->ns_count = ns_count;
    c->track = track;
    interop_sub_answers_init(&c->answers, (moq_namespace_t){ ns, ns_count }, track);
    orl_verifier_init(&c->ver);
}

/* Publisher: write as much of the sequence as the session admits now; a
 * WOULD_BLOCK leaves the position unchanged for the next cycle. Runs on the
 * network thread. Session calls are made without c->mu, except the refusals
 * interop_sub_answers_flush sends, whose queue and counts live under it (the
 * app thread never calls into the session, so holding it there is safe);
 * every field the app thread reads is updated under it. */
static void orl_publish_failed(orl_ctx_t *c, const char *step, moq_result_t rc)
{
    pthread_mutex_lock(&c->mu);
    c->publish_failed = true;
    c->publish_step = step;
    c->publish_rc = (int)rc;
    pthread_mutex_unlock(&c->mu);
}

static void orl_publish_step(orl_ctx_t *c, moq_session_t *sess, uint64_t now)
{
    static uint8_t buf[ORL_MAX_PAYLOAD];
    moq_result_t rc;
    while (c->g_next < ORL_GROUPS) {
        if (!c->sg_open) {
            moq_subgroup_cfg_t sgc;
            moq_subgroup_cfg_init(&sgc);
            sgc.group_id = c->g_next;
            sgc.subgroup_id = 0;
            sgc.publisher_priority = 128;
            /* A new subgroup carrying its first object ever published
             * (draft-18 and draft-22 Section 2.2). */
            sgc.first_object = true;
            rc = moq_session_open_subgroup(sess, c->up_sub, &sgc, now, &c->sg);
            if (rc == MOQ_ERR_WOULD_BLOCK) return;
            if (rc != MOQ_OK) { orl_publish_failed(c, "open_subgroup failed", rc); return; }
            c->sg_open = true;
            pthread_mutex_lock(&c->mu);
            c->streams_opened++;
            pthread_mutex_unlock(&c->mu);
        }
        uint64_t last = ORL_OBJECTS;
        if (c->fault == ORL_FAULT_EXTRA_OBJECT && c->g_next == ORL_GROUPS - 1)
            last = ORL_OBJECTS + 1;
        while (c->o_next < last) {
            uint64_t g = c->g_next, o = c->o_next;
            if (c->fault == ORL_FAULT_DROP_OBJECT && g == 1 && o == 3) {
                c->o_next++;
                continue;
            }
            /* The extra object (fault only) reuses the tail object's bytes. */
            uint64_t src = o < ORL_OBJECTS ? o : ORL_OBJECTS - 1;
            size_t len = orl_payload_len(g, src);
            orl_payload_fill(g, src, buf);
            if (c->fault == ORL_FAULT_CORRUPT_BYTE && g == 1 && o == 2)
                buf[len / 2] ^= 0x01;
            moq_rcbuf_t *pl = NULL;
            rc = moq_rcbuf_create(moq_alloc_default(), buf, len, &pl);
            if (rc != MOQ_OK) { orl_publish_failed(c, "rcbuf_create failed", rc); return; }
            rc = moq_session_write_object(sess, c->sg, o, pl, now);
            moq_rcbuf_decref(pl);   /* the session keeps its own reference */
            if (rc == MOQ_ERR_WOULD_BLOCK) return;
            if (rc != MOQ_OK) { orl_publish_failed(c, "write_object failed", rc); return; }
            c->o_next++;
            pthread_mutex_lock(&c->mu);
            c->objects_written++;
            c->bytes_written += len;
            pthread_mutex_unlock(&c->mu);
        }
        rc = moq_session_close_subgroup(sess, c->sg, now);
        if (rc == MOQ_ERR_WOULD_BLOCK) return;
        if (rc != MOQ_OK) { orl_publish_failed(c, "close_subgroup failed", rc); return; }
        c->sg_open = false;
        c->g_next++;
        c->o_next = 0;
    }
    /* PUBLISH_DONE only after every stream is closed (draft-18 Section
     * 10.11, draft-22 Section 9.9), counting each subgroup stream opened.
     * MOQ_INTEROP_OBJECT_RELAY_DONE_DELAY_MS holds it back that long after the
     * last FIN: a diagnostic that separates a relay ending its downstream
     * before forwarding the tail from a missing tail. A passing run never
     * sets it. */
    if (!c->tail_closed_us) c->tail_closed_us = now ? now : 1;
    if (now < c->tail_closed_us + c->done_delay_us) return;
    moq_done_subscribe_cfg_t dc;
    moq_done_subscribe_cfg_init(&dc);
    dc.status_code = ORL_DONE_TRACK_ENDED;
    dc.stream_count = c->streams_opened;
    rc = moq_session_done_subscribe(sess, c->up_sub, &dc, now);
    if (rc == MOQ_ERR_WOULD_BLOCK) return;
    if (rc != MOQ_OK) { orl_publish_failed(c, "done_subscribe failed", rc); return; }
    pthread_mutex_lock(&c->mu);
    c->done_sent = true;
    pthread_mutex_unlock(&c->mu);
}

static void orl_object(orl_ctx_t *c, bool own, uint64_t g, uint64_t sg,
                       uint64_t o, uint64_t status, const uint8_t *data,
                       size_t len)
{
    orl_verifier_object(&c->ver, own, g, sg, o, status, data, len);
}

static moq_result_t orl_pump_task(moq_endpoint_t *ep, moq_session_t *sess,
                                  uint64_t now, void *ctx)
{
    (void)ep;
    orl_ctx_t *c = (orl_ctx_t *)ctx;
    if (!sess) {
        pthread_mutex_lock(&c->mu);
        c->session_closed = true;
        pthread_mutex_unlock(&c->mu);
        return MOQ_OK;
    }

    pthread_mutex_lock(&c->mu);
    if (moq_session_state(sess) == MOQ_SESS_ESTABLISHED) c->setup_complete = true;
    /* Refusals queued by earlier cycles that the session could not take. */
    interop_sub_answers_flush(&c->answers, sess, now);
    bool do_publish_ns = c->req_publish_ns && !c->publish_ns_sent;
    bool do_accept = c->answers.have_test && !c->accept_sent;
    if (do_accept) c->up_sub = c->answers.test_sub;
    bool do_subscribe = c->req_subscribe && !c->subscribe_sent;
    bool do_publish = c->go && !c->done_sent && !c->publish_failed;
    pthread_mutex_unlock(&c->mu);

    if (do_publish_ns) {
        moq_publish_namespace_cfg_t ncfg;
        moq_publish_namespace_cfg_init(&ncfg);
        ncfg.track_namespace.parts = c->ns;
        ncfg.track_namespace.count = c->ns_count;
        moq_announcement_t ann;
        moq_result_t rc = moq_session_publish_namespace(sess, &ncfg, now, &ann);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) c->publish_ns_sent = true;
        else if (rc != MOQ_ERR_WOULD_BLOCK) {
            c->publish_ns_sent = c->publish_ns_failed = true;
            c->publish_ns_rc = (int)rc;
        }
        pthread_mutex_unlock(&c->mu);
    }
    if (do_accept) {
        moq_accept_subscribe_cfg_t acfg;
        moq_accept_subscribe_cfg_init(&acfg);
        moq_result_t rc = moq_session_accept_subscribe(sess, c->up_sub, &acfg, now);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) c->accept_sent = true;
        else if (rc != MOQ_ERR_WOULD_BLOCK) {
            c->accept_sent = c->accept_failed = true;
            c->accept_rc = (int)rc;
        }
        pthread_mutex_unlock(&c->mu);
    }
    if (do_subscribe) {
        moq_subscribe_cfg_t scfg;
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace.parts = c->ns;
        scfg.track_namespace.count = c->ns_count;
        scfg.track_name = c->track;
        /* The track has no Objects yet; the publisher writes only after this
         * subscription is acknowledged, so every Object of the sequence lies
         * after the Largest Object at subscribe time. */
        scfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
        scfg.has_forward = true;
        scfg.forward = true;
        moq_subscription_t sub;
        moq_result_t rc = moq_session_subscribe(sess, &scfg, now, &sub);
        pthread_mutex_lock(&c->mu);
        if (rc == MOQ_OK) { c->subscribe_sent = true; c->sub = sub; }
        else if (rc != MOQ_ERR_WOULD_BLOCK) {
            c->subscribe_sent = c->subscribe_failed = true;
            c->subscribe_rc = (int)rc;
        }
        pthread_mutex_unlock(&c->mu);
    }
    if (do_publish) orl_publish_step(c, sess, now);

    moq_event_t ev;
    while (moq_session_poll_events(sess, &ev, 1) == 1) {
        if (c->verbose)
            fprintf(stderr, "  [%s] event kind=%u\n", c->log_prefix, ev.kind);
        pthread_mutex_lock(&c->mu);
        switch (ev.kind) {
        case MOQ_EVENT_SESSION_CLOSED:
            c->session_closed = true;
            break;
        case MOQ_EVENT_NAMESPACE_ACCEPTED:
            c->ns_accepted = true;
            break;
        case MOQ_EVENT_NAMESPACE_REJECTED:
            c->ns_rejected = true;
            break;
        case MOQ_EVENT_SUBSCRIBE_REQUEST:
            /* The test track's first request is accepted on the next cycle;
             * every other one is refused after this drain. */
            interop_sub_answers_on_request(&c->answers, sess,
                                           &ev.u.subscribe_request);
            break;
        case MOQ_EVENT_SUBSCRIBE_OK:
            c->subscribe_ok = true;
            break;
        case MOQ_EVENT_SUBSCRIBE_ERROR:
            c->subscribe_error = true;
            break;
        case MOQ_EVENT_OBJECT_RECEIVED: {
            const moq_object_received_event_t *o = &ev.u.object_received;
            const moq_rcbuf_t *p = o->payload;
            orl_object(c, moq_subscription_eq(o->sub, c->sub), o->group_id,
                       o->subgroup_id, o->object_id, o->status,
                       p ? moq_rcbuf_data(p) : NULL, p ? moq_rcbuf_len(p) : 0);
            break;
        }
        case MOQ_EVENT_OBJECT_CHUNK: {
            const moq_object_chunk_event_t *k = &ev.u.object_chunk;
            if (k->begin) {
                if (c->chunk_open)
                    orl_verifier_error(&c->ver, "object chunk began before the "
                                       "previous object ended");
                c->chunk_open = true;
                c->chunk_own = moq_subscription_eq(k->sub, c->sub);
                c->chunk_group = k->group_id;
                c->chunk_subgroup = k->subgroup_id;
                c->chunk_object = k->object_id;
                c->chunk_status = k->status;
                c->chunk_len = 0;
            }
            if (k->chunk) {
                size_t n = moq_rcbuf_len(k->chunk);
                if (!c->chunk_open || c->chunk_len + n > sizeof(c->chunk_buf)) {
                    orl_verifier_error(&c->ver, "object chunk outside a bounded "
                                       "object");
                } else {
                    memcpy(c->chunk_buf + c->chunk_len, moq_rcbuf_data(k->chunk), n);
                    c->chunk_len += n;
                }
            }
            if (k->end && c->chunk_open) {
                c->chunk_open = false;
                if (k->terminal != MOQ_OBJECT_TERMINAL_NORMAL)
                    orl_verifier_error(&c->ver, "object ended by a reset or stop");
                else
                    orl_object(c, c->chunk_own, c->chunk_group, c->chunk_subgroup,
                               c->chunk_object, c->chunk_status, c->chunk_buf,
                               c->chunk_len);
            }
            break;
        }
        case MOQ_EVENT_SUBGROUP_FINISHED: {
            const moq_subgroup_finished_event_t *f = &ev.u.subgroup_finished;
            orl_verifier_finished(&c->ver, moq_subscription_eq(f->sub, c->sub),
                                  f->group_id, f->subgroup_id);
            break;
        }
        case MOQ_EVENT_SUBGROUP_RESET: {
            const moq_subgroup_reset_event_t *r = &ev.u.subgroup_reset;
            char what[128];
            snprintf(what, sizeof(what), "subgroup g=%" PRIu64 " sg=%" PRIu64
                     " reset with code 0x%" PRIx64 " after %" PRIu64 " objects",
                     r->group_id, r->subgroup_id, r->error_code,
                     r->group_id < ORL_GROUPS ? c->ver.next_object[r->group_id] : 0);
            orl_verifier_error(&c->ver, what);
            break;
        }
        case MOQ_EVENT_SUBSCRIBE_DONE:
            if (moq_subscription_eq(ev.u.subscribe_done.sub, c->sub))
                orl_verifier_done(&c->ver, ev.u.subscribe_done.status_code);
            else
                orl_verifier_error(&c->ver, "done for another subscription");
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&c->mu);
        moq_event_cleanup(&ev);
    }
    pthread_mutex_lock(&c->mu);
    interop_sub_answers_flush(&c->answers, sess, now);
    pthread_mutex_unlock(&c->mu);
    return MOQ_OK;
}

/* Pump one endpoint for up to rem_us (see ep_cycle for the lock ordering). */
static moq_result_t orl_cycle(moq_endpoint_t *ep, orl_ctx_t *c, uint64_t rem_us)
{
    moq_endpoint_post(ep, orl_pump_task, c);
    return moq_endpoint_wait(ep, rem_us);
}

typedef enum { ORL_WAIT_SETUP, ORL_WAIT_NS, ORL_WAIT_SUBSCRIBED,
               ORL_WAIT_DELIVERED } orl_wait_t;

/* Pump both endpoints until the condition holds, either side ends or the
 * deadline passes. Returns NULL on success, otherwise the reason. */
static const char *orl_wait(moq_endpoint_t *pub_ep, orl_ctx_t *pub,
                            moq_endpoint_t *sub_ep, orl_ctx_t *sub,
                            orl_wait_t what, uint64_t timeout_us)
{
    uint64_t deadline = now_us_mono() + timeout_us;
    for (;;) {
        bool met = false;
        const char *why = NULL;
        pthread_mutex_lock(&pub->mu);
        pthread_mutex_lock(&sub->mu);
        switch (what) {
        case ORL_WAIT_SETUP:
            met = sub_ep ? sub->setup_complete : pub->setup_complete;
            break;
        case ORL_WAIT_NS:
            met = pub->ns_accepted;
            if (pub->ns_rejected) why = "namespace rejected";
            if (pub->publish_ns_failed) why = "publish_namespace failed";
            break;
        case ORL_WAIT_SUBSCRIBED:
            met = interop_publisher_verdict(pub->accept_sent && sub->subscribe_ok,
                                            &pub->answers, &why) == INTEROP_VERDICT_PASS;
            if (pub->accept_failed) why = "accept_subscribe failed";
            if (sub->subscribe_error) why = "SUBSCRIBE_ERROR";
            if (sub->subscribe_failed) why = "subscribe failed";
            break;
        case ORL_WAIT_DELIVERED:
            /* Requests forwarded during delivery must be answered too. */
            met = interop_publisher_verdict(sub->ver.done, &pub->answers, &why)
                  == INTEROP_VERDICT_PASS;
            if (pub->publish_failed) why = pub->publish_step;
            break;
        }
        bool closed = pub->session_closed || (sub_ep && sub->session_closed);
        pthread_mutex_unlock(&sub->mu);
        pthread_mutex_unlock(&pub->mu);
        /* A failure is authoritative, also when the step looks met. */
        if (why) return why;
        if (met) return NULL;
        if (closed || moq_endpoint_is_closed(pub_ep) ||
            (sub_ep && moq_endpoint_is_closed(sub_ep)))
            return "session closed";
        if (moq_endpoint_is_fatal(pub_ep) || (sub_ep && moq_endpoint_is_fatal(sub_ep)))
            return "fatal";
        if (now_us_mono() >= deadline) return "timeout";
        if (orl_cycle(pub_ep, pub, 5000) == MOQ_ERR_CLOSED)
            return "publisher connection closed";
        if (sub_ep && orl_cycle(sub_ep, sub, 5000) == MOQ_ERR_CLOSED)
            return "subscriber connection closed";
    }
}

static void orl_tap_negotiated(const char *role, moq_endpoint_t *ep,
                               const char *url)
{
    printf("# negotiated %s: draft-%u over %s (backend: %s)\n", role,
           (unsigned)moq_endpoint_negotiated_version(ep), transport_label(url),
           g_backend_name);
    fflush(stdout);
}

static orl_fault_t orl_fault_from_env(void)
{
    const char *f = getenv("MOQ_INTEROP_OBJECT_RELAY_FAULT");
    if (!f || !*f) return ORL_FAULT_NONE;
    if (strcmp(f, "corrupt-byte") == 0) return ORL_FAULT_CORRUPT_BYTE;
    if (strcmp(f, "drop-object") == 0) return ORL_FAULT_DROP_OBJECT;
    if (strcmp(f, "extra-object") == 0) return ORL_FAULT_EXTRA_OBJECT;
    fprintf(stderr, "MOQ_INTEROP_OBJECT_RELAY_FAULT=%s is not a known fault\n", f);
    exit(1);
}

static int run_object_relay(const char *url, bool insecure, bool verbose)
{
    /* A namespace no other run shares: pid plus the monotonic clock. */
    char run_tag[48];
    snprintf(run_tag, sizeof(run_tag), "%ld-%" PRIu64, (long)getpid(),
             now_us_mono());
    moq_bytes_t ns[3] = {
        { (const uint8_t *)"moq-test", 8 },
        { (const uint8_t *)"object-relay", 12 },
        { (const uint8_t *)run_tag, strlen(run_tag) },
    };
    moq_bytes_t track = { (const uint8_t *)"orl-track", 9 };

    orl_ctx_t pub, sub;
    orl_ctx_init(&pub, verbose, "pub", ns, 3, track);
    orl_ctx_init(&sub, verbose, "sub", ns, 3, track);
    pub.fault = orl_fault_from_env();
    {
        const char *d = getenv("MOQ_INTEROP_OBJECT_RELAY_DONE_DELAY_MS");
        if (d && *d) pub.done_delay_us = strtoull(d, NULL, 10) * 1000u;
    }

    static const char *names[7] = {
        "publisher setup",
        "publisher namespace accepted",
        "subscriber setup",
        "SUBSCRIBE forwarded and accepted upstream",
        "subscriber got SUBSCRIBE_OK",
        "publisher wrote the sequence and PUBLISH_DONE",
        "subscriber verified every object, FIN and the done",
    };
    bool ok[7] = { false };
    uint64_t ms[7] = { 0 };
    char msg[7][256];
    for (int i = 0; i < 7; i++) snprintf(msg[i], sizeof(msg[i]), "not reached");
    int reached = 0;

    tap_plan(7);
    printf("# object-relay: opaque object conformance (payload bytes are not "
           "media)\n# namespace: moq-test/object-relay/%s track: orl-track\n",
           run_tag);
    if (pub.fault != ORL_FAULT_NONE)
        printf("# fault injected at the publisher: %s\n",
               getenv("MOQ_INTEROP_OBJECT_RELAY_FAULT"));
    if (pub.done_delay_us)
        printf("# diagnostic: PUBLISH_DONE held %" PRIu64 " ms after the last FIN\n",
               pub.done_delay_us / 1000u);
    fflush(stdout);

    moq_endpoint_t *pub_ep = make_endpoint(url, insecure);
    moq_endpoint_t *sub_ep = NULL;
    const char *why;
    uint64_t t0;

    /* 1: publisher setup */
    reached = 1;
    t0 = now_us_mono();
    why = pub_ep ? orl_wait(pub_ep, &pub, NULL, &sub, ORL_WAIT_SETUP, ORL_SETUP_US)
                 : endpoint_failure("publisher");
    ms[0] = (now_us_mono() - t0) / 1000;
    if (why) { snprintf(msg[0], sizeof(msg[0]), "%s", why); goto emit; }
    ok[0] = true;
    snprintf(msg[0], sizeof(msg[0]), "MoQ setup complete");
    orl_tap_negotiated("publisher", pub_ep, url);

    /* 2: namespace accepted */
    reached = 2;
    pthread_mutex_lock(&pub.mu);
    pub.req_publish_ns = true;
    pthread_mutex_unlock(&pub.mu);
    t0 = now_us_mono();
    why = orl_wait(pub_ep, &pub, NULL, &sub, ORL_WAIT_NS, ORL_SETUP_US);
    ms[1] = (now_us_mono() - t0) / 1000;
    if (why) { snprintf(msg[1], sizeof(msg[1]), "%s", why); goto emit; }
    ok[1] = true;
    snprintf(msg[1], sizeof(msg[1]), "NAMESPACE_ACCEPTED received");

    /* 3: subscriber setup */
    reached = 3;
    sub_ep = make_endpoint(url, insecure);
    t0 = now_us_mono();
    why = sub_ep ? orl_wait(pub_ep, &pub, sub_ep, &sub, ORL_WAIT_SETUP, ORL_SETUP_US)
                 : endpoint_failure("subscriber");
    ms[2] = (now_us_mono() - t0) / 1000;
    if (why) { snprintf(msg[2], sizeof(msg[2]), "%s", why); goto emit; }
    ok[2] = true;
    snprintf(msg[2], sizeof(msg[2]), "MoQ setup complete");
    orl_tap_negotiated("subscriber", sub_ep, url);

    /* 4 + 5: the SUBSCRIBE reaches the publisher through the relay, is
     * accepted there, and the relay acknowledges the subscriber. */
    reached = 5;
    pthread_mutex_lock(&sub.mu);
    sub.req_subscribe = true;
    pthread_mutex_unlock(&sub.mu);
    t0 = now_us_mono();
    why = orl_wait(pub_ep, &pub, sub_ep, &sub, ORL_WAIT_SUBSCRIBED, ORL_SUBSCRIBE_US);
    ms[3] = ms[4] = (now_us_mono() - t0) / 1000;
    pthread_mutex_lock(&pub.mu);
    ok[3] = interop_publisher_verdict(pub.accept_sent && !pub.accept_failed,
                                      &pub.answers, NULL) == INTEROP_VERDICT_PASS;
    pthread_mutex_unlock(&pub.mu);
    pthread_mutex_lock(&sub.mu);
    ok[4] = sub.subscribe_ok;
    pthread_mutex_unlock(&sub.mu);
    snprintf(msg[3], sizeof(msg[3]), "%s", ok[3] ? "SUBSCRIBE accepted" :
             (why ? why : "no SUBSCRIBE reached the publisher"));
    snprintf(msg[4], sizeof(msg[4]), "%s", ok[4] ? "SUBSCRIBE_OK received" :
             (why ? why : "no SUBSCRIBE_OK"));
    if (why || !ok[3] || !ok[4]) goto emit;

    /* 6 + 7: the sequence, its FINs and the done, verified end to end. */
    reached = 7;
    pthread_mutex_lock(&pub.mu);
    pub.go = true;
    pthread_mutex_unlock(&pub.mu);
    t0 = now_us_mono();
    why = orl_wait(pub_ep, &pub, sub_ep, &sub, ORL_WAIT_DELIVERED, ORL_DELIVERY_US);
    ms[5] = ms[6] = (now_us_mono() - t0) / 1000;
    pthread_mutex_lock(&pub.mu);
    const char *answer_why = NULL;
    ok[5] = interop_publisher_verdict(pub.done_sent && !pub.publish_failed,
                                      &pub.answers, &answer_why) == INTEROP_VERDICT_PASS;
    printf("# publisher wrote: %" PRIu64 " objects in %" PRIu64 " subgroup streams, "
           "%" PRIu64 " payload bytes, done %s\n", pub.objects_written,
           pub.streams_opened, pub.bytes_written, pub.done_sent ? "sent" : "not sent");
    if (ok[5])
        snprintf(msg[5], sizeof(msg[5]), "every subgroup closed with a FIN, "
                 "PUBLISH_DONE TRACK_ENDED sent");
    else if (pub.publish_failed)
        snprintf(msg[5], sizeof(msg[5]), "%s (rc %d)", pub.publish_step,
                 pub.publish_rc);
    else if (answer_why)
        snprintf(msg[5], sizeof(msg[5]), "%s", answer_why);
    else if (pub.done_sent && pub.answers.n_pending != 0)
        snprintf(msg[5], sizeof(msg[5]), "a forwarded SUBSCRIBE refusal was "
                 "still queued at the deadline");
    else
        snprintf(msg[5], sizeof(msg[5]), "%s", why ? why : "not finished");
    pthread_mutex_unlock(&pub.mu);
    pthread_mutex_lock(&sub.mu);
    printf("# subscriber received: %zu objects (%zu matching), %" PRIu64
           " payload bytes verified, done %s\n", sub.ver.objects_received,
           sub.ver.objects_ok, sub.ver.bytes_verified,
           sub.ver.done ? "observed" : "not observed");
    ok[6] = orl_verifier_pass(&sub.ver, msg[6], sizeof(msg[6]));
    pthread_mutex_unlock(&sub.mu);
    /* A wait that ended early (timeout, close) is reported even when the
     * verifier, given what did arrive, would have more to say. */
    if (why && !ok[6]) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.60s; %.180s", why, msg[6]);
        memcpy(msg[6], tmp, sizeof(tmp));
    }

emit:
    close_endpoint(sub_ep);
    close_endpoint(pub_ep);
    int exit_code = 0;
    for (int i = 0; i < 7; i++) {
        if (i < reached) {
            tap_result(i + 1, ok[i], names[i], ms[i], msg[i]);
            if (!ok[i]) exit_code = 1;
        } else {
            tap_result_skip(i + 1, names[i], "an earlier step failed");
        }
    }
    pthread_mutex_destroy(&pub.mu);
    pthread_mutex_destroy(&sub.mu);
    return exit_code;
}

/* ------------------------------------------------------------------ */
/* Known test table                                                    */
/* ------------------------------------------------------------------ */

static const char *unsupported_tests[] = {
    NULL,
};

static bool is_unsupported(const char *name)
{
    for (const char **p = unsupported_tests; *p; p++)
        if (strcmp(name, *p) == 0) return true;
    return false;
}

typedef struct {
    const char *name;
    int (*run)(const char *, bool, bool);
} test_entry_t;

static const test_entry_t supported_tests[] = {
    { "setup-only",                run_setup_only },
    { "announce-only",             run_announce_only },
    { "publish-namespace-done",    run_publish_namespace_done },
    { "subscribe-error",           run_subscribe_error },
    { "announce-subscribe",        run_announce_subscribe },
    { "subscribe-before-announce", run_subscribe_before_announce },
    { NULL, NULL },
};

/* Cases selected only by name (--test / TESTCASE), never by run-all: they
 * need a relay that forwards Objects, which the run-all interop suite does
 * not assume of its peers. */
static const test_entry_t named_only_tests[] = {
    { "object-relay",              run_object_relay },
    { NULL, NULL },
};

/* Pull the first failing sub-step out of a case's captured TAP as
 * "<sub-step>: <message>", so the one-line-per-case view still says WHY it
 * failed (a bare "case did not pass" is useless in the runner's report).
 * Returns false if the capture held no failure. */
static bool first_failure(FILE *cap, char *out, size_t out_len)
{
    char line[512], step[256];
    step[0] = '\0';
    rewind(cap);
    while (fgets(line, sizeof(line), cap)) {
        if (step[0] == '\0') {
            /* Find the first failing sub-step and remember its description. */
            if (strncmp(line, "not ok ", 7) != 0) continue;
            const char *dash = strstr(line, " - ");
            snprintf(step, sizeof(step), "%s", dash ? dash + 3 : "sub-test");
            step[strcspn(step, "\r\n")] = '\0';
            continue;
        }
        /* Its YAML block follows; the first message: is the reason. A new
         * result line first means it carried no message. */
        const char *m = strstr(line, "message: \"");
        if (m) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", m + strlen("message: \""));
            char *q = strchr(msg, '"');
            if (q) *q = '\0';
            snprintf(out, out_len, "%s: %s", step, msg);
            return true;
        }
        if (strncmp(line, "ok ", 3) == 0 || strncmp(line, "not ok ", 7) == 0)
            break;
    }
    if (step[0] == '\0') return false;
    snprintf(out, out_len, "%s", step);
    return true;
}

/* No explicit test selected (bare `make test` sets RELAY_URL but no TESTCASE):
 * run the whole suite and emit ONE TAP line per case, matching the other
 * interop clients (moq-rs, imquic, ...). Each case's own multi-step TAP is
 * captured off stdout (not discarded) so the run-all view stays one line per
 * case while still reporting the real duration and the first failing sub-step;
 * use --test/TESTCASE to run a single case in full detail. The exit code is
 * lenient: 0 as long as at least one case ran to a result (the runner grades
 * per-case from the TAP, and reserves a non-zero exit for a client that could
 * not run at all). */
static int run_all_tests(const char *url, bool insecure, bool verbose)
{
    int total = 0;
    for (const test_entry_t *e = supported_tests; e->name; e++) total++;
    tap_plan(total);

    int num = 0, passed = 0;
    for (const test_entry_t *e = supported_tests; e->name; e++) {
        num++;
        /* Distinct namespace per case so back-to-back announces don't collide
         * on a relay still holding a prior case's namespace. */
        set_case_ns(e->name);
        /* Capture the case's stdout (its own plan + sub-steps) so only the
         * per-case summary line below reaches the runner, while the detail
         * stays available for the failure message. stderr (verbose diag) is
         * left alone. Restore stdout before reporting. */
        FILE *cap = tmpfile();
        int sink = cap ? fileno(cap) : open("/dev/null", O_WRONLY);
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        if (sink >= 0) dup2(sink, STDOUT_FILENO);

        uint64_t t0 = now_us_mono();
        int rc = e->run(url, insecure, verbose);
        uint64_t ms = (now_us_mono() - t0) / 1000;

        fflush(stdout);
        if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }
        if (!cap && sink >= 0) close(sink);

        bool ok = (rc == 0);
        char why[320];
        const char *msg = NULL;
        if (!ok)
            msg = (cap && first_failure(cap, why, sizeof(why)))
                      ? why : "case did not pass";
        if (cap) fclose(cap);

        if (ok) passed++;
        tap_result(num, ok, e->name, ms, msg);
    }
    return passed > 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    cli_opts_t opts = parse_cli(argc, argv);
    g_draft_pin = opts.draft;
    g_backend = opts.backend;
    if (opts.backend_name) g_backend_name = opts.backend_name;
    g_wt_profile = opts.wt_profile;
    if (opts.wt_profile_name) g_wt_profile_name = opts.wt_profile_name;

    if (!opts.relay_url) {
        fprintf(stderr, "error: --relay is required "
                "(or set RELAY_URL)\n");
        print_usage(argv[0]);
        return 1;
    }

    /* No test named -> run the whole suite (one TAP line per case). */
    if (!opts.test_name) {
        return run_all_tests(opts.relay_url, opts.tls_disable_verify,
                             opts.verbose);
    }

    if (is_unsupported(opts.test_name)) {
        tap_skip(opts.test_name, "not yet implemented");
        return 127;
    }

    const test_entry_t *entry = NULL;
    for (const test_entry_t *e = supported_tests; e->name; e++) {
        if (strcmp(opts.test_name, e->name) == 0) {
            entry = e;
            break;
        }
    }
    for (const test_entry_t *e = named_only_tests; !entry && e->name; e++) {
        if (strcmp(opts.test_name, e->name) == 0)
            entry = e;
    }
    if (!entry) {
        tap_skip(opts.test_name, "unknown test case");
        return 127;
    }

    return entry->run(opts.relay_url, opts.tls_disable_verify, opts.verbose);
}
