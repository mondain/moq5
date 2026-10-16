/* Deterministic fill scenario runner.
 *
 * Per seed: one subscription with a fill on its SUBSCRIBE, a seeded mix of
 * publisher writes, gaps, FIN or abort, subscriber cancels and further fills
 * on REQUEST_UPDATE, under data-stream splitting, then an unsubscribe. Each
 * seed runs twice; the trace hash and every counter must match. Pinned:
 * every child ends with exactly one FILL_TERMINAL on each side, the
 * subscription survives every fill outcome, the session never closes, and
 * once the subscription is gone neither side retains a publisher child.
 * Under a profile without fills the fill request is refused up front and the
 * same flow runs without it.
 *
 * Replay: MOQ_SCENARIO_SEED_START=0x<seed> MOQ_SCENARIO_SEEDS=1 ./test_scenario_fill
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

typedef struct {
    uint64_t hash;
    size_t created, requests, objects, gaps, terminals_c, terminals_s;
} run_t;
#define MIX(h,v) do { (h) ^= (v); (h) *= 0x100000001B3ULL; } while (0)
static void trace_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    run_t *t = (run_t *)ctx;
    uint64_t h = t->hash;
    MIX(h, r->seed); MIX(h, r->step); MIX(h, r->now_us); MIX(h, (uint64_t)r->kind);
    MIX(h, (uint64_t)r->action_kind); MIX(h, (uint64_t)r->input_kind);
    MIX(h, (uint64_t)r->result); MIX(h, r->bytes.len);
    for (size_t i = 0; r->bytes.data && i < r->bytes.len; i++) MIX(h, r->bytes.data[i]);
    t->hash = h;
}

#define MAX_FILLS 4
typedef struct { bool live; moq_fill_t h; int terminals; } child_t;

/* Poll every event on one side, counting fill events against the shadow
 * table. A terminal for a child already ended is a failure. */
static int drain(moq_session_t *s, child_t *kids, size_t n, run_t *t, bool client,
                 const char *who)
{
    int failures = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        moq_fill_t h = { 0 };
        switch (ev.kind) {
        case MOQ_EVENT_FILL_CREATED: h = ev.u.fill_created.fill; t->created++; break;
        case MOQ_EVENT_FILL_REQUEST: h = ev.u.fill_request.fill; t->requests++; break;
        case MOQ_EVENT_FILL_OBJECT:  t->objects++; break;
        case MOQ_EVENT_FILL_GAP:     t->gaps++; break;
        case MOQ_EVENT_FILL_TERMINAL: h = ev.u.fill_terminal.fill;
            if (client) t->terminals_c++; else t->terminals_s++;
            break;
        case MOQ_EVENT_SESSION_CLOSED:
            fprintf(stderr, "  %s session closed unexpectedly\n", who);
            failures++;
            break;
        default: break;
        }
        if (moq_fill_is_valid(h)) {
            size_t i;
            for (i = 0; i < n; i++)
                if (kids[i].live && moq_fill_eq(kids[i].h, h)) break;
            if (i == n) {
                for (i = 0; i < n; i++) if (!kids[i].live) break;
                if (i < n) { kids[i].live = true; kids[i].h = h; kids[i].terminals = 0; }
            }
            if (i < n && ev.kind == MOQ_EVENT_FILL_TERMINAL) {
                kids[i].terminals++;
                if (kids[i].terminals != 1) {
                    fprintf(stderr, "  %s terminal delivered %d times\n", who, kids[i].terminals);
                    failures++;
                }
            }
        }
        moq_event_cleanup(&ev);
    }
    return failures;
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
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = out;
    cfg.fault_per_mille = 300;
    cfg.fault_flags = MOQ_SIM_FAULT_SPLIT_DATA | MOQ_SIM_FAULT_SPLIT_BIDI;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0 || moq_simpair_start(sp) < 0) {
        fprintf(stderr, "  pair start failed\n");
        if (sp) moq_simpair_destroy(sp);
        return 1;
    }
    moq_simpair_enable_faults(sp);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    rng_t rng = { seed ^ 0x5151 };
    child_t ckids[MAX_FILLS] = { { 0 } }, skids[MAX_FILLS] = { { 0 } };
    uint64_t now = 1;

    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init(&loc);
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_subscribe_cfg_t scfg;
    moq_subscribe_cfg_init(&scfg);
    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    scfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    scfg.fill = &fill;
    moq_subscription_t csub;
    moq_result_t rc = moq_session_subscribe(c, &scfg, now, &csub);
    bool fills = (rc == MOQ_OK);
    if (rc == MOQ_ERR_UNSUPPORTED) {
        scfg.fill = NULL;
        rc = moq_session_subscribe(c, &scfg, now, &csub);
    }
    if (rc != MOQ_OK) { fprintf(stderr, "  subscribe rc=%d\n", (int)rc); failures++; }
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    moq_event_t ev;
    moq_subscription_t ssub = { 0 };
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    if (!moq_subscription_is_valid(ssub)) { fprintf(stderr, "  no SUBSCRIBE_REQUEST\n"); failures++; }
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 3; ac.largest_object = 4;
    if (moq_session_accept_subscribe(sv, ssub, &ac, ++now) != MOQ_OK) failures++;
    moq_simpair_run_until_quiescent(sp, 64, NULL);

    for (int step = 0; step < 24; step++) {
        uint64_t x = rng_next(&rng);
        unsigned op = (unsigned)(x % 7);
        now++;
        switch (op) {
        case 0: case 1: {   /* publisher writes an object on an open child */
            for (size_t i = 0; i < MAX_FILLS; i++) {
                if (!skids[i].live || skids[i].terminals) continue;
                (void)moq_session_open_fill(sv, skids[i].h, now);
                moq_fetch_object_cfg_t oc;
                moq_fetch_object_cfg_init(&oc);
                oc.group_id = (x >> 8) % 4; oc.object_id = (x >> 16) % 5;
                oc.publisher_priority = 3;
                moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"abc", 3, &oc.payload);
                (void)moq_session_write_fill_object(sv, skids[i].h, &oc, now);
                moq_rcbuf_decref(oc.payload);
                break;
            }
            break;
        }
        case 2: {   /* publisher gap then FIN */
            for (size_t i = 0; i < MAX_FILLS; i++) {
                if (!skids[i].live || skids[i].terminals) continue;
                (void)moq_session_open_fill(sv, skids[i].h, now);
                (void)moq_session_write_fill_range(sv, skids[i].h, MOQ_FETCH_RANGE_UNKNOWN,
                                                   3, 4, now);
                (void)moq_session_end_fill(sv, skids[i].h, now);
                break;
            }
            break;
        }
        case 3: {   /* publisher aborts (opened or not) */
            for (size_t i = 0; i < MAX_FILLS; i++) {
                if (!skids[i].live || skids[i].terminals) continue;
                (void)moq_session_abort_fill(sv, skids[i].h, 0x9, now);
                break;
            }
            break;
        }
        case 4: {   /* subscriber cancels one */
            for (size_t i = 0; i < MAX_FILLS; i++) {
                if (!ckids[i].live || ckids[i].terminals) continue;
                (void)moq_session_cancel_fill(c, ckids[i].h, now);
                break;
            }
            break;
        }
        case 5: {   /* a further fill on REQUEST_UPDATE */
            if (!fills) break;
            moq_subscription_update_cfg_t up;
            moq_subscription_update_cfg_init_sized(&up, sizeof(up));
            up.fill = &fill;
            (void)moq_session_update_subscription(c, csub, &up, now);
            break;
        }
        default: break;   /* pump only */
        }
        moq_simpair_run_until_quiescent(sp, 64, NULL);
        failures += drain(c, ckids, MAX_FILLS, out, true, "client");
        failures += drain(sv, skids, MAX_FILLS, out, false, "server");
        if (sub_resolve_handle(c, csub) < 0 || sub_resolve_handle(sv, ssub) < 0) {
            fprintf(stderr, "  subscription ended during fill activity\n");
            failures++;
            break;
        }
    }

    /* Cancelling the subscription resets every open fill; afterwards no
     * publisher child remains and every child saw exactly one terminal. */
    (void)moq_session_unsubscribe(c, csub, ++now);
    for (int i = 0; i < 4; i++) {
        moq_simpair_run_until_quiescent(sp, 64, NULL);
        failures += drain(c, ckids, MAX_FILLS, out, true, "client");
        failures += drain(sv, skids, MAX_FILLS, out, false, "server");
    }
    if (sv->fills.live != 0) {
        fprintf(stderr, "  publisher retains %zu fill children\n", sv->fills.live);
        failures++;
    }
    for (size_t i = 0; i < MAX_FILLS; i++) {
        if (skids[i].live && skids[i].terminals != 1) {
            fprintf(stderr, "  server child %zu terminals=%d\n", i, skids[i].terminals);
            failures++;
        }
        if (ckids[i].live && ckids[i].terminals != 1) {
            fprintf(stderr, "  client child %zu terminals=%d\n", i, ckids[i].terminals);
            failures++;
        }
    }
    if (moq_session_state(c) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(sv) != MOQ_SESS_ESTABLISHED) {
        fprintf(stderr, "  a session is not established at the end\n");
        failures++;
    }
    if (fills && out->created == 0) { fprintf(stderr, "  no fill was created\n"); failures++; }
    if (!fills && (out->created || out->requests)) {
        fprintf(stderr, "  fill events on a profile without fills\n");
        failures++;
    }
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    if (scenario_selected_version(MOQ_VERSION_DRAFT_16) == 0) return 1;
    uint64_t start = 0, seeds = 40;
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
    if (failures) { fprintf(stderr, "FAIL: test_scenario_fill (%d seeds)\n", failures); return 1; }
    printf("PASS: test_scenario_fill (%llu seeds)\n", (unsigned long long)seeds);
    return 0;
}
