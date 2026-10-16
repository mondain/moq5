/* Deterministic receive-backpressure runner, plus namespace response pressure.
 *
 * The four receive rows give the receiving session a small event queue and drain it
 * only between transport passes, so the session refuses an input with
 * MOQ_ERR_WOULD_BLOCK while the publisher has more queued behind it. Every
 * receive row pins that the refusal happened (the trace records an input of the
 * row's kind answered WOULD_BLOCK) and that it cost nothing it may not:
 *
 *   control         GOAWAY reaches a client whose queue is full. The control
 *                   bytes are retained and resumed; the objects written
 *                   before and after it all surface, and so does GOAWAY. On
 *                   draft-18/22 (a unidirectional control pair) a blocked
 *                   control channel pauses the other unidirectional inputs:
 *                   no data input reaches the client while it is owed, and
 *                   the objects written after GOAWAY surface after it.
 *   responses       six SUBSCRIBE_OKs and an object answered in one pass:
 *                   on draft-16 they travel on the control channel, on
 *                   draft-18/22 on request streams. Every response and the
 *                   object surface.
 *   datagram        six object datagrams, then an object on a stream, in one
 *                   pass. A refused datagram is dropped (datagrams are
 *                   unreliable); the stream object behind it surfaces.
 *   unadmitted      the client admits one receive stream at a time and two
 *                   subgroups arrive interleaved. The second stream is
 *                   refused before the session owns it, delivered again
 *                   once the first ends, and both surface in full.
 *   namespace_accept (18/22) a full response queue refuses acceptance without
 *                   committing it; draining permits one response on that ref.
 *
 * Each row also pins that no session closes and that run_until_quiescent
 * never fails. Each row runs twice; the trace hash and counters must match.
 *
 * MOQ_SIM_VERSION selects one profile; without it every profile runs.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIX(h, v) do { (h) ^= (uint64_t)(v); (h) *= 0x100000001B3ULL; } while (0)

enum { MAX_TRACKS = 8, MAX_STREAMS = 2, MAX_OBJECTS = 3, DATAGRAMS = 6 };

typedef struct {
    size_t track, count, payload_len;
    uint64_t group;
    uint8_t payload[MAX_OBJECTS][3];
    bool seen[MAX_OBJECTS], finished;
} reliable_t;

typedef struct {
    uint64_t trace;
    size_t   refused[16];        /* WOULD_BLOCK inputs by sim input kind */
    size_t   objects, finished, sub_oks, goaways, closed, datagram_objects;
    size_t   run_failures;       /* run_until_quiescent returned < 0 */
    size_t   events;             /* client events, in surfacing order */
    size_t   goaway_at, first_g2_at;   /* 1-based event positions, 0 = never */
    bool     client_ctrl_blocked;      /* the client's control input is owed */
    size_t   data_while_ctrl_blocked;  /* client data inputs meanwhile */
    moq_subscription_t csubs[MAX_TRACKS], ssubs[MAX_TRACKS];
    bool requests[MAX_TRACKS], accepted[MAX_TRACKS];
    size_t ntracks, nssubs;
    reliable_t reliable[MAX_STREAMS];
    size_t nreliable;
    bool allow_datagrams, datagrams[DATAGRAMS];
    size_t namespace_refusals, namespace_responses;
    int errors;
} tally_t;

static bool check(tally_t *t, bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "  FAIL: %s\n", what);
        t->errors++;
    }
    return ok;
}

static bool expect_stream(tally_t *t, size_t track, uint64_t group,
                           size_t count, size_t payload_len)
{
    if (!check(t, t->nreliable < MAX_STREAMS && track < t->ntracks &&
        count > 0 && count <= MAX_OBJECTS && (payload_len == 2 || payload_len == 3),
        "expected.stream.bounds")) return false;
    reliable_t *r = &t->reliable[t->nreliable++];
    r->track = track; r->group = group; r->count = count; r->payload_len = payload_len;
    for (size_t i = 0; i < count; i++) {
        r->payload[i][0] = (uint8_t)group;
        r->payload[i][1] = (uint8_t)i;
        r->payload[i][2] = 0x5A;
    }
    return true;
}

static reliable_t *reliable_stream(tally_t *t, moq_subscription_t sub,
                                    moq_publication_t pub, uint64_t group,
                                    uint64_t subgroup, bool end_of_group)
{
    if (!check(t, !moq_publication_is_valid(pub) && subgroup == 0 && !end_of_group,
               "reliable.stream.metadata")) return NULL;
    for (size_t i = 0; i < t->nreliable; i++) {
        reliable_t *r = &t->reliable[i];
        if (r->group == group && moq_subscription_eq(t->csubs[r->track], sub)) return r;
    }
    check(t, false, "reliable.track.group.identity");
    return NULL;
}

static void observe_object(tally_t *t, const moq_object_received_event_t *o)
{
    check(t, o->status == MOQ_OBJECT_NORMAL, "object.normal.status");
    if (o->datagram) {
        t->datagram_objects++;
        if (!check(t, t->allow_datagrams && t->ntracks == 1 &&
            moq_subscription_eq(o->sub, t->csubs[0]) && !moq_publication_is_valid(o->pub) &&
            o->group_id == 9 && o->object_id < DATAGRAMS && !o->end_of_group,
            "datagram.identity")) return;
        size_t id = (size_t)o->object_id;
        const uint8_t expected[] = {9, (uint8_t)id};
        check(t, !t->datagrams[id], "datagram.once");
        check(t, o->payload && moq_rcbuf_len(o->payload) == sizeof(expected) &&
            !memcmp(moq_rcbuf_data(o->payload), expected, sizeof(expected)), "datagram.payload");
        t->datagrams[id] = true;
        return;
    }
    reliable_t *r = reliable_stream(t, o->sub, o->pub, o->group_id,
                                     o->subgroup_id, o->end_of_group);
    if (!r || !check(t, o->object_id < r->count, "reliable.object.identity")) return;
    size_t id = (size_t)o->object_id;
    check(t, !r->finished, "reliable.object.before.completion");
    check(t, !r->seen[id], "reliable.object.once");
    check(t, o->payload && moq_rcbuf_len(o->payload) == r->payload_len &&
        !memcmp(moq_rcbuf_data(o->payload), r->payload[id], r->payload_len), "reliable.payload");
    r->seen[id] = true;
}

static void observe_finished(tally_t *t, const moq_subgroup_finished_event_t *f)
{
    reliable_t *r = reliable_stream(t, f->sub, f->pub, f->group_id,
                                     f->subgroup_id, f->end_of_group);
    if (!r) return;
    check(t, !r->finished, "reliable.completion.once");
    for (size_t i = 0; i < r->count; i++)
        check(t, r->seen[i], "reliable.completion.after.objects");
    r->finished = true;
}

static void trace_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    tally_t *t = (tally_t *)ctx;
    uint64_t h = t->trace;
    MIX(h, r->step); MIX(h, r->now_us); MIX(h, r->kind); MIX(h, r->from);
    MIX(h, r->action_kind); MIX(h, r->input_kind); MIX(h, r->result);
    MIX(h, r->bytes.len);
    for (size_t i = 0; r->bytes.data && i < r->bytes.len; i++) MIX(h, r->bytes.data[i]);
    t->trace = h;
    if (r->kind == MOQ_SIM_TRACE_INPUT && r->result == MOQ_ERR_WOULD_BLOCK &&
        (unsigned)r->input_kind < 16)
        t->refused[r->input_kind]++;
    if (r->kind == MOQ_SIM_TRACE_INPUT && r->to == MOQ_PERSPECTIVE_CLIENT) {
        if (r->input_kind == MOQ_SIM_INPUT_CONTROL_BYTES)
            t->client_ctrl_blocked = r->result == MOQ_ERR_WOULD_BLOCK;
        else if (r->input_kind == MOQ_SIM_INPUT_DATA_BYTES && t->client_ctrl_blocked)
            t->data_while_ctrl_blocked++;
    }
}

static void observe_event(tally_t *t, const moq_event_t *ev, bool client)
{
    if (client) t->events++;
    switch (ev->kind) {
        case MOQ_EVENT_OBJECT_RECEIVED:
            check(t, client, "object.client.only");
            if (client) {
                t->objects++;
                if (ev->u.object_received.group_id == 2 && !t->first_g2_at)
                    t->first_g2_at = t->events;
                observe_object(t, &ev->u.object_received);
            }
            break;
        case MOQ_EVENT_SUBGROUP_FINISHED:
            check(t, client, "completion.client.only");
            if (client) { t->finished++; observe_finished(t, &ev->u.subgroup_finished); }
            break;
        case MOQ_EVENT_SUBSCRIBE_OK: {
            size_t i;
            for (i = 0; i < t->ntracks; i++)
                if (moq_subscription_eq(t->csubs[i], ev->u.subscribe_ok.sub)) break;
            if (check(t, client && i < t->ntracks, "subscribe_ok.track.identity")) {
                check(t, !t->accepted[i], "subscribe_ok.once");
                t->accepted[i] = true;
            }
            if (client) t->sub_oks++;
            break;
        }
        case MOQ_EVENT_GOAWAY:
            if (client) { t->goaways++; t->goaway_at = t->events; }
            break;
        case MOQ_EVENT_SUBSCRIBE_REQUEST: {
            const moq_subscribe_request_event_t *r = &ev->u.subscribe_request;
            if (!check(t, !client && r->track_namespace.count == 1 && r->track_namespace.parts &&
                r->track_namespace.parts[0].len == 4 && r->track_namespace.parts[0].data &&
                !memcmp(r->track_namespace.parts[0].data, "live", 4) &&
                r->track_name.len == 1 && r->track_name.data &&
                r->track_name.data[0] >= 'a' &&
                (size_t)(r->track_name.data[0] - 'a') < t->ntracks &&
                moq_subscription_is_valid(r->sub), "subscribe.request.track.identity")) break;
            size_t i = (size_t)(r->track_name.data[0] - 'a');
            check(t, !t->requests[i], "subscribe.request.once");
            t->requests[i] = true;
            t->ssubs[i] = r->sub;
            t->nssubs++;
            break;
        }
        case MOQ_EVENT_SESSION_CLOSED: t->closed++; break;
        case MOQ_EVENT_SETUP_COMPLETE:
        case MOQ_EVENT_REQUEST_READY: break;
        default: check(t, false, "event.unexpected"); break;
    }
}

static void drain(moq_session_t *s, tally_t *t, bool client)
{
    for (;;) {
        moq_event_t ev;
        size_t n = 0;
        if (!check(t, moq_session_poll_events_ex(s, &ev, 1, sizeof(ev), &n) == MOQ_OK,
                   "event.poll")) {
            if (n) moq_event_cleanup(&ev);
            break;
        }
        if (!n) break;
        observe_event(t, &ev, client);
        moq_event_cleanup(&ev);
    }
}

static void pass(moq_simpair_t *sp, tally_t *t)
{
    if (moq_simpair_run_until_quiescent(sp, 64, NULL) < 0) t->run_failures++;
}

static void settle(moq_simpair_t *sp, tally_t *t)
{
    for (int i = 0; i < 24; i++) {
        pass(sp, t);
        drain(moq_simpair_client(sp), t, true);
        drain(moq_simpair_server(sp), t, false);
    }
}

static moq_simpair_t *make_pair(moq_version_t v, uint32_t max_events,
                                uint32_t client_max_data_streams, uint32_t max_actions,
                                tally_t *t)
{
    memset(t, 0, sizeof(*t));
    t->trace = 0xCBF29CE484222325ULL;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x13AB;
    cfg.version = v;
    cfg.max_events = max_events;
    cfg.max_actions = max_actions;
    cfg.client_max_data_streams = client_max_data_streams;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.trace_fn = trace_fn;
    cfg.trace_ctx = t;
    moq_simpair_t *sp = NULL;
    if (!check(t, moq_simpair_create(&cfg, &sp) == MOQ_OK && sp, "pair.create") ||
        !check(t, moq_simpair_start(sp) == MOQ_OK, "pair.start")) {
        if (sp) { settle(sp, t); moq_simpair_destroy(sp); }
        return NULL;
    }
    settle(sp, t);
    for (unsigned side = 0; side < 2; side++) {
        moq_session_t *s = side ? moq_simpair_server(sp) : moq_simpair_client(sp);
        check(t, moq_session_state(s) == MOQ_SESS_ESTABLISHED, "pair.established");
        check(t, moq_session_version(s) == v, "pair.actual.version");
    }
    if (t->errors || t->run_failures) { moq_simpair_destroy(sp); return NULL; }
    return sp;
}

/* n subscriptions to tracks "a".."a"+n-1, all accepted in one pass; the
 * pass is not drained. */
static bool subscribe_n(moq_simpair_t *sp, tally_t *t, size_t n, bool settle_after)
{
    if (!check(t, n > 0 && n <= MAX_TRACKS && t->ntracks == 0, "subscribe.bounds")) return false;
    t->ntracks = n;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_bytes_t parts[] = { { (const uint8_t *)"live", 4 } };
    static const char names[] = "abcdefgh";
    for (size_t i = 0; i < n; i++) {
        moq_subscribe_cfg_t scfg;
        moq_subscribe_cfg_init(&scfg);
        scfg.track_namespace = (moq_namespace_t){ parts, 1 };
        scfg.track_name = (moq_bytes_t){ (const uint8_t *)&names[i], 1 };
        scfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
        scfg.start_group = 0;
        if (!check(t, moq_session_subscribe(c, &scfg, moq_simpair_now_us(sp), &t->csubs[i]) == MOQ_OK &&
            moq_subscription_is_valid(t->csubs[i]), "subscribe.create")) return false;
        settle(sp, t);
        if (t->errors || t->run_failures) return false;
    }
    if (t->nssubs != n) {
        fprintf(stderr, "  %zu of %zu subscriptions reached the publisher\n", t->nssubs, n);
        check(t, false, "subscribe.request.inventory");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        if (!check(t, moq_session_accept_subscribe(sv, t->ssubs[i], &ac,
            moq_simpair_now_us(sp)) == MOQ_OK, "subscribe.accept")) return false;
    }
    if (settle_after) settle(sp, t);
    return !t->errors && !t->run_failures;
}

static bool write_objects(moq_simpair_t *sp, tally_t *t, moq_subscription_t sub, uint64_t group,
                          uint64_t n, bool close)
{
    moq_session_t *sv = moq_simpair_server(sp);
    moq_subgroup_cfg_t sg;
    moq_subgroup_cfg_init(&sg);
    sg.group_id = group;
    moq_subgroup_handle_t sh = MOQ_SUBGROUP_INVALID;
    if (!check(t, moq_session_open_subgroup(sv, sub, &sg, moq_simpair_now_us(sp), &sh) == MOQ_OK &&
        moq_subgroup_is_valid(sh), "subgroup.open")) return false;
    for (uint64_t o = 0; o < n; o++) {
        uint8_t body[3] = { (uint8_t)group, (uint8_t)o, 0x5A };
        moq_rcbuf_t *pl = NULL;
        if (!check(t, moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &pl) == MOQ_OK && pl,
                   "payload.create")) { moq_rcbuf_decref(pl); return false; }
        moq_result_t rc = moq_session_write_object(sv, sh, o, pl, moq_simpair_now_us(sp));
        moq_rcbuf_decref(pl);
        if (!check(t, rc == MOQ_OK, "object.write")) return false;
    }
    return !close || check(t, moq_session_close_subgroup(sv, sh, moq_simpair_now_us(sp)) == MOQ_OK,
                          "subgroup.close");
}

static int common_checks(moq_simpair_t *sp, tally_t *t)
{
    int failures = 0;
    /* GOAWAY moves a session to DRAINING; neither may close. */
    moq_session_state_t cs = moq_session_state(moq_simpair_client(sp));
    moq_session_state_t ss = moq_session_state(moq_simpair_server(sp));
    if (t->closed || (cs != MOQ_SESS_ESTABLISHED && cs != MOQ_SESS_DRAINING) ||
        (ss != MOQ_SESS_ESTABLISHED && ss != MOQ_SESS_DRAINING)) {
        fprintf(stderr, "  a session closed\n");
        failures++;
    }
    if (t->run_failures) {
        fprintf(stderr, "  run_until_quiescent failed %zu times\n", t->run_failures);
        failures++;
    }
    for (size_t i = 0; i < t->ntracks; i++)
        check(t, t->requests[i] && t->accepted[i], "track.request.and.acceptance.complete");
    for (size_t i = 0; i < t->nreliable; i++) {
        const reliable_t *r = &t->reliable[i];
        for (size_t j = 0; j < r->count; j++)
            check(t, r->seen[j], "reliable.object.missing");
        check(t, r->finished, "reliable.completion.missing");
    }
    return failures + t->errors;
}

/* GOAWAY between two bursts of objects, the client's queue full. */
static int row_control(moq_version_t v, tally_t *t)
{
    moq_simpair_t *sp = make_pair(v, 2, 0, 0, t);
    if (!sp) return 1;
    int failures = 0;
    size_t objs0 = t->objects;
    if (!subscribe_n(sp, t, 1, true) ||
        !expect_stream(t, 0, 1, 3, 3) || !expect_stream(t, 0, 2, 3, 3) ||
        !write_objects(sp, t, t->ssubs[0], 1, 3, true)) goto done;
    pass(sp, t);   /* the queue fills; group 1 stalls */
    if (!check(t, moq_session_goaway(moq_simpair_server(sp), NULL, 0,
        moq_simpair_now_us(sp)) == MOQ_OK, "goaway.send")) goto done;
    pass(sp, t);   /* GOAWAY meets the full queue */
    if (!write_objects(sp, t, t->ssubs[0], 2, 3, true)) goto done;
done:
    settle(sp, t);
    if (t->refused[MOQ_SIM_INPUT_CONTROL_BYTES] == 0) {
        fprintf(stderr, "  no control input was refused\n");
        failures++;
    }
    /* On a unidirectional control pair a blocked control channel pauses the
     * session's other unidirectional inputs, as the bridge does: no data
     * input reaches the client while its control input is owed, and group
     * 2, written after GOAWAY, surfaces after it. */
    if (v != MOQ_VERSION_DRAFT_16 &&
        (t->data_while_ctrl_blocked != 0 ||
         !(t->goaway_at && t->first_g2_at && t->goaway_at < t->first_g2_at))) {
        fprintf(stderr, "  %zu data inputs while control was owed; GOAWAY at event "
                "%zu, first group-2 object at %zu\n", t->data_while_ctrl_blocked,
                t->goaway_at, t->first_g2_at);
        failures++;
    }
    if (t->goaways != 1 || t->objects - objs0 != 6 || t->finished != 2) {
        fprintf(stderr, "  goaway=%zu objects=%zu finished=%zu\n", t->goaways,
                t->objects - objs0, t->finished);
        failures++;
    }
    failures += common_checks(sp, t);
    moq_simpair_destroy(sp);
    return failures;
}

/* Six responses and an object in one pass. */
static int row_responses(moq_version_t v, tally_t *t)
{
    moq_simpair_t *sp = make_pair(v, 2, 0, 0, t);
    if (!sp) return 1;
    int failures = 0;
    if (!subscribe_n(sp, t, 6, false) || !expect_stream(t, 0, 1, 2, 3) ||
        !write_objects(sp, t, t->ssubs[0], 1, 2, true)) goto done;
done:
    settle(sp, t);
    unsigned kind = v == MOQ_VERSION_DRAFT_16 ? MOQ_SIM_INPUT_CONTROL_BYTES
                                              : MOQ_SIM_INPUT_BIDI_BYTES;
    if (t->refused[kind] == 0) {
        fprintf(stderr, "  no %s input was refused\n",
                v == MOQ_VERSION_DRAFT_16 ? "control" : "request-stream");
        failures++;
    }
    if (t->sub_oks != 6 || t->objects != 2 || t->finished != 1) {
        fprintf(stderr, "  subscribe_ok=%zu objects=%zu finished=%zu\n", t->sub_oks,
                t->objects, t->finished);
        failures++;
    }
    failures += common_checks(sp, t);
    moq_simpair_destroy(sp);
    return failures;
}

/* Six datagrams, then an object on a stream, in one pass. */
static int row_datagram(moq_version_t v, tally_t *t)
{
    moq_simpair_t *sp = make_pair(v, 2, 0, 0, t);
    if (!sp) return 1;
    int failures = 0;
    if (!subscribe_n(sp, t, 1, true) || !expect_stream(t, 0, 10, 1, 3)) goto done;
    t->allow_datagrams = true;
    moq_session_t *sv = moq_simpair_server(sp);
    for (uint64_t o = 0; o < 6; o++) {
        uint8_t body[2] = { 9, (uint8_t)o };
        moq_rcbuf_t *pl = NULL;
        if (!check(t, moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &pl) == MOQ_OK && pl,
                   "datagram.payload.create")) { moq_rcbuf_decref(pl); goto done; }
        moq_result_t rc = moq_session_send_object_datagram(sv, t->ssubs[0], 9, o, 1, false, pl,
                                                          NULL, 0, moq_simpair_now_us(sp));
        moq_rcbuf_decref(pl);
        if (!check(t, rc == MOQ_OK, "datagram.send")) goto done;
    }
    if (!write_objects(sp, t, t->ssubs[0], 10, 1, true)) goto done;
done:
    settle(sp, t);
    if (t->refused[MOQ_SIM_INPUT_DATAGRAM] == 0) {
        fprintf(stderr, "  no datagram was refused\n");
        failures++;
    }
    /* Refused datagrams are gone; every other one and the stream object
     * surface. */
    size_t dropped = t->refused[MOQ_SIM_INPUT_DATAGRAM];
    if (dropped > DATAGRAMS || t->datagram_objects != DATAGRAMS - dropped ||
        t->objects - t->datagram_objects != 1 ||
        t->finished != 1) {
        fprintf(stderr, "  datagrams=%zu (dropped %zu) stream objects=%zu finished=%zu\n",
                t->datagram_objects, dropped, t->objects - t->datagram_objects,
                t->finished);
        failures++;
    }
    failures += common_checks(sp, t);
    moq_simpair_destroy(sp);
    return failures;
}

/* A second subgroup arrives while the first is still open, at a client
 * that admits one stream. */
static bool write_one(moq_simpair_t *sp, tally_t *t, moq_subgroup_handle_t sh, uint64_t g,
                      uint64_t o)
{
    uint8_t body[2] = { (uint8_t)g, (uint8_t)o };
    moq_rcbuf_t *pl = NULL;
    if (!check(t, moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &pl) == MOQ_OK && pl,
               "payload.create")) { moq_rcbuf_decref(pl); return false; }
    moq_result_t rc = moq_session_write_object(moq_simpair_server(sp), sh, o, pl,
                                               moq_simpair_now_us(sp));
    moq_rcbuf_decref(pl);
    return check(t, rc == MOQ_OK, "object.write");
}

static int row_unadmitted(moq_version_t v, tally_t *t)
{
    moq_simpair_t *sp = make_pair(v, 0, 1, 0, t);
    if (!sp) return 1;
    int failures = 0;
    if (!subscribe_n(sp, t, 1, true) ||
        !expect_stream(t, 0, 1, 2, 2) || !expect_stream(t, 0, 2, 2, 2)) goto done;
    moq_session_t *sv = moq_simpair_server(sp);
    moq_subgroup_handle_t sh[2] = {MOQ_SUBGROUP_INVALID, MOQ_SUBGROUP_INVALID};
    for (int g = 0; g < 2; g++) {
        moq_subgroup_cfg_t sg;
        moq_subgroup_cfg_init(&sg);
        sg.group_id = 1 + (uint64_t)g;
        if (!check(t, moq_session_open_subgroup(sv, t->ssubs[0], &sg, moq_simpair_now_us(sp),
            &sh[g]) == MOQ_OK && moq_subgroup_is_valid(sh[g]), "subgroup.open")) goto done;
    }
    if (!write_one(sp, t, sh[0], 1, 0)) goto done;
    pass(sp, t);                       /* stream 1 is admitted and stays open */
    if (!write_one(sp, t, sh[1], 2, 0) || !write_one(sp, t, sh[1], 2, 1) ||
        !check(t, moq_session_close_subgroup(sv, sh[1], moq_simpair_now_us(sp)) == MOQ_OK,
               "subgroup.close")) goto done;
    pass(sp, t);                       /* stream 2 finds no receive entry */
    if (!write_one(sp, t, sh[0], 1, 1) ||
        !check(t, moq_session_close_subgroup(sv, sh[0], moq_simpair_now_us(sp)) == MOQ_OK,
               "subgroup.close")) goto done;
done:
    settle(sp, t);
    if (t->refused[MOQ_SIM_INPUT_DATA_BYTES] == 0) {
        fprintf(stderr, "  no data input was refused\n");
        failures++;
    }
    if (t->objects != 4 || t->finished != 2) {
        fprintf(stderr, "  objects=%zu finished=%zu\n", t->objects, t->finished);
        failures++;
    }
    failures += common_checks(sp, t);
    moq_simpair_destroy(sp);
    return failures;
}

static bool no_events(moq_session_t *s, tally_t *t)
{
    moq_event_t e;
    size_t n = 0;
    bool ok = check(t, moq_session_poll_events_ex(s, &e, 1, sizeof(e), &n) == MOQ_OK,
                    "namespace.event.poll");
    if (n) moq_event_cleanup(&e);
    return check(t, n == 0, "namespace.no.extra.event") && ok;
}

static bool take_action(moq_session_t *s, tally_t *t, moq_action_t *a, bool present)
{
    size_t n = 0;
    bool ok = check(t, moq_session_poll_actions_ex(s, a, 1, sizeof(*a), &n) == MOQ_OK,
                    "namespace.action.poll");
    ok = check(t, n == (present ? 1u : 0u), "namespace.action.inventory") && ok;
    if (!ok && n) moq_action_cleanup(a);
    return ok;
}

typedef struct {
    moq_announcement_t sender, receiver;
    moq_stream_ref_t outgoing, incoming;
} namespace_request_t;

/* Route only these three requests manually, retaining both public stream refs.
 * This keeps the two response slots full until the row explicitly drains them. */
static bool namespace_request(moq_simpair_t *sp, tally_t *t, unsigned i,
                               namespace_request_t *q)
{
    moq_session_t *c = moq_simpair_client(sp), *s = moq_simpair_server(sp);
    const uint8_t name = (uint8_t)('a' + i);
    moq_bytes_t part = {&name, 1};
    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    if (!check(t, moq_session_publish_namespace(c, &cfg, moq_simpair_now_us(sp), &q->sender) == MOQ_OK &&
        moq_announcement_is_valid(q->sender), "namespace.publish")) return false;
    moq_action_t a;
    if (!take_action(c, t, &a, true)) return false;
    const uint8_t expected[] = {6, 0, 5, (uint8_t)(2 * i), 1, 1, name, 0};
    bool ok = check(t, a.kind == MOQ_ACTION_OPEN_BIDI_STREAM, "namespace.open.kind");
    if (ok) {
        const moq_open_bidi_stream_action_t *o = &a.u.open_bidi_stream;
        q->outgoing = o->stream_ref;
        q->incoming = moq_stream_ref_from_u64(0x8000u + i);
        ok = check(t, !o->fin && o->len == sizeof(expected) && o->data &&
            !memcmp(o->data, expected, sizeof(expected)), "namespace.open.literal.bytes");
        if (ok) ok = check(t, moq_session_on_bidi_stream_bytes(s, q->incoming,
            o->data, o->len, false, moq_simpair_now_us(sp)) == MOQ_OK, "namespace.request.ingress");
    }
    moq_action_cleanup(&a);
    if (!ok || !take_action(c, t, &a, false) || !take_action(s, t, &a, false)) return false;
    moq_event_t e;
    size_t n = 0;
    ok = check(t, moq_session_poll_events_ex(s, &e, 1, sizeof(e), &n) == MOQ_OK && n == 1,
               "namespace.request.single");
    if (n) {
        ok = check(t, e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED, "namespace.request.kind") && ok;
        if (e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
            const moq_namespace_published_event_t *r = &e.u.namespace_published;
            q->receiver = r->ann;
            ok = check(t, moq_announcement_is_valid(r->ann) && r->track_namespace.count == 1 &&
                r->track_namespace.parts && r->track_namespace.parts[0].len == 1 &&
                r->track_namespace.parts[0].data && r->track_namespace.parts[0].data[0] == name,
                "namespace.request.identity") && ok;
        }
        moq_event_cleanup(&e);
    }
    return no_events(c, t) && no_events(s, t) && ok;
}

static bool namespace_response(moq_simpair_t *sp, tally_t *t, const namespace_request_t *q)
{
    moq_session_t *c = moq_simpair_client(sp), *s = moq_simpair_server(sp);
    moq_action_t a;
    if (!take_action(s, t, &a, true)) return false;
    const uint8_t expected[] = {7, 0, 1, 0};
    bool ok = check(t, a.kind == MOQ_ACTION_SEND_BIDI_STREAM, "namespace.response.kind");
    if (ok) {
        const moq_send_bidi_stream_action_t *r = &a.u.send_bidi_stream;
        ok = check(t, r->stream_ref._v == q->incoming._v && !r->fin &&
            r->len == sizeof(expected) && r->data && !memcmp(r->data, expected, sizeof(expected)),
            "namespace.response.ref.bytes.fin");
        if (ok) ok = check(t, moq_session_on_bidi_stream_bytes(c, q->outgoing,
            r->data, r->len, false, moq_simpair_now_us(sp)) == MOQ_OK, "namespace.response.ingress");
    }
    moq_action_cleanup(&a);
    if (!ok) return false;
    moq_event_t e;
    size_t n = 0;
    ok = check(t, moq_session_poll_events_ex(c, &e, 1, sizeof(e), &n) == MOQ_OK && n == 1,
               "namespace.accepted.single");
    if (n) {
        ok = check(t, e.kind == MOQ_EVENT_NAMESPACE_ACCEPTED &&
            moq_announcement_eq(e.u.namespace_accepted.ann, q->sender), "namespace.accepted.handle") && ok;
        moq_event_cleanup(&e);
    }
    if (ok) t->namespace_responses++;
    return no_events(c, t) && no_events(s, t) && ok;
}

static int row_namespace_accept(moq_version_t v, tally_t *t)
{
    moq_simpair_t *sp = make_pair(v, 0, 0, 2, t);
    if (!sp) return 1;
    namespace_request_t q[3] = {0};
    moq_session_t *s = moq_simpair_server(sp);
    moq_accept_namespace_cfg_t cfg;
    moq_accept_namespace_cfg_init(&cfg);
    for (unsigned i = 0; i < 3; i++)
        if (!namespace_request(sp, t, i, &q[i])) goto done;
    for (unsigned i = 1; i < 3; i++)
        if (!check(t, moq_session_accept_namespace(s, q[i].receiver, &cfg,
            moq_simpair_now_us(sp)) == MOQ_OK, "namespace.filler.accept")) goto done;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!check(t, moq_session_has_transport_stream(s, q[0].incoming), "namespace.pending.owner") ||
            !check(t, moq_session_accept_namespace(s, q[0].receiver, &cfg,
                moq_simpair_now_us(sp)) == MOQ_ERR_WOULD_BLOCK, "namespace.accept.real.pressure") ||
            !no_events(s, t) || !no_events(moq_simpair_client(sp), t)) goto done;
        t->namespace_refusals++;
    }
    /* Only the two fillers may be queued: no premature target acceptance. */
    if (!namespace_response(sp, t, &q[1]) || !namespace_response(sp, t, &q[2])) goto done;
    moq_action_t a;
    if (!take_action(s, t, &a, false) ||
        !check(t, moq_session_has_transport_stream(s, q[0].incoming), "namespace.same.pending.owner") ||
        !check(t, moq_session_accept_namespace(s, q[0].receiver, &cfg,
            moq_simpair_now_us(sp)) == MOQ_OK, "namespace.same.handle.retry") ||
        !namespace_response(sp, t, &q[0]) || !take_action(s, t, &a, false)) goto done;
    check(t, moq_session_accept_namespace(s, q[0].receiver, &cfg,
        moq_simpair_now_us(sp)) == MOQ_ERR_WRONG_STATE, "namespace.accept.committed.once");
    (void)take_action(s, t, &a, false);
done:
    /* Failed manual routing must not leave unmapped actions for SimPair. */
    if (t->errors) {
        for (unsigned side = 0; side < 2; side++) {
            moq_session_t *session = side ? s : moq_simpair_client(sp);
            moq_action_t pending;
            while (moq_session_poll_actions(session, &pending, 1)) moq_action_cleanup(&pending);
        }
    }
    settle(sp, t);
    check(t, t->namespace_refusals == 2 && t->namespace_responses == 3,
          "namespace.pressure.and.response.inventory");
    int failures = common_checks(sp, t);
    moq_simpair_destroy(sp);
    return failures;
}

typedef int (*row_fn)(moq_version_t, tally_t *);

static int run_row(const char *name, row_fn fn, moq_version_t v)
{
    tally_t a, b;
    int fa = fn(v, &a);
    int fb = fn(v, &b);
    int failures = fa + fb;
    /* Handles are per-session identities, not outcomes. */
    memset(a.csubs, 0, sizeof(a.csubs));
    memset(b.csubs, 0, sizeof(b.csubs));
    memset(a.ssubs, 0, sizeof(a.ssubs));
    memset(b.ssubs, 0, sizeof(b.ssubs));
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        fprintf(stderr, "  %s: runs differ\n", name);
        failures++;
    }
    if (failures)
        fprintf(stderr, "FAIL: %s (draft-%d)\n", name, (int)v);
    else
        printf("PASS: %s (draft-%d)\n", name, (int)v);
    return failures ? 1 : 0;
}

int main(void)
{
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
    };
    const char *e = getenv("MOQ_SIM_VERSION");
    if (e && strcmp(e, "16") && strcmp(e, "18") && strcmp(e, "22")) {
        fprintf(stderr, "FAIL: MOQ_SIM_VERSION=%s unsupported; require 16, 18 or 22\n", e);
        return 1;
    }
    int failures = 0;
    size_t rows = 0;
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        moq_version_t v = versions[i];
        if (e && (moq_version_t)strtoul(e, NULL, 10) != v) continue;
        failures += run_row("control", row_control, v);
        failures += run_row("responses", row_responses, v);
        failures += run_row("datagram", row_datagram, v);
        failures += run_row("unadmitted", row_unadmitted, v);
        rows += 4;
        if (v != MOQ_VERSION_DRAFT_16) {
            failures += run_row("namespace_accept", row_namespace_accept, v);
            rows++;
        }
    }
    if (!rows) {
        fprintf(stderr, "FAIL: version.selection.empty\n");
        return 1;
    }
    if (failures) {
        fprintf(stderr, "FAIL: test_scenario_input_backpressure (%d rows)\n", failures);
        return 1;
    }
    printf("PASS: test_scenario_input_backpressure\n");
    return 0;
}
