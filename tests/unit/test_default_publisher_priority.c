/* Draft-22 10.4/11.2.1/11.3.1 and draft-18 12.4: omitted data priority
 * inherits property 0x0e; absence means 128. Draft-16 remains unchanged.
 * Peer control/data bytes and expected priorities are independent literals.
 * No private headers, counterfeit sessions, encoders, or internal counters. */
#include <moq/moq.h>
#include "test_binding_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static size_t checks, rows;
static const char *label;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL[%s] %s:%d: %s\n", label, __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FIXTURE[%s] %s:%d: %s\n", label, __FILE__, __LINE__, #expr); \
        exit(2); \
    } \
    checks++; \
} while (0)

typedef struct { size_t blocks, bytes; } balance_t;

static void *count_alloc(size_t size, void *ctx)
{
    balance_t *b = ctx;
    void *p = malloc(size ? size : 1);
    if (p) { b->blocks++; b->bytes += size; }
    return p;
}

static void count_free(void *p, size_t size, void *ctx)
{
    balance_t *b = ctx;
    if (!p) return;
    REQUIRE(b->blocks > 0 && b->bytes >= size);
    b->blocks--; b->bytes -= size;
    free(p);
}

static void *count_realloc(void *p, size_t old_size, size_t size, void *ctx)
{
    balance_t *b = ctx;
    if (!p) return count_alloc(size, ctx);
    if (!size) { count_free(p, old_size, ctx); return NULL; }
    REQUIRE(b->blocks > 0 && b->bytes >= old_size);
    void *q = realloc(p, size);
    if (q) b->bytes = b->bytes - old_size + size;
    return q;
}

typedef struct {
    moq_session_t *s;
    balance_t balance;
    uint64_t now, next_stream;
    moq_version_t version;
    moq_stream_ref_t retry_stream;
    bool retry_pending;
} rig_t;

typedef struct {
    moq_subscription_t sub;
    moq_publication_t pub;
    moq_stream_ref_t ref;
} owner_t;

static const uint8_t direct_zero[] = {0x0e, 0};
static const uint8_t direct_last[] = {0x0e, 0x80, 0xff};
static const uint8_t immutable_zero[] = {0x0b, 2, 0x0e, 0};
static const uint8_t immutable_last[] = {0x0b, 3, 0x0e, 0x80, 0xff};
static const uint8_t invalid_direct[] = {0x0e, 0x81, 0}; /* vi64(256) */
static const uint8_t invalid_immutable[] = {0x0b, 3, 0x0e, 0x81, 0};
static const uint8_t direct_middle[] = {0x0e, 0x80, 0x80};
static const uint8_t mixed_immutable[] = {2, 5, 9, 5, 0x0e, 0x80, 0xff, 0x2a, 9};
static const uint8_t projected_immutable[] = {0x0b, 5, 0x0e, 0x80, 0xff, 0x2a, 9};
/* Draft-16's Track Extensions use QUIC varints, not vi64. */
static const uint8_t d16_default_last[] = {0x0e, 0x40, 0xff};

typedef struct { const char *name; moq_bytes_t bytes; uint8_t priority; } property_t;
static const property_t properties[] = {
    {"absent", {NULL, 0}, 128},
    {"direct0", {direct_zero, sizeof(direct_zero)}, 0},
    {"direct255", {direct_last, sizeof(direct_last)}, 255},
    {"immutable0", {immutable_zero, sizeof(immutable_zero)}, 0},
    {"immutable255", {immutable_last, sizeof(immutable_last)}, 255}
};

static bool bytes_equal(moq_bytes_t a, moq_bytes_t b)
{
    return a.len == b.len && (!a.len || (a.data && b.data &&
           memcmp(a.data, b.data, a.len) == 0));
}

static void discard_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) == 1) {
        CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        moq_action_cleanup(&a);
    }
}

static void rig_open_settings(rig_t *r, moq_version_t version,
                              uint32_t subscriptions, bool streaming,
                              uint32_t max_events)
{
    memset(r, 0, sizeof(*r));
    r->version = version; r->now = 1; r->next_stream = 0x5000;
    moq_alloc_t alloc = {.ctx = &r->balance, .alloc = count_alloc,
                        .realloc = count_realloc, .free = count_free};
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), &alloc, MOQ_PERSPECTIVE_CLIENT);
    cfg.version = version;
    cfg.max_subscriptions = subscriptions;
    cfg.max_publishes = 2;
    cfg.max_data_streams = 4;
    cfg.streaming_objects = streaming;
    cfg.max_events = max_events;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 32;
    REQUIRE(moq_session_create(&cfg, 0, &r->s) == MOQ_OK && r->s);
    CHECK(moq_session_version(r->s) == version);
    REQUIRE(test_bind_if_required(r->s) == MOQ_OK);
    REQUIRE(moq_session_start(r->s, r->now) == MOQ_OK);
    discard_actions(r->s);
    /* Empty server SETUP; only draft-16 needs MAX_REQUEST_ID=32. */
    static const uint8_t modern[] = {0xaf, 0, 0, 0};
    static const uint8_t legacy[] = {0x21, 0, 1, 0, 0x15, 0, 1, 32};
    moq_bytes_t wire = version == MOQ_VERSION_DRAFT_16
        ? (moq_bytes_t){legacy, sizeof(legacy)}
        : (moq_bytes_t){modern, sizeof(modern)};
    REQUIRE(moq_session_on_control_bytes(r->s, wire.data, wire.len, ++r->now) == MOQ_OK);
    REQUIRE(moq_session_state(r->s) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_version(r->s) == version);
    moq_event_t e;
    size_t setup = 0;
    while (moq_session_poll_events(r->s, &e, 1) == 1) {
        CHECK(e.kind == MOQ_EVENT_SETUP_COMPLETE);
        if (e.kind == MOQ_EVENT_SETUP_COMPLETE) {
            setup++;
            CHECK(e.u.setup_complete.local_perspective == MOQ_PERSPECTIVE_CLIENT);
            CHECK(e.u.setup_complete.peer_perspective == MOQ_PERSPECTIVE_SERVER);
        }
        moq_event_cleanup(&e);
    }
    REQUIRE(setup == 1);
    discard_actions(r->s);
}

static void rig_open(rig_t *r, moq_version_t version, uint32_t subscriptions)
{
    rig_open_settings(r, version, subscriptions, false, 0);
}

static void rig_close(rig_t *r)
{
    moq_event_t e;
    while (moq_session_poll_events(r->s, &e, 1) == 1) moq_event_cleanup(&e);
    moq_action_t a;
    while (moq_session_poll_actions(r->s, &a, 1) == 1) moq_action_cleanup(&a);
    moq_session_destroy(r->s);
    CHECK(r->balance.blocks == 0);
    CHECK(r->balance.bytes == 0);
    rows++;
}

static owner_t subscribe_start(rig_t *r)
{
    owner_t o = {0};
    static const moq_bytes_t parts[] = {{(const uint8_t *)"n", 1}};
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    REQUIRE(moq_session_subscribe(r->s, &cfg, ++r->now, &o.sub) == MOQ_OK);
    REQUIRE(moq_subscription_is_valid(o.sub));
    moq_action_t a;
    size_t opened = 0, control = 0;
    while (moq_session_poll_actions(r->s, &a, 1) == 1) {
        if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            o.ref = a.u.open_bidi_stream.stream_ref; opened++;
            CHECK(a.u.open_bidi_stream.len > 3 && a.u.open_bidi_stream.data[0] == 3);
        } else if (a.kind == MOQ_ACTION_SEND_CONTROL) {
            control++;
            CHECK(a.u.send_control.len > 3 && a.u.send_control.data[0] == 3);
        } else CHECK(false);
        moq_action_cleanup(&a);
    }
    REQUIRE(r->version == MOQ_VERSION_DRAFT_16 ? control == 1 : opened == 1);
    return o;
}

static moq_result_t subscribe_response(rig_t *r, owner_t o, moq_bytes_t props)
{
    uint8_t wire[32] = {4, 0, 0};
    size_t n = 3;
    /* The draft-16 control response includes the first local Request ID=0. */
    if (r->version == MOQ_VERSION_DRAFT_16) wire[n++] = 0;
    wire[n++] = 7; wire[n++] = 0; /* Track Alias, parameter count */
    REQUIRE(props.len <= sizeof(wire) - n);
    if (props.len) memcpy(wire + n, props.data, props.len);
    n += props.len; wire[2] = (uint8_t)(n - 3);
    if (r->version == MOQ_VERSION_DRAFT_16)
        return moq_session_on_control_bytes(r->s, wire, n, ++r->now);
    return moq_session_on_bidi_stream_bytes(r->s, o.ref, wire, n, false, ++r->now);
}

static void subscribe_ok(rig_t *r, owner_t o, moq_bytes_t props)
{
    REQUIRE(subscribe_response(r, o, props) == MOQ_OK);
    moq_event_t e;
    REQUIRE(moq_session_poll_events(r->s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_OK);
    CHECK(moq_subscription_eq(e.u.subscribe_ok.sub, o.sub));
    CHECK(e.u.subscribe_ok.track_alias == 7);
    CHECK(bytes_equal(e.u.subscribe_ok.track_properties, props));
    moq_event_cleanup(&e);
}

static moq_result_t publish_request(rig_t *r, moq_bytes_t props)
{
    uint8_t wire[32] = {0x1d, 0, 0, 1, 1, 1, 'n', 1, 't', 7, 0};
    size_t n = 11;
    REQUIRE(props.len <= sizeof(wire) - n);
    if (props.len) memcpy(wire + n, props.data, props.len);
    n += props.len; wire[2] = (uint8_t)(n - 3);
    return moq_session_on_bidi_stream_bytes(r->s, moq_stream_ref_from_u64(0x4000),
                                           wire, n, false, ++r->now);
}

static owner_t publish_start(rig_t *r, moq_bytes_t props)
{
    owner_t o = {0};
    REQUIRE(publish_request(r, props) == MOQ_OK);
    moq_event_t e;
    REQUIRE(moq_session_poll_events(r->s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_PUBLISH_REQUEST);
    o.pub = e.u.publish_request.pub;
    REQUIRE(moq_publication_is_valid(o.pub));
    CHECK(e.u.publish_request.track_alias == 7);
    CHECK(e.u.publish_request.forward);
    CHECK(bytes_equal(e.u.publish_request.track_properties, props));
    moq_event_cleanup(&e);
    return o;
}

static void publish_accept(rig_t *r, owner_t o)
{
    moq_accept_publish_cfg_t cfg;
    moq_accept_publish_cfg_init_sized(&cfg, sizeof(cfg));
    REQUIRE(moq_session_accept_publish(r->s, o.pub, &cfg, ++r->now) == MOQ_OK);
    discard_actions(r->s);
}

/* A complete one-byte payload on each ingress. Small location fields have
 * identical literal encodings in all three drafts. No data-plane encoder. */
static void data_pair(rig_t *r, int wire_priority, bool fin)
{
    uint8_t dg[6] = {0x08, 7, 9, 3};
    size_t dn = 4;
    if (wire_priority >= 0) { dg[0] = 0; dg[dn++] = (uint8_t)wire_priority; }
    dg[dn++] = 0xd3;
    REQUIRE(moq_session_on_datagram(r->s, dg, dn, ++r->now) == MOQ_OK);
    uint8_t sg[8] = {0x30, 7, 10};
    size_t sn = 3;
    if (wire_priority >= 0) { sg[0] = 0x10; sg[sn++] = (uint8_t)wire_priority; }
    sg[sn++] = 0; sg[sn++] = 1; sg[sn++] = 0x53;
    REQUIRE(moq_session_on_data_bytes(r->s, moq_stream_ref_from_u64(r->next_stream++),
                                     sg, sn, fin, ++r->now) == MOQ_OK);
}

static int owner_index(const owner_t *owners, size_t n, moq_subscription_t sub,
                       moq_publication_t pub)
{
    for (size_t i = 0; i < n; i++) {
        if (moq_subscription_is_valid(sub) && moq_subscription_eq(owners[i].sub, sub))
            return (int)i;
        if (moq_publication_is_valid(pub) && moq_publication_eq(owners[i].pub, pub))
            return (int)i;
    }
    return -1;
}

static size_t poll_data_event(rig_t *r, moq_event_t *e)
{
    size_t n = moq_session_poll_events(r->s, e, 1);
    if (n && r->retry_pending) {
        moq_result_t rc = moq_session_on_data_bytes(r->s, r->retry_stream,
            NULL, 0, false, ++r->now);
        CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        if (rc == MOQ_OK) r->retry_pending = false;
    }
    return n;
}

static void objects(rig_t *r, const owner_t *owners, const uint8_t *priorities,
                    size_t n)
{
    size_t datagrams[2] = {0}, streams[2] = {0};
    REQUIRE(n <= 2);
    moq_event_t e;
    while (poll_data_event(r, &e) == 1) {
        if (e.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *o = &e.u.object_received;
            int at = owner_index(owners, n, o->sub, o->pub);
            CHECK(at >= 0);
            if (at >= 0) {
                CHECK(moq_subscription_eq(o->sub, owners[at].sub));
                CHECK(moq_publication_eq(o->pub, owners[at].pub));
                if (o->publisher_priority != priorities[at]) {
                    fprintf(stderr, "PRIORITY[%s] %s owner%u observed=%u expected=%u\n",
                        label, o->datagram ? "datagram" : "subgroup", (unsigned)at,
                        (unsigned)o->publisher_priority, (unsigned)priorities[at]);
                }
                CHECK(o->publisher_priority == priorities[at]);
                CHECK(o->group_id == (o->datagram ? 9u : 10u));
                CHECK(o->object_id == (o->datagram ? 3u : 0u));
                CHECK(o->subgroup_id == 0 && o->status == MOQ_OBJECT_NORMAL);
                CHECK(!o->end_of_group && !o->properties);
                CHECK(o->payload && moq_rcbuf_len(o->payload) == 1);
                if (o->payload && moq_rcbuf_len(o->payload) == 1)
                    CHECK(moq_rcbuf_data(o->payload)[0] == (o->datagram ? 0xd3 : 0x53));
                if (o->datagram) datagrams[at]++; else streams[at]++;
            }
        } else if (e.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
            CHECK(owner_index(owners, n, e.u.subgroup_finished.sub,
                              e.u.subgroup_finished.pub) >= 0);
        } else CHECK(false);
        moq_event_cleanup(&e);
    }
    for (size_t i = 0; i < n; i++) {
        CHECK(datagrams[i] == 1);
        CHECK(streams[i] == 1);
    }
    CHECK(moq_session_version(r->s) == r->version);
    CHECK(moq_session_state(r->s) == MOQ_SESS_ESTABLISHED);
}

static void no_events(rig_t *r)
{
    moq_event_t e;
    while (moq_session_poll_events(r->s, &e, 1) == 1) {
        CHECK(false);
        moq_event_cleanup(&e);
    }
}

static void no_actions(rig_t *r)
{
    moq_action_t a;
    while (moq_session_poll_actions(r->s, &a, 1) == 1) {
        CHECK(false);
        moq_action_cleanup(&a);
    }
}

static void single(moq_version_t version, bool publish, const property_t *p,
                   int wire_priority, bool early)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d%u.%s.%s.wire%d.%s", (unsigned)version,
                   publish ? "publish" : "subscribe", p->name, wire_priority,
                   early ? "before-response" : "established");
    label = name;
    rig_t r; rig_open(&r, version, 1);
    owner_t o = publish ? publish_start(&r, p->bytes) : subscribe_start(&r);
    uint8_t expected = wire_priority < 0 ? p->priority : (uint8_t)wire_priority;
    if (early) {
        data_pair(&r, wire_priority, true);
        if (publish) objects(&r, &o, &expected, 1); else no_events(&r);
    }
    if (publish) publish_accept(&r, o); else subscribe_ok(&r, o, p->bytes);
    if (!early) data_pair(&r, wire_priority, true);
    if (!publish || !early) objects(&r, &o, &expected, 1);
    discard_actions(r.s);
    rig_close(&r);
}

/* Different response-local defaults on a shared alias must not pick the alias
 * index's first owner. The pending owner learns its own default only later. */
static void shared(bool publish, bool pending, int wire_priority)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d22.shared.%s.%s.wire%d",
        publish ? "subscribe-publish" : "subscribe-subscribe",
        pending ? "pending" : "established", wire_priority);
    label = name;
    rig_t r; rig_open(&r, MOQ_VERSION_DRAFT_22, 2);
    owner_t owners[2];
    owners[0] = subscribe_start(&r);
    subscribe_ok(&r, owners[0], properties[1].bytes);
    owners[1] = publish ? publish_start(&r, properties[4].bytes) : subscribe_start(&r);
    uint8_t expected[2] = {0, 255};
    if (wire_priority >= 0) expected[0] = expected[1] = (uint8_t)wire_priority;
    if (publish) {
        if (!pending) publish_accept(&r, owners[1]);
    } else if (!pending) subscribe_ok(&r, owners[1], properties[4].bytes);
    data_pair(&r, wire_priority, true);
    if (!publish && pending) {
        objects(&r, owners, expected, 1);
        subscribe_ok(&r, owners[1], properties[4].bytes);
        objects(&r, owners + 1, expected + 1, 1);
    } else {
        objects(&r, owners, expected, 2);
        if (publish && pending) publish_accept(&r, owners[1]);
    }
    discard_actions(r.s);
    rig_close(&r);
}

/* Cancel a pending recipient with held data, then reuse its physical slot.
 * The new generation gets neither old data nor an old default. Slot bits are
 * the documented public handle layout, not an internal owner-counter seam. */
static void reuse(int wire_priority)
{
    char name[64];
    (void)snprintf(name, sizeof(name), "d22.cancel-reuse.wire%d", wire_priority);
    label = name;
    rig_t r; rig_open(&r, MOQ_VERSION_DRAFT_22, 2);
    owner_t first = subscribe_start(&r);
    subscribe_ok(&r, first, properties[0].bytes);
    owner_t canceled = subscribe_start(&r);
    uint8_t old_priority = wire_priority < 0 ? 128 : (uint8_t)wire_priority;
    data_pair(&r, wire_priority, true);
    objects(&r, &first, &old_priority, 1);
    REQUIRE(moq_session_unsubscribe(r.s, canceled.sub, ++r.now) == MOQ_OK);
    discard_actions(r.s);
    owner_t next = subscribe_start(&r);
    CHECK(next.sub._opaque != canceled.sub._opaque);
    CHECK((next.sub._opaque & UINT64_C(0xffff)) ==
          (canceled.sub._opaque & UINT64_C(0xffff)));
    CHECK(moq_session_unsubscribe(r.s, canceled.sub, ++r.now) == MOQ_ERR_STALE_HANDLE);
    subscribe_ok(&r, next, properties[4].bytes);
    no_events(&r); /* Old held objects cannot move to the new generation. */
    REQUIRE(moq_session_unsubscribe(r.s, first.sub, ++r.now) == MOQ_OK);
    discard_actions(r.s);
    data_pair(&r, wire_priority, true);
    uint8_t expected = wire_priority < 0 ? 255 : (uint8_t)wire_priority;
    objects(&r, &next, &expected, 1);
    rig_close(&r);
}

static void malformed(moq_version_t version, bool publish, bool immutable)
{
    char name[64];
    (void)snprintf(name, sizeof(name), "d%u.%s.invalid256.%s", (unsigned)version,
        publish ? "publish" : "subscribe", immutable ? "immutable" : "direct");
    label = name;
    rig_t r; rig_open(&r, version, 1);
    moq_bytes_t props = immutable
        ? (moq_bytes_t){invalid_immutable, sizeof(invalid_immutable)}
        : (moq_bytes_t){invalid_direct, sizeof(invalid_direct)};
    moq_result_t rc;
    if (publish) rc = publish_request(&r, props);
    else {
        owner_t o = subscribe_start(&r);
        rc = subscribe_response(&r, o, props);
    }
    CHECK(rc == MOQ_OK || rc == MOQ_ERR_PROTO || rc == MOQ_ERR_CLOSED);
    /* 10.4/12.4 forbid accepting this value; they do not themselves specify
     * a unique rejection surface, so do not pin an invented close policy. */
    moq_event_t e;
    size_t successes = 0;
    while (moq_session_poll_events(r.s, &e, 1) == 1) {
        if (e.kind == MOQ_EVENT_SUBSCRIBE_OK || e.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            successes++;
            CHECK(bytes_equal(e.kind == MOQ_EVENT_SUBSCRIBE_OK
                ? e.u.subscribe_ok.track_properties : e.u.publish_request.track_properties,
                props));
        }
        CHECK(e.kind != MOQ_EVENT_OBJECT_RECEIVED);
        moq_event_cleanup(&e);
    }
    if (successes)
        fprintf(stderr, "INVALID[%s] accepted=%zu ingress=%d state=%u\n", label,
                successes, (int)rc, (unsigned)moq_session_state(r.s));
    CHECK(successes == 0);
    rig_close(&r);
}

static void controls(void)
{
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    for (size_t v = 0; v < 2; v++)
        for (int pub = 0; pub < 2; pub++)
            for (int early = 0; early < 2; early++) {
                single(versions[v], pub != 0, &properties[0], -1, early != 0);
                for (size_t p = 0; p < 5; p++) {
                    single(versions[v], pub != 0, &properties[p], 0, early != 0);
                    single(versions[v], pub != 0, &properties[p], 255, early != 0);
                }
            }
    /* Task3562 explicitly holds draft-16 behavior. Its pinned 11.1.1.1 also
     * requires inheritance: these 128 expectations are compatibility controls,
     * NOT draft-16 conformance credit. That existing gap remains deferred. */
    property_t held_legacy = {"held-legacy255",
        {d16_default_last, sizeof(d16_default_last)}, 128};
    for (int early = 0; early < 2; early++)
        for (int wire = -1; wire <= 255; wire += wire < 0 ? 1 : 255) {
            single(MOQ_VERSION_DRAFT_16, false, &properties[0], wire, early != 0);
            single(MOQ_VERSION_DRAFT_16, false, &held_legacy, wire, early != 0);
        }
    for (int pub = 0; pub < 2; pub++)
        for (int pending = 0; pending < 2; pending++) {
            shared(pub != 0, pending != 0, 0);
            shared(pub != 0, pending != 0, 255);
        }
    reuse(0); reuse(255);
}

/* Malformed-Track condition 1 compares an Object's Publisher Priority with
 * that of the previous Object of its Subgroup (draft-22 §12.1, draft-18
 * §2.4.2). K (Track default 0) and L (default 128) share alias 7, and an
 * omitted priority resolves through L while it is there. Objects 0, 1 and 2
 * of Subgroup {10,0} arrive on three streams, object 2 after L ended, so its
 * omitted priority resolves through K. Explicit 128, omitted, omitted:
 * object 2 follows an omission and is the same statement, not a change.
 * Omitted, explicit 128, omitted: object 2 (0 for K) follows the explicit
 * 128 and the Track is malformed. */
static void history(bool explicit_first, bool close_first)
{
    label = close_first ? "d22.history.close-then-destroy"
          : explicit_first ? "d22.history.explicit-omitted-omitted"
                           : "d22.history.omitted-explicit-omitted";
    rig_t r; rig_open(&r, MOQ_VERSION_DRAFT_22, 2);
    owner_t l = subscribe_start(&r);
    subscribe_ok(&r, l, properties[0].bytes);
    owner_t k = subscribe_start(&r);
    subscribe_ok(&r, k, properties[1].bytes);
    for (uint8_t id = 0; id < 3; id++) {
        if (id == 2) {
            REQUIRE(moq_session_unsubscribe(r.s, l.sub, ++r.now) == MOQ_OK);
            discard_actions(r.s);
        }
        bool explicit_priority = explicit_first ? id == 0 : id == 1;
        uint8_t sg[8];
        size_t n = 0;
        sg[n++] = explicit_priority ? 0x10 : 0x30;
        sg[n++] = 7; sg[n++] = 10;
        if (explicit_priority) sg[n++] = 128;
        sg[n++] = id; sg[n++] = 1; sg[n++] = 0x53;
        /* Left open: streams of one Subgroup ending at different final
         * Objects would be a malformation of their own. */
        REQUIRE(moq_session_on_data_bytes(r.s, moq_stream_ref_from_u64(r.next_stream++),
                                         sg, n, false, ++r.now) == MOQ_OK);
    }
    size_t k_objects = 0, k_malformed = 0;
    moq_event_t e;
    while (moq_session_poll_events(r.s, &e, 1) == 1) {
        if (e.kind == MOQ_EVENT_OBJECT_RECEIVED &&
            moq_subscription_eq(e.u.object_received.sub, k.sub))
            k_objects++;
        if (e.kind == MOQ_EVENT_UNSUBSCRIBED &&
            moq_subscription_eq(e.u.unsubscribed.sub, k.sub) &&
            e.u.unsubscribed.cause == MOQ_REQUEST_TERMINAL_MALFORMED_TRACK)
            k_malformed++;
        moq_event_cleanup(&e);
    }
    discard_actions(r.s);
    if (explicit_first) {
        CHECK(k_malformed == 0);
        CHECK(k_objects == 3);
    } else {
        CHECK(k_malformed == 1);
        CHECK(k_objects == 2);
    }
    /* The shared streams are still open: closing the session releases their
     * recipient lists, and destroying it afterwards releases nothing twice
     * (rig_close checks the allocator balance). */
    if (close_first)
        CHECK(moq_session_close(r.s, 0, "done", ++r.now) == MOQ_OK);
    rig_close(&r);
}

static void inheritance(void)
{
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    for (size_t v = 0; v < 2; v++)
        for (int pub = 0; pub < 2; pub++)
            for (size_t p = 1; p < 5; p++)
                for (int early = 0; early < 2; early++)
                    single(versions[v], pub != 0, &properties[p], -1, early != 0);
    for (int pub = 0; pub < 2; pub++)
        for (int pending = 0; pending < 2; pending++)
            shared(pub != 0, pending != 0, -1);
    reuse(-1);
    history(true, false);
    history(false, false);
    history(true, true);
}

static void chunk_objects(rig_t *r, const owner_t *owners,
                           const uint8_t *priorities, size_t n)
{
    size_t datagrams[2] = {0}, begins[2] = {0}, ends[2] = {0}, bytes[2] = {0};
    REQUIRE(n <= 2);
    moq_event_t e;
    while (poll_data_event(r, &e) == 1) {
        if (e.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *o = &e.u.object_received;
            int at = owner_index(owners, n, o->sub, o->pub);
            REQUIRE(at >= 0);
            CHECK(o->datagram && o->group_id == 9 && o->object_id == 3);
            CHECK(o->publisher_priority == priorities[at]);
            CHECK(o->payload && moq_rcbuf_len(o->payload) == 1);
            if (o->payload && moq_rcbuf_len(o->payload) == 1)
                CHECK(moq_rcbuf_data(o->payload)[0] == 0xd3);
            datagrams[at]++;
        } else if (e.kind == MOQ_EVENT_OBJECT_CHUNK) {
            const moq_object_chunk_event_t *o = &e.u.object_chunk;
            int at = owner_index(owners, n, o->sub, o->pub);
            REQUIRE(at >= 0);
            CHECK(o->group_id == 10 && o->object_id == 0 && o->subgroup_id == 0);
            CHECK(o->terminal == MOQ_OBJECT_TERMINAL_NORMAL && o->error_code == 0);
            if (o->begin) {
                begins[at]++;
                CHECK(o->publisher_priority == priorities[at]);
                CHECK(o->status == MOQ_OBJECT_NORMAL && !o->end_of_group);
                CHECK(o->payload_length == 1 && !o->properties);
            }
            if (o->end) ends[at]++;
            if (o->chunk) {
                CHECK(moq_rcbuf_len(o->chunk) == 1);
                if (moq_rcbuf_len(o->chunk) == 1)
                    CHECK(moq_rcbuf_data(o->chunk)[0] == 0x53);
                bytes[at] += moq_rcbuf_len(o->chunk);
            }
        } else if (e.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
            CHECK(owner_index(owners, n, e.u.subgroup_finished.sub,
                              e.u.subgroup_finished.pub) >= 0);
        } else CHECK(false);
        moq_event_cleanup(&e);
    }
    for (size_t i = 0; i < n; i++) {
        CHECK(datagrams[i] == 1 && begins[i] == 1 && ends[i] == 1 && bytes[i] == 1);
    }
    CHECK(moq_session_state(r->s) == MOQ_SESS_ESTABLISHED);
}

static void chunks(moq_version_t version, bool publish, const property_t *p,
                     int wire_priority, bool early)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d%u.chunks.%s.%s.wire%d.%s",
        (unsigned)version, publish ? "publish" : "subscribe", p->name,
        wire_priority, early ? "before-response" : "established");
    label = name;
    rig_t r; rig_open_settings(&r, version, 1, true, 0);
    owner_t o = publish ? publish_start(&r, p->bytes) : subscribe_start(&r);
    uint8_t expected = wire_priority < 0 ? p->priority : (uint8_t)wire_priority;
    if (early) {
        data_pair(&r, wire_priority, true);
        if (publish) chunk_objects(&r, &o, &expected, 1); else no_events(&r);
    }
    if (publish) publish_accept(&r, o); else subscribe_ok(&r, o, p->bytes);
    if (!early) data_pair(&r, wire_priority, true);
    if (!publish || !early) chunk_objects(&r, &o, &expected, 1);
    discard_actions(r.s);
    rig_close(&r);
}

/* Datagram fanout fills the event queue before the subgroup arrives. The
 * adapter's empty re-drive after each poll must not duplicate a recipient. */
static void shared_pressure(bool publish, bool streaming, int wire_priority)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d22.pressure.%s.%s.wire%d",
        publish ? "subscribe-publish" : "subscribe-subscribe",
        streaming ? "chunks" : "whole", wire_priority);
    label = name;
    rig_t r; rig_open_settings(&r, MOQ_VERSION_DRAFT_22, 2, streaming, 2);
    owner_t owners[2];
    owners[0] = subscribe_start(&r);
    subscribe_ok(&r, owners[0], properties[1].bytes);
    owners[1] = publish ? publish_start(&r, properties[4].bytes) : subscribe_start(&r);
    if (publish) publish_accept(&r, owners[1]);
    else subscribe_ok(&r, owners[1], properties[4].bytes);
    uint8_t expected[2] = {0, 255};
    if (wire_priority >= 0) expected[0] = expected[1] = (uint8_t)wire_priority;
    uint8_t dg[6] = {0x08, 7, 9, 3};
    size_t dn = 4;
    if (wire_priority >= 0) { dg[0] = 0; dg[dn++] = (uint8_t)wire_priority; }
    dg[dn++] = 0xd3;
    REQUIRE(moq_session_on_datagram(r.s, dg, dn, ++r.now) == MOQ_OK);
    uint8_t sg[8] = {0x30, 7, 10};
    size_t sn = 3;
    if (wire_priority >= 0) { sg[0] = 0x10; sg[sn++] = (uint8_t)wire_priority; }
    sg[sn++] = 0; sg[sn++] = 1; sg[sn++] = 0x53;
    r.retry_stream = moq_stream_ref_from_u64(r.next_stream++);
    moq_result_t rc = moq_session_on_data_bytes(r.s,
        r.retry_stream, sg, sn, true, ++r.now);
    CHECK(rc == MOQ_ERR_WOULD_BLOCK);
    r.retry_pending = rc == MOQ_ERR_WOULD_BLOCK;
    if (streaming) chunk_objects(&r, owners, expected, 2);
    else objects(&r, owners, expected, 2);
    CHECK(!r.retry_pending);
    REQUIRE(moq_session_tick(r.s, ++r.now) == MOQ_OK);
    no_events(&r);
    discard_actions(r.s);
    rig_close(&r);
}

static void before_control_pressure(moq_version_t version, bool streaming,
                                     const property_t *p)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d%u.before-control.pressure.%s.%s",
        (unsigned)version, streaming ? "chunks" : "whole", p->name);
    label = name;
    rig_t r; rig_open_settings(&r, version, 1, streaming, 1);
    owner_t o = subscribe_start(&r);
    data_pair(&r, -1, true);
    no_events(&r);
    subscribe_ok(&r, o, p->bytes);
    if (streaming) chunk_objects(&r, &o, &p->priority, 1);
    else objects(&r, &o, &p->priority, 1);
    REQUIRE(moq_session_tick(r.s, ++r.now) == MOQ_OK);
    no_events(&r);
    discard_actions(r.s);
    rig_close(&r);
}

/* A one-event queue parks each shared begin/continuation/end halfway through
 * fanout. Each phase is fully re-driven before the next transport fragment. */
static void fragmented_chunks(bool publish, int wire_priority)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d22.fragmented.%s.wire%d",
        publish ? "subscribe-publish" : "subscribe-subscribe", wire_priority);
    label = name;
    rig_t r; rig_open_settings(&r, MOQ_VERSION_DRAFT_22, 2, true, 1);
    owner_t owners[2];
    owners[0] = subscribe_start(&r);
    subscribe_ok(&r, owners[0], properties[1].bytes);
    owners[1] = publish ? publish_start(&r, properties[4].bytes) : subscribe_start(&r);
    if (publish) publish_accept(&r, owners[1]);
    else subscribe_ok(&r, owners[1], properties[4].bytes);
    uint8_t expected[2] = {0, 255};
    if (wire_priority >= 0) expected[0] = expected[1] = (uint8_t)wire_priority;
    uint8_t header[8] = {0x30, 7, 10};
    size_t hn = 3;
    if (wire_priority >= 0) { header[0] = 0x10; header[hn++] = (uint8_t)wire_priority; }
    header[hn++] = 0; header[hn++] = 3; header[hn++] = 0x53;
    const uint8_t tail[] = {0x54, 0x55};
    size_t begins[2] = {0}, ends[2] = {0}, lengths[2] = {0};
    r.retry_stream = moq_stream_ref_from_u64(r.next_stream++);
    for (size_t phase = 0; phase < 3; phase++) {
        const uint8_t *wire = phase ? &tail[phase - 1] : header;
        size_t len = phase ? 1 : hn;
        moq_result_t rc = moq_session_on_data_bytes(r.s, r.retry_stream,
            wire, len, phase == 2, ++r.now);
        CHECK(rc == MOQ_ERR_WOULD_BLOCK);
        r.retry_pending = rc == MOQ_ERR_WOULD_BLOCK;
        size_t chunks_seen = 0;
        moq_event_t e;
        while (poll_data_event(&r, &e) == 1) {
            if (e.kind == MOQ_EVENT_OBJECT_CHUNK) {
                const moq_object_chunk_event_t *o = &e.u.object_chunk;
                int at = owner_index(owners, 2, o->sub, o->pub);
                REQUIRE(at >= 0);
                chunks_seen++;
                CHECK(o->group_id == 10 && o->subgroup_id == 0 && o->object_id == 0);
                CHECK(o->begin == (phase == 0) && o->end == (phase == 2));
                CHECK(o->terminal == MOQ_OBJECT_TERMINAL_NORMAL && o->error_code == 0);
                if (o->begin) {
                    begins[at]++;
                    CHECK(o->publisher_priority == expected[at]);
                    CHECK(o->payload_length == 3 && o->status == MOQ_OBJECT_NORMAL);
                }
                if (o->end) ends[at]++;
                REQUIRE(o->chunk && moq_rcbuf_len(o->chunk) == 1);
                CHECK(moq_rcbuf_data(o->chunk)[0] == (uint8_t)(0x53 + phase));
                lengths[at]++;
            } else if (e.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
                CHECK(phase == 2);
                CHECK(owner_index(owners, 2, e.u.subgroup_finished.sub,
                                  e.u.subgroup_finished.pub) >= 0);
            } else CHECK(false);
            moq_event_cleanup(&e);
        }
        CHECK(chunks_seen == 2 && !r.retry_pending);
    }
    for (size_t i = 0; i < 2; i++)
        CHECK(begins[i] == 1 && ends[i] == 1 && lengths[i] == 3);
    REQUIRE(moq_session_tick(r.s, ++r.now) == MOQ_OK);
    no_events(&r);
    CHECK(moq_session_state(r.s) == MOQ_SESS_ESTABLISHED);
    discard_actions(r.s);
    rig_close(&r);
}

static void local_invalid(moq_version_t version, bool accept, bool immutable,
                            bool omit)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d%u.local-invalid.%s.%s.omit%u",
        (unsigned)version, accept ? "accept" : "publish",
        immutable ? "immutable" : "direct", (unsigned)omit);
    label = name;
    rig_t r; rig_open(&r, version, 1);
    moq_bytes_t props = immutable
        ? (moq_bytes_t){invalid_immutable, sizeof(invalid_immutable)}
        : (moq_bytes_t){invalid_direct, sizeof(invalid_direct)};
    if (accept) {
        uint8_t wire[] = {3, 0, 7, 1, 1, 1, 'n', 1, 't', 0, 0x35, 0};
        if (omit) { wire[2] = 9; wire[9] = 1; }
        REQUIRE(moq_session_on_bidi_stream_bytes(r.s,
            moq_stream_ref_from_u64(0x4000), wire, omit ? sizeof(wire) : 10,
            false, ++r.now) == MOQ_OK);
        moq_event_t e;
        REQUIRE(moq_session_poll_events(r.s, &e, 1) == 1);
        REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST);
        moq_subscription_t sub = e.u.subscribe_request.sub;
        moq_event_cleanup(&e);
        discard_actions(r.s);
        moq_accept_subscribe_cfg_t cfg;
        moq_accept_subscribe_cfg_init(&cfg);
        cfg.track_properties = props;
        CHECK(moq_session_accept_subscribe(r.s, sub, &cfg, UINT64_MAX) == MOQ_ERR_INVAL);
        no_events(&r);
        no_actions(&r);
        cfg.track_properties = properties[4].bytes;
        REQUIRE(moq_session_accept_subscribe(r.s, sub, &cfg, ++r.now) == MOQ_OK);
        discard_actions(r.s);
    } else {
        static const moq_bytes_t parts[] = {{(const uint8_t *)"n", 1}};
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = (moq_namespace_t){parts, 1};
        cfg.track_name = MOQ_BYTES_LITERAL("t");
        cfg.track_properties = props;
        moq_publication_t out = {._opaque = UINT64_C(0x12345678)};
        CHECK(moq_session_publish(r.s, &cfg, UINT64_MAX, &out) == MOQ_ERR_INVAL);
        CHECK(out._opaque == UINT64_C(0x12345678));
        no_events(&r);
        no_actions(&r);
        cfg.track_properties = properties[4].bytes;
        REQUIRE(moq_session_publish(r.s, &cfg, ++r.now, &out) == MOQ_OK);
        CHECK(moq_publication_is_valid(out));
        discard_actions(r.s);
    }
    CHECK(moq_session_state(r.s) == MOQ_SESS_ESTABLISHED);
    rig_close(&r);
}

static void chunk_suite(void)
{
    for (int v = 18; v <= 22; v += 4)
        for (int pub = 0; pub < 2; pub++)
            for (size_t p = 0; p < 5; p++)
                for (int early = 0; early < 2; early++)
                    for (int wire = -1; wire <= 255; wire += wire < 0 ? 1 : 255)
                        chunks((moq_version_t)v, pub != 0, &properties[p], wire, early != 0);
}

static void pressure_suite(void)
{
    for (int pub = 0; pub < 2; pub++)
        for (int streaming = 0; streaming < 2; streaming++)
            for (int wire = -1; wire <= 255; wire += wire < 0 ? 1 : 255)
                shared_pressure(pub != 0, streaming != 0, wire);
    for (int v = 18; v <= 22; v += 4)
        for (int streaming = 0; streaming < 2; streaming++)
            for (size_t p = 0; p < 3; p++)
                before_control_pressure((moq_version_t)v, streaming != 0, &properties[p]);
    for (int pub = 0; pub < 2; pub++)
        for (int wire = -1; wire <= 255; wire += wire < 0 ? 1 : 255)
            fragmented_chunks(pub != 0, wire);
}

static void local_suite(void)
{
    for (int v = 18; v <= 22; v += 4)
        for (int accept = 0; accept < 2; accept++)
            for (int immutable = 0; immutable < 2; immutable++) {
                local_invalid((moq_version_t)v, accept != 0, immutable != 0, false);
                if (v == 22 && accept)
                    local_invalid((moq_version_t)v, true, immutable != 0, true);
            }
}

static void writer_projection(moq_version_t version, const property_t *p,
                                bool omit, moq_bytes_t projected)
{
    char name[96];
    (void)snprintf(name, sizeof(name), "d%u.writer.%s.omit%u",
        (unsigned)version, p->name, (unsigned)omit);
    label = name;
    rig_t writer, reader;
    rig_open(&writer, version, 1);
    rig_open(&reader, version, 1);
    owner_t receiver = subscribe_start(&reader);
    uint8_t request[] = {3, 0, 7, 1, 1, 1, 'n', 1, 't', 0, 0x35, 0};
    if (omit) { request[2] = 9; request[9] = 1; }
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x4000);
    REQUIRE(moq_session_on_bidi_stream_bytes(writer.s, ref, request,
        omit ? sizeof(request) : 10, false, ++writer.now) == MOQ_OK);
    moq_event_t e;
    REQUIRE(moq_session_poll_events(writer.s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST);
    moq_subscription_t publisher = e.u.subscribe_request.sub;
    moq_event_cleanup(&e);
    discard_actions(writer.s);
    moq_accept_subscribe_cfg_t cfg;
    moq_accept_subscribe_cfg_init(&cfg);
    cfg.has_track_alias = true; cfg.track_alias = 7;
    cfg.track_properties = p->bytes;
    REQUIRE(moq_session_accept_subscribe(writer.s, publisher, &cfg, ++writer.now) == MOQ_OK);
    uint8_t expected[32] = {4, 0, 0, 7, 0};
    REQUIRE(projected.len <= sizeof(expected) - 5);
    if (projected.len) memcpy(expected + 5, projected.data, projected.len);
    expected[2] = (uint8_t)(projected.len + 2);
    moq_action_t a;
    size_t answers = 0;
    while (moq_session_poll_actions(writer.s, &a, 1) == 1) {
        REQUIRE(a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        CHECK(a.u.send_bidi_stream.stream_ref._v == 0x4000);
        CHECK(!a.u.send_bidi_stream.fin);
        CHECK(bytes_equal((moq_bytes_t){a.u.send_bidi_stream.data,
            a.u.send_bidi_stream.len}, (moq_bytes_t){expected, projected.len + 5}));
        REQUIRE(moq_session_on_bidi_stream_bytes(reader.s, receiver.ref,
            a.u.send_bidi_stream.data, a.u.send_bidi_stream.len, false,
            ++reader.now) == MOQ_OK);
        answers++;
        moq_action_cleanup(&a);
    }
    REQUIRE(answers == 1);
    REQUIRE(moq_session_poll_events(reader.s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_OK);
    CHECK(moq_subscription_eq(e.u.subscribe_ok.sub, receiver.sub));
    CHECK(e.u.subscribe_ok.track_alias == 7);
    CHECK(bytes_equal(e.u.subscribe_ok.track_properties, projected));
    moq_event_cleanup(&e);
    data_pair(&reader, -1, true);
    objects(&reader, &receiver, &p->priority, 1);
    discard_actions(reader.s);
    rig_close(&writer);
    rig_close(&reader);
}

static void writer_ack_parameters(unsigned combination, bool omit)
{
    const uint8_t supplied[] = {6, 9, 8, 42, 0x14, 2, 0x56, 1};
    /* D22 9.7: expiry0 (08), largest(3,4) (09), or both (delta1).
     * INCLUDE_PROPERTIES=0 retains priority42 and inherited order2 only. */
    const uint8_t expires_full[] = {
        4, 0, 0x0c, 1, 1, 8, 0, 6, 9, 8, 42, 0x14, 2, 0x56, 1
    };
    const uint8_t expires_retained[] = {
        4, 0, 8, 1, 1, 8, 0, 0x0e, 42, 0x14, 2
    };
    const uint8_t largest_full[] = {
        4, 0, 0x0d, 1, 1, 9, 3, 4, 6, 9, 8, 42, 0x14, 2, 0x56, 1
    };
    const uint8_t largest_retained[] = {
        4, 0, 9, 1, 1, 9, 3, 4, 0x0e, 42, 0x14, 2
    };
    const uint8_t both_full[] = {
        4, 0, 0x0f, 1, 2, 8, 0, 1, 3, 4, 6, 9, 8, 42, 0x14, 2, 0x56, 1
    };
    const uint8_t both_retained[] = {
        4, 0, 0x0b, 1, 2, 8, 0, 1, 3, 4, 0x0e, 42, 0x14, 2
    };
    const moq_bytes_t expected[3][2] = {
        {{expires_full, sizeof(expires_full)}, {expires_retained, sizeof(expires_retained)}},
        {{largest_full, sizeof(largest_full)}, {largest_retained, sizeof(largest_retained)}},
        {{both_full, sizeof(both_full)}, {both_retained, sizeof(both_retained)}}
    };
    char name[96];
    (void)snprintf(name, sizeof(name), "d22.writer.ack-parameters%u.omit%u",
                   combination, (unsigned)omit);
    label = name;
    REQUIRE(combination < 3);
    rig_t writer;
    rig_open(&writer, MOQ_VERSION_DRAFT_22, 1);
    /* Omit GROUP_ORDER in both requests; only the second disables properties. */
    const uint8_t included_request[] = {3, 0, 7, 1, 1, 1, 'n', 1, 't', 0};
    const uint8_t omitted_request[] = {3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x35, 0};
    moq_bytes_t request = omit
        ? (moq_bytes_t){omitted_request, sizeof(omitted_request)}
        : (moq_bytes_t){included_request, sizeof(included_request)};
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x4000);
    REQUIRE(moq_session_on_bidi_stream_bytes(writer.s, ref, request.data,
        request.len, false, ++writer.now) == MOQ_OK);
    moq_event_t e;
    REQUIRE(moq_session_poll_events(writer.s, &e, 1) == 1);
    REQUIRE(e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST);
    moq_subscription_t publisher = e.u.subscribe_request.sub;
    CHECK(moq_subscription_is_valid(publisher));
    moq_event_cleanup(&e);
    REQUIRE(moq_session_poll_events(writer.s, &e, 1) == 0);
    discard_actions(writer.s);
    moq_accept_subscribe_cfg_t cfg;
    moq_accept_subscribe_cfg_init(&cfg);
    cfg.has_track_alias = true; cfg.track_alias = 1;
    cfg.has_expires = combination != 1; cfg.expires_ms = 0;
    cfg.has_largest = combination != 0; cfg.largest_group = 3; cfg.largest_object = 4;
    cfg.track_properties = (moq_bytes_t){supplied, sizeof(supplied)};
    REQUIRE(moq_session_accept_subscribe(writer.s, publisher, &cfg, ++writer.now) == MOQ_OK);
    moq_action_t a;
    size_t answers = 0;
    while (moq_session_poll_actions(writer.s, &a, 1) == 1) {
        REQUIRE(a.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        CHECK(a.u.send_bidi_stream.stream_ref._v == ref._v);
        CHECK(!a.u.send_bidi_stream.fin);
        CHECK(bytes_equal((moq_bytes_t){a.u.send_bidi_stream.data,
            a.u.send_bidi_stream.len}, expected[combination][omit ? 1 : 0]));
        answers++;
        moq_action_cleanup(&a);
    }
    REQUIRE(answers == 1);
    CHECK(moq_session_state(writer.s) == MOQ_SESS_ESTABLISHED);
    rig_close(&writer);
}

static void projection_suite(void)
{
    const property_t extra[] = {
        {"direct128", {direct_middle, sizeof(direct_middle)}, 128},
        {"mutable-and-immutable255", {mixed_immutable, sizeof(mixed_immutable)}, 255}
    };
    for (int v = 18; v <= 22; v += 4)
        for (size_t i = 0; i < 7; i++) {
            const property_t *p = i < 5 ? &properties[i] : &extra[i - 5];
            writer_projection((moq_version_t)v, p, false, p->bytes);
            if (v == 22) {
                moq_bytes_t projected = p->priority == 128 ? (moq_bytes_t){NULL, 0} :
                    i == 6 ? (moq_bytes_t){projected_immutable, sizeof(projected_immutable)} :
                    p->bytes;
                writer_projection(MOQ_VERSION_DRAFT_22, p, true, projected);
            }
        }
    for (unsigned combination = 0; combination < 3; combination++) {
        writer_ack_parameters(combination, false);
        writer_ack_parameters(combination, true);
    }
}

int main(int argc, char **argv)
{
    const char *mode = argc == 1 ? "all" : argv[1];
    if (argc > 2 || (strcmp(mode, "all") && strcmp(mode, "controls") &&
                    strcmp(mode, "inheritance") && strcmp(mode, "malformed") &&
                    strcmp(mode, "qualification") && strcmp(mode, "chunks") &&
                    strcmp(mode, "pressure") && strcmp(mode, "local") &&
                    strcmp(mode, "projection"))) {
        fprintf(stderr, "usage: %s [all|controls|inheritance|malformed|qualification|chunks|pressure|local|projection]\n", argv[0]);
        return 2;
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "controls")) controls();
    if (!strcmp(mode, "all") || !strcmp(mode, "inheritance")) inheritance();
    if (!strcmp(mode, "all") || !strcmp(mode, "malformed"))
        for (int v = 18; v <= 22; v += 4)
            for (int pub = 0; pub < 2; pub++)
                for (int immutable = 0; immutable < 2; immutable++)
                    malformed((moq_version_t)v, pub != 0, immutable != 0);
    bool qualify = !strcmp(mode, "all") || !strcmp(mode, "qualification");
    if (qualify || !strcmp(mode, "chunks")) chunk_suite();
    if (qualify || !strcmp(mode, "pressure")) pressure_suite();
    if (qualify || !strcmp(mode, "local")) local_suite();
    if (qualify || !strcmp(mode, "projection")) projection_suite();
    printf("default_publisher_priority.%s: rows=%zu checks=%zu failures=%d\n",
           mode, rows, checks, failures);
    return failures ? 1 : 0;
}
