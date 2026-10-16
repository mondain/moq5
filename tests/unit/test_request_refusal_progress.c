/* A legal one-action queue must finish a request-local rejection without
 * requiring two terminal actions to fit simultaneously. Literal D22 inputs. */
#include <moq/moq.h>
#include <moq/sim.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    const char *name;
    moq_simpair_t *pair;
    moq_session_t *session;
    moq_stream_ref_t ref;
    uint64_t handle;
    uint64_t now;
    moq_alloc_t alloc;
    size_t live;
    bool publish;
    int failures;
} fixture_t;

static void *count_alloc(size_t size, void *ctx)
{
    fixture_t *f = ctx;
    void *p = malloc(size);
    if (p) ++f->live;
    return p;
}

static void count_free(void *p, size_t size, void *ctx)
{
    fixture_t *f = ctx;
    (void)size;
    if (p) --f->live;
    free(p);
}

static void *count_realloc(void *p, size_t old_size, size_t size, void *ctx)
{
    if (!p) return count_alloc(size, ctx);
    if (!size) { count_free(p, old_size, ctx); return NULL; }
    return realloc(p, size);
}

static bool check(fixture_t *f, bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s\n", f->name, what);
        ++f->failures;
    }
    return ok;
}

static bool discard_events(fixture_t *f, moq_session_t *s)
{
    for (unsigned i = 0; i < 32; ++i) {
        moq_event_t event = {0};
        size_t count = 0;
        if (!check(f, moq_session_poll_events_ex(s, &event, 1,
            sizeof(event), &count) == MOQ_OK, "event.poll")) return false;
        if (!count) return true;
        moq_event_cleanup(&event);
    }
    return check(f, false, "event.drain.finite");
}

static bool poll_action(fixture_t *f, unsigned expected, bool present)
{
    moq_action_t action = {0};
    size_t count = 0;
    if (!check(f, moq_session_poll_actions_ex(f->session, &action, 1,
        sizeof(action), &count) == MOQ_OK, "action.poll")) return false;
    bool ok = (count == (present ? 1u : 0u));
    if (count && present) {
        ok = ok && action.kind == expected;
        if (ok && expected == MOQ_ACTION_SEND_BIDI_STREAM) {
            static const uint8_t error[] = {5, 0, 3, 3, 0, 0};
            ok = action.u.send_bidi_stream.stream_ref._v == f->ref._v &&
                action.u.send_bidi_stream.fin &&
                action.u.send_bidi_stream.len == sizeof(error) &&
                action.u.send_bidi_stream.data &&
                memcmp(action.u.send_bidi_stream.data, error, sizeof(error)) == 0;
        } else if (ok && expected == MOQ_ACTION_STOP_BIDI_STREAM) {
            ok = action.u.stop_bidi_stream.stream_ref._v == f->ref._v &&
                action.u.stop_bidi_stream.error_code == 1;
        }
    }
    if (count) moq_action_cleanup(&action);
    return check(f, ok, present ? "action.exact.kind.ref.bytes.fin" : "action.none");
}

static moq_result_t reject(fixture_t *f)
{
    if (f->publish) {
        moq_reject_publish_cfg_t cfg;
        moq_reject_publish_cfg_init(&cfg);
        cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
        return moq_session_reject_publish(f->session,
            (moq_publication_t){f->handle}, &cfg, f->now++);
    }
    moq_reject_subscribe_cfg_t cfg;
    moq_reject_subscribe_cfg_init(&cfg);
    cfg.error_code = MOQ_REQUEST_ERROR_NOT_SUPPORTED;
    return moq_session_reject_subscribe(f->session,
        (moq_subscription_t){f->handle}, &cfg, f->now++);
}

typedef enum terminal_order {
    FIN_AFTER_OUTPUT,
    RESET_AFTER_OUTPUT,
    FIN_WITH_UPDATE,
    FIN_BEFORE_OUTPUT,
    RESET_BEFORE_OUTPUT,
    STOP_BEFORE_OUTPUT_THEN_FIN,
    STOP_BEFORE_OUTPUT_THEN_RESET,
    TERMINAL_ORDER_COUNT
} terminal_order_t;

static int run_case(uint32_t action_cap, bool publish, bool peer_server,
                    terminal_order_t order)
{
    bool peer_fin = order == FIN_WITH_UPDATE;
    bool before = order == FIN_BEFORE_OUTPUT || order == RESET_BEFORE_OUTPUT;
    bool stop = order == STOP_BEFORE_OUTPUT_THEN_FIN ||
                order == STOP_BEFORE_OUTPUT_THEN_RESET;
    bool reset = order == RESET_AFTER_OUTPUT || order == RESET_BEFORE_OUTPUT ||
                 order == STOP_BEFORE_OUTPUT_THEN_RESET;
    char name[96];
    snprintf(name, sizeof(name), "%s.peer-%s.cap%u.order%d",
        publish ? "publish" : "subscribe", peer_server ? "server" : "client",
        action_cap, (int)order);
    fixture_t f = {.name = name, .publish = publish, .now = 100,
                   .ref = moq_stream_ref_from_u64(0x7f10)};
    f.alloc = (moq_alloc_t){.ctx = &f, .alloc = count_alloc,
                          .realloc = count_realloc, .free = count_free};
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &f.alloc;
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_actions = action_cap;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    if (!check(&f, moq_simpair_create(&cfg, &f.pair) == MOQ_OK && f.pair,
        "pair.create")) goto done;
    if (!check(&f, moq_simpair_start(f.pair) == MOQ_OK, "pair.start") ||
        !check(&f, moq_simpair_run_until_quiescent(f.pair, 16, NULL) == MOQ_OK,
               "pair.pump")) goto done;
    moq_session_t *client = moq_simpair_client(f.pair);
    moq_session_t *server = moq_simpair_server(f.pair);
    f.session = peer_server ? client : server;
    if (!check(&f, moq_session_state(f.session) == MOQ_SESS_ESTABLISHED &&
        moq_session_version(f.session) == MOQ_VERSION_DRAFT_22, "pair.d22.established") ||
        !discard_events(&f, client) || !discard_events(&f, server) ||
        !poll_action(&f, 0, false)) goto done;

    /* Namespace n, track v, no parameters; PUBLISH additionally names alias1. */
    uint8_t sub[] = {3, 0, 7, 0, 1, 1, 'n', 1, 'v', 0};
    uint8_t pub[] = {0x1d, 0, 8, 0, 1, 1, 'n', 1, 'v', 1, 0};
    uint8_t id = peer_server ? 1 : 0;
    sub[3] = pub[3] = id;
    if (!check(&f, moq_session_on_bidi_stream_bytes(f.session, f.ref,
        publish ? pub : sub, publish ? sizeof(pub) : sizeof(sub), false, f.now++)
        == MOQ_OK, "request.ingress")) goto done;
    moq_event_t event = {0};
    size_t count = 0;
    if (!check(&f, moq_session_poll_events_ex(f.session, &event, 1,
        sizeof(event), &count) == MOQ_OK && count == 1, "request.event")) goto done;
    bool expected_event = event.kind ==
        (publish ? MOQ_EVENT_PUBLISH_REQUEST : MOQ_EVENT_SUBSCRIBE_REQUEST);
    if (expected_event)
        f.handle = publish ? event.u.publish_request.pub._opaque :
                             event.u.subscribe_request.sub._opaque;
    moq_event_cleanup(&event);
    if (!check(&f, expected_event && f.handle, "request.event.kind.handle")) goto done;

    uint8_t update[] = {2, 0, 2, 0, 0, 0x0b, 0, 3, 0, 0, 0};
    update[3] = (uint8_t)(id + 2);
    size_t update_len = publish && peer_fin ? sizeof(update) : 5;
    if (!check(&f, moq_session_on_bidi_stream_bytes(f.session, f.ref,
        update, update_len, peer_fin, f.now++) == MOQ_ERR_WOULD_BLOCK,
        "update.retained")) goto done;
    if (!check(&f, reject(&f) == MOQ_OK, "rejection.progress.legal.action.capacity"))
        goto done;
    if (before || stop) {
        moq_result_t rc = stop
            ? moq_session_on_bidi_stream_stop(f.session, f.ref, 1, f.now++)
            : reset
                ? moq_session_on_bidi_stream_reset(f.session, f.ref, 1, f.now++)
                : moq_session_on_bidi_stream_bytes(f.session, f.ref, NULL, 0, true,
                                                    f.now++);
        if (!check(&f, rc == MOQ_OK, "peer.signal.before.output") ||
            !check(&f, moq_session_has_transport_stream(f.session, f.ref) == stop,
                   "directional.owner.before.output")) goto done;
    }
    if (!poll_action(&f, MOQ_ACTION_SEND_BIDI_STREAM, true)) goto done;
    /* FIN/RESET makes an unqueued STOP moot; an already queued STOP remains
     * observable. Peer STOP alone never cancels the receive-side obligation. */
    bool expect_stop = !peer_fin && !(before && action_cap == 1);
    if (expect_stop && !poll_action(&f, MOQ_ACTION_STOP_BIDI_STREAM, true)) goto done;
    if (!poll_action(&f, 0, false)) goto done;
    if (!check(&f, moq_session_state(f.session) == MOQ_SESS_ESTABLISHED,
               "session.still.established")) goto done;
    if (!peer_fin && !before) {
        if (!check(&f, moq_session_has_transport_stream(f.session, f.ref),
                   "drain.retained.until.peer.receive.terminal")) goto done;
        moq_result_t rc = reset
            ? moq_session_on_bidi_stream_reset(f.session, f.ref, 1, f.now++)
            : moq_session_on_bidi_stream_bytes(f.session, f.ref, NULL, 0, true, f.now++);
        if (!check(&f, rc == MOQ_OK, "peer.receive.terminal")) goto done;
    }
    check(&f, !moq_session_has_transport_stream(f.session, f.ref), "owner.retired");
    check(&f, reject(&f) == MOQ_ERR_STALE_HANDLE, "old.handle.stale");
    (void)poll_action(&f, 0, false);
    event = (moq_event_t){0};
    count = 0;
    check(&f, moq_session_poll_events_ex(f.session, &event, 1, sizeof(event), &count)
          == MOQ_OK && count == 0, "no.extra.event");
    if (count) moq_event_cleanup(&event);
done:
    if (f.pair) moq_simpair_destroy(f.pair);
    check(&f, f.live == 0, "allocator.balance.zero");
    return f.failures;
}

int main(int argc, char **argv)
{
    bool controls = argc == 2 && strcmp(argv[1], "--controls") == 0;
    if (argc > 1 && !controls) return 2;
    int failures = 0;
    unsigned cases = 0;
    for (unsigned cap = controls ? 2 : 1; cap <= 2; ++cap)
        for (unsigned pub = 0; pub < 2; ++pub)
            for (unsigned peer = 0; peer < 2; ++peer)
                for (unsigned terminal = 0; terminal < TERMINAL_ORDER_COUNT; ++terminal) {
                    failures += run_case(cap, pub != 0, peer != 0,
                                          (terminal_order_t)terminal);
                    ++cases;
                }
    printf("%s: request_refusal_progress (%u cases, %d failures)\n",
        failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
