#ifndef SCENARIO_ACTION_TRANSPORT_H
#define SCENARIO_ACTION_TRANSPORT_H

#include "scenario_pair_version.h"

static const char *scenario_runner;

/* Request bytes stay opaque. Each endpoint has its own transport ref; retain
 * both halves (including closed entries) for late FIN/reset/stop actions. This
 * bounded fixture deliberately fails if a longer scenario exhausts its map. */
#define SCENARIO_REQUEST_STREAMS 128
typedef struct {
    uint64_t ref[2];
    bool fin[2];
} scenario_request_stream_t;

typedef struct {
    scenario_request_stream_t requests[SCENARIO_REQUEST_STREAMS];
    size_t count;
    void (*drain_client)(moq_session_t *, void *);
    void *drain_ctx;
    size_t retries;
} scenario_action_transport_t;

static inline bool scenario_drain_blocked(scenario_action_transport_t *t,
        moq_session_t *to, moq_result_t rc, unsigned *attempts)
{
    if (rc != MOQ_ERR_WOULD_BLOCK || !t->drain_client ||
        moq_session_perspective(to) != MOQ_PERSPECTIVE_CLIENT ||
        (*attempts)++ >= 256) return false;
    t->retries++;
    t->drain_client(to, t->drain_ctx);
    return true;
}

static inline moq_result_t scenario_control_input(scenario_action_transport_t *t,
        moq_session_t *to, const uint8_t *data, size_t len, uint64_t now)
{
    moq_result_t rc = moq_session_on_control_bytes(to, data, len, now);
    unsigned attempts = 0;
    while (scenario_drain_blocked(t, to, rc, &attempts))
        rc = moq_session_on_control_bytes(to, NULL, 0, now);
    return rc;
}

static inline moq_result_t scenario_route_action_impl(scenario_action_transport_t *t,
        moq_session_t *from, moq_session_t *to, const moq_action_t *a,
        uint64_t now)
{
    unsigned side = moq_session_perspective(from) == MOQ_PERSPECTIVE_CLIENT ? 0 : 1;
    uint64_t ref = 0, code = 0;
    const uint8_t *bytes = NULL;
    size_t len = 0;
    bool fin = false, reset = false, stop = false;
    switch (a->kind) {
    case MOQ_ACTION_SEND_CONTROL:
        return scenario_control_input(t, to, a->u.send_control.data,
                                            a->u.send_control.len, now);
    case MOQ_ACTION_OPEN_UNI_CONTROL:
        return scenario_control_input(t, to, a->u.open_uni_control.data,
                                            a->u.open_uni_control.len, now);
    case MOQ_ACTION_SEND_UNI_CONTROL:
        if (a->u.send_uni_control.fin) {
            fprintf(stderr, "FAIL action-transport: unexpected control FIN\n");
            return MOQ_ERR_INTERNAL;
        }
        return scenario_control_input(t, to, a->u.send_uni_control.data,
                                            a->u.send_uni_control.len, now);
    case MOQ_ACTION_OPEN_BIDI_STREAM:
        ref = a->u.open_bidi_stream.stream_ref._v;
        bytes = a->u.open_bidi_stream.data;
        len = a->u.open_bidi_stream.len;
        fin = a->u.open_bidi_stream.fin;
        break;
    case MOQ_ACTION_SEND_BIDI_STREAM:
        ref = a->u.send_bidi_stream.stream_ref._v;
        bytes = a->u.send_bidi_stream.data;
        len = a->u.send_bidi_stream.len;
        fin = a->u.send_bidi_stream.fin;
        break;
    case MOQ_ACTION_CLOSE_BIDI_STREAM:
        ref = a->u.close_bidi_stream.stream_ref._v;
        fin = true;
        break;
    case MOQ_ACTION_RESET_BIDI_STREAM:
        ref = a->u.reset_bidi_stream.stream_ref._v;
        code = a->u.reset_bidi_stream.error_code;
        reset = true;
        break;
    case MOQ_ACTION_STOP_BIDI_STREAM:
        ref = a->u.stop_bidi_stream.stream_ref._v;
        code = a->u.stop_bidi_stream.error_code;
        stop = true;
        break;
    case MOQ_ACTION_ABORT_BIDI_STREAM:
        ref = a->u.abort_bidi_stream.stream_ref._v;
        code = a->u.abort_bidi_stream.error_code;
        reset = stop = true;
        break;
    case MOQ_ACTION_CLOSE_SESSION:
        return moq_session_on_transport_close(to, a->u.close_session.code, now);
    default:
        fprintf(stderr, "FAIL action-transport: unsupported action %u\n", a->kind);
        return MOQ_ERR_INTERNAL;
    }

    scenario_request_stream_t *m = NULL;
    for (size_t i = 0; i < t->count; i++)
        if (t->requests[i].ref[side] == ref) { m = &t->requests[i]; break; }
    if (a->kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        if (m || t->count == SCENARIO_REQUEST_STREAMS) {
            fprintf(stderr, "FAIL action-transport: duplicate open or full request map\n");
            return MOQ_ERR_INTERNAL;
        }
        m = &t->requests[t->count++];
        m->ref[side] = ref;
        m->ref[1 - side] = UINT64_C(0x100000000) + t->count;
    }
    if (!m) {
        fprintf(stderr, "FAIL action-transport: unmapped action %u side=%u ref=%llu\n",
                a->kind, side, (unsigned long long)ref);
        return MOQ_ERR_INTERNAL;
    }

    moq_stream_ref_t peer_ref = moq_stream_ref_from_u64(m->ref[1 - side]);
    moq_result_t rc = MOQ_OK;
    unsigned attempts = 0;
    if (reset || stop) {
        if (reset) {
            rc = moq_session_on_bidi_stream_reset(to, peer_ref, code, now);
            while (scenario_drain_blocked(t, to, rc, &attempts))
                rc = moq_session_on_bidi_stream_reset(to, peer_ref, code, now);
            if (rc != MOQ_OK) return rc;
            m->fin[side] = true;
        }
        if (stop) {
            rc = moq_session_on_bidi_stream_stop(to, peer_ref, code, now);
            while (scenario_drain_blocked(t, to, rc, &attempts))
                rc = moq_session_on_bidi_stream_stop(to, peer_ref, code, now);
        }
        return rc;
    }
    if (m->fin[side]) {
        if (fin && len == 0) return MOQ_OK;
        fprintf(stderr, "FAIL action-transport: bytes after request FIN\n");
        return MOQ_ERR_INTERNAL;
    }
    rc = moq_session_on_bidi_stream_bytes(to, peer_ref, bytes, len, fin, now);
    while (scenario_drain_blocked(t, to, rc, &attempts))
        rc = moq_session_on_bidi_stream_bytes(to, peer_ref, NULL, 0, false, now);
    if (rc == MOQ_OK && fin) m->fin[side] = true;
    return rc;
}

static inline moq_result_t scenario_route_action(scenario_action_transport_t *t,
        moq_session_t *from, moq_session_t *to, const moq_action_t *a,
        uint64_t now)
{
    moq_result_t rc = scenario_route_action_impl(t, from, to, a, now);
    if (rc != MOQ_OK)
        fprintf(stderr, "FAIL action-transport: kind=%u from=%u rc=%d\n",
                a->kind, moq_session_perspective(from), rc);
    return rc;
}

static inline bool scenario_profile_matches(moq_session_t *client,
                                             moq_session_t *server)
{
    moq_version_t selected = scenario_selected_version(MOQ_VERSION_DRAFT_16);
    return moq_session_version(client) == selected &&
           moq_session_version(server) == selected;
}

static inline unsigned scenario_profile_number(void)
{
    switch (scenario_selected_version(MOQ_VERSION_DRAFT_16)) {
    case MOQ_VERSION_DRAFT_18: return 18;
    case MOQ_VERSION_DRAFT_22: return 22;
    default: return 16;
    }
}

static inline void scenario_replay(uint64_t start, uint64_t seeds, int steps)
{
    fprintf(stderr, "  Replay: MOQ_SIM_VERSION=%u MOQ_SCENARIO_SEED_START=0x%llx "
            "MOQ_SCENARIO_SEEDS=%llu MOQ_SCENARIO_STEPS=%d %s\n",
            scenario_profile_number(), (unsigned long long)start,
            (unsigned long long)seeds, steps, scenario_runner);
}

static inline int scenario_random_delivery_floor(uint64_t start, uint64_t seeds,
        int steps, size_t sent, size_t received)
{
    /* The quick budget is a deterministic coverage gate; a one-seed replay
     * need not randomly open, accept and deliver an object. */
    if (start == 0 && seeds >= 1000 && steps >= 50 && received < 100) {
        fprintf(stderr, "FAIL random-delivery-floor: D%u random=%zu/%zu "
                "expected >=100 received\n", scenario_profile_number(),
                sent, received);
        scenario_replay(start, seeds, steps);
        return 1;
    }
    return 0;
}

#endif
