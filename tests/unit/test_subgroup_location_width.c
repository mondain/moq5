/* Public-path subgroup locations, with literal wire oracles from D16 10.4.2,
 * D18 1.4.1/11.4.2 and D22 8.1/11.3.1. No codec or private session access. */
#include <moq/session.h>
#include <moq/sim.h>
#include <moq/rcbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, checks;
static char case_name[128];

static bool check(bool ok, const char *expr, int line)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL %s:%d: %s\n", case_name, line, expr);
        failures++;
    }
    return ok;
}
#define CHECK(expr) check((expr), #expr, __LINE__)
#define REQUIRE(expr) do { if (!CHECK(expr)) return false; } while (0)
#define NEED(expr) do { if (!CHECK(expr)) goto done; } while (0)

typedef struct memory {
    size_t allocations, reallocations, frees;
    int64_t live;
} memory_t;

static void *count_alloc(size_t n, void *ctx)
{
    memory_t *m = ctx;
    m->allocations++;
    void *p = malloc(n);
    if (p) m->live++;
    return p;
}

static void *count_realloc(void *p, size_t old, size_t n, void *ctx)
{
    memory_t *m = ctx;
    (void)old;
    m->reallocations++;
    if (!n) {
        if (p) m->live--;
        free(p);
        return NULL;
    }
    bool fresh = p == NULL;
    void *q = realloc(p, n);
    if (q && fresh) m->live++;
    return q;
}

static void count_free(void *p, size_t n, void *ctx)
{
    memory_t *m = ctx;
    (void)n;
    m->frees++;
    if (p) m->live--;
    free(p);
}

typedef struct boundary {
    const char *name;
    uint64_t value;
    size_t d16_len, wide_len;
    uint8_t d16[8], wide[9];
} boundary_t;

static const boundary_t boundaries[] = {
    {"zero", 0, 1, 1, {0}, {0}},
    {"max62", UINT64_C(0x3fffffffffffffff), 8, 9,
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
     {0xff, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
    {"first64", UINT64_C(0x4000000000000000), 0, 9, {0},
     {0xff, 0x40, 0, 0, 0, 0, 0, 0, 0}},
    {"max64", UINT64_MAX, 0, 9, {0},
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
};

enum { TRACK_ALIAS = 37, PRIORITY = 43, OBJECT_ID = 7 };
static const uint8_t payload_bytes[] = {0x61, 0, 0x7f, 0x80, 0xff, 0x62};

typedef struct fixture {
    memory_t memory;
    moq_alloc_t alloc;
    moq_simpair_t *pair;
    moq_session_t *sender, *receiver;
    moq_subscription_t send_sub, recv_sub;
    moq_publication_t send_pub, recv_pub;
    moq_version_t version;
    bool publication;
} fixture_t;

static uint64_t now(const fixture_t *f)
{
    return moq_simpair_now_us(f->pair);
}

static bool pump(fixture_t *f)
{
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 64, NULL) == MOQ_OK);
    return true;
}

static bool take_event(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t ev;
        size_t n = 0;
        REQUIRE(moq_session_poll_events_ex(s, &ev, 1, sizeof(ev), &n) == MOQ_OK);
        if (!n) break;
        if (ev.kind == kind) { *out = ev; return true; }
        bool closed = ev.kind == MOQ_EVENT_SESSION_CLOSED;
        moq_event_cleanup(&ev);
        REQUIRE(!closed);
    }
    REQUIRE(false);
    return false;
}

static bool drain_events(moq_session_t *s)
{
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t ev;
        size_t n = 0;
        REQUIRE(moq_session_poll_events_ex(s, &ev, 1, sizeof(ev), &n) == MOQ_OK);
        if (!n) return true;
        bool closed = ev.kind == MOQ_EVENT_SESSION_CLOSED;
        moq_event_cleanup(&ev);
        REQUIRE(!closed);
    }
    REQUIRE(false);
    return false;
}

static bool no_actions(moq_session_t *s)
{
    moq_action_t a;
    size_t n = 0;
    REQUIRE(moq_session_poll_actions_ex(s, &a, 1, sizeof(a), &n) == MOQ_OK);
    if (n) moq_action_cleanup(&a);
    REQUIRE(n == 0);
    return true;
}

static bool start(fixture_t *f, moq_version_t version, bool publication)
{
    f->version = version;
    f->publication = publication;
    f->alloc = (moq_alloc_t){.ctx = &f->memory, .alloc = count_alloc,
        .realloc = count_realloc, .free = count_free};
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = &f->alloc;
    pc.version = version;
    pc.initial_now_us = 1000;
    pc.seed = 3778;
    pc.client_send_request_capacity = pc.server_send_request_capacity = true;
    pc.client_initial_request_capacity = pc.server_initial_request_capacity = 16;
    REQUIRE(moq_simpair_create(&pc, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK && pump(f));
    f->sender = moq_simpair_server(f->pair);
    f->receiver = moq_simpair_client(f->pair);
    REQUIRE(moq_session_version(f->sender) == version);
    REQUIRE(moq_session_version(f->receiver) == version);
    REQUIRE(moq_session_state(f->sender) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(f->receiver) == MOQ_SESS_ESTABLISHED);
    REQUIRE(drain_events(f->sender) && drain_events(f->receiver));
    moq_bytes_t part = MOQ_BYTES_LITERAL("subgroup-width");
    moq_namespace_t ns = {&part, 1};
    moq_event_t ev;
    if (publication) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        cfg.has_track_alias = true;
        cfg.track_alias = TRACK_ALIAS;
        REQUIRE(moq_session_publish(f->sender, &cfg, now(f), &f->send_pub) == MOQ_OK);
        REQUIRE(pump(f) && take_event(f->receiver, MOQ_EVENT_PUBLISH_REQUEST, &ev));
        f->recv_pub = ev.u.publish_request.pub;
        bool alias_ok = ev.u.publish_request.track_alias == TRACK_ALIAS;
        moq_event_cleanup(&ev);
        REQUIRE(alias_ok && moq_publication_is_valid(f->recv_pub));
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init_sized(&ac, sizeof(ac));
        REQUIRE(moq_session_accept_publish(f->receiver, f->recv_pub, &ac, now(f)) == MOQ_OK);
        REQUIRE(pump(f) && take_event(f->sender, MOQ_EVENT_PUBLISH_OK, &ev));
        bool accepted = moq_publication_eq(ev.u.publish_ok.pub, f->send_pub)
            && ev.u.publish_ok.send_allowed;
        moq_event_cleanup(&ev);
        REQUIRE(accepted);
    } else {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = ns;
        cfg.track_name = MOQ_BYTES_LITERAL("v");
        REQUIRE(moq_session_subscribe(f->receiver, &cfg, now(f), &f->recv_sub) == MOQ_OK);
        REQUIRE(pump(f) && take_event(f->sender, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
        f->send_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        REQUIRE(moq_subscription_is_valid(f->send_sub));
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        ac.has_track_alias = true;
        ac.track_alias = TRACK_ALIAS;
        REQUIRE(moq_session_accept_subscribe(f->sender, f->send_sub, &ac, now(f)) == MOQ_OK);
        REQUIRE(pump(f) && take_event(f->receiver, MOQ_EVENT_SUBSCRIBE_OK, &ev));
        bool accepted = moq_subscription_eq(ev.u.subscribe_ok.sub, f->recv_sub)
            && ev.u.subscribe_ok.track_alias == TRACK_ALIAS;
        moq_event_cleanup(&ev);
        REQUIRE(accepted);
    }
    REQUIRE(drain_events(f->sender) && drain_events(f->receiver));
    REQUIRE(no_actions(f->sender) && no_actions(f->receiver));
    return true;
}

static moq_result_t open_subgroup(fixture_t *f, const moq_subgroup_cfg_t *cfg,
    bool invalid_owner, uint64_t at, moq_subgroup_handle_t *out)
{
    return f->publication
        ? moq_session_open_pub_subgroup(f->sender,
            invalid_owner ? MOQ_PUBLICATION_INVALID : f->send_pub, cfg, at, out)
        : moq_session_open_subgroup(f->sender,
            invalid_owner ? MOQ_SUBSCRIPTION_INVALID : f->send_sub, cfg, at, out);
}

static bool take_data(fixture_t *f, moq_action_t *a)
{
    size_t n = 0;
    REQUIRE(moq_session_poll_actions_ex(f->sender, a, 1, sizeof(*a), &n) == MOQ_OK);
    REQUIRE(n == 1);
    if (a->kind != MOQ_ACTION_SEND_DATA) {
        moq_action_cleanup(a);
        REQUIRE(false);
    }
    return true;
}

static bool header_matches(const fixture_t *f, const moq_action_t *a,
    const boundary_t *group, const boundary_t *subgroup)
{
    bool legacy = f->version == MOQ_VERSION_DRAFT_16;
    size_t gn = legacy ? group->d16_len : group->wide_len;
    size_t sn = legacy ? subgroup->d16_len : subgroup->wide_len;
    uint8_t expected[21] = {0x14, TRACK_ALIAS};
    REQUIRE(gn && sn && 3 + gn + sn <= sizeof(expected));
    memcpy(expected + 2, legacy ? group->d16 : group->wide, gn);
    memcpy(expected + 2 + gn, legacy ? subgroup->d16 : subgroup->wide, sn);
    expected[2 + gn + sn] = PRIORITY;
    REQUIRE(a->u.send_data.header_len == 3 + gn + sn);
    REQUIRE(memcmp(a->u.send_data.header, expected, 3 + gn + sn) == 0);
    REQUIRE(a->u.send_data.payload == NULL && !a->u.send_data.fin);
    return true;
}

/* Only genuine outgoing actions are fed, unchanged, to the established peer.
 * The test owns this one data-stream mapping; SimPair owns the control paths. */
static bool deliver(fixture_t *f, const moq_action_t *a, moq_stream_ref_t stream)
{
    const moq_send_data_action_t *d = &a->u.send_data;
    REQUIRE(d->stream_ref._v == stream._v);
    if (d->header_len)
        REQUIRE(moq_session_on_data_bytes(f->receiver, stream, d->header,
            d->header_len, d->fin && !d->payload, now(f)) == MOQ_OK);
    if (d->payload)
        REQUIRE(moq_session_on_data_bytes(f->receiver, stream,
            moq_rcbuf_data(d->payload), moq_rcbuf_len(d->payload),
            d->fin, now(f)) == MOQ_OK);
    if (!d->header_len && !d->payload)
        REQUIRE(moq_session_on_data_bytes(f->receiver, stream, NULL, 0,
            d->fin, now(f)) == MOQ_OK);
    return true;
}

static bool refusal(fixture_t *f, const moq_subgroup_cfg_t *cfg, uint64_t epoch)
{
    const moq_subgroup_handle_t marker = {UINT64_C(0xfedcba9811223344)};
    uint64_t deadline = moq_session_next_deadline_us(f->sender);
    memory_t before = f->memory;
    REQUIRE(moq_session_borrow_valid(f->sender, epoch));
    for (unsigned invalid_owner = 0; invalid_owner < 2; invalid_owner++) {
        moq_subgroup_handle_t out = marker;
        REQUIRE(open_subgroup(f, cfg, invalid_owner != 0, now(f) + 1000, &out)
            == MOQ_ERR_INVAL);
        REQUIRE(moq_subgroup_eq(out, marker));
        REQUIRE(moq_session_borrow_valid(f->sender, epoch));
        REQUIRE(moq_session_next_deadline_us(f->sender) == deadline);
        REQUIRE(moq_session_state(f->sender) == MOQ_SESS_ESTABLISHED);
        REQUIRE(no_actions(f->sender) && no_actions(f->receiver));
        REQUIRE(f->memory.allocations == before.allocations);
        REQUIRE(f->memory.reallocations == before.reallocations);
        REQUIRE(f->memory.frees == before.frees && f->memory.live == before.live);
    }
    return true;
}

static void run_case(moq_version_t version, bool publication,
    const boundary_t *group, const boundary_t *subgroup)
{
    fixture_t f = {0};
    moq_rcbuf_t *sent = NULL, *received = NULL;
    moq_action_t a = {0};
    bool reject = version == MOQ_VERSION_DRAFT_16 &&
        (!group->d16_len || !subgroup->d16_len);
    const boundary_t *wire_group = reject ? &boundaries[0] : group;
    const boundary_t *wire_subgroup = reject ? &boundaries[0] : subgroup;
    NEED(start(&f, version, publication));
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = wire_group->value;
    cfg.subgroup_id = wire_subgroup->value;
    cfg.publisher_priority = PRIORITY;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    NEED(open_subgroup(&f, &cfg, false, now(&f), &sg) == MOQ_OK);
    NEED(moq_subgroup_is_valid(sg));
    NEED(take_data(&f, &a));
    NEED(header_matches(&f, &a, wire_group, wire_subgroup));
    moq_stream_ref_t stream = a.u.send_data.stream_ref;
    uint64_t epoch = a.borrow_epoch;
    NEED(deliver(&f, &a, stream));
    moq_action_cleanup(&a);
    NEED(no_actions(f.sender));
    if (reject) {
        cfg.group_id = group->value;
        cfg.subgroup_id = subgroup->value;
        NEED(refusal(&f, &cfg, epoch));
    }
    NEED(moq_rcbuf_create(&f.alloc, payload_bytes, sizeof(payload_bytes), &sent) == MOQ_OK);
    NEED(moq_session_write_object(f.sender, sg, OBJECT_ID, sent, now(&f)) == MOQ_OK);
    moq_rcbuf_decref(sent);
    sent = NULL;
    NEED(take_data(&f, &a));
    NEED(a.u.send_data.payload != NULL && !a.u.send_data.fin);
    NEED(deliver(&f, &a, stream));
    moq_action_cleanup(&a);
    NEED(moq_session_close_subgroup(f.sender, sg, now(&f)) == MOQ_OK);
    NEED(take_data(&f, &a));
    NEED(a.u.send_data.fin && a.u.send_data.payload == NULL);
    NEED(deliver(&f, &a, stream));
    moq_action_cleanup(&a);
    NEED(no_actions(f.sender) && no_actions(f.receiver));
    unsigned objects = 0, finished = 0;
    bool drained = false;
    for (unsigned i = 0; i < 64; i++) {
        moq_event_t ev;
        size_t n = 0;
        NEED(moq_session_poll_events_ex(f.receiver, &ev, 1, sizeof(ev), &n) == MOQ_OK);
        if (!n) { drained = true; break; }
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *o = &ev.u.object_received;
            CHECK(objects == 0 && finished == 0);
            CHECK(o->group_id == wire_group->value && o->subgroup_id == wire_subgroup->value);
            CHECK(o->object_id == OBJECT_ID && o->publisher_priority == PRIORITY);
            CHECK(o->status == MOQ_OBJECT_NORMAL && !o->datagram && !o->end_of_group);
            CHECK(o->properties == NULL);
            CHECK(moq_subscription_eq(o->sub, f.recv_sub));
            CHECK(moq_publication_eq(o->pub, f.recv_pub));
            if (CHECK(o->payload != NULL) && !received) {
                received = o->payload;
                ev.u.object_received.payload = NULL;
            }
            objects++;
        } else if (ev.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
            const moq_subgroup_finished_event_t *end = &ev.u.subgroup_finished;
            CHECK(objects == 1 && finished == 0);
            CHECK(end->group_id == wire_group->value && end->subgroup_id == wire_subgroup->value);
            CHECK(moq_subscription_eq(end->sub, f.recv_sub));
            CHECK(moq_publication_eq(end->pub, f.recv_pub));
            CHECK(!end->end_of_group);
            finished++;
        } else {
            CHECK(false);
        }
        moq_event_cleanup(&ev);
    }
    NEED(drained && objects == 1 && finished == 1 && received != NULL);
    NEED(moq_session_state(f.sender) == MOQ_SESS_ESTABLISHED);
    NEED(moq_session_state(f.receiver) == MOQ_SESS_ESTABLISHED);
done:
    moq_action_cleanup(&a);
    moq_rcbuf_decref(sent);
    if (f.pair) moq_simpair_destroy(f.pair);
    if (received) {
        CHECK(f.memory.live > 0);
        if (CHECK(moq_rcbuf_len(received) == sizeof(payload_bytes)))
            CHECK(memcmp(moq_rcbuf_data(received), payload_bytes, sizeof(payload_bytes)) == 0);
        moq_rcbuf_decref(received);
    }
    CHECK(f.memory.live == 0);
}

int main(void)
{
    const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22
    };
    unsigned cases = 0, passed = 0;
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        for (unsigned origin = 0; origin < 2; origin++) {
            for (size_t g = 0; g < sizeof(boundaries) / sizeof(boundaries[0]); g++) {
                for (size_t sg = 0; sg < sizeof(boundaries) / sizeof(boundaries[0]); sg++) {
                    snprintf(case_name, sizeof(case_name), "D%u/%s/group=%s/subgroup=%s",
                        (unsigned)versions[v], origin ? "PUBLISH" : "SUBSCRIBE",
                        boundaries[g].name, boundaries[sg].name);
                    unsigned before = failures;
                    run_case(versions[v], origin != 0, &boundaries[g], &boundaries[sg]);
                    cases++;
                    if (failures == before) passed++;
                    printf("%s %s\n", failures == before ? "PASS" : "FAIL", case_name);
                }
            }
        }
    }
    printf("%u/%u cases, %u checks, %u failures\n", passed, cases, checks, failures);
    return failures ? 1 : 0;
}
