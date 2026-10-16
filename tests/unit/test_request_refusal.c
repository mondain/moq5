/*
 * New requests the session refuses itself, before the application sees them.
 *
 * After its own GOAWAY: the peer "SHOULD NOT initiate new requests", and the
 * endpoint "MAY reject new requests with an appropriate error code (e.g.,
 * REQUEST_ERROR with error code GOING_AWAY)" (draft-16 §9.4, draft-18 §10.4,
 * draft-22 §9.2). A request that crossed the GOAWAY is refused with
 * REQUEST_ERROR -- GOING_AWAY on draft-18/22, INTERNAL_ERROR on draft-16,
 * which registers no GOING_AWAY request code -- and the session stays up.
 * Every request type, on every profile.
 *
 * Session-level namespaces (draft-18 §3.2.1-§3.2.2, draft-22 §2.4.3, §6.5):
 * a request whose Track Namespace or prefix begins with ".session" or "."
 * is refused with DOES_NOT_EXIST rather than passed to the application, and
 * the application may not publish under one (MOQ_ERR_INVAL). Draft-16
 * defines neither, so such a request reaches its application there.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/profile.h"
#include <string.h>

static moq_version_t g_version;

static moq_simpair_t *mk_pair(void)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    return sp;
}

static void pump(moq_simpair_t *sp)
{
    moq_simpair_run_until_quiescent(sp, 64, NULL);
    moq_simpair_run_until_quiescent(sp, 64, NULL);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

enum { R_SUBSCRIBE, R_FETCH, R_TRACK_STATUS, R_PUBLISH_NAMESPACE, R_SUBSCRIBE_NAMESPACE,
       R_PUBLISH, R_SUBSCRIBE_TRACKS, R_COUNT };
static const char *const k_names[R_COUNT] = {
    "SUBSCRIBE", "FETCH", "TRACK_STATUS", "PUBLISH_NAMESPACE", "SUBSCRIBE_NAMESPACE",
    "PUBLISH", "SUBSCRIBE_TRACKS" };

/* Start request `which` from `c` under namespace `ns`/`name`. */
static moq_result_t start_request(moq_session_t *c, int which, moq_namespace_t ns,
                                  moq_bytes_t name)
{
    switch (which) {
    case R_SUBSCRIBE: {
        moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = name;
        moq_subscription_t h; return moq_session_subscribe(c, &cfg, 3, &h);
    }
    case R_FETCH: {
        moq_fetch_cfg_t cfg; moq_fetch_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = name; cfg.end_group = 2;
        moq_fetch_t h; return moq_session_fetch(c, &cfg, 3, &h);
    }
    case R_TRACK_STATUS: {
        moq_track_status_cfg_t cfg; moq_track_status_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = name;
        moq_track_status_handle_t h; return moq_session_track_status(c, &cfg, 3, &h);
    }
    case R_PUBLISH_NAMESPACE: {
        moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
        cfg.track_namespace = ns;
        moq_announcement_t h; return moq_session_publish_namespace(c, &cfg, 3, &h);
    }
    case R_SUBSCRIBE_NAMESPACE: {
        moq_subscribe_namespace_cfg_t cfg; moq_subscribe_namespace_cfg_init(&cfg);
        cfg.track_namespace_prefix = ns;
        cfg.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
        moq_ns_sub_handle_t h; return moq_session_subscribe_namespace(c, &cfg, 3, &h);
    }
    case R_PUBLISH: {
        moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
        cfg.track_namespace = ns; cfg.track_name = name;
        moq_publication_t h; return moq_session_publish(c, &cfg, 3, &h);
    }
    default: {
        moq_subscribe_tracks_cfg_t cfg; moq_subscribe_tracks_cfg_init(&cfg);
        cfg.track_namespace_prefix = ns;
        moq_track_sub_handle_t h; return moq_session_subscribe_tracks(c, &cfg, 3, &h);
    }
    }
}

static bool is_request_event(uint32_t kind)
{
    return kind == MOQ_EVENT_SUBSCRIBE_REQUEST || kind == MOQ_EVENT_FETCH_REQUEST ||
           kind == MOQ_EVENT_TRACK_STATUS_REQUEST || kind == MOQ_EVENT_NAMESPACE_PUBLISHED ||
           kind == MOQ_EVENT_NS_SUB_REQUEST || kind == MOQ_EVENT_PUBLISH_REQUEST ||
           kind == MOQ_EVENT_SUBSCRIBE_TRACKS_REQUEST;
}

/* The error code of the refusal the requester surfaced, or UINT64_MAX. */
static uint64_t take_refusal(moq_session_t *s)
{
    uint64_t code = UINT64_MAX;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        switch (ev.kind) {
        case MOQ_EVENT_SUBSCRIBE_ERROR: code = ev.u.subscribe_error.error_code; break;
        case MOQ_EVENT_FETCH_ERROR: code = ev.u.fetch_error.error_code; break;
        case MOQ_EVENT_TRACK_STATUS_ERROR: code = ev.u.track_status_error.error_code; break;
        case MOQ_EVENT_NAMESPACE_REJECTED: code = ev.u.namespace_rejected.error_code; break;
        case MOQ_EVENT_NS_SUB_ERROR: code = ev.u.ns_sub_error.error_code; break;
        case MOQ_EVENT_PUBLISH_ERROR: code = ev.u.publish_error.error_code; break;
        case MOQ_EVENT_SUBSCRIBE_TRACKS_ERROR: code = ev.u.subscribe_tracks_error.error_code; break;
        default: break;
        }
        moq_event_cleanup(&ev);
    }
    return code;
}

static size_t count_request_events(moq_session_t *s)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (is_request_event(ev.kind)) n++;
        moq_event_cleanup(&ev);
    }
    return n;
}

/* The client starts a request that crosses the server's GOAWAY. */
static int test_after_goaway(int which)
{
    int failures = 0;
    if (g_version == MOQ_VERSION_DRAFT_16 && which == R_SUBSCRIBE_TRACKS) return 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    MOQ_TEST_CHECK_EQ_INT((int)start_request(c, which, (moq_namespace_t){ parts, 1 },
                                             MOQ_BYTES_LITERAL("v")), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_goaway(sv, NULL, 0, 3), (int)MOQ_OK);
    pump(sp);
    MOQ_TEST_CHECK(moq_session_state(c) != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(moq_session_state(sv) != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK_EQ_SIZE(count_request_events(sv), 0);
    uint64_t want = g_version == MOQ_VERSION_DRAFT_16 ? MOQ_REQUEST_ERROR_INTERNAL_ERROR
                                                      : MOQ_REQUEST_ERROR_GOING_AWAY;
    uint64_t got = take_refusal(c);
    MOQ_TEST_CHECK_EQ_U64(got, want);
    if (failures) fprintf(stderr, "  after GOAWAY: %s\n", k_names[which]);
    moq_simpair_destroy(sp);
    return failures;
}

/* A request under a session-level namespace. The client's own refusal to
 * publish under it is lifted (a copy of its profile without the rule), so
 * the request reaches the server as a peer's would. */
static int test_session_namespace(int which, const char *first, const char *name)
{
    int failures = 0;
    if (g_version == MOQ_VERSION_DRAFT_16 && which == R_SUBSCRIBE_TRACKS) return 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_bytes_t parts[] = { { (const uint8_t *)first, strlen(first) },
                            MOQ_BYTES_LITERAL("x") };
    moq_namespace_t ns = { parts, 2 };
    moq_bytes_t tname = { (const uint8_t *)name, strlen(name) };

    bool publishes = which == R_PUBLISH || which == R_PUBLISH_NAMESPACE;
    if (publishes && g_version != MOQ_VERSION_DRAFT_16)
        MOQ_TEST_CHECK_EQ_INT((int)start_request(c, which, ns, tname), (int)MOQ_ERR_INVAL);
    static moq_profile_ops_t relaxed;
    const moq_profile_ops_t *real = c->profile;
    relaxed = *real;
    relaxed.reserved_session_namespaces = false;
    c->profile = &relaxed;
    MOQ_TEST_CHECK_EQ_INT((int)start_request(c, which, ns, tname), (int)MOQ_OK);
    pump(sp);
    c->profile = real;
    MOQ_TEST_CHECK(moq_session_state(c) != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(moq_session_state(sv) != MOQ_SESS_CLOSED);
    if (g_version == MOQ_VERSION_DRAFT_16) {
        /* No such rule: the application decides. */
        MOQ_TEST_CHECK_EQ_SIZE(count_request_events(sv), 1);
    } else {
        MOQ_TEST_CHECK_EQ_SIZE(count_request_events(sv), 0);
        MOQ_TEST_CHECK_EQ_U64(take_refusal(c), MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    }
    if (failures)
        fprintf(stderr, "  session namespace: %s '%s'/'%s'\n", k_names[which], first, name);
    moq_simpair_destroy(sp);
    return failures;
}

/* Fields that merely start with ".session" or "." are not reserved. */
static int test_lookalike_namespace(void)
{
    int failures = 0;
    static const char *const firsts[] = { ".sessions", ".", "..", "session" };
    for (size_t i = 0; i < 4; i++) {
        moq_bytes_t parts[] = { { (const uint8_t *)firsts[i], strlen(firsts[i]) } };
        moq_namespace_t ns = { parts, 1 };
        bool want = (i == 1);
        MOQ_TEST_CHECK(session_namespace_reserved(&ns) == want);
    }
    moq_bytes_t sp[] = { MOQ_BYTES_LITERAL(".session") };
    moq_namespace_t ns = { sp, 1 };
    MOQ_TEST_CHECK(session_namespace_reserved(&ns));
    moq_namespace_t empty = { NULL, 0 };
    MOQ_TEST_CHECK(!session_namespace_reserved(&empty));
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t v = 0; v < 3; v++) {
        g_version = versions[v];
        int before = failures;
        for (int w = 0; w < R_COUNT; w++) {
            failures += test_after_goaway(w);
            failures += test_session_namespace(w, ".session", "");
            failures += test_session_namespace(w, ".session", "t");
            failures += test_session_namespace(w, ".", "t");
        }
        if (failures != before)
            fprintf(stderr, "  version 0x%llx: %d failures\n",
                    (unsigned long long)versions[v], failures - before);
    }
    failures += test_lookalike_namespace();
    if (failures == 0) MOQ_TEST_PASS("request_refusal");
    return failures != 0;
}
