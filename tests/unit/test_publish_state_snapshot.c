/* D22 state observation uses literal notifications, not an encoder roundtrip. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include <string.h>

typedef struct old_state_detail {
    moq_request_family_t family;
    union {
        moq_subscription_t subscription;
        moq_publication_t publication;
        uint64_t raw;
    } handle;
    bool has_forward, forward, has_largest, has_filter;
    uint8_t padding[4];
    uint64_t largest_group, largest_object;
} old_state_detail_t;

typedef struct state_detail {
    old_state_detail_t old;
    const moq_location_filter_cfg_t *location;
} state_detail_t;

typedef struct observed_refs {
    moq_stream_ref_t requester, responder;
} observed_refs_t;

static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    observed_refs_t *refs = ctx;
    if (r->struct_size < offsetof(moq_sim_trace_record_t, stream_ref) +
                         sizeof(r->stream_ref)) return;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM && !refs->requester._v)
        refs->requester = r->stream_ref;
    if (r->kind == MOQ_SIM_TRACE_INPUT && r->to == MOQ_PERSPECTIVE_SERVER &&
        r->input_kind == MOQ_SIM_INPUT_BIDI_BYTES && !refs->responder._v)
        refs->responder = r->stream_ref;
}

static size_t drain(moq_session_t *s)
{
    moq_event_t event;
    size_t count = 0;
    while (moq_session_poll_events(s, &event, 1)) {
        ++count;
        moq_event_cleanup(&event);
    }
    return count;
}

static size_t drain_actions(moq_session_t *s)
{
    moq_action_t action;
    size_t count = 0;
    while (moq_session_poll_actions(s, &action, 1)) {
        ++count;
        moq_action_cleanup(&action);
    }
    return count;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) {
        if (event.kind == kind) { *out = event; return true; }
        moq_event_cleanup(&event);
    }
    return false;
}

static size_t notification(uint8_t wire[32], int kind,
                           moq_location_filter_cfg_t *expected)
{
    moq_location_filter_cfg_init(expected);
    wire[0] = 0x22;
    wire[1] = 0;
    wire[3] = 1;
    size_t n = 4;
    if (kind < 0) {
        wire[n++] = 0x10;
        wire[n++] = 1; /* Forward unchanged; no location parameter. */
    } else {
        wire[n++] = 0x21;
        wire[n++] = (uint8_t)kind;
        expected->kind = (uint32_t)kind;
        if (kind == 1) {
            wire[n++] = 3;
            expected->relative_group_offset = 3;
        } else if (kind >= 2 && kind <= 4) {
            wire[n++] = 7;
            wire[n++] = 9;
            expected->start_group = 7;
            expected->start_object = 9;
            if (kind >= 3) {
                wire[n++] = 3;
                expected->end_group = 10;
            }
            if (kind == 4) {
                wire[n++] = 11;
                expected->end_object = 11;
            }
        }
    }
    wire[2] = (uint8_t)(n - 3);
    return n;
}

static bool observe_location(moq_session_t *s, bool publish, uint64_t handle,
                             const moq_location_filter_cfg_t *expected)
{
    moq_event_t event;
    if (moq_session_poll_events(s, &event, 1) != 1) return false;
    const moq_publish_state_event_t *state = &event.u.publish_state;
    bool valid = event.kind == MOQ_EVENT_PUBLISH_STATE &&
        event.detail_size >= offsetof(moq_publish_state_event_t, location) +
                             sizeof(state->location) &&
        state->family == (publish ? MOQ_REQUEST_FAMILY_PUBLISH :
                                   MOQ_REQUEST_FAMILY_SUBSCRIBE) &&
        state->handle.raw == handle && state->has_filter &&
        !state->has_forward && !state->has_largest &&
        state->location != NULL &&
        (uintptr_t)state->location % _Alignof(moq_location_filter_cfg_t) == 0 &&
        memcmp(state->location, expected, sizeof(*expected)) == 0 &&
        moq_session_borrow_valid(s, event.borrow_epoch);
    moq_event_cleanup(&event);
    return valid;
}

static int row(moq_version_t version, bool publish, int kind, bool historical,
               bool churn)
{
    int failures = 0;
    test_alloc_state_t memory = {0};
    moq_alloc_t alloc = test_allocator(&memory);
    observed_refs_t refs = {0};
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &alloc;
    cfg.version = version;
    cfg.trace_fn = trace;
    cfg.trace_ctx = &refs;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 16;
    moq_simpair_t *pair = NULL;
    moq_event_t event;
    bool event_owned = false;
    moq_subscription_t sub = MOQ_SUBSCRIPTION_INVALID;
    moq_subscription_t publisher_sub = MOQ_SUBSCRIPTION_INVALID;
    moq_publication_t pub = MOQ_PUBLICATION_INVALID;
    moq_publication_t publisher_pub = MOQ_PUBLICATION_INVALID;
    moq_bytes_t part = MOQ_BYTES_LITERAL("state");
    uint8_t wire[32];
    moq_location_filter_cfg_t expected, copy, second;
    moq_rcbuf_t *payload = NULL;
    size_t n = notification(wire, kind, &expected);
    if (historical) {
        /* Independent literals for a group-bounded filter A=[0,5]. */
        wire[6] = 0; wire[7] = 0; wire[8] = 5;
        expected.start_group = expected.start_object = 0;
        expected.end_group = 5;
    }
#define NEED(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: state d%u %s kind=%d: %s\n", (unsigned)version, \
            publish ? "publish" : "subscribe", kind, #expr); \
    ++failures; goto done; \
} } while (0)
    NEED(moq_simpair_create(&cfg, &pair) == MOQ_OK);
    NEED(moq_simpair_start(pair) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED &&
         moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    drain(client); drain(server);
    memset(&refs, 0, sizeof(refs));
    uint64_t now = moq_simpair_now_us(pair);
    if (publish) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init_sized(&pc, sizeof(pc));
        pc.track_namespace = (moq_namespace_t){&part, 1};
        pc.track_name = MOQ_BYTES_LITERAL("v");
        pc.location = kind < 0 ? NULL : &expected;
        NEED(moq_session_publish(client, &pc, now, &pub) == MOQ_OK);
        publisher_pub = pub;
    } else {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init_sized(&sc, sizeof(sc));
        sc.track_namespace = (moq_namespace_t){&part, 1};
        sc.track_name = MOQ_BYTES_LITERAL("v");
        sc.location = kind < 0 ? NULL : &expected;
        NEED(moq_session_subscribe(client, &sc, now, &sub) == MOQ_OK);
    }
    NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    event_owned = take(server, publish ? MOQ_EVENT_PUBLISH_REQUEST :
                       MOQ_EVENT_SUBSCRIBE_REQUEST, &event);
    NEED(event_owned);
    uint64_t recipient_handle = sub._opaque;
    if (publish) {
        pub = event.u.publish_request.pub;
        recipient_handle = pub._opaque;
        moq_accept_publish_cfg_t accept;
        moq_accept_publish_cfg_init(&accept);
        moq_event_cleanup(&event); event_owned = false;
        NEED(moq_session_accept_publish(server, pub, &accept, now) == MOQ_OK);
    } else {
        publisher_sub = event.u.subscribe_request.sub;
        moq_accept_subscribe_cfg_t accept;
        moq_accept_subscribe_cfg_init(&accept);
        moq_event_cleanup(&event); event_owned = false;
        NEED(moq_session_accept_subscribe(server, publisher_sub, &accept, now) == MOQ_OK);
    }
    NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    drain(client); drain(server);
    if (version != MOQ_VERSION_DRAFT_22) {
        moq_publish_state_cfg_t notify;
        moq_publish_state_cfg_init(&notify);
        notify.has_forward = true;
        notify.forward = true;
        moq_result_t rc = publish ? moq_session_notify_publication_state(
            client, publisher_pub, &notify, now) : moq_session_notify_subscription_state(
            server, publisher_sub, &notify, now);
        NEED(rc == MOQ_ERR_UNSUPPORTED);
        NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
        NEED(drain(client) == 0);
        NEED(drain(server) == 0);
        NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED &&
             moq_session_state(server) == MOQ_SESS_ESTABLISHED);
        goto done;
    }
    NEED(refs.requester._v && refs.responder._v);
    moq_session_t *receiver = publish ? server : client;
    moq_stream_ref_t ref = publish ? refs.responder : refs.requester;
    if (churn) {
        size_t queued = 0, observed = 0;
        const size_t notices = 2048;
        NEED(moq_session_tick(receiver, moq_simpair_now_us(pair)) == MOQ_OK);
        /* Keep one snapshot queued across ingress calls. The event ring never
         * fills; occupied snapshot storage must not become a hard error. */
        for (size_t i = 0; i < notices; ++i) {
            moq_result_t rc = moq_session_on_bidi_stream_bytes(receiver, ref,
                wire, n, false, moq_simpair_now_us(pair));
            NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
            if (rc == MOQ_ERR_WOULD_BLOCK) {
                NEED(queued == 1);
                NEED(observe_location(receiver, publish, recipient_handle, &expected));
                ++observed; queued = 0;
                NEED(drain(receiver) == 0 && drain_actions(receiver) == 0);
                NEED(moq_session_tick(receiver, moq_simpair_now_us(pair)) == MOQ_OK);
                NEED(drain(receiver) == 0);
                NEED(moq_session_on_bidi_stream_bytes(receiver, ref, NULL, 0,
                    false, moq_simpair_now_us(pair)) == MOQ_OK);
            }
            ++queued;
            if (queued == 2) {
                NEED(observe_location(receiver, publish, recipient_handle, &expected));
                ++observed; --queued;
            }
        }
        NEED(queued == 1);
        NEED(observe_location(receiver, publish, recipient_handle, &expected));
        ++observed;
        NEED(observed == notices && drain(receiver) == 0);
        NEED(drain_actions(receiver) == 0);
        NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED &&
             moq_session_state(server) == MOQ_SESS_ESTABLISHED);
        goto done;
    }
    NEED(moq_session_on_bidi_stream_bytes(receiver, ref, wire, n, false,
                                         moq_simpair_now_us(pair)) == MOQ_OK);
    NEED(drain_actions(receiver) == 0);
    if (historical) {
        /* A real subscriber UPDATE authorizes B before the next notification.
         * Do not poll A while the update and ACK advance the same session. */
        second = expected;
        second.end_group = 10;
        if (publish) {
            moq_publication_update_cfg_t update;
            moq_publication_update_cfg_init(&update);
            update.location = &second;
            NEED(moq_session_update_publication(receiver, pub, &update,
                moq_simpair_now_us(pair)) == MOQ_OK);
        } else {
            moq_subscription_update_cfg_t update;
            moq_subscription_update_cfg_init_sized(&update, sizeof(update));
            update.location = &second;
            NEED(moq_session_update_subscription(receiver, sub, &update,
                moq_simpair_now_us(pair)) == MOQ_OK);
        }
        NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
        moq_location_filter_cfg_t current;
        moq_result_t rc = publish ? moq_session_publication_filter_sized(
            receiver, pub, &current, sizeof(current)) :
            moq_session_subscription_filter_sized(receiver, sub, &current,
                sizeof(current));
        NEED(rc == MOQ_OK && memcmp(&current, &second, sizeof(current)) == 0);
        wire[8] = 10;
        NEED(moq_session_on_bidi_stream_bytes(receiver, ref, wire, n, false,
            moq_simpair_now_us(pair)) == MOQ_OK);
        NEED(drain_actions(receiver) == 0);
    }
    event_owned = take(receiver, MOQ_EVENT_PUBLISH_STATE, &event);
    NEED(event_owned);
    NEED(event.u.publish_state.family == (publish ? MOQ_REQUEST_FAMILY_PUBLISH :
                                         MOQ_REQUEST_FAMILY_SUBSCRIBE));
    NEED(event.u.publish_state.handle.raw == recipient_handle);
    NEED(event.u.publish_state.has_filter == (kind >= 0));
    NEED(event.u.publish_state.has_forward == (kind < 0));
    if (kind < 0) NEED(event.u.publish_state.forward);
    NEED(!event.u.publish_state.has_largest &&
         event.u.publish_state.largest_group == 0 &&
         event.u.publish_state.largest_object == 0);
    const size_t offset = offsetof(state_detail_t, location);
    NEED(event.detail_size >= offset + sizeof(void *));
    const moq_location_filter_cfg_t *location = NULL;
    memcpy(&location, (const uint8_t *)&event.u.publish_state + offset,
           sizeof(location));
    if (kind < 0) {
        NEED(location == NULL);
    } else {
        NEED(location != NULL && location->struct_size >= sizeof(copy));
        NEED((uintptr_t)location % _Alignof(moq_location_filter_cfg_t) == 0);
        memcpy(&copy, location, sizeof(copy));
        expected.struct_size = copy.struct_size;
        NEED(memcmp(&copy, &expected, sizeof(copy)) == 0);
        memset(wire, 0xa5, sizeof(wire));
        NEED(memcmp(location, &copy, sizeof(copy)) == 0);
        NEED(moq_session_borrow_valid(receiver, event.borrow_epoch));
    }
    moq_event_cleanup(&event); event_owned = false;
    if (historical) {
        event_owned = moq_session_poll_events(receiver, &event, 1) == 1;
        NEED(event_owned && event.kind == (publish ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                                                   MOQ_EVENT_SUBSCRIPTION_UPDATE_OK));
        moq_event_cleanup(&event); event_owned = false;
        event_owned = moq_session_poll_events(receiver, &event, 1) == 1;
        NEED(event_owned && event.kind == MOQ_EVENT_PUBLISH_STATE);
        NEED(event.u.publish_state.family == (publish ? MOQ_REQUEST_FAMILY_PUBLISH :
                                             MOQ_REQUEST_FAMILY_SUBSCRIBE));
        NEED(event.u.publish_state.handle.raw == recipient_handle &&
             event.u.publish_state.has_filter);
        NEED(event.detail_size >= offset + sizeof(void *));
        const moq_location_filter_cfg_t *later = NULL;
        memcpy(&later, (const uint8_t *)&event.u.publish_state + offset, sizeof(later));
        NEED(later && later != location && later->struct_size == sizeof(second));
        NEED(memcmp(later, &second, sizeof(second)) == 0);
        NEED(memcmp(location, &copy, sizeof(copy)) == 0);
        moq_event_cleanup(&event); event_owned = false;
        NEED(drain(receiver) == 0);
        /* Notify did not narrow the actual acknowledged window back to A. */
        moq_subgroup_cfg_t group;
        moq_subgroup_cfg_init(&group);
        group.group_id = 7;
        moq_subgroup_handle_t subgroup;
        moq_session_t *sender = publish ? client : server;
        moq_result_t rc = publish ? moq_session_open_pub_subgroup(sender,
            publisher_pub, &group, moq_simpair_now_us(pair), &subgroup) :
            moq_session_open_subgroup(sender, publisher_sub, &group,
                moq_simpair_now_us(pair), &subgroup);
        NEED(rc == MOQ_OK);
        const uint8_t bytes[] = {0x37, 0x00};
        NEED(moq_rcbuf_create(&alloc, bytes, sizeof(bytes), &payload) == MOQ_OK);
        NEED(moq_session_write_object(sender, subgroup, 0, payload,
            moq_simpair_now_us(pair)) == MOQ_OK);
        moq_rcbuf_decref(payload); payload = NULL;
        NEED(moq_session_close_subgroup(sender, subgroup,
            moq_simpair_now_us(pair)) == MOQ_OK);
        NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
        event_owned = take(receiver, MOQ_EVENT_OBJECT_RECEIVED, &event);
        NEED(event_owned && event.u.object_received.group_id == 7 &&
             event.u.object_received.object_id == 0 && event.u.object_received.payload);
        NEED(publish ? event.u.object_received.pub._opaque == recipient_handle :
                       event.u.object_received.sub._opaque == recipient_handle);
        NEED(moq_rcbuf_len(event.u.object_received.payload) == sizeof(bytes) &&
             memcmp(moq_rcbuf_data(event.u.object_received.payload), bytes,
                    sizeof(bytes)) == 0);
        moq_event_cleanup(&event); event_owned = false;
        event_owned = moq_session_poll_events(receiver, &event, 1) == 1;
        NEED(event_owned && event.kind == MOQ_EVENT_SUBGROUP_FINISHED);
        NEED(event.u.subgroup_finished.group_id == 7 &&
             event.u.subgroup_finished.subgroup_id == 0);
        moq_event_cleanup(&event); event_owned = false;
        NEED(drain(receiver) == 0);
    }
    NEED(moq_session_tick(receiver, moq_simpair_now_us(pair) + 1) == MOQ_OK);
    if (kind >= 0) NEED(memcmp(&copy, &expected, sizeof(copy)) == 0);
    NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED &&
         moq_session_state(server) == MOQ_SESS_ESTABLISHED);
done:
    if (payload) moq_rcbuf_decref(payload);
    if (event_owned) moq_event_cleanup(&event);
    if (pair) moq_simpair_destroy(pair);
    MOQ_TEST_CHECK(memory.balance == 0);
    printf("%s: state d%u %s kind=%d history=%d churn=%d\n", failures ? "FAIL" : "PASS",
           (unsigned)version, publish ? "publish" : "subscribe", kind, historical, churn);
#undef NEED
    return failures;
}

int main(void)
{
    unsigned failed = 0, count = 0;
    for (unsigned publish = 0; publish < 2; ++publish)
        for (int kind = -1; kind <= 5; ++kind) {
            ++count;
            if (row(MOQ_VERSION_DRAFT_22, publish != 0, kind, false, false)) ++failed;
        }
    for (unsigned publish = 0; publish < 2; ++publish) {
        count += 4;
        if (row(MOQ_VERSION_DRAFT_16, publish != 0, -1, false, false)) ++failed;
        if (row(MOQ_VERSION_DRAFT_18, publish != 0, -1, false, false)) ++failed;
        if (row(MOQ_VERSION_DRAFT_22, publish != 0, 3, true, false)) ++failed;
        if (row(MOQ_VERSION_DRAFT_22, publish != 0, 1, false, true)) ++failed;
    }
    printf("%u/%u state snapshot cases passed\n", count - failed, count);
    return failed ? 1 : 0;
}
