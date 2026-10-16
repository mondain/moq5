/*
 * Deterministic streaming fault injection scenario runner.
 *
 * Exercises OBJECT_CHUNK delivery under transport-level data faults:
 * SPLIT_DATA, DROP_DATA, MUTATE_DATA, REORDER_ACTION, INJECT_RESET,
 * INJECT_STOP, INJECT_CLOSE.
 *
 * Uses SimPair with client_streaming_objects=true. Pumps through
 * run_until_quiescent so faults are actually applied to data streams.
 * Persistent chunk oracle validates begin/end pairing across all drain
 * paths. Each seed runs twice; all counters and hashes must match.
 */

#include <moq/sim.h>
#include "scenario_pair_version.h"
#include "../../tests/unit/test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../sim/src/simpair_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -- splitmix64 PRNG ------------------------------------------------ */

typedef struct { uint64_t s; } rng_t;

static uint64_t rng_next(rng_t *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* -- FNV-1a helpers ------------------------------------------------- */

#define FNV_INIT 0xCBF29CE484222325ULL

static void fnv_u64(uint64_t *h, uint64_t v) {
    *h ^= v; *h *= 0x100000001B3ULL;
}

static void fnv_bytes(uint64_t *h, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        *h ^= d[i]; *h *= 0x100000001B3ULL;
    }
}

/* -- Run summary ---------------------------------------------------- */

#define ORACLE_OBJECTS 32

typedef struct {
    bool active, has_ref;
    moq_subscription_t sub;
    moq_publication_t pub;
    uint64_t group, subgroup, object, length, received;
    moq_stream_ref_t ref;
} oracle_object_t;

typedef struct {
    /* Trace determinism. */
    uint64_t trace_hash;
    size_t   trace_count;
    /* Chunk oracle. */
    size_t   chunks;
    size_t   complete;
    size_t   violations;
    uint64_t chunk_hash;
    bool     in_object;
    oracle_object_t objects[ORACLE_OBJECTS];
    moq_session_t *client;
    bool observe_inputs;
    size_t finite_open, delivered_fin, delivered_reset;
    /* Fault counters. */
    size_t   data_inputs;
    size_t   f_drop;
    size_t   f_mutate;
    size_t   f_reorder;
    size_t   f_inject_reset;
    size_t   f_inject_stop;
    size_t   f_inject_close;
    size_t   f_truncate_control;
    size_t   f_truncate_data;
    size_t   reset_terminals;
    /* Session state. */
    size_t   closed;
    /* Prelude. */
    size_t   pre_objects;
    size_t   pre_chunks;
    uint64_t pre_payload_hash;
} run_summary_t;

static void oracle_after_input(run_summary_t *s,
    const moq_sim_trace_record_t *r, bool suppress_terminal);

static void trace_fn(void *ctx, const moq_sim_trace_record_t *r) {
    run_summary_t *s = (run_summary_t *)ctx;
    uint64_t h = s->trace_hash;
    fnv_u64(&h, r->seed);
    fnv_u64(&h, r->step);
    fnv_u64(&h, r->now_us);
    fnv_u64(&h, (uint64_t)r->kind);
    fnv_u64(&h, (uint64_t)r->from);
    fnv_u64(&h, (uint64_t)r->to);
    fnv_u64(&h, (uint64_t)r->action_kind);
    fnv_u64(&h, (uint64_t)r->input_kind);
    fnv_u64(&h, (uint64_t)r->result);
    fnv_u64(&h, r->code);
    fnv_u64(&h, r->bytes.len);
    if (r->bytes.data && r->bytes.len > 0)
        fnv_bytes(&h, r->bytes.data, r->bytes.len);
    s->trace_hash = h;
    s->trace_count++;

    if (r->kind == MOQ_SIM_TRACE_FAULT_DROP) {
        if (r->action_kind == MOQ_ACTION_SEND_DATA) s->f_drop++;
    }
    if (r->kind == MOQ_SIM_TRACE_FAULT_MUTATE) {
        if (r->action_kind == MOQ_ACTION_SEND_DATA) s->f_mutate++;
    }
    if (r->kind == MOQ_SIM_TRACE_FAULT_REORDER)
        s->f_reorder++;
    if (r->kind == MOQ_SIM_TRACE_FAULT_INJECT) {
        if (r->action_kind == MOQ_ACTION_RESET_DATA)
            s->f_inject_reset++;
        else if (r->action_kind == MOQ_ACTION_STOP_DATA)
            s->f_inject_stop++;
        else if (r->action_kind == MOQ_ACTION_CLOSE_SESSION)
            s->f_inject_close++;
    }
    if (r->kind == MOQ_SIM_TRACE_FAULT_TRUNCATE) {
        if (r->action_kind == MOQ_ACTION_SEND_CONTROL)
            s->f_truncate_control++;
        else if (r->action_kind == MOQ_ACTION_SEND_DATA)
            s->f_truncate_data++;
    }
    if (r->kind == MOQ_SIM_TRACE_INPUT &&
        r->input_kind == MOQ_SIM_INPUT_DATA_BYTES)
        s->data_inputs++;
    if (s->observe_inputs && r->kind == MOQ_SIM_TRACE_INPUT &&
        r->to == MOQ_PERSPECTIVE_CLIENT)
        oracle_after_input(s, r, false);
}

/* -- Counting allocator --------------------------------------------- */

typedef struct { int64_t balance; } scen_alloc_t;

static void *sa_alloc(size_t sz, void *ctx) {
    scen_alloc_t *s = (scen_alloc_t *)ctx;
    void *p = malloc(sz); if (p) s->balance++;
    return p;
}
static void *sa_realloc(void *p, size_t o, size_t n, void *ctx) {
    scen_alloc_t *s = (scen_alloc_t *)ctx;
    (void)o;
    if (!p) { void *r = realloc(NULL, n); if (r) s->balance++; return r; }
    return realloc(p, n);
}
static void sa_free(void *p, size_t sz, void *ctx) {
    scen_alloc_t *s = (scen_alloc_t *)ctx;
    (void)sz; if (p) s->balance--;
    free(p);
}

/* -- Chunk oracle --------------------------------------------------- */

static oracle_object_t *oracle_find(run_summary_t *s,
    moq_subscription_t sub, moq_publication_t pub,
    uint64_t group, uint64_t subgroup)
{
    for (size_t i = 0; i < ORACLE_OBJECTS; i++) {
        oracle_object_t *o = &s->objects[i];
        if (o->active && moq_subscription_eq(o->sub, sub) &&
            moq_publication_eq(o->pub, pub) &&
            o->group == group && o->subgroup == subgroup)
            return o;
    }
    return NULL;
}

static void oracle_open_count(run_summary_t *s)
{
    s->finite_open = 0;
    for (size_t i = 0; i < ORACLE_OBJECTS; i++)
        if (s->objects[i].active) s->finite_open++;
    s->in_object = s->finite_open != 0;
}

static void oracle_observe(run_summary_t *s, const moq_object_chunk_event_t *ck)
{
    s->chunks++;
    oracle_object_t *o = oracle_find(s, ck->sub, ck->pub,
                                   ck->group_id, ck->subgroup_id);
    if (ck->begin) {
        if (o) s->violations++;
        else for (size_t i = 0; i < ORACLE_OBJECTS; i++) {
            if (!s->objects[i].active) { o = &s->objects[i]; break; }
        }
        if (o) {
            memset(o, 0, sizeof(*o));
            o->active = true;
            o->sub = ck->sub; o->pub = ck->pub;
            o->group = ck->group_id; o->subgroup = ck->subgroup_id;
            o->object = ck->object_id; o->length = ck->payload_length;
        }
    }
    if (!o) { s->violations++; return; }
    if (o->object != ck->object_id) s->violations++;
    fnv_u64(&s->chunk_hash, ck->group_id);
    fnv_u64(&s->chunk_hash, ck->subgroup_id);
    fnv_u64(&s->chunk_hash, ck->object_id);
    fnv_u64(&s->chunk_hash, ck->begin);
    fnv_u64(&s->chunk_hash, ck->end);
    if (ck->chunk) {
        size_t cl = moq_rcbuf_len(ck->chunk);
        fnv_bytes(&s->chunk_hash, moq_rcbuf_data(ck->chunk), cl);
        fnv_u64(&s->chunk_hash, cl);
        if (o->received > o->length || cl > o->length - o->received)
            s->violations++;
        o->received += cl;
    }
    if (ck->end) {
        if (ck->terminal == MOQ_OBJECT_TERMINAL_NORMAL &&
            o->received != o->length) s->violations++;
        o->active = false;
        s->complete++;
        if (ck->terminal == MOQ_OBJECT_TERMINAL_RESET)
            s->reset_terminals++;
    }
    oracle_open_count(s);
}

static bool oracle_stream_owner(const moq_rx_stream_t *rx,
    const oracle_object_t *o)
{
    if (!rx->fan_shared)
        return moq_subscription_eq(rx->sub, o->sub) &&
               moq_publication_eq(rx->pub_handle, o->pub);
    for (uint32_t i = 0; i < rx->fan_count; i++)
        if (moq_subscription_eq(rx->fan[i].sub, o->sub) &&
            moq_publication_eq(rx->fan[i].pub, o->pub)) return true;
    return false;
}

static void oracle_bind_streams(const moq_session_t *c, run_summary_t *s)
{
    /* Test-private, read-only identity lookup: delayed begins can surface on
     * SUBSCRIBE_OK, after multiple data streams overtook the response. Snapshot
     * full generation-bearing owner handles and the decoded subgroup's ref,
     * never parser terminal state. Keep that ref until the observed object ends
     * or the session closes, even if the core frees/reuses its receive slot.
     * Completed objects need no mapping: their receive slot may already be gone.
     * The bounded object inventory is also the provenance inventory. */
    for (size_t i = 0; i < ORACLE_OBJECTS; i++) {
        oracle_object_t *o = &s->objects[i];
        if (!o->active || o->has_ref) continue;
        const moq_rx_stream_t *match = NULL;
        size_t matches = 0;
        for (size_t j = 0; j < c->rx_cap; j++) {
            const moq_rx_stream_t *rx = &c->rx_streams[j];
            if (rx->active && rx->stream_kind == MOQ_STREAM_KIND_SUBGROUP &&
                rx->subgroup_id_resolved && rx->group_id == o->group &&
                rx->subgroup_id == o->subgroup && oracle_stream_owner(rx, o)) {
                match = rx;
                matches++;
            }
        }
        if (matches != 1) { s->violations++; continue; }
        o->ref = match->stream_ref;
        o->has_ref = true;
    }
}

static void oracle_drain(moq_session_t *c, run_summary_t *s,
    bool suppress_terminal)
{
    moq_event_t evts[8];
    size_t ne;
    while ((ne = moq_session_poll_events(c, evts, 8)) > 0)
        for (size_t i = 0; i < ne; i++) {
            moq_event_t *ev = &evts[i];
            if (ev->kind == MOQ_EVENT_OBJECT_CHUNK) {
                const moq_object_chunk_event_t *ck = &ev->u.object_chunk;
                if (!suppress_terminal || !ck->end) oracle_observe(s, ck);
            } else if (ev->kind == MOQ_EVENT_SUBGROUP_FINISHED) {
                const moq_subgroup_finished_event_t *sg = &ev->u.subgroup_finished;
                if (oracle_find(s, sg->sub, sg->pub, sg->group_id, sg->subgroup_id))
                    s->violations++;
            } else if (ev->kind == MOQ_EVENT_SUBGROUP_RESET) {
                const moq_subgroup_reset_event_t *sg = &ev->u.subgroup_reset;
                if (oracle_find(s, sg->sub, sg->pub, sg->group_id, sg->subgroup_id))
                    s->violations++;
            } else if (ev->kind == MOQ_EVENT_SESSION_CLOSED) {
                /* Hard session closure supersedes queued object output. */
                s->closed++;
                memset(s->objects, 0, sizeof(s->objects));
                oracle_open_count(s);
            }
            moq_event_cleanup(&evts[i]);
        }
    oracle_bind_streams(c, s);
}

static void drain_client(moq_session_t *c, run_summary_t *s)
{
    oracle_drain(c, s, false);
}

static void oracle_after_input(run_summary_t *s,
    const moq_sim_trace_record_t *r, bool suppress_terminal)
{
    bool data = r->input_kind == MOQ_SIM_INPUT_DATA_BYTES ||
                r->input_kind == MOQ_SIM_INPUT_DATA_RESET;
    /* Poll only the core session, never reenter SimPair. The INPUT ref names
     * the delivered FIN/reset; object provenance is independent of which
     * data or response input happened to release its begin event. */
    oracle_drain(s->client, s, suppress_terminal);
    if (!data || r->result != MOQ_OK) return;
    bool reset = r->input_kind == MOQ_SIM_INPUT_DATA_RESET;
    if (!reset && !r->fin) return;
    if (reset) s->delivered_reset++; else s->delivered_fin++;
    for (size_t i = 0; i < ORACLE_OBJECTS; i++) {
        const oracle_object_t *o = &s->objects[i];
        if (o->active && (!o->has_ref || o->ref._v == r->stream_ref._v))
            s->violations++;
    }
}

static void oracle_finish(run_summary_t *s)
{
    /* Finite execution supplies no FIN/reset. Only observed delivery and
     * closure above discharge terminal obligations; unfinished input is legal. */
    oracle_open_count(s);
}

/* -- Shadow model --------------------------------------------------- */

#define MAX_SUBS 4
#define MAX_SGS  4

typedef struct {
    bool active; bool accepted;
    moq_subscription_t server_handle, client_handle;
} shadow_sub_t;

typedef struct {
    bool active;
    moq_subgroup_handle_t handle;
    uint64_t next_object_id;
} shadow_sg_t;

typedef struct {
    shadow_sub_t subs[MAX_SUBS];
    shadow_sg_t  sgs[MAX_SGS];
    bool         closed;
    uint64_t     next_group;
} shadow_state_t;

/* -- Return-code policy --------------------------------------------- */

static bool rc_expected(moq_result_t rc) {
    return rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK ||
           rc == MOQ_ERR_CLOSED || rc == MOQ_ERR_WRONG_STATE ||
           rc == MOQ_ERR_STALE_HANDLE || rc == MOQ_ERR_REQUEST_BLOCKED ||
           rc == MOQ_ERR_PROTO || rc == MOQ_ERR_INVAL ||
           rc == MOQ_ERR_NOMEM || rc == MOQ_ERR_BUFFER;
}

static int pump_checked(moq_simpair_t *sp, size_t max, run_summary_t *s)
{
    moq_result_t rc = moq_simpair_run_until_quiescent(sp, max, NULL);
    if (rc != MOQ_OK && rc != MOQ_ERR_WOULD_BLOCK &&
        rc != MOQ_ERR_CLOSED && rc != MOQ_ERR_PROTO)
        return -1;
    drain_client(moq_simpair_client(sp), s);
    return 0;
}

/* -- Prelude (no faults) -------------------------------------------- */

static int run_prelude(moq_simpair_t *sp, run_summary_t *s,
                        const moq_alloc_t *alloc)
{
    moq_session_t *c = moq_simpair_client(sp);
    moq_session_t *sv = moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);

    /* Subscribe. */
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("ns") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = ns;
    sub_cfg.track_name = MOQ_BYTES_LITERAL("pre");
    moq_subscription_t csub;
    if (moq_session_subscribe(c, &sub_cfg, now, &csub) != MOQ_OK) return -1;
    if (pump_checked(sp, 8, s) < 0) return -1;

    /* Accept. */
    moq_event_t ev;
    if (moq_session_poll_events(sv, &ev, 1) != 1) return -1;
    if (ev.kind != MOQ_EVENT_SUBSCRIBE_REQUEST) { moq_event_cleanup(&ev); return -1; }
    moq_subscription_t ssub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    if (moq_session_accept_subscribe(sv, ssub, &acc, now) != MOQ_OK) return -1;
    if (pump_checked(sp, 8, s) < 0) return -1;

    /* Open subgroup + write multi-chunk object (100 bytes). */
    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 0; sg_cfg.subgroup_id = 0;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(sv, ssub, &sg_cfg, now, &sg) != MOQ_OK) return -1;

    uint8_t payload[100];
    for (int i = 0; i < 100; i++) payload[i] = (uint8_t)(i + 1);
    moq_rcbuf_t *p = NULL;
    if (moq_rcbuf_create(alloc, payload, sizeof(payload), &p) != MOQ_OK) return -1;
    if (moq_session_write_object(sv, sg, 0, p, now) != MOQ_OK) {
        moq_rcbuf_decref(p); return -1;
    }
    moq_rcbuf_decref(p);
    if (moq_session_close_subgroup(sv, sg, now) != MOQ_OK) return -1;

    /* Pump and reconstruct payload from OBJECT_CHUNK events. */
    {
        moq_result_t prc = moq_simpair_run_until_quiescent(sp, 16, NULL);
        if (prc != MOQ_OK && prc != MOQ_ERR_WOULD_BLOCK) return -1;

        uint8_t reasm[100];
        size_t rlen = 0;
        bool got_begin = false, got_end = false;
        moq_event_t ev;
        while (moq_session_poll_events(c, &ev, 1) == 1) {
            if (ev.kind == MOQ_EVENT_OBJECT_CHUNK) {
                oracle_observe(s, &ev.u.object_chunk);
                if (ev.u.object_chunk.begin) got_begin = true;
                if (ev.u.object_chunk.chunk) {
                    size_t cl = moq_rcbuf_len(ev.u.object_chunk.chunk);
                    if (rlen + cl <= sizeof(reasm))
                        memcpy(reasm + rlen,
                            moq_rcbuf_data(ev.u.object_chunk.chunk), cl);
                    rlen += cl;
                }
                if (ev.u.object_chunk.end) got_end = true;
            }
            moq_event_cleanup(&ev);
        }
        if (!got_begin || !got_end) return -1;
        if (rlen != sizeof(payload)) return -1;
        if (memcmp(reasm, payload, sizeof(payload)) != 0) return -1;

        /* Hash the expected payload for determinism comparison. */
        fnv_bytes(&s->pre_payload_hash, payload, sizeof(payload));
        s->pre_chunks = s->chunks;
        /* Exactly one prelude object must have completed. */
        if (s->complete != 1) return -1;
        s->pre_objects = 1;
    }

    return 0;
}

/* -- Operations ----------------------------------------------------- */

typedef enum {
    OP_PUMP, OP_SUBSCRIBE, OP_ACCEPT,
    OP_OPEN_SG, OP_WRITE, OP_CLOSE_SG, OP_RESET_SG,
    OP_DRAIN, OP_DRAIN_SERVER, OP_ADVANCE_TIME,
    OP_COUNT,
} scenario_op_t;

static int execute_step(moq_simpair_t *sp, shadow_state_t *st,
    rng_t *rng, run_summary_t *s, const moq_alloc_t *alloc)
{
    if (st->closed) return 0;
    moq_session_t *c = moq_simpair_client(sp);
    moq_session_t *sv = moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);

    scenario_op_t op = (scenario_op_t)(rng_next(rng) % OP_COUNT);

    switch (op) {
    case OP_PUMP:
        if (pump_checked(sp, 4, s) < 0) return -1;
        break;

    case OP_SUBSCRIBE: {
        char name[16];
        snprintf(name, sizeof(name), "t%llu",
                 (unsigned long long)(rng_next(rng) % 1000));
        moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("ns") };
        moq_namespace_t ns = { ns_parts, 1 };
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = ns;
        cfg.track_name = moq_bytes_cstr(name);
        moq_subscription_t h;
        moq_result_t rc = moq_session_subscribe(c, &cfg, now, &h);
        if (rc == MOQ_OK) {
            if (pump_checked(sp, 4, s) < 0) return -1;
            moq_event_t evts[4];
            size_t ne = moq_session_poll_events(sv, evts, 4);
            for (size_t j = 0; j < ne; j++) {
                if (evts[j].kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
                    for (int k = 0; k < MAX_SUBS; k++) {
                        if (!st->subs[k].active) {
                            st->subs[k].active = true;
                            st->subs[k].accepted = false;
                            st->subs[k].server_handle = evts[j].u.subscribe_request.sub;
                            st->subs[k].client_handle = h;
                            break;
                        }
                    }
                }
                moq_event_cleanup(&evts[j]);
            }
        } else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_ACCEPT: {
        int idx = -1;
        for (int i = 0; i < MAX_SUBS; i++)
            if (st->subs[i].active && !st->subs[i].accepted) { idx = i; break; }
        if (idx < 0) { pump_checked(sp, 4, s); break; }
        moq_accept_subscribe_cfg_t acc;
        moq_accept_subscribe_cfg_init(&acc);
        moq_result_t rc = moq_session_accept_subscribe(sv,
            st->subs[idx].server_handle, &acc, now);
        if (rc == MOQ_OK) {
            st->subs[idx].accepted = true;
            if (pump_checked(sp, 4, s) < 0) return -1;
        } else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_OPEN_SG: {
        int sub_idx = -1;
        for (int i = 0; i < MAX_SUBS; i++)
            if (st->subs[i].active && st->subs[i].accepted) { sub_idx = i; break; }
        if (sub_idx < 0) { pump_checked(sp, 4, s); break; }
        int sg_slot = -1;
        for (int i = 0; i < MAX_SGS; i++)
            if (!st->sgs[i].active) { sg_slot = i; break; }
        if (sg_slot < 0) { pump_checked(sp, 4, s); break; }
        moq_subgroup_cfg_t cfg;
        moq_subgroup_cfg_init(&cfg);
        cfg.group_id = st->next_group; cfg.subgroup_id = 0;
        moq_subgroup_handle_t h;
        moq_result_t rc = moq_session_open_subgroup(sv,
            st->subs[sub_idx].server_handle, &cfg, now, &h);
        if (rc == MOQ_OK) {
            st->sgs[sg_slot].active = true;
            st->sgs[sg_slot].handle = h;
            st->sgs[sg_slot].next_object_id = 0;
            st->next_group++;
        } else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_WRITE: {
        int sg_idx = -1;
        for (int i = 0; i < MAX_SGS; i++)
            if (st->sgs[i].active) { sg_idx = i; break; }
        if (sg_idx < 0) { pump_checked(sp, 4, s); break; }
        size_t plen = rng_next(rng) % 64;
        if (rng_next(rng) % 10 == 0) plen = 0;
        uint8_t pdata[64];
        for (size_t j = 0; j < plen; j++) pdata[j] = (uint8_t)(rng_next(rng) & 0xFF);
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(alloc, pdata, plen, &p);
        if (!p) break;
        moq_result_t rc = moq_session_write_object(sv,
            st->sgs[sg_idx].handle, st->sgs[sg_idx].next_object_id,
            p, now);
        moq_rcbuf_decref(p);
        if (rc == MOQ_OK) st->sgs[sg_idx].next_object_id++;
        else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_CLOSE_SG: {
        int sg_idx = -1;
        for (int i = 0; i < MAX_SGS; i++)
            if (st->sgs[i].active) { sg_idx = i; break; }
        if (sg_idx < 0) { pump_checked(sp, 4, s); break; }
        moq_result_t rc = moq_session_close_subgroup(sv,
            st->sgs[sg_idx].handle, now);
        if (rc == MOQ_OK) st->sgs[sg_idx].active = false;
        else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_RESET_SG: {
        int sg_idx = -1;
        for (int i = 0; i < MAX_SGS; i++)
            if (st->sgs[i].active) { sg_idx = i; break; }
        if (sg_idx < 0) { pump_checked(sp, 4, s); break; }
        moq_result_t rc = moq_session_reset_subgroup(sv,
            st->sgs[sg_idx].handle, 0x1, now);
        if (rc == MOQ_OK) st->sgs[sg_idx].active = false;
        else if (!rc_expected(rc)) return -1;
        break;
    }

    case OP_DRAIN:
        drain_client(c, s);
        break;

    case OP_DRAIN_SERVER: {
        moq_event_t evts[8];
        size_t ne = moq_session_poll_events(sv, evts, 8);
        for (size_t i = 0; i < ne; i++) moq_event_cleanup(&evts[i]);
        break;
    }

    case OP_ADVANCE_TIME: {
        uint64_t delta = (rng_next(rng) % 100) + 1;
        moq_result_t rc = moq_simpair_advance_to(sp, now + delta);
        if (rc != MOQ_OK && rc != MOQ_ERR_CLOSED) return -1;
        break;
    }

    default:
        pump_checked(sp, 4, s);
        break;
    }

    if (moq_session_state(c) == MOQ_SESS_CLOSED ||
        moq_session_state(sv) == MOQ_SESS_CLOSED) {
        st->closed = true;
    }

    return 0;
}

/* -- Run one scenario ----------------------------------------------- */

static int run_scenario(uint64_t seed, uint64_t per_mille,
                         const moq_alloc_t *alloc,
                         run_summary_t *summary, int run_id,
                         int max_steps)
{
    memset(summary, 0, sizeof(*summary));
    summary->trace_hash = FNV_INIT;
    summary->chunk_hash = FNV_INIT;
    summary->pre_payload_hash = FNV_INIT;

    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = scenario_selected_version(MOQ_VERSION_DRAFT_16);
    cfg.alloc = alloc;
    cfg.seed = seed;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = summary;
    cfg.client_streaming_objects = true;
    cfg.fault_per_mille = (uint32_t)per_mille;
    cfg.fault_flags = MOQ_SIM_FAULT_SPLIT_DATA |
                      MOQ_SIM_FAULT_DROP_DATA |
                      MOQ_SIM_FAULT_MUTATE_DATA |
                      MOQ_SIM_FAULT_REORDER_ACTION |
                      MOQ_SIM_FAULT_INJECT_RESET |
                      MOQ_SIM_FAULT_INJECT_STOP |
                      MOQ_SIM_FAULT_INJECT_CLOSE |
                      MOQ_SIM_FAULT_TRUNCATE_CONTROL |
                      MOQ_SIM_FAULT_TRUNCATE_DATA;

    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return -1;
    summary->client = moq_simpair_client(sp);
    moq_simpair_start(sp);
    {
        moq_result_t rc = moq_simpair_run_until_quiescent(sp, 8, NULL);
        if (rc != MOQ_OK && rc != MOQ_ERR_WOULD_BLOCK) {
            moq_simpair_destroy(sp); return -1;
        }
    }
    { moq_event_t ev;
      if (moq_session_poll_events(moq_simpair_client(sp), &ev, 1) == 1)
          moq_event_cleanup(&ev);
      if (moq_session_poll_events(moq_simpair_server(sp), &ev, 1) == 1)
          moq_event_cleanup(&ev);
    }

    /* Prelude without faults. */
    if (run_prelude(sp, summary, alloc) < 0) {
        fprintf(stderr, "FAIL seed=0x%llx run=%d: prelude failed\n",
                (unsigned long long)seed, run_id);
        fprintf(stderr, "  Replay: MOQ_SCENARIO_SEED_START=0x%llx "
                "MOQ_SCENARIO_SEEDS=1 MOQ_SCENARIO_STEPS=%d "
                "MOQ_SCENARIO_FAULT_PERMILLE=%llu "
                "./build/tests/test_scenario_streaming_faults\n",
                (unsigned long long)seed, max_steps,
                (unsigned long long)per_mille);
        /* Drain owned resources. */
        drain_client(moq_simpair_client(sp), summary);
        { moq_event_t d[16]; size_t ne;
          while ((ne = moq_session_poll_events(moq_simpair_server(sp), d, 16)) > 0)
              for (size_t i = 0; i < ne; i++) moq_event_cleanup(&d[i]);
          moq_action_t a[16]; size_t na;
          while ((na = moq_session_poll_actions(moq_simpair_server(sp), a, 16)) > 0)
              for (size_t i = 0; i < na; i++) moq_action_cleanup(&a[i]);
          while ((na = moq_session_poll_actions(moq_simpair_client(sp), a, 16)) > 0)
              for (size_t i = 0; i < na; i++) moq_action_cleanup(&a[i]);
        }
        moq_simpair_destroy(sp);
        return 1;
    }

    /* Enable faults for random phase. */
    summary->observe_inputs = true;
    moq_simpair_enable_faults(sp);

    shadow_state_t st;
    memset(&st, 0, sizeof(st));
    rng_t rng = { seed };
    int failures = 0;

    for (int step = 0; step < max_steps; step++) {
        int src = execute_step(sp, &st, &rng, summary, alloc);
        if (src < 0) {
            fprintf(stderr, "FAIL seed=0x%llx run=%d step=%d: "
                    "unexpected error\n",
                    (unsigned long long)seed, run_id, step);
            fprintf(stderr, "  Replay: MOQ_SCENARIO_SEED_START=0x%llx "
                    "MOQ_SCENARIO_SEEDS=1 MOQ_SCENARIO_STEPS=%d "
                    "MOQ_SCENARIO_FAULT_PERMILLE=%llu "
                    "./build/tests/test_scenario_streaming_faults\n",
                    (unsigned long long)seed, max_steps,
                    (unsigned long long)per_mille);
            failures++;
            st.closed = true;
            break;
        }
        if (st.closed) break;
    }

    /* Final pump + drain. */
    if (!st.closed) {
        for (int f = 0; f < 8; f++) {
            moq_result_t prc = moq_simpair_run_until_quiescent(sp, 8, NULL);
            if (prc != MOQ_OK && prc != MOQ_ERR_WOULD_BLOCK) break;
            drain_client(moq_simpair_client(sp), summary);
        }
    }

    /* Drain all remaining owned resources through the oracle. */
    drain_client(moq_simpair_client(sp), summary);
    { moq_event_t d[16]; size_t ne;
      while ((ne = moq_session_poll_events(moq_simpair_server(sp), d, 16)) > 0)
          for (size_t i = 0; i < ne; i++) moq_event_cleanup(&d[i]);
      moq_action_t a[16]; size_t na;
      while ((na = moq_session_poll_actions(moq_simpair_server(sp), a, 16)) > 0)
          for (size_t i = 0; i < na; i++) moq_action_cleanup(&a[i]);
      while ((na = moq_session_poll_actions(moq_simpair_client(sp), a, 16)) > 0)
          for (size_t i = 0; i < na; i++) moq_action_cleanup(&a[i]);
    }

    oracle_finish(summary);

    if (summary->violations > 0) {
        fprintf(stderr, "FAIL seed=0x%llx run=%d: %zu chunk oracle "
                "violations\n",
                (unsigned long long)seed, run_id, summary->violations);
        fprintf(stderr, "  Replay: MOQ_SCENARIO_SEED_START=0x%llx "
                "MOQ_SCENARIO_SEEDS=1 MOQ_SCENARIO_STEPS=%d "
                "MOQ_SCENARIO_FAULT_PERMILLE=%llu "
                "./build/tests/test_scenario_streaming_faults\n",
                (unsigned long long)seed, max_steps,
                (unsigned long long)per_mille);
        failures++;
    }

    moq_simpair_destroy(sp);
    return failures;
}

/* -- Isolated terminal controls (close injection disabled) ---------- */

static int check_terminal_control(int mode, uint64_t *hash_out)
{
    int failures = 0;
    scen_alloc_t as = {0};
    moq_alloc_t alloc = { &as, sa_alloc, sa_realloc, sa_free };
    run_summary_t sum = {0};
    sum.trace_hash = sum.chunk_hash = FNV_INIT;
    moq_simpair_t *sp = NULL;
#define CONTROL_NEED(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL terminal control mode=%d prerequisite: %s\n", mode, #expr); \
    failures++; goto done; \
} } while (0)
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = scenario_selected_version(MOQ_VERSION_DRAFT_16);
    cfg.alloc = &alloc;
    cfg.initial_now_us = 1000;
    cfg.client_streaming_objects = true;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 8;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = &sum;
    CONTROL_NEED(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    sum.client = moq_simpair_client(sp);
    moq_session_t *c = sum.client, *sv = moq_simpair_server(sp);
    CONTROL_NEED(moq_simpair_start(sp) == MOQ_OK);
    CONTROL_NEED(pump_checked(sp, 8, &sum) == 0);
    CONTROL_NEED(moq_session_version(c) == cfg.version);
    CONTROL_NEED(moq_session_version(sv) == cfg.version);
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1)) moq_event_cleanup(&ev);

    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("finite")};
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = (moq_namespace_t){parts, 1};
    sub_cfg.track_name = MOQ_BYTES_LITERAL("partial");
    moq_subscription_t sub;
    CONTROL_NEED(moq_session_subscribe(c, &sub_cfg, 1000, &sub) == MOQ_OK);
    CONTROL_NEED(pump_checked(sp, 8, &sum) == 0);
    CONTROL_NEED(moq_session_poll_events(sv, &ev, 1) == 1);
    bool request = ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST;
    moq_subscription_t server_sub = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    CONTROL_NEED(request);
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    CONTROL_NEED(moq_session_accept_subscribe(sv, server_sub, &acc, 1000) == MOQ_OK);
    CONTROL_NEED(pump_checked(sp, 8, &sum) == 0);
    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 3;
    moq_subgroup_handle_t sg;
    CONTROL_NEED(moq_session_open_subgroup(sv, server_sub, &sg_cfg, 1000, &sg) == MOQ_OK);
    moq_rcbuf_t *payload = NULL;
    CONTROL_NEED(moq_rcbuf_create(&alloc, (const uint8_t *)"abcd", 4, &payload) == MOQ_OK);
    moq_result_t rc = moq_session_write_object(sv, sg, 0, payload, 1000);
    moq_rcbuf_decref(payload);
    CONTROL_NEED(rc == MOQ_OK);
    uint8_t wire[128];
    size_t length = 0;
    moq_action_t action;
    while (moq_session_poll_actions(sv, &action, 1)) {
        if (action.kind == MOQ_ACTION_SEND_DATA) {
            size_t h = action.u.send_data.header_len;
            size_t p = moq_rcbuf_len(action.u.send_data.payload);
            if (length + h + p > sizeof(wire)) {
                moq_action_cleanup(&action);
                CONTROL_NEED(false);
            }
            memcpy(wire + length, action.u.send_data.header, h);
            length += h;
            if (p) memcpy(wire + length, moq_rcbuf_data(action.u.send_data.payload), p);
            length += p;
        }
        moq_action_cleanup(&action);
    }
    CONTROL_NEED(length > 4);
    moq_sim_trace_record_t input = {0};
    input.struct_size = sizeof(input);
    input.kind = MOQ_SIM_TRACE_INPUT;
    input.to = MOQ_PERSPECTIVE_CLIENT;
    input.input_kind = MOQ_SIM_INPUT_DATA_BYTES;
    input.stream_ref = moq_stream_ref_from_u64(9000);
    input.result = moq_session_on_data_bytes(c, input.stream_ref,
        wire, length - 3, false, 1000);
    CONTROL_NEED(input.result == MOQ_OK);
    oracle_after_input(&sum, &input, false);
    CONTROL_NEED(sum.in_object && sum.complete == 0 && sum.violations == 0);

    if (mode == 1) {
        /* Consume an actual sender FIN without delivering it. */
        CONTROL_NEED(moq_session_close_subgroup(sv, sg, 1000) == MOQ_OK);
        size_t dropped_fin = 0;
        while (moq_session_poll_actions(sv, &action, 1)) {
            if (action.kind == MOQ_ACTION_SEND_DATA && action.u.send_data.fin)
                dropped_fin++;
            moq_action_cleanup(&action);
        }
        CONTROL_NEED(dropped_fin == 1);
    } else if (mode == 2 || mode == 3) {
        input.input_kind = MOQ_SIM_INPUT_DATA_RESET;
        input.error_code = 0x77;
        input.result = moq_session_on_data_reset(c, input.stream_ref, 0x77, 1000);
        CONTROL_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, mode == 3);
        CONTROL_NEED(sum.delivered_reset == 1);
        if (mode == 2) CONTROL_NEED(sum.reset_terminals == 1);
    } else if (mode == 4 || mode == 5 || mode == 7) {
        input.fin = true;
        input.result = moq_session_on_data_bytes(c, input.stream_ref,
            mode == 7 ? NULL : wire + length - 3, mode == 7 ? 0 : 3, true, 1000);
        CONTROL_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, mode == 5);
        CONTROL_NEED(sum.delivered_fin == 1);
        if (mode == 4) CONTROL_NEED(sum.complete == 1);
    } else if (mode == 6) {
        const uint8_t unknown[] = {
            cfg.version == MOQ_VERSION_DRAFT_16 ? 0x40 : 0x80, 0xff, 0, 0
        };
        input.input_kind = MOQ_SIM_INPUT_CONTROL_BYTES;
        input.result = moq_session_on_control_bytes(c, unknown, sizeof(unknown), 1000);
        CONTROL_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, false);
    } else if (mode == 8 || mode == 9) {
        moq_object_chunk_event_t wrong = {0};
        wrong.sub = sub;
        wrong.group_id = 3;
        wrong.object_id = mode == 8 ? 1 : 0;
        wrong.begin = mode == 9;
        wrong.payload_length = 4;
        oracle_observe(&sum, &wrong);
    }
    CONTROL_NEED(sum.f_inject_close == 0);
    if (mode == 6 || mode == 7) {
        CONTROL_NEED(moq_session_state(c) == MOQ_SESS_CLOSED && sum.closed == 1);
    } else {
        CONTROL_NEED(moq_session_state(c) == MOQ_SESS_ESTABLISHED);
        CONTROL_NEED(moq_session_state(sv) == MOQ_SESS_ESTABLISHED);
    }
    oracle_finish(&sum);
    bool negative = mode == 3 || mode == 5 || mode == 8 || mode == 9;
    MOQ_TEST_CHECK(negative ? sum.violations > 0 : sum.violations == 0);
    if (mode < 2) MOQ_TEST_CHECK(sum.finite_open == 1);
    if (mode == 2 || mode == 4 || mode == 6 || mode == 7)
        MOQ_TEST_CHECK(sum.finite_open == 0);
done:
    if (sp) moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
    *hash_out = sum.chunk_hash;
    fnv_u64(hash_out, sum.violations);
    fnv_u64(hash_out, sum.finite_open);
    fnv_u64(hash_out, sum.delivered_fin);
    fnv_u64(hash_out, sum.delivered_reset);
    return failures;
#undef CONTROL_NEED
}

static int check_terminal_controls(void)
{
    static const char *names[] = {
        "finite-absent", "finite-dropped-fin", "delivered-reset",
        "suppressed-reset-terminal", "delivered-fin", "suppressed-fin-terminal",
        "protocol-session-close", "partial-fin-session-close",
        "wrong-object-identity", "duplicate-begin"
    };
    int failures = 0;
    for (int mode = 0; mode < 10; mode++) {
        uint64_t h[2] = {0};
        int n = check_terminal_control(mode, &h[0]);
        n += check_terminal_control(mode, &h[1]);
        if (n) fprintf(stderr, "FAIL terminal control %s: %d\n", names[mode], n);
        failures += n;
        MOQ_TEST_CHECK(h[0] == h[1]);
    }
    /* Keep the original failures named even in a shortened/random-seed run. */
    static const uint64_t regressions[] = {198, 204};
    for (size_t i = 0; i < 2; i++) {
        uint64_t h[2] = {0};
        for (int run = 0; run < 2; run++) {
            scen_alloc_t as = {0};
            moq_alloc_t alloc = {&as, sa_alloc, sa_realloc, sa_free};
            run_summary_t sum;
            int n = run_scenario(regressions[i], 50, &alloc, &sum, run, 50);
            MOQ_TEST_CHECK(n == 0);
            MOQ_TEST_CHECK(as.balance == 0);
            h[run] = sum.trace_hash;
            fnv_u64(&h[run], sum.chunk_hash);
            fnv_u64(&h[run], sum.violations);
            fnv_u64(&h[run], sum.finite_open);
        }
        MOQ_TEST_CHECK(h[0] == h[1]);
    }
    if (!failures)
        fprintf(stderr, "PASS: terminal controls (10 modes x2; seeds198/204 x2; alloc balance=0)\n");
    return failures;
}

typedef struct {
    uint8_t bytes[512];
    size_t length;
    bool bidi, fin;
    moq_stream_ref_t ref;
} oracle_response_t;

static bool oracle_hold_responses(moq_simpair_t *sp,
    oracle_response_t responses[8], size_t *count)
{
    moq_action_t a;
    while (moq_session_poll_actions(moq_simpair_server(sp), &a, 1)) {
        oracle_response_t r = {0};
        const uint8_t *bytes = NULL;
        bool supported = true;
        if (a.kind == MOQ_ACTION_SEND_CONTROL) {
            bytes = a.u.send_control.data;
            r.length = a.u.send_control.len;
        } else if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            bytes = a.u.send_bidi_stream.data;
            r.length = a.u.send_bidi_stream.len;
            r.bidi = true;
            r.fin = a.u.send_bidi_stream.fin;
            supported = false;
            for (size_t i = 0; i < MOQ_SIM_MAX_BIDI_STREAMS; i++) {
                const moq_sim_bidi_map_t *map = &sp->bidi_map[i];
                if (map->active && map->opener == MOQ_PERSPECTIVE_CLIENT &&
                    map->responder_ref == a.u.send_bidi_stream.stream_ref._v) {
                    r.ref = moq_stream_ref_from_u64(map->opener_ref);
                    supported = true;
                    break;
                }
            }
        } else supported = false;
        bool fits = r.length <= sizeof(r.bytes) && *count < 8;
        if (supported && fits) {
            memcpy(r.bytes, bytes, r.length);
            responses[(*count)++] = r;
        }
        moq_action_cleanup(&a);
        if (!supported || !fits) return false;
    }
    return true;
}

static int check_delayed_begin(bool delayed, int topology, int terminal,
    size_t target, uint64_t *hash_out)
{
    int failures = 0;
    scen_alloc_t as = {0};
    moq_alloc_t alloc = {&as, sa_alloc, sa_realloc, sa_free};
    run_summary_t sum = {0};
    sum.trace_hash = sum.chunk_hash = FNV_INIT;
    moq_simpair_t *sp = NULL;
    size_t streams = topology == 0 ? 1 : 2;
    size_t owners = topology == 2 ? 2 : 1;
#define DELAY_NEED(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL delayed=%d topology=%d terminal=%d target=%zu prerequisite: %s\n", \
            delayed, topology, terminal, target, #expr); \
    failures++; goto done; \
} } while (0)
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.version = scenario_selected_version(MOQ_VERSION_DRAFT_16);
    cfg.alloc = &alloc;
    cfg.initial_now_us = 1000;
    cfg.client_streaming_objects = true;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 8;
    DELAY_NEED(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    sum.client = c;
    DELAY_NEED(moq_simpair_start(sp) == MOQ_OK);
    DELAY_NEED(moq_simpair_run_until_quiescent(sp, 8, NULL) == MOQ_OK);
    DELAY_NEED(moq_session_version(c) == cfg.version && moq_session_version(sv) == cfg.version);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1)) moq_event_cleanup(&ev);
    while (moq_session_poll_events(sv, &ev, 1)) moq_event_cleanup(&ev);
    moq_subscription_t subs[2], server_subs[2];
    oracle_response_t responses[8] = {0};
    size_t response_count = 0;
    for (size_t i = 0; i < owners; i++) {
        moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("delayed")};
        moq_subscribe_cfg_t sub;
        moq_subscribe_cfg_init(&sub);
        sub.track_namespace = (moq_namespace_t){parts, 1};
        sub.track_name = i ? MOQ_BYTES_LITERAL("second") : MOQ_BYTES_LITERAL("first");
        DELAY_NEED(moq_session_subscribe(c, &sub, 1000, &subs[i]) == MOQ_OK);
        DELAY_NEED(moq_simpair_run_until_quiescent(sp, 8, NULL) == MOQ_OK);
        DELAY_NEED(moq_session_poll_events(sv, &ev, 1) == 1);
        bool request = ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST;
        server_subs[i] = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        DELAY_NEED(request);
        moq_accept_subscribe_cfg_t acc;
        moq_accept_subscribe_cfg_init(&acc);
        DELAY_NEED(moq_session_accept_subscribe(sv, server_subs[i], &acc, 1000) == MOQ_OK);
        DELAY_NEED(oracle_hold_responses(sp, responses, &response_count));
    }
    DELAY_NEED(response_count >= owners);
    uint8_t wire[2][256];
    size_t lengths[2] = {0};
    moq_stream_ref_t refs[2] = {moq_stream_ref_from_u64(9000), moq_stream_ref_from_u64(9017)};
    for (size_t i = 0; i < streams; i++) {
        moq_subgroup_cfg_t sg_cfg;
        moq_subgroup_cfg_init(&sg_cfg);
        sg_cfg.group_id = 3;
        sg_cfg.subgroup_id = topology == 1 ? i : 0;
        moq_subgroup_handle_t sg;
        DELAY_NEED(moq_session_open_subgroup(sv, server_subs[owners == 2 ? i : 0],
                   &sg_cfg, 1000, &sg) == MOQ_OK);
        moq_rcbuf_t *payload = NULL;
        DELAY_NEED(moq_rcbuf_create(&alloc, (const uint8_t *)"abcd", 4, &payload) == MOQ_OK);
        moq_result_t rc = moq_session_write_object(sv, sg, 0, payload, 1000);
        moq_rcbuf_decref(payload);
        DELAY_NEED(rc == MOQ_OK);
        moq_action_t a;
        while (moq_session_poll_actions(sv, &a, 1)) {
            bool fits = a.kind == MOQ_ACTION_SEND_DATA;
            if (fits) {
                size_t h = a.u.send_data.header_len, n = moq_rcbuf_len(a.u.send_data.payload);
                fits = lengths[i] + h + n <= sizeof(wire[i]);
                if (fits) {
                    memcpy(wire[i] + lengths[i], a.u.send_data.header, h);
                    lengths[i] += h;
                    if (n) memcpy(wire[i] + lengths[i], moq_rcbuf_data(a.u.send_data.payload), n);
                    lengths[i] += n;
                }
            }
            moq_action_cleanup(&a);
            DELAY_NEED(fits);
        }
        DELAY_NEED(lengths[i] > 4);
    }
    moq_sim_trace_record_t input = {0};
    input.struct_size = sizeof(input);
    input.kind = MOQ_SIM_TRACE_INPUT;
    input.from = MOQ_PERSPECTIVE_SERVER;
    input.to = MOQ_PERSPECTIVE_CLIENT;
    for (int phase = 0; phase < 2; phase++) {
        if ((phase == 0) == delayed) {
            /* Interleave split headers, then partial payloads in reverse order.
             * The last data ref cannot identify both later begin events. */
            for (int part = 0; part < 2; part++) for (size_t n = 0; n < streams; n++) {
                size_t i = part ? streams - n - 1 : n;
                input.input_kind = MOQ_SIM_INPUT_DATA_BYTES;
                input.stream_ref = refs[i];
                input.fin = false;
                input.result = moq_session_on_data_bytes(c, refs[i],
                    wire[i] + (part ? 1 : 0), part ? lengths[i] - 4 : 1, false, 1000);
                DELAY_NEED(input.result == MOQ_OK);
                oracle_after_input(&sum, &input, false);
                if (delayed) DELAY_NEED(sum.chunks == 0);
            }
        } else {
            for (size_t i = 0; i < response_count; i++) {
                const oracle_response_t *r = &responses[i];
                input.input_kind = r->bidi ? MOQ_SIM_INPUT_BIDI_BYTES : MOQ_SIM_INPUT_CONTROL_BYTES;
                input.stream_ref = r->ref;
                input.fin = r->fin;
                input.result = r->bidi ?
                    moq_session_on_bidi_stream_bytes(c, r->ref, r->bytes, r->length, r->fin, 1000) :
                    moq_session_on_control_bytes(c, r->bytes, r->length, 1000);
                DELAY_NEED(input.result == MOQ_OK);
                oracle_after_input(&sum, &input, false);
            }
        }
    }
    DELAY_NEED(sum.chunks == streams && sum.complete == 0 && sum.finite_open == streams);
    MOQ_TEST_CHECK(sum.violations == 0);
    for (size_t i = 0; i < streams; i++) {
        const oracle_object_t *o = oracle_find(&sum, subs[owners == 2 ? i : 0],
            MOQ_PUBLICATION_INVALID, 3, topology == 1 ? i : 0);
        MOQ_TEST_CHECK(o && o->has_ref && o->ref._v == refs[i]._v);
    }
    bool suppress = terminal == 2 || terminal == 4;
    if (terminal == 1 || terminal == 2) {
        input.input_kind = MOQ_SIM_INPUT_DATA_RESET;
        input.stream_ref = refs[target];
        input.error_code = 0x77;
        input.result = moq_session_on_data_reset(c, input.stream_ref, input.error_code, 1000);
        DELAY_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, suppress);
        DELAY_NEED(sum.delivered_reset == 1);
        MOQ_TEST_CHECK(sum.reset_terminals == (suppress ? 0u : 1u));
        if (!suppress) {
            input.result = moq_session_on_data_reset(c, input.stream_ref, input.error_code, 1000);
            DELAY_NEED(input.result == MOQ_OK);
            oracle_after_input(&sum, &input, false);
            MOQ_TEST_CHECK(sum.reset_terminals == 1 && sum.complete == 1);
        }
    } else if (terminal == 3 || terminal == 4) {
        input.input_kind = MOQ_SIM_INPUT_DATA_BYTES;
        input.stream_ref = refs[target];
        input.fin = true;
        input.result = moq_session_on_data_bytes(c, refs[target], wire[target] + lengths[target] - 3,
            3, true, 1000);
        DELAY_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, suppress);
        DELAY_NEED(sum.delivered_fin == 1);
    } else if (terminal == 5) {
        const uint8_t unknown[] = {cfg.version == MOQ_VERSION_DRAFT_16 ? 0x40 : 0x80, 0xff, 0, 0};
        input.input_kind = MOQ_SIM_INPUT_CONTROL_BYTES;
        input.result = moq_session_on_control_bytes(c, unknown, sizeof(unknown), 1000);
        DELAY_NEED(input.result == MOQ_OK);
        oracle_after_input(&sum, &input, false);
        DELAY_NEED(moq_session_state(c) == MOQ_SESS_CLOSED && sum.closed == 1);
    }
    oracle_finish(&sum);
    MOQ_TEST_CHECK(suppress ? sum.violations > 0 : sum.violations == 0);
    MOQ_TEST_CHECK(sum.complete == ((terminal == 1 || terminal == 3) ? 1u : 0u));
    MOQ_TEST_CHECK(sum.finite_open == (terminal == 5 ? 0 : streams - sum.complete));
    MOQ_TEST_CHECK(sum.f_inject_close == 0);
    if (terminal != 5)
        DELAY_NEED(moq_session_state(c) == MOQ_SESS_ESTABLISHED && moq_session_state(sv) == MOQ_SESS_ESTABLISHED);
    if (streams == 2 && terminal != 5) {
        size_t other = 1 - target;
        const oracle_object_t *o = oracle_find(&sum, subs[owners == 2 ? other : 0],
            MOQ_PUBLICATION_INVALID, 3, topology == 1 ? other : 0);
        MOQ_TEST_CHECK(o && o->has_ref && o->ref._v == refs[other]._v);
    }
done:
    if (sp) moq_simpair_destroy(sp);
    MOQ_TEST_CHECK(as.balance == 0);
    *hash_out = sum.chunk_hash;
    fnv_u64(hash_out, sum.violations);
    fnv_u64(hash_out, sum.finite_open);
    fnv_u64(hash_out, sum.delivered_fin);
    fnv_u64(hash_out, sum.delivered_reset);
    return failures;
#undef DELAY_NEED
}

static int check_oracle_inventory(void)
{
    int failures = 0;
    run_summary_t sum = {0};
    moq_rx_stream_t rx[ORACLE_OBJECTS] = {0};
    moq_session_t c = {0};
    c.rx_streams = rx;
    c.rx_cap = ORACLE_OBJECTS;
    moq_object_chunk_event_t chunk = {0};
    chunk.sub._opaque = moq_handle_pack(MOQ_HANDLE_POOL_SUBSCRIPTION, 1, 1, 0);
    chunk.begin = true;
    chunk.payload_length = 4;
    for (size_t i = 0; i < ORACLE_OBJECTS; i++) {
        rx[i].active = true;
        rx[i].stream_kind = MOQ_STREAM_KIND_SUBGROUP;
        rx[i].subgroup_id_resolved = true;
        rx[i].group_id = chunk.group_id = i;
        rx[i].sub = chunk.sub;
        rx[i].stream_ref = moq_stream_ref_from_u64(100 + i);
        oracle_observe(&sum, &chunk);
    }
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.finite_open == ORACLE_OBJECTS && sum.violations == 0);
    for (size_t i = 0; i < ORACLE_OBJECTS; i++)
        MOQ_TEST_CHECK(sum.objects[i].has_ref && sum.objects[i].ref._v == 100 + i);
    chunk.group_id = ORACLE_OBJECTS;
    oracle_observe(&sum, &chunk);
    MOQ_TEST_CHECK(sum.finite_open == ORACLE_OBJECTS && sum.violations == 1);
    chunk.group_id = 0;
    chunk.begin = false;
    chunk.end = true;
    chunk.terminal = MOQ_OBJECT_TERMINAL_RESET;
    oracle_observe(&sum, &chunk);
    MOQ_TEST_CHECK(sum.finite_open == ORACLE_OBJECTS - 1 && sum.violations == 1);
    oracle_observe(&sum, &chunk);
    MOQ_TEST_CHECK(sum.violations == 2);
    chunk.sub._opaque = moq_handle_pack(MOQ_HANDLE_POOL_SUBSCRIPTION, 1, 3, 0);
    chunk.begin = true;
    chunk.end = false;
    oracle_observe(&sum, &chunk);
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.violations == 3 && !sum.objects[0].has_ref);
    rx[0].sub = chunk.sub;
    rx[0].stream_ref = moq_stream_ref_from_u64(500);
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.violations == 3 && sum.objects[0].has_ref && sum.objects[0].ref._v == 500);
    /* A retired/reused core receive slot must not rewrite existing provenance. */
    rx[0].stream_ref = moq_stream_ref_from_u64(600);
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.objects[0].ref._v == 500 && sum.violations == 3);
    oracle_finish(&sum);
    MOQ_TEST_CHECK(sum.finite_open == ORACLE_OBJECTS && sum.violations == 3);
    /* Fresh object with two matching receive records is ambiguous, not mapped. */
    memset(&sum, 0, sizeof(sum));
    rx[1] = rx[0];
    rx[1].stream_ref = moq_stream_ref_from_u64(700);
    oracle_observe(&sum, &chunk);
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.violations == 1 && !sum.objects[0].has_ref);
    /* Shared-alias fanout also keys the full publication generation. */
    memset(&sum, 0, sizeof(sum));
    memset(rx, 0, sizeof(rx));
    moq_rx_fan_t fan = {0};
    chunk.sub = MOQ_SUBSCRIPTION_INVALID;
    chunk.pub._opaque = moq_handle_pack(MOQ_HANDLE_POOL_PUBLICATION, 1, 3, 0);
    fan.pub._opaque = moq_handle_pack(MOQ_HANDLE_POOL_PUBLICATION, 1, 1, 0);
    rx[0].active = rx[0].subgroup_id_resolved = rx[0].fan_shared = true;
    rx[0].stream_kind = MOQ_STREAM_KIND_SUBGROUP;
    rx[0].fan = &fan;
    rx[0].fan_count = 1;
    rx[0].stream_ref = moq_stream_ref_from_u64(800);
    oracle_observe(&sum, &chunk);
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.violations == 1 && !sum.objects[0].has_ref);
    fan.pub = chunk.pub;
    oracle_bind_streams(&c, &sum);
    MOQ_TEST_CHECK(sum.violations == 1 && sum.objects[0].has_ref && sum.objects[0].ref._v == 800);
    return failures;
}

static int check_delayed_controls(void)
{
    int failures = check_oracle_inventory();
    for (int delayed = 0; delayed < 2; delayed++)
        for (int topology = 0; topology < 3; topology++)
            for (int terminal = 0; terminal < 6; terminal++)
                for (size_t target = 0; target < (topology == 0 ? 1u : 2u); target++) {
                    uint64_t h[2] = {0};
                    failures += check_delayed_begin(delayed != 0, topology, terminal, target, &h[0]);
                    failures += check_delayed_begin(delayed != 0, topology, terminal, target, &h[1]);
                    MOQ_TEST_CHECK(h[0] == h[1]);
                }
    if (!failures)
        fprintf(stderr, "PASS: delayed-begin controls (60 cases x2; inventory32; alloc balance=0)\n");
    return failures;
}

/* -- Main ----------------------------------------------------------- */

int main(void)
{
    if (scenario_selected_version(MOQ_VERSION_DRAFT_16) == 0) return 1;
    int failures = check_terminal_controls();
    failures += check_delayed_controls();
    const char *env_seeds = getenv("MOQ_SCENARIO_SEEDS");
    const char *env_start = getenv("MOQ_SCENARIO_SEED_START");
    const char *env_steps = getenv("MOQ_SCENARIO_STEPS");
    const char *env_fault = getenv("MOQ_SCENARIO_FAULT_PERMILLE");

    uint64_t num_seeds = env_seeds ? (uint64_t)strtoull(env_seeds, NULL, 0) : 100;
    uint64_t seed_start = env_start ? (uint64_t)strtoull(env_start, NULL, 0) : 0;
    int max_steps = env_steps ? atoi(env_steps) : 50;
    uint64_t per_mille = env_fault ? (uint64_t)strtoull(env_fault, NULL, 0) : 50;
    if (per_mille > 1000) per_mille = 1000;

    size_t t_chunks = 0, t_complete = 0, t_violations = 0, t_closed = 0, t_rst_term = 0;
    size_t t_data_inputs = 0, t_drop = 0, t_mutate = 0, t_reorder = 0;
    size_t t_inj_rst = 0, t_inj_stp = 0, t_inj_cls = 0;
    size_t t_trunc_ctl = 0, t_trunc_data = 0;
    size_t t_pre = 0;

    for (uint64_t seed = seed_start; seed < seed_start + num_seeds; seed++) {
        run_summary_t sums[2];

        for (int run = 0; run < 2; run++) {
            scen_alloc_t as = {0};
            moq_alloc_t alloc = { &as, sa_alloc, sa_realloc, sa_free };
            failures += run_scenario(seed, per_mille, &alloc,
                                      &sums[run], run, max_steps);
            if (as.balance != 0) {
                fprintf(stderr, "FAIL seed=0x%llx run=%d: "
                        "alloc balance=%lld\n",
                        (unsigned long long)seed, run,
                        (long long)as.balance);
                fprintf(stderr, "  Replay: MOQ_SCENARIO_SEED_START=0x%llx "
                        "MOQ_SCENARIO_SEEDS=1 MOQ_SCENARIO_STEPS=%d "
                        "MOQ_SCENARIO_FAULT_PERMILLE=%llu "
                        "./build/tests/test_scenario_streaming_faults\n",
                        (unsigned long long)seed, max_steps,
                        (unsigned long long)per_mille);
                failures++;
            }
        }

        /* Determinism. */
        if (sums[0].trace_hash != sums[1].trace_hash ||
            sums[0].trace_count != sums[1].trace_count ||
            sums[0].chunks != sums[1].chunks ||
            sums[0].complete != sums[1].complete ||
            sums[0].violations != sums[1].violations ||
            sums[0].chunk_hash != sums[1].chunk_hash ||
            sums[0].reset_terminals != sums[1].reset_terminals ||
            sums[0].closed != sums[1].closed ||
            sums[0].finite_open != sums[1].finite_open ||
            sums[0].delivered_fin != sums[1].delivered_fin ||
            sums[0].delivered_reset != sums[1].delivered_reset ||
            sums[0].data_inputs != sums[1].data_inputs ||
            sums[0].f_drop != sums[1].f_drop ||
            sums[0].f_mutate != sums[1].f_mutate ||
            sums[0].f_reorder != sums[1].f_reorder ||
            sums[0].f_inject_reset != sums[1].f_inject_reset ||
            sums[0].f_inject_stop != sums[1].f_inject_stop ||
            sums[0].f_inject_close != sums[1].f_inject_close ||
            sums[0].f_truncate_control != sums[1].f_truncate_control ||
            sums[0].f_truncate_data != sums[1].f_truncate_data ||
            sums[0].pre_objects != sums[1].pre_objects ||
            sums[0].pre_chunks != sums[1].pre_chunks ||
            sums[0].pre_payload_hash != sums[1].pre_payload_hash) {
            fprintf(stderr, "FAIL seed=0x%llx: determinism mismatch\n",
                    (unsigned long long)seed);
            fprintf(stderr, "  Replay: MOQ_SCENARIO_SEED_START=0x%llx "
                    "MOQ_SCENARIO_SEEDS=1 MOQ_SCENARIO_STEPS=%d "
                    "MOQ_SCENARIO_FAULT_PERMILLE=%llu "
                    "./build/tests/test_scenario_streaming_faults\n",
                    (unsigned long long)seed, max_steps,
                    (unsigned long long)per_mille);
            failures++;
        }

        t_chunks += sums[0].chunks;
        t_complete += sums[0].complete;
        t_violations += sums[0].violations;
        t_closed += sums[0].closed;
        t_rst_term += sums[0].reset_terminals;
        t_data_inputs += sums[0].data_inputs;
        t_drop += sums[0].f_drop;
        t_mutate += sums[0].f_mutate;
        t_reorder += sums[0].f_reorder;
        t_inj_rst += sums[0].f_inject_reset;
        t_inj_stp += sums[0].f_inject_stop;
        t_inj_cls += sums[0].f_inject_close;
        t_trunc_ctl += sums[0].f_truncate_control;
        t_trunc_data += sums[0].f_truncate_data;
        t_pre += sums[0].pre_objects;
    }

    if (t_violations > 0) {
        fprintf(stderr, "FAIL: %zu total chunk oracle violations across "
                "%llu seeds\n", t_violations, (unsigned long long)num_seeds);
        failures++;
    }

    if (t_pre != num_seeds) {
        fprintf(stderr, "FAIL: prelude=%zu expected=%llu\n",
                t_pre, (unsigned long long)num_seeds);
        failures++;
    }

    if (per_mille > 0 && num_seeds >= 10 &&
        t_drop + t_mutate + t_reorder +
        t_inj_rst + t_inj_stp + t_inj_cls +
        t_trunc_ctl + t_trunc_data == 0) {
        fprintf(stderr, "FAIL: no data faults across %llu seeds\n",
                (unsigned long long)num_seeds);
        failures++;
    }
    if (per_mille > 0 && num_seeds >= 10 &&
        t_trunc_ctl + t_trunc_data == 0) {
        fprintf(stderr, "FAIL: no truncation faults across %llu seeds\n",
                (unsigned long long)num_seeds);
        failures++;
    }

    /* rst_term depends on inject timing vs. parser state. SimPair's
     * inject phase fires between pump rounds, so small objects usually
     * complete before inject lands. The unit tests verify the terminal
     * path; the scenario validates no oracle corruption either way. */

    if (failures == 0)
        fprintf(stderr, "PASS: test_scenario_streaming_faults "
                "(%llu seeds, prelude=%zu chunks=%zu complete=%zu "
                "closed=%zu violations=%zu rst_term=%zu "
                "data_in=%zu drop=%zu mutate=%zu reord=%zu "
                "inj_rst=%zu inj_stp=%zu inj_cls=%zu "
                "trunc_ctl=%zu trunc_data=%zu)\n",
                (unsigned long long)num_seeds,
                t_pre, t_chunks, t_complete, t_closed, t_violations,
                t_rst_term,
                t_data_inputs, t_drop, t_mutate, t_reorder,
                t_inj_rst, t_inj_stp, t_inj_cls,
                t_trunc_ctl, t_trunc_data);
    else
        fprintf(stderr, "FAIL: test_scenario_streaming_faults "
                "(%d failures in %llu seeds)\n",
                failures, (unsigned long long)num_seeds);

    return failures ? 1 : 0;
}
