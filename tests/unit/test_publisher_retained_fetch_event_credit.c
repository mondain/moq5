#include <moq/publisher.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "../../core/src/session/session_internal.h"
#include <stdio.h>
#include <string.h>

/* Private layout inspection only: all input, polling and mutation use public
 * APIs. Link this TU to the matching shipping core, not a second core copy. */
static unsigned failures, setup_failures, checks;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

#define SETUP(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "SETUP FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
        ++setup_failures; \
        goto cleanup; \
    } \
} while (0)

typedef enum { DRIVE, REPEAT, CANCEL_FIRST, CANCEL_SECOND, CLOSE } drive_t;

typedef struct {
    const char *name;
    bool refused, full, second;
    drive_t drive;
} case_t;

typedef struct {
    moq_pub_track_t *track;
    unsigned requests, terminals, closes;
    moq_pub_fill_request_t request;
    moq_pub_fill_terminal_t terminal;
} probe_t;

typedef struct {
    moq_fetch_t client, server;
    uint64_t request_id;
    moq_stream_ref_t bidi, data;
    unsigned responses, headers, objects, fins, resets, request_fins;
} fetch_t;

static size_t event_count(const moq_session_t *s)
{ return s->event_tail - s->event_head; }

static const moq_event_t *queued_event(const moq_session_t *s, size_t n)
{
    if (n >= event_count(s) || !s->event_cap) return NULL;
    return &s->events[(s->event_head + n) % s->event_cap];
}

static const moq_fetch_entry_t *find_fetch(const moq_session_t *s, moq_fetch_t h)
{
    if (moq_fetch_eq(h, MOQ_FETCH_INVALID)) return NULL;
    for (size_t i = 0; i < s->fetch_cap; ++i)
        if (s->fetches[i].state != MOQ_FETCH_FREE &&
            moq_fetch_eq(s->fetches[i].handle, h)) return &s->fetches[i];
    return NULL;
}

static void discard_events(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

static moq_result_t pump(moq_publisher_t *pub, moq_session_t *s,
                          uint64_t now, bool manual)
{
    if (!manual) return moq_pub_tick(pub, now);
    moq_result_t rc = moq_pub_flush(pub, now);
    if (rc < 0 && rc != MOQ_ERR_WOULD_BLOCK && rc != MOQ_ERR_CLOSED) return rc;
    moq_event_t event;
    for (size_t i = 0; i < s->event_cap && moq_session_poll_events(s, &event, 1); ++i) {
        moq_pub_event_result_t result;
        rc = moq_pub_handle_event(pub, &event, now, &result);
        CHECK(result != MOQ_PUB_EVENT_ERROR);
        moq_event_cleanup(&event);
        if (rc < 0 && rc != MOQ_ERR_WOULD_BLOCK) return rc;
    }
    return moq_pub_flush(pub, now);
}

static moq_pub_fill_decision_t on_fill(void *ctx, moq_pub_track_t *track,
                                      const moq_pub_fill_request_t *request)
{
    probe_t *p = ctx;
    CHECK(track == p->track);
    CHECK(request->struct_size == sizeof(*request));
    CHECK(p->requests == 0);
    p->request = *request;
    ++p->requests;
    return MOQ_PUB_FILL_DEFER;
}

static void on_terminal(void *ctx, moq_pub_track_t *track,
                        const moq_pub_fill_terminal_t *terminal)
{
    probe_t *p = ctx;
    CHECK(track == p->track);
    CHECK(terminal->struct_size == sizeof(*terminal));
    CHECK(p->terminals == 0);
    CHECK(moq_pub_fill_eq(terminal->fill, p->request.fill));
    CHECK(moq_subscription_eq(terminal->sub, p->request.sub));
    CHECK(moq_publication_eq(terminal->pub, p->request.pub));
    CHECK(terminal->initiating_request_id == p->request.initiating_request_id);
    CHECK(terminal->reason == MOQ_FILL_LOCAL_ABORTED);
    CHECK(terminal->error_domain == MOQ_FILL_ERROR_STREAM);
    CHECK(terminal->error_code == 6);
    p->terminal = *terminal;
    ++p->terminals;
}

static void on_closed(void *ctx, uint64_t code)
{
    probe_t *p = ctx;
    CHECK(code == 0x99);
    ++p->closes;
}

static moq_result_t namespace_request(moq_session_t *s, const char *name,
                                      uint64_t now)
{
    moq_bytes_t part = {(const uint8_t *)name, strlen(name)};
    moq_publish_namespace_cfg_t cfg;
    moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    moq_announcement_t announcement;
    return moq_session_publish_namespace(s, &cfg, now, &announcement);
}

static bool capture_fetch(const moq_session_t *server, const moq_session_t *client,
                           fetch_t *f, size_t event_index)
{
    const moq_event_t *event = queued_event(server, event_index);
    const moq_fetch_entry_t *local = find_fetch(client, f->client);
    if (!event || event->kind != MOQ_EVENT_FETCH_REQUEST || !local) return false;
    f->server = event->u.fetch_request.fetch;
    const moq_fetch_entry_t *remote = find_fetch(server, f->server);
    if (!remote || remote->state != MOQ_FETCH_PENDING_PUBLISHER ||
        remote->request_id != local->request_id || !remote->rs.ref._v)
        return false;
    f->request_id = remote->request_id;
    f->bidi = remote->rs.ref;
    return f->request_id < 100 && (f->request_id & 1u) == 0;
}

/* D22 sections 8.9/9.5: both are lawful FETCH UPDATE parameters. The
 * unregistered USE_ALIAS is a request-local 0x17 refusal, not bad input.
 * ID100 is fresh, client-parity, and follows every simulated peer request. */
static moq_result_t early_update(moq_session_t *s, moq_stream_ref_t ref,
                                 bool refused, uint64_t now)
{
    static const uint8_t priority[] = {0x02, 0, 4, 100, 1, 0x20, 3};
    static const uint8_t alias[] = {0x02, 0, 6, 100, 1, 0x03, 2, 2, 99};
    return moq_session_on_bidi_stream_bytes(s, ref,
        refused ? alias : priority, refused ? sizeof(alias) : sizeof(priority),
        false, now);
}

static int response_owner(fetch_t f[2], moq_stream_ref_t ref)
{
    for (int i = 0; i < 2; ++i)
        if (f[i].bidi._v && f[i].bidi._v == ref._v) return i;
    return -1;
}

static void check_response(fetch_t *f, const moq_send_bidi_stream_action_t *a,
                            bool update, bool refused)
{
    static const uint8_t fetch_ok[] = {0x18, 0, 4, 0, 3, 0, 0};
    static const uint8_t update_ok[] = {0x07, 0, 1, 0};
    static const uint8_t update_error[] = {0x05, 0, 3, 0x17, 0, 0};
    size_t extra = !update ? 0 : refused ? sizeof(update_error) : sizeof(update_ok);
    CHECK(a->len == sizeof(fetch_ok) + extra);
    if (a->len == sizeof(fetch_ok) + extra) {
        CHECK(memcmp(a->data, fetch_ok, sizeof(fetch_ok)) == 0);
        if (update)
            CHECK(memcmp(a->data + sizeof(fetch_ok),
                refused ? update_error : update_ok, extra) == 0);
    }
    CHECK(a->fin == (update && refused));
    if (a->fin) {
        CHECK(f->request_fins == 0);
        ++f->request_fins;
    }
    ++f->responses;
}

static void drain_actions(moq_session_t *s, fetch_t f[2], bool refused,
                           unsigned *closes)
{
    moq_action_t action;
    while (moq_session_poll_actions(s, &action, 1)) {
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            int i = response_owner(f, action.u.send_bidi_stream.stream_ref);
            /* Tick mode is the session's only consumer, so it refuses the
             * filler namespace requests it does not serve: REQUEST_ERROR
             * NOT_SUPPORTED (0x3) with FIN on each one's own stream. */
            static const uint8_t not_supported[] = {0x05, 0, 3, 0x03, 0, 0};
            const moq_send_bidi_stream_action_t *sb = &action.u.send_bidi_stream;
            bool refusal = i < 0 && sb->fin && sb->len == sizeof(not_supported) &&
                           memcmp(sb->data, not_supported, sizeof(not_supported)) == 0;
            CHECK(i >= 0 || refusal);
            if (i >= 0) check_response(&f[i], &action.u.send_bidi_stream,
                                       i == 0, refused);
        } else if (action.kind == MOQ_ACTION_SEND_DATA) {
            const moq_send_data_action_t *a = &action.u.send_data;
            int owner = -1;
            for (int i = 0; i < 2; ++i) {
                if (f[i].data._v && f[i].data._v == a->stream_ref._v) owner = i;
                if (a->header_len == 2 && a->header[0] == 0x05 &&
                    f[i].bidi._v && a->header[1] == f[i].request_id) {
                    CHECK(f[i].headers == 0 && !a->payload && !a->fin);
                    f[i].data = a->stream_ref;
                    ++f[i].headers;
                    owner = i;
                }
            }
            CHECK(owner >= 0);
            if (owner >= 0) {
                if (a->payload) {
                    CHECK(moq_rcbuf_len(a->payload) == 3);
                    if (moq_rcbuf_len(a->payload) == 3)
                        CHECK(memcmp(moq_rcbuf_data(a->payload), "one", 3) == 0);
                    ++f[owner].objects;
                }
                f[owner].fins += a->fin;
            }
        } else if (action.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            int i = response_owner(f, action.u.close_bidi_stream.stream_ref);
            CHECK(i >= 0);
            if (i >= 0) {
                CHECK(i != 0 || !refused);
                CHECK(f[i].responses == 1 && f[i].headers == 1 &&
                      f[i].objects == 1 && f[i].fins == 1 && f[i].resets == 0);
                CHECK(f[i].request_fins == 0);
                ++f[i].request_fins;
            }
        } else if (action.kind == MOQ_ACTION_RESET_DATA) {
            const moq_reset_data_action_t *a = &action.u.reset_data;
            CHECK(f[0].headers == 1 && a->stream_ref._v == f[0].data._v);
            CHECK(refused && a->error_code == 1 && a->reliable_size == 2);
            if (f[0].data._v == a->stream_ref._v) ++f[0].resets;
            else ++f[1].resets;
        } else if (action.kind == MOQ_ACTION_CLOSE_SESSION) {
            CHECK(action.u.close_session.code == 0x99);
            ++*closes;
        } else {
            fprintf(stderr, "unexpected action kind=%u\n", action.kind);
            CHECK(false);
        }
        moq_action_cleanup(&action);
    }
}

static void check_fetch(const moq_session_t *s, const fetch_t *f,
                         unsigned answered, bool refused)
{
    CHECK(f->responses == answered && f->headers == answered &&
          f->objects == (refused ? 0 : answered) &&
          f->fins == (refused ? 0 : answered));
    CHECK(f->resets == (refused ? answered : 0));
    CHECK(f->request_fins == answered);
    CHECK(find_fetch(s, f->server) == NULL);
}

static void run_case(const case_t *test, bool manual)
{
    unsigned before = failures, setup_before = setup_failures;
    test_alloc_state_t allocations = {0};
    moq_alloc_t alloc = test_allocator(&allocations);
    moq_simpair_t *pair = NULL;
    moq_publisher_t *publisher = NULL;
    moq_rcbuf_t *payload = NULL;
    probe_t probe = {0};
    fetch_t fetch[2] = {0};
    unsigned blocked = 0, closed_actions = 0;
    const bool cancelling = test->drive == CANCEL_FIRST || test->drive == CANCEL_SECOND;
    const uint64_t now = 1000;
    moq_simpair_cfg_t sim = MOQ_SIMPAIR_CFG_INIT;
    sim.alloc = &alloc;
    sim.version = MOQ_VERSION_DRAFT_22;
    sim.max_actions = 3;
    sim.seed = 7;
    sim.initial_now_us = now;
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
    SETUP(server->event_cap == 16 && moq_session_action_capacity(server) == 3);
    moq_pub_cfg_t cfg;
    moq_pub_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.accept_mode = MOQ_PUB_ACCEPT_ALL;
    cfg.max_fills = 1;
    cfg.max_fill_snapshot_bytes = 3;
    cfg.on_fill_request = on_fill;
    cfg.on_fill_request_ctx = &probe;
    cfg.on_fill_terminal = on_terminal;
    cfg.on_fill_terminal_ctx = &probe;
    cfg.callbacks.ctx = &probe;
    cfg.callbacks.on_closed = on_closed;
    SETUP(moq_pub_create(server, &alloc, &cfg, &publisher) == MOQ_OK);
    moq_bytes_t ns = MOQ_BYTES_LITERAL("retained-fetch-event-credit");
    moq_pub_track_cfg_t track;
    moq_pub_track_cfg_init_sized(&track, sizeof(track));
    track.track_namespace = (moq_namespace_t){&ns, 1};
    track.track_name = MOQ_BYTES_LITERAL("video");
    SETUP(moq_pub_add_track(publisher, &track, now, &probe.track) == MOQ_OK);
    SETUP(moq_rcbuf_create(&alloc, (const uint8_t *)"one", 3, &payload) == MOQ_OK);
    moq_pub_retained_object_t object = {
        .object_id = 0, .payload = payload, .end_of_group = true
    };
    moq_pub_retained_group_cfg_t retained;
    moq_pub_retained_group_cfg_init(&retained);
    retained.group_id = 3;
    retained.objects = &object;
    retained.object_count = 1;
    SETUP(moq_pub_set_retained_group(publisher, probe.track, &retained) == MOQ_OK);

    moq_location_filter_cfg_t location, history;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    moq_location_filter_cfg_init(&history);
    history.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
    history.relative_group_offset = 1;
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &history;
    moq_subscribe_cfg_t subscribe;
    moq_subscribe_cfg_init_sized(&subscribe, sizeof(subscribe));
    subscribe.track_namespace = (moq_namespace_t){&ns, 1};
    subscribe.track_name = MOQ_BYTES_LITERAL("video");
    subscribe.location = &location;
    subscribe.fill = &fill;
    subscribe.forward = true;
    moq_subscription_t sub;
    SETUP(moq_session_subscribe(client, &subscribe, now, &sub) == MOQ_OK);
    SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    for (unsigned i = 0; i < 12; ++i) {
        moq_result_t rc = pump(publisher, server, now, manual);
        SETUP(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    }
    discard_events(client);
    SETUP(probe.requests == 1 && probe.terminals == 0 && !event_count(server));
    SETUP(probe.request.initiating_request_id < 100);

    moq_fetch_cfg_t request;
    moq_fetch_cfg_init(&request);
    request.track_namespace = (moq_namespace_t){&ns, 1};
    request.track_name = MOQ_BYTES_LITERAL("video");
    request.start_group = request.end_group = 3;
    request.end_object = 1;
    SETUP(moq_session_fetch(client, &request, now, &fetch[0].client) == MOQ_OK);
    SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    SETUP(event_count(server) == 1 && capture_fetch(server, client, &fetch[0], 0));
    SETUP(namespace_request(server, "held-a", now) == MOQ_OK);
    SETUP(namespace_request(server, "held-b", now) == MOQ_OK);
    SETUP(moq_session_action_capacity(server) == 1);
    SETUP(pump(publisher, server, now, manual) == MOQ_ERR_WOULD_BLOCK);
    SETUP(!event_count(server) && find_fetch(server, fetch[0].server));
    /* No action drain: repeated ticks must report actual backpressure. */
    for (unsigned i = 0; i < 3; ++i) {
        SETUP(pump(publisher, server, now, manual) == MOQ_ERR_WOULD_BLOCK);
        SETUP(moq_session_action_capacity(server) == 1 && !event_count(server));
    }
    SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    discard_events(client);
    SETUP(moq_session_action_capacity(server) == 3);

    if (test->second) {
        SETUP(moq_session_fetch(client, &request, now, &fetch[1].client) == MOQ_OK);
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
        SETUP(event_count(server) == 1 && capture_fetch(server, client, &fetch[1], 0));
        SETUP(fetch[0].request_id != fetch[1].request_id &&
              fetch[0].bidi._v != fetch[1].bidi._v &&
              !moq_fetch_eq(fetch[0].server, fetch[1].server));
    }
    SETUP(moq_pub_abort_fill(publisher, probe.request.fill, 6, now) == MOQ_OK);
    SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    discard_events(client);
    const moq_event_t *terminal = queued_event(server, test->second ? 1 : 0);
    SETUP(terminal && terminal->kind == MOQ_EVENT_FILL_TERMINAL);
    SETUP(terminal->u.fill_terminal.initiating_request_id ==
          probe.request.initiating_request_id);
    /* Leave one slot for peer cancellation when that is the final input. */
    size_t wanted = server->event_cap - ((!test->full || cancelling) ? 1u : 0u);
    for (size_t i = event_count(server); i < wanted; ++i) {
        char name[40];
        int len = snprintf(name, sizeof(name), "event-credit-%zu", i);
        SETUP(len > 0 && (size_t)len < sizeof(name));
        SETUP(namespace_request(client, name, now) == MOQ_OK);
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
    }
    SETUP(event_count(server) == wanted && moq_session_action_capacity(server) == 3);
    /* All simulated client requests are still owned here; none used ID100. */
    for (size_t i = 0; i < client->ann_cap; ++i)
        if (client->announcements[i].state != MOQ_ANN_FREE)
            SETUP(client->announcements[i].request_id < 100);
    SETUP(early_update(server, fetch[0].bidi, test->refused, now) == MOQ_OK);
    const moq_fetch_entry_t *first = find_fetch(server, fetch[0].server);
    SETUP(first && first->upd_failed == test->refused && first->upd_owed == 1);
    SETUP(!test->refused || first->upd_fail_code == 0x17);
    SETUP(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    SETUP(probe.terminals == 0 && failures == before);
    if (cancelling) {
        unsigned i = test->drive == CANCEL_FIRST ? 0 : 1;
        SETUP(moq_session_fetch_cancel(client, fetch[i].client, now) == MOQ_OK);
        SETUP(moq_simpair_run_until_quiescent(pair, 8, NULL) == MOQ_OK);
        SETUP(!find_fetch(server, fetch[i].server));
        SETUP(event_count(server) == server->event_cap);
    }
    printf("%s prerequisites=0 events=%zu/%zu actions_free=%zu\n", test->name,
           event_count(server), server->event_cap, moq_session_action_capacity(server));

    if (test->drive == CLOSE) {
        SETUP(moq_session_close(server, 0x99, "credit close", now) == MOQ_OK);
        for (unsigned round = 0; round < 3; ++round) {
            moq_result_t rc = pump(publisher, server, now, manual);
            CHECK(rc == MOQ_OK || rc == MOQ_ERR_CLOSED);
            drain_actions(server, fetch, test->refused, &closed_actions);
        }
        CHECK(moq_session_state(server) == MOQ_SESS_CLOSED);
        CHECK(probe.closes == 1 && closed_actions == 1 && probe.terminals == 0);
        /* Close supersedes queued terminals. Core slots may remain allocated
         * until destroy, but no request can produce output and facade-held
         * snapshots must already be released (caller + track retain two refs). */
        CHECK(moq_rcbuf_refcount(payload) == 2);
        moq_accept_fetch_cfg_t accept;
        moq_accept_fetch_cfg_init(&accept);
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(fetch[i].responses == 0 && fetch[i].headers == 0 &&
                  fetch[i].objects == 0 && fetch[i].fins == 0 && fetch[i].resets == 0 &&
                  fetch[i].request_fins == 0);
            CHECK(moq_session_accept_fetch(server, fetch[i].server, &accept, now) ==
                  MOQ_ERR_CLOSED);
        }
        CHECK(moq_session_action_capacity(server) == 3);
    } else {
        /* The client API did not originate ID100. Inspect the publisher's
         * literal output locally rather than feeding an unowned response to it. */
        for (unsigned round = 0; round < 24; ++round) {
            moq_result_t rc = pump(publisher, server, now, manual);
            CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
            blocked += rc == MOQ_ERR_WOULD_BLOCK;
            if (test->drive == REPEAT && rc == MOQ_ERR_WOULD_BLOCK)
                CHECK(pump(publisher, server, now, manual) == MOQ_ERR_WOULD_BLOCK);
            drain_actions(server, fetch, test->refused, &closed_actions);
        }
        printf("%s blocked=%u/24 first=%u/%u/%u/%u/%u second=%u/%u/%u/%u/%u "
               "terminal=%u retired=%d,%d events=%zu actions_free=%zu\n", test->name,
               blocked, fetch[0].responses, fetch[0].headers, fetch[0].objects,
               fetch[0].fins, fetch[0].resets, fetch[1].responses, fetch[1].headers,
               fetch[1].objects, fetch[1].fins, fetch[1].resets, probe.terminals,
               !find_fetch(server, fetch[0].server), !find_fetch(server, fetch[1].server),
               event_count(server), moq_session_action_capacity(server));
        CHECK(probe.terminals == 1);
        check_fetch(server, &fetch[0], test->drive == CANCEL_FIRST ? 0 : 1,
                    test->refused);
        if (test->second)
            check_fetch(server, &fetch[1], test->drive == CANCEL_SECOND ? 0 : 1, false);
        CHECK(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
        CHECK(probe.closes == 0 && closed_actions == 0);
        if (!test->second && find_fetch(server, fetch[0].server)) {
            /* Diagnostic recovery only, after the four outcome assertions.
             * No private queue mutation: dispatch exactly the real head event. */
            moq_event_t event;
            SETUP(moq_session_poll_events(server, &event, 1) == 1);
            CHECK(event.kind == MOQ_EVENT_FILL_TERMINAL);
            moq_pub_event_result_t consumed;
            CHECK(moq_pub_handle_event(publisher, &event, now, &consumed) == MOQ_OK);
            CHECK(consumed == MOQ_PUB_EVENT_CONSUMED);
            moq_event_cleanup(&event);
            for (unsigned round = 0; round < 4; ++round) {
                moq_result_t rc = pump(publisher, server, now, manual);
                CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
                drain_actions(server, fetch, test->refused, &closed_actions);
            }
            CHECK(probe.terminals == 1);
            check_fetch(server, &fetch[0], 1, test->refused);
            printf("%s public-head recovery terminal=%u retired=%d\n", test->name,
                   probe.terminals, !find_fetch(server, fetch[0].server));
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
        CHECK(moq_rcbuf_refcount(payload) == 1);
        moq_rcbuf_decref(payload);
    }
    CHECK(allocations.balance == 0);
    printf("%s: %s failures=%u prerequisites=%u balance=%lld\n", test->name,
           failures == before ? "PASS" : "FAIL", failures - before,
           setup_failures - setup_before, (long long)allocations.balance);
}

int main(int argc, char **argv)
{
    const case_t cases[] = {
        {"accepted_full", false, true, false, DRIVE},
        {"refused_one_free", true, false, false, DRIVE},
        {"refused_full", true, true, false, DRIVE},
        {"accepted_second_full", false, true, true, DRIVE},
        {"refused_second_one_free", true, false, true, DRIVE},
        {"refused_second_full", true, true, true, DRIVE},
        {"refused_second_repeated", true, true, true, REPEAT},
        {"cancel_staged_first", true, true, true, CANCEL_FIRST},
        {"cancel_queued_second", true, true, true, CANCEL_SECOND},
        {"close_pending_and_queued", true, true, true, CLOSE}
    };
    size_t count = sizeof(cases) / sizeof(cases[0]);
    if (argc == 2 && strcmp(argv[1], "--baseline") == 0) count = 3;
    else if (argc != 1) return 2;
    for (size_t i = 0; i < count; ++i) run_case(&cases[i], false);
    for (size_t i = 0; i < count; ++i) {
        printf("manual: ");
        run_case(&cases[i], true);
    }
    printf("retained FETCH event credit: %zu cases, %u checks, %u failures, "
           "%u prerequisite failures\n", 2 * count, checks, failures, setup_failures);
    return failures ? 1 : 0;
}
