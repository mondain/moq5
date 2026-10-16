/* Deterministic concurrent-subscription scenario runner.
 *
 * Per seed: two or three subscriptions to one track, each with a seeded
 * Location filter (overlapping or disjoint), each accepted under the alias
 * of the first one or under its own; a seeded sequence of subgroup streams
 * and datagrams the publisher sends for the subscriptions whose window
 * admits the group (as a conforming publisher does: "once for each matching
 * subscription"), with a seeded unsubscribe of one of them, under split
 * data, request and control bytes; then every remaining subscription is
 * ended with its exact Stream Count. Each seed runs twice; the event trace
 * hash and every counter must match. Pinned:
 * - no subscription ever receives an object its filter does not admit;
 * - every stream object sent under an alias reaches each live subscription
 *   holding that alias whose filter admits it, exactly once per copy sent
 *   (draft-22 §3.1); datagrams likewise;
 * - no object names an unknown subscription, the session never closes;
 * - every subscription that was ended sees exactly one SUBSCRIBE_DONE
 *   (immediately when its streams are attributable, otherwise at its
 *   terminal timer), and an unsubscribed one sees none.
 * Under a profile without concurrent subscriptions the second subscribe is
 * refused before anything is sent and the same flow runs with one.
 *
 * Replay: MOQ_SCENARIO_SEED_START=0x<seed> MOQ_SCENARIO_SEEDS=1 \
 *         ./test_scenario_concurrent_subs
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "scenario_pair_version.h"
#include "../../core/src/session/session_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t s; } rng_t;
static uint64_t rng_next(rng_t *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#define MIX(h,v) do { (h) ^= (v); (h) *= 0x100000001B3ULL; } while (0)
#define MAXS 3
#define GROUPS 6
#define MAXO 3

typedef struct {
    moq_subscription_t csub, ssub;
    uint64_t alias;
    uint64_t sg, eg;          /* window: groups sg..eg (eg = UINT64_MAX open) */
    bool live;                /* subscribed and not ended by unsubscribe */
    bool ended_by_done;
    uint64_t streams;         /* subgroup streams the publisher opened for it */
    uint32_t expect[GROUPS][MAXO];
    uint32_t got[GROUPS][MAXO];
    uint32_t dones;
} sub_t;

typedef struct {
    uint64_t hash;
    size_t objects, datagrams, finished, dones, misattributed, stray;
} run_t;

static bool admits(const sub_t *x, uint64_t g)
{
    return g >= x->sg && g <= x->eg;
}

static int drain_client(moq_session_t *c, sub_t *subs, size_t n, run_t *t)
{
    int failures = 0;
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        MIX(t->hash, ev.kind);
        moq_subscription_t h = MOQ_SUBSCRIPTION_INVALID;
        uint64_t g = 0, o = 0;
        switch (ev.kind) {
        case MOQ_EVENT_OBJECT_RECEIVED:
            h = ev.u.object_received.sub;
            g = ev.u.object_received.group_id;
            o = ev.u.object_received.object_id;
            t->objects++;
            if (ev.u.object_received.datagram) t->datagrams++;
            break;
        case MOQ_EVENT_SUBGROUP_FINISHED: h = ev.u.subgroup_finished.sub; t->finished++; break;
        case MOQ_EVENT_SUBSCRIBE_DONE:    h = ev.u.subscribe_done.sub; t->dones++; break;
        case MOQ_EVENT_SESSION_CLOSED:
            fprintf(stderr, "  client session closed (code 0x%llx)\n",
                    (unsigned long long)ev.u.closed.code);
            failures++;
            break;
        default: break;
        }
        if (moq_subscription_is_valid(h)) {
            size_t i;
            for (i = 0; i < n; i++)
                if (moq_subscription_eq(subs[i].csub, h)) break;
            MIX(t->hash, (uint64_t)i);
            MIX(t->hash, g); MIX(t->hash, o);
            if (i == n) {
                if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) t->stray++;
            } else if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
                if (!admits(&subs[i], g)) t->misattributed++;
                else if (g < GROUPS && o < MAXO) subs[i].got[g][o]++;
            } else if (ev.kind == MOQ_EVENT_SUBSCRIBE_DONE) {
                subs[i].dones++;
            }
        }
        moq_event_cleanup(&ev);
    }
    return failures;
}

static void drain_server(moq_session_t *sv)
{
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static void settle(moq_simpair_t *sp, sub_t *subs, size_t n, run_t *t, int *failures)
{
    for (int i = 0; i < 4; i++) {
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        *failures += drain_client(moq_simpair_client(sp), subs, n, t);
        drain_server(moq_simpair_server(sp));
    }
}

/* Every live client subscription holding `alias` whose window admits `g`
 * is owed the copy. */
static void expect_copy(sub_t *subs, size_t n, uint64_t alias, uint64_t g,
                        uint64_t o)
{
    for (size_t i = 0; i < n; i++)
        if (subs[i].live && subs[i].alias == alias && admits(&subs[i], g))
            subs[i].expect[g][o]++;
}

static int run_seed(uint64_t seed, run_t *out)
{
    int failures = 0;
    memset(out, 0, sizeof(*out));
    out->hash = 0xCBF29CE484222325ULL;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = scenario_selected_version(MOQ_VERSION_DRAFT_16);
    cfg.alloc = moq_alloc_default();
    cfg.seed = seed;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.max_events = 64;
    cfg.fault_per_mille = 300;
    cfg.fault_flags = MOQ_SIM_FAULT_SPLIT_DATA | MOQ_SIM_FAULT_SPLIT_BIDI |
                      MOQ_SIM_FAULT_SPLIT_CONTROL;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0 || moq_simpair_start(sp) < 0) {
        fprintf(stderr, "  pair start failed\n");
        if (sp) moq_simpair_destroy(sp);
        return 1;
    }
    moq_simpair_enable_faults(sp);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    sub_t subs[MAXS];
    memset(subs, 0, sizeof(subs));
    settle(sp, subs, 0, out, &failures);
    rng_t rng = { seed ^ 0xC0C0 };
    uint64_t now = 100;
    bool concurrent = c->profile->allows_concurrent_subscriptions;
    size_t n = 2 + (size_t)(rng_next(&rng) % 2);
    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };

    size_t made = 0;
    for (size_t i = 0; i < n; i++) {
        sub_t *x = &subs[made];
        uint64_t r = rng_next(&rng);
        x->sg = r % 3;
        bool range = (r >> 8) & 1;
        x->eg = range ? x->sg + ((r >> 16) % 3) : UINT64_MAX;
        moq_subscribe_cfg_t scfg;
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ parts, 1 };
        scfg.track_name = (moq_bytes_t){ (const uint8_t *)"t", 1 };
        scfg.filter = range ? MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE
                            : MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        scfg.start_group = x->sg;
        scfg.end_group = range ? x->eg : 0;
        moq_result_t rc = moq_session_subscribe(c, &scfg, ++now, &x->csub);
        if (made > 0 && !concurrent) {
            /* One subscription per track and role on this profile. */
            if (rc != MOQ_ERR_INVAL) {
                fprintf(stderr, "  second subscribe rc=%d without concurrency\n", (int)rc);
                failures++;
            }
            break;
        }
        if (rc != MOQ_OK) { fprintf(stderr, "  subscribe rc=%d\n", (int)rc); failures++; break; }
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        moq_event_t ev;
        while (moq_session_poll_events(sv, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) x->ssub = ev.u.subscribe_request.sub;
            moq_event_cleanup(&ev);
        }
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        ac.has_largest = true;
        bool share = made > 0 && ((r >> 24) & 1);
        ac.has_track_alias = share;
        ac.track_alias = share ? subs[0].alias : 0;
        if (moq_session_accept_subscribe(sv, x->ssub, &ac, ++now) != MOQ_OK) {
            fprintf(stderr, "  accept failed (share=%d)\n", share);
            failures++;
            break;
        }
        moq_simpair_run_until_quiescent(sp, 256, NULL);
        while (moq_session_poll_events(c, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK && moq_subscription_eq(ev.u.subscribe_ok.sub, x->csub))
                x->alias = ev.u.subscribe_ok.track_alias;
            moq_event_cleanup(&ev);
        }
        x->live = true;
        MIX(out->hash, x->alias); MIX(out->hash, x->sg); MIX(out->hash, x->eg);
        made++;
    }

    /* Publisher activity. */
    size_t unsub_step = (size_t)(rng_next(&rng) % 10);
    size_t unsub_who = made > 1 ? (size_t)(rng_next(&rng) % made) : MAXS;
    for (size_t step = 0; step < 10 && failures == 0; step++) {
        if (step == unsub_step && unsub_who < made && subs[unsub_who].live) {
            if (moq_session_unsubscribe(c, subs[unsub_who].csub, ++now) != MOQ_OK) failures++;
            subs[unsub_who].live = false;
            settle(sp, subs, made, out, &failures);
        }
        uint64_t x = rng_next(&rng);
        uint64_t g = x % GROUPS;
        for (size_t i = 0; i < made; i++) {
            sub_t *p = &subs[i];
            if (!p->live || !admits(p, g)) continue;
            /* Copies sent under one alias are one Track: each carries the same
             * objects in the same delivery mode and ends its subgroup at the
             * same final Object, or the Track is malformed (draft-22 §12.1
             * conditions 2, 3 and 7). Their shape is drawn once per alias. */
            size_t k = i;
            for (size_t j = 0; j < i; j++)
                if (subs[j].alias == p->alias) { k = j; break; }
            uint64_t nobj = 1 + ((x >> (8 + 2 * k)) % 2);
            uint64_t o0 = (x >> 20) % (MAXO - 1);
            if (((x >> (16 + k)) & 3) == 0) {
                moq_rcbuf_t *pl = NULL;
                moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"dg", 2, &pl);
                if (moq_session_send_object_datagram(sv, p->ssub, g, o0, 3, false, pl,
                                                     NULL, 0, ++now) == MOQ_OK)
                    expect_copy(subs, made, p->alias, g, o0);
                moq_rcbuf_decref(pl);
                continue;
            }
            moq_subgroup_cfg_t sc;
            moq_subgroup_cfg_init(&sc);
            sc.group_id = g;
            sc.subgroup_id = step;
            moq_subgroup_handle_t sg;
            if (moq_session_open_subgroup(sv, p->ssub, &sc, ++now, &sg) != MOQ_OK) {
                fprintf(stderr, "  open_subgroup failed\n");
                failures++;
                continue;
            }
            p->streams++;
            for (uint64_t k = 0; k < nobj; k++) {
                moq_rcbuf_t *pl = NULL;
                moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"obj", 3, &pl);
                if (moq_session_write_object(sv, sg, o0 + k, pl, ++now) == MOQ_OK)
                    expect_copy(subs, made, p->alias, g, o0 + k);
                else
                    failures++;
                moq_rcbuf_decref(pl);
            }
            if (moq_session_close_subgroup(sv, sg, ++now) != MOQ_OK) failures++;
        }
        settle(sp, subs, made, out, &failures);
    }

    /* End every remaining subscription with its exact Stream Count. */
    for (size_t i = 0; i < made; i++) {
        if (!subs[i].live) continue;
        moq_done_subscribe_cfg_t dc;
        moq_done_subscribe_cfg_init(&dc);
        dc.status_code = 0x2;
        dc.stream_count = subs[i].streams;
        if (moq_session_done_subscribe(sv, subs[i].ssub, &dc, ++now) != MOQ_OK) {
            fprintf(stderr, "  done_subscribe failed\n");
            failures++;
        }
        subs[i].ended_by_done = true;
        settle(sp, subs, made, out, &failures);
    }
    /* Subscriptions whose streams were not attributable complete at their
     * terminal timer. */
    (void)moq_session_tick(c, UINT64_C(1) << 50);
    settle(sp, subs, made, out, &failures);

    for (size_t i = 0; i < made; i++) {
        for (uint64_t g = 0; g < GROUPS; g++)
            for (uint64_t o = 0; o < MAXO; o++)
                if (subs[i].got[g][o] != subs[i].expect[g][o]) {
                    fprintf(stderr, "  sub %zu (alias %llu, groups %llu..%lld) at {%llu,%llu}: got %u expected %u\n",
                            i, (unsigned long long)subs[i].alias,
                            (unsigned long long)subs[i].sg,
                            subs[i].eg == UINT64_MAX ? -1LL : (long long)subs[i].eg,
                            (unsigned long long)g, (unsigned long long)o,
                            subs[i].got[g][o], subs[i].expect[g][o]);
                    failures++;
                }
        uint32_t want = subs[i].ended_by_done ? 1 : 0;
        if (subs[i].dones != want) {
            fprintf(stderr, "  sub %zu SUBSCRIBE_DONE x%u, expected %u\n", i,
                    subs[i].dones, want);
            failures++;
        }
        MIX(out->hash, subs[i].streams);
    }
    if (out->misattributed) {
        fprintf(stderr, "  %zu objects outside the receiving filter\n", out->misattributed);
        failures++;
    }
    if (out->stray) {
        fprintf(stderr, "  %zu objects named no subscription\n", out->stray);
        failures++;
    }
    if (moq_session_state(c) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(sv) != MOQ_SESS_ESTABLISHED) {
        fprintf(stderr, "  a session is not established at the end\n");
        failures++;
    }
    if (c->recv_payload_bytes != 0) {
        fprintf(stderr, "  receive budget holds %zu bytes at the end\n", c->recv_payload_bytes);
        failures++;
    }
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    if (scenario_selected_version(MOQ_VERSION_DRAFT_16) == 0) return 1;
    uint64_t start = 0, seeds = 60;
    const char *e;
    if ((e = getenv("MOQ_SCENARIO_SEED_START"))) start = strtoull(e, NULL, 0);
    if ((e = getenv("MOQ_SCENARIO_SEEDS"))) seeds = strtoull(e, NULL, 0);
    int failures = 0;
    for (uint64_t i = 0; i < seeds; i++) {
        uint64_t seed = start + i;
        run_t a, b;
        int fa = run_seed(seed, &a);
        int fb = run_seed(seed, &b);
        if (fa || fb || memcmp(&a, &b, sizeof(a)) != 0) {
            fprintf(stderr, "FAIL seed=0x%llx (run0=%d run1=%d, %s)\n",
                    (unsigned long long)seed, fa, fb,
                    memcmp(&a, &b, sizeof(a)) ? "runs differ" : "invariant");
            failures++;
        }
    }
    if (failures) {
        fprintf(stderr, "FAIL: test_scenario_concurrent_subs (%d seeds)\n", failures);
        return 1;
    }
    printf("PASS: test_scenario_concurrent_subs (%llu seeds)\n", (unsigned long long)seeds);
    return 0;
}
