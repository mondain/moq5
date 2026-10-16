/* Local state notifications observe acknowledged subscriber intent. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_alloc_support.h"
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
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

typedef struct pair {
    moq_simpair_t *sim;
    moq_session_t *publisher, *subscriber;
    uint64_t pub_handle, sub_handle;
    bool publish;
    test_alloc_state_t memory;
    moq_alloc_t alloc;
} pair_t;

static const moq_bytes_t part = {
    .data = (const uint8_t *)"permission", .len = sizeof("permission") - 1
};

static unsigned drain(moq_session_t *s)
{
    moq_event_t e;
    unsigned n = 0;
    while (moq_session_poll_events(s, &e, 1)) {
        ++n;
        moq_event_cleanup(&e);
    }
    return n;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1)) {
        if (e.kind == kind) { *out = e; return true; }
        moq_event_cleanup(&e);
    }
    return false;
}

static moq_location_filter_cfg_t location(unsigned kind)
{
    moq_location_filter_cfg_t f;
    moq_location_filter_cfg_init(&f);
    switch (kind) {
    case 0: break;
    case 1: f.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP; break;
    case 2:
        f.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP;
        f.relative_group_offset = 3;
        break;
    case 3: f.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT; break;
    default:
        f.kind = kind == 4 ? MOQ_LOCATION_FILTER_ABSOLUTE_START :
                 kind == 5 ? MOQ_LOCATION_FILTER_GROUP_END : MOQ_LOCATION_FILTER_OBJECT_END;
        f.start_group = 2;
        f.start_object = 1;
        if (kind >= 5) f.end_group = 10;
        if (kind == 6) f.end_object = 7;
        break;
    }
    return f;
}

static uint64_t now(const pair_t *p) { return moq_simpair_now_us(p->sim); }
static bool pump(pair_t *p)
{
    return CHECK(moq_simpair_run_until_quiescent(p->sim, 32, NULL) == MOQ_OK);
}

static bool setup(pair_t *p, bool publish, moq_version_t version,
                  const moq_location_filter_cfg_t *filter, bool forward, bool defer)
{
    memset(p, 0, sizeof(*p));
    p->publish = publish;
    p->alloc = test_allocator(&p->memory);
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = &p->alloc;
    cfg.version = version;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 64;
    cfg.client_defer_subscription_updates = cfg.server_defer_subscription_updates = defer;
    if (!CHECK(moq_simpair_create(&cfg, &p->sim) == MOQ_OK)) return false;
    if (!CHECK(moq_simpair_start(p->sim) == MOQ_OK) || !pump(p)) return false;
    moq_session_t *c = moq_simpair_client(p->sim), *s = moq_simpair_server(p->sim);
    p->publisher = publish ? c : s;
    p->subscriber = publish ? s : c;
    drain(c); drain(s);
    moq_event_t event;
    if (publish) {
        moq_publish_cfg_t pc;
        moq_publish_cfg_init_sized(&pc, sizeof(pc));
        pc.track_namespace = (moq_namespace_t){&part, 1};
        pc.track_name = MOQ_BYTES_LITERAL("v");
        pc.location = filter;
        pc.has_forward = true;
        pc.forward = forward;
        moq_publication_t h;
        if (!CHECK(moq_session_publish(c, &pc, now(p), &h) == MOQ_OK)) return false;
        p->pub_handle = h._opaque;
        if (!pump(p) || !CHECK(take(s, MOQ_EVENT_PUBLISH_REQUEST, &event))) return false;
        h = event.u.publish_request.pub;
        p->sub_handle = h._opaque;
        moq_event_cleanup(&event);
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init(&ac);
        if (!CHECK(moq_session_accept_publish(s, h, &ac, now(p)) == MOQ_OK)) return false;
    } else {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init_sized(&sc, sizeof(sc));
        sc.track_namespace = (moq_namespace_t){&part, 1};
        sc.track_name = MOQ_BYTES_LITERAL("v");
        sc.location = filter;
        sc.has_forward = true;
        sc.forward = forward;
        moq_subscription_t h;
        if (!CHECK(moq_session_subscribe(c, &sc, now(p), &h) == MOQ_OK)) return false;
        p->sub_handle = h._opaque;
        if (!pump(p) || !CHECK(take(s, MOQ_EVENT_SUBSCRIBE_REQUEST, &event))) return false;
        h = event.u.subscribe_request.sub;
        p->pub_handle = h._opaque;
        moq_event_cleanup(&event);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        if (!CHECK(moq_session_accept_subscribe(s, h, &ac, now(p)) == MOQ_OK)) return false;
    }
    if (!pump(p)) return false;
    drain(c); drain(s);
    return true;
}

static void cleanup(pair_t *p)
{
    if (p->sim) moq_simpair_destroy(p->sim);
    CHECK(p->memory.balance == 0);
}

/* Read-only scans keep the fixture linkable with the shipping core. */
static const void *owner(const pair_t *p, size_t *size)
{
    moq_session_t *s = p->publisher;
    if (p->publish) {
        *size = sizeof(moq_pub_entry_t);
        for (size_t i = 0; i < s->pub_cap; ++i)
            if (s->publishes[i].state != MOQ_PUB_FREE &&
                s->publishes[i].handle._opaque == p->pub_handle) return &s->publishes[i];
    } else {
        *size = sizeof(moq_sub_entry_t);
        for (size_t i = 0; i < s->sub_cap; ++i)
            if (s->subs[i].state != MOQ_SUB_FREE &&
                s->subs[i].handle._opaque == p->pub_handle) return &s->subs[i];
    }
    return NULL;
}

static moq_result_t notify(pair_t *p, const moq_publish_state_cfg_t *cfg, bool wrong_role)
{
    moq_session_t *s = wrong_role ? p->subscriber : p->publisher;
    uint64_t h = wrong_role ? p->sub_handle : p->pub_handle;
    return p->publish ? moq_session_notify_publication_state(s,
        (moq_publication_t){h}, cfg, now(p)) : moq_session_notify_subscription_state(s,
        (moq_subscription_t){h}, cfg, now(p));
}

static bool checked_notify(pair_t *p, const moq_publish_state_cfg_t *cfg,
                           moq_result_t expected)
{
    moq_session_t *s = p->publisher;
    if (!CHECK(moq_session_tick(s, now(p)) == MOQ_OK)) return false;
    size_t size = 0;
    const void *entry = owner(p, &size);
    if (!CHECK(entry != NULL)) return false;
    union { moq_sub_entry_t sub; moq_pub_entry_t pub; } saved;
    memcpy(&saved, entry, size);
    size_t actions = s->action_tail - s->action_head, send = s->send_len;
    size_t events = s->event_tail - s->event_head;
    uint64_t epoch = s->borrow_epoch;
    moq_result_t rc = notify(p, cfg, false);
    bool unchanged = memcmp(entry, &saved, size) == 0;
    size_t added = (s->action_tail - s->action_head) - actions;
    if (rc != expected || !unchanged)
        fprintf(stderr, "observed %s rc=%d actions_added=%zu owner_changed=%d\n",
            p->publish ? "publish" : "subscribe", rc, added, !unchanged);
    bool ok = CHECK(rc == expected);
    ok &= CHECK(unchanged); /* includes saved window, pending/owed UPDATE and stream budget */
    ok &= CHECK(s->event_tail - s->event_head == events);
    ok &= CHECK(added == (expected == MOQ_OK ? 1u : 0u));
    if (expected != MOQ_OK) ok &= CHECK(s->send_len == send);
    if (expected == MOQ_ERR_INVAL || expected == MOQ_ERR_ABI_MISMATCH ||
        expected == MOQ_ERR_UNSUPPORTED) ok &= CHECK(s->borrow_epoch == epoch);
    return ok;
}

static bool observed(pair_t *p, const moq_publish_state_cfg_t *cfg,
                     const moq_location_filter_cfg_t *filter, bool largest)
{
    if (!pump(p)) return false;
    moq_event_t e;
    if (!CHECK(moq_session_poll_events(p->subscriber, &e, 1) == 1)) return false;
    bool ok = CHECK(e.kind == MOQ_EVENT_PUBLISH_STATE);
    ok &= CHECK(e.u.publish_state.family == (p->publish ? MOQ_REQUEST_FAMILY_PUBLISH :
                                                        MOQ_REQUEST_FAMILY_SUBSCRIBE));
    ok &= CHECK(e.u.publish_state.handle.raw == p->sub_handle);
    ok &= CHECK(e.u.publish_state.has_forward == cfg->has_forward);
    if (cfg->has_forward) ok &= CHECK(e.u.publish_state.forward == cfg->forward);
    ok &= CHECK(e.u.publish_state.has_filter == (filter != NULL));
    if (filter) {
        ok &= CHECK(e.u.publish_state.location != NULL);
        if (e.u.publish_state.location)
            ok &= CHECK(memcmp(e.u.publish_state.location, filter, sizeof(*filter)) == 0);
    } else ok &= CHECK(e.u.publish_state.location == NULL);
    ok &= CHECK(e.u.publish_state.has_largest == largest);
    if (largest) {
        ok &= CHECK(e.u.publish_state.largest_group == 20);
        ok &= CHECK(e.u.publish_state.largest_object == 4);
    }
    moq_event_cleanup(&e);
    ok &= CHECK(drain(p->subscriber) == 0 && drain(p->publisher) == 0);
    ok &= CHECK(p->subscriber->action_head == p->subscriber->action_tail);
    ok &= CHECK(moq_session_state(p->publisher) == MOQ_SESS_ESTABLISHED &&
                moq_session_state(p->subscriber) == MOQ_SESS_ESTABLISHED);
    return ok;
}

static bool update(pair_t *p, const moq_location_filter_cfg_t *filter, bool forward)
{
    moq_result_t rc;
    if (p->publish) {
        moq_publication_update_cfg_t u;
        moq_publication_update_cfg_init(&u);
        u.location = filter;
        u.has_forward = true; u.forward = forward;
        rc = moq_session_update_publication(p->subscriber,
            (moq_publication_t){p->sub_handle}, &u, now(p));
    } else {
        moq_subscription_update_cfg_t u;
        moq_subscription_update_cfg_init_sized(&u, sizeof(u));
        u.location = filter;
        u.has_forward = true; u.forward = forward;
        rc = moq_session_update_subscription(p->subscriber,
            (moq_subscription_t){p->sub_handle}, &u, now(p));
    }
    return CHECK(rc == MOQ_OK) && pump(p);
}

static bool acknowledged(pair_t *p)
{
    moq_event_t e;
    if (!CHECK(take(p->subscriber, p->publish ? MOQ_EVENT_PUBLICATION_UPDATE_OK :
                                               MOQ_EVENT_SUBSCRIPTION_UPDATE_OK, &e))) return false;
    moq_event_cleanup(&e);
    drain(p->publisher); drain(p->subscriber);
    return true;
}

static void refusal_row(bool publish, unsigned fields)
{
    pair_t p;
    moq_location_filter_cfg_t bounded = location(5), none = location(0);
    if (!setup(&p, publish, MOQ_VERSION_DRAFT_22, &bounded, false, false)) goto done;
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    cfg.has_forward = (fields & 1) != 0; cfg.forward = true;
    cfg.filter = fields & 2 ? &none : NULL;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    NEED(update(&p, &none, true) && acknowledged(&p));
    NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(observed(&p, &cfg, cfg.filter, false));
done:
    cleanup(&p);
}

static void equal_row(bool publish, unsigned kind)
{
    pair_t p;
    moq_location_filter_cfg_t f = location(kind), noisy = f;
    if (!setup(&p, publish, MOQ_VERSION_DRAFT_22, &f, true, false)) goto done;
    /* A newer Largest must not rebase an acknowledged relative/Next Object window. */
    moq_namespace_t ns = {&part, 1};
    NEED(moq_session_note_object_published(p.publisher, &ns, MOQ_BYTES_LITERAL("v"),
        20, 4) == MOQ_OK);
    if (kind <= 3) {
        noisy.start_group = 999; noisy.start_object = 998;
        noisy.end_group = 997; noisy.end_object = 996;
        if (kind == 0 || kind == 3) noisy.relative_group_offset = 995;
    } else {
        noisy.relative_group_offset = 995;
        if (kind == 4) noisy.end_group = 997;
        if (kind != 6) noisy.end_object = 996;
    }
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    cfg.filter = &noisy; /* inactive coordinates are ignored, not compared as bytes */
    cfg.forward = false; /* absent FORWARD must stay unchanged */
    NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(observed(&p, &cfg, &f, true));
    cfg.filter = NULL;
    cfg.has_forward = true; cfg.forward = true;
    NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(observed(&p, &cfg, NULL, true));
    moq_location_filter_cfg_t different = f;
    if (kind == 0) different.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT;
    else if (kind <= 2) ++different.relative_group_offset;
    else if (kind == 3) different.kind = MOQ_LOCATION_FILTER_NONE;
    else if (kind == 4) ++different.start_object;
    else if (kind == 5) --different.end_group;
    else --different.end_object;
    cfg.filter = &different;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
done:
    cleanup(&p);
}

static void pending_row(bool publish)
{
    pair_t p;
    moq_location_filter_cfg_t bounded = location(5), none = location(0);
    if (!setup(&p, publish, MOQ_VERSION_DRAFT_22, &bounded, false, true)) goto done;
    NEED(update(&p, &none, true));
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    cfg.has_forward = true; cfg.forward = true; cfg.filter = &none;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    cfg.forward = false; cfg.filter = &bounded;
    NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(pump(&p));
    moq_event_t e;
    NEED(take(p.subscriber, MOQ_EVENT_PUBLISH_STATE, &e));
    moq_event_cleanup(&e);
    NEED(drain(p.subscriber) == 0); /* notification did not answer the UPDATE */
    uint64_t id = 0;
    moq_result_t rc = publish ? moq_session_publication_owed_update(p.publisher,
        (moq_publication_t){p.pub_handle}, &id, NULL, NULL) :
        moq_session_subscription_owed_update(p.publisher,
        (moq_subscription_t){p.pub_handle}, &id, NULL, NULL);
    NEED(rc == MOQ_OK);
    moq_accept_subscription_update_cfg_t accept;
    moq_accept_subscription_update_cfg_init(&accept);
    accept.update_request_id = id;
    rc = publish ? moq_session_accept_publication_update(p.publisher,
        (moq_publication_t){p.pub_handle}, &accept, now(&p)) :
        moq_session_accept_subscription_update(p.publisher,
        (moq_subscription_t){p.pub_handle}, &accept, now(&p));
    NEED(rc == MOQ_OK && pump(&p) && acknowledged(&p));
    cfg.forward = true; cfg.filter = &none;
    NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(observed(&p, &cfg, &none, false));
done:
    cleanup(&p);
}

static void validation_row(bool publish)
{
    pair_t p;
    moq_location_filter_cfg_t f = location(5), bad = f;
    if (!setup(&p, publish, MOQ_VERSION_DRAFT_22, &f, false, false)) goto done;
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    cfg.has_forward = true;
    cfg.struct_size = MOQ_PUBLISH_STATE_CFG_V0_SIZE - 1;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    cfg.struct_size = sizeof(cfg);
    cfg.filter = &bad;
    bad.struct_size = MOQ_LOCATION_FILTER_CFG_V0_SIZE - 1;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_ABI_MISMATCH));
    bad = f; bad.kind = 99;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    bad = f; bad.end_group = 1;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL));
    cfg.filter = &f;
    NEED(notify(&p, &cfg, true) == MOQ_ERR_WRONG_STATE);
    NEED(drain(p.publisher) == 0 && drain(p.subscriber) == 0);
    for (size_t n = 0; n < p.publisher->action_cap; ++n)
        NEED(checked_notify(&p, &cfg, MOQ_OK));
    NEED(checked_notify(&p, &cfg, MOQ_ERR_WOULD_BLOCK));
    cfg.forward = true;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_INVAL)); /* permission before queue pressure */
done:
    cleanup(&p);
}

static void legacy_row(bool publish, moq_version_t version)
{
    pair_t p;
    if (!setup(&p, publish, version, NULL, true, false)) goto done;
    moq_publish_state_cfg_t cfg;
    moq_publish_state_cfg_init(&cfg);
    cfg.has_forward = true; cfg.forward = false;
    NEED(checked_notify(&p, &cfg, MOQ_ERR_UNSUPPORTED));
    NEED(pump(&p));
    NEED(drain(p.publisher) == 0 && drain(p.subscriber) == 0);
done:
    cleanup(&p);
}

int main(void)
{
    unsigned rows = 0, passed = 0;
#define RUN(call) do { unsigned before = failures; ++rows; call; \
    if (failures == before) ++passed; } while (0)
    for (unsigned origin = 0; origin < 2; ++origin) {
        for (unsigned fields = 1; fields <= 3; ++fields) RUN(refusal_row(origin != 0, fields));
        for (unsigned kind = 0; kind <= 6; ++kind) RUN(equal_row(origin != 0, kind));
        RUN(pending_row(origin != 0));
        RUN(validation_row(origin != 0));
        RUN(legacy_row(origin != 0, MOQ_VERSION_DRAFT_16));
        RUN(legacy_row(origin != 0, MOQ_VERSION_DRAFT_18));
    }
    printf("%u/%u permission cases, %u checks, %u failures\n", passed, rows, checks, failures);
    return failures ? 1 : 0;
}
