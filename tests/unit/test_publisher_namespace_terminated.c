#include <moq/publisher.h>
#include <moq/sim.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* This client uses only the unchanged API prefix, so it also fails against a
 * library without namespace-terminal reporting. Numeric kind 3 is
 * intentional: the fixture does not depend on the kind's name. */
static unsigned failures, setups, cases;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define NEED(x) do { if (!(x)) { fprintf(stderr, "SETUP:%d: %s\n", __LINE__, #x); ++setups; return false; } } while (0)

typedef struct observation {
    unsigned count;
    moq_pub_namespace_terminal_info_t info;
} observation_t;
typedef struct fixture {
    moq_simpair_t *pair;
    moq_publisher_t *pub[2];
    moq_pub_track_t *tracks[3];
    moq_stream_ref_t refs[2];
    moq_announcement_t peer[2];
    unsigned opened;
    observation_t seen[2];
} fixture_t;

static void observed(void *ctx, const moq_pub_namespace_terminal_info_t *info)
{
    observation_t *o = ctx;
    ++o->count;
    memset(&o->info, 0, sizeof(o->info));
    size_t size = info->struct_size < sizeof(o->info) ? info->struct_size : sizeof(o->info);
    memcpy(&o->info, info, size);
    CHECK(info->namespace_.count == 1);
    CHECK(info->namespace_.parts[0].len == 1);
    CHECK(info->namespace_.parts[0].data[0] == 'a');
}
static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM && f->opened < 2)
        f->refs[f->opened++] = r->stream_ref;
}
static void drain(moq_session_t *s)
{
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}
static bool dispatch(fixture_t *f, uint64_t now)
{
    moq_event_t e;
    while (moq_session_poll_events(moq_simpair_client(f->pair), &e, 1)) {
        for (unsigned i = 0; i < 2; ++i) {
            moq_pub_event_result_t result;
            if (e.kind == MOQ_EVENT_NAMESPACE_TERMINATED) {
                moq_event_t short_event = e;
                short_event.detail_size = sizeof(moq_namespace_terminated_event_t) - 1;
                CHECK(moq_pub_handle_event(f->pub[i], &short_event, now, &result) == MOQ_ERR_INVAL);
                CHECK(result == MOQ_PUB_EVENT_IGNORED);
            }
            moq_result_t rc = moq_pub_handle_event(f->pub[i], &e, now, &result);
            CHECK(rc == MOQ_OK);
            if (e.kind == MOQ_EVENT_NAMESPACE_TERMINATED) {
                CHECK(result == (i == 0 ? MOQ_PUB_EVENT_CONSUMED : MOQ_PUB_EVENT_IGNORED));
                /* Replaying an already observed owner must not fire again. */
                CHECK(moq_pub_handle_event(f->pub[i], &e, now, &result) == MOQ_OK);
            }
        }
        moq_event_cleanup(&e);
    }
    return true;
}
static bool run(fixture_t *f, bool accepted, bool stop, uint64_t code)
{
    ++cases;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default(); cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.trace_fn = trace; cfg.trace_ctx = f;
    NEED(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    NEED(moq_simpair_start(f->pair) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_session_t *client = moq_simpair_client(f->pair), *server = moq_simpair_server(f->pair);
    drain(client); drain(server);
    for (unsigned i = 0; i < 2; ++i) {
        moq_pub_cfg_t pc; moq_pub_cfg_init_sized(&pc, sizeof(pc));
        pc.callbacks.ctx = &f->seen[i]; pc.callbacks.on_namespace_terminal = observed;
        NEED(moq_pub_create(client, moq_alloc_default(), &pc, &f->pub[i]) == MOQ_OK);
        moq_pub_track_cfg_t tc; moq_pub_track_cfg_init(&tc);
        moq_bytes_t ns = i ? MOQ_BYTES_LITERAL("b") : MOQ_BYTES_LITERAL("a");
        tc.track_namespace = (moq_namespace_t){&ns, 1};
        tc.track_name = MOQ_BYTES_LITERAL("v"); tc.advertise_namespace = true;
        NEED(moq_pub_add_track(f->pub[i], &tc, 1, &f->tracks[i]) == MOQ_OK);
        if (!i) {
            tc.track_name = MOQ_BYTES_LITERAL("audio");
            NEED(moq_pub_add_track(f->pub[i], &tc, 1, &f->tracks[2]) == MOQ_OK);
        }
    }
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_event_t e; unsigned requests = 0;
    while (moq_session_poll_events(server, &e, 1)) {
        CHECK(e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED);
        if (e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED && requests < 2)
            f->peer[requests++] = e.u.namespace_published.ann;
        moq_event_cleanup(&e);
    }
    NEED(requests == 2 && f->opened == 2);
    if (accepted) {
        moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
        NEED(moq_session_accept_namespace(server, f->peer[0], &ac, 2) == MOQ_OK);
        NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
        dispatch(f, 2);
    }
    NEED((stop ? moq_session_on_bidi_stream_stop(client, f->refs[0], code, 3)
               : moq_session_on_bidi_stream_reset(client, f->refs[0], code, 3)) == MOQ_OK);
    dispatch(f, 3);
    printf("namespace accepted=%u stop=%u code=%" PRIu64 " callbacks=%u\n",
           accepted, stop, code, f->seen[0].count);
    CHECK(f->seen[0].count == 1);
    CHECK((unsigned)f->seen[0].info.kind == 3);
    CHECK(f->seen[0].info.error_code == 0 && !f->seen[0].info.can_retry);
    CHECK(f->seen[0].info.retry_after_ms == 0 && f->seen[0].info.reason.len == 0);
#ifndef MOQ_TEST_OLD_PREFIX_CLIENT
    CHECK(f->seen[0].info.struct_size == sizeof(f->seen[0].info));
    CHECK(f->seen[0].info.cause == (stop ? MOQ_NAMESPACE_TERMINAL_PEER_STOP_SENDING : MOQ_NAMESPACE_TERMINAL_PEER_RESET_STREAM));
    CHECK(f->seen[0].info.local_role == MOQ_NAMESPACE_ROLE_REQUESTER);
    CHECK(f->seen[0].info.stream_error_code == code);
#endif
    CHECK(f->seen[1].count == 0);
    moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
    NEED(moq_session_accept_namespace(server, f->peer[1], &ac, 4) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    dispatch(f, 4);
    CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    CHECK(f->seen[0].count == 1 && f->seen[1].count == 0);
    CHECK(moq_pub_remove_track(f->pub[0], f->tracks[0], 5) == MOQ_OK);
    CHECK(moq_pub_remove_track(f->pub[0], f->tracks[2], 5) == MOQ_OK);
    CHECK(moq_pub_remove_track(f->pub[1], f->tracks[1], 5) == MOQ_OK);
    return true;
}

/* Public manual transport gives this fixture a one-event queue and an exact
 * response scratch limit. No session/private layout is used. */
static uint32_t accept_uri(void *ctx, uint32_t present, moq_bytes_t authority, moq_bytes_t path)
{
    (void)ctx; (void)authority; (void)path;
    return present == MOQ_BINDING_TARGET_COMPLETE ? MOQ_URI_ACCEPT : MOQ_URI_INVALID_AUTHORITY;
}
static bool take_action(moq_session_t *s, moq_action_kind_t kind, moq_action_t *a)
{
    NEED(moq_session_poll_actions(s, a, 1) == 1);
    if (a->kind != kind) { moq_action_cleanup(a); NEED(false); }
    return true;
}
typedef struct manual_fixture {
    moq_session_t *s[2];
    moq_publisher_t *pub;
    observation_t seen;
} manual_fixture_t;
static bool manual_collect(manual_fixture_t *f)
{
    for (unsigned pass = 0; pass < 8; ++pass) {
        moq_event_t e;
        while (moq_session_poll_events(f->s[0], &e, 1)) {
            moq_pub_event_result_t result;
            moq_result_t hrc = moq_pub_handle_event(f->pub, &e, 10, &result);
            /* The pressure TRACK_STATUS names this publisher's track, so the
             * facade takes it and answers it; with a one-action queue that
             * answer can wait for a flush (the manual WOULD_BLOCK + CONSUMED
             * contract). */
            CHECK(hrc == MOQ_OK ||
                  (hrc == MOQ_ERR_WOULD_BLOCK && result == MOQ_PUB_EVENT_CONSUMED));
            moq_event_cleanup(&e);
        }
        moq_action_t a;
        while (moq_session_poll_actions(f->s[0], &a, 1)) moq_action_cleanup(&a);
        moq_result_t frc = moq_pub_flush(f->pub, 10);
        CHECK(frc == MOQ_OK || frc == MOQ_ERR_WOULD_BLOCK);
        while (moq_session_poll_actions(f->s[0], &a, 1)) moq_action_cleanup(&a);
        moq_result_t rc = moq_session_process_pending(f->s[0], 10);
        NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    }
    return true;
}
/* 0 missing response, 1 valid response+FIN, 2 partial response+FIN,
 * 3 local capacity, 4 exact-size peer ERROR negative control. */
static bool manual_run(manual_fixture_t *f, unsigned scenario, bool pressure)
{
    ++cases;
    static const uint8_t reason[] = "0123456789abcdef0123456789abcdef0123456789abcdef";
    for (unsigned side = 0; side < 2; ++side) {
        moq_session_cfg_t c;
        moq_session_cfg_init_sized(&c, sizeof(c), moq_alloc_default(),
            side ? MOQ_PERSPECTIVE_SERVER : MOQ_PERSPECTIVE_CLIENT);
        c.version = MOQ_VERSION_DRAFT_22; c.initial_request_capacity = 16;
        c.send_request_capacity = false; c.max_actions = 1; c.max_events = 1;
        if (!side && scenario >= 3) c.output_scratch_size = (uint32_t)sizeof(reason) - (scenario == 3 ? 2u : 1u);
        NEED(moq_session_create(&c, 0, &f->s[side]) == MOQ_OK);
        moq_transport_binding_t b; moq_transport_binding_init(&b);
        b.kind = MOQ_BINDING_RAW_QUIC; b.facts_known = b.facts_supported = MOQ_BINDING_FACTS_ALL;
        if (side) b.accept_uri = accept_uri;
        else { b.target_present = MOQ_BINDING_TARGET_COMPLETE; b.authority = MOQ_BYTES_LITERAL("test.invalid"); }
        NEED(moq_session_bind_transport(f->s[side], &b, sizeof(b)) == MOQ_OK);
        NEED(moq_session_start(f->s[side], 0) == MOQ_OK);
    }
    for (unsigned side = 0; side < 2; ++side) {
        moq_action_t a; NEED(take_action(f->s[side], MOQ_ACTION_OPEN_UNI_CONTROL, &a));
        moq_result_t rc = moq_session_on_control_bytes(f->s[1-side], a.u.open_uni_control.data, a.u.open_uni_control.len, 1);
        moq_action_cleanup(&a); NEED(rc == MOQ_OK);
    }
    drain(f->s[0]); drain(f->s[1]);
    moq_pub_cfg_t pc; moq_pub_cfg_init_sized(&pc, sizeof(pc));
    pc.callbacks.ctx = &f->seen; pc.callbacks.on_namespace_terminal = observed;
    NEED(moq_pub_create(f->s[0], moq_alloc_default(), &pc, &f->pub) == MOQ_OK);
    moq_bytes_t part = MOQ_BYTES_LITERAL("a");
    moq_pub_track_cfg_t tc; moq_pub_track_cfg_init(&tc);
    tc.track_namespace = (moq_namespace_t){&part, 1}; tc.track_name = MOQ_BYTES_LITERAL("v"); tc.advertise_namespace = true;
    moq_pub_track_t *track = NULL;
    NEED(moq_pub_add_track(f->pub, &tc, 2, &track) == MOQ_OK);
    moq_action_t a; NEED(take_action(f->s[0], MOQ_ACTION_OPEN_BIDI_STREAM, &a));
    moq_stream_ref_t local = a.u.open_bidi_stream.stream_ref, remote = moq_stream_ref_from_u64(local._v + 65536);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f->s[1], remote, a.u.open_bidi_stream.data, a.u.open_bidi_stream.len, false, 2);
    moq_action_cleanup(&a); NEED(rc == MOQ_OK);
    moq_event_t e; NEED(moq_session_poll_events(f->s[1], &e, 1) == 1);
    NEED(e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED);
    moq_announcement_t peer = e.u.namespace_published.ann; moq_event_cleanup(&e);
    if (pressure) {
        moq_track_status_cfg_t ts; moq_track_status_cfg_init(&ts);
        ts.track_namespace = tc.track_namespace; ts.track_name = tc.track_name;
        moq_track_status_handle_t h;
        NEED(moq_session_track_status(f->s[1], &ts, 3, &h) == MOQ_OK);
        NEED(take_action(f->s[1], MOQ_ACTION_OPEN_BIDI_STREAM, &a));
        rc = moq_session_on_bidi_stream_bytes(f->s[0], moq_stream_ref_from_u64(99999), a.u.open_bidi_stream.data, a.u.open_bidi_stream.len, false, 3);
        moq_action_cleanup(&a); NEED(rc == MOQ_OK);
    }
    if (scenario == 0) {
        rc = moq_session_on_bidi_stream_bytes(f->s[0], local, NULL, 0, true, 4);
    } else {
        if (scenario >= 3) {
            moq_reject_namespace_cfg_t r; moq_reject_namespace_cfg_init(&r);
            r.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST; r.can_retry = true; r.retry_after_ms = 17;
            r.reason = (moq_bytes_t){reason, sizeof(reason) - 1};
            NEED(moq_session_reject_namespace(f->s[1], peer, &r, 4) == MOQ_OK);
        } else {
            moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
            NEED(moq_session_accept_namespace(f->s[1], peer, &ac, 4) == MOQ_OK);
        }
        NEED(take_action(f->s[1], MOQ_ACTION_SEND_BIDI_STREAM, &a));
        size_t n = scenario == 2 ? 1 : a.u.send_bidi_stream.len;
        rc = moq_session_on_bidi_stream_bytes(f->s[0], local, a.u.send_bidi_stream.data, n, scenario < 3, 4);
        moq_action_cleanup(&a);
    }
    NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    if (scenario >= 3) {
        /* Once a complete response is validated, a later STOP cannot replace
         * its peer ERROR or already known LOCAL_CAPACITY outcome. */
        rc = moq_session_on_bidi_stream_stop(f->s[0], local, UINT64_MAX, 5);
        NEED(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    }
    NEED(manual_collect(f));
    CHECK(f->seen.count == 1);
    if (scenario == 4) {
        CHECK(f->seen.info.kind == MOQ_PUB_NAMESPACE_REJECTED);
        CHECK(f->seen.info.error_code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
        CHECK(f->seen.info.can_retry && f->seen.info.retry_after_ms == 17);
        CHECK(f->seen.info.reason.len == sizeof(reason) - 1);
    } else {
        static const uint32_t causes[] = {MOQ_NAMESPACE_TERMINAL_MISSING_RESPONSE,
            MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE, MOQ_NAMESPACE_TERMINAL_TRUNCATED_MESSAGE,
            MOQ_NAMESPACE_TERMINAL_LOCAL_CAPACITY};
        CHECK(f->seen.info.kind == MOQ_PUB_NAMESPACE_TERMINATED);
        CHECK(f->seen.info.cause == causes[scenario]);
        CHECK(f->seen.info.local_role == MOQ_NAMESPACE_ROLE_REQUESTER);
        CHECK(f->seen.info.stream_error_code == 0 && f->seen.info.error_code == 0);
        CHECK(!f->seen.info.can_retry && !f->seen.info.retry_after_ms && !f->seen.info.reason.len);
    }
    CHECK(moq_session_state(f->s[0]) == MOQ_SESS_ESTABLISHED);
    NEED(moq_pub_remove_track(f->pub, track, 11) == MOQ_OK);
    /* Native owner retirement and facade terminal state allow a fresh owner. */
    NEED(moq_pub_add_track(f->pub, &tc, 12, &track) == MOQ_OK);
    NEED(take_action(f->s[0], MOQ_ACTION_OPEN_BIDI_STREAM, &a));
    CHECK(a.u.open_bidi_stream.stream_ref._v != local._v); moq_action_cleanup(&a);
    printf("namespace manual scenario=%u pressure=%u callbacks=%u\n", scenario, pressure, f->seen.count);
    return true;
}
int main(void)
{
    for (unsigned accepted = 0; accepted < 2; ++accepted)
        for (unsigned stop = 0; stop < 2; ++stop)
            for (unsigned max = 0; max < 2; ++max) {
                fixture_t f = {0};
                (void)run(&f, accepted != 0, stop != 0, max ? UINT64_MAX : 0);
                moq_pub_destroy(f.pub[0]); moq_pub_destroy(f.pub[1]);
                moq_simpair_destroy(f.pair);
            }
    for (unsigned scenario = 0; scenario < 5; ++scenario) for (unsigned p = 0; p < 2; ++p) {
        manual_fixture_t f = {0}; (void)manual_run(&f, scenario, p != 0);
        moq_pub_destroy(f.pub); moq_session_destroy(f.s[0]); moq_session_destroy(f.s[1]);
    }
    printf("publisher_namespace_terminated: %u cases, %u failures, %u setup failures\n", cases, failures, setups);
    return setups ? 2 : failures ? 1 : 0;
}
