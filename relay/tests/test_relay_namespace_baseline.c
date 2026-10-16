/* Draft-22 namespace updates (§9.5), track discovery (§9.18) and rendezvous
 * (§9.20.6) at the production relay binding, from literal request bytes and
 * supplied time only, with no namespace-update API on the requesting side.
 * --controls pins behaviour that needs none of these features (initial
 * announcement and namespace subscription, a wrong-context update closing
 * the session, absent and zero rendezvous refused at once); --features pins
 * the features themselves. A case that fails reports BEHAVIOR_FAIL, and one
 * whose update could not be reached because the initial SUBSCRIBE_TRACKS
 * was refused reports PREREQUISITE_FAIL. */
#include <moq/moq.h>
#include <moq/relay/moqr_bind.h>
#include <moq/relay/relay.h>
#include <moq/sim.h>
#include "../../tests/unit/test_support.h"

#include <stdio.h>
#include <string.h>

typedef struct fixture {
    moq_simpair_t *pair;
    moq_session_t *session;
    moqr_core_t *core;
    moqr_bind_t *bind;
} fixture_t;

typedef struct observation {
    unsigned ok, error, fin, close, other;
    unsigned error_code;
    uint64_t close_code;
} observation_t;

typedef struct totals {
    unsigned cases, behavior_failures, blocked, fixture_errors;
} totals_t;

static const moq_stream_ref_t request_ref = {UINT64_C(0x7900)};

/* Namespace n, track v. All varints here fit one byte; 50/51 use the
 * already-qualified non-minimal two-byte type spelling. */
static const uint8_t announce[] = {6, 0, 5, 0, 1, 1, 'n', 0};
static const uint8_t namespace_sub[] = {0x80, 0x50, 0, 5, 0, 1, 1, 'n', 0};
static const uint8_t tracks_sub[] = {0x80, 0x51, 0, 5, 0, 1, 1, 'n', 0};
static const uint8_t subscribe_absent[] = {3, 0, 7, 0, 1, 1, 'n', 1, 'v', 0};
static const uint8_t subscribe_zero[] = {3, 0, 9, 0, 1, 1, 'n', 1, 'v', 1, 4, 0};
static const uint8_t subscribe_wait[] = {3, 0, 9, 0, 1, 1, 'n', 1, 'v', 1, 4, 25};
/* Request ID2, USE_VALUE token(type0, value aa); no token-cache mutation. */
static const uint8_t update_auth[] = {2, 0, 7, 2, 1, 3, 3, 3, 0, 0xaa};
/* Request ID2, TRACK_NAMESPACE_PREFIX=(m). */
static const uint8_t update_prefix[] = {2, 0, 6, 2, 1, 0x34, 1, 1, 'm'};
static const uint8_t update_forward[] = {2, 0, 4, 2, 1, 0x10, 0};

static void destroy_fixture(fixture_t *f)
{
    moqr_bind_destroy(f->bind);
    moqr_core_destroy(f->core);
    moq_simpair_destroy(f->pair);
    memset(f, 0, sizeof(*f));
}

static int create_fixture(fixture_t *f)
{
    int failures = 0;
    memset(f, 0, sizeof(*f));
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_core_relay_cfg_t core_cfg;
    moqr_core_relay_cfg_init_sized(&core_cfg, sizeof(core_cfg), alloc);
    /* NULL authorization hook is the production allow-all policy. */
    moqr_result_t rr = moqr_core_create(&core_cfg, &f->core);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    if (rr != MOQR_OK) return failures;
    moqr_bind_cfg_t bind_cfg;
    moqr_bind_cfg_init_sized(&bind_cfg, sizeof(bind_cfg), alloc);
    bind_cfg.core = f->core;
    bind_cfg.max_conns = 1;
    rr = moqr_bind_create(&bind_cfg, &f->bind);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    if (rr != MOQR_OK) return failures;
    moq_simpair_cfg_t pair_cfg = MOQ_SIMPAIR_CFG_INIT;
    pair_cfg.alloc = alloc;
    pair_cfg.version = MOQ_VERSION_DRAFT_22;
    pair_cfg.seed = UINT64_C(0xD22);
    moq_result_t rc = moq_simpair_create(&pair_cfg, &f->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return failures;
    f->session = moq_simpair_server(f->pair);
    rc = moq_simpair_start(f->pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) return failures;
    rr = moqr_bind_conn_open(f->bind, f->session, MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    if (rr != MOQR_OK) return failures;
    rc = moq_simpair_run_until_quiescent(f->pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(f->session), MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(f->session), MOQ_SESS_ESTABLISHED);
    if (rc != MOQ_OK) return failures;
    rr = moqr_bind_pump(f->bind, 0);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    /* After SETUP, raw peer bytes and drained public actions own the request
     * stream. Do not ask SimPair to route a stream it did not open. */
    moq_event_t event;
    for (unsigned i = 0; i < 32; ++i) {
        if (!moq_session_poll_events(moq_simpair_client(f->pair), &event, 1)) break;
        MOQ_TEST_CHECK_EQ_INT(event.kind, MOQ_EVENT_SETUP_COMPLETE);
        moq_event_cleanup(&event);
        MOQ_TEST_CHECK(i != 31);
    }
    moq_action_t action;
    const size_t n = moq_session_poll_actions(f->session, &action, 1);
    MOQ_TEST_CHECK_EQ_SIZE(n, 0);
    if (n) moq_action_cleanup(&action);
    return failures;
}

/* The expected response types/codes are all one-byte varints. Check the
 * complete frame length and error reason length without a product codec. */
static int observe(fixture_t *f, observation_t *o, const char *phase)
{
    int failures = 0;
    memset(o, 0, sizeof(*o));
    for (unsigned i = 0; i < 64; ++i) {
        moq_action_t action;
        if (!moq_session_poll_actions(f->session, &action, 1)) break;
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            const moq_send_bidi_stream_action_t *s = &action.u.send_bidi_stream;
            MOQ_TEST_CHECK_EQ_U64(s->stream_ref._v, request_ref._v);
            if (s->fin) ++o->fin;
            MOQ_TEST_CHECK(s->len >= 4);
            if (s->len >= 4) {
                const uint8_t *p = s->data;
                const size_t body = ((size_t)p[1] << 8) | p[2];
                MOQ_TEST_CHECK_EQ_SIZE(body + 3, s->len);
                printf("WIRE phase=%s fin=%u bytes=", phase, s->fin ? 1u : 0u);
                for (size_t j = 0; j < s->len && j < 128; ++j) printf("%02x", p[j]);
                printf("\n");
                if (p[0] == 7) {
                    ++o->ok;
                    const uint8_t expected[] = {7, 0, 1, 0};
                    MOQ_TEST_CHECK_EQ_SIZE(s->len, sizeof(expected));
                    if (s->len == sizeof(expected))
                        MOQ_TEST_CHECK(memcmp(p, expected, sizeof(expected)) == 0);
                } else if (p[0] == 5) {
                    ++o->error;
                    o->error_code = p[3];
                    MOQ_TEST_CHECK(s->len >= 6);
                    if (s->len >= 6) {
                        MOQ_TEST_CHECK(p[3] < 128 && p[4] == 0 && p[5] < 128);
                        MOQ_TEST_CHECK_EQ_SIZE((size_t)p[5] + 6, s->len);
                    }
                } else {
                    ++o->other;
                }
            }
        } else if (action.kind == MOQ_ACTION_CLOSE_SESSION) {
            ++o->close;
            o->close_code = action.u.close_session.code;
        } else {
            ++o->other;
        }
        moq_action_cleanup(&action);
        MOQ_TEST_CHECK(i != 63);
    }
    printf("OBS phase=%s ok=%u error=%u code=%u fin=%u close=%u other=%u\n",
        phase, o->ok, o->error, o->error_code, o->fin, o->close, o->other);
    return failures;
}

static int deliver(fixture_t *f, const uint8_t *wire, size_t len, uint64_t now)
{
    int failures = 0;
    moq_result_t rc = moq_session_on_bidi_stream_bytes(f->session, request_ref,
                                                      wire, len, false, now);
    MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    if (rc != MOQ_OK && rc != MOQ_ERR_WOULD_BLOCK) return failures;
    moqr_result_t rr = moqr_bind_pump(f->bind, now);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    if (rc == MOQ_ERR_WOULD_BLOCK) {
        rc = moq_session_on_bidi_stream_bytes(f->session, request_ref,
                                             NULL, 0, false, now);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        rr = moqr_bind_pump(f->bind, now);
        MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    }
    return failures;
}

static bool accepted(const observation_t *o)
{
    return o->ok == 1 && o->error == 0 && o->fin == 0 &&
           o->close == 0 && o->other == 0;
}

static bool not_supported(const observation_t *o)
{
    return o->ok == 0 && o->error == 1 && o->error_code == 3 && o->fin == 1 &&
           o->close == 0 && o->other == 0;
}

static void finish_case(totals_t *t, const char *name, int failures,
                        bool good, bool blocked)
{
    ++t->cases;
    if (failures) {
        t->fixture_errors += (unsigned)failures;
        printf("FIXTURE_ERROR case=%s failures=%d\n", name, failures);
    } else if (blocked) {
        ++t->blocked;
        printf("PREREQUISITE_FAIL case=%s initial_SUBSCRIBE_TRACKS_rejected; UPDATE_unreached\n", name);
    } else if (!good) {
        ++t->behavior_failures;
        printf("BEHAVIOR_FAIL case=%s\n", name);
    } else {
        printf("PASS case=%s\n", name);
    }
}

static void namespace_case(totals_t *t, const char *name,
    const uint8_t *initial, size_t initial_len, const uint8_t *update,
    size_t update_len, bool tracks, bool malformed)
{
    fixture_t f;
    int failures = create_fixture(&f);
    bool good = false, blocked = false;
    observation_t o;
    if (failures) goto done;
    failures += deliver(&f, initial, initial_len, 1000);
    failures += observe(&f, &o, "initial");
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(f.session), MOQ_SESS_ESTABLISHED);
    if (failures) goto done;
    if (!accepted(&o)) {
        /* Only a refused SUBSCRIBE_TRACKS may stop a case here, and it never
         * counts as having reached the REQUEST_UPDATE. */
        MOQ_TEST_CHECK(tracks && not_supported(&o));
        blocked = tracks && update != NULL && not_supported(&o);
        goto done;
    }
    if (update == NULL) {
        good = true;
        goto done;
    }
    failures += deliver(&f, update, update_len, 2000);
    failures += observe(&f, &o, "update");
    if (malformed) {
        good = o.ok == 0 && o.error == 0 && o.fin == 0 && o.close == 1 &&
               o.close_code == 3 && o.other == 0 &&
               moq_session_state(f.session) == MOQ_SESS_CLOSED;
    } else {
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(f.session), MOQ_SESS_ESTABLISHED);
        good = accepted(&o);
        printf("EXPECT case=%s one_UPDATE_OK_without_FIN actual_not_supported=%u\n",
            name, not_supported(&o) ? 1u : 0u);
    }
done:
    destroy_fixture(&f);
    finish_case(t, name, failures, good, blocked);
}

static void rendezvous_case(totals_t *t, const char *name,
                            const uint8_t *wire, size_t len, bool positive)
{
    fixture_t f;
    int failures = create_fixture(&f);
    bool good = false;
    observation_t o;
    if (failures) goto done;
    failures += deliver(&f, wire, len, 1000);
    failures += observe(&f, &o, "no-publisher-admission");
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(f.session), MOQ_SESS_ESTABLISHED);
    if (failures) goto done;
    if (!positive) {
        good = o.ok == 0 && o.error == 1 && o.error_code == 0x10 &&
               o.fin == 1 && o.close == 0 && o.other == 0;
        goto done;
    }
    printf("EXPECT case=%s retained_without_early_response timeout_ms=25\n", name);
    if (o.ok || o.error || o.fin || o.close || o.other) goto done;
    MOQ_TEST_CHECK_EQ_U64(moqr_bind_next_deadline_us(f.bind), 26000);
    moqr_result_t rr = moqr_bind_pump(f.bind, 25999);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    failures += observe(&f, &o, "before-deadline");
    if (o.ok || o.error || o.fin || o.close || o.other) goto done;
    rr = moqr_bind_pump(f.bind, 26000);
    MOQ_TEST_CHECK_EQ_INT(rr, MOQR_OK);
    failures += observe(&f, &o, "at-deadline");
    good = o.ok == 0 && o.error == 1 && o.error_code == 2 &&
           o.fin == 1 && o.close == 0 && o.other == 0;
done:
    destroy_fixture(&f);
    finish_case(t, name, failures, good, false);
}

int main(int argc, char **argv)
{
    const bool controls = argc == 2 && strcmp(argv[1], "--controls") == 0;
    const bool features = argc == 2 && strcmp(argv[1], "--features") == 0;
    if (!controls && !features) {
        fprintf(stderr, "expected --controls or --features\n");
        return 2;
    }
    totals_t t = {0};
    if (controls) {
        namespace_case(&t, "announce-initial", announce, sizeof(announce),
                       NULL, 0, false, false);
        namespace_case(&t, "namespace-initial", namespace_sub, sizeof(namespace_sub),
                       NULL, 0, false, false);
        namespace_case(&t, "announce-wrong-context", announce, sizeof(announce),
                       update_forward, sizeof(update_forward), false, true);
        rendezvous_case(&t, "rendezvous-absent", subscribe_absent,
                        sizeof(subscribe_absent), false);
        rendezvous_case(&t, "rendezvous-zero", subscribe_zero,
                        sizeof(subscribe_zero), false);
    } else {
        namespace_case(&t, "tracks-initial", tracks_sub, sizeof(tracks_sub),
                       NULL, 0, true, false);
        namespace_case(&t, "announce-auth-update", announce, sizeof(announce),
                       update_auth, sizeof(update_auth), false, false);
        namespace_case(&t, "namespace-prefix-update", namespace_sub, sizeof(namespace_sub),
                       update_prefix, sizeof(update_prefix), false, false);
        namespace_case(&t, "tracks-forward-update", tracks_sub, sizeof(tracks_sub),
                       update_forward, sizeof(update_forward), true, false);
        rendezvous_case(&t, "rendezvous-positive", subscribe_wait,
                        sizeof(subscribe_wait), true);
    }
    printf("SUMMARY cases=%u behavior_failures=%u blocked=%u fixture_errors=%u\n",
           t.cases, t.behavior_failures, t.blocked, t.fixture_errors);
    if (t.fixture_errors) return 2;
    return t.behavior_failures || t.blocked ? 1 : 0;
}
