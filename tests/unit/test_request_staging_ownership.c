/* Real incoming request handoff must not erase an unrelated by-ID owner. */
#include <moq/sim.h>
#include <moq/rcbuf.h>
#include "../../core/src/session/session_internal.h"
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, prerequisites, cases;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL d%u line%d: %s\n", (unsigned)version, __LINE__, #x); \
    failures++; } } while (0)
#define NEED(x) do { unsigned before = failures; CHECK(x); \
    if (before != failures) { ++prerequisites; goto done; } } while (0)

static bool settle(moq_simpair_t *pair)
{
    return moq_simpair_run_until_quiescent(pair, 64, NULL) == MOQ_OK;
}

static void drain(moq_session_t *session)
{
    moq_event_t event;
    for (unsigned i = 0; i < 64 && moq_session_poll_events(session, &event, 1); i++)
        moq_event_cleanup(&event);
}

static void run(moq_version_t version, unsigned announcements)
{
    unsigned before = failures;
    ++cases;
    test_alloc_state_t memory = {0};
    moq_alloc_t alloc = test_allocator(&memory);
    moq_simpair_t *pair = NULL;
    moq_rcbuf_t *body = NULL;
    moq_simpair_cfg_t config = MOQ_SIMPAIR_CFG_INIT;
    config.alloc = &alloc;
    config.version = version;
    config.seed = 7;
    config.initial_now_us = 1000;
    NEED(moq_simpair_create(&config, &pair) == MOQ_OK);
    NEED(moq_simpair_start(pair) == MOQ_OK && settle(pair));
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    NEED(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    drain(client);
    drain(server);

    moq_bytes_t ns[] = {MOQ_BYTES_LITERAL("staging")};
    moq_fetch_cfg_t fetch_config;
    moq_fetch_cfg_init(&fetch_config);
    fetch_config.track_namespace = (moq_namespace_t){ns, 1};
    fetch_config.track_name = MOQ_BYTES_LITERAL("v");
    fetch_config.end_group = 0;
    fetch_config.end_object = 1;
    moq_fetch_t local, peer = MOQ_FETCH_INVALID;
    NEED(moq_session_fetch(client, &fetch_config, 1000, &local) == MOQ_OK);
    NEED(settle(pair));
    moq_event_t event;
    while (moq_session_poll_events(server, &event, 1)) {
        if (event.kind == MOQ_EVENT_FETCH_REQUEST) peer = event.u.fetch_request.fetch;
        moq_event_cleanup(&event);
    }
    NEED(moq_fetch_is_valid(peer));
    moq_request_endpoint_t original = request_registry_find_by_id(client, 0);
    NEED(original.kind == MOQ_REQ_FETCH && original.slot >= 0);
    NEED(fetch_resolve_handle(client, local) == original.slot);

    for (unsigned i = 0; i < announcements; i++) {
        uint8_t name[] = {'p', (uint8_t)('0' + i)};
        moq_bytes_t published = {name, sizeof(name)};
        moq_publish_namespace_cfg_t announcement;
        moq_publish_namespace_cfg_init(&announcement);
        announcement.track_namespace = (moq_namespace_t){&published, 1};
        moq_announcement_t handle;
        NEED(moq_session_publish_namespace(server, &announcement, 1000, &handle) == MOQ_OK);
        NEED(settle(pair));
        moq_request_endpoint_t retained = request_registry_find_by_id(client, 0);
        CHECK(retained.kind == original.kind && retained.slot == original.slot);
        CHECK(fetch_resolve_handle(client, local) == original.slot);
        CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
        drain(client);
    }

    moq_accept_fetch_cfg_t accepted;
    moq_accept_fetch_cfg_init(&accepted);
    accepted.end_group = 0;
    accepted.end_object = 1;
    NEED(moq_session_accept_fetch(server, peer, &accepted, 1000) == MOQ_OK);
    static const uint8_t bytes[] = {'b', 'o', 'd', 'y'};
    NEED(moq_rcbuf_create(&alloc, bytes, sizeof(bytes), &body) == MOQ_OK);
    moq_fetch_object_cfg_t object;
    moq_fetch_object_cfg_init(&object);
    object.payload = body;
    NEED(moq_session_write_fetch_object(server, peer, &object, 1000) == MOQ_OK);
    NEED(moq_session_end_fetch(server, peer, 1000) == MOQ_OK);
    CHECK(settle(pair));
    unsigned oks = 0, objects = 0, completes = 0, closed = 0;
    while (moq_session_poll_events(client, &event, 1)) {
        if (event.kind == MOQ_EVENT_FETCH_OK) {
            CHECK(moq_fetch_eq(event.u.fetch_ok.fetch, local));
            oks++;
        } else if (event.kind == MOQ_EVENT_FETCH_OBJECT) {
            CHECK(moq_fetch_eq(event.u.fetch_object.fetch, local));
            CHECK(event.u.fetch_object.payload &&
                  moq_rcbuf_len(event.u.fetch_object.payload) == sizeof(bytes) &&
                  memcmp(moq_rcbuf_data(event.u.fetch_object.payload), bytes, sizeof(bytes)) == 0);
            objects++;
        } else if (event.kind == MOQ_EVENT_FETCH_COMPLETE) {
            CHECK(moq_fetch_eq(event.u.fetch_complete.fetch, local));
            completes++;
        } else if (event.kind == MOQ_EVENT_SESSION_CLOSED) {
            closed++;
        }
        moq_event_cleanup(&event);
    }
    CHECK(oks == 1 && objects == 1 && completes == 1 && closed == 0);
    CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
done:
    moq_rcbuf_decref(body);
    if (pair) moq_simpair_destroy(pair);
    CHECK(memory.balance == 0);
    printf("%s: d%u FETCH0 with %u incoming namespace requests\n",
           before == failures ? "PASS" : "FAIL", (unsigned)version, announcements);
}

static void zero_subscription_release(void)
{
    moq_version_t version = MOQ_VERSION_DRAFT_16;
    unsigned before = failures;
    ++cases;
    test_alloc_state_t memory = {0};
    moq_alloc_t alloc = test_allocator(&memory);
    moq_simpair_t *pair = NULL;
    moq_simpair_cfg_t config = MOQ_SIMPAIR_CFG_INIT;
    config.alloc = &alloc;
    config.version = version;
    config.seed = 7;
    config.initial_now_us = 1000;
    config.client_send_request_capacity = true;
    config.server_send_request_capacity = true;
    config.client_initial_request_capacity = 32;
    config.server_initial_request_capacity = 32;
    NEED(moq_simpair_create(&config, &pair) == MOQ_OK);
    NEED(moq_simpair_start(pair) == MOQ_OK && settle(pair));
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    drain(client);
    drain(server);
    moq_bytes_t ns = MOQ_BYTES_LITERAL("real-sub-zero");
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.track_namespace = (moq_namespace_t){&ns, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("video");
    moq_subscription_t local = MOQ_SUBSCRIPTION_INVALID;
    moq_subscription_t peer = MOQ_SUBSCRIPTION_INVALID;
    NEED(moq_session_subscribe(client, &cfg, 1000, &local) == MOQ_OK);
    NEED(settle(pair));
    moq_event_t event;
    while (moq_session_poll_events(server, &event, 1)) {
        if (event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            peer = event.u.subscribe_request.sub;
        moq_event_cleanup(&event);
    }
    NEED(moq_subscription_is_valid(peer));
    moq_request_endpoint_t registered = request_registry_find_by_id(client, 0);
    NEED(registered.kind == MOQ_REQ_SUBSCRIPTION &&
         registered.slot == sub_resolve_handle(client, local));
    registered = request_registry_find_by_id(server, 0);
    NEED(registered.kind == MOQ_REQ_SUBSCRIPTION &&
         registered.slot == sub_resolve_handle(server, peer));
    moq_accept_subscribe_cfg_t accepted;
    moq_accept_subscribe_cfg_init(&accepted);
    NEED(moq_session_accept_subscribe(server, peer, &accepted, 1000) == MOQ_OK);
    NEED(settle(pair));
    drain(client);
    /* Zero is a valid registered subscription ID on both peers, not a marker
     * for the unregistered staging slots exercised by the other cases. */
    NEED(moq_session_unsubscribe(client, local, 1000) == MOQ_OK);
    CHECK(request_registry_find_by_id(client, 0).kind == MOQ_REQ_NONE);
    NEED(settle(pair));
    CHECK(request_registry_find_by_id(server, 0).kind == MOQ_REQ_NONE);
done:
    if (pair) moq_simpair_destroy(pair);
    CHECK(memory.balance == 0);
    printf("%s: real D16 subscription ID0 removes both registry owners\n",
           before == failures ? "PASS" : "FAIL");
}

int main(void)
{
    run(MOQ_VERSION_DRAFT_18, 0);
    run(MOQ_VERSION_DRAFT_18, 1);
    run(MOQ_VERSION_DRAFT_18, 3);
    run(MOQ_VERSION_DRAFT_22, 0);
    run(MOQ_VERSION_DRAFT_22, 1);
    run(MOQ_VERSION_DRAFT_22, 3);
    zero_subscription_release();
    printf("request staging ownership: %u cases, %u checks, %u failures, "
           "%u prerequisite failures\n", cases, checks, failures, prerequisites);
    return failures ? 1 : 0;
}
