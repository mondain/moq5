#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"

static int check_codec(const moq_profile_ops_t *profile)
{
    int failures = 0;
    /* D18 11.4.4.1 and D22 11.4.1.1: an absent Object Delta means
     * prior Object ID + 1 even when a Group Delta selects a new group. */
    const struct {
        uint8_t bytes[8];
        size_t len;
        uint64_t group;
        uint64_t object;
    } rows[] = {
        {{0x08, 2, 0}, 3, 7, 7},
        {{0x0c, 2, 3, 0}, 4, 7, 3},
        {{0x04, 2, 0}, 3, 4, 8},
        {{0x00, 0}, 2, 4, 7},
        {{0x08, 0, 1, 0x52}, 4, 5, 7},
    };
    moq_fetch_prior_object_t prior = {
        .has_prev = true, .has_actual = true, .group_id = 4,
        .object_id = 6, .subgroup_id = 2, .publisher_priority = 128,
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        moq_buf_reader_t reader;
        moq_buf_reader_init(&reader, rows[i].bytes, rows[i].len);
        moq_decoded_fetch_object_t object = {0};
        moq_result_t rc = profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) continue;
        MOQ_TEST_CHECK_EQ_U64(object.group_id, rows[i].group);
        MOQ_TEST_CHECK_EQ_U64(object.object_id, rows[i].object);
        MOQ_TEST_CHECK_EQ_U64(object.publisher_priority, 128);
        MOQ_TEST_CHECK_EQ_SIZE(reader.pos, rows[i].len);
        MOQ_TEST_CHECK_EQ_SIZE(object.payload_len, i == 4 ? 1 : 0);
        if (object.payload_len) MOQ_TEST_CHECK(rows[i].bytes[reader.pos - 1] == 0x52);
    }

    const uint8_t group_only[] = {0x08, 0, 0};
    const uint8_t first_missing_object[] = {0x18, 4, 128, 0};
    const uint8_t first_full[] = {0x1c, 4, 6, 128, 0};
    const uint8_t after_range[] = {0x18, 2, 128, 0};
    moq_buf_reader_t reader;
    moq_decoded_fetch_object_t object = {0};
    moq_buf_reader_init(&reader, first_missing_object, sizeof(first_missing_object));
    MOQ_TEST_CHECK_EQ_INT(profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, NULL, &object), MOQ_ERR_PROTO);
    moq_buf_reader_init(&reader, first_full, sizeof(first_full));
    moq_result_t rc = profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, NULL, &object);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(object.group_id, 4);
        MOQ_TEST_CHECK_EQ_U64(object.object_id, 6);
    }

    prior.has_actual = false;
    moq_buf_reader_init(&reader, after_range, sizeof(after_range));
    rc = profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(object.group_id, 7);
        MOQ_TEST_CHECK_EQ_U64(object.object_id, 7);
    }
    moq_buf_reader_init(&reader, group_only, sizeof(group_only));
    MOQ_TEST_CHECK_EQ_INT(profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object), MOQ_ERR_PROTO);

    prior.has_actual = true;
    prior.object_id = UINT64_MAX;
    moq_buf_reader_init(&reader, group_only, sizeof(group_only));
    MOQ_TEST_CHECK_EQ_INT(profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object), MOQ_ERR_PROTO);
    prior.object_id = 6;
    prior.group_id = UINT64_MAX;
    moq_buf_reader_init(&reader, group_only, sizeof(group_only));
    MOQ_TEST_CHECK_EQ_INT(profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object), MOQ_ERR_PROTO);
    const uint8_t full_delta[] = {0x0c, 0, 0, 0};
    moq_buf_reader_init(&reader, full_delta, sizeof(full_delta));
    MOQ_TEST_CHECK_EQ_INT(profile->decode_fetch_object(NULL, &reader, MOQ_GROUP_ORDER_ASCENDING, &prior, &object), MOQ_ERR_PROTO);
    return failures;
}

static void drain_events(moq_session_t *session)
{
    moq_event_t event;
    while (moq_session_poll_events(session, &event, 1)) moq_event_cleanup(&event);
}

static int check_session(moq_version_t version)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    moq_simpair_t *pair = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_simpair_create(&cfg, &pair), MOQ_OK);
    if (!pair) return failures;
    moq_result_t rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(client), version);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(server), version);
    MOQ_TEST_CHECK_EQ_INT(client->state, MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(server->state, MOQ_SESS_ESTABLISHED);
    if (client->state != MOQ_SESS_ESTABLISHED || server->state != MOQ_SESS_ESTABLISHED)
        goto done;
    drain_events(client);
    drain_events(server);

    moq_fetch_cfg_t fetch_cfg;
    moq_fetch_cfg_init_sized(&fetch_cfg, sizeof(fetch_cfg));
    const moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("delta")};
    fetch_cfg.track_namespace = (moq_namespace_t){parts, 1};
    fetch_cfg.track_name = MOQ_BYTES_LITERAL("track");
    fetch_cfg.start_group = 4;
    fetch_cfg.end_group = 7;
    fetch_cfg.end_object = 8;
    moq_fetch_t client_fetch = {0};
    rc = moq_session_fetch(client, &fetch_cfg, 1, &client_fetch);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_fetch_t server_fetch = {0};
    size_t requests = 0;
    moq_event_t event;
    while (moq_session_poll_events(server, &event, 1)) {
        if (event.kind == MOQ_EVENT_FETCH_REQUEST) {
            server_fetch = event.u.fetch_request.fetch;
            ++requests;
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    int slot = fetch_resolve_handle(server, server_fetch);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    MOQ_TEST_CHECK_EQ_U64(server->fetches[slot].request_id, 0);
    moq_accept_fetch_cfg_t accept;
    moq_accept_fetch_cfg_init(&accept);
    accept.end_group = 7;
    accept.end_object = 8;
    rc = moq_session_accept_fetch(server, server_fetch, &accept, 2);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    drain_events(client);
    slot = fetch_resolve_handle(client, client_fetch);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    MOQ_TEST_CHECK(client->fetches[slot].data_stream_started);
    if (!client->fetches[slot].data_stream_started) goto done;
    /* Acceptance already opened the real FETCH response stream. Inject literal
     * objects (4,6) and (7,7) there, not a second FETCH_HEADER. */
    const uint8_t wire[] = {0x1c, 4, 6, 128, 1, 0x41, 0x08, 2, 1, 0x42};
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(client,
        client->fetches[slot].data_stream_ref, wire, sizeof(wire), false, 3), MOQ_OK);
    size_t objects = 0;
    while (moq_session_poll_events(client, &event, 1)) {
        if (event.kind == MOQ_EVENT_SESSION_CLOSED)
            fprintf(stderr, "fetch delta version %u closed: %llu %.*s\n",
                (unsigned)version, (unsigned long long)event.u.closed.code,
                (int)event.u.closed.reason.len,
                event.u.closed.reason.data ? (const char *)event.u.closed.reason.data : "");
        MOQ_TEST_CHECK(event.kind != MOQ_EVENT_SESSION_CLOSED);
        if (event.kind == MOQ_EVENT_FETCH_OBJECT) {
            MOQ_TEST_CHECK(objects < 2);
            MOQ_TEST_CHECK_EQ_U64(event.u.fetch_object.group_id, objects == 0 ? 4 : 7);
            MOQ_TEST_CHECK_EQ_U64(event.u.fetch_object.object_id, objects == 0 ? 6 : 7);
            MOQ_TEST_CHECK_EQ_U64(event.u.fetch_object.fetch._opaque, client_fetch._opaque);
            MOQ_TEST_CHECK_EQ_SIZE(moq_rcbuf_len(event.u.fetch_object.payload), 1);
            if (moq_rcbuf_len(event.u.fetch_object.payload) == 1)
                MOQ_TEST_CHECK(moq_rcbuf_data(event.u.fetch_object.payload)[0] ==
                               (objects == 0 ? 0x41 : 0x42));
            ++objects;
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(objects, 2);
    MOQ_TEST_CHECK_EQ_INT(client->state, MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(pair);
    return failures;
}

int main(void)
{
    int failures = check_codec(moq_d18_profile_ops());
    failures += check_codec(moq_d22_profile_ops());
    failures += check_session(MOQ_VERSION_DRAFT_18);
    failures += check_session(MOQ_VERSION_DRAFT_22);
    fprintf(stderr, "fetch_object_delta: %d failures\n", failures);
    return failures ? 1 : 0;
}
