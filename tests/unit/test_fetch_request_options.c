/* Approved FETCH options are a self-contained value prefix, not a pointer
 * to another borrowed record. These literal requests do not use its encoder. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include <string.h>

typedef struct options_v0 {
    uint32_t struct_size, flags;
    moq_location_filter_cfg_t location;
    uint64_t fill_timeout_ms;
} options_v0_t;

_Static_assert(sizeof(options_v0_t) == 64, "v0 FETCH options prefix");
_Static_assert(offsetof(options_v0_t, location) == 8, "value location offset");
_Static_assert(offsetof(options_v0_t, fill_timeout_ms) == 56, "timeout offset");

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1) moq_event_cleanup(&ev);
}

/* Test values are single-byte vi64 or its explicit nine-byte MAX form. */
static void integer(uint8_t *buf, size_t *n, uint64_t value)
{
    if (value == UINT64_MAX) {
        memset(buf + *n, 0xff, 9);
        *n += 9;
    } else {
        buf[(*n)++] = (uint8_t)value;
    }
}

static size_t request(uint8_t buf[128], int location_kind, int properties,
                      bool has_timeout, uint64_t timeout,
                      moq_location_filter_cfg_t *expected_location)
{
    const uint8_t header[] = {0x16, 0, 0, 0, 1, 1, 'n', 1, 'v', 0};
    memcpy(buf, header, sizeof(header));
    size_t n = sizeof(header);
    buf[9] = (uint8_t)((has_timeout ? 1 : 0) + (location_kind >= 0 ? 1 : 0) +
                       (properties >= 0 ? 1 : 0));
    unsigned previous = 0;
    moq_location_filter_cfg_init(expected_location);
    if (has_timeout) {
        buf[n++] = 0x0a;
        integer(buf, &n, timeout);
        previous = 0x0a;
    }
    if (location_kind >= 0) {
        buf[n++] = (uint8_t)(0x21 - previous);
        buf[n++] = (uint8_t)location_kind;
        expected_location->kind = (uint32_t)location_kind;
        switch (location_kind) {
        case 1:
            integer(buf, &n, UINT64_MAX);
            expected_location->relative_group_offset = UINT64_MAX;
            break;
        case 2: case 3: case 4:
            integer(buf, &n, UINT64_MAX);
            integer(buf, &n, 7);
            expected_location->start_group = UINT64_MAX;
            expected_location->start_object = 7;
            if (location_kind >= 3) {
                integer(buf, &n, 0); /* Group delta, not absolute end. */
                expected_location->end_group = UINT64_MAX;
            }
            if (location_kind == 4) {
                integer(buf, &n, UINT64_MAX);
                expected_location->end_object = UINT64_MAX;
            }
            break;
        default:
            break;
        }
        previous = 0x21;
    }
    if (properties >= 0) {
        buf[n++] = (uint8_t)(0x35 - previous);
        buf[n++] = (uint8_t)properties;
    }
    buf[1] = (uint8_t)((n - 3) >> 8);
    buf[2] = (uint8_t)(n - 3);
    return n;
}

static int options_row(moq_version_t version, int location_kind, int properties,
                       bool has_timeout, uint64_t timeout)
{
    int failures = 0;
    test_alloc_state_t alloc_state = {0};
    moq_alloc_t alloc = test_allocator(&alloc_state);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &alloc;
    cfg.version = version;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    moq_simpair_t *pair = NULL;
    moq_session_t *server = NULL, *client = NULL;
    moq_event_t event;
    bool event_owned = false;
    options_v0_t observed, expected;
    memset(&expected, 0, sizeof(expected));
    uint8_t wire[128];
    size_t wire_len = request(wire, location_kind, properties, has_timeout,
                              timeout, &expected.location);
#define NEED(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: fetch_options d%u location=%d properties=%d timeout=%u:%llu: %s\n", \
                (unsigned)version, location_kind, properties, (unsigned)has_timeout, \
                (unsigned long long)timeout, #expr); \
        failures++; \
        goto done; \
    } \
} while (0)
    NEED(moq_simpair_create(&cfg, &pair) == MOQ_OK);
    client = moq_simpair_client(pair);
    server = moq_simpair_server(pair);
    NEED(moq_simpair_start(pair) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    NEED(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    drain(client);
    drain(server);
    if (version == MOQ_VERSION_DRAFT_22) {
        NEED(moq_session_set_defer_fetch_ranges(server, true) == MOQ_OK);
        NEED(moq_session_on_bidi_stream_bytes(server, moq_stream_ref_from_u64(0x7000),
                    wire, wire_len, false, moq_simpair_now_us(pair)) == MOQ_OK);
    } else {
        moq_bytes_t part = MOQ_BYTES_LITERAL("n");
        moq_fetch_cfg_t fetch_cfg;
        moq_fetch_cfg_init_sized(&fetch_cfg, sizeof(fetch_cfg));
        fetch_cfg.track_namespace = (moq_namespace_t){ &part, 1 };
        fetch_cfg.track_name = MOQ_BYTES_LITERAL("v");
        fetch_cfg.end_group = 1;
        moq_fetch_t fetch;
        NEED(moq_session_fetch(client, &fetch_cfg, moq_simpair_now_us(pair), &fetch) == MOQ_OK);
        NEED(moq_simpair_run_until_quiescent(pair, 16, NULL) == MOQ_OK);
    }
    event_owned = moq_session_poll_events(server, &event, 1) == 1;
    NEED(event_owned);
    NEED(event.kind == MOQ_EVENT_FETCH_REQUEST);
    NEED(event.detail_size >= offsetof(moq_fetch_request_event_t, options) +
                              sizeof(event.u.fetch_request.options));
    const moq_fetch_request_options_t *options = event.u.fetch_request.options;
    if (version != MOQ_VERSION_DRAFT_22) {
        NEED(options == NULL);
        goto done;
    }
    NEED(options != NULL);
    NEED((uintptr_t)options % _Alignof(options_v0_t) == 0);
    NEED(options->struct_size >= sizeof(observed));
    memcpy(&observed, options, sizeof(observed));
    expected.struct_size = observed.struct_size;
    expected.flags = (has_timeout ? 1u : 0u) | (properties != 0 ? 2u : 0u);
    expected.fill_timeout_ms = has_timeout ? timeout : 0;
    NEED(memcmp(&observed, &expected, sizeof(observed)) == 0);
    memset(wire, 0xa5, sizeof(wire));
    NEED(memcmp(options, &expected, sizeof(expected)) == 0);
    moq_event_cleanup(&event);
    event_owned = false;
    NEED(moq_session_tick(server, moq_simpair_now_us(pair) + 1) == MOQ_OK);
    NEED(memcmp(&observed, &expected, sizeof(observed)) == 0);
done:
    if (event_owned) moq_event_cleanup(&event);
    if (server) MOQ_TEST_CHECK(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    if (client) MOQ_TEST_CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    if (pair) moq_simpair_destroy(pair);
    MOQ_TEST_CHECK(alloc_state.balance == 0);
    printf("%s: fetch_options d%u location=%d properties=%d timeout=%u:%llu\n",
           failures ? "FAIL" : "PASS", (unsigned)version, location_kind,
           properties, (unsigned)has_timeout, (unsigned long long)timeout);
#undef NEED
    return failures;
}

int main(void)
{
    int failures = 0;
    const uint64_t timeouts[] = {0, 17, UINT64_MAX};
    for (int location = -1; location <= 5; location++) {
        for (int properties = -1; properties <= 1; properties++) {
            failures += options_row(MOQ_VERSION_DRAFT_22, location, properties, false, 0);
            for (size_t t = 0; t < sizeof(timeouts) / sizeof(timeouts[0]); t++)
                failures += options_row(MOQ_VERSION_DRAFT_22, location, properties,
                                        true, timeouts[t]);
        }
    }
    failures += options_row(MOQ_VERSION_DRAFT_16, -1, -1, false, 0);
    failures += options_row(MOQ_VERSION_DRAFT_18, -1, -1, false, 0);
    printf("fetch_request_options: 86 cases, %d failures\n", failures);
    return failures ? 1 : 0;
}
