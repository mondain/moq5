/* Deterministic scenario: subscription updates answered by the application.
 *
 * Per seed, on draft-22 (or the profile MOQ_SIM_VERSION names) with control, request-stream and data bytes split
 * at seeded points and deliveries delayed in virtual time: a subscription
 * whose publisher leaves update answers to the application
 * (defer_subscription_updates), a seeded mix of subscriber updates (forward,
 * priority, replacement filter, fill), publisher data, and application
 * answers -- mostly accepts, sometimes rejects, sometimes after a delay of
 * several steps. A rejected update ends the subscription and the subscriber
 * subscribes again. Each seed runs twice; the trace hash and every counter
 * must match. Pinned: every update the subscriber sent ends exactly once,
 * acknowledged when the publisher accepted it and as an UPDATE_FAILED
 * terminal when it rejected it; nothing an owed update asks for is in
 * effect on the publisher before the answer, and the accepted forward state
 * is in effect after it; neither session closes; at the end no answer is
 * owed and no fill child is left on the publisher.
 *
 * Replay: MOQ_SCENARIO_SEED_START=0x<seed> MOQ_SCENARIO_SEEDS=1 \
 *         ./test_scenario_update_answer
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
    size_t sent, owed_events, accepted, rejected, acked, failed_done, other_done,
           subscriptions, objects, delayed, splits;
} run_t;
#define MIX(h,v) do { (h) ^= (v); (h) *= 0x100000001B3ULL; } while (0)
static void trace_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    run_t *t = (run_t *)ctx;
    if (r->kind == MOQ_SIM_TRACE_DELAY_ENQUEUE) t->delayed++;
    if (r->kind == MOQ_SIM_TRACE_FAULT_INJECT) t->splits++;
    uint64_t h = t->hash;
    MIX(h, r->seed); MIX(h, r->step); MIX(h, r->now_us); MIX(h, (uint64_t)r->kind);
    MIX(h, (uint64_t)r->action_kind); MIX(h, (uint64_t)r->input_kind);
    MIX(h, (uint64_t)r->result); MIX(h, r->bytes.len);
    for (size_t i = 0; r->bytes.data && i < r->bytes.len; i++) MIX(h, r->bytes.data[i]);
    t->hash = h;
}

/* Deliver everything, advancing virtual time through every delayed
 * delivery. */
static void settle(moq_simpair_t *sp)
{
    for (int i = 0; i < 64; i++) {
        moq_simpair_run_until_quiescent(sp, 64, NULL);
        uint64_t dl = moq_simpair_next_deadline_us(sp);
        if (dl == UINT64_MAX || dl <= moq_simpair_now_us(sp)) break;
        moq_simpair_advance_to(sp, dl);
    }
    moq_simpair_run_until_quiescent(sp, 64, NULL);
}

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    moq_subscription_t csub, ssub;
    bool live;                /* a subscription exists on both sides */
    bool client_pending;      /* the subscriber's update awaits its outcome */
    bool pending_forward_set, pending_forward;
    bool owed;                /* the publisher application owes an answer */
    uint64_t owed_id;
    int owed_age;
    bool forward;             /* the publisher's forward state, by the model */
    bool rejected;            /* the owed update was rejected; terminal due */
    uint64_t sg_next;
} model_t;

static int subscribe(model_t *m, run_t *t, uint64_t now)
{
    int failures = 0;
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    if (moq_session_subscribe(m->c, &cfg, now, &m->csub) != MOQ_OK) {
        fprintf(stderr, "  subscribe refused\n");
        return 1;
    }
    settle(m->sp);
    moq_event_t ev;
    moq_subscription_t ssub = { 0 };
    while (moq_session_poll_events(m->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    if (!moq_subscription_is_valid(ssub)) { fprintf(stderr, "  no SUBSCRIBE_REQUEST\n"); return 1; }
    m->ssub = ssub;
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 3; ac.largest_object = 4;
    if (moq_session_accept_subscribe(m->sv, ssub, &ac, moq_simpair_now_us(m->sp) + 1) != MOQ_OK)
        failures++;
    settle(m->sp);
    m->live = true;
    m->client_pending = false;
    m->owed = false;
    m->forward = true;
    m->rejected = false;
    m->sg_next = 0;
    t->subscriptions++;
    return failures;
}

/* Drain both sides, checking every update outcome against the model. */
static int drain(model_t *m, run_t *t)
{
    int failures = 0;
    moq_event_t ev;
    while (moq_session_poll_events(m->sv, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_SUBSCRIBE_UPDATED: {
            const moq_subscribe_updated_event_t *u = &ev.u.subscribe_updated;
            if (!u->answer_owed || u->update_count != 1 || m->owed) {
                fprintf(stderr, "  unexpected update event (owed=%d count=%u)\n",
                        (int)u->answer_owed, (unsigned)u->update_count);
                failures++;
            }
            m->owed = true;
            m->owed_id = u->update_request_id;
            m->owed_age = 0;
            t->owed_events++;
            break;
        }
        case MOQ_EVENT_SESSION_CLOSED:
            fprintf(stderr, "  publisher session closed\n");
            failures++;
            break;
        default: break;
        }
        moq_event_cleanup(&ev);
    }
    while (moq_session_poll_events(m->c, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_SUBSCRIPTION_UPDATE_OK:
            if (!m->client_pending || m->rejected) {
                fprintf(stderr, "  acknowledgement without an accepted update\n");
                failures++;
            }
            m->client_pending = false;
            t->acked++;
            break;
        case MOQ_EVENT_SUBSCRIBE_DONE:
            if (ev.u.subscribe_done.status_code == 0x8) {
                if (!m->rejected || !m->client_pending) {
                    fprintf(stderr, "  UPDATE_FAILED without a rejected update\n");
                    failures++;
                }
                t->failed_done++;
            } else {
                t->other_done++;
            }
            m->client_pending = false;
            m->live = false;
            break;
        case MOQ_EVENT_OBJECT_RECEIVED:
        case MOQ_EVENT_OBJECT_CHUNK:
            t->objects++;
            break;
        case MOQ_EVENT_SESSION_CLOSED:
            fprintf(stderr, "  subscriber session closed\n");
            failures++;
            break;
        default: break;
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
    cfg.alloc = moq_alloc_default();
    cfg.seed = seed;
    cfg.version = scenario_selected_version(MOQ_VERSION_DRAFT_22);
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 256;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 256;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = out;
    cfg.fault_per_mille = 300;
    cfg.fault_flags = MOQ_SIM_FAULT_SPLIT_DATA | MOQ_SIM_FAULT_SPLIT_BIDI |
                      MOQ_SIM_FAULT_SPLIT_CONTROL | MOQ_SIM_FAULT_DELAY;
    cfg.server_defer_subscription_updates = true;
    model_t m;
    memset(&m, 0, sizeof(m));
    if (moq_simpair_create(&cfg, &m.sp) < 0 || moq_simpair_start(m.sp) < 0) {
        fprintf(stderr, "  pair start failed\n");
        if (m.sp) moq_simpair_destroy(m.sp);
        return 1;
    }
    moq_simpair_enable_faults(m.sp);
    settle(m.sp);
    m.c = moq_simpair_client(m.sp);
    m.sv = moq_simpair_server(m.sp);
    rng_t rng = { seed ^ 0xA5A5 };
    uint64_t now = moq_simpair_now_us(m.sp) + 1;
    failures += subscribe(&m, out, now);

    static moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init_sized(&loc, sizeof(loc));
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = 1;
    static moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;

    for (int step = 0; step < 40 && !failures; step++) {
        uint64_t x = rng_next(&rng);
        now = moq_simpair_now_us(m.sp) + 1;
        if (!m.live) {
            failures += subscribe(&m, out, now);
            continue;
        }
        switch ((unsigned)(x % 6)) {
        case 0: case 1: {   /* the subscriber updates its subscription */
            if (m.client_pending) break;
            moq_subscription_update_cfg_t up;
            moq_subscription_update_cfg_init_sized(&up, sizeof(up));
            unsigned what = (unsigned)((x >> 8) % 4);
            if (what == 0 || what == 3) { up.has_forward = true; up.forward = (x >> 12) & 1; }
            if (what == 1) { up.has_subscriber_priority = true; up.subscriber_priority = (uint8_t)(x >> 16); }
            if (what == 2) {
                up.has_filter = true;
                up.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
                up.start_group = (x >> 20) % 6;
            }
            if (what == 3) up.fill = &fill;
            moq_result_t rc = moq_session_update_subscription(m.c, m.csub, &up, now);
            if (rc == MOQ_ERR_UNSUPPORTED && up.fill) {   /* a profile without fills */
                up.fill = NULL;
                rc = moq_session_update_subscription(m.c, m.csub, &up, now);
            }
            if (rc == MOQ_OK) {
                m.client_pending = true;
                m.pending_forward_set = up.has_forward;
                m.pending_forward = up.forward;
                out->sent++;
            }
            break;
        }
        case 2: case 3: {   /* the publisher application answers */
            if (!m.owed || ++m.owed_age < (int)((x >> 4) % 3)) break;
            int slot = sub_resolve_handle(m.sv, m.ssub);
            if (slot < 0 || m.sv->subs[slot].forward != m.forward) {
                fprintf(stderr, "  update applied before its answer\n");
                failures++;
                break;
            }
            if ((x >> 8) % 7 == 0) {
                moq_reject_subscription_update_cfg_t rj;
                moq_reject_subscription_update_cfg_init(&rj);
                rj.update_request_id = m.owed_id;
                rj.error_code = (moq_request_error_t)0x5;
                moq_result_t rc = moq_session_reject_subscription_update(m.sv, m.ssub, &rj, now);
                if (rc == MOQ_OK) {
                    m.owed = false;
                    m.rejected = true;
                    out->rejected++;
                } else if (rc != MOQ_ERR_WOULD_BLOCK) {
                    fprintf(stderr, "  reject rc=%d\n", (int)rc);
                    failures++;
                }
            } else {
                moq_accept_subscription_update_cfg_t ac;
                moq_accept_subscription_update_cfg_init(&ac);
                ac.update_request_id = m.owed_id;
                moq_result_t rc = moq_session_accept_subscription_update(m.sv, m.ssub, &ac, now);
                if (rc == MOQ_OK) {
                    m.owed = false;
                    if (m.pending_forward_set) m.forward = m.pending_forward;
                    if (m.sv->subs[slot].forward != m.forward) {
                        fprintf(stderr, "  accepted forward state not in effect\n");
                        failures++;
                    }
                    out->accepted++;
                } else if (rc != MOQ_ERR_WOULD_BLOCK) {
                    fprintf(stderr, "  accept rc=%d\n", (int)rc);
                    failures++;
                }
            }
            break;
        }
        case 4: {   /* the publisher sends an object */
            if (m.rejected) break;
            moq_subgroup_cfg_t sg;
            moq_subgroup_cfg_init(&sg);
            sg.group_id = 7 + m.sg_next++; sg.subgroup_id = 0; sg.publisher_priority = 2;
            moq_subgroup_handle_t h;
            if (moq_session_open_subgroup(m.sv, m.ssub, &sg, now, &h) != MOQ_OK) break;
            moq_rcbuf_t *pl = NULL;
            moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"obj", 3, &pl);
            (void)moq_session_write_object(m.sv, h, 0, pl, now);
            moq_rcbuf_decref(pl);
            if ((x >> 9) & 1) (void)moq_session_close_subgroup(m.sv, h, now);
            break;
        }
        default: break;   /* pump only */
        }
        settle(m.sp);
        failures += drain(&m, out);
    }

    /* Settle: answer what is owed, then end the subscription. */
    for (int i = 0; i < 8 && m.live; i++) {
        if (m.owed) {
            moq_accept_subscription_update_cfg_t ac;
            moq_accept_subscription_update_cfg_init(&ac);
            ac.update_request_id = m.owed_id;
            if (moq_session_accept_subscription_update(m.sv, m.ssub, &ac,
                                                       moq_simpair_now_us(m.sp) + 1) == MOQ_OK) {
                m.owed = false;
                out->accepted++;
            }
        }
        settle(m.sp);
        failures += drain(&m, out);
    }
    if (m.live) {
        (void)moq_session_unsubscribe(m.c, m.csub, moq_simpair_now_us(m.sp) + 1);
        for (int i = 0; i < 4; i++) {
            settle(m.sp);
            failures += drain(&m, out);
        }
    }
    for (size_t i = 0; i < m.sv->sub_cap; i++) {
        if (m.sv->subs[i].owed_update.owed) {
            fprintf(stderr, "  an answer is still owed at the end\n");
            failures++;
        }
    }
    if (m.sv->fills.live != 0) {
        fprintf(stderr, "  publisher retains %zu fill children\n", m.sv->fills.live);
        failures++;
    }
    /* Every update ended exactly once: acknowledged or failed with the
     * subscription; the one still pending at unsubscribe ends with it. */
    if (out->acked + out->failed_done > out->sent || out->acked != out->accepted ||
        out->failed_done != out->rejected) {
        fprintf(stderr, "  outcomes: sent=%zu acked=%zu accepted=%zu failed=%zu rejected=%zu\n",
                out->sent, out->acked, out->accepted, out->failed_done, out->rejected);
        failures++;
    }
    if (out->owed_events != out->accepted + out->rejected) {
        fprintf(stderr, "  owed=%zu answered=%zu\n", out->owed_events,
                out->accepted + out->rejected);
        failures++;
    }
    if (moq_session_state(m.c) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(m.sv) != MOQ_SESS_ESTABLISHED) {
        fprintf(stderr, "  a session is not established at the end\n");
        failures++;
    }
    moq_simpair_destroy(m.sp);
    return failures;
}

int main(void)
{
    if (scenario_selected_version(MOQ_VERSION_DRAFT_22) == 0) return 1;
    uint64_t start = 0, seeds = 40;
    const char *e;
    if ((e = getenv("MOQ_SCENARIO_SEED_START"))) start = strtoull(e, NULL, 0);
    if ((e = getenv("MOQ_SCENARIO_SEEDS"))) seeds = strtoull(e, NULL, 0);
    int failures = 0;
    run_t total;
    memset(&total, 0, sizeof(total));
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
        total.sent += a.sent; total.accepted += a.accepted; total.rejected += a.rejected;
        total.subscriptions += a.subscriptions;
        total.delayed += a.delayed;
        total.objects += a.objects;
    }
    if (failures) { fprintf(stderr, "FAIL: test_scenario_update_answer (%d seeds)\n", failures); return 1; }
    printf("PASS: test_scenario_update_answer (%llu seeds: %zu updates, %zu accepted, "
           "%zu rejected, %zu subscriptions, %zu objects, %zu delayed deliveries)\n",
           (unsigned long long)seeds, total.sent, total.accepted, total.rejected,
           total.subscriptions, total.objects, total.delayed);
    return 0;
}
