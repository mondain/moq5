/*
 * A REAL moq_media_receiver_t behind the production relay binding, with a
 * real origin: the publisher facade on the origin connection retains the
 * MSF catalog generation {0: catalog JSON}; the receiver (driven through
 * its test pump, no network) must reach TRACK_ADDED and CATALOG_READY from
 * retained data through the relay, with no fatal or later track event (no
 * catalog mutation or refresh). Drafts 16 and 18 use Joining FETCH; draft 22
 * uses fill and must request the missing retained range from the origin.
 *
 * The scripted NOT_SUPPORTED control (test_media_receiver_fetch_catalog_rejected)
 * stays as the receiver's view of a relay that rejects joining fetches; this
 * row is the receiver's view of one that forwards them.
 */
#include <moq/media_receiver.h>
#include <moq/msf.h>
#include <moq/sim.h>
#include <moq/session.h>
#include <moq/publisher.h>
#include <moq/relay/relay.h>
#include <moq/relay/moqr_bind.h>
#include "test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static int failures = 0;

/* -- Test seam (media_receiver.c, MOQ_MEDIA_RECEIVER_TESTING) ----------- */
moq_media_receiver_t *moq_media_receiver_test_new_cfg(
    const moq_media_receiver_cfg_t *cfg);
void moq_media_receiver_test_free(moq_media_receiver_t *r);
void moq_media_receiver_test_pump(moq_media_receiver_t *r,
                                  moq_session_t *session, uint64_t now_us);
bool moq_media_receiver_test_catalog_fetch_pending(const moq_media_receiver_t *r);
bool moq_media_receiver_test_catalog_fill_pending(const moq_media_receiver_t *r);
bool moq_media_receiver_test_joins_by_fill(const moq_media_receiver_t *r);

/* Minimal MSF catalog declaring one live LOC video track. */
static const char CATALOG_JSON[] =
    "{\"version\":\"1\",\"tracks\":["
    "{\"name\":\"video\",\"packaging\":\"loc\",\"isLive\":true,"
    "\"role\":\"video\",\"codec\":\"avc1.42e01e\"}]}";

static const char CATALOG_D22_JSON[] =
    "{\"version\":1,\"generatedAt\":1,\"tracks\":["
    "{\"name\":\"video\",\"packaging\":\"loc\",\"isLive\":false,"
    "\"role\":\"video\",\"codec\":\"av01\",\"bitrate\":1500000,\"trackDuration\":2000}]}";

typedef struct origin_observation {
    moq_subscription_t catalog_sub;
    unsigned subscribes, fill_requests, fetch_requests;
} origin_observation_t;

static bool
bytes_equal(moq_bytes_t bytes, const char *text)
{
    size_t len = strlen(text);
    return bytes.len == len && bytes.data != NULL &&
           memcmp(bytes.data, text, len) == 0;
}

static bool
catalog_identity(moq_namespace_t ns, moq_bytes_t name)
{
    return ns.count == 2 && ns.parts != NULL &&
           bytes_equal(ns.parts[0], "svc") && bytes_equal(ns.parts[1], "demo") &&
           bytes_equal(name, "catalog");
}

/* Observe real decoded requests before dispatch to the unchanged facade.
 * D22 manual mode drains FIFO events even when a prior admission is blocked. */
static void
origin_pump(moq_publisher_t *pub, moq_session_t *session, uint64_t now,
            origin_observation_t *seen)
{
    moq_result_t rc = moq_pub_flush(pub, now);
    MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    moq_event_t ev;
    while (moq_session_poll_events(session, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            const moq_subscribe_request_event_t *q = &ev.u.subscribe_request;
            MOQ_TEST_CHECK(catalog_identity(q->track_namespace, q->track_name));
            MOQ_TEST_CHECK(q->options != NULL);
            MOQ_TEST_CHECK(q->options &&
                           (q->options->flags & MOQ_SUBSCRIPTION_ADMISSION_LOCATION) != 0 &&
                           q->options->location.kind == MOQ_LOCATION_FILTER_NEXT_OBJECT);
            seen->catalog_sub = q->sub;
            seen->subscribes++;
            fprintf(stderr, "UPSTREAM d22 SUBSCRIBE location=%u fill=%u\n",
                    q->options ? (unsigned)q->options->location.kind : UINT32_MAX,
                    q->options ? (unsigned)((q->options->flags &
                                            MOQ_SUBSCRIPTION_ADMISSION_FILL) != 0) : 0u);
        } else if (ev.kind == MOQ_EVENT_FILL_REQUEST) {
            const moq_fill_request_event_t *q = &ev.u.fill_request;
            MOQ_TEST_CHECK(q->sub._opaque != 0 &&
                           q->sub._opaque == seen->catalog_sub._opaque);
            MOQ_TEST_CHECK(q->start_group == 0 && q->start_object == 0 &&
                           q->end_group == 0 && q->end_object == 0);
            seen->fill_requests++;
            fprintf(stderr, "UPSTREAM d22 FILL_REQUEST range=%llu:%llu-%llu:%llu\n",
                    (unsigned long long)q->start_group, (unsigned long long)q->start_object,
                    (unsigned long long)q->end_group, (unsigned long long)q->end_object);
        } else if (ev.kind == MOQ_EVENT_FETCH_REQUEST) {
            const moq_fetch_request_event_t *q = &ev.u.fetch_request;
            MOQ_TEST_CHECK(catalog_identity(q->track_namespace, q->track_name));
            MOQ_TEST_CHECK(q->joining_sub._opaque == 0 && q->joining_pub._opaque == 0);
            MOQ_TEST_CHECK(q->start_group == 0 && q->start_object == 0 &&
                           q->end_group == 0 && q->end_object == 1);
            seen->fetch_requests++;
            fprintf(stderr, "UPSTREAM d22 FETCH_REQUEST range=%llu:%llu-%llu:%llu\n",
                    (unsigned long long)q->start_group, (unsigned long long)q->start_object,
                    (unsigned long long)q->end_group, (unsigned long long)q->end_object);
        }
        moq_pub_event_result_t result = MOQ_PUB_EVENT_ERROR;
        rc = moq_pub_handle_event(pub, &ev, now, &result);
        MOQ_TEST_CHECK(rc == MOQ_OK ||
                       (rc == MOQ_ERR_WOULD_BLOCK && result == MOQ_PUB_EVENT_CONSUMED));
        MOQ_TEST_CHECK(result != MOQ_PUB_EVENT_ERROR);
        moq_event_cleanup(&ev);
    }
    rc = moq_pub_flush(pub, now);
    MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
}

typedef struct pair {
    moq_simpair_t *sp;
    moq_session_t *client;   /* the test's endpoint (origin / receiver) */
    moq_session_t *server;   /* the relay's session                     */
} pair_t;

static bool
pair_open(pair_t *p, moqr_bind_t *bind, moq_version_t version, uint64_t seed)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = seed;
    cfg.version = version;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 1024;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 1024;
    if (moq_simpair_create(&cfg, &p->sp) != MOQ_OK) return false;
    p->client = moq_simpair_client(p->sp);
    p->server = moq_simpair_server(p->sp);
    if (moq_simpair_start(p->sp) != MOQ_OK) return false;
    return moqr_bind_conn_open(bind, p->server, version) == MOQR_OK;
}

static int
run(moq_version_t version)
{
    const bool d22 = version == MOQ_VERSION_DRAFT_22;
    const char *lbl = version == MOQ_VERSION_DRAFT_16 ? "v16" : d22 ? "v22" : "v18";
    int before = failures;
    const moq_alloc_t *alloc = moq_alloc_default();

    moqr_core_relay_cfg_t ccfg;
    moqr_core_relay_cfg_init_sized(&ccfg, sizeof(ccfg), alloc);
    ccfg.log_budget.max_groups = 8;
    ccfg.log_budget.max_bytes = 1 << 20;
    ccfg.linger_us = 500;
    moqr_core_t *core = NULL;
    MOQ_TEST_CHECK(moqr_core_create(&ccfg, &core) == MOQR_OK);
    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), alloc);
    bcfg.core = core;
    bcfg.max_conns = 4;
    moqr_bind_t *bind = NULL;
    MOQ_TEST_CHECK(moqr_bind_create(&bcfg, &bind) == MOQR_OK);

    pair_t origin, down;
    memset(&origin, 0, sizeof(origin));
    memset(&down, 0, sizeof(down));
    MOQ_TEST_CHECK(pair_open(&origin, bind, version, 0xB001u));
    MOQ_TEST_CHECK(pair_open(&down, bind, version, 0xB002u));

    uint64_t now = 1;
    moq_publisher_t *pub = NULL;
    moq_media_receiver_t *r = NULL;
    origin_observation_t seen = {0};
#define CYCLE()                                                           \
    do {                                                                  \
        now += 1000;                                                      \
        size_t steps_ = 0;                                                \
        (void)moq_simpair_advance_to(origin.sp, now);                     \
        (void)moq_simpair_run_until_quiescent(origin.sp, 64, &steps_);    \
        (void)moq_simpair_advance_to(down.sp, now);                       \
        (void)moq_simpair_run_until_quiescent(down.sp, 64, &steps_);      \
        (void)moqr_bind_pump(bind, now);                                  \
        if (pub) {                                                        \
            if (d22) origin_pump(pub, origin.client, now, &seen);           \
            else (void)moq_pub_tick(pub, now);                              \
        }                                                                 \
        if (r) moq_media_receiver_test_pump(r, down.client, now);         \
    } while (0)
    for (int i = 0; i < 8; i++) CYCLE();
    MOQ_TEST_CHECK(moq_session_state(origin.client) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(down.client) == MOQ_SESS_ESTABLISHED);

    /* Origin: namespace svc/demo, catalog track with the retained generation. */
    moq_bytes_t nsp[2] = { MOQ_BYTES_LITERAL("svc"), MOQ_BYTES_LITERAL("demo") };
    moq_namespace_t ns = { .parts = nsp, .count = 2 };
    moq_pub_cfg_t pcfg;
    moq_pub_cfg_init_sized(&pcfg, sizeof(pcfg));
    pcfg.accept_mode = MOQ_PUB_ACCEPT_ALL;
    MOQ_TEST_CHECK(moq_pub_create(origin.client, alloc, &pcfg, &pub) == MOQ_OK);
    moq_pub_track_cfg_t tcfg;
    moq_pub_track_cfg_init_sized(&tcfg, sizeof(tcfg));
    tcfg.track_namespace = ns;
    tcfg.track_name = MOQ_BYTES_LITERAL("catalog");
    tcfg.advertise_namespace = true;
    moq_pub_track_t *track = NULL;
    MOQ_TEST_CHECK(pub && moq_pub_add_track(pub, &tcfg, now, &track) == MOQ_OK);
    moq_rcbuf_t *cat = NULL;
    const char *catalog = d22 ? CATALOG_D22_JSON : CATALOG_JSON;
    MOQ_TEST_CHECK(moq_rcbuf_create(alloc, (const uint8_t *)catalog,
                                    strlen(catalog), &cat) == MOQ_OK);
    moq_pub_retained_object_t objs[1] = {
        { .object_id = 0, .payload = cat, .properties = NULL, .end_of_group = true },
    };
    moq_pub_retained_group_cfg_t rg;
    moq_pub_retained_group_cfg_init(&rg);
    rg.group_id = 0;
    rg.objects = objs;
    rg.object_count = 1;
    MOQ_TEST_CHECK(pub && track && moq_pub_set_retained_group(pub, track, &rg) == MOQ_OK);
    if (cat) moq_rcbuf_decref(cat);
    for (int i = 0; i < 8 && !(pub && track && moq_pub_namespace_accepted(pub, track)); i++) CYCLE();
    MOQ_TEST_CHECK(pub && track && moq_pub_namespace_accepted(pub, track));
    MOQ_TEST_CHECK(seen.subscribes == 0 && seen.fill_requests == 0 && seen.fetch_requests == 0);

    /* The real receiver on the downstream connection: discover only. */
    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live_sized(&rcfg, sizeof(rcfg));
    if (d22) rcfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
    MOQ_TEST_CHECK(rcfg.struct_size == sizeof(rcfg));
    /* Drafts 16/18 keep the default AUTO companion, which the session latch
     * resolves to the legacy profile. */
    MOQ_TEST_CHECK(rcfg.companion_profile ==
                   (d22 ? MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC
                        : MOQ_MEDIA_COMPANION_AUTO));
    rcfg.namespace_.parts = nsp;
    rcfg.namespace_.count = 2;
    rcfg.auto_subscribe = false;
    rcfg.time_mode = MOQ_MEDIA_TIME_RAW;
    rcfg.overflow.policy = MOQ_MEDIA_OVERFLOW_DROP_GROUP;
    rcfg.overflow.max_objects = 64;
    rcfg.overflow.max_bytes = 1u << 20;
    r = moq_media_receiver_test_new_cfg(&rcfg);
    MOQ_TEST_CHECK(r != NULL);

    bool fetch_pending_seen = false, fetch_cleared = false;
    bool fill_pending_seen = false, fill_cleared = false;
    moq_media_track_t *discovered = NULL;
    int track_added = 0, catalog_ready = 0, other_events = 0, after_ready = 0;
    int ready_cycle = -1, fetch_seen_cycle = -1, fetch_cleared_cycle = -1;
    /* Advance simulated time past the unchanged relay default fill deadline. */
    for (int cycle = 0; cycle < (d22 ? 1280 : 80) && r; cycle++) {
        CYCLE();
        bool pending = moq_media_receiver_test_catalog_fetch_pending(r);
        if (pending && !fetch_pending_seen) {
            fetch_pending_seen = true;
            fetch_seen_cycle = cycle;
        }
        if (fetch_pending_seen && !pending && !fetch_cleared) {
            fetch_cleared = true;
            fetch_cleared_cycle = cycle;
        }
        if (d22) {
            bool fill_pending = moq_media_receiver_test_catalog_fill_pending(r);
            if (fill_pending) fill_pending_seen = true;
            if (fill_pending_seen && !fill_pending) fill_cleared = true;
        }
        moq_media_track_event_t te;
        while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {
            if (catalog_ready > 0) after_ready++;
            if (te.kind == MOQ_MEDIA_TRACK_ADDED) { track_added++; discovered = te.track; }
            else if (te.kind == MOQ_MEDIA_CATALOG_READY) { catalog_ready++; ready_cycle = cycle; }
            else other_events++;
        }
        (void)moq_session_process_pending(down.client, now);
        if (moq_media_receiver_is_fatal(r)) {
            fprintf(stderr, "receiver FATAL: code=0x%llx\n",
                    (unsigned long long)moq_media_receiver_fatal_code(r));
            break;
        }
    }
    bool fatal = r ? moq_media_receiver_is_fatal(r) : true;
    fprintf(stderr,
            "RESULT %s: track_added=%d catalog_ready=%d(cycle %d) other=%d after_ready=%d "
            "fetch_pending_seen=%d(cycle %d) fetch_cleared=%d(cycle %d) fatal=%d\n",
            lbl, track_added, catalog_ready, ready_cycle, other_events, after_ready,
            fetch_pending_seen, fetch_seen_cycle, fetch_cleared, fetch_cleared_cycle, fatal);
    if (d22) {
        fprintf(stderr, "ORIGIN v22: subscribes=%u fill_requests=%u fetch_requests=%u "
                        "fill_pending_seen=%d fill_cleared=%d\n",
                seen.subscribes, seen.fill_requests, seen.fetch_requests,
                fill_pending_seen, fill_cleared);
        MOQ_TEST_CHECK(seen.subscribes > 0);
        MOQ_TEST_CHECK(seen.fill_requests > 0 || seen.fetch_requests > 0);
        MOQ_TEST_CHECK(fill_pending_seen && fill_cleared);
        MOQ_TEST_CHECK(r && moq_media_receiver_test_joins_by_fill(r));
        MOQ_TEST_CHECK(r && !moq_media_receiver_test_catalog_fill_pending(r));
        MOQ_TEST_CHECK(!fetch_pending_seen && !fetch_cleared);
        const moq_media_track_desc_t *desc = discovered ? moq_media_track_desc_get(discovered) : NULL;
        MOQ_TEST_CHECK(desc != NULL);
        if (desc) {
            MOQ_TEST_CHECK(bytes_equal(desc->name, "video"));
            MOQ_TEST_CHECK(bytes_equal(desc->codec, "av01"));
            MOQ_TEST_CHECK(bytes_equal(desc->role, "video"));
            MOQ_TEST_CHECK(!desc->is_live);
            MOQ_TEST_CHECK(desc->has_bitrate && desc->bitrate == 1500000);
            MOQ_TEST_CHECK(desc->has_track_duration && desc->track_duration_ms == 2000);
        }
    } else {
        MOQ_TEST_CHECK(fetch_pending_seen);     /* the Joining FETCH was issued */
        MOQ_TEST_CHECK(fetch_cleared);          /* and answered (terminal)      */
    }
    MOQ_TEST_CHECK(!fatal);
    MOQ_TEST_CHECK(track_added == 1);
    MOQ_TEST_CHECK(catalog_ready == 1);
    MOQ_TEST_CHECK(other_events == 0);
    MOQ_TEST_CHECK(after_ready == 0);            /* no later mutation / refresh  */
    MOQ_TEST_CHECK(r && !moq_media_receiver_test_catalog_fetch_pending(r));
    MOQ_TEST_CHECK(moq_session_state(down.client) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(moq_session_state(origin.client) == MOQ_SESS_ESTABLISHED);
    /* Legacy forwarding does not enter the live log. D22 records cache state
     * without choosing whether a future fill response is cached or forwarded. */
    {
        char dump[4096];
        size_t dn = 0;
        if (moqr_core_route_dump_text(core, dump, sizeof(dump), &dn) == MOQR_OK &&
            dn < sizeof(dump)) {
            dump[dn] = '\0';
            const char *c = strstr(dump, "\"catalog\"");
            const char *log = c ? strstr(c, "log:") : NULL;
            if (d22) fprintf(stderr, "RELAY v22:\n%s", dump);
            else MOQ_TEST_CHECK(log != NULL && strstr(log, "records=0") != NULL);
        } else {
            MOQ_TEST_CHECK(0);
        }
    }
#undef CYCLE
    if (r) moq_media_receiver_test_free(r);
    if (pub) moq_pub_destroy(pub);
    moqr_bind_destroy(bind);
    moqr_core_destroy(core);
    if (origin.sp) moq_simpair_destroy(origin.sp);
    if (down.sp) moq_simpair_destroy(down.sp);
    if (failures == before) {
        printf("PASS: media_receiver_relay_bootstrap %s\n", lbl);
    }
    return failures - before;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        (void)run(MOQ_VERSION_DRAFT_16);
        (void)run(MOQ_VERSION_DRAFT_18);
        (void)run(MOQ_VERSION_DRAFT_22);
    } else if (argc == 2 && strcmp(argv[1], "d16") == 0) {
        (void)run(MOQ_VERSION_DRAFT_16);
    } else if (argc == 2 && strcmp(argv[1], "d18") == 0) {
        (void)run(MOQ_VERSION_DRAFT_18);
    } else if (argc == 2 && strcmp(argv[1], "d22") == 0) {
        (void)run(MOQ_VERSION_DRAFT_22);
    } else {
        fprintf(stderr, "usage: %s [d16|d18|d22]\n", argv[0]);
        return 2;
    }
    if (failures == 0)
        MOQ_TEST_PASS("media_receiver_relay_bootstrap");
    return failures ? 1 : 0;
}
