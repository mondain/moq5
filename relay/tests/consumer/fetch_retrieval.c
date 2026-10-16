/* An installed-SDK consumer of the relay runtime: a draft-22 standalone FETCH
 * whose range the relay holds only in part is completed from the origin
 * through the installed libraries alone. The runtime admits and positions the
 * request through relay-core, so this links and runs only if every call the
 * runtime makes into relay-core is exported by the installed relay-core. */
#include <moq/relay/moqr_bind.h>
#include <moq/sim.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "fetch_retrieval:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

typedef struct peer {
    moq_simpair_t *sp;
    moq_session_t *app;
} peer_t;

typedef struct world {
    moqr_core_t *core;
    moqr_bind_t *bind;
    peer_t origin, sub;
    uint64_t now;
    /* origin */
    moq_publication_t pub;
    bool published;
    moq_fetch_t upstream;
    unsigned upstream_requests;
    uint64_t up_start_group, up_start_object, up_end_group, up_end_object;
    /* subscriber */
    moq_fetch_t fetch;
    bool fetch_ok, complete, failed;
    unsigned objects;
    uint64_t object_ids[4];
} world_t;

static moq_bytes_t
bytes(const char *s)
{
    return (moq_bytes_t){ (const uint8_t *)s, strlen(s) };
}

static int
connect_peer(world_t *w, peer_t *p, uint64_t seed)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = seed;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 64;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 64;
    CHECK(moq_simpair_create(&cfg, &p->sp) == MOQ_OK);
    p->app = moq_simpair_client(p->sp);
    CHECK(moq_simpair_start(p->sp) == MOQ_OK);
    CHECK(moqr_bind_conn_open(w->bind, moq_simpair_server(p->sp),
                              MOQ_VERSION_DRAFT_22) == MOQR_OK);
    return 0;
}

static void
drain(world_t *w)
{
    moq_event_t ev;
    while (moq_session_poll_events(w->origin.app, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_PUBLISH_OK) {
            w->published = ev.u.publish_ok.send_allowed;
        } else if (ev.kind == MOQ_EVENT_FETCH_REQUEST) {
            const moq_fetch_request_event_t *q = &ev.u.fetch_request;
            w->upstream = q->fetch;
            w->upstream_requests++;
            w->up_start_group = q->start_group;
            w->up_start_object = q->start_object;
            w->up_end_group = q->end_group;
            w->up_end_object = q->end_object;
        }
        moq_event_cleanup(&ev);
    }
    while (moq_session_poll_events(w->sub.app, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_FETCH_OK &&
            ev.u.fetch_ok.fetch._opaque == w->fetch._opaque) {
            w->fetch_ok = true;
        } else if (ev.kind == MOQ_EVENT_FETCH_OBJECT &&
                   ev.u.fetch_object.fetch._opaque == w->fetch._opaque) {
            if (w->objects < 4) {
                w->object_ids[w->objects] = ev.u.fetch_object.object_id;
            }
            w->objects++;
        } else if (ev.kind == MOQ_EVENT_FETCH_COMPLETE &&
                   ev.u.fetch_complete.fetch._opaque == w->fetch._opaque) {
            w->complete = true;
        } else if (ev.kind == MOQ_EVENT_FETCH_ERROR ||
                   ev.kind == MOQ_EVENT_FETCH_RESET ||
                   ev.kind == MOQ_EVENT_FETCH_GAP ||
                   ev.kind == MOQ_EVENT_SESSION_CLOSED) {
            w->failed = true;
        }
        moq_event_cleanup(&ev);
    }
}

static void
pump(world_t *w, unsigned cycles)
{
    for (unsigned i = 0; i < cycles; i++) {
        w->now += 1000;
        peer_t *peers[2] = { &w->origin, &w->sub };
        for (unsigned j = 0; j < 2; j++) {
            if (peers[j]->sp == NULL) continue;
            size_t steps = 0;
            (void)moq_simpair_advance_to(peers[j]->sp, w->now);
            (void)moq_simpair_run_until_quiescent(peers[j]->sp, 64, &steps);
        }
        (void)moqr_bind_pump(w->bind, w->now);
        drain(w);
    }
}

static int
send_object(world_t *w, uint64_t object)
{
    const uint8_t body[3] = { 0x5d, 0x4b, (uint8_t)object };
    moq_rcbuf_t *payload = NULL;
    CHECK(moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &payload) ==
          MOQ_OK);
    moq_result_t rc = moq_session_send_pub_object_datagram(
        w->origin.app, w->pub, 0, object, 1, false, payload, NULL, 0, w->now);
    moq_rcbuf_decref(payload);
    CHECK(rc == MOQ_OK);
    return 0;
}

int
main(void)
{
    world_t w;
    memset(&w, 0, sizeof(w));
    w.now = 1;
    moqr_core_relay_cfg_t ccfg;
    moqr_core_relay_cfg_init_sized(&ccfg, sizeof(ccfg), moq_alloc_default());
    CHECK(moqr_core_create(&ccfg, &w.core) == MOQR_OK);
    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), moq_alloc_default());
    bcfg.core = w.core;
    bcfg.max_conns = 4;
    CHECK(moqr_bind_create(&bcfg, &w.bind) == MOQR_OK);
    CHECK(connect_peer(&w, &w.origin, 0x5d0001) == 0);
    CHECK(connect_peer(&w, &w.sub, 0x5d0002) == 0);
    pump(&w, 8);

    moq_bytes_t ns[1] = { bytes("sdk") };
    moq_publish_cfg_t pcfg;
    moq_publish_cfg_init_sized(&pcfg, sizeof(pcfg));
    pcfg.track_namespace = (moq_namespace_t){ .parts = ns, .count = 1 };
    pcfg.track_name = bytes("v");
    CHECK(moq_session_publish(w.origin.app, &pcfg, w.now, &w.pub) == MOQ_OK);
    pump(&w, 8);
    CHECK(w.published);
    /* The relay holds {0,0} and {0,2}; {0,1} exists only at the origin. */
    CHECK(send_object(&w, 0) == 0);
    CHECK(send_object(&w, 2) == 0);
    pump(&w, 8);

    moq_fetch_cfg_t fcfg;
    moq_fetch_cfg_init_sized(&fcfg, sizeof(fcfg));
    fcfg.track_namespace = (moq_namespace_t){ .parts = ns, .count = 1 };
    fcfg.track_name = bytes("v");
    fcfg.start_group = 0;
    fcfg.start_object = 0;
    fcfg.end_group = 0;
    fcfg.end_object = 3;
    fcfg.group_order = MOQ_GROUP_ORDER_ASCENDING;
    fcfg.has_fill_timeout = true;
    fcfg.fill_timeout_ms = 1000;
    CHECK(moq_session_fetch(w.sub.app, &fcfg, w.now, &w.fetch) == MOQ_OK);
    pump(&w, 12);
    /* The relay asked the origin for exactly the run it does not hold. */
    CHECK(w.upstream_requests == 1);
    CHECK(w.up_start_group == 0 && w.up_start_object == 1 &&
          w.up_end_group == 0 && w.up_end_object == 2);
    CHECK(w.fetch_ok && !w.complete && !w.failed);

    moq_accept_fetch_cfg_t acfg;
    moq_accept_fetch_cfg_init(&acfg);
    acfg.end_group = w.up_end_group;
    acfg.end_object = w.up_end_object;
    CHECK(moq_session_accept_fetch(w.origin.app, w.upstream, &acfg, w.now) == MOQ_OK);
    const uint8_t body[3] = { 0x5d, 0x4b, 1 };
    moq_rcbuf_t *payload = NULL;
    CHECK(moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &payload) ==
          MOQ_OK);
    moq_fetch_object_cfg_t ocfg;
    moq_fetch_object_cfg_init(&ocfg);
    ocfg.group_id = 0;
    ocfg.object_id = 1;
    ocfg.payload = payload;
    moq_result_t wrc = moq_session_write_fetch_object(w.origin.app, w.upstream,
                                                      &ocfg, w.now);
    moq_rcbuf_decref(payload);
    CHECK(wrc == MOQ_OK);
    CHECK(moq_session_end_fetch(w.origin.app, w.upstream, w.now) == MOQ_OK);
    pump(&w, 12);

    CHECK(w.complete && !w.failed && w.objects == 3);
    CHECK(w.object_ids[0] == 0 && w.object_ids[1] == 1 && w.object_ids[2] == 2);
    moqr_bind_stats_t stats;
    CHECK(moqr_bind_get_stats_sized(w.bind, &stats, sizeof(stats)) == MOQR_OK);
    CHECK(stats.session_errors == 0);

    moqr_bind_destroy(w.bind);
    moqr_core_destroy(w.core);
    moq_simpair_destroy(w.origin.sp);
    moq_simpair_destroy(w.sub.sp);
    printf("PASS: installed runtime standalone FETCH retrieval\n");
    return 0;
}
