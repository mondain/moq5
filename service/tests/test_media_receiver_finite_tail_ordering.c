/* D18 finite-tail delivery under cross-stream reordering. The real publisher,
 * relay binding and receiver pump use SimPair for establishment/catalog. Only
 * transport delivery of the final two data streams and request terminal is
 * scheduled here; byte order within each reliable stream is never changed.
 *
 * D18 sections 10.11 and 11.2.1.1: END at {1,0} says nothing about completion
 * of group0. DONE can arrive before data/FIN, but counts every opened stream.
 * Direct rows use a correctly counted scripted terminal after both producer
 * streams close. Relay rows replay its actual, unmodified terminal bytes.
 */
#include <moq/media_receiver.h>
#include <moq/publisher.h>
#include <moq/control_d18.h>
#include <moq/sim.h>
#include <moq/relay/relay.h>
#include <moq/relay/moqr_bind.h>

#include <stdio.h>
#include <string.h>

moq_media_receiver_t *moq_media_receiver_test_new_cfg(
    const moq_media_receiver_cfg_t *cfg);
void moq_media_receiver_test_pump(moq_media_receiver_t *r,
    moq_session_t *session, uint64_t now_us);
void moq_media_receiver_test_free(moq_media_receiver_t *r);

enum { OBJECTS = 4, PAYLOAD_SIZE = 37, STREAM_CAP = 2048 };
static unsigned failures, checks, setup_failures;
static const char *row;

static bool check(bool ok, const char *expr, int line)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s line%d: %s\n", row, line, expr);
    }
    return ok;
}
#define CHECK(e) check((e), #e, __LINE__)
#define REQUIRE(e) do { if (!CHECK(e)) goto out; } while (0)

typedef struct observer {
    moq_perspective_t receiver_side;
    moq_stream_ref_t request, peer_request;
    unsigned requests, peer_requests, oks;
    bool has_filter, has_largest;
    uint64_t alias;
} observer_t;

/* Existing SimPair observer only; no event is consumed from the service. */
static void observe(void *ctx, const moq_sim_trace_record_t *rec)
{
    observer_t *o = ctx;
    bool request = rec->kind == MOQ_SIM_TRACE_ACTION &&
        rec->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM &&
        rec->from == o->receiver_side;
    bool response = rec->kind == MOQ_SIM_TRACE_INPUT &&
        rec->input_kind == MOQ_SIM_INPUT_BIDI_BYTES &&
        rec->to == o->receiver_side &&
        rec->stream_ref._v == o->request._v && o->requests != 0;
    bool peer_request = rec->kind == MOQ_SIM_TRACE_INPUT &&
        rec->input_kind == MOQ_SIM_INPUT_BIDI_BYTES &&
        rec->to != o->receiver_side;
    if (!request && !response && !peer_request) return;
    moq_bytes_t bytes = request ? rec->detail_bytes : rec->bytes;
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, bytes.data, bytes.len);
    moq_control_envelope_t env;
    if (moq_d18_decode_envelope(&r, &env) != MOQ_OK) return;
    if ((request || peer_request) && env.msg_type == MOQ_D18_SUBSCRIBE) {
        moq_d18_subscribe_t s;
        moq_bytes_t parts[4];
        if (!CHECK(moq_d18_decode_subscribe(env.payload, env.payload_len,
                                            parts, 4, &s) == MOQ_OK)) return;
        if (s.track_name.len != 5 || memcmp(s.track_name.data, "video", 5)) return;
        if (peer_request) {
            o->peer_request = rec->stream_ref;
            o->peer_requests++;
            return;
        }
        o->request = rec->stream_ref;
        o->requests++;
        o->has_filter = s.params.has_filter;
    } else if (response && env.msg_type == MOQ_D18_SUBSCRIBE_OK) {
        moq_d18_subscribe_ok_t ok;
        if (!CHECK(moq_d18_decode_subscribe_ok(env.payload, env.payload_len,
                                               &ok) == MOQ_OK)) return;
        o->oks++;
        o->has_largest = ok.params.has_largest;
        o->alias = ok.track_alias;
    }
}

typedef struct wire_stream {
    moq_stream_ref_t source;
    uint8_t bytes[STREAM_CAP];
    size_t len;
    bool used, fin;
} wire_stream_t;

typedef struct wire_tail {
    wire_stream_t streams[2];
    uint8_t done[128];
    size_t done_len;
    bool done_fin;
    uint64_t done_count;
    unsigned done_messages;
} wire_tail_t;

typedef struct rig {
    moq_simpair_t *origin, *down;
    moqr_core_t *core;
    moqr_bind_t *bind;
    moq_publisher_t *pub;
    moq_pub_track_t *catalog, *video;
    moq_media_receiver_t *receiver;
    moq_media_track_t *track;
    moq_session_t *rx;
    observer_t observer;
    wire_tail_t wire;
    uint64_t now;
    unsigned added, catalogs, objects, ended;
    bool seen[OBJECTS];
} rig_t;

enum step { STOP, DATA_FIN, END_FIN, DONE_FIN, DATA_ONLY, FIN_ONLY };
static const enum step schedules[4][5] = {
    { DATA_FIN, END_FIN, DONE_FIN, STOP },
    { END_FIN, DATA_FIN, DONE_FIN, STOP },
    { END_FIN, DONE_FIN, DATA_FIN, STOP },
    { END_FIN, DATA_ONLY, DONE_FIN, FIN_ONLY, STOP }
};

static void payload(unsigned object, uint8_t out[PAYLOAD_SIZE])
{
    for (unsigned i = 0; i < PAYLOAD_SIZE; i++)
        out[i] = (uint8_t)(object * 61u + i * 17u);
}

static bool pair_open(moq_simpair_t **pair, moqr_bind_t *bind,
                      observer_t *observer, uint64_t seed)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.seed = seed;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 64;
    cfg.server_streaming_objects = bind != NULL;
    if (observer) { cfg.trace_fn = observe; cfg.trace_ctx = observer; }
    if (!CHECK(moq_simpair_create(&cfg, pair) == MOQ_OK)) return false;
    if (!CHECK(moq_simpair_start(*pair) == MOQ_OK)) return false;
    return !bind || CHECK(moqr_bind_conn_open(bind, moq_simpair_server(*pair),
                                              MOQ_VERSION_DRAFT_18) == MOQR_OK);
}

static void poll_receiver(rig_t *r)
{
    moq_media_track_event_t ev;
    while (moq_media_receiver_poll_track(r->receiver, &ev, sizeof(ev)) == MOQ_OK) {
        if (ev.kind == MOQ_MEDIA_TRACK_ADDED) { r->track = ev.track; r->added++; }
        else if (ev.kind == MOQ_MEDIA_CATALOG_READY) r->catalogs++;
        else if (ev.kind == MOQ_MEDIA_TRACK_ENDED) {
            CHECK(ev.track == r->track);
            r->ended++;
        }
    }
    moq_media_object_t obj;
    moq_result_t rc;
    while ((rc = moq_media_receiver_poll_object(r->receiver, &obj, sizeof(obj))) == MOQ_OK) {
        CHECK(obj.track == r->track && obj.status == MOQ_OBJECT_NORMAL);
        CHECK(obj.group_id == 0 && obj.has_subgroup && obj.subgroup_id == 0);
        CHECK(!obj.datagram && obj.publisher_priority == 128);
        if (CHECK(obj.object_id < OBJECTS)) {
            unsigned i = (unsigned)obj.object_id;
            uint8_t expected[PAYLOAD_SIZE];
            payload(i, expected);
            CHECK(!r->seen[i]);
            CHECK(obj.payload.len == sizeof(expected) &&
                  memcmp(obj.payload.data, expected, sizeof(expected)) == 0);
            r->seen[i] = true;
        }
        r->objects++;
        moq_media_object_cleanup(&obj);
    }
    /* The existing offline receiver seam has no endpoint; empty is CLOSED. */
    CHECK(rc == MOQ_ERR_CLOSED);
    CHECK(!moq_media_receiver_is_fatal(r->receiver));
}

static void receiver_pump(rig_t *r, const char *phase)
{
    r->now++;
    CHECK(moq_session_process_pending(r->rx, r->now) >= 0);
    moq_media_receiver_test_pump(r->receiver, r->rx, r->now);
    poll_receiver(r);
    if (phase)
        printf("PHASE %s %s objects=%u ended=%u requests=%u state=%d\n",
               row, phase, r->objects, r->ended, r->observer.requests,
               (int)moq_session_state(r->rx));
}

static void cycle(rig_t *r)
{
    r->now++;
    CHECK(moq_simpair_advance_to(r->origin, r->now) == MOQ_OK);
    CHECK(moq_simpair_run_until_quiescent(r->origin, 64, NULL) >= 0);
    if (r->down) {
        CHECK(moq_simpair_advance_to(r->down, r->now) == MOQ_OK);
        CHECK(moq_simpair_run_until_quiescent(r->down, 64, NULL) >= 0);
    }
    if (r->bind) CHECK(moqr_bind_pump(r->bind, r->now) >= 0);
    if (r->pub) CHECK(moq_pub_tick(r->pub, r->now) >= 0);
    if (r->receiver) receiver_pump(r, NULL);
}

static bool capture(moq_session_t *sender, wire_tail_t *tail, const observer_t *observer)
{
    moq_action_t act;
    while (moq_session_poll_actions(sender, &act, 1) == 1) {
        bool ok = true;
        if (act.kind == MOQ_ACTION_SEND_DATA) {
            moq_send_data_action_t *d = &act.u.send_data;
            wire_stream_t *s = NULL;
            for (unsigned i = 0; i < 2; i++)
                if (tail->streams[i].used && tail->streams[i].source._v == d->stream_ref._v)
                    s = &tail->streams[i];
            if (!s) {
                moq_buf_reader_t reader;
                moq_buf_reader_init(&reader, d->header, d->header_len);
                moq_d18_subgroup_header_t h;
                ok = CHECK(moq_d18_decode_subgroup_header(&reader, &h) == MOQ_OK);
                if (ok) ok = CHECK(h.group_id < 2);
                if (ok) ok = CHECK(h.track_alias == observer->alias);
                if (ok) {
                    s = &tail->streams[h.group_id];
                    ok = CHECK(!s->used);
                    s->used = true;
                    s->source = d->stream_ref;
                }
            }
            size_t n = moq_rcbuf_len(d->payload);
            if (ok) ok = CHECK(!s->fin && s->len + d->header_len + n <= STREAM_CAP);
            if (ok) {
                memcpy(s->bytes + s->len, d->header, d->header_len);
                s->len += d->header_len;
                if (n) memcpy(s->bytes + s->len, moq_rcbuf_data(d->payload), n);
                s->len += n;
                s->fin = d->fin;
            }
        } else if (act.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            moq_send_bidi_stream_action_t *d = &act.u.send_bidi_stream;
            ok = CHECK(d->stream_ref._v == observer->peer_request._v);
            moq_buf_reader_t reader;
            moq_buf_reader_init(&reader, d->data, d->len);
            moq_control_envelope_t env;
            moq_d18_publish_done_t done;
            if (ok) ok = CHECK(moq_d18_decode_envelope(&reader, &env) == MOQ_OK &&
                               env.msg_type == MOQ_D18_PUBLISH_DONE);
            if (ok) ok = CHECK(moq_d18_decode_publish_done(env.payload, env.payload_len,
                                                           &done) == MOQ_OK);
            if (ok) ok = CHECK(tail->done_messages == 0 && d->len <= sizeof(tail->done));
            if (ok) {
                CHECK(done.status_code == 2);
                memcpy(tail->done, d->data, d->len);
                tail->done_len = d->len;
                tail->done_fin = d->fin;
                tail->done_count = done.stream_count;
                tail->done_messages++;
            }
        } else {
            ok = CHECK(false);
            fprintf(stderr, "unexpected captured action=%u\n", act.kind);
        }
        moq_action_cleanup(&act);
        if (!ok) return false;
    }
    return true;
}

static bool feed_data(rig_t *r, unsigned group, bool bytes, bool fin)
{
    wire_stream_t *s = &r->wire.streams[group];
    moq_result_t rc = moq_session_on_data_bytes(r->rx,
        moq_stream_ref_from_u64(0x413200u + group),
        bytes ? s->bytes : NULL, bytes ? s->len : 0, fin, ++r->now);
    if (!CHECK(rc == MOQ_OK)) return false;
    receiver_pump(r, group == 0 ? (bytes ? "group0-bytes" : "group0-FIN") : "END-FIN");
    return true;
}

static bool feed_done(rig_t *r)
{
    moq_result_t rc = moq_session_on_bidi_stream_bytes(r->rx, r->observer.request,
        r->wire.done, r->wire.done_len, r->wire.done_fin, ++r->now);
    if (!CHECK(rc == MOQ_OK)) return false;
    receiver_pump(r, "DONE-FIN");
    return true;
}

/* Separate public-core consumer: observe terminal timing without stealing
 * events from the real receiver's facade. The replayed tail is unchanged. */
static bool core_probe(const rig_t *source, unsigned order)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                MOQ_PERSPECTIVE_CLIENT);
    cfg.version = MOQ_VERSION_DRAFT_18;
    moq_session_t *s = NULL;
    bool ready = false;
    REQUIRE(moq_session_create(&cfg, 0, &s) == MOQ_OK);
    REQUIRE(moq_session_start(s, 0) == MOQ_OK);
    moq_action_t act;
    while (moq_session_poll_actions(s, &act, 1)) moq_action_cleanup(&act);
    uint8_t control[128];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, control, sizeof(control));
    REQUIRE(moq_d18_encode_setup(&w) == MOQ_OK);
    REQUIRE(moq_session_on_control_bytes(s, control, moq_buf_writer_offset(&w), 0) == MOQ_OK);
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1)) moq_event_cleanup(&ev);
    REQUIRE(moq_session_state(s) == MOQ_SESS_ESTABLISHED);
    moq_subscribe_cfg_t subcfg;
    moq_subscribe_cfg_init(&subcfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("finite"), MOQ_BYTES_LITERAL("tail") };
    subcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    subcfg.track_name = MOQ_BYTES_LITERAL("video");
    subcfg.filter = MOQ_SUBSCRIBE_FILTER_NONE;
    moq_subscription_t sub;
    REQUIRE(moq_session_subscribe(s, &subcfg, 1, &sub) == MOQ_OK);
    REQUIRE(moq_session_poll_actions(s, &act, 1) == 1);
    bool request_ok = act.kind == MOQ_ACTION_OPEN_BIDI_STREAM;
    moq_stream_ref_t request = act.u.open_bidi_stream.stream_ref;
    moq_action_cleanup(&act);
    REQUIRE(request_ok);
    moq_buf_writer_init(&w, control, sizeof(control));
    REQUIRE(moq_d18_encode_subscribe_ok(&w, source->observer.alias,
        &(moq_d18_msg_params_t){0}, (moq_bytes_t){0}) == MOQ_OK);
    REQUIRE(moq_session_on_bidi_stream_bytes(s, request, control,
        moq_buf_writer_offset(&w), false, 1) == MOQ_OK);
    while (moq_session_poll_events(s, &ev, 1)) moq_event_cleanup(&ev);
    ready = true;
    unsigned objects = 0, ends = 0, finished = 0, dones = 0, stops = 0;
    for (unsigned i = 0; schedules[order][i] != STOP; i++) {
        enum step step = schedules[order][i];
        if (step == DONE_FIN) {
            REQUIRE(moq_session_on_bidi_stream_bytes(s, request, source->wire.done,
                source->wire.done_len, source->wire.done_fin, 2 + i) == MOQ_OK);
        } else {
            unsigned group = step == END_FIN ? 1 : 0;
            const wire_stream_t *stream = &source->wire.streams[group];
            REQUIRE(moq_session_on_data_bytes(s, moq_stream_ref_from_u64(0x413200u + group),
                step == FIN_ONLY ? NULL : stream->bytes,
                step == FIN_ONLY ? 0 : stream->len, step != DATA_ONLY, 2 + i) == MOQ_OK);
        }
        while (moq_session_poll_events(s, &ev, 1)) {
            if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
                moq_object_received_event_t *o = &ev.u.object_received;
                if (o->status == MOQ_OBJECT_END_OF_TRACK) {
                    CHECK(o->group_id == 1 && o->object_id == 0);
                    ends++;
                } else {
                    CHECK(o->status == MOQ_OBJECT_NORMAL && o->group_id == 0 &&
                          o->subgroup_id == 0 && o->object_id == objects);
                    uint8_t expected[PAYLOAD_SIZE];
                    payload(objects, expected);
                    CHECK(moq_rcbuf_len(o->payload) == sizeof(expected) &&
                          memcmp(moq_rcbuf_data(o->payload), expected, sizeof(expected)) == 0);
                    objects++;
                }
            } else if (ev.kind == MOQ_EVENT_SUBGROUP_FINISHED) finished++;
            else if (ev.kind == MOQ_EVENT_SUBSCRIBE_DONE) {
                CHECK(finished == 2);
                CHECK(ev.u.subscribe_done.stream_count == 2);
                dones++;
            } else CHECK(false);
            moq_event_cleanup(&ev);
        }
        while (moq_session_poll_actions(s, &act, 1)) {
            if (act.kind == MOQ_ACTION_STOP_DATA) {
                printf("CORE-STOP %s stream=%llu code=%llu\n", row,
                       (unsigned long long)act.u.stop_data.stream_ref._v,
                       (unsigned long long)act.u.stop_data.error_code);
                stops++;
            } else CHECK(act.kind == MOQ_ACTION_CLOSE_BIDI_STREAM);
            moq_action_cleanup(&act);
        }
        moq_location_filter_cfg_t filter;
        moq_location_filter_cfg_init(&filter);
        moq_result_t live = moq_session_subscription_filter(s, sub, &filter);
        printf("CORE %s step=%u objects=%u END=%u FIN=%u DONE=%u handle-result=%d\n",
               row, (unsigned)step, objects, ends, finished, dones, (int)live);
        if (step == DONE_FIN && finished < 2) CHECK(live == MOQ_OK && dones == 0);
    }
    CHECK(objects == OBJECTS && ends == 1 && finished == 2 && dones == 1 && stops == 0);
    CHECK(moq_session_state(s) == MOQ_SESS_ESTABLISHED);
out:
    if (s) moq_session_destroy(s);
    return ready;
}

static const char catalog[] =
    "{\"version\":\"1\",\"tracks\":[{\"name\":\"video\","
    "\"packaging\":\"loc\",\"isLive\":true,\"role\":\"video\",\"codec\":\"av01\"}]}";

static void run(bool relayed, unsigned order)
{
    static const char *orders[] = { "data-END-DONE", "END-data-DONE",
                                    "END-DONE-data", "END-data-DONE-FIN" };
    char label[96];
    snprintf(label, sizeof(label), "%s/%s", relayed ? "relay" : "direct", orders[order]);
    row = label;
    unsigned before = failures;
    bool ready = false;
    rig_t r = {0};
    r.observer.receiver_side = relayed ? MOQ_PERSPECTIVE_CLIENT : MOQ_PERSPECTIVE_SERVER;
    if (relayed) {
        moqr_core_relay_cfg_t cfg;
        moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default());
        cfg.log_budget.max_groups = 8;
        cfg.log_budget.max_bytes = 1u << 20;
        REQUIRE(moqr_core_create(&cfg, &r.core) == MOQR_OK);
        moqr_bind_cfg_t bcfg;
        moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), moq_alloc_default());
        bcfg.core = r.core;
        bcfg.max_conns = 2;
        REQUIRE(moqr_bind_create(&bcfg, &r.bind) == MOQR_OK);
    }
    REQUIRE(pair_open(&r.origin, r.bind, relayed ? NULL : &r.observer, 0x413201));
    if (relayed) REQUIRE(pair_open(&r.down, r.bind, &r.observer, 0x413202));
    r.rx = relayed ? moq_simpair_client(r.down) : moq_simpair_server(r.origin);
    for (unsigned i = 0; i < 8; i++) cycle(&r);
    REQUIRE(moq_session_state(r.rx) == MOQ_SESS_ESTABLISHED);
    moq_pub_cfg_t pcfg;
    moq_pub_cfg_init_sized(&pcfg, sizeof(pcfg));
    pcfg.accept_mode = MOQ_PUB_ACCEPT_ALL;
    REQUIRE(moq_pub_create(moq_simpair_client(r.origin), moq_alloc_default(),
                           &pcfg, &r.pub) == MOQ_OK);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("finite"), MOQ_BYTES_LITERAL("tail") };
    moq_pub_track_cfg_t tcfg;
    moq_pub_track_cfg_init_sized(&tcfg, sizeof(tcfg));
    tcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    tcfg.track_name = MOQ_BYTES_LITERAL("catalog");
    tcfg.advertise_namespace = relayed;
    REQUIRE(moq_pub_add_track(r.pub, &tcfg, r.now, &r.catalog) == MOQ_OK);
    tcfg.track_name = MOQ_BYTES_LITERAL("video");
    REQUIRE(moq_pub_add_track(r.pub, &tcfg, r.now, &r.video) == MOQ_OK);
    moq_rcbuf_t *cat = NULL;
    REQUIRE(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)catalog,
                             sizeof(catalog) - 1, &cat) == MOQ_OK);
    moq_pub_retained_object_t retained = {
        .object_id = 0, .payload = cat, .properties = NULL, .end_of_group = true
    };
    moq_pub_retained_group_cfg_t group;
    moq_pub_retained_group_cfg_init(&group);
    group.objects = &retained;
    group.object_count = 1;
    moq_result_t cat_rc = moq_pub_set_retained_group(r.pub, r.catalog, &group);
    moq_rcbuf_decref(cat);
    REQUIRE(cat_rc == MOQ_OK);
    for (unsigned i = 0; i < 8; i++) cycle(&r);
    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live(&rcfg);
    rcfg.namespace_ = tcfg.track_namespace;
    rcfg.auto_subscribe = false;
    rcfg.time_mode = MOQ_MEDIA_TIME_RAW;
    rcfg.overflow.max_objects = 64;
    rcfg.overflow.max_bytes = 1u << 20;
    r.receiver = moq_media_receiver_test_new_cfg(&rcfg);
    REQUIRE(r.receiver != NULL);
    for (unsigned i = 0; i < 64 && !r.catalogs; i++) cycle(&r);
    REQUIRE(r.catalogs == 1 && r.added == 1 && r.track != NULL);
    moq_location_filter_cfg_t location = { .struct_size = sizeof(location),
                                           .kind = MOQ_LOCATION_FILTER_NONE };
    moq_media_receiver_track_subscribe_cfg_t options;
    moq_media_receiver_track_subscribe_cfg_init_sized(&options, sizeof(options));
    options.location = &location;
    REQUIRE(moq_media_receiver_subscribe_track(r.receiver, r.track, &options) == MOQ_OK);
    for (unsigned i = 0; i < 64 && r.observer.oks == 0; i++) cycle(&r);
    REQUIRE(r.observer.requests == 1 && r.observer.peer_requests == 1 && r.observer.oks == 1);
    REQUIRE(!r.observer.has_filter && !r.observer.has_largest);
    for (unsigned i = 0; i < 4; i++) cycle(&r);
    REQUIRE(r.objects == 0 && r.ended == 0);
    printf("WINDOW %s wire-filter=NONE largest=absent start=0/0 alias=%llu\n",
           row, (unsigned long long)r.observer.alias);

    /* From here the receiving pair is frozen. Produce the complete tail before
     * delivering any of it; the publisher closes both independent streams. */
    for (unsigned i = 0; i < OBJECTS; i++) {
        uint8_t bytes[PAYLOAD_SIZE];
        payload(i, bytes);
        moq_rcbuf_t *body = NULL;
        REQUIRE(moq_rcbuf_create(moq_alloc_default(), bytes, sizeof(bytes), &body) == MOQ_OK);
        moq_result_t rc = moq_pub_write_object(r.pub, r.video, 0, i, body, r.now);
        moq_rcbuf_decref(body);
        REQUIRE(rc == MOQ_OK);
    }
    REQUIRE(moq_pub_end_track(r.pub, r.video, r.now) == MOQ_OK);
    if (relayed) {
        for (unsigned i = 0; i < 16; i++) {
            r.now++;
            REQUIRE(moq_simpair_advance_to(r.origin, r.now) == MOQ_OK);
            REQUIRE(moq_simpair_run_until_quiescent(r.origin, 64, NULL) >= 0);
            REQUIRE(moqr_bind_pump(r.bind, r.now) >= 0);
            REQUIRE(capture(moq_simpair_server(r.down), &r.wire, &r.observer));
            REQUIRE(moq_pub_tick(r.pub, r.now) >= 0);
        }
        REQUIRE(r.wire.done_messages == 1);
    } else {
        REQUIRE(capture(moq_simpair_client(r.origin), &r.wire, &r.observer));
        REQUIRE(r.wire.done_messages == 0);
        moq_buf_writer_t w;
        moq_buf_writer_init(&w, r.wire.done, sizeof(r.wire.done));
        REQUIRE(moq_d18_encode_publish_done(&w, 2, 2, (moq_bytes_t){0}) == MOQ_OK);
        r.wire.done_len = moq_buf_writer_offset(&w);
        r.wire.done_count = 2;
        r.wire.done_fin = true;
        r.wire.done_messages = 1;
    }
    REQUIRE(r.wire.streams[0].used && r.wire.streams[1].used);
    REQUIRE(r.wire.streams[0].fin && r.wire.streams[1].fin && r.wire.done_fin);
    ready = failures == before;
    REQUIRE(ready);
    printf("WIRE %s streams=2 FINs=2 DONE-count=%llu DONE-fin=%d\n", row,
           (unsigned long long)r.wire.done_count, (int)r.wire.done_fin);
    CHECK(r.wire.done_count == 2);
    REQUIRE(core_probe(&r, order));
    for (unsigned i = 0; schedules[order][i] != STOP; i++) {
        enum step step = schedules[order][i];
        if (step == DONE_FIN) REQUIRE(feed_done(&r));
        else REQUIRE(feed_data(&r, step == END_FIN ? 1 : 0,
                               step != FIN_ONLY, step != DATA_ONLY));
        if (step == END_FIN && order != 0) CHECK(r.objects == 0 && r.ended == 1);
    }
    CHECK(r.objects == OBJECTS && r.ended == 1);
    for (unsigned i = 0; i < OBJECTS; i++) CHECK(r.seen[i]);
    moq_media_track_state_t state;
    CHECK(moq_media_receiver_track_state(r.receiver, r.track, &state) == MOQ_OK &&
          state == MOQ_MEDIA_TRACK_STATE_ENDED);
    CHECK(moq_session_state(r.rx) == MOQ_SESS_ESTABLISHED);
    moq_media_receiver_stats_t stats;
    CHECK(moq_media_receiver_get_stats(r.receiver, &stats, sizeof(stats)) == MOQ_OK);
    printf("RESULT %s objects=%u ended=%u received=%llu dropped=%llu parse=%llu\n",
           row, r.objects, r.ended, (unsigned long long)stats.objects_received,
           (unsigned long long)stats.objects_dropped, (unsigned long long)stats.parse_drops);
    CHECK(stats.objects_received == OBJECTS && stats.objects_dropped == 0 && stats.parse_drops == 0);
out:
    if (!ready) setup_failures++;
    if (r.receiver) moq_media_receiver_test_free(r.receiver);
    if (r.pub) moq_pub_destroy(r.pub);
    if (r.bind) moqr_bind_destroy(r.bind);
    if (r.core) moqr_core_destroy(r.core);
    if (r.down) moq_simpair_destroy(r.down);
    if (r.origin) moq_simpair_destroy(r.origin);
    printf("%s %s new-failures=%u setup=%d\n", failures == before ? "PASS" : "FAIL",
           row, failures - before, !ready);
}

int main(void)
{
    for (unsigned order = 0; order < 4; order++) run(false, order);
    for (unsigned order = 0; order < 4; order++) run(true, order);
    printf("finite-tail-ordering cases=8 checks=%u failures=%u setup=%u\n",
           checks, failures, setup_failures);
    return failures ? 1 : 0;
}
