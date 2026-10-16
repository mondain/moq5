#include <moq/subscriber.h>
#include <moq/sim.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); ++failures; \
} } while (0)
#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "SETUP_FAIL:%d: %s\n", __LINE__, #expr); return false; \
} } while (0)

typedef struct fixture {
    moq_simpair_t *pair;
    moq_subscriber_t *sub;
    moq_stream_ref_t request_stream;
} fixture_t;

static void trace(void *ctx, const moq_sim_trace_record_t *event)
{
    fixture_t *f = ctx;
    if (event->kind == MOQ_SIM_TRACE_ACTION &&
        event->from == MOQ_PERSPECTIVE_CLIENT &&
        event->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM)
        f->request_stream = event->stream_ref;
}

static bool run(fixture_t *f, uint64_t code)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.trace_fn = trace;
    cfg.trace_ctx = f;
    REQUIRE(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_start(f->pair) == MOQ_OK);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_session_t *client = moq_simpair_client(f->pair);
    moq_session_t *server = moq_simpair_server(f->pair);
    moq_event_t event;
    while (moq_session_poll_events(client, &event, 1)) moq_event_cleanup(&event);
    while (moq_session_poll_events(server, &event, 1)) moq_event_cleanup(&event);
    moq_sub_cfg_t sub_cfg;
    moq_sub_cfg_init_sized(&sub_cfg, sizeof(sub_cfg));
    REQUIRE(moq_sub_create(client, moq_alloc_default(), &sub_cfg, &f->sub) == MOQ_OK);
    moq_sub_status_cfg_t request;
    moq_sub_status_cfg_init(&request);
    moq_bytes_t part = MOQ_BYTES_LITERAL("status");
    request.track_namespace = (moq_namespace_t){&part, 1};
    request.track_name = MOQ_BYTES_LITERAL("v");
    moq_sub_status_req_t *token = NULL;
    REQUIRE(moq_sub_track_status(f->sub, &request, 1, &token) == MOQ_OK);
    REQUIRE(token != NULL);
    REQUIRE(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    unsigned requests = 0;
    while (moq_session_poll_events(server, &event, 1)) {
        CHECK(event.kind == MOQ_EVENT_TRACK_STATUS_REQUEST);
        if (event.kind == MOQ_EVENT_TRACK_STATUS_REQUEST) ++requests;
        moq_event_cleanup(&event);
    }
    REQUIRE(requests == 1 && f->request_stream._v != 0);
    REQUIRE(moq_session_on_bidi_stream_reset(client, f->request_stream, code, 2) == MOQ_OK);
    REQUIRE(moq_sub_tick(f->sub, 2) == MOQ_OK);
    moq_sub_status_result_t result;
    unsigned char before[sizeof(result)];
    memset(&result, 0xa5, sizeof(result));
    memcpy(before, &result, sizeof(before));
    for (unsigned i = 0; i < 2; ++i) {
        moq_result_t rc = moq_sub_poll_status(f->sub, &result);
        printf("legacy reset=%" PRIu64 " poll=%u result=%d\n", code, i, rc);
        CHECK(rc == MOQ_ERR_UNSUPPORTED);
        CHECK(memcmp(&result, before, sizeof(result)) == 0);
    }
    CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    return true;
}

int main(void)
{
    const uint64_t codes[] = {0, UINT64_MAX};
    unsigned setup_failures = 0;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        fixture_t f = {0};
        if (!run(&f, codes[i])) ++setup_failures;
        moq_sub_destroy(f.sub);
        moq_simpair_destroy(f.pair);
    }
    printf("subscriber_status_legacy: 2 cases, %u failures, %u setup failures\n",
           failures, setup_failures);
    return setup_failures ? 2 : failures ? 1 : 0;
}
