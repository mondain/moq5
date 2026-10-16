/* Initial SUBSCRIBE metadata: literal D22 inputs and an independent value
 * prefix. No native encoder is used to construct the D22 requests. */
#include <moq/moq.h>

typedef struct legacy_request {
    moq_subscription_t sub;
    moq_namespace_t track_namespace;
    moq_bytes_t track_name;
    moq_subscribe_filter_t filter;
    uint8_t subscriber_priority;
    moq_group_order_t group_order;
    bool forward;
    uint8_t padding[3];
    uint64_t start_group, start_object, end_group, delivery_timeout_us;
    const moq_resolved_token_t *tokens;
    size_t token_count;
    bool has_new_group_request;
    uint64_t new_group_request;
} legacy_request_t;

typedef struct admission_prefix {
    uint32_t struct_size, flags;
    moq_location_filter_cfg_t location;
    uint64_t object_delivery_timeout_ms, subgroup_delivery_timeout_ms;
} admission_prefix_t;

_Static_assert(sizeof(admission_prefix_t) == 72, "admission value prefix");
_Static_assert(offsetof(admission_prefix_t, location) == 8, "location offset");
_Static_assert(offsetof(admission_prefix_t, object_delivery_timeout_ms) == 56,
               "independent timeouts offset");
_Static_assert(offsetof(moq_subscription_admission_options_t, rendezvous_timeout_ms) ==
               sizeof(admission_prefix_t),
               "native admission prefix; later fields start past it");
_Static_assert(MOQ_SUBSCRIPTION_ADMISSION_OPTIONS_V0_SIZE == 72, "frozen floor");
_Static_assert(offsetof(moq_subscribe_request_event_t, options) == sizeof(legacy_request_t),
               "options must not reuse legacy padding");
_Static_assert(sizeof(moq_event_t) == 144, "frozen event ABI");
_Static_assert(MOQ_SUBSCRIPTION_ADMISSION_LOCATION == 1 &&
               MOQ_SUBSCRIPTION_ADMISSION_FILL == 2 &&
               MOQ_SUBSCRIPTION_ADMISSION_OBJECT_DELIVERY_TIMEOUT == 4 &&
               MOQ_SUBSCRIPTION_ADMISSION_SUBGROUP_DELIVERY_TIMEOUT == 8,
               "stable flags");

#ifndef MOQ_ADMISSION_LAYOUT_ONLY
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include <string.h>

typedef struct timeout_case {
    bool object, subgroup;
    uint64_t object_ms, subgroup_ms;
} timeout_case_t;

static const timeout_case_t timeouts[] = {
    {false, false, 0, 0}, {true, false, 0, 0}, {false, true, 0, 0},
    {true, false, UINT64_MAX, 0}, {false, true, 0, UINT64_MAX},
    {true, true, 17, 23}, {true, true, UINT64_MAX, 0}
};

static void drain(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

/* Every selected test value is a one-byte integer or the nine-byte maximum. */
static void integer(uint8_t *buf, size_t *n, uint64_t value)
{
    if (value == UINT64_MAX) {
        memset(buf + *n, 0xff, 9);
        *n += 9;
    } else {
        buf[(*n)++] = (uint8_t)value;
    }
}

static size_t request(uint8_t buf[128], int location, bool fill,
                      const timeout_case_t *timeout, admission_prefix_t *expected)
{
    const uint8_t header[] = {3, 0, 0, 0, 1, 1, 'n', 1, 'v', 0};
    memcpy(buf, header, sizeof(header));
    size_t n = sizeof(header);
    memset(expected, 0, sizeof(*expected));
    moq_location_filter_cfg_init(&expected->location);
    buf[9] = (uint8_t)((location >= 0) + fill + timeout->object + timeout->subgroup);
    unsigned previous = 0;
    if (timeout->object) {
        buf[n++] = 2;
        integer(buf, &n, timeout->object_ms);
        expected->flags |= 4;
        expected->object_delivery_timeout_ms = timeout->object_ms;
        previous = 2;
    }
    if (timeout->subgroup) {
        buf[n++] = (uint8_t)(6 - previous);
        integer(buf, &n, timeout->subgroup_ms);
        expected->flags |= 8;
        expected->subgroup_delivery_timeout_ms = timeout->subgroup_ms;
        previous = 6;
    }
    if (location >= 0) {
        buf[n++] = (uint8_t)(0x21 - previous);
        buf[n++] = (uint8_t)location;
        expected->flags |= 1;
        expected->location.kind = (uint32_t)location;
        switch (location) {
        case 1:
            integer(buf, &n, UINT64_MAX);
            expected->location.relative_group_offset = UINT64_MAX;
            break;
        case 2: case 3: case 4:
            integer(buf, &n, UINT64_MAX);
            integer(buf, &n, 7);
            expected->location.start_group = UINT64_MAX;
            expected->location.start_object = 7;
            if (location >= 3) {
                integer(buf, &n, 0);
                expected->location.end_group = UINT64_MAX;
            }
            if (location == 4) {
                integer(buf, &n, UINT64_MAX);
                expected->location.end_object = UINT64_MAX;
            }
            break;
        default:
            break;
        }
        previous = 0x21;
    }
    if (fill) {
        buf[n++] = (uint8_t)(0x23 - previous);
        buf[n++] = 1; /* Length 1: Number of Parameters 0, not an absent fill. */
        buf[n++] = 0;
        expected->flags |= 2;
    }
    buf[1] = (uint8_t)((n - 3) >> 8);
    buf[2] = (uint8_t)(n - 3);
    return n;
}

static int row(moq_version_t version, int location, bool fill, unsigned ti)
{
    int failures = 0;
    test_alloc_state_t memory = {0};
    moq_alloc_t alloc = test_allocator(&memory);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &alloc;
    cfg.version = version;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 16;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 16;
    moq_simpair_t *pair = NULL;
    moq_session_t *client = NULL, *server = NULL;
    moq_event_t event;
    bool event_owned = false, started = false;
    uint8_t wire[128];
    admission_prefix_t expected, copy;
    size_t n = request(wire, location, fill, &timeouts[ti], &expected);
#define NEED(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: admission d%u location=%d fill=%u timeout=%u: %s\n", \
            (unsigned)version, location, (unsigned)fill, ti, #expr); \
    ++failures; goto done; \
} } while (0)
    NEED(moq_simpair_create(&cfg, &pair) == MOQ_OK);
    client = moq_simpair_client(pair);
    server = moq_simpair_server(pair);
    NEED(moq_simpair_start(pair) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    NEED(moq_session_state(client) == MOQ_SESS_ESTABLISHED &&
         moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    started = true;
    drain(client);
    drain(server);
    if (version == MOQ_VERSION_DRAFT_22) {
        NEED(moq_session_on_bidi_stream_bytes(server, moq_stream_ref_from_u64(0x7100),
                    wire, n, false, moq_simpair_now_us(pair)) == MOQ_OK);
    } else {
        moq_subscribe_cfg_t sub;
        moq_subscribe_cfg_init(&sub);
        moq_bytes_t part = MOQ_BYTES_LITERAL("n");
        sub.track_namespace = (moq_namespace_t){&part, 1};
        sub.track_name = MOQ_BYTES_LITERAL("v");
        sub.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
        moq_subscription_t handle;
        NEED(moq_session_subscribe(client, &sub, moq_simpair_now_us(pair), &handle) == MOQ_OK);
        NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    }
    event_owned = moq_session_poll_events(server, &event, 1) == 1;
    NEED(event_owned && event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST);
    NEED(event.detail_size >= sizeof(legacy_request_t));
    const admission_prefix_t *options = NULL;
    bool covered = event.detail_size >= sizeof(legacy_request_t) + sizeof(options);
    if (covered)
        memcpy(&options, (const uint8_t *)&event.u.subscribe_request +
                         sizeof(legacy_request_t), sizeof(options));
    if (version != MOQ_VERSION_DRAFT_22) {
        NEED(options == NULL);
        goto done;
    }
    NEED(covered && options != NULL);
    NEED((uintptr_t)options % _Alignof(admission_prefix_t) == 0);
    NEED(options->struct_size >= sizeof(copy));
    memcpy(&copy, options, sizeof(copy));
    expected.struct_size = copy.struct_size;
    NEED(memcmp(&copy, &expected, sizeof(copy)) == 0);
    memset(wire, 0xa5, sizeof(wire));
    NEED(memcmp(options, &expected, sizeof(expected)) == 0);
    moq_action_t action;
    size_t actions = moq_session_poll_actions(server, &action, 1);
    if (actions) moq_action_cleanup(&action);
    NEED(actions == 0); /* Observation precedes the application's ACK decision. */
    moq_event_cleanup(&event);
    event_owned = false;
    NEED(moq_session_tick(server, moq_simpair_now_us(pair) + 1) == MOQ_OK);
    NEED(memcmp(&copy, &expected, sizeof(copy)) == 0);
done:
    if (event_owned) moq_event_cleanup(&event);
    if (started) {
        MOQ_TEST_CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    }
    if (pair) moq_simpair_destroy(pair);
    MOQ_TEST_CHECK(memory.balance == 0);
    printf("%s: admission d%u location=%d fill=%u timeout=%u\n",
           failures ? "FAIL" : "PASS", (unsigned)version, location,
           (unsigned)fill, ti);
#undef NEED
    return failures;
}

int main(void)
{
    unsigned cases = 0, failed = 0;
    for (int location = -1; location <= 5; ++location)
        for (unsigned fill = 0; fill <= 1; ++fill)
            for (unsigned ti = 0; ti < sizeof(timeouts) / sizeof(timeouts[0]); ++ti) {
                ++cases;
                if (row(MOQ_VERSION_DRAFT_22, location, fill != 0, ti)) ++failed;
            }
    ++cases;
    if (row(MOQ_VERSION_DRAFT_16, -1, false, 0)) ++failed;
    ++cases;
    if (row(MOQ_VERSION_DRAFT_18, -1, false, 0)) ++failed;
    printf("%u/%u admission cases passed\n", cases - failed, cases);
    return failed ? 1 : 0;
}
#endif
