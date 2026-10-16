/*
 * Scheduling of competing queued objects: the order the publisher's data
 * actions are polled follows the scheduling algorithm (subscriber priority,
 * publisher priority, the subscription's group order between groups, then
 * within a group the fill-delivered object, a datagram before a subgroup, and
 * the lowest Subgroup ID or Object ID), while actions on one stream keep their
 * order and a non-data action holds its place in the queue. A fill whose group
 * order differs from its subscription's yields to the subscription-delivered
 * object.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "test_binding_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static int failures;

typedef struct fx {
    moq_simpair_t *sp;
    moq_session_t *c;    /* subscriber */
    moq_session_t *sv;   /* publisher */
} fx_t;

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static void drain_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

static bool fx_open(fx_t *fx, moq_version_t version)
{
    memset(fx, 0, sizeof(*fx));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    scfg.client_send_request_capacity = true;
    scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = 32;
    scfg.server_initial_request_capacity = 32;
    if (moq_simpair_create(&scfg, &fx->sp) < 0) return false;
    fx->c = moq_simpair_client(fx->sp);
    fx->sv = moq_simpair_server(fx->sp);
    if (moq_simpair_start(fx->sp) < 0) return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    drain_events(fx->c); drain_events(fx->sv);
    return moq_session_state(fx->sv) == MOQ_SESS_ESTABLISHED;
}

static void fx_close(fx_t *fx) { if (fx->sp) moq_simpair_destroy(fx->sp); memset(fx, 0, sizeof(*fx)); }

/* Subscribe to track `name` with the given subscriber priority and group
 * order (DEFAULT = omitted), optionally with a fill; accept it on the
 * publisher (Largest {0,0} when a fill is requested so the fill has a range).
 * Returns the publisher's subscription handle; *out_fill receives the fill
 * child surfaced to the publisher when one was requested. */
static moq_subscription_t subscribe(fx_t *fx, const char *name, uint8_t priority,
                                    uint8_t group_order, const moq_fill_cfg_t *fill,
                                    moq_fill_t *out_fill)
{
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)name, strlen(name) };
    cfg.has_subscriber_priority = true;
    cfg.subscriber_priority = priority;
    cfg.group_order = group_order;
    if (fill) { cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT; cfg.fill = fill; }
    moq_subscription_t csub, ssub = MOQ_SUBSCRIPTION_INVALID;
    if (moq_session_subscribe(fx->c, &cfg, moq_simpair_now_us(fx->sp), &csub) < 0) return ssub;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    moq_event_t ev;
    while (moq_session_poll_events(fx->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST && !moq_subscription_is_valid(ssub))
            ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    if (!moq_subscription_is_valid(ssub)) return ssub;
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    if (fill) { acc.has_largest = true; acc.largest_group = 0; acc.largest_object = 0; }
    if (moq_session_accept_subscribe(fx->sv, ssub, &acc, moq_simpair_now_us(fx->sp)) < 0)
        return MOQ_SUBSCRIPTION_INVALID;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    if (out_fill) {
        *out_fill = (moq_fill_t){0};
        while (moq_session_poll_events(fx->sv, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_FILL_REQUEST) *out_fill = ev.u.fill_request.fill;
            moq_event_cleanup(&ev);
        }
    }
    drain_events(fx->c); drain_events(fx->sv);
    drain_actions(fx->sv);
    return ssub;
}

static moq_rcbuf_t *payload(const char *text)
{
    moq_rcbuf_t *b = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)text, strlen(text), &b);
    return b;
}

/* Open a subgroup and write one object on it; returns the stream ref. */
static moq_stream_ref_t sg_object(fx_t *fx, moq_subscription_t ssub, uint64_t group,
                                  uint64_t subgroup, uint8_t publisher_priority,
                                  moq_subgroup_handle_t *out_sg)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.subgroup_id = subgroup;
    cfg.publisher_priority = publisher_priority;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0);
    if (moq_session_open_subgroup(fx->sv, ssub, &cfg, moq_simpair_now_us(fx->sp), &sg) != MOQ_OK) {
        failures++; return ref;
    }
    uint32_t slot = moq_handle_slot(sg._opaque);
    ref = fx->sv->subgroups[slot].stream_ref;
    moq_rcbuf_t *p = payload("o");
    if (moq_session_write_object(fx->sv, sg, 0, p, moq_simpair_now_us(fx->sp)) != MOQ_OK) failures++;
    moq_rcbuf_decref(p);
    if (out_sg) *out_sg = sg;
    return ref;
}

typedef struct polled { moq_action_t acts[32]; size_t n; } polled_t;

static void poll_all(moq_session_t *s, polled_t *out)
{
    out->n = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (out->n < 32) out->acts[out->n++] = a; else moq_action_cleanup(&a);
    }
}

static void polled_free(polled_t *p)
{
    for (size_t i = 0; i < p->n; i++) moq_action_cleanup(&p->acts[i]);
    p->n = 0;
}

/* Index of the first SEND_DATA on `ref`, or SIZE_MAX. */
static size_t first_on(const polled_t *p, moq_stream_ref_t ref)
{
    for (size_t i = 0; i < p->n; i++)
        if (p->acts[i].kind == MOQ_ACTION_SEND_DATA && p->acts[i].u.send_data.stream_ref._v == ref._v)
            return i;
    return SIZE_MAX;
}

static size_t first_kind(const polled_t *p, moq_action_kind_t kind)
{
    for (size_t i = 0; i < p->n; i++) if (p->acts[i].kind == kind) return i;
    return SIZE_MAX;
}

/* Every SEND_DATA on `ref` appears in queue order: header first, then the
 * payload-bearing object. */
static bool stream_in_order(const polled_t *p, moq_stream_ref_t ref)
{
    bool seen_header = false;
    for (size_t i = 0; i < p->n; i++) {
        if (p->acts[i].kind != MOQ_ACTION_SEND_DATA || p->acts[i].u.send_data.stream_ref._v != ref._v) continue;
        if (!seen_header) { if (!p->acts[i].u.send_data.payload) seen_header = true; else return false; }
    }
    return seen_header;
}

static void dump(const polled_t *p)
{
    for (size_t i = 0; i < p->n; i++) {
        const moq_action_t *a = &p->acts[i];
        if (a->kind == MOQ_ACTION_SEND_DATA)
            fprintf(stderr, "  [%zu] SEND_DATA ref=%llu hdr=%u payload=%s fin=%d prio=0x%llx\n", i,
                    (unsigned long long)a->u.send_data.stream_ref._v, a->u.send_data.header_len,
                    a->u.send_data.payload ? "yes" : "no", (int)a->u.send_data.fin,
                    (unsigned long long)a->u.send_data.scheduling_priority);
        else
            fprintf(stderr, "  [%zu] kind=%u\n", i, (unsigned)a->kind);
    }
}

#define EXPECT_BEFORE(p, a, b, what) do { \
    size_t ia_ = (a), ib_ = (b); \
    if (ia_ == SIZE_MAX || ib_ == SIZE_MAX || ia_ >= ib_) { \
        fprintf(stderr, "FAIL %s:%d %s: indexes %zu vs %zu\n", __FILE__, __LINE__, what, ia_, ib_); \
        dump(p); \
        failures++; } } while (0)

/* -- Rules 1-4 on subgroups and datagrams ----------------------------------- */

static void test_rules(moq_version_t v)
{
    fx_t fx;
    if (!fx_open(&fx, v)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    polled_t p;

    /* Rule 1: the higher subscriber priority (lower number) goes first even
     * though it was queued second. */
    moq_subscription_t low = subscribe(&fx, "low", 20, MOQ_GROUP_ORDER_ASCENDING, NULL, NULL);
    moq_subscription_t high = subscribe(&fx, "high", 10, MOQ_GROUP_ORDER_ASCENDING, NULL, NULL);
    MOQ_TEST_CHECK(moq_subscription_is_valid(low) && moq_subscription_is_valid(high));
    moq_stream_ref_t r_low = sg_object(&fx, low, 0, 0, 128, NULL);
    moq_stream_ref_t r_high = sg_object(&fx, high, 0, 0, 128, NULL);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_high), first_on(&p, r_low), "subscriber priority");
    MOQ_TEST_CHECK(stream_in_order(&p, r_low) && stream_in_order(&p, r_high));
    polled_free(&p);

    /* Rule 2: same subscription, the higher publisher priority first. */
    moq_stream_ref_t r_p5 = sg_object(&fx, low, 1, 0, 5, NULL);
    moq_stream_ref_t r_p3 = sg_object(&fx, low, 2, 0, 3, NULL);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_p3), first_on(&p, r_p5), "publisher priority");
    polled_free(&p);

    /* Rule 3: ascending group order sends the lower group first. */
    moq_stream_ref_t r_g11 = sg_object(&fx, low, 11, 0, 128, NULL);
    moq_stream_ref_t r_g10 = sg_object(&fx, low, 10, 0, 128, NULL);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_g10), first_on(&p, r_g11), "ascending group order");
    polled_free(&p);

    /* Rule 3, descending subscription: the higher group first. */
    moq_subscription_t desc = subscribe(&fx, "desc", 10, MOQ_GROUP_ORDER_DESCENDING, NULL, NULL);
    MOQ_TEST_CHECK(moq_subscription_is_valid(desc));
    moq_stream_ref_t r_d0 = sg_object(&fx, desc, 0, 0, 128, NULL);
    moq_stream_ref_t r_d1 = sg_object(&fx, desc, 1, 0, 128, NULL);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_d1), first_on(&p, r_d0), "descending group order");
    polled_free(&p);

    /* Rule 4: same group, the lower Subgroup ID first. */
    moq_stream_ref_t r_sg2 = sg_object(&fx, low, 20, 2, 128, NULL);
    moq_stream_ref_t r_sg1 = sg_object(&fx, low, 20, 1, 128, NULL);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_sg1), first_on(&p, r_sg2), "lowest subgroup id");
    polled_free(&p);

    /* Rule 4: different delivery modes in one group, the datagram first. */
    moq_stream_ref_t r_sg = sg_object(&fx, low, 30, 0, 128, NULL);
    moq_rcbuf_t *dg = payload("d");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_send_object_datagram(fx.sv, low, 30, 7, 128, false, dg, NULL, 0,
                                                                 moq_simpair_now_us(fx.sp)), (int)MOQ_OK);
    moq_rcbuf_decref(dg);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_kind(&p, MOQ_ACTION_SEND_DATAGRAM), first_on(&p, r_sg), "datagram before subgroup");
    polled_free(&p);

    /* Priorities outrank the group tie-breaks: a lower-priority datagram does
     * not jump a higher-priority subgroup of the same subscription. */
    moq_stream_ref_t r_hp = sg_object(&fx, low, 40, 0, 1, NULL);
    dg = payload("d");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_send_object_datagram(fx.sv, low, 40, 7, 200, false, dg, NULL, 0,
                                                                 moq_simpair_now_us(fx.sp)), (int)MOQ_OK);
    moq_rcbuf_decref(dg);
    poll_all(fx.sv, &p);
    EXPECT_BEFORE(&p, first_on(&p, r_hp), first_kind(&p, MOQ_ACTION_SEND_DATAGRAM), "priority over mode");
    polled_free(&p);

    /* A non-data action holds its place: group 50's data queued after a RESET
     * of another stream does not overtake the reset, although group order
     * would put it ahead of group 60 queued before the reset. */
    moq_subgroup_handle_t victim;
    moq_stream_ref_t r_victim = sg_object(&fx, low, 70, 0, 128, &victim);
    moq_stream_ref_t r_60 = sg_object(&fx, low, 60, 0, 128, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reset_subgroup(fx.sv, victim, 0x5, moq_simpair_now_us(fx.sp)), (int)MOQ_OK);
    moq_stream_ref_t r_50 = sg_object(&fx, low, 50, 0, 128, NULL);
    poll_all(fx.sv, &p);
    size_t i_reset = first_kind(&p, MOQ_ACTION_RESET_DATA);
    EXPECT_BEFORE(&p, first_on(&p, r_60), i_reset, "data before the reset barrier");
    EXPECT_BEFORE(&p, first_on(&p, r_victim), i_reset, "victim stream data before its reset");
    EXPECT_BEFORE(&p, i_reset, first_on(&p, r_50), "later data stays behind the barrier");
    polled_free(&p);

    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.sv), (int)MOQ_SESS_ESTABLISHED);
    fx_close(&fx);
}

/* -- Fill-delivered objects (draft-22) --------------------------------------- */

/* Fills need a fill pool on each session, which the simulator does not
 * configure, so these run over a hand-pumped raw pair. */
#define RAW_DATA_REF_OFFSET UINT64_C(0x100000)

static moq_session_t *raw_session(moq_perspective_t persp)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), persp);
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 64;
    cfg.max_fills = 2;
    moq_session_t *s = NULL;
    if (moq_session_create(&cfg, 0, &s) < 0) return NULL;
    if (test_bind_if_required(s) < 0) { moq_session_destroy(s); return NULL; }
    if (moq_session_start(s, 0) < 0) { moq_session_destroy(s); return NULL; }
    return s;
}

static void deliver(moq_session_t *to, const moq_action_t *a, bool from_server, uint64_t now)
{
    switch (a->kind) {
    case MOQ_ACTION_OPEN_UNI_CONTROL:
        moq_session_on_control_bytes(to, a->u.open_uni_control.data, a->u.open_uni_control.len, now);
        break;
    case MOQ_ACTION_SEND_UNI_CONTROL:
        moq_session_on_control_bytes(to, a->u.send_uni_control.data, a->u.send_uni_control.len, now);
        break;
    case MOQ_ACTION_OPEN_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.open_bidi_stream.stream_ref,
            a->u.open_bidi_stream.data, a->u.open_bidi_stream.len, a->u.open_bidi_stream.fin, now);
        break;
    case MOQ_ACTION_SEND_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.send_bidi_stream.stream_ref,
            a->u.send_bidi_stream.data, a->u.send_bidi_stream.len, a->u.send_bidi_stream.fin, now);
        break;
    case MOQ_ACTION_CLOSE_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.close_bidi_stream.stream_ref, NULL, 0, true, now);
        break;
    case MOQ_ACTION_SEND_DATA: {
        moq_stream_ref_t r = moq_stream_ref_from_u64(a->u.send_data.stream_ref._v +
            (from_server ? RAW_DATA_REF_OFFSET : 0));
        bool hp = a->u.send_data.payload != NULL;
        if (a->u.send_data.header_len > 0)
            moq_session_on_data_bytes(to, r, a->u.send_data.header, a->u.send_data.header_len,
                                      a->u.send_data.fin && !hp, now);
        if (hp)
            moq_session_on_data_bytes(to, r, moq_rcbuf_data(a->u.send_data.payload),
                                      moq_rcbuf_len(a->u.send_data.payload), a->u.send_data.fin, now);
        if (!hp && a->u.send_data.header_len == 0 && a->u.send_data.fin)
            moq_session_on_data_bytes(to, r, NULL, 0, true, now);
        break;
    }
    default: break;
    }
}

static void raw_run(moq_session_t *c, moq_session_t *sv, uint64_t now)
{
    moq_action_t a;
    for (int i = 0; i < 8; i++) {
        while (moq_session_poll_actions(c, &a, 1) > 0) { deliver(sv, &a, false, now); moq_action_cleanup(&a); }
        while (moq_session_poll_actions(sv, &a, 1) > 0) { deliver(c, &a, true, now); moq_action_cleanup(&a); }
    }
}

typedef struct raw { moq_session_t *c, *sv; } raw_t;

static bool raw_open(raw_t *r)
{
    r->c = raw_session(MOQ_PERSPECTIVE_CLIENT);
    r->sv = raw_session(MOQ_PERSPECTIVE_SERVER);
    if (!r->c || !r->sv) return false;
    raw_run(r->c, r->sv, 1);
    drain_events(r->c); drain_events(r->sv);
    return moq_session_state(r->sv) == MOQ_SESS_ESTABLISHED;
}

static void raw_close(raw_t *r)
{
    if (r->c) moq_session_destroy(r->c);
    if (r->sv) moq_session_destroy(r->sv);
    memset(r, 0, sizeof(*r));
}

/* SUBSCRIBE from {0,0} with a fill (group order `fill_order`) on a
 * subscription with `sub_order`, accepted with Largest {2,0} so the fill has
 * a range; returns the publisher's handle and the fill child the publisher was
 * asked to serve. */
static moq_subscription_t raw_subscribe_fill(raw_t *r, uint8_t sub_order, uint32_t fill_order,
                                             moq_fill_t *out_fill)
{
    moq_fill_cfg_t fc; moq_fill_cfg_init(&fc);
    fc.has_group_order = true; fc.group_order = fill_order;
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.has_subscriber_priority = true; cfg.subscriber_priority = 10;
    cfg.group_order = sub_order;
    cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    cfg.start_group = 0; cfg.start_object = 0;
    cfg.fill = &fc;
    moq_subscription_t csub, ssub = MOQ_SUBSCRIPTION_INVALID;
    *out_fill = (moq_fill_t){0};
    if (moq_session_subscribe(r->c, &cfg, 2, &csub) != MOQ_OK) return ssub;
    raw_run(r->c, r->sv, 3);
    moq_event_t ev;
    while (moq_session_poll_events(r->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    if (!moq_subscription_is_valid(ssub)) return ssub;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 2; ac.largest_object = 0;
    if (moq_session_accept_subscribe(r->sv, ssub, &ac, 4) != MOQ_OK) return MOQ_SUBSCRIPTION_INVALID;
    raw_run(r->c, r->sv, 5);
    while (moq_session_poll_events(r->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_REQUEST) *out_fill = ev.u.fill_request.fill;
        moq_event_cleanup(&ev);
    }
    drain_events(r->c);
    drain_actions(r->sv);
    return ssub;
}

/* The fill stream's ref: the one live fill child bound to a stream. */
static moq_stream_ref_t fill_ref(moq_session_t *s)
{
    for (size_t i = 0; s->fill_storage && i < s->fill_cap; i++)
        if (s->fill_storage[i].active && s->fill_storage[i].stream_bound)
            return s->fill_storage[i].stream;
    return moq_stream_ref_from_u64(0);
}

static void write_fill(moq_session_t *sv, moq_fill_t fill, uint64_t g, uint64_t o, uint64_t now)
{
    moq_fetch_object_cfg_t oc;
    moq_fetch_object_cfg_init(&oc);
    oc.group_id = g; oc.subgroup_id = 0; oc.object_id = o; oc.publisher_priority = 128;
    oc.payload = payload("f");
    if (moq_session_write_fill_object(sv, fill, &oc, now) != MOQ_OK) failures++;
    moq_rcbuf_decref(oc.payload);
}

static moq_stream_ref_t raw_sg_object(moq_session_t *sv, moq_subscription_t ssub, uint64_t group,
                                      uint64_t now)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group; cfg.subgroup_id = 0; cfg.publisher_priority = 128;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0);
    if (moq_session_open_subgroup(sv, ssub, &cfg, now, &sg) != MOQ_OK) { failures++; return ref; }
    ref = sv->subgroups[moq_handle_slot(sg._opaque)].stream_ref;
    moq_rcbuf_t *p = payload("o");
    if (moq_session_write_object(sv, sg, 0, p, now) != MOQ_OK) failures++;
    moq_rcbuf_decref(p);
    return ref;
}

static void test_fill_rules(void)
{
    polled_t p;
    /* Same group: the fill-delivered object first, although queued second. */
    {
        raw_t r;
        if (!raw_open(&r)) { MOQ_TEST_CHECK(false); raw_close(&r); return; }
        moq_fill_t fill;
        moq_subscription_t ssub = raw_subscribe_fill(&r, MOQ_GROUP_ORDER_ASCENDING, MOQ_GROUP_ORDER_ASCENDING, &fill);
        MOQ_TEST_CHECK(moq_subscription_is_valid(ssub) && fill._opaque != 0);
        moq_stream_ref_t r_sg = raw_sg_object(r.sv, ssub, 1, 6);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(r.sv, fill, 6), (int)MOQ_OK);
        write_fill(r.sv, fill, 1, 0, 6);
        moq_stream_ref_t r_fill = fill_ref(r.sv);
        poll_all(r.sv, &p);
        EXPECT_BEFORE(&p, first_on(&p, r_fill), first_on(&p, r_sg), "fill first within a group");
        polled_free(&p);
        raw_close(&r);
    }
    /* Different groups, the fill's group order differs from the
     * subscription's: the subscription-delivered object first, although
     * ascending order alone would send the fill's lower group first. */
    {
        raw_t r;
        if (!raw_open(&r)) { MOQ_TEST_CHECK(false); raw_close(&r); return; }
        moq_fill_t fill;
        moq_subscription_t ssub = raw_subscribe_fill(&r, MOQ_GROUP_ORDER_ASCENDING, MOQ_GROUP_ORDER_DESCENDING, &fill);
        MOQ_TEST_CHECK(moq_subscription_is_valid(ssub) && fill._opaque != 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(r.sv, fill, 6), (int)MOQ_OK);
        write_fill(r.sv, fill, 0, 0, 6);
        moq_stream_ref_t r_fill = fill_ref(r.sv);
        moq_stream_ref_t r_sg = raw_sg_object(r.sv, ssub, 1, 6);
        poll_all(r.sv, &p);
        EXPECT_BEFORE(&p, first_on(&p, r_sg), first_on(&p, r_fill), "subscription first on order mismatch");
        polled_free(&p);
        raw_close(&r);
    }
    /* Different groups, orders agree: the group order decides (the fill's
     * lower group first under ascending). */
    {
        raw_t r;
        if (!raw_open(&r)) { MOQ_TEST_CHECK(false); raw_close(&r); return; }
        moq_fill_t fill;
        moq_subscription_t ssub = raw_subscribe_fill(&r, MOQ_GROUP_ORDER_ASCENDING, MOQ_GROUP_ORDER_ASCENDING, &fill);
        MOQ_TEST_CHECK(moq_subscription_is_valid(ssub) && fill._opaque != 0);
        moq_stream_ref_t r_sg = raw_sg_object(r.sv, ssub, 1, 6);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(r.sv, fill, 6), (int)MOQ_OK);
        write_fill(r.sv, fill, 0, 0, 6);
        moq_stream_ref_t r_fill = fill_ref(r.sv);
        poll_all(r.sv, &p);
        EXPECT_BEFORE(&p, first_on(&p, r_fill), first_on(&p, r_sg), "group order across fill and subscription");
        polled_free(&p);
        raw_close(&r);
    }
}

int main(void)
{
    test_rules(MOQ_VERSION_DRAFT_16);
    test_rules(MOQ_VERSION_DRAFT_18);
    test_rules(MOQ_VERSION_DRAFT_22);
    test_fill_rules();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    MOQ_TEST_PASS("scheduling_order");
    return 0;
}
