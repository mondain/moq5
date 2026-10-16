/* Public-only lawful shared-alias histories, with matched NOTIFY controls. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct owner {
    bool publish;
    moq_subscription_t rx_sub, tx_sub;
    moq_publication_t rx_pub, tx_pub;
} owner_t;

typedef struct rig {
    moq_simpair_t *pair;
    moq_session_t *rx, *tx;
    test_alloc_state_t memory;
    moq_alloc_t alloc;
    owner_t target, sibling;
    bool watch_notify;
    unsigned replies, failures, setup_failures;
} rig_t;

static const moq_bytes_t ns_part = {(const uint8_t *)"st-window", 9};
static const moq_namespace_t ns = {&ns_part, 1};
static const moq_bytes_t name = {(const uint8_t *)"v", 1};
static const uint64_t alias = 37;

static bool setup_fail(rig_t *r, const char *expr, int line)
{
    printf("SETUP_FAIL line=%d expression=%s\n", line, expr);
    r->setup_failures++;
    return false;
}
#define NEED(r, expr) do { if (!(expr)) return setup_fail(r, #expr, __LINE__); } while (0)

static uint64_t now(const rig_t *r) { return moq_simpair_now_us(r->pair); }
static bool pump(rig_t *r)
{
    NEED(r, moq_simpair_run_until_quiescent(r->pair, 32, NULL) == MOQ_OK);
    NEED(r, moq_session_state(r->rx) == MOQ_SESS_ESTABLISHED);
    NEED(r, moq_session_state(r->tx) == MOQ_SESS_ESTABLISHED);
    return true;
}

static void trace(void *ctx, const moq_sim_trace_record_t *record)
{
    rig_t *r = ctx;
    if (r->watch_notify && record->kind == MOQ_SIM_TRACE_ACTION &&
        record->from == MOQ_PERSPECTIVE_CLIENT)
        r->replies++;
}

static bool empty(rig_t *r, moq_session_t *s)
{
    moq_event_t event;
    if (!moq_session_poll_events(s, &event, 1)) return true;
    printf("UNEXPECTED_EVENT kind=%u\n", (unsigned)event.kind);
    moq_event_cleanup(&event);
    return setup_fail(r, "event queue empty", __LINE__);
}

static bool take(rig_t *r, moq_session_t *s, moq_event_kind_t kind, moq_event_t *event)
{
    NEED(r, moq_session_poll_events(s, event, 1) == 1);
    if (event->kind != kind) {
        printf("UNEXPECTED_EVENT got=%u wanted=%u\n", (unsigned)event->kind, (unsigned)kind);
        moq_event_cleanup(event);
        return setup_fail(r, "exact event kind", __LINE__);
    }
    return true;
}

static bool start(rig_t *r)
{
    r->alloc = test_allocator(&r->memory);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &r->alloc;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.trace_fn = trace;
    cfg.trace_ctx = r;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 16;
    NEED(r, moq_simpair_create(&cfg, &r->pair) == MOQ_OK);
    r->rx = moq_simpair_client(r->pair);
    r->tx = moq_simpair_server(r->pair);
    NEED(r, moq_simpair_start(r->pair) == MOQ_OK);
    NEED(r, pump(r));
    moq_event_t event;
    while (moq_session_poll_events(r->rx, &event, 1)) moq_event_cleanup(&event);
    while (moq_session_poll_events(r->tx, &event, 1)) moq_event_cleanup(&event);
    NEED(r, moq_session_note_object_published(r->tx, &ns, name, 5, 2) == MOQ_OK);
    return true;
}

static bool establish(rig_t *r, owner_t *o, const moq_location_filter_cfg_t *location)
{
    moq_event_t event;
    if (o->publish) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = name;
        cfg.has_track_alias = true;
        cfg.track_alias = alias;
        cfg.location = location;
        NEED(r, moq_session_publish(r->tx, &cfg, now(r), &o->tx_pub) == MOQ_OK);
        NEED(r, pump(r));
        NEED(r, take(r, r->rx, MOQ_EVENT_PUBLISH_REQUEST, &event));
        o->rx_pub = event.u.publish_request.pub;
        bool valid = event.u.publish_request.track_alias == alias &&
            event.u.publish_request.has_largest &&
            event.u.publish_request.largest_group == 5 &&
            event.u.publish_request.largest_object == 2;
        moq_event_cleanup(&event);
        NEED(r, valid);
        moq_accept_publish_cfg_t accept;
        moq_accept_publish_cfg_init(&accept);
        NEED(r, moq_session_accept_publish(r->rx, o->rx_pub, &accept, now(r)) == MOQ_OK);
        NEED(r, pump(r));
        NEED(r, take(r, r->tx, MOQ_EVENT_PUBLISH_OK, &event));
        valid = moq_publication_eq(event.u.publish_ok.pub, o->tx_pub);
        moq_event_cleanup(&event);
        NEED(r, valid);
    } else {
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.track_namespace = ns;
        cfg.track_name = name;
        cfg.location = location;
        NEED(r, moq_session_subscribe(r->rx, &cfg, now(r), &o->rx_sub) == MOQ_OK);
        NEED(r, pump(r));
        NEED(r, take(r, r->tx, MOQ_EVENT_SUBSCRIBE_REQUEST, &event));
        o->tx_sub = event.u.subscribe_request.sub;
        moq_event_cleanup(&event);
        moq_accept_subscribe_cfg_t accept;
        moq_accept_subscribe_cfg_init(&accept);
        accept.has_track_alias = true;
        accept.track_alias = alias;
        accept.has_largest = true;
        accept.largest_group = 5;
        accept.largest_object = 2;
        NEED(r, moq_session_accept_subscribe(r->tx, o->tx_sub, &accept, now(r)) == MOQ_OK);
        NEED(r, pump(r));
        NEED(r, take(r, r->rx, MOQ_EVENT_SUBSCRIBE_OK, &event));
        bool valid = moq_subscription_eq(event.u.subscribe_ok.sub, o->rx_sub) &&
            event.u.subscribe_ok.track_alias == alias && event.u.subscribe_ok.has_largest &&
            event.u.subscribe_ok.largest_group == 5 && event.u.subscribe_ok.largest_object == 2;
        moq_event_cleanup(&event);
        NEED(r, valid);
    }
    printf("ESTABLISHED family=%s rx=%" PRIu64 " tx=%" PRIu64
           " alias=37 largest={5,2} filter=%u\n", o->publish ? "PUBLISH" : "SUBSCRIBE",
           o->publish ? o->rx_pub._opaque : o->rx_sub._opaque,
           o->publish ? o->tx_pub._opaque : o->tx_sub._opaque, location->kind);
    return empty(r, r->rx) && empty(r, r->tx);
}

static bool query(rig_t *r, const moq_location_filter_cfg_t *expected)
{
    moq_location_filter_cfg_t current;
    moq_result_t rc = r->target.publish ? moq_session_publication_filter_sized(
        r->rx, r->target.rx_pub, &current, sizeof(current)) :
        moq_session_subscription_filter_sized(r->rx, r->target.rx_sub, &current, sizeof(current));
    NEED(r, rc == MOQ_OK && memcmp(&current, expected, sizeof(current)) == 0);
    return true;
}

static bool update(rig_t *r, const moq_location_filter_cfg_t *location)
{
    if (r->target.publish) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init(&cfg);
        cfg.location = location;
        NEED(r, moq_session_update_publication(r->rx, r->target.rx_pub, &cfg, now(r)) == MOQ_OK);
    } else {
        moq_subscription_update_cfg_t cfg;
        moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.location = location;
        NEED(r, moq_session_update_subscription(r->rx, r->target.rx_sub, &cfg, now(r)) == MOQ_OK);
    }
    NEED(r, pump(r));
    moq_event_t event;
    NEED(r, take(r, r->tx, r->target.publish ? MOQ_EVENT_PUBLISH_UPDATED :
                 MOQ_EVENT_SUBSCRIBE_UPDATED, &event));
    const moq_subscription_update_options_t *options = r->target.publish ?
        event.u.publish_updated.options : event.u.subscribe_updated.options;
    bool valid = options && options->struct_size >= sizeof(*options) &&
        options->resolution.largest_group == 9 && options->resolution.largest_object == 1 &&
        (options->resolution.flags & MOQ_UPDATE_RESOLUTION_HAS_LARGEST);
    if (valid && location->kind == MOQ_LOCATION_FILTER_NEXT_OBJECT)
        valid = options->resolution.location.kind == MOQ_LOCATION_FILTER_ABSOLUTE_START &&
            options->resolution.location.start_group == 9 && options->resolution.location.start_object == 2;
    if (valid)
        printf("UPDATE id=%" PRIu64 " largest={9,1} resolved={%" PRIu64 ",%" PRIu64 "}\n",
            options->resolution.update_request_id, options->resolution.location.start_group,
            options->resolution.location.start_object);
    moq_event_cleanup(&event);
    NEED(r, valid);
    NEED(r, take(r, r->rx, r->target.publish ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                 MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &event));
    valid = r->target.publish ?
        moq_publication_eq(event.u.publication_update_ok.pub, r->target.rx_pub) &&
        event.u.publication_update_ok.has_largest && event.u.publication_update_ok.largest_group == 9 &&
        event.u.publication_update_ok.largest_object == 1 :
        moq_subscription_eq(event.u.subscription_update_ok.sub, r->target.rx_sub) &&
        event.u.subscription_update_ok.has_largest && event.u.subscription_update_ok.largest_group == 9 &&
        event.u.subscription_update_ok.largest_object == 1;
    moq_event_cleanup(&event);
    NEED(r, valid);
    return query(r, location) && empty(r, r->rx) && empty(r, r->tx);
}

static bool notify(rig_t *r, const moq_location_filter_cfg_t *location)
{
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    cfg.filter = location;
    r->watch_notify = true;
    moq_result_t rc = r->target.publish ? moq_session_notify_publication_state(
        r->tx, r->target.tx_pub, &cfg, now(r)) : moq_session_notify_subscription_state(
        r->tx, r->target.tx_sub, &cfg, now(r));
    NEED(r, rc == MOQ_OK && pump(r));
    r->watch_notify = false;
    moq_event_t event;
    NEED(r, take(r, r->rx, MOQ_EVENT_PUBLISH_STATE, &event));
    const moq_publish_state_event_t *state = &event.u.publish_state;
    bool valid = state->family == (r->target.publish ? MOQ_REQUEST_FAMILY_PUBLISH :
        MOQ_REQUEST_FAMILY_SUBSCRIBE) && state->handle.raw == (r->target.publish ?
        r->target.rx_pub._opaque : r->target.rx_sub._opaque) && state->has_filter &&
        state->location && memcmp(state->location, location, sizeof(*location)) == 0 &&
        !state->has_forward && state->has_largest && state->largest_group == 9 &&
        state->largest_object == 1;
    moq_event_cleanup(&event);
    NEED(r, valid && r->replies == 0);
    printf("NOTIFY same_filter=%u largest={9,1} receiver_actions=0\n", location->kind);
    return query(r, location) && empty(r, r->rx) && empty(r, r->tx);
}

static bool matches(const owner_t *o, moq_subscription_t sub, moq_publication_t pub)
{
    return o->publish ? !moq_subscription_is_valid(sub) && moq_publication_eq(pub, o->rx_pub) :
        !moq_publication_is_valid(pub) && moq_subscription_eq(sub, o->rx_sub);
}

static bool deliver(rig_t *r, const owner_t *sender, uint64_t group, uint64_t object_id,
                    unsigned want_target, unsigned want_sibling)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.subgroup_id = object_id;
    moq_subgroup_handle_t subgroup;
    moq_result_t rc = sender->publish ? moq_session_open_pub_subgroup(
        r->tx, sender->tx_pub, &cfg, now(r), &subgroup) : moq_session_open_subgroup(
        r->tx, sender->tx_sub, &cfg, now(r), &subgroup);
    NEED(r, rc == MOQ_OK);
    const uint8_t bytes[] = {0x39, 0x88, (uint8_t)group, (uint8_t)object_id};
    moq_rcbuf_t *payload = NULL;
    NEED(r, moq_rcbuf_create(&r->alloc, bytes, sizeof(bytes), &payload) == MOQ_OK);
    rc = moq_session_write_object(r->tx, subgroup, object_id, payload, now(r));
    moq_rcbuf_decref(payload);
    NEED(r, rc == MOQ_OK);
    NEED(r, moq_session_close_subgroup(r->tx, subgroup, now(r)) == MOQ_OK && pump(r));
    unsigned target = 0, sibling = 0, stray = 0, seen = 0;
    bool exact = true;
    moq_event_t event;
    while (seen < 16 && moq_session_poll_events(r->rx, &event, 1)) {
        seen++;
        if (event.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *object = &event.u.object_received;
            if (matches(&r->target, object->sub, object->pub)) target++;
            else if (matches(&r->sibling, object->sub, object->pub)) sibling++;
            else stray++;
            exact = exact && object->group_id == group && object->object_id == object_id &&
                object->subgroup_id == object_id &&
                !object->datagram && object->payload && moq_rcbuf_len(object->payload) == sizeof(bytes) &&
                memcmp(moq_rcbuf_data(object->payload), bytes, sizeof(bytes)) == 0;
        } else if (event.kind == MOQ_EVENT_SUBGROUP_FINISHED) {
            exact = exact && event.u.subgroup_finished.group_id == group &&
                event.u.subgroup_finished.subgroup_id == object_id;
        } else {
            stray++;
        }
        moq_event_cleanup(&event);
    }
    NEED(r, seen < 16 && empty(r, r->rx) && empty(r, r->tx));
    bool passed = target == want_target && sibling == want_sibling && stray == 0 && exact;
    printf("%s delivery alias=37 location={%" PRIu64 ",%" PRIu64 "} expected=%u/%u got=%u/%u stray=%u exact_payload=%u\n",
        passed ? "PASS" : "ASSERT", group, object_id, want_target, want_sibling,
        target, sibling, stray, exact ? 1u : 0u);
    if (!passed) r->failures++;
    return true;
}

static void range(moq_location_filter_cfg_t *location, uint64_t first, uint64_t last)
{
    moq_location_filter_cfg_init(location);
    location->kind = MOQ_LOCATION_FILTER_GROUP_END;
    location->start_group = first;
    location->end_group = last;
}

static unsigned row(bool publish, unsigned history, bool with_notify, unsigned *setup)
{
    rig_t r = {0};
    r.target.publish = publish;
    moq_location_filter_cfg_t initial, effective, other;
    range(&initial, 0, 5);
    if (history == 1 || history == 3) {
        moq_location_filter_cfg_init(&initial);
        initial.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    }
    effective = initial;
    if (history == 0) effective.end_group = 10;
    if (history == 2) {
        moq_location_filter_cfg_init(&effective);
        effective.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    }
    if (history == 3) moq_location_filter_cfg_init(&effective);
    range(&other, history == 0 ? 11 : history == 1 ? 0 : 6,
          history == 0 ? 20 : history == 1 ? 4 : 8);
    if (history == 2) {
        other.kind = MOQ_LOCATION_FILTER_OBJECT_END;
        other.end_group = 9;
        other.end_object = 1;
    }
    if (history == 3) range(&other, 12, 14);
    printf("BEGIN target=%s history=%u notify=%u\n", publish ? "PUBLISH" : "SUBSCRIBE", history, with_notify);
    if (start(&r) && establish(&r, &r.target, &initial) && establish(&r, &r.sibling, &other) &&
        moq_session_note_object_published(r.tx, &ns, name, 9, 1) == MOQ_OK &&
        (history == 1 || update(&r, &effective)) &&
        (!with_notify || notify(&r, &effective)) && query(&r, &effective)) {
        if (history == 0) {
            if (deliver(&r, &r.target, 7, 0, 1, 0)) (void)deliver(&r, &r.sibling, 15, 0, 0, 1);
        } else if (history == 1) {
            if (deliver(&r, &r.target, 6, 0, 1, 0)) (void)deliver(&r, &r.sibling, 4, 0, 0, 1);
        } else if (history == 2) {
            if (deliver(&r, &r.sibling, 7, 0, 0, 1) &&
                deliver(&r, &r.sibling, 9, 1, 0, 1) &&
                deliver(&r, &r.target, 9, 2, 1, 0))
                (void)deliver(&r, &r.target, 10, 0, 1, 0);
        } else {
            if (deliver(&r, &r.target, 4, 0, 1, 0)) (void)deliver(&r, &r.sibling, 13, 0, 1, 1);
        }
    } else if (!r.setup_failures) {
        (void)setup_fail(&r, "restored track history", __LINE__);
    }
    if (r.pair) moq_simpair_destroy(r.pair);
    if (r.memory.balance != 0) {
        printf("ASSERT allocation_balance=%" PRId64 "\n", r.memory.balance);
        r.failures++;
    }
    printf("ROW target=%s history=%u notify=%u assertions=%u setup=%u balance=%" PRId64 "\n",
        publish ? "PUBLISH" : "SUBSCRIBE", history, with_notify, r.failures, r.setup_failures, r.memory.balance);
    *setup += r.setup_failures;
    return r.failures;
}

int main(void)
{
    unsigned cases = 0, failures = 0, setup = 0;
    for (unsigned publish = 0; publish < 2; publish++)
        for (unsigned history = 0; history < 4; history++)
            for (unsigned notification = 0; notification < 2; notification++) {
                cases++;
                failures += row(publish != 0, history, notification != 0, &setup);
            }
    printf("SUMMARY cases=%u assertions=%u setup=%u\n", cases, failures, setup);
    return setup ? 2 : failures ? 1 : 0;
}
