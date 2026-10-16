/* Draft22 2.4.3/6.5: first fields ".session" and "." are refused.
 * Wire requests and the exact refusal are independent of product codecs. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

typedef struct request_kind {
    const char *name;
    uint8_t type;
    bool track;
    bool alias;
    uint32_t event;
} request_kind_t;

static const request_kind_t kinds[] = {
    {"SUBSCRIBE", 0x03, true, false, MOQ_EVENT_SUBSCRIBE_REQUEST},
    {"FETCH", 0x16, true, false, MOQ_EVENT_FETCH_REQUEST},
    {"TRACK_STATUS", 0x0d, true, false, MOQ_EVENT_TRACK_STATUS_REQUEST},
    {"PUBLISH", 0x1d, true, true, MOQ_EVENT_PUBLISH_REQUEST},
    {"PUBLISH_NAMESPACE", 0x06, false, false, MOQ_EVENT_NAMESPACE_PUBLISHED},
    {"SUBSCRIBE_NAMESPACE", 0x50, false, false, MOQ_EVENT_NS_SUB_REQUEST},
    {"SUBSCRIBE_TRACKS", 0x51, false, false, MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST},
};

typedef struct namespace_case {
    const char *name;
    moq_bytes_t parts[2];
    size_t count;
    bool reserved;
} namespace_case_t;

static const namespace_case_t namespaces[] = {
    {"session", {{(const uint8_t *)".session", 8}}, 1, true},
    {"dot", {{(const uint8_t *)".", 1}}, 1, true},
    {"longer", {{(const uint8_t *)".sessions", 9}}, 1, false},
    {"embedded-nul", {{(const uint8_t *)".session\0", 9}}, 1, false},
    {"case-sensitive", {{(const uint8_t *)".Session", 8}}, 1, false},
    {"second-field", {{(const uint8_t *)"n", 1},
                      {(const uint8_t *)".session", 8}}, 2, false},
    {"session-prefix", {{(const uint8_t *)".session", 8},
                        {(const uint8_t *)"child", 5}}, 2, true},
    {"dot-prefix", {{(const uint8_t *)".", 1},
                    {(const uint8_t *)"child", 5}}, 2, true},
};

/* All fields are literal one-byte vi64 values except message types50/51.
 * The fixed64-byte span covers every enumerated request, including FETCH's
 * explicit inclusive range so its controls do not need track-history state. */
static size_t request_bytes(uint8_t out[64], const request_kind_t *kind,
    const namespace_case_t *ns, uint8_t id, bool empty_track)
{
    size_t n = 0;
    if (kind->type >= 0x40) out[n++] = 0x80;
    out[n++] = kind->type;
    out[n++] = 0;
    const size_t length_at = n++;
    const size_t body = n;
    out[n++] = id;
    out[n++] = (uint8_t)ns->count;
    for (size_t i = 0; i < ns->count; ++i) {
        out[n++] = (uint8_t)ns->parts[i].len;
        memcpy(out + n, ns->parts[i].data, ns->parts[i].len);
        n += ns->parts[i].len;
    }
    if (kind->track) {
        out[n++] = empty_track ? 0 : 1;
        if (!empty_track) out[n++] = 'v';
    }
    if (kind->alias) out[n++] = 1;
    if (kind->type == 0x16) {
        const uint8_t range[] = {1, 0x21, 4, 0, 0, 0, 0};
        memcpy(out + n, range, sizeof(range));
        n += sizeof(range);
    } else {
        out[n++] = 0; /* no parameters */
    }
    out[length_at] = (uint8_t)(n - body);
    return n;
}

static int check_request_event(const moq_event_t *event,
    const request_kind_t *kind, const namespace_case_t *ns, bool empty_track)
{
    int failures = 0;
    moq_namespace_t observed = {0};
    moq_bytes_t name = {0};
    bool valid = false;
    switch (event->kind) {
    case MOQ_EVENT_SUBSCRIBE_REQUEST:
        observed = event->u.subscribe_request.track_namespace;
        name = event->u.subscribe_request.track_name;
        valid = moq_subscription_is_valid(event->u.subscribe_request.sub);
        break;
    case MOQ_EVENT_FETCH_REQUEST:
        observed = event->u.fetch_request.track_namespace;
        name = event->u.fetch_request.track_name;
        valid = moq_fetch_is_valid(event->u.fetch_request.fetch);
        break;
    case MOQ_EVENT_TRACK_STATUS_REQUEST:
        observed = event->u.track_status_request.track_namespace;
        name = event->u.track_status_request.track_name;
        valid = moq_track_status_handle_is_valid(event->u.track_status_request.handle);
        break;
    case MOQ_EVENT_PUBLISH_REQUEST:
        observed = event->u.publish_request.track_namespace;
        name = event->u.publish_request.track_name;
        valid = moq_publication_is_valid(event->u.publish_request.pub);
        break;
    case MOQ_EVENT_NAMESPACE_PUBLISHED:
        observed = event->u.namespace_published.track_namespace;
        valid = moq_announcement_is_valid(event->u.namespace_published.ann);
        break;
    case MOQ_EVENT_NS_SUB_REQUEST:
        observed = event->u.ns_sub_request.track_namespace_prefix;
        valid = moq_ns_sub_handle_is_valid(event->u.ns_sub_request.handle);
        break;
    case MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST:
        observed = event->u.subscribe_tracks_request.track_namespace_prefix;
        valid = moq_track_sub_handle_is_valid(event->u.subscribe_tracks_request.handle);
        break;
    default:
        MOQ_TEST_CHECK(false);
        return failures;
    }
    MOQ_TEST_CHECK(valid);
    MOQ_TEST_CHECK_EQ_SIZE(observed.count, ns->count);
    MOQ_TEST_CHECK(observed.parts != NULL);
    if (observed.count == ns->count && observed.parts) {
        for (size_t i = 0; i < ns->count; ++i) {
            MOQ_TEST_CHECK_EQ_SIZE(observed.parts[i].len, ns->parts[i].len);
            MOQ_TEST_CHECK(observed.parts[i].data != NULL);
            if (observed.parts[i].len == ns->parts[i].len && observed.parts[i].data)
                MOQ_TEST_CHECK(memcmp(observed.parts[i].data, ns->parts[i].data,
                                      ns->parts[i].len) == 0);
        }
    }
    if (kind->track) {
        MOQ_TEST_CHECK_EQ_SIZE(name.len, empty_track ? 0 : 1);
        if (!empty_track) {
            MOQ_TEST_CHECK(name.data != NULL);
            if (name.data && name.len == 1) MOQ_TEST_CHECK(name.data[0] == 'v');
        }
    }
    return failures;
}

static int one_case(const request_kind_t *kind, const namespace_case_t *ns,
                    bool server_peer, bool empty_track, bool fragmented)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&cfg, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(pair != NULL);
    if (rc != MOQ_OK || !pair) goto done;
    rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *s = server_peer ? moq_simpair_client(pair) : moq_simpair_server(pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(s), MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    if (moq_session_state(s) != MOQ_SESS_ESTABLISHED) goto done;
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
    moq_action_t action;
    size_t leftovers = 0;
    while (moq_session_poll_actions(s, &action, 1)) {
        ++leftovers;
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(leftovers, 0);
    if (leftovers) goto done;

    uint8_t wire[64] = {0};
    const size_t len = request_bytes(wire, kind, ns, server_peer ? 1 : 0, empty_track);
    const moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7300);
    size_t offset = 0;
    while (offset < len) {
        const size_t take = fragmented ? 1 : len;
        rc = moq_session_on_bidi_stream_bytes(s, ref, wire + offset, take, false, 1);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) goto done;
        offset += take;
        if (offset < len) {
            size_t count = moq_session_poll_events(s, &event, 1);
            MOQ_TEST_CHECK_EQ_SIZE(count, 0);
            if (count) moq_event_cleanup(&event);
            count = moq_session_poll_actions(s, &action, 1);
            MOQ_TEST_CHECK_EQ_SIZE(count, 0);
            if (count) moq_action_cleanup(&action);
        }
    }
    memset(wire, 0, sizeof(wire));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    size_t events = 0;
    while (moq_session_poll_events(s, &event, 1)) {
        ++events;
        MOQ_TEST_CHECK_EQ_INT(event.kind, kind->event);
        if (event.kind == kind->event)
            failures += check_request_event(&event, kind, ns, empty_track);
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(events, ns->reserved ? 0 : 1);
    size_t sends = 0;
    while (moq_session_poll_actions(s, &action, 1)) {
        ++sends;
        MOQ_TEST_CHECK_EQ_INT(action.kind, MOQ_ACTION_SEND_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            const uint8_t expected[] = {0x05, 0x00, 0x03, 0x10, 0x00, 0x00};
            const moq_send_bidi_stream_action_t *send = &action.u.send_bidi_stream;
            MOQ_TEST_CHECK_EQ_U64(send->stream_ref._v, ref._v);
            MOQ_TEST_CHECK(send->fin);
            MOQ_TEST_CHECK_EQ_SIZE(send->len, sizeof(expected));
            if (send->len == sizeof(expected))
                MOQ_TEST_CHECK(memcmp(send->data, expected, sizeof(expected)) == 0);
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(sends, ns->reserved ? 1 : 0);
    if (ns->reserved) {
        rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, true, 2);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
        size_t count = moq_session_poll_events(s, &event, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 0);
        if (count) moq_event_cleanup(&event);
        count = moq_session_poll_actions(s, &action, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 0);
        if (count) moq_action_cleanup(&action);
    }
done:
    moq_simpair_destroy(pair);
    if (failures) fprintf(stderr,
        "namespace wire: kind=%s namespace=%s server_peer=%d empty_track=%d fragmented=%d failures=%d\n",
        kind->name, ns->name, server_peer, empty_track, fragmented, failures);
    return failures;
}

int main(void)
{
    int failures = 0;
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k)
        for (size_t n = 0; n < sizeof(namespaces) / sizeof(namespaces[0]); ++n)
            for (int peer = 0; peer < 2; ++peer)
                for (int fragmented = 0; fragmented < 2; ++fragmented) {
                    failures += one_case(&kinds[k], &namespaces[n], peer != 0,
                        false, fragmented != 0);
                    if (kinds[k].track && n == 0)
                        failures += one_case(&kinds[k], &namespaces[n], peer != 0,
                            true, fragmented != 0);
                }
    if (!failures) MOQ_TEST_PASS("D22 literal session namespace policy");
    return failures ? 1 : 0;
}
