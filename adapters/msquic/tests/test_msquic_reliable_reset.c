/*
 * Reliable resets over real MsQuic on localhost (managed facade), observed
 * from the receiving application.
 *
 * The publisher opens a data stream and resets it in the same pump, so the
 * stream header has barely been handed to MsQuic when the reset comes. Both
 * managed sides enable ReliableResetEnabled; once MsQuic reports
 * RELIABLE_RESET_NEGOTIATED the adapter resets with RESET_STREAM_AT keeping
 * the header, and the subscriber can still identify the stream:
 *
 *   done_count (draft-16, draft-22): subgroup opened, written, reset, then
 *     PUBLISH_DONE counting it -- the subscriber surfaces the done promptly
 *     (well inside its 30 s done-wait) only with the header;
 *   aborted_fill (draft-22): the publisher aborts a fill the moment it is
 *     requested (FETCH_HEADER then reset, §3.4.1) -- the subscriber ends that
 *     fill only with the header.
 *
 * Each scenario runs RUNS fresh connections; every run must deliver, and
 * neither side may go fatal. The publisher's reset is issued from whichever
 * thread runs its pump (a worker after a transport batch, or the lane
 * doorbell), so both of the adapter's reliable-reset paths are reachable.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <moq/msquic_managed.h>
#include <moq/rcbuf.h>

#include "msq_loopback_port.h"

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define RUNS 5

enum scenario { DONE_COUNT, ABORTED_FILL };

struct side {
    pthread_mutex_t mu;
    enum scenario sc;
    int setup;
    bool subscribed;
    bool published;
    bool delivered;          /* client: the reset stream was identified */
    bool close_sent;
    int errors;
    moq_subscription_t sub;
    bool have_sub;
};

static const moq_bytes_t ns_part = { (const uint8_t *)"msq", 3 };

static int client_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                       uint64_t now, void *ctx)
{
    struct side *sd = ctx;
    moq_session_t *s = moq_msquic_managed_session(m);
    moq_event_t ev;
    (void)lane;
    pthread_mutex_lock(&sd->mu);
    while (s != NULL && moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) sd->setup++;
        if (sd->sc == DONE_COUNT && ev.kind == MOQ_EVENT_SUBSCRIBE_DONE)
            sd->delivered = true;
        if (sd->sc == ABORTED_FILL && ev.kind == MOQ_EVENT_FILL_TERMINAL &&
            ev.u.fill_terminal.reason == MOQ_FILL_PEER_CANCELLED_OR_RESET)
            sd->delivered = true;
        moq_event_cleanup(&ev);
    }
    if (s != NULL && sd->setup > 0 && !sd->subscribed) {
        moq_subscribe_cfg_t sc;
        moq_location_filter_cfg_t loc;
        moq_fill_cfg_t fill;
        moq_subscription_t sub;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace.parts = (moq_bytes_t *)&ns_part;
        sc.track_namespace.count = 1;
        sc.track_name = (moq_bytes_t){ (const uint8_t *)"track", 5 };
        sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
        if (sd->sc == ABORTED_FILL) {
            moq_location_filter_cfg_init(&loc);
            loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
            loc.start_group = 3;
            moq_fill_cfg_init(&fill);
            fill.location = &loc;
            sc.fill = &fill;
        }
        sd->subscribed = true;
        if (moq_session_subscribe(s, &sc, now, &sub) < 0) sd->errors++;
    }
    if (s != NULL && sd->delivered && !sd->close_sent) {
        sd->close_sent = true;
        (void)moq_session_close(s, 0, NULL, now);
    }
    pthread_mutex_unlock(&sd->mu);
    return 0;
}

static int server_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                       uint64_t now, void *ctx)
{
    struct side *sd = ctx;
    moq_msquic_managed_conn_t *c = moq_msquic_lane_next_conn(lane, NULL);
    moq_session_t *s = c != NULL ? moq_msquic_managed_conn_session(c) : NULL;
    moq_event_t ev;
    (void)m;
    pthread_mutex_lock(&sd->mu);
    while (s != NULL && moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            sd->sub = ev.u.subscribe_request.sub;
            sd->have_sub = true;
        }
        if (ev.kind == MOQ_EVENT_FILL_REQUEST &&
            moq_session_abort_fill(s, ev.u.fill_request.fill, 0x7, now) < 0)
            sd->errors++;
        moq_event_cleanup(&ev);
    }
    if (s != NULL && sd->have_sub && !sd->published) {
        sd->published = true;
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        ac.has_largest = true; ac.largest_group = 5; ac.largest_object = 2;
        if (moq_session_accept_subscribe(s, sd->sub, &ac, now) < 0) sd->errors++;
        if (sd->sc == DONE_COUNT) {
            moq_subgroup_cfg_t sgc;
            moq_subgroup_handle_t sg;
            moq_rcbuf_t *b = NULL;
            moq_done_subscribe_cfg_t dc;
            moq_subgroup_cfg_init(&sgc);
            sgc.group_id = 6;
            sgc.publisher_priority = 128;
            moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"object", 6, &b);
            moq_done_subscribe_cfg_init(&dc);
            dc.status_code = 0x2;
            dc.stream_count = 1;
            if (moq_session_open_subgroup(s, sd->sub, &sgc, now, &sg) < 0 ||
                moq_session_write_object(s, sg, 0, b, now) < 0 ||
                moq_session_reset_subgroup(s, sg, 0x1, now) < 0 ||
                moq_session_done_subscribe(s, sd->sub, &dc, now) < 0)
                sd->errors++;
            moq_rcbuf_decref(b);
        }
    }
    pthread_mutex_unlock(&sd->mu);
    return 0;
}

static bool wait_delivered(moq_msquic_managed_t *m, struct side *sd, int max_ms)
{
    for (int i = 0; i < max_ms / 10; i++) {
        pthread_mutex_lock(&sd->mu);
        bool d = sd->delivered;
        pthread_mutex_unlock(&sd->mu);
        if (d) return true;
        if (moq_msquic_managed_is_fatal(m)) return false;
        (void)moq_msquic_managed_wait(m, 10 * 1000);
    }
    return false;
}

/* One fresh connection; returns 1 when the subscriber identified the reset
 * stream within two seconds. */
static int run_once(const char *cert, const char *key, moq_version_t v, enum scenario sc)
{
    struct side sv, cl;
    memset(&sv, 0, sizeof(sv));
    memset(&cl, 0, sizeof(cl));
    pthread_mutex_init(&sv.mu, NULL);
    pthread_mutex_init(&cl.mu, NULL);
    sv.sc = cl.sc = sc;

    moq_msquic_managed_t *srv = NULL, *cli = NULL;
    moq_msquic_managed_cfg_t scfg;
    moq_msquic_managed_cfg_init_sized(&scfg, sizeof(scfg));
    scfg.alloc = moq_alloc_default();
    scfg.perspective = MOQ_PERSPECTIVE_SERVER;
    scfg.host = "127.0.0.1";
    scfg.cert_path = cert;
    scfg.key_path = key;
    scfg.on_lane_pump = server_pump;
    scfg.on_lane_pump_user = &sv;
    scfg.send_request_capacity = true;
    scfg.initial_request_capacity = 16;
    scfg.version = v;
    CHECK(msqt_server_create(&scfg, &srv) == MOQ_OK);
    int got = 0;
    if (srv != NULL) {
        moq_msquic_managed_cfg_t ccfg;
        moq_msquic_managed_cfg_init_sized(&ccfg, sizeof(ccfg));
        ccfg.alloc = moq_alloc_default();
        ccfg.perspective = MOQ_PERSPECTIVE_CLIENT;
        ccfg.host = "127.0.0.1";
        ccfg.port = moq_msquic_managed_port(srv);
        ccfg.insecure_skip_verify = true;
        ccfg.on_lane_pump = client_pump;
        ccfg.on_lane_pump_user = &cl;
        ccfg.send_request_capacity = true;
        ccfg.initial_request_capacity = 16;
        ccfg.version = v;
        CHECK(moq_msquic_managed_create(&ccfg, &cli) == MOQ_OK);
        if (cli != NULL) {
            got = wait_delivered(cli, &cl, 2000) ? 1 : 0;
            CHECK(!moq_msquic_managed_is_fatal(cli));
            CHECK(!moq_msquic_managed_is_fatal(srv));
            CHECK(moq_msquic_managed_stop(cli) == MOQ_OK);
            moq_msquic_managed_destroy(cli);
        }
        CHECK(moq_msquic_managed_stop(srv) == MOQ_OK);
        moq_msquic_managed_destroy(srv);
    }
    CHECK(sv.errors == 0);
    CHECK(cl.errors == 0);
    pthread_mutex_destroy(&sv.mu);
    pthread_mutex_destroy(&cl.mu);
    return got;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    /* One library reference for the whole process (see msquic_loopback). */
    static const QUIC_API_TABLE *lib_pin;
    if (QUIC_FAILED(MsQuicOpen2(&lib_pin))) {
        fprintf(stderr, "MsQuicOpen2 failed\n");
        return 2;
    }
    static const struct {
        const char *name; moq_version_t v; enum scenario sc;
    } runs[] = {
        { "done_count_d16", MOQ_VERSION_DRAFT_16, DONE_COUNT },
        { "done_count_d22", MOQ_VERSION_DRAFT_22, DONE_COUNT },
        { "aborted_fill_d22", MOQ_VERSION_DRAFT_22, ABORTED_FILL },
    };
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        int got = 0;
        for (int r = 0; r < RUNS; r++)
            got += run_once(argv[1], argv[2], runs[i].v, runs[i].sc);
        printf("msquic %s: header delivered in %d/%d runs\n", runs[i].name, got, RUNS);
        CHECK(got == RUNS);
    }
    if (failures == 0)
        printf("PASS: msquic_reliable_reset\n");
    return failures != 0;
}
