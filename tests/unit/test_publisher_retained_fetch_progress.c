#include <moq/publisher.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include <string.h>

static int failures;
static unsigned setup_failures;

#define SETUP(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "SETUP FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++setup_failures; \
        ++failures; \
        goto cleanup; \
    } \
} while (0)

typedef enum {
    AUTOMATIC,
    SERIALIZED_MANUAL,
    REPEATED_TICK,
    CANCEL_SECOND,
    DESTROY_PENDING
} drive_t;

typedef struct {
    const char *name;
    moq_version_t version;
    uint32_t actions;
    drive_t drive;
} case_t;

static void discard_events(moq_session_t *session)
{
    moq_event_t event;
    while (moq_session_poll_events(session, &event, 1))
        moq_event_cleanup(&event);
}

static void run_case(const case_t *test)
{
    int before = failures;
    test_alloc_state_t allocations = {0};
    moq_alloc_t alloc = test_allocator(&allocations);
    moq_simpair_t *pair = NULL;
    moq_publisher_t *publisher = NULL;
    moq_pub_track_t *track = NULL;
    moq_rcbuf_t *payload = NULL;
    unsigned oks[2] = {0}, objects[2] = {0}, completes[2] = {0};
    unsigned errors = 0, closes = 0, ignored = 0;
    bool blocked = false;
    moq_fetch_t fetches[2] = {MOQ_FETCH_INVALID, MOQ_FETCH_INVALID};
    moq_simpair_cfg_t sim = MOQ_SIMPAIR_CFG_INIT;
    sim.alloc = &alloc;
    sim.version = test->version;
    sim.max_actions = test->actions;
    sim.seed = 7;
    sim.initial_now_us = 1000;
    sim.client_send_request_capacity = true;
    sim.client_initial_request_capacity = 32;
    sim.server_send_request_capacity = true;
    sim.server_initial_request_capacity = 32;
    SETUP(moq_simpair_create(&sim, &pair) == MOQ_OK);
    SETUP(moq_simpair_start(pair) == MOQ_OK);
    SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    discard_events(client);
    discard_events(server);

    moq_pub_cfg_t publisher_cfg;
    moq_pub_cfg_init_sized(&publisher_cfg, sizeof(publisher_cfg));
    publisher_cfg.accept_mode = MOQ_PUB_ACCEPT_ALL;
    SETUP(moq_pub_create(server, &alloc, &publisher_cfg, &publisher) == MOQ_OK);
    moq_bytes_t ns = MOQ_BYTES_LITERAL("retained-fetch-progress");
    moq_pub_track_cfg_t track_cfg;
    moq_pub_track_cfg_init_sized(&track_cfg, sizeof(track_cfg));
    track_cfg.track_namespace = (moq_namespace_t){&ns, 1};
    track_cfg.track_name = MOQ_BYTES_LITERAL("video");
    SETUP(moq_pub_add_track(publisher, &track_cfg, moq_simpair_now_us(pair),
                            &track) == MOQ_OK);
    SETUP(moq_rcbuf_create(&alloc, (const uint8_t *)"one", 3, &payload) == MOQ_OK);
    moq_pub_retained_object_t object = {
        .object_id = 0, .payload = payload, .end_of_group = true
    };
    moq_pub_retained_group_cfg_t retained;
    moq_pub_retained_group_cfg_init(&retained);
    retained.group_id = 3;
    retained.objects = &object;
    retained.object_count = 1;
    SETUP(moq_pub_set_retained_group(publisher, track, &retained) == MOQ_OK);

    /* Both requests reach the server before the facade polls either event. */
    for (unsigned i = 0; i < 2; ++i) {
        moq_fetch_cfg_t request;
        moq_fetch_cfg_init(&request);
        request.track_namespace = (moq_namespace_t){&ns, 1};
        request.track_name = MOQ_BYTES_LITERAL("video");
        request.start_group = 3;
        request.end_group = 3;
        request.end_object = 1;
        SETUP(moq_session_fetch(client, &request, moq_simpair_now_us(pair),
                                 &fetches[i]) == MOQ_OK);
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    }
    SETUP(!moq_fetch_eq(fetches[0], fetches[1]));

    for (unsigned round = 0; round < 24; ++round) {
        uint64_t now_us = moq_simpair_now_us(pair);
        moq_result_t rc;
        if (test->drive == SERIALIZED_MANUAL) {
            rc = moq_pub_flush(publisher, now_us);
            SETUP(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
            if (rc == MOQ_OK) {
                moq_event_t event;
                if (moq_session_poll_events(server, &event, 1)) {
                    moq_pub_event_result_t result;
                    rc = moq_pub_handle_event(publisher, &event, now_us, &result);
                    ignored += result == MOQ_PUB_EVENT_IGNORED;
                    moq_event_cleanup(&event);
                    SETUP(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
                }
            }
        } else {
            rc = moq_pub_tick(publisher, now_us);
            SETUP(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        }
        blocked |= rc == MOQ_ERR_WOULD_BLOCK;
        if (round == 0 && test->drive == DESTROY_PENDING) {
            MOQ_TEST_CHECK(rc == MOQ_ERR_WOULD_BLOCK);
            break;
        }
        if (test->drive == REPEATED_TICK) {
            /* A retry before action drain must still report backpressure. */
            moq_result_t retry = moq_pub_tick(publisher, now_us);
            SETUP(retry == MOQ_OK || retry == MOQ_ERR_WOULD_BLOCK);
            if (rc == MOQ_ERR_WOULD_BLOCK)
                MOQ_TEST_CHECK(retry == MOQ_ERR_WOULD_BLOCK);
        }
        if (round == 0 && test->drive == CANCEL_SECOND) {
            MOQ_TEST_CHECK(rc == MOQ_ERR_WOULD_BLOCK);
            SETUP(moq_session_fetch_cancel(client, fetches[1], now_us) == MOQ_OK);
        }
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
        moq_event_t event;
        while (moq_session_poll_events(client, &event, 1)) {
            for (unsigned i = 0; i < 2; ++i) {
                if (event.kind == MOQ_EVENT_FETCH_OK &&
                    moq_fetch_eq(event.u.fetch_ok.fetch, fetches[i])) ++oks[i];
                if (event.kind == MOQ_EVENT_FETCH_OBJECT &&
                    moq_fetch_eq(event.u.fetch_object.fetch, fetches[i])) {
                    ++objects[i];
                    MOQ_TEST_CHECK(event.u.fetch_object.group_id == 3);
                    MOQ_TEST_CHECK(event.u.fetch_object.object_id == 0);
                    MOQ_TEST_CHECK(moq_rcbuf_len(event.u.fetch_object.payload) == 3);
                    if (moq_rcbuf_len(event.u.fetch_object.payload) == 3)
                        MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(event.u.fetch_object.payload),
                                              "one", 3) == 0);
                }
                if (event.kind == MOQ_EVENT_FETCH_COMPLETE &&
                    moq_fetch_eq(event.u.fetch_complete.fetch, fetches[i])) ++completes[i];
            }
            errors += event.kind == MOQ_EVENT_FETCH_ERROR;
            closes += event.kind == MOQ_EVENT_SESSION_CLOSED;
            moq_event_cleanup(&event);
        }
    }
    printf("%s: ok=%u,%u object=%u,%u complete=%u,%u errors=%u closes=%u\n",
           test->name, oks[0], oks[1], objects[0], objects[1],
           completes[0], completes[1], errors, closes);
    MOQ_TEST_CHECK(errors == 0 && closes == 0 && ignored == 0);
    MOQ_TEST_CHECK(blocked == (test->actions == 3));
    if (test->drive != DESTROY_PENDING) {
        for (unsigned i = 0; i < 2; ++i) {
            unsigned expected = test->drive == CANCEL_SECOND && i == 1 ? 0 : 1;
            MOQ_TEST_CHECK_EQ_INT(oks[i], expected);
            MOQ_TEST_CHECK_EQ_INT(objects[i], expected);
            MOQ_TEST_CHECK_EQ_INT(completes[i], expected);
        }
    }

cleanup:
    moq_pub_destroy(publisher);
    if (pair) {
        discard_events(moq_simpair_server(pair));
        discard_events(moq_simpair_client(pair));
        moq_simpair_destroy(pair);
    }
    if (payload) {
        MOQ_TEST_CHECK_EQ_INT(moq_rcbuf_refcount(payload), 1);
        moq_rcbuf_decref(payload);
    }
    MOQ_TEST_CHECK_EQ_INT(allocations.balance, 0);
    printf("%s: %s\n", before == failures ? "PASS" : "FAIL CASE", test->name);
}

int main(void)
{
    const case_t tests[] = {
        {"d22_automatic_capacity64", MOQ_VERSION_DRAFT_22, 64, AUTOMATIC},
        {"d22_serialized_manual_capacity3", MOQ_VERSION_DRAFT_22, 3, SERIALIZED_MANUAL},
        {"d16_automatic_capacity3", MOQ_VERSION_DRAFT_16, 3, AUTOMATIC},
        {"d18_automatic_capacity3", MOQ_VERSION_DRAFT_18, 3, AUTOMATIC},
        {"d22_automatic_capacity3", MOQ_VERSION_DRAFT_22, 3, AUTOMATIC},
        {"d22_repeated_tick_capacity3", MOQ_VERSION_DRAFT_22, 3, REPEATED_TICK},
        {"d22_cancel_queued_second", MOQ_VERSION_DRAFT_22, 3, CANCEL_SECOND},
        {"d22_destroy_pending_and_queued", MOQ_VERSION_DRAFT_22, 3, DESTROY_PENDING}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
        run_case(&tests[i]);
    printf("publisher retained FETCH progress: %zu cases, %d failures, %u setup failures\n",
           sizeof(tests) / sizeof(tests[0]), failures, setup_failures);
    return failures ? 1 : 0;
}
