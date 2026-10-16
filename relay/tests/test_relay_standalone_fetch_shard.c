/* Draft-22 standalone FETCH on a multi-shard relay: a FETCH landing on a
 * shard that holds no copy of the track is answered by the shard that owns
 * it, from its cache and upstream, with request-local cancellation across the
 * shards. The rig is test_relay_shard_fill's. */
#define main relay_shard_fill_main
#include "test_relay_shard_fill.c"
#undef main
#include "standalone_fetch_observation.h"

static void
rfs_shard_step(sfrig_t *r, sf_retained_origin_t *origin, sfconn_t *prime,
               sfps_t *prime_obs, sfconn_t *consumer, rfs_observation_t *obs)
{
    sfrig_cycle(r, 1000);
    sf_retained_drain(r, origin);
    sfps_drain(r, prime, prime_obs);
    moq_event_t ev;
    while (moq_session_poll_events(consumer->peer, &ev, 1) == 1) {
        if (!rfs_observe(obs, &ev, r->now) && ev.kind != MOQ_EVENT_SETUP_COMPLETE &&
            ev.kind != MOQ_EVENT_REQUEST_READY) {
            printf("FIXTURE: standalone_shard unexpected event=%d\n", (int)ev.kind);
            r->failures++;
        }
        moq_event_cleanup(&ev);
    }
}

static int
rfs_shard_case(const char *row)
{
    bool cached = strcmp(row, "cross-cache") == 0;
    bool cancel = strcmp(row, "cross-cancel") == 0;
    ca_t a;
    ca_init(&a);
    sfrig_t r = {0};
    sf_retained_origin_t origin = {0};
    sfconn_t *prime = NULL, *consumer = NULL;
    sfps_t prime_obs = {0};
    rfs_observation_t obs = {0};
    const char *missing = NULL;
    moq_fetch_t fetch = {0};
#define RSH_FIX(e) do { if (!(e)) { \
    printf("FIXTURE: %s:%d: %s\n", row, __LINE__, #e); r.failures++; goto cleanup; \
} } while (0)
#define RSH_NEED(e, label) do { if (!(e)) { missing = label; goto cleanup; } } while (0)
    RSH_FIX(sfrig_create_ex(&r, &a, true, 1, 2) == MOQR_OK);
    sf_retained_announce(&r, &origin, true, 0xa1);
    RSH_FIX(r.failures == 0 && origin.cn != NULL);
    prime = sfrig_connect(&r, 0);
    consumer = sfrig_connect(&r, 1);
    RSH_FIX(prime != NULL && consumer != NULL);
    sfrig_pump(&r, 4);
    moq_subscription_t prime_sub = sf_retained_subscribe(&r, prime, false);
    RSH_FIX(prime_sub._opaque != 0);
    for (unsigned i = 0; i < 16; i++)
        rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
    RSH_FIX(r.failures == 0 && origin.subscribes == 1 && prime_obs.subscribe_ok);
    static const uint64_t islands[] = {0, 2}, all[] = {0, 1, 2};
    sf_pub_ids(&r, origin.cn, origin.sub, 0, 0, cached ? all : islands, cached ? 3 : 2);
    for (unsigned i = 0; i < 12; i++)
        rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
    if (!cached) sf_retained_keep(&r, &origin, 0xa1);
    moqr_core_stats_t requester = {0}, owner = {0};
    moqr_core_get_stats(moqr_shards_core(r.s, 1), &requester);
    moqr_core_get_stats(moqr_shards_core(r.s, 0), &owner);
    RSH_FIX(r.failures == 0 && requester.tracks == 0 && requester.subs == 0 &&
        requester.ingested_total == 0 && owner.ingested_total == (cached ? 3u : 2u) &&
        prime_obs.objects == (cached ? 3 : 2) && origin.requests == 0);
    moq_bytes_t ns[] = { B("live"), B("cam") };
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){ .parts = ns, .count = 2 };
    cfg.track_name = B("v"); cfg.end_group = 0; cfg.end_object = 3;
    cfg.group_order = MOQ_GROUP_ORDER_ASCENDING;
    cfg.has_fill_timeout = true; cfg.fill_timeout_ms = 100;
    uint64_t issued = r.now;
    RSH_FIX(moq_session_fetch(consumer->peer, &cfg, r.now, &fetch) == MOQ_OK);
    for (unsigned i = 0; i < 24; i++)
        rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
    RSH_FIX(r.failures == 0 && obs.invalid == 0);
    if (!cached) {
        RSH_NEED(origin.requests == 1, "owner-upstream-request");
        RSH_NEED(rfs_count(&obs, fetch, MOQ_EVENT_FETCH_COMPLETE) == 0 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_ERROR) == 0 && r.now < issued + 100000,
            "no-local-cache-refusal");
        sf_retained_accept(&r, &origin);
        for (unsigned i = 0; i < 4; i++)
            rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
        RSH_FIX(r.failures == 0);
        if (cancel) {
            origin.cn->transport_paused = true;
            RSH_FIX(moq_session_fetch_cancel(consumer->peer, fetch, r.now) == MOQ_OK);
            for (unsigned i = 0; i < 8; i++)
                rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
            sf_retained_write(&r, &origin, false);
            origin.cn->transport_paused = false;
        } else sf_retained_write(&r, &origin, true);
        RSH_FIX(r.failures == 0);
        for (unsigned i = 0; i < 20; i++)
            rfs_shard_step(&r, &origin, prime, &prime_obs, consumer, &obs);
    }
    RSH_NEED(obs.closed == 0 && moq_session_state(consumer->peer) == MOQ_SESS_ESTABLISHED &&
        moq_session_state(consumer->rsess) == MOQ_SESS_ESTABLISHED, "request-local-lifetime");
    if (cancel) {
        RSH_NEED(origin.cancelled == 1 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_COMPLETE) == 0 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_OBJECT) <= 1, "remote-cancel-no-late-tail");
    } else {
        RSH_NEED(rfs_count(&obs, fetch, MOQ_EVENT_FETCH_COMPLETE) == 1 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_ERROR) == 0 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_RESET) == 0 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_GAP) == 0 &&
            rfs_count(&obs, fetch, MOQ_EVENT_FETCH_OBJECT) == 3, "remote-exact-coverage");
        for (unsigned i = 0; i < 3; i++) {
            const rfs_event_t *v = rfs_nth(&obs, fetch, MOQ_EVENT_FETCH_OBJECT, i);
            uint8_t tag = !cached && i == 1 ? 0xa1 : 0xc3;
            uint8_t body[] = { 0, (uint8_t)i, tag, (uint8_t)(tag ^ 0xffu) };
            RSH_NEED(v != NULL && v->group == 0 && v->object == i &&
                v->bytes == sizeof(body) && memcmp(v->body, body, sizeof(body)) == 0,
                "remote-ordered-bytes");
        }
    }
    moqr_core_get_stats(moqr_shards_core(r.s, 1), &requester);
    moqr_core_get_stats(moqr_shards_core(r.s, 0), &owner);
    RSH_NEED(requester.subs == 0 && requester.ingested_total == 0 &&
        owner.ingested_total == (cached ? 3u : 2u), "no-synthetic-subscription-or-live-rescue");
    sf_expect_channels_idle(&r);
cleanup:
    printf("OBS: %s origin_requests=%u cancels=%u downstream_events=%u closed=%u\n",
        row, origin.requests, origin.cancelled, obs.n, obs.closed);
    moq_rcbuf_decref(origin.payload);
    if (r.s != NULL) sfrig_destroy(&r);
    int errors = r.failures + obs.invalid + (a.live != 0);
    printf("%s: standalone_shard(%s) causal=%s fixture_failures=%d\n",
        errors ? "FIXTURE" : missing != NULL ? "FAIL" : "PASS", row,
        missing != NULL ? missing : "none", errors);
#undef RSH_FIX
#undef RSH_NEED
    return errors ? 2 : missing != NULL ? 1 : 0;
}

/* The shared setup of the rows below: an origin on shard 0 whose namespace
 * is announced, a prime subscriber on shard 0 whose subscription makes the
 * relay subscribe upstream, and a FETCH subscriber on shard 1, which holds
 * no copy of the track. */
typedef struct rfs_shard_rig {
    ca_t a;
    sfrig_t r;
    sf_retained_origin_t origin;
    sfconn_t *prime, *consumer;
    sfps_t prime_obs;
    rfs_observation_t obs;
} rfs_shard_rig_t;

static void
rfs_shard_steps(rfs_shard_rig_t *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        rfs_shard_step(&x->r, &x->origin, x->prime, &x->prime_obs, x->consumer, &x->obs);
}

static bool
rfs_shard_setup(rfs_shard_rig_t *x, uint32_t max_fetches)
{
    memset(x, 0, sizeof(*x));
    ca_init(&x->a);
    if (sfrig_create_ex(&x->r, &x->a, true, 1, max_fetches) != MOQR_OK) return false;
    sf_retained_announce(&x->r, &x->origin, true, 0xa1);
    if (x->r.failures != 0 || x->origin.cn == NULL) return false;
    x->prime = sfrig_connect(&x->r, 0);
    x->consumer = sfrig_connect(&x->r, 1);
    if (x->prime == NULL || x->consumer == NULL) return false;
    sfrig_pump(&x->r, 4);
    if (sf_retained_subscribe(&x->r, x->prime, false)._opaque == 0) return false;
    rfs_shard_steps(x, 16);
    return x->r.failures == 0 && x->origin.subscribes == 1 && x->prime_obs.subscribe_ok;
}

static bool
rfs_shard_fetch(rfs_shard_rig_t *x, sfconn_t *cn, const char *name, uint64_t sg,
                uint64_t so, uint64_t eg, uint64_t eo, bool descending,
                uint64_t timeout_ms, moq_fetch_t *out)
{
    moq_bytes_t ns[] = { B("live"), B("cam") };
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){ .parts = ns, .count = 2 };
    cfg.track_name = B(name);
    cfg.start_group = sg; cfg.start_object = so; cfg.end_group = eg; cfg.end_object = eo;
    cfg.group_order = descending ? MOQ_GROUP_ORDER_DESCENDING : MOQ_GROUP_ORDER_ASCENDING;
    cfg.has_fill_timeout = true; cfg.fill_timeout_ms = timeout_ms;
    return moq_session_fetch(cn->peer, &cfg, x->r.now, out) == MOQ_OK;
}

static int
rfs_shard_finish(rfs_shard_rig_t *x, const char *row, const char *missing)
{
    printf("OBS: %s origin_requests=%u cancels=%u downstream_events=%u closed=%u\n",
        row, x->origin.requests, x->origin.cancelled, x->obs.n, x->obs.closed);
    moq_rcbuf_decref(x->origin.payload);
    if (x->r.s != NULL) sfrig_destroy(&x->r);
    int errors = x->r.failures + x->obs.invalid + (x->a.live != 0);
    printf("%s: standalone_shard(%s) causal=%s fixture_failures=%d\n",
        errors ? "FIXTURE" : missing != NULL ? "FAIL" : "PASS", row,
        missing != NULL ? missing : "none", errors);
    return errors ? 2 : missing != NULL ? 1 : 0;
}

#define RSX_FIX(e) do { if (!(e)) { \
    printf("FIXTURE: %s:%d: %s\n", row, __LINE__, #e); x.r.failures++; goto cleanup; \
} } while (0)
#define RSX_NEED(e, label) do { if (!(e)) { missing = label; goto cleanup; } } while (0)

static bool
rfs_shard_body(const rfs_event_t *e, uint8_t g, uint8_t o)
{
    uint8_t body[4] = { g, o, 0xC3, 0x3C };
    return e != NULL && e->group == g && e->object == o &&
        e->bytes == sizeof(body) && memcmp(e->body, body, sizeof(body)) == 0;
}

/* A routed FETCH for a track the owning shard does not have: the owner's
 * admission refuses it with DOES_NOT_EXIST, exactly as a FETCH arriving there
 * would be, and nothing is asked upstream. */
static int
rfs_shard_absent(void)
{
    const char *row = "cross-absent";
    const char *missing = NULL;
    rfs_shard_rig_t x;
    moq_fetch_t fetch = {0};
    RSX_FIX(rfs_shard_setup(&x, 2));
    RSX_FIX(rfs_shard_fetch(&x, x.consumer, "w", 0, 0, 0, 1, false, 100, &fetch));
    rfs_shard_steps(&x, 24);
    RSX_FIX(x.r.failures == 0 && x.obs.invalid == 0);
    const rfs_event_t *err = rfs_nth(&x.obs, fetch, MOQ_EVENT_FETCH_ERROR, 0);
    RSX_NEED(err != NULL && err->code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST &&
        rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_OK) == 0 && x.origin.requests == 0,
        "owner-admission-refusal");
    RSX_NEED(x.obs.closed == 0 && moq_session_state(x.consumer->peer) ==
        MOQ_SESS_ESTABLISHED, "request-local-lifetime");
    sf_expect_channels_idle(&x.r);
cleanup:
    return rfs_shard_finish(&x, row, missing);
}

/* Descending group order answered by the owning shard: groups from the end
 * group down, each in Object order, through the shard boundary. A FILL_TIMEOUT
 * of 0 asks nothing upstream, so the unseen tails of groups 1 and 0 are marked
 * Timed-Out where they fall in that order. */
static int
rfs_shard_descending(void)
{
    const char *row = "cross-descending";
    const char *missing = NULL;
    rfs_shard_rig_t x;
    moq_fetch_t fetch = {0};
    RSX_FIX(rfs_shard_setup(&x, 2));
    static const uint64_t two[] = { 0, 1 }, one[] = { 0 };
    sf_pub_ids(&x.r, x.origin.cn, x.origin.sub, 0, 0, two, 2);
    sf_pub_ids(&x.r, x.origin.cn, x.origin.sub, 1, 0, two, 2);
    sf_pub_ids(&x.r, x.origin.cn, x.origin.sub, 2, 0, one, 1);
    rfs_shard_steps(&x, 12);
    RSX_FIX(x.r.failures == 0 && x.prime_obs.objects == 5);
    RSX_FIX(rfs_shard_fetch(&x, x.consumer, "v", 0, 0, 2, 1, true, 0, &fetch));
    rfs_shard_steps(&x, 24);
    RSX_FIX(x.r.failures == 0 && x.obs.invalid == 0);
    RSX_NEED(rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_COMPLETE) == 1 &&
        rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_ERROR) == 0 &&
        rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_RESET) == 0 && x.origin.requests == 0,
        "one-complete");
    static const uint8_t want[][2] = { { 2, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 0, 1 } };
    RSX_NEED(rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_OBJECT) == 5, "exact-objects");
    for (unsigned i = 0; i < 5; i++) {
        RSX_NEED(rfs_shard_body(rfs_nth(&x.obs, fetch, MOQ_EVENT_FETCH_OBJECT, i),
            want[i][0], want[i][1]), "descending-group-order");
    }
    RSX_NEED(rfs_count(&x.obs, fetch, MOQ_EVENT_FETCH_GAP) == 2, "two-tails");
    /* Each tail marker follows its group's Objects and precedes the next. */
    unsigned seen = 0;
    for (unsigned i = 0; i < x.obs.n; i++) {
        const rfs_event_t *e = &x.obs.events[i];
        if (e->fetch != fetch._opaque) continue;
        if (e->kind == MOQ_EVENT_FETCH_GAP) {
            RSX_NEED(e->code == MOQ_FETCH_RANGE_TIMED_OUT && e->object == UINT64_MAX &&
                e->group == (seen == 3 ? 1u : 0u) && (seen == 3 || seen == 5),
                "tail-marker-position");
        } else if (e->kind == MOQ_EVENT_FETCH_OBJECT) {
            seen++;
        }
    }
    RSX_NEED(x.obs.closed == 0, "request-local-lifetime");
    sf_expect_channels_idle(&x.r);
cleanup:
    return rfs_shard_finish(&x, row, missing);
}

/* The owning shard's FETCH pool bounds routed requests too. With one FETCH
 * slot per shard, a FETCH on shard 0 waiting on upstream holds the owner's
 * slot, so a routed FETCH from shard 1 is refused (INTERNAL_ERROR) at once,
 * without an upstream request and without touching the subscriber's session;
 * once the first FETCH completes the slot is free again and a routed FETCH
 * is served. */
static int
rfs_shard_capacity(void)
{
    const char *row = "cross-capacity";
    const char *missing = NULL;
    rfs_shard_rig_t x;
    moq_fetch_t local = {0}, refused = {0}, served = {0};
    RSX_FIX(rfs_shard_setup(&x, 1));
    static const uint64_t islands[] = { 0, 2 };
    sf_pub_ids(&x.r, x.origin.cn, x.origin.sub, 0, 0, islands, 2);
    rfs_shard_steps(&x, 12);
    sf_retained_keep(&x.r, &x.origin, 0xa1);
    RSX_FIX(x.r.failures == 0 && x.prime_obs.objects == 2);
    RSX_FIX(rfs_shard_fetch(&x, x.prime, "v", 0, 0, 0, 3, false, 100, &local));
    rfs_shard_steps(&x, 16);
    RSX_FIX(x.r.failures == 0);
    RSX_NEED(x.origin.requests == 1, "local-upstream-request");
    RSX_FIX(rfs_shard_fetch(&x, x.consumer, "v", 0, 0, 0, 3, false, 100, &refused));
    rfs_shard_steps(&x, 16);
    const rfs_event_t *err = rfs_nth(&x.obs, refused, MOQ_EVENT_FETCH_ERROR, 0);
    RSX_NEED(err != NULL && err->code == MOQ_REQUEST_ERROR_INTERNAL_ERROR &&
        rfs_count(&x.obs, refused, MOQ_EVENT_FETCH_OK) == 0 && x.origin.requests == 1,
        "owner-capacity-refusal");
    sf_retained_accept(&x.r, &x.origin);
    sf_retained_write(&x.r, &x.origin, true);
    rfs_shard_steps(&x, 16);
    RSX_FIX(x.r.failures == 0);
    RSX_NEED(x.prime_obs.fetch_complete == 1 && x.prime_obs.fetch_n == 3,
        "local-fetch-complete");
    RSX_FIX(rfs_shard_fetch(&x, x.consumer, "v", 0, 0, 0, 3, false, 0, &served));
    rfs_shard_steps(&x, 24);
    RSX_NEED(rfs_count(&x.obs, served, MOQ_EVENT_FETCH_COMPLETE) == 1 &&
        rfs_count(&x.obs, served, MOQ_EVENT_FETCH_OBJECT) == 2 &&
        rfs_count(&x.obs, served, MOQ_EVENT_FETCH_GAP) == 1 && x.origin.requests == 1,
        "slot-recovered");
    RSX_NEED(x.obs.closed == 0 && moq_session_state(x.consumer->peer) ==
        MOQ_SESS_ESTABLISHED, "request-local-lifetime");
    sf_expect_channels_idle(&x.r);
cleanup:
    return rfs_shard_finish(&x, row, missing);
}

#undef RSX_FIX
#undef RSX_NEED

int
main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "cross-absent") == 0) return rfs_shard_absent();
    if (strcmp(argv[1], "cross-descending") == 0) return rfs_shard_descending();
    if (strcmp(argv[1], "cross-capacity") == 0) return rfs_shard_capacity();
    if (strcmp(argv[1], "cross-gap") != 0 && strcmp(argv[1], "cross-cache") != 0 &&
        strcmp(argv[1], "cross-cancel") != 0) return 2;
    return rfs_shard_case(argv[1]);
}
