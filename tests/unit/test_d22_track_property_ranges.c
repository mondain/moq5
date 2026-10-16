#include <moq/moq.h>
#include <moq/sim.h>
#include "../../core/src/session/request_stream_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, rows;
static const char *label;
#define CHECK(x) do { if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL[%s]:%d: %s\n", label, __LINE__, #x); } } while (0)
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "FIXTURE[%s]:%d: %s\n", label, __LINE__, #x); exit(2); } } while (0)
#define BYTES(a) ((moq_bytes_t){(a), sizeof(a)})

typedef struct fixture {
    moq_simpair_t *pair;
    moq_alloc_t alloc;
    size_t live;
} fixture_t;

static void *allocate(size_t n, void *ctx)
{
    fixture_t *f = ctx;
    void *p = malloc(n);
    if (p) ++f->live;
    return p;
}
static void release(void *p, size_t n, void *ctx)
{
    fixture_t *f = ctx;
    (void)n;
    if (p) { --f->live; free(p); }
}
static void *resize(void *p, size_t old, size_t n, void *ctx)
{
    (void)old;
    if (!p) return allocate(n, ctx);
    if (!n) { release(p, old, ctx); return NULL; }
    return realloc(p, n);
}
static void drain(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) moq_action_cleanup(&a);
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}
static void open_pair(fixture_t *f)
{
    f->alloc = (moq_alloc_t){.ctx=f, .alloc=allocate, .realloc=resize, .free=release};
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &f->alloc;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    REQUIRE(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    REQUIRE(moq_session_state(moq_simpair_client(f->pair)) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(moq_simpair_server(f->pair)) == MOQ_SESS_ESTABLISHED);
    drain(moq_simpair_client(f->pair)); drain(moq_simpair_server(f->pair));
}
static void close_pair(fixture_t *f)
{
    moq_simpair_destroy(f->pair);
    CHECK(f->live == 0);
}
static moq_subscription_t pending(moq_session_t *s, bool omit)
{
    uint8_t wire[] = {3, 0, 9, 0, 1, 1, 'n', 1, 't', 1, 0x35, 0};
    size_t n = sizeof(wire);
    if (!omit) { wire[2] = 7; wire[9] = 0; n -= 2; }
    REQUIRE(moq_session_on_bidi_stream_bytes(s, moq_stream_ref_from_u64(0x7000),
        wire, n, false, 10) == MOQ_OK);
    moq_event_t e;
    REQUIRE(moq_session_poll_events(s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST);
    moq_subscription_t sub = e.u.subscribe_request.sub;
    moq_event_cleanup(&e);
    return sub;
}

static void local_refusal(moq_bytes_t properties, bool publish, bool variant)
{
    ++rows;
    fixture_t f = {0};
    open_pair(&f);
    moq_session_t *s = publish && variant ? moq_simpair_client(f.pair) : moq_simpair_server(f.pair);
    moq_subscription_t sub = publish ? MOQ_SUBSCRIPTION_INVALID : pending(s, variant);
    moq_session_t before = *s;
    moq_request_stream_state_t ids = *(moq_request_stream_state_t *)s->profile_state;
    size_t live = f.live;
    size_t sub_bytes = s->sub_cap * sizeof(*s->subs), pub_bytes = s->pub_cap * sizeof(*s->publishes);
    void *subs = malloc(sub_bytes), *pubs = malloc(pub_bytes);
    REQUIRE(subs && pubs);
    memcpy(subs, s->subs, sub_bytes); memcpy(pubs, s->publishes, pub_bytes);
    uint8_t original[64];
    REQUIRE(properties.len <= sizeof(original));
    memcpy(original, properties.data, properties.len);
    moq_result_t rc;
    if (publish) {
        moq_bytes_t part = MOQ_BYTES_LITERAL("n");
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        cfg.track_properties = properties;
        moq_publication_t handle = {0x7777};
        rc = moq_session_publish(s, &cfg, 100, &handle);
        CHECK(handle._opaque == 0x7777);
    } else {
        moq_accept_subscribe_cfg_t cfg;
        moq_accept_subscribe_cfg_init(&cfg);
        cfg.track_properties = properties;
        rc = moq_session_accept_subscribe(s, sub, &cfg, 100);
    }
    CHECK(rc == MOQ_ERR_INVAL);
    CHECK(memcmp(&before, s, sizeof(before)) == 0);
    CHECK(memcmp(&ids, s->profile_state, sizeof(ids)) == 0);
    CHECK(memcmp(subs, s->subs, sub_bytes) == 0);
    CHECK(memcmp(pubs, s->publishes, pub_bytes) == 0);
    CHECK(f.live == live);
    CHECK(memcmp(original, properties.data, properties.len) == 0);
    free(subs); free(pubs);
    close_pair(&f);
}

static void incoming(moq_bytes_t properties, bool publish, bool valid)
{
    ++rows;
    fixture_t f = {0};
    open_pair(&f);
    moq_session_t *s = publish ? moq_simpair_server(f.pair) : moq_simpair_client(f.pair);
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7000);
    if (!publish) {
        moq_bytes_t part = MOQ_BYTES_LITERAL("n");
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        moq_subscription_t sub;
        REQUIRE(moq_session_subscribe(s, &cfg, 10, &sub) == MOQ_OK);
        moq_action_t a;
        REQUIRE(moq_session_poll_actions(s, &a, 1) == 1);
        REQUIRE(a.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
        ref = a.u.open_bidi_stream.stream_ref;
        moq_action_cleanup(&a);
    }
    uint8_t wire[96];
    const uint8_t sub_ok[] = {4, 0, 2, 1, 0};
    const uint8_t publication[] = {0x1d, 0, 8, 0, 1, 1, 'n', 1, 't', 1, 0};
    size_t prefix = publish ? sizeof(publication) : sizeof(sub_ok);
    memcpy(wire, publish ? publication : sub_ok, prefix);
    wire[2] = (uint8_t)(prefix - 3 + properties.len);
    if (properties.len) memcpy(wire + prefix, properties.data, properties.len);
    CHECK(moq_session_on_bidi_stream_bytes(s, ref, wire, prefix + properties.len, false, 20) == MOQ_OK);
    CHECK(moq_session_state(s) == (valid ? MOQ_SESS_ESTABLISHED : MOQ_SESS_CLOSED));
    moq_action_t a;
    unsigned closes = 0;
    while (moq_session_poll_actions(s, &a, 1)) {
        CHECK(!valid && a.kind == MOQ_ACTION_CLOSE_SESSION && a.u.close_session.code == 3);
        if (a.kind == MOQ_ACTION_CLOSE_SESSION && a.u.close_session.code == 3) ++closes;
        moq_action_cleanup(&a);
    }
    CHECK(closes == (valid ? 0u : 1u));
    moq_event_t e;
    unsigned events = 0;
    while (moq_session_poll_events(s, &e, 1)) {
        CHECK(e.kind == (valid ? (publish ? MOQ_EVENT_PUBLISH_REQUEST : MOQ_EVENT_SUBSCRIBE_OK) : MOQ_EVENT_SESSION_CLOSED));
        if (!valid) CHECK(e.u.closed.code == 3);
        ++events;
        moq_event_cleanup(&e);
    }
    CHECK(events == 1);
    close_pair(&f);
}

static void local_success(moq_bytes_t properties, moq_bytes_t expected,
                          bool publish, bool variant)
{
    ++rows;
    fixture_t f = {0};
    open_pair(&f);
    moq_session_t *s = publish && variant ? moq_simpair_client(f.pair) : moq_simpair_server(f.pair);
    uint8_t original[64];
    REQUIRE(properties.len <= sizeof(original));
    if (properties.len) memcpy(original, properties.data, properties.len);
    if (publish) {
        moq_bytes_t part = MOQ_BYTES_LITERAL("n");
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        cfg.track_properties = properties;
        moq_publication_t handle = MOQ_PUBLICATION_INVALID;
        REQUIRE(moq_session_publish(s, &cfg, 100, &handle) == MOQ_OK);
        CHECK(handle._opaque != MOQ_PUBLICATION_INVALID._opaque);
    } else {
        moq_subscription_t sub = pending(s, variant);
        moq_accept_subscribe_cfg_t cfg;
        moq_accept_subscribe_cfg_init(&cfg);
        cfg.has_track_alias = true; cfg.track_alias = 1;
        cfg.track_properties = properties;
        REQUIRE(moq_session_accept_subscribe(s, sub, &cfg, 100) == MOQ_OK);
    }
    moq_action_t a;
    REQUIRE(moq_session_poll_actions(s, &a, 1) == 1);
    CHECK(a.kind == (publish ? MOQ_ACTION_OPEN_BIDI_STREAM : MOQ_ACTION_SEND_BIDI_STREAM));
    if (!publish && a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
        uint8_t oracle[96] = {4, 0, (uint8_t)(2 + expected.len), 1, 0};
        if (expected.len) memcpy(oracle + 5, expected.data, expected.len);
        CHECK(a.u.send_bidi_stream.len == 5 + expected.len);
        if (a.u.send_bidi_stream.len == 5 + expected.len)
            CHECK(memcmp(a.u.send_bidi_stream.data, oracle, 5 + expected.len) == 0);
    }
    moq_action_cleanup(&a);
    CHECK(moq_session_poll_actions(s, &a, 1) == 0);
    if (properties.len) CHECK(memcmp(original, properties.data, properties.len) == 0);
    close_pair(&f);
}

static void encode_control(moq_bytes_t properties, moq_bytes_t expected, bool omit,
                            moq_version_t version)
{
    ++rows;
    uint8_t wire[96], oracle[96] = {0xa5, 4, 0, 0};
    memset(wire, 0xa5, sizeof(wire));
    size_t pos = 4;
    if (version == MOQ_VERSION_DRAFT_16) oracle[pos++] = 0;
    oracle[pos++] = 1; oracle[pos++] = 0;
    if (expected.len) memcpy(oracle + pos, expected.data, expected.len);
    pos += expected.len; oracle[3] = (uint8_t)(pos - 4);
    moq_subscribe_ok_encode_args_t args = {
        .track_alias=1, .track_properties=properties.data, .track_properties_len=properties.len,
        .omit_track_properties=omit, .inherit_track_group_order=true
    };
    moq_buf_writer_t w = {wire, sizeof(wire), 1};
    CHECK(moq_profile_lookup(version)->encode_subscribe_ok(NULL, &w, &args) == MOQ_OK);
    CHECK(w.pos == pos);
    CHECK(memcmp(wire, oracle, pos) == 0);
}

static void codec_refusal(moq_bytes_t properties)
{
    ++rows;
    uint8_t body[96] = {1, 0};
    memcpy(body + 2, properties.data, properties.len);
    moq_d22_message_context_t ctx = {0};
    moq_d22_message_t value, before;
    memset(&value, 0xa5, sizeof(value)); before = value;
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE_OK, body, 2 + properties.len, &ctx, &value) == MOQ_ERR_PROTO);
    CHECK(memcmp(&value, &before, sizeof(value)) == 0);
    for (unsigned omit = 0; omit < 2; ++omit) {
        uint8_t wire[96]; memset(wire, 0xa5, sizeof(wire));
        moq_buf_writer_t w = {wire, sizeof(wire), 1};
        moq_subscribe_ok_encode_args_t args = {
            .track_alias=1, .track_properties=properties.data, .track_properties_len=properties.len,
            .omit_track_properties=omit != 0, .inherit_track_group_order=true
        };
        CHECK(moq_profile_lookup(MOQ_VERSION_DRAFT_22)->encode_subscribe_ok(NULL, &w, &args) == MOQ_ERR_INVAL);
        CHECK(w.pos == 1 && wire[0] == 0xa5);
    }
}

static void local_fetch(moq_bytes_t properties, bool omit, bool valid)
{
    ++rows;
    fixture_t f = {0};
    open_pair(&f);
    moq_session_t *s = moq_simpair_server(f.pair);
    /* Bounded Group-End filter and an explicit INCLUDE_PROPERTIES value. */
    uint8_t request[] = {0x16,0,14,0,1,1,'n',1,'t',2,0x21,3,1,0,0,0x14,0};
    request[16] = omit ? 0 : 1;
    REQUIRE(moq_session_on_bidi_stream_bytes(s, moq_stream_ref_from_u64(0x7010),
        request, sizeof(request), false, 10) == MOQ_OK);
    moq_event_t event;
    REQUIRE(moq_session_poll_events(s, &event, 1) == 1);
    REQUIRE(event.kind == MOQ_EVENT_FETCH_REQUEST);
    moq_fetch_t handle = event.u.fetch_request.fetch;
    moq_event_cleanup(&event);

    moq_accept_fetch_cfg_t cfg;
    moq_accept_fetch_cfg_init(&cfg);
    cfg.end_group = 1;
    cfg.empty = true;
    cfg.track_properties = properties;
    moq_session_t before = *s;
    moq_request_stream_state_t ids = *(moq_request_stream_state_t *)s->profile_state;
    size_t live = f.live;
    size_t owners_len = s->fetch_cap * sizeof(*s->fetches);
    void *owners = malloc(owners_len);
    REQUIRE(owners != NULL);
    memcpy(owners, s->fetches, owners_len);
    moq_result_t rc = moq_session_accept_fetch(s, handle, &cfg, 100);
    CHECK(rc == (valid ? MOQ_OK : MOQ_ERR_INVAL));
    if (!valid) {
        CHECK(memcmp(&before, s, sizeof(before)) == 0);
        CHECK(memcmp(&ids, s->profile_state, sizeof(ids)) == 0);
        CHECK(memcmp(owners, s->fetches, owners_len) == 0);
        CHECK(f.live == live);
    }
    free(owners);

    moq_action_t action;
    unsigned actions = 0;
    while (moq_session_poll_actions(s, &action, 1)) {
        if (valid && actions == 0) {
            uint8_t expected[96] = {0x18,0,12,0,1,
                0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0};
            size_t n = omit ? 0 : properties.len;
            REQUIRE(n <= sizeof(expected) - 15);
            expected[2] = (uint8_t)(12 + n);
            if (n) memcpy(expected + 15, properties.data, n);
            CHECK(action.kind == MOQ_ACTION_SEND_BIDI_STREAM);
            if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
                CHECK(action.u.send_bidi_stream.stream_ref._v == 0x7010);
                CHECK(action.u.send_bidi_stream.fin);
                CHECK(action.u.send_bidi_stream.len == 15 + n);
                if (action.u.send_bidi_stream.len == 15 + n)
                    CHECK(memcmp(action.u.send_bidi_stream.data, expected, 15 + n) == 0);
            }
        } else CHECK(valid && actions == 1 && action.kind == MOQ_ACTION_SEND_DATA);
        ++actions;
        moq_action_cleanup(&action);
    }
    CHECK(actions == (valid ? 2u : 0u));
    CHECK(moq_session_state(s) == MOQ_SESS_ESTABLISHED);
    close_pair(&f);
}

/* §3.7: an unknown Mandatory Track Property (0x4000-0x7FFF), in the mutable
 * list or inside IMMUTABLE_PROPERTIES, is never processed. PUBLISH (kind 0) is
 * refused on its request stream with REQUEST_ERROR UNSUPPORTED_EXTENSION and
 * a FIN, with no PUBLISH_REQUEST; SUBSCRIBE_OK (1) and FETCH_OK (2) cancel the
 * request -- the request stream is aborted, RESET_STREAM and STOP_SENDING
 * (§6.4.2.3) -- and surface SUBSCRIBE_ERROR / FETCH_ERROR with
 * UNSUPPORTED_EXTENSION. The session stays up in every case. */
static void mandatory_incoming(moq_bytes_t properties, int kind)
{
    ++rows;
    fixture_t f = {0};
    open_pair(&f);
    moq_session_t *s = kind == 0 ? moq_simpair_server(f.pair) : moq_simpair_client(f.pair);
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7000);
    moq_bytes_t part = MOQ_BYTES_LITERAL("n");
    if (kind == 1) {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        moq_subscription_t sub;
        REQUIRE(moq_session_subscribe(s, &cfg, 10, &sub) == MOQ_OK);
    } else if (kind == 2) {
        moq_fetch_cfg_t cfg;
        moq_fetch_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){&part, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        cfg.end_group = 1;
        moq_fetch_t fetch;
        REQUIRE(moq_session_fetch(s, &cfg, 10, &fetch) == MOQ_OK);
    }
    if (kind != 0) {
        moq_action_t a;
        REQUIRE(moq_session_poll_actions(s, &a, 1) == 1);
        REQUIRE(a.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
        ref = a.u.open_bidi_stream.stream_ref;
        moq_action_cleanup(&a);
    }
    static const uint8_t publication[] = {0x1d, 0, 8, 0, 1, 1, 'n', 1, 't', 1, 0};
    static const uint8_t sub_ok[] = {4, 0, 2, 1, 0};
    /* End of Track 0, End Location {1, 2^64-1}, no parameters. */
    static const uint8_t fetch_ok[] = {0x18, 0, 12, 0, 1,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    const uint8_t *head = kind == 0 ? publication : kind == 1 ? sub_ok : fetch_ok;
    size_t prefix = kind == 0 ? sizeof(publication) : kind == 1 ? sizeof(sub_ok)
                                                                : sizeof(fetch_ok);
    uint8_t wire[96];
    REQUIRE(prefix + properties.len <= sizeof(wire));
    memcpy(wire, head, prefix);
    wire[2] = (uint8_t)(prefix - 3 + properties.len);
    memcpy(wire + prefix, properties.data, properties.len);
    CHECK(moq_session_on_bidi_stream_bytes(s, ref, wire, prefix + properties.len,
                                           false, 20) == MOQ_OK);
    CHECK(moq_session_state(s) == MOQ_SESS_ESTABLISHED);

    unsigned refusals = 0, aborts = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) {
        CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        if (kind == 0 && a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref._v) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.request_type = MOQ_D22_PUBLISH;
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                env.msg_type == MOQ_D22_REQUEST_ERROR &&
                moq_d22_decode_message(MOQ_D22_REQUEST_ERROR, env.payload,
                                       env.payload_len, &ctx, &m) == MOQ_OK &&
                m.code == MOQ_REQUEST_ERROR_UNSUPPORTED_EXTENSION &&
                a.u.send_bidi_stream.fin)
                ++refusals;
        }
        if (kind != 0 && a.kind == MOQ_ACTION_ABORT_BIDI_STREAM &&
            a.u.abort_bidi_stream.stream_ref._v == ref._v)
            ++aborts;
        moq_action_cleanup(&a);
    }
    CHECK(refusals == (kind == 0 ? 1u : 0u));
    CHECK(aborts == (kind == 0 ? 0u : 1u));

    unsigned terminals = 0;
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1)) {
        CHECK(e.kind != MOQ_EVENT_PUBLISH_REQUEST && e.kind != MOQ_EVENT_SUBSCRIBE_OK &&
              e.kind != MOQ_EVENT_FETCH_OK);
        if (kind == 1 && e.kind == MOQ_EVENT_SUBSCRIBE_ERROR &&
            e.u.subscribe_error.error_code == MOQ_REQUEST_ERROR_UNSUPPORTED_EXTENSION)
            ++terminals;
        if (kind == 2 && e.kind == MOQ_EVENT_FETCH_ERROR &&
            e.u.fetch_error.error_code == MOQ_REQUEST_ERROR_UNSUPPORTED_EXTENSION)
            ++terminals;
        moq_event_cleanup(&e);
    }
    CHECK(terminals == (kind == 0 ? 0u : 1u));
    close_pair(&f);
}

int main(void)
{
    const uint8_t order0[] = {0x22,0}, order3[] = {0x22,3};
    const uint8_t wrapped_order3[] = {0x0b,2,0x22,3}, wrapped_order0[] = {0x0b,2,0x22,0};
    const uint8_t dynamic2[] = {0x30,2}, wrapped_dynamic2[] = {0x0b,2,0x30,2};
    /* Inner types are deltas: 0x0e + 0x14 = order, 0x0e + 0x22 = dynamic. */
    const uint8_t priority_order3[] = {0x0b,4,0x0e,42,0x14,3};
    const uint8_t priority_dynamic2[] = {0x0b,4,0x0e,42,0x22,2};
    const struct { const char *name; moq_bytes_t properties; } invalid[] = {
        {"order0",BYTES(order0)}, {"order3",BYTES(order3)},
        {"immutable-order3",BYTES(wrapped_order3)}, {"dynamic2",BYTES(dynamic2)},
        {"immutable-dynamic2",BYTES(wrapped_dynamic2)},
        {"retained-priority-order3",BYTES(priority_order3)},
        {"retained-priority-dynamic2",BYTES(priority_dynamic2)},
        {"immutable-order0",BYTES(wrapped_order0)}
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        label = invalid[i].name;
        for (unsigned variant = 0; variant < 2; ++variant) {
            local_refusal(invalid[i].properties, false, variant != 0);
            local_refusal(invalid[i].properties, true, variant != 0);
            incoming(invalid[i].properties, variant != 0, false);
            local_fetch(invalid[i].properties, variant != 0, false);
        }
        codec_refusal(invalid[i].properties);
    }
    const uint8_t ascending[] = {0x22,1}, descending[] = {0x22,2};
    const uint8_t dynamic0[] = {0x30,0}, dynamic1[] = {0x30,1};
    const uint8_t wrapped_low[] = {0x0b,4,0x22,1,0x0e,0};
    const uint8_t wrapped_high[] = {0x80,0x0b,0x80,10,0x0e,42,0x80,0x14,0x80,2,0x0e,1,0x48,7};
    const uint8_t unknown[] = {0x78,7,1,2,0x22,3};
    moq_bytes_t none = {NULL,0};
    const struct { const char *name; moq_bytes_t properties, omitted; } valid[] = {
        {"empty",{NULL,0},{NULL,0}}, {"ascending",BYTES(ascending),{NULL,0}},
        {"descending",BYTES(descending),BYTES(descending)},
        {"dynamic0",BYTES(dynamic0),{NULL,0}}, {"dynamic1",BYTES(dynamic1),{NULL,0}},
        {"immutable-low-endpoints",BYTES(wrapped_low),{NULL,0}},
        {"immutable-nonminimal-high-endpoints",BYTES(wrapped_high),BYTES(wrapped_high)},
        {"unknown-even-and-opaque-odd",BYTES(unknown),{NULL,0}}
    };
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
        label = valid[i].name;
        encode_control(valid[i].properties, valid[i].properties, false, MOQ_VERSION_DRAFT_22);
        encode_control(valid[i].properties, valid[i].omitted, true, MOQ_VERSION_DRAFT_22);
        incoming(valid[i].properties, false, true);
        incoming(valid[i].properties, true, true);
        for (unsigned variant = 0; variant < 2; ++variant) {
            local_success(valid[i].properties,
                variant ? valid[i].omitted : valid[i].properties, false, variant != 0);
            local_success(valid[i].properties, none, true, variant != 0);
            local_fetch(valid[i].properties, variant != 0, true);
        }
    }
    /* Mandatory range endpoints (vi64 3-byte form 0xC0..: 0x4000 even with
     * value 1, 0x7FFF odd with an empty value), one reached by delta after a
     * known property, and one inside IMMUTABLE_PROPERTIES. */
    const uint8_t mandatory_low[] = {0xc0,0x40,0x00,1};
    const uint8_t mandatory_high[] = {0xc0,0x7f,0xff,0};
    const uint8_t mandatory_after_order[] = {0x22,1,0xbf,0xde,5};
    const uint8_t mandatory_immutable[] = {0x0b,4,0xc0,0x40,0x00,1};
    const struct { const char *name; moq_bytes_t properties; } mandatory[] = {
        {"mandatory-0x4000",BYTES(mandatory_low)}, {"mandatory-0x7fff",BYTES(mandatory_high)},
        {"mandatory-after-order",BYTES(mandatory_after_order)},
        {"mandatory-immutable",BYTES(mandatory_immutable)}
    };
    for (size_t i = 0; i < sizeof(mandatory)/sizeof(mandatory[0]); ++i) {
        label = mandatory[i].name;
        for (int kind = 0; kind < 3; ++kind) mandatory_incoming(mandatory[i].properties, kind);
    }
    /* Neighbours of the range are ordinary unknown properties. */
    const uint8_t below_mandatory[] = {0xbf,0xff,1,7}, above_mandatory[] = {0xc0,0x80,0x00,0};
    const struct { const char *name; moq_bytes_t properties; } neighbours[] = {
        {"unknown-0x3fff",BYTES(below_mandatory)}, {"unknown-0x8000",BYTES(above_mandatory)}
    };
    for (size_t i = 0; i < sizeof(neighbours)/sizeof(neighbours[0]); ++i) {
        label = neighbours[i].name;
        incoming(neighbours[i].properties, false, true);
        incoming(neighbours[i].properties, true, true);
    }
    label = "legacy-order3-characterization-not-conformance";
    for (unsigned legacy = 0; legacy < 2; ++legacy) {
        moq_version_t version = legacy ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_16;
        encode_control(BYTES(order3), BYTES(order3), false, version);
        encode_control(BYTES(order3), none, true, version);
    }
    printf("d22_track_property_ranges rows=%u failures=%u\n", rows, failures);
    return failures ? 1 : 0;
}
