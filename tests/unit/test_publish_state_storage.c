/* Fixed notification records must not depend on the variable byte arena. */
#include <moq/moq.h>
#include "test_alloc_support.h"
#include "test_binding_support.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static bool check(bool value, const char *expression, int line)
{
    ++checks;
    if (!value) {
        ++failures;
        fprintf(stderr, "FAIL:%d: %s\n", line, expression);
    }
    return value;
}
#define CHECK(x) check(!!(x), #x, __LINE__)
#define NEED(x) do { if (!CHECK(x)) goto done; } while (0)

typedef struct fixture {
    test_alloc_state_t memory;
    moq_alloc_t alloc;
    moq_session_t *session;
    moq_subscription_t subs[2];
    moq_stream_ref_t refs[2];
    moq_location_filter_cfg_t locations[2];
} fixture_t;

static size_t drain_actions(moq_session_t *s, moq_stream_ref_t *ref)
{
    moq_action_t a;
    size_t n = 0;
    while (moq_session_poll_actions(s, &a, 1)) {
        if (ref && a.kind == MOQ_ACTION_OPEN_BIDI_STREAM)
            *ref = a.u.open_bidi_stream.stream_ref;
        ++n;
        moq_action_cleanup(&a);
    }
    return n;
}

static bool setup(fixture_t *f, uint32_t scratch)
{
    memset(f, 0, sizeof(*f));
    f->alloc = test_allocator(&f->memory);
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), &f->alloc, MOQ_PERSPECTIVE_CLIENT);
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_events = 2;
    cfg.output_scratch_size = scratch;
    if (!CHECK(moq_session_create(&cfg, 0, &f->session) == MOQ_OK)) return false;
    moq_session_t *s = f->session;
    if (!CHECK(test_bind_if_required(s) == MOQ_OK) ||
        !CHECK(moq_session_start(s, 0) == MOQ_OK)) return false;
    drain_actions(s, NULL);
    /* D22 SETUP with an empty options body. */
    static const uint8_t setup_wire[] = {0xaf, 0, 0, 0};
    if (!CHECK(moq_session_on_control_bytes(s, setup_wire,
        sizeof(setup_wire), 1) == MOQ_OK)) return false;
    moq_event_t e;
    if (!CHECK(moq_session_poll_events(s, &e, 1) == 1)) return false;
    bool valid = e.kind == MOQ_EVENT_SETUP_COMPLETE;
    moq_event_cleanup(&e);
    if (!CHECK(valid && moq_session_state(s) == MOQ_SESS_ESTABLISHED)) return false;
    drain_actions(s, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        moq_location_filter_cfg_init(&f->locations[i]);
        if (i != 0) {
            f->locations[i].kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
            f->locations[i].relative_group_offset = 3;
        }
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init_sized(&sc, sizeof(sc));
        sc.track_name = i == 0 ? MOQ_BYTES_LITERAL("a") : MOQ_BYTES_LITERAL("b");
        sc.location = &f->locations[i];
        if (!CHECK(moq_session_subscribe(s, &sc, 2, &f->subs[i]) == MOQ_OK)) return false;
        drain_actions(s, &f->refs[i]);
        if (!CHECK(f->refs[i]._v != 0)) return false;
        /* SUBSCRIBE_OK with alias1/2 and no optional parameters. */
        uint8_t ack[] = {4, 0, 2, (uint8_t)(i + 1), 0};
        if (!CHECK(moq_session_on_bidi_stream_bytes(s, f->refs[i], ack,
            sizeof(ack), false, 3) == MOQ_OK)) return false;
        if (!CHECK(moq_session_poll_events(s, &e, 1) == 1)) return false;
        valid = e.kind == MOQ_EVENT_SUBSCRIBE_OK &&
            e.u.subscribe_ok.sub._opaque == f->subs[i]._opaque;
        moq_event_cleanup(&e);
        if (!CHECK(valid)) return false;
        drain_actions(s, NULL);
    }
    return CHECK(moq_session_poll_events(s, &e, 1) == 0);
}

static moq_result_t notify(fixture_t *f, unsigned owner, uint64_t now)
{
    static const uint8_t none[] = {0x22, 0, 3, 1, 0x21, 0};
    static const uint8_t relative[] = {0x22, 0, 4, 1, 0x21, 1, 3};
    return moq_session_on_bidi_stream_bytes(f->session, f->refs[owner],
        owner ? relative : none, owner ? sizeof(relative) : sizeof(none),
        false, now);
}

static bool observation(fixture_t *f, unsigned owner, moq_event_t *event)
{
    if (!CHECK(moq_session_poll_events(f->session, event, 1) == 1)) return false;
    const moq_publish_state_event_t *state = &event->u.publish_state;
    bool valid = event->kind == MOQ_EVENT_PUBLISH_STATE &&
        event->detail_size >= sizeof(*state) &&
        state->family == MOQ_REQUEST_FAMILY_SUBSCRIBE &&
        state->handle.subscription._opaque == f->subs[owner]._opaque &&
        state->has_filter && !state->has_forward && !state->has_largest &&
        state->location != NULL &&
        (uintptr_t)state->location % _Alignof(moq_location_filter_cfg_t) == 0 &&
        memcmp(state->location, &f->locations[owner], sizeof(f->locations[owner])) == 0 &&
        moq_session_borrow_valid(f->session, event->borrow_epoch);
    moq_event_cleanup(event);
    return CHECK(valid);
}

static void row(uint32_t scratch, bool short_poll)
{
    unsigned before = failures;
    fixture_t f;
    NEED(setup(&f, scratch));
    moq_session_t *s = f.session;
    /* One queued record survives advances while retired records are recycled.
     * Distinct lawful locations expose an overwrite, not just pointer reuse. */
    NEED(notify(&f, 0, 4) == MOQ_OK);
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        unsigned first = cycle % 2, second = 1 - first;
        uint64_t now = 5 + cycle;
        NEED(notify(&f, second, now) == MOQ_OK);
        moq_event_t a, b;
        if (short_poll) {
            uint8_t too_short[offsetof(moq_event_t, u) - 1];
            memset(too_short, 0xa5, sizeof(too_short));
            uint8_t saved[sizeof(too_short)];
            memcpy(saved, too_short, sizeof(saved));
            size_t count = 99;
            NEED(moq_session_poll_events_ex(s, too_short, 1, sizeof(too_short),
                &count) == MOQ_ERR_INVAL);
            NEED(count == 0 && memcmp(too_short, saved, sizeof(saved)) == 0);
            uint8_t prefix[offsetof(moq_event_t, u)];
            NEED(moq_session_poll_events_ex(s, prefix, 1, sizeof(prefix), &count) == MOQ_OK);
            NEED(count == 1);
            uint32_t kind;
            memcpy(&kind, prefix + offsetof(moq_event_t, kind), sizeof(kind));
            NEED(kind == MOQ_EVENT_PUBLISH_STATE);
        } else {
            NEED(observation(&f, first, &a));
            /* Polling a second record must not invalidate the first borrow. */
            NEED(observation(&f, second, &b));
            NEED(a.u.publish_state.location != b.u.publish_state.location);
            NEED(moq_session_borrow_valid(s, a.borrow_epoch));
            NEED(memcmp(a.u.publish_state.location, &f.locations[first],
                sizeof(f.locations[first])) == 0);
            NEED(memcmp(b.u.publish_state.location, &f.locations[second],
                sizeof(f.locations[second])) == 0);
            NEED(moq_session_poll_events(s, &b, 1) == 0);
            NEED(moq_session_tick(s, now) == MOQ_OK);
            NEED(!moq_session_borrow_valid(s, a.borrow_epoch));
            NEED(notify(&f, second, now) == MOQ_OK);
        }
        /* In the prefix row, the second queued record remains untouched.
         * The next advancing ingress may recycle only the polled record. */
    }
    moq_event_t last;
    NEED(observation(&f, 0, &last));
    NEED(moq_session_poll_events(s, &last, 1) == 0);
    NEED(drain_actions(s, NULL) == 0);
    NEED(moq_session_state(s) == MOQ_SESS_ESTABLISHED);
done:
    if (f.session) moq_session_destroy(f.session);
    CHECK(f.memory.balance == 0);
    printf("%s: snapshot storage scratch=%u short=%d\n",
        failures == before ? "PASS" : "FAIL", scratch, short_poll);
}

static void failed_construction(void)
{
    test_alloc_state_t memory = {0};
    moq_alloc_t alloc = test_allocator(&memory);
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), &alloc, MOQ_PERSPECTIVE_CLIENT);
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_events = UINT32_MAX;
    moq_session_t *s = NULL;
    CHECK(moq_session_create(&cfg, 0, &s) == MOQ_ERR_INVAL);
    CHECK(s == NULL && memory.balance == 0);
    if (s) moq_session_destroy(s);
}

int main(void)
{
    row(32, false);
    row(32, true);
    row(48, false);
    row(48, true);
    failed_construction();
    printf("5 storage cases, %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
