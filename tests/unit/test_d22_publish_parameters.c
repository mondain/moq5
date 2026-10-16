/* Independent literal oracles for the initial D22 PUBLISH contract. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned cases;

typedef struct old_publish_cfg {
    uint32_t struct_size;
    moq_namespace_t track_namespace;
    moq_bytes_t track_name;
    bool has_track_alias;
    uint64_t track_alias;
    bool has_forward, forward;
    moq_bytes_t track_properties;
    const moq_auth_token_t *auth_tokens;
    size_t auth_token_count;
} old_publish_cfg_t;

typedef struct old_accept_cfg {
    uint32_t struct_size;
    bool has_subscriber_priority;
    uint8_t subscriber_priority;
    moq_group_order_t group_order;
    bool has_new_group_request;
    uint64_t new_group_request;
    moq_subscribe_filter_t filter;
    uint64_t start_group, start_object, end_group;
    bool has_forward, forward;
} old_accept_cfg_t;

_Static_assert(offsetof(moq_publish_cfg_t, expires_ms) >= sizeof(old_publish_cfg_t),
    "PUBLISH tail must preserve the complete old layout");
_Static_assert(offsetof(moq_accept_publish_cfg_t, expires_ms) >= sizeof(old_accept_cfg_t),
    "accept tail must preserve old padding");
_Static_assert(sizeof(moq_publish_initial_parameters_t) == 80, "frozen output layout");

static const moq_bytes_t namespace_parts[] = {{.data = (const uint8_t *)"n", .len = 1}};

static void set_track(moq_publish_cfg_t *cfg)
{
    cfg->track_namespace = (moq_namespace_t){namespace_parts, 1};
    cfg->track_name = MOQ_BYTES_LITERAL("t");
    cfg->has_track_alias = true;
    cfg->track_alias = 7;
}

static bool all_bytes(const void *p, size_t size, uint8_t byte)
{
    const uint8_t *bytes = p;
    for (size_t i = 0; i < size; ++i) if (bytes[i] != byte) return false;
    return true;
}

typedef struct alloc_counts {
    size_t allocs, reallocs, frees;
    int64_t balance;
} alloc_counts_t;

static void *count_alloc(size_t size, void *ctx)
{
    alloc_counts_t *counts = ctx;
    ++counts->allocs;
    void *p = malloc(size);
    if (p) ++counts->balance;
    return p;
}

static void *count_realloc(void *p, size_t old_size, size_t size, void *ctx)
{
    (void)old_size;
    alloc_counts_t *counts = ctx;
    ++counts->reallocs;
    if (!size) {
        if (p) --counts->balance;
        free(p);
        return NULL;
    }
    bool had_pointer = p != NULL;
    void *next = realloc(p, size);
    if (next && !had_pointer) ++counts->balance;
    return next;
}

static void count_free(void *p, size_t size, void *ctx)
{
    (void)size;
    alloc_counts_t *counts = ctx;
    ++counts->frees;
    if (p) --counts->balance;
    free(p);
}

typedef struct fixture {
    moq_simpair_t *pair;
    moq_session_t *client, *server;
    alloc_counts_t counts;
    moq_alloc_t alloc;
} fixture_t;

static void discard_events(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

static void fixture_close(fixture_t *fixture)
{
    moq_simpair_destroy(fixture->pair);
    fixture->pair = NULL;
    MOQ_TEST_CHECK_EQ_INT(fixture->counts.balance, 0);
}

static bool fixture_open(fixture_t *fixture, moq_version_t version)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->alloc = (moq_alloc_t){
        .ctx = &fixture->counts, .alloc = count_alloc,
        .realloc = count_realloc, .free = count_free,
    };
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &fixture->alloc;
    cfg.version = version;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    moq_result_t rc = moq_simpair_create(&cfg, &fixture->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(fixture->pair != NULL);
    if (rc != MOQ_OK || !fixture->pair) goto failed;
    fixture->client = moq_simpair_client(fixture->pair);
    fixture->server = moq_simpair_server(fixture->pair);
    MOQ_TEST_CHECK(fixture->client != NULL && fixture->server != NULL);
    if (!fixture->client || !fixture->server) goto failed;
    rc = moq_simpair_start(fixture->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto failed;
    rc = moq_simpair_run_until_quiescent(fixture->pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto failed;
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(fixture->client), version);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(fixture->server), version);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(fixture->client), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(fixture->server), MOQ_SESS_ESTABLISHED);
    if (moq_session_version(fixture->client) != version ||
        moq_session_version(fixture->server) != version ||
        moq_session_state(fixture->client) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(fixture->server) != MOQ_SESS_ESTABLISHED) goto failed;
    discard_events(fixture->client);
    discard_events(fixture->server);
    return true;
failed:
    fixture_close(fixture);
    return false;
}

/* D22 8.1, 9.8, 9.20: ID 0, namespace "n", name "t", alias 7.
 * Parameters use sorted delta types; priority/order/FORWARD are raw uint8. */
static const uint8_t default_publish[] = {
    0x1d, 0, 12, 0, 1, 1, 'n', 1, 't', 7, 2, 0x20, 128, 2, 1,
};
static const uint8_t typed_publish[] = {
    0x1d, 0, 30, 0, 1, 1, 'n', 1, 't', 7, 7,
    2, 0, 4, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    2, 0, 8, 0, 16, 7, 1, 0, 1, 2,
};
static const uint8_t paused_publish[] = {
    0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 7, 1, 0x10, 0,
};

static bool take_publish_request(moq_session_t *s, moq_publication_t *handle)
{
    size_t requests = 0;
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) {
        MOQ_TEST_CHECK(event.kind == MOQ_EVENT_PUBLISH_REQUEST);
        if (event.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            *handle = event.u.publish_request.pub;
            ++requests;
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    return requests == 1;
}

static bool feed_request(moq_session_t *s, moq_stream_ref_t ref,
                         const uint8_t *wire, size_t size, bool split,
                         moq_publication_t *handle)
{
    moq_result_t rc;
    if (split) {
        rc = moq_session_on_bidi_stream_bytes(s, ref, wire, size - 1, false, 2000);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) return false;
        moq_event_t event;
        size_t count = moq_session_poll_events(s, &event, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 0);
        if (count) moq_event_cleanup(&event);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
        if (count || moq_session_state(s) != MOQ_SESS_ESTABLISHED) return false;
    }
    rc = moq_session_on_bidi_stream_bytes(s, ref, wire + (split ? size - 1 : 0),
        split ? 1 : size, false, 2001);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return false;
    return take_publish_request(s, handle);
}

static void check_outgoing(bool typed)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init_sized(&cfg, sizeof(cfg));
    const moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("n")};
    cfg.track_namespace = (moq_namespace_t){parts, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("t");
    cfg.has_track_alias = true;
    cfg.track_alias = 7;
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    location.kind = MOQ_LOCATION_FILTER_NONE;
    if (typed) {
        cfg.has_forward = true;
        cfg.forward = false;
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 7;
        cfg.has_group_order = true;
        cfg.group_order = MOQ_GROUP_ORDER_DESCENDING;
        cfg.location = &location;
        cfg.has_object_delivery_timeout = true;
        cfg.object_delivery_timeout_ms = 0;
        cfg.has_subgroup_delivery_timeout = true;
        cfg.subgroup_delivery_timeout_ms = UINT64_MAX;
        cfg.has_expires = true;
        cfg.expires_ms = 0;
    }
    moq_publication_t handle;
    moq_result_t rc = moq_session_publish(fixture.client, &cfg, 2000, &handle);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    location.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    location.start_group = 99;
    cfg.subscriber_priority = 99;
    size_t opens = 0;
    moq_action_t action;
    while (moq_session_poll_actions(fixture.client, &action, 1)) {
        MOQ_TEST_CHECK(action.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            ++opens;
            const uint8_t *expected = typed ? typed_publish : default_publish;
            size_t size = typed ? sizeof(typed_publish) : sizeof(default_publish);
            MOQ_TEST_CHECK(!action.u.open_bidi_stream.fin);
            MOQ_TEST_CHECK_EQ_SIZE(action.u.open_bidi_stream.len, size);
            if (action.u.open_bidi_stream.len == size) {
                MOQ_TEST_CHECK(action.u.open_bidi_stream.data != NULL);
                if (action.u.open_bidi_stream.data)
                    MOQ_TEST_CHECK(memcmp(action.u.open_bidi_stream.data, expected, size) == 0);
            }
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(opens, 1);
done:
    fixture_close(&fixture);
}

static void check_literal(bool peer_server, bool split)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    moq_session_t *s = peer_server ? fixture.client : fixture.server;
    uint8_t wire[sizeof(typed_publish)];
    memcpy(wire, typed_publish, sizeof(wire));
    wire[3] = peer_server ? 1 : 0;
    moq_publication_t handle;
    if (!feed_request(s, moq_stream_ref_from_u64(0x7400), wire, sizeof(wire),
                      split, &handle)) goto done;
    memset(wire, 0, sizeof(wire));
    int slot = pub_resolve_handle(s, handle);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    const moq_pub_entry_t *entry = &s->publishes[slot];
    MOQ_TEST_CHECK_EQ_INT(entry->subscriber_priority, 7);
    MOQ_TEST_CHECK_EQ_INT(entry->group_order, MOQ_GROUP_ORDER_DESCENDING);
    MOQ_TEST_CHECK(!entry->send_allowed);
    MOQ_TEST_CHECK(entry->dt_sub_has_object);
    MOQ_TEST_CHECK_EQ_U64(entry->dt_sub_object_ms, 0);
    MOQ_TEST_CHECK(entry->dt_sub_has_subgroup);
    MOQ_TEST_CHECK_EQ_U64(entry->dt_sub_subgroup_ms, UINT64_MAX);
done:
    fixture_close(&fixture);
}

typedef struct effects {
    moq_session_t session;
    uint8_t *profile, *publications, *auth;
    size_t profile_size, publications_size, auth_size;
    alloc_counts_t counts;
} effects_t;

static bool effects_save(effects_t *saved, moq_session_t *s,
                         const alloc_counts_t *counts)
{
    memset(saved, 0, sizeof(*saved));
    saved->session = *s;
    saved->counts = *counts;
    saved->profile_size = s->profile->state_size;
    saved->publications_size = s->pub_cap * sizeof(*s->publishes);
    saved->auth_size = s->peer_token_cache.cap * sizeof(*s->peer_token_cache.entries);
    saved->profile = malloc(saved->profile_size);
    saved->publications = malloc(saved->publications_size);
    saved->auth = saved->auth_size ? malloc(saved->auth_size) : NULL;
    MOQ_TEST_CHECK(saved->profile != NULL && saved->publications != NULL &&
                   (!saved->auth_size || saved->auth != NULL));
    if (!saved->profile || !saved->publications || (saved->auth_size && !saved->auth)) {
        free(saved->profile);
        free(saved->publications);
        free(saved->auth);
        return false;
    }
    memcpy(saved->profile, s->profile_state, saved->profile_size);
    memcpy(saved->publications, s->publishes, saved->publications_size);
    if (saved->auth_size) memcpy(saved->auth, s->peer_token_cache.entries, saved->auth_size);
    return true;
}

static void effects_check_free(effects_t *saved, moq_session_t *s,
                               const alloc_counts_t *counts)
{
    MOQ_TEST_CHECK(memcmp(&saved->session, s, sizeof(*s)) == 0);
    MOQ_TEST_CHECK(memcmp(saved->profile, s->profile_state, saved->profile_size) == 0);
    MOQ_TEST_CHECK(memcmp(saved->publications, s->publishes, saved->publications_size) == 0);
    MOQ_TEST_CHECK(memcmp(&saved->counts, counts, sizeof(*counts)) == 0);
    if (saved->auth_size)
        MOQ_TEST_CHECK(memcmp(saved->auth, s->peer_token_cache.entries, saved->auth_size) == 0);
    free(saved->profile);
    free(saved->publications);
    free(saved->auth);
}

static void check_accept_refusal(unsigned choice)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    moq_publication_t handle;
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400),
        paused_publish, sizeof(paused_publish), false, &handle)) goto done;
    moq_accept_publish_cfg_t cfg;
    moq_accept_publish_cfg_init_sized(&cfg, sizeof(cfg));
    switch (choice) {
    case 0: cfg.has_subscriber_priority = true; cfg.subscriber_priority = 128; break;
    case 1: cfg.group_order = MOQ_GROUP_ORDER_ASCENDING; break;
    case 2: cfg.has_forward = true; cfg.forward = false; break;
    case 3: cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START; cfg.start_group = 1; break;
    case 4: cfg.has_new_group_request = true; cfg.new_group_request = 0; break;
    default: MOQ_TEST_CHECK(choice < 5); goto done;
    }
    effects_t saved;
    if (!effects_save(&saved, fixture.server, &fixture.counts)) goto done;
    moq_result_t rc = moq_session_accept_publish(fixture.server, handle, &cfg, 999999);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_INVAL);
    effects_check_free(&saved, fixture.server, &fixture.counts);
done:
    fixture_close(&fixture);
}

static void check_expiry_ok(unsigned expiry)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    const moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7400);
    moq_publication_t handle;
    if (!feed_request(fixture.server, ref, paused_publish, sizeof(paused_publish),
                      false, &handle)) goto done;
    moq_accept_publish_cfg_t cfg;
    moq_accept_publish_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.has_expires = expiry != 0;
    cfg.expires_ms = expiry == 2 ? UINT64_MAX : 0;
    moq_result_t rc = moq_session_accept_publish(fixture.server, handle, &cfg, 2002);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    int slot = pub_resolve_handle(fixture.server, handle);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    MOQ_TEST_CHECK(!fixture.server->publishes[slot].send_allowed);
    const uint8_t absent[] = {7, 0, 1, 0};
    const uint8_t zero[] = {7, 0, 3, 1, 8, 0};
    const uint8_t maximum[] = {7, 0, 11, 1, 8,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t *expected = expiry == 2 ? maximum : expiry == 1 ? zero : absent;
    size_t size = expiry == 2 ? sizeof(maximum) : expiry == 1 ? sizeof(zero) : sizeof(absent);
    size_t sends = 0;
    moq_action_t action;
    while (moq_session_poll_actions(fixture.server, &action, 1)) {
        MOQ_TEST_CHECK(action.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            ++sends;
            MOQ_TEST_CHECK_EQ_U64(action.u.send_bidi_stream.stream_ref._v, ref._v);
            MOQ_TEST_CHECK(!action.u.send_bidi_stream.fin);
            MOQ_TEST_CHECK_EQ_SIZE(action.u.send_bidi_stream.len, size);
            if (action.u.send_bidi_stream.len == size) {
                MOQ_TEST_CHECK(action.u.send_bidi_stream.data != NULL);
                if (action.u.send_bidi_stream.data)
                    MOQ_TEST_CHECK(memcmp(action.u.send_bidi_stream.data, expected, size) == 0);
            }
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(sends, 1);
done:
    fixture_close(&fixture);
}

static void discard_actions(moq_session_t *s)
{
    moq_action_t action;
    while (moq_session_poll_actions(s, &action, 1)) moq_action_cleanup(&action);
}

static const uint8_t omitted_publish[] = {0x1d, 0, 8, 0, 1, 1, 'n', 1, 't', 7, 0};
static const uint8_t none_publish[] = {0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 7, 1, 0x21, 0};
static const uint8_t absolute_publish[] = {
    0x1d, 0, 12, 0, 1, 1, 'n', 1, 't', 7, 1, 0x21, 2, 3, 4,
};

static void check_observation(unsigned kind, bool split)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    const uint8_t *source = kind == 0 ? omitted_publish : kind == 1 ? none_publish :
                            kind == 2 ? absolute_publish : typed_publish;
    size_t size = kind == 0 ? sizeof(omitted_publish) : kind == 1 ? sizeof(none_publish) :
                  kind == 2 ? sizeof(absolute_publish) : sizeof(typed_publish);
    uint8_t wire[sizeof(typed_publish)];
    memcpy(wire, source, size);
    moq_publication_t handle;
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7400);
    if (!feed_request(fixture.server, ref, wire, size, split, &handle)) goto done;
    memset(wire, 0, sizeof(wire));
    effects_t saved;
    if (!effects_save(&saved, fixture.server, &fixture.counts)) goto done;
    struct {moq_publish_initial_parameters_t value; uint8_t canary[16];} output;
    memset(&output, 0xa5, sizeof(output));
    moq_result_t rc = moq_session_get_publish_initial_parameters(fixture.server, handle,
        &output.value, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    effects_check_free(&saved, fixture.server, &fixture.counts);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_SIZE(output.value.struct_size, 80);
    MOQ_TEST_CHECK(all_bytes(output.canary, sizeof(output.canary), 0xa5));
    MOQ_TEST_CHECK(all_bytes(output.value.reserved, sizeof(output.value.reserved), 0));
    MOQ_TEST_CHECK_EQ_INT(output.value.subscriber_priority, kind == 3 ? 7 : 128);
    MOQ_TEST_CHECK_EQ_INT(output.value.group_order,
        kind == 3 ? MOQ_GROUP_ORDER_DESCENDING : MOQ_GROUP_ORDER_ASCENDING);
    uint32_t expected_flags = kind == 3 ? 31u : kind ? MOQ_PUBLISH_INITIAL_HAS_LOCATION : 0;
    MOQ_TEST_CHECK_EQ_U64(output.value.flags, expected_flags);
    if (kind) {
        MOQ_TEST_CHECK_EQ_INT(output.value.location.struct_size, MOQ_LOCATION_FILTER_CFG_V0_SIZE);
        MOQ_TEST_CHECK_EQ_INT(output.value.location.kind,
            kind == 2 ? MOQ_LOCATION_FILTER_ABSOLUTE_START : MOQ_LOCATION_FILTER_NONE);
        MOQ_TEST_CHECK_EQ_U64(output.value.location.start_group, kind == 2 ? 3 : 0);
        MOQ_TEST_CHECK_EQ_U64(output.value.location.start_object, kind == 2 ? 4 : 0);
    } else MOQ_TEST_CHECK(all_bytes(&output.value.location, sizeof(output.value.location), 0));
    MOQ_TEST_CHECK_EQ_U64(output.value.object_delivery_timeout_ms, 0);
    MOQ_TEST_CHECK_EQ_U64(output.value.subgroup_delivery_timeout_ms, kind == 3 ? UINT64_MAX : 0);
    moq_publish_initial_parameters_t initial = output.value;
    moq_accept_publish_cfg_t accept;
    moq_accept_publish_cfg_init(&accept);
    rc = moq_session_accept_publish(fixture.server, handle, &accept, 2002);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    discard_actions(fixture.server);
    moq_publication_update_cfg_t update;
    moq_publication_update_cfg_init(&update);
    update.has_subscriber_priority = true; update.subscriber_priority = 9;
    update.delivery_timeout_flags = MOQ_SUBSCRIPTION_UPDATE_OBJECT_DELIVERY_TIMEOUT |
                                    MOQ_SUBSCRIPTION_UPDATE_SUBGROUP_DELIVERY_TIMEOUT;
    update.object_delivery_timeout_ms = 1;
    update.subgroup_delivery_timeout_ms = 1;
    rc = moq_session_update_publication(fixture.server, handle, &update, 2003);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_action_t action;
    size_t updates = 0;
    while (moq_session_poll_actions(fixture.server, &action, 1)) {
        MOQ_TEST_CHECK(action.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            ++updates;
            MOQ_TEST_CHECK_EQ_U64(action.u.send_bidi_stream.stream_ref._v, ref._v);
            MOQ_TEST_CHECK(action.u.send_bidi_stream.len > 3);
            if (action.u.send_bidi_stream.len > 3 && action.u.send_bidi_stream.data)
                MOQ_TEST_CHECK_EQ_INT(action.u.send_bidi_stream.data[0], 2);
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(updates, 1);
    if (updates != 1) goto done;
    /* Actual update ACK on the same publication bidi; it replaces both current timeouts. */
    const uint8_t ack[] = {7, 0, 1, 0};
    rc = moq_session_on_bidi_stream_bytes(fixture.server, ref, ack, sizeof(ack), false, 2004);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    int slot = pub_resolve_handle(fixture.server, handle);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    MOQ_TEST_CHECK_EQ_U64(fixture.server->publishes[slot].dt_sub_object_ms, 1);
    MOQ_TEST_CHECK_EQ_U64(fixture.server->publishes[slot].dt_sub_subgroup_ms, 1);
    MOQ_TEST_CHECK(!fixture.server->publishes[slot].update_pending);
    if (!effects_save(&saved, fixture.server, &fixture.counts)) goto done;
    rc = moq_session_get_publish_initial_parameters(fixture.server, handle,
        &output.value, MOQ_PUBLISH_INITIAL_PARAMETERS_V0_SIZE);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    effects_check_free(&saved, fixture.server, &fixture.counts);
    if (rc == MOQ_OK) MOQ_TEST_CHECK(memcmp(&initial, &output.value, sizeof(initial)) == 0);
done:
    fixture_close(&fixture);
}

static void check_query_refusal(unsigned kind)
{
    ++cases;
    fixture_t fixture;
    moq_version_t version = kind == 6 ? MOQ_VERSION_DRAFT_16 :
                            kind == 7 ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_22;
    if (!fixture_open(&fixture, version)) return;
    moq_publication_t handle = MOQ_PUBLICATION_INVALID;
    if (kind == 4) {
        moq_publish_cfg_t cfg;
        moq_publish_cfg_init(&cfg); set_track(&cfg);
        moq_result_t rc = moq_session_publish(fixture.server, &cfg, 2000, &handle);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) goto done;
    } else if (kind < 6) {
        if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400), omitted_publish,
                          sizeof(omitted_publish), false, &handle)) goto done;
        if (kind == 3) {
            moq_reject_publish_cfg_t reject;
            moq_reject_publish_cfg_init(&reject); reject.error_code = 1;
            moq_result_t rc = moq_session_reject_publish(fixture.server, handle, &reject, 2002);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc != MOQ_OK) goto done;
        } else if (kind == 5) {
            /* Pending terminal waits for one genuine data stream: logical termination. */
            const uint8_t terminal[] = {0x0b, 0, 3, 0, 1, 0};
            moq_result_t rc = moq_session_on_bidi_stream_bytes(fixture.server,
                moq_stream_ref_from_u64(0x7400), terminal, sizeof(terminal), false, 2002);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc != MOQ_OK) goto done;
        }
    }
    moq_publish_initial_parameters_t output;
    memset(&output, 0xa5, sizeof(output));
    effects_t saved;
    if (!effects_save(&saved, fixture.server, &fixture.counts)) goto done;
    moq_result_t rc = moq_session_get_publish_initial_parameters(kind == 0 ? NULL : fixture.server,
        handle, kind == 1 ? NULL : &output, kind == 2 ? 79 : sizeof(output));
    moq_result_t expected = kind < 3 ? MOQ_ERR_INVAL : kind == 3 ? MOQ_ERR_STALE_HANDLE :
                            kind < 6 ? MOQ_ERR_WRONG_STATE : MOQ_ERR_UNSUPPORTED;
    MOQ_TEST_CHECK_EQ_INT(rc, expected);
    MOQ_TEST_CHECK(all_bytes(&output, sizeof(output), 0xa5));
    effects_check_free(&saved, fixture.server, &fixture.counts);
done:
    fixture_close(&fixture);
}

static void check_input_refusal(moq_version_t version, unsigned field)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, version)) return;
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init_sized(&cfg, sizeof(cfg)); set_track(&cfg);
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location);
    switch (field) {
    case 0: cfg.has_expires = true; cfg.expires_ms = UINT64_MAX; break;
    case 1: cfg.has_subscriber_priority = true; cfg.subscriber_priority = 128; break;
    case 2: cfg.has_group_order = true; cfg.group_order = MOQ_GROUP_ORDER_DEFAULT; break;
    case 3: cfg.location = &location; location.kind = (moq_location_filter_kind_t)99; break;
    case 4: cfg.has_object_delivery_timeout = true; cfg.object_delivery_timeout_ms = 0; break;
    case 5: cfg.has_subgroup_delivery_timeout = true; cfg.subgroup_delivery_timeout_ms = UINT64_MAX; break;
    case 6: cfg.location = &location; location.struct_size = 47; break;
    default: MOQ_TEST_CHECK(field < 7); goto done;
    }
    moq_publication_t output = {UINT64_C(0xabcdef)};
    effects_t saved;
    if (!effects_save(&saved, fixture.client, &fixture.counts)) goto done;
    moq_result_t rc = moq_session_publish(fixture.client, &cfg, 999999, &output);
    moq_result_t expected = version != MOQ_VERSION_DRAFT_22 ? MOQ_ERR_UNSUPPORTED :
                            field == 6 ? MOQ_ERR_ABI_MISMATCH : MOQ_ERR_INVAL;
    MOQ_TEST_CHECK_EQ_INT(rc, expected);
    MOQ_TEST_CHECK_EQ_U64(output._opaque, UINT64_C(0xabcdef));
    effects_check_free(&saved, fixture.client, &fixture.counts);
done:
    fixture_close(&fixture);
}

static void check_input_extent(unsigned row)
{
    ++cases;
    const size_t sizes[] = {MOQ_PUBLISH_CFG_V0_SIZE, sizeof(old_publish_cfg_t),
        offsetof(moq_publish_cfg_t, has_expires), offsetof(moq_publish_cfg_t, subscriber_priority),
        offsetof(moq_publish_cfg_t, group_order) + sizeof(moq_group_order_t) - 1,
        offsetof(moq_publish_cfg_t, location) + sizeof(void *) - 1,
        offsetof(moq_publish_cfg_t, object_delivery_timeout_ms) + 7,
        offsetof(moq_publish_cfg_t, subgroup_delivery_timeout_ms) + 7,
        sizeof(moq_publish_cfg_t) + 16};
    size_t size = sizes[row];
    uint8_t *storage = malloc(size + 16);
    MOQ_TEST_CHECK(storage != NULL);
    if (!storage) return;
    memset(storage, 0xa5, size + 16);
    moq_publish_cfg_t *cfg = (moq_publish_cfg_t *)storage;
    /* The sized initializer is applied to the caller's block itself: it must
     * clear and stamp exactly the covered bytes and nothing past them. */
    moq_publish_cfg_init_sized(cfg, size);
    const size_t covered = size < sizeof(moq_publish_cfg_t) ? size : sizeof(moq_publish_cfg_t);
    uint32_t stamped;
    memcpy(&stamped, storage + offsetof(moq_publish_cfg_t, struct_size), sizeof(stamped));
    MOQ_TEST_CHECK_EQ_SIZE(stamped, covered);
    /* The fields are set on a full, properly typed local template that starts
     * from the initialized bytes, and only the covered bytes are copied back
     * into the block, so no member is accessed through a struct lvalue larger
     * than the block. Every field set here lies inside that row's size. */
    moq_publish_cfg_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    memcpy(&tmpl, storage, covered);
    set_track(&tmpl);
    if (row == 3) tmpl.has_subscriber_priority = true;
    if (row == 4) tmpl.has_group_order = true;
    if (row == 6) tmpl.has_object_delivery_timeout = true;
    if (row == 7) tmpl.has_subgroup_delivery_timeout = true;
    if (row == 8) {
        tmpl.expires_ms = UINT64_MAX;
        tmpl.group_order = (moq_group_order_t)99;
        tmpl.object_delivery_timeout_ms = UINT64_MAX;
        tmpl.subgroup_delivery_timeout_ms = UINT64_MAX;
    }
    memcpy(storage, &tmpl, covered);
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) goto free_storage;
    moq_publication_t handle;
    moq_result_t rc = moq_session_publish(fixture.client, cfg, 2000, &handle);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        moq_action_t action;
        size_t count = 0;
        while (moq_session_poll_actions(fixture.client, &action, 1)) {
            ++count;
            MOQ_TEST_CHECK(action.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
            if (action.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
                MOQ_TEST_CHECK_EQ_SIZE(action.u.open_bidi_stream.len, sizeof(default_publish));
                if (action.u.open_bidi_stream.len == sizeof(default_publish) && action.u.open_bidi_stream.data)
                    MOQ_TEST_CHECK(memcmp(action.u.open_bidi_stream.data, default_publish, sizeof(default_publish)) == 0);
            }
            moq_action_cleanup(&action);
        }
        MOQ_TEST_CHECK_EQ_SIZE(count, 1);
    }
    MOQ_TEST_CHECK(all_bytes(storage + size, 16, 0xa5));
    if (row == 8) MOQ_TEST_CHECK(all_bytes(storage + covered, size - covered, 0xa5));
    fixture_close(&fixture);
free_storage:
    free(storage);
}

static void check_accept_extent(unsigned row)
{
    ++cases;
    size_t size = row == 0 ? MOQ_ACCEPT_PUBLISH_CFG_V0_SIZE : row == 1 ? sizeof(old_accept_cfg_t) :
                  row == 2 ? offsetof(moq_accept_publish_cfg_t, has_expires) : sizeof(moq_accept_publish_cfg_t) + 16;
    uint8_t *storage = malloc(size + 16);
    MOQ_TEST_CHECK(storage != NULL);
    if (!storage) return;
    memset(storage, 0xa5, size + 16);
    moq_accept_publish_cfg_t *cfg = (moq_accept_publish_cfg_t *)storage;
    moq_accept_publish_cfg_init_sized(cfg, size);
    const size_t covered = size < sizeof(moq_accept_publish_cfg_t) ? size : sizeof(moq_accept_publish_cfg_t);
    if (row == 1) memset(storage + offsetof(old_accept_cfg_t, forward) + sizeof(bool), 0xa5,
                         sizeof(old_accept_cfg_t) - offsetof(old_accept_cfg_t, forward) - sizeof(bool));
    if (row > 1) {
        /* Inactive or incomplete pair. Set on a full typed template from the
         * initialized bytes; only the covered bytes, which include
         * expires_ms on both rows, are copied back into the block. */
        moq_accept_publish_cfg_t tmpl;
        memset(&tmpl, 0, sizeof(tmpl));
        memcpy(&tmpl, storage, covered);
        tmpl.expires_ms = UINT64_MAX;
        memcpy(storage, &tmpl, covered);
    }
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) goto free_storage;
    moq_publication_t handle;
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400), paused_publish,
                      sizeof(paused_publish), false, &handle)) goto done;
    moq_result_t rc = moq_session_accept_publish(fixture.server, handle, cfg, 2002);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        moq_action_t action;
        size_t count = 0;
        const uint8_t expected[] = {7, 0, 1, 0};
        while (moq_session_poll_actions(fixture.server, &action, 1)) {
            ++count;
            MOQ_TEST_CHECK(action.kind == MOQ_ACTION_SEND_BIDI_STREAM);
            if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
                MOQ_TEST_CHECK_EQ_SIZE(action.u.send_bidi_stream.len, sizeof(expected));
                if (action.u.send_bidi_stream.len == sizeof(expected) && action.u.send_bidi_stream.data)
                    MOQ_TEST_CHECK(memcmp(action.u.send_bidi_stream.data, expected, sizeof(expected)) == 0);
            }
            moq_action_cleanup(&action);
        }
        MOQ_TEST_CHECK_EQ_SIZE(count, 1);
    }
    MOQ_TEST_CHECK(all_bytes(storage + size, 16, 0xa5));
    if (row == 3) MOQ_TEST_CHECK(all_bytes(storage + covered, size - covered, 0xa5));
done:
    fixture_close(&fixture);
free_storage:
    free(storage);
}

static void check_lifecycle(bool coalesced)
{
    ++cases;
    printf("CASE: lifecycle %s\n", coalesced ? "coalesced" : "split");
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    const uint8_t done[] = {0x0b, 0, 3, 0, 0, 0};
    uint8_t wire[sizeof(typed_publish) + sizeof(done)];
    memcpy(wire, typed_publish, sizeof(typed_publish));
    memcpy(wire + sizeof(typed_publish), done, sizeof(done));
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7400);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(fixture.server, ref, wire,
        coalesced ? sizeof(wire) : sizeof(typed_publish), coalesced, 2000);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    if (!coalesced) {
        rc = moq_session_on_bidi_stream_bytes(fixture.server, ref, done, sizeof(done), true, 2001);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) goto done;
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(fixture.server), MOQ_SESS_ESTABLISHED);
    unsigned requests = 0, finished = 0;
    moq_publication_t handle = MOQ_PUBLICATION_INVALID;
    moq_event_t event;
    while (moq_session_poll_events(fixture.server, &event, 1)) {
        if (event.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            ++requests; handle = event.u.publish_request.pub;
            MOQ_TEST_CHECK_EQ_SIZE(finished, 0);
            MOQ_TEST_CHECK(event.u.publish_request.has_expires);
            MOQ_TEST_CHECK_EQ_U64(event.u.publish_request.expires_ms, 0);
        } else if (event.kind == MOQ_EVENT_PUBLISH_FINISHED) {
            ++finished;
            MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
            MOQ_TEST_CHECK_EQ_U64(event.u.publish_finished.pub._opaque, handle._opaque);
        } else {
            if (event.kind == MOQ_EVENT_SESSION_CLOSED)
                fprintf(stderr, "lifecycle %s close: %.*s\n", coalesced ? "coalesced" : "split",
                    (int)event.u.closed.reason.len, (const char *)event.u.closed.reason.data);
            MOQ_TEST_CHECK(false);
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    MOQ_TEST_CHECK_EQ_SIZE(finished, 1);
    moq_publish_initial_parameters_t output;
    memset(&output, 0xa5, sizeof(output));
    rc = moq_session_get_publish_initial_parameters(fixture.server, handle, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_STALE_HANDLE);
    MOQ_TEST_CHECK(all_bytes(&output, sizeof(output), 0xa5));
done:
    fixture_close(&fixture);
}

static void check_malformed(bool truncated)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    uint8_t wire[sizeof(typed_publish)];
    memcpy(wire, typed_publish, sizeof(wire));
    if (!truncated) wire[sizeof(wire) - 1] = 3; /* illegal GROUP_ORDER */
    moq_result_t rc = moq_session_on_bidi_stream_bytes(fixture.server,
        moq_stream_ref_from_u64(0x7400), wire, truncated ? sizeof(wire) - 1 : sizeof(wire),
        truncated, 2000);
    /* Protocol closure is surfaced as an event; transport input was consumed. */
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(fixture.server), MOQ_SESS_CLOSED);
    moq_event_t event;
    while (moq_session_poll_events(fixture.server, &event, 1)) {
        MOQ_TEST_CHECK(event.kind != MOQ_EVENT_PUBLISH_REQUEST);
        moq_event_cleanup(&event);
    }
    for (size_t i = 0; i < fixture.server->pub_cap; ++i)
        MOQ_TEST_CHECK_EQ_INT(fixture.server->publishes[i].state, MOQ_PUB_FREE);
    fixture_close(&fixture);
}

static void check_preference(bool immutable)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    const uint8_t direct[] = {0x22, 2};
    const uint8_t nested[] = {0x0b, 2, 0x22, 2};
    const uint8_t *properties = immutable ? nested : direct;
    size_t properties_size = immutable ? sizeof(nested) : sizeof(direct);
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init(&cfg); set_track(&cfg);
    cfg.track_properties = (moq_bytes_t){properties, properties_size};
    moq_publication_t handle;
    moq_result_t rc = moq_session_publish(fixture.client, &cfg, 2000, &handle);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    uint8_t expected[sizeof(default_publish) + sizeof(nested)];
    memcpy(expected, default_publish, sizeof(default_publish));
    expected[2] = (uint8_t)(12 + properties_size);
    expected[sizeof(default_publish) - 1] = 2;
    memcpy(expected + sizeof(default_publish), properties, properties_size);
    moq_action_t action;
    size_t opens = 0;
    while (moq_session_poll_actions(fixture.client, &action, 1)) {
        ++opens;
        MOQ_TEST_CHECK(action.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            MOQ_TEST_CHECK_EQ_SIZE(action.u.open_bidi_stream.len, sizeof(default_publish) + properties_size);
            if (action.u.open_bidi_stream.len == sizeof(default_publish) + properties_size && action.u.open_bidi_stream.data)
                MOQ_TEST_CHECK(memcmp(action.u.open_bidi_stream.data, expected,
                    sizeof(default_publish) + properties_size) == 0);
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(opens, 1);
    uint8_t incoming[sizeof(omitted_publish) + sizeof(nested)];
    memcpy(incoming, omitted_publish, sizeof(omitted_publish));
    incoming[2] = (uint8_t)(8 + properties_size);
    memcpy(incoming + sizeof(omitted_publish), properties, properties_size);
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400), incoming,
        sizeof(omitted_publish) + properties_size, false, &handle)) goto done;
    moq_publish_initial_parameters_t output;
    rc = moq_session_get_publish_initial_parameters(fixture.server, handle, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_INT(output.group_order, MOQ_GROUP_ORDER_DESCENDING);
    MOQ_TEST_CHECK_EQ_INT(output.subscriber_priority, 128);
    MOQ_TEST_CHECK_EQ_INT(output.flags, 0);
done:
    fixture_close(&fixture);
}

static void check_timeout_properties(void)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    uint8_t wire[sizeof(typed_publish) + 4];
    memcpy(wire, typed_publish, sizeof(typed_publish));
    wire[2] += 4;
    const uint8_t properties[] = {2, 5, 4, 9}; /* independent property ms: object5/subgroup9 */
    memcpy(wire + sizeof(typed_publish), properties, sizeof(properties));
    moq_publication_t handle;
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400), wire, sizeof(wire), false, &handle)) goto done;
    int slot = pub_resolve_handle(fixture.server, handle);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) goto done;
    const moq_pub_entry_t *entry = &fixture.server->publishes[slot];
    MOQ_TEST_CHECK(entry->dt_pub_has_object && entry->dt_pub_has_subgroup);
    MOQ_TEST_CHECK_EQ_U64(entry->dt_pub_object_ms, 5);
    MOQ_TEST_CHECK_EQ_U64(entry->dt_pub_subgroup_ms, 9);
    moq_publish_initial_parameters_t output;
    moq_result_t rc = moq_session_get_publish_initial_parameters(fixture.server, handle, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(output.object_delivery_timeout_ms, 0);
        MOQ_TEST_CHECK_EQ_U64(output.subgroup_delivery_timeout_ms, UINT64_MAX);
    }
done:
    fixture_close(&fixture);
}

static void check_initializer(bool accept, bool undersized)
{
    ++cases;
    size_t size = undersized ? 3 : accept ? MOQ_ACCEPT_PUBLISH_CFG_V0_SIZE : MOQ_PUBLISH_CFG_V0_SIZE;
    uint8_t *storage = malloc(size + 16);
    MOQ_TEST_CHECK(storage != NULL);
    if (!storage) return;
    memset(storage, 0xa5, size + 16);
    if (accept) {
        if (undersized) moq_accept_publish_cfg_init_sized((moq_accept_publish_cfg_t *)storage, size);
        else moq_accept_publish_cfg_init((moq_accept_publish_cfg_t *)storage);
    } else {
        if (undersized) moq_publish_cfg_init_sized((moq_publish_cfg_t *)storage, size);
        else moq_publish_cfg_init((moq_publish_cfg_t *)storage);
    }
    if (undersized) MOQ_TEST_CHECK(all_bytes(storage, size, 0xa5));
    else {
        uint32_t stamp;
        memcpy(&stamp, storage, sizeof(stamp));
        MOQ_TEST_CHECK_EQ_SIZE(stamp, size);
        MOQ_TEST_CHECK(all_bytes(storage + sizeof(stamp), size - sizeof(stamp), 0));
    }
    MOQ_TEST_CHECK(all_bytes(storage + size, 16, 0xa5));
    free(storage);
}

static void check_legacy_accept_expiry(moq_version_t version)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, version)) return;
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init(&cfg); set_track(&cfg);
    moq_publication_t local, peer;
    moq_result_t rc = moq_session_publish(fixture.client, &cfg, 2000, &local);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(fixture.pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK || !take_publish_request(fixture.server, &peer)) goto done;
    moq_accept_publish_cfg_t accept;
    moq_accept_publish_cfg_init_sized(&accept, sizeof(accept));
    accept.has_expires = true; accept.expires_ms = 0;
    effects_t saved;
    if (!effects_save(&saved, fixture.server, &fixture.counts)) goto done;
    rc = moq_session_accept_publish(fixture.server, peer, &accept, 999999);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_UNSUPPORTED);
    effects_check_free(&saved, fixture.server, &fixture.counts);
    moq_publish_initial_parameters_t output;
    memset(&output, 0xa5, sizeof(output));
    rc = moq_session_get_publish_initial_parameters(fixture.server, peer, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_UNSUPPORTED);
    MOQ_TEST_CHECK(all_bytes(&output, sizeof(output), 0xa5));
done:
    fixture_close(&fixture);
}

static void check_reuse(void)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    moq_publication_t old, next;
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7400), typed_publish,
                      sizeof(typed_publish), false, &old)) goto done;
    int old_slot = pub_resolve_handle(fixture.server, old);
    MOQ_TEST_CHECK(old_slot >= 0);
    if (old_slot < 0) goto done;
    moq_reject_publish_cfg_t reject;
    moq_reject_publish_cfg_init(&reject); reject.error_code = 1;
    moq_result_t rc = moq_session_reject_publish(fixture.server, old, &reject, 2002);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    discard_actions(fixture.server);
    uint8_t wire[sizeof(omitted_publish)];
    memcpy(wire, omitted_publish, sizeof(wire)); wire[3] = 2;
    if (!feed_request(fixture.server, moq_stream_ref_from_u64(0x7402), wire, sizeof(wire), false, &next)) goto done;
    int new_slot = pub_resolve_handle(fixture.server, next);
    MOQ_TEST_CHECK_EQ_INT(new_slot, old_slot);
    MOQ_TEST_CHECK(old._opaque != next._opaque);
    moq_publish_initial_parameters_t output;
    memset(&output, 0xa5, sizeof(output));
    rc = moq_session_get_publish_initial_parameters(fixture.server, old, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_STALE_HANDLE);
    MOQ_TEST_CHECK(all_bytes(&output, sizeof(output), 0xa5));
    rc = moq_session_get_publish_initial_parameters(fixture.server, next, &output, sizeof(output));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_INT(output.flags, 0);
        MOQ_TEST_CHECK_EQ_INT(output.subscriber_priority, 128);
        MOQ_TEST_CHECK_EQ_INT(output.group_order, MOQ_GROUP_ORDER_ASCENDING);
        MOQ_TEST_CHECK(all_bytes(&output.location, sizeof(output.location), 0));
        MOQ_TEST_CHECK_EQ_U64(output.object_delivery_timeout_ms, 0);
        MOQ_TEST_CHECK_EQ_U64(output.subgroup_delivery_timeout_ms, 0);
    }
done:
    fixture_close(&fixture);
}

static void check_public_pair(bool maximum)
{
    ++cases;
    fixture_t fixture;
    if (!fixture_open(&fixture, MOQ_VERSION_DRAFT_22)) return;
    moq_publish_cfg_t cfg;
    moq_publish_cfg_init_sized(&cfg, sizeof(cfg)); set_track(&cfg);
    moq_location_filter_cfg_t location;
    moq_location_filter_cfg_init(&location); location.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    location.start_group = 3; location.start_object = 4;
    cfg.location = &location;
    cfg.has_forward = true; cfg.forward = false;
    cfg.has_subscriber_priority = true; cfg.subscriber_priority = 7;
    cfg.has_group_order = true; cfg.group_order = MOQ_GROUP_ORDER_DESCENDING;
    cfg.has_expires = true; cfg.expires_ms = maximum ? UINT64_MAX : 0;
    cfg.has_object_delivery_timeout = true; cfg.object_delivery_timeout_ms = maximum ? UINT64_MAX : 0;
    cfg.has_subgroup_delivery_timeout = true; cfg.subgroup_delivery_timeout_ms = maximum ? 0 : UINT64_MAX;
    moq_publication_t local, peer = MOQ_PUBLICATION_INVALID;
    moq_result_t rc = moq_session_publish(fixture.client, &cfg, 2000, &local);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    location.start_group = 99; cfg.subscriber_priority = 99;
    rc = moq_simpair_run_until_quiescent(fixture.pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_event_t event;
    size_t requests = 0;
    while (moq_session_poll_events(fixture.server, &event, 1)) {
        MOQ_TEST_CHECK(event.kind == MOQ_EVENT_PUBLISH_REQUEST);
        if (event.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            ++requests; peer = event.u.publish_request.pub;
            MOQ_TEST_CHECK(event.u.publish_request.has_expires);
            MOQ_TEST_CHECK_EQ_U64(event.u.publish_request.expires_ms, maximum ? UINT64_MAX : 0);
            MOQ_TEST_CHECK(!event.u.publish_request.forward);
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    if (requests != 1) goto done;
    moq_publish_initial_parameters_t pending, established;
    rc = moq_session_get_publish_initial_parameters(fixture.server, peer, &pending, sizeof(pending));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_INT(pending.flags, 31);
    MOQ_TEST_CHECK_EQ_U64(pending.location.start_group, 3);
    MOQ_TEST_CHECK_EQ_U64(pending.location.start_object, 4);
    MOQ_TEST_CHECK_EQ_U64(pending.object_delivery_timeout_ms, maximum ? UINT64_MAX : 0);
    MOQ_TEST_CHECK_EQ_U64(pending.subgroup_delivery_timeout_ms, maximum ? 0 : UINT64_MAX);
    moq_accept_publish_cfg_t accept;
    moq_accept_publish_cfg_init_sized(&accept, sizeof(accept));
    accept.has_expires = true; accept.expires_ms = maximum ? 0 : UINT64_MAX;
    rc = moq_session_accept_publish(fixture.server, peer, &accept, 2001);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(fixture.pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    size_t responses = 0;
    while (moq_session_poll_events(fixture.client, &event, 1)) {
        MOQ_TEST_CHECK(event.kind == MOQ_EVENT_PUBLISH_OK);
        if (event.kind == MOQ_EVENT_PUBLISH_OK) {
            ++responses;
            MOQ_TEST_CHECK_EQ_U64(event.u.publish_ok.pub._opaque, local._opaque);
            MOQ_TEST_CHECK(!event.u.publish_ok.send_allowed);
            MOQ_TEST_CHECK_EQ_INT(event.u.publish_ok.subscriber_priority, 7);
            MOQ_TEST_CHECK_EQ_INT(event.u.publish_ok.group_order, MOQ_GROUP_ORDER_DESCENDING);
            MOQ_TEST_CHECK(event.u.publish_ok.has_expires);
            MOQ_TEST_CHECK_EQ_U64(event.u.publish_ok.expires_ms, maximum ? 0 : UINT64_MAX);
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(responses, 1);
    rc = moq_session_get_publish_initial_parameters(fixture.server, peer, &established, sizeof(established));
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) MOQ_TEST_CHECK(memcmp(&pending, &established, sizeof(pending)) == 0);
    int slot = pub_resolve_handle(fixture.client, local);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot >= 0) {
        MOQ_TEST_CHECK_EQ_U64(fixture.client->publishes[slot].dt_sub_object_ms, maximum ? UINT64_MAX : 0);
        MOQ_TEST_CHECK_EQ_U64(fixture.client->publishes[slot].dt_sub_subgroup_ms, maximum ? 0 : UINT64_MAX);
    }
done:
    fixture_close(&fixture);
}

int main(int argc, char **argv)
{
    bool parameters_only = argc == 2 && strcmp(argv[1], "parameters") == 0;
    if (argc > 2 || (argc == 2 && !parameters_only)) return 2;
    check_outgoing(false);
    check_outgoing(true);
    for (unsigned peer_server = 0; peer_server < 2; ++peer_server)
        for (unsigned split = 0; split < 2; ++split)
            check_literal(peer_server != 0, split != 0);
    for (unsigned choice = 0; choice < 5; ++choice) check_accept_refusal(choice);
    for (unsigned expiry = 0; expiry < 3; ++expiry) check_expiry_ok(expiry);
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned split = 0; split < 2; ++split) check_observation(kind, split != 0);
    for (unsigned kind = 0; kind < 8; ++kind) check_query_refusal(kind);
    for (unsigned field = 0; field < 6; ++field) {
        check_input_refusal(MOQ_VERSION_DRAFT_16, field);
        check_input_refusal(MOQ_VERSION_DRAFT_18, field);
    }
    check_input_refusal(MOQ_VERSION_DRAFT_22, 2);
    check_input_refusal(MOQ_VERSION_DRAFT_22, 3);
    check_input_refusal(MOQ_VERSION_DRAFT_22, 6);
    for (unsigned row = 0; row < 9; ++row) check_input_extent(row);
    for (unsigned row = 0; row < 4; ++row) check_accept_extent(row);
    if (!parameters_only) { check_lifecycle(false); check_lifecycle(true); }
    check_malformed(false); check_malformed(true);
    check_preference(false); check_preference(true);
    check_timeout_properties();
    for (unsigned accept = 0; accept < 2; ++accept)
        for (unsigned undersized = 0; undersized < 2; ++undersized)
            check_initializer(accept != 0, undersized != 0);
    check_legacy_accept_expiry(MOQ_VERSION_DRAFT_16);
    check_legacy_accept_expiry(MOQ_VERSION_DRAFT_18);
    check_reuse();
    check_public_pair(false); check_public_pair(true);
    if (failures) {
        fprintf(stderr, "FAIL: test_d22_publish_parameters (%d checks, %u cases)\n", failures, cases);
        return 1;
    }
    printf("PASS: test_d22_publish_parameters (%u cases)\n", cases);
    return 0;
}
