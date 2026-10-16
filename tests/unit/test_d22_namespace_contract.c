#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

static void drain_events(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

static int tracks_options(bool selected, bool include, moq_group_order_t order)
{
    int failures = 0;
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = moq_alloc_default();
    pc.version = MOQ_VERSION_DRAFT_22;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&pc, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return failures;
    rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *client = moq_simpair_client(pair);
    moq_session_t *server = moq_simpair_server(pair);
    drain_events(client);
    drain_events(server);
    moq_bytes_t part = MOQ_BYTES_LITERAL("n");
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){&part, 1};
    cfg.options_flags = selected ? MOQ_SUBSCRIBE_TRACKS_GROUP_ORDER | MOQ_SUBSCRIBE_TRACKS_INCLUDE_PROPERTIES : 0;
    cfg.group_order = order;
    cfg.include_properties = include;
    moq_track_sub_handle_t handle = MOQ_TRACK_SUB_HANDLE_INVALID;
    rc = moq_session_subscribe_tracks(client, &cfg, 1000, &handle);
    /* A selected GROUP_ORDER travels as 1 or 2; the default is omission. */
    bool refused = selected && order == MOQ_GROUP_ORDER_DEFAULT;
    MOQ_TEST_CHECK_EQ_INT(rc, refused ? MOQ_ERR_INVAL : MOQ_OK);
    moq_action_t action;
    size_t count = moq_session_poll_actions(client, &action, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, refused ? 0 : 1);
    if (!count) goto done;
    /* Canonical one-byte type 0x51; GROUP_ORDER 0x22, then INCLUDE_PROPERTIES
     * 0x35 as the delta 0x13. */
    uint8_t wire[] = {0x51, 0, 4, 0, 1, 1, 'n', 0, 0x22, (uint8_t)order, 0x13, 0};
    size_t len = selected ? sizeof(wire) : 8;
    wire[2] = (uint8_t)(len - 3);
    wire[7] = selected ? 2 : 0;
    wire[11] = include ? 1 : 0;
    MOQ_TEST_CHECK_EQ_INT(action.kind, MOQ_ACTION_OPEN_BIDI_STREAM);
    if (action.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
        MOQ_TEST_CHECK_EQ_SIZE(action.u.open_bidi_stream.len, len);
        if (action.u.open_bidi_stream.len == len)
            MOQ_TEST_CHECK(memcmp(action.u.open_bidi_stream.data, wire, len) == 0);
    }
    rc = moq_session_on_bidi_stream_bytes(server,
        moq_stream_ref_from_u64(0x7900), wire, len, false, 1000);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    moq_event_t event;
    count = moq_session_poll_events(server, &event, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, 1);
    if (count) {
        MOQ_TEST_CHECK_EQ_INT(event.kind, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST);
        if (event.kind == MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST) {
            const moq_subscribe_tracks_options_t *o = event.u.subscribe_tracks_request.options;
            MOQ_TEST_CHECK(o != NULL);
            if (o) {
                MOQ_TEST_CHECK_EQ_INT(o->flags, cfg.options_flags);
                MOQ_TEST_CHECK_EQ_INT(o->group_order, selected ? order : MOQ_GROUP_ORDER_DEFAULT);
                MOQ_TEST_CHECK(o->include_properties == (selected ? include : true));
            }
        }
        moq_event_cleanup(&event);
    }
    moq_action_cleanup(&action);
done:
    moq_simpair_destroy(pair);
    return failures;
}

/* An older draft carries no options: a selection is refused before output and
 * its request event has none. */
static int legacy_options(void)
{
    int failures = 0;
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = moq_alloc_default();
    pc.version = MOQ_VERSION_DRAFT_18;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&pc, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return failures;
    MOQ_TEST_CHECK_EQ_INT(moq_simpair_start(pair), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_simpair_run_until_quiescent(pair, 32, NULL), MOQ_OK);
    moq_session_t *client = moq_simpair_client(pair);
    drain_events(client);
    moq_bytes_t part = MOQ_BYTES_LITERAL("n");
    moq_subscribe_tracks_cfg_t cfg;
    moq_subscribe_tracks_cfg_init(&cfg);
    cfg.track_namespace_prefix = (moq_namespace_t){&part, 1};
    cfg.options_flags = MOQ_SUBSCRIBE_TRACKS_INCLUDE_PROPERTIES;
    moq_track_sub_handle_t handle = MOQ_TRACK_SUB_HANDLE_INVALID;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(client, &cfg, 1000, &handle),
                          MOQ_ERR_UNSUPPORTED);
    moq_action_t action;
    MOQ_TEST_CHECK_EQ_SIZE(moq_session_poll_actions(client, &action, 1), 0);
    /* The symbol initializes only the layout an older caller allocated. */
    moq_subscribe_tracks_cfg_t old;
    memset(&old, 0x5a, sizeof(old));
    (moq_subscribe_tracks_cfg_init)(&old);
    MOQ_TEST_CHECK_EQ_INT(old.struct_size,
                          offsetof(moq_subscribe_tracks_cfg_t, options_flags));
    MOQ_TEST_CHECK_EQ_INT(old.options_flags, 0x5a5a5a5au);
    cfg.options_flags = 0;
    MOQ_TEST_CHECK_EQ_INT(moq_session_subscribe_tracks(client, &old, 1000, &handle),
                          MOQ_OK);
    moq_simpair_destroy(pair);
    return failures;
}

int main(void)
{
    int failures = 0;
    for (unsigned selected = 0; selected < 2; ++selected)
        for (unsigned include = 0; include < 2; ++include)
            for (moq_group_order_t order = MOQ_GROUP_ORDER_DEFAULT; order <= MOQ_GROUP_ORDER_DESCENDING; ++order)
                failures += tracks_options(selected != 0, include != 0, order);
    failures += legacy_options();
    if (!failures) MOQ_TEST_PASS("D22 namespace contract");
    return failures ? 1 : 0;
}
