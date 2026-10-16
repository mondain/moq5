#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

static void drain_events(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

static int input_case(moq_version_t version, bool selected, uint64_t ms, bool torn)
{
    int failures = 0;
    moq_simpair_cfg_t pc = MOQ_SIMPAIR_CFG_INIT;
    pc.alloc = moq_alloc_default();
    pc.version = version;
    pc.server_send_request_capacity = true;
    pc.server_initial_request_capacity = 16;
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
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){&part, 1};
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.filter = version == MOQ_VERSION_DRAFT_22 ? MOQ_SUBSCRIBE_FILTER_NONE :
        MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    cfg.has_rendezvous_timeout = selected;
    cfg.rendezvous_timeout_ms = ms;
    if (torn) cfg.struct_size = offsetof(moq_subscribe_cfg_t, rendezvous_timeout_ms) + 7;
    moq_subscription_t sub = MOQ_SUBSCRIPTION_INVALID;
    rc = moq_session_subscribe(client, &cfg, 1000, &sub);
    bool present = selected && !torn;
    bool legacy_refusal = version != MOQ_VERSION_DRAFT_22 && present;
    MOQ_TEST_CHECK_EQ_INT(rc, legacy_refusal ? MOQ_ERR_UNSUPPORTED : MOQ_OK);
    moq_action_t action;
    size_t count = moq_session_poll_actions(client, &action, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, legacy_refusal ? 0 : 1);
    if (!count) goto done;
    if (version == MOQ_VERSION_DRAFT_22 && rc == MOQ_OK) {
        /* Independent literal oracle, including explicit zero and all64 bits. */
        uint8_t wire[32] = {3, 0, 7, 0, 1, 1, 'n', 1, 'v', 0};
        size_t len = 10;
        if (present) {
            wire[9] = 1;
            wire[len++] = 4;
            if (ms == UINT64_MAX) {
                memset(wire + len, 0xff, 9);
                len += 9;
            } else wire[len++] = (uint8_t)ms;
        }
        wire[2] = (uint8_t)(len - 3);
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
            MOQ_TEST_CHECK_EQ_INT(event.kind, MOQ_EVENT_SUBSCRIBE_REQUEST);
            if (event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
                const moq_subscription_admission_options_t *o = event.u.subscribe_request.options;
                MOQ_TEST_CHECK(o != NULL);
                if (o) {
                    MOQ_TEST_CHECK(o->struct_size >= offsetof(moq_subscription_admission_options_t,
                        rendezvous_timeout_ms) + sizeof(uint64_t));
                    MOQ_TEST_CHECK_EQ_INT(o->flags, present ? MOQ_SUBSCRIPTION_ADMISSION_RENDEZVOUS_TIMEOUT : 0);
                    MOQ_TEST_CHECK_EQ_U64(o->rendezvous_timeout_ms, present ? ms : 0);
                }
            }
            moq_event_cleanup(&event);
        }
    }
    moq_action_cleanup(&action);
done:
    moq_simpair_destroy(pair);
    return failures;
}

int main(void)
{
    int failures = 0;
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    const uint64_t values[] = {0, 25, UINT64_MAX};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v)
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            for (unsigned selected = 0; selected < 2; ++selected)
                for (unsigned torn = 0; torn < 2; ++torn)
                    failures += input_case(versions[v], selected != 0, values[i], torn != 0);
    if (!failures) MOQ_TEST_PASS("subscription rendezvous exact input and admission");
    return failures ? 1 : 0;
}
