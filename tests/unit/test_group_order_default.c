/*
 * The group order of a subscription whose SUBSCRIBE left it out is the
 * publisher's preference from the track: its DEFAULT_PUBLISHER_GROUP_ORDER
 * Track Property (draft-16 §7.1 and §11.1.1.2, draft-18 §12.5, draft-22
 * §2.4 and §10.5), Ascending when that is absent too. The publisher's
 * scheduler orders the subscription's groups by it, and a fill whose
 * FILL_PARAMETERS leave the order out takes the subscription's (draft-22
 * §9.20.15).
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_version_t g_version;
static moq_bytes_t g_parts[] = { { (const uint8_t *)"live", 4 } };

/* DEFAULT_PUBLISHER_GROUP_ORDER (0x22) = Descending (0x2): one-byte type and
 * value in every profile's property encoding. */
static const uint8_t k_desc_props[] = { 0x22, 0x02 };
static const uint8_t k_asc_props[]  = { 0x22, 0x01 };

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    moq_subscription_t csub, ssub;
} pair_t;

static void pump(pair_t *p)
{
    moq_simpair_run_until_quiescent(p->sp, 64, NULL);
    moq_simpair_run_until_quiescent(p->sp, 64, NULL);
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static void drain_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

/* Subscribe (group order `requested`, DEFAULT = left out; optional fill) and
 * accept with Track Properties `props`. */
static bool mk(pair_t *p, moq_group_order_t requested, const uint8_t *props, size_t props_len,
               const moq_fill_cfg_t *fill, moq_fill_request_event_t *fill_req)
{
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = g_version;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    if (moq_simpair_create(&scfg, &p->sp) < 0) return false;
    if (moq_simpair_start(p->sp) < 0) return false;
    p->c = moq_simpair_client(p->sp);
    p->sv = moq_simpair_server(p->sp);
    pump(p);
    drain(p->c); drain(p->sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.group_order = requested;
    cfg.fill = fill;
    if (moq_session_subscribe(p->c, &cfg, 1, &p->csub) != MOQ_OK) return false;
    pump(p);
    moq_event_t ev;
    bool got = false;
    while (moq_session_poll_events(p->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) { p->ssub = ev.u.subscribe_request.sub; got = true; }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ props, props_len };
    ac.has_largest = true; ac.largest_group = 9; ac.largest_object = 0;
    if (moq_session_accept_subscribe(p->sv, p->ssub, &ac, 2) != MOQ_OK) return false;
    bool got_fill = false;
    while (moq_session_poll_events(p->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_REQUEST && fill_req) { *fill_req = ev.u.fill_request; got_fill = true; }
        moq_event_cleanup(&ev);
    }
    if (fill && !got_fill) return false;
    drain_actions(p->sv);
    return true;
}

/* Queue one object in group 1, then one in group 2; return the group of the
 * first data the publisher hands over. */
static uint64_t first_group(pair_t *p)
{
    moq_subgroup_handle_t h[2];
    uint64_t refs[2] = { 0, 0 };
    for (int g = 0; g < 2; g++) {
        moq_subgroup_cfg_t sc; moq_subgroup_cfg_init(&sc);
        sc.group_id = (uint64_t)g + 1;
        if (moq_session_open_subgroup(p->sv, p->ssub, &sc, 3, &h[g]) != MOQ_OK) return 0;
        refs[g] = p->sv->subgroups[moq_handle_slot(h[g]._opaque)].stream_ref._v;
        moq_rcbuf_t *pl = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
        (void)moq_session_write_object(p->sv, h[g], 0, pl, 3);
        moq_rcbuf_decref(pl);
    }
    uint64_t first = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p->sv, &a, 1) > 0) {
        if (first == 0 && a.kind == MOQ_ACTION_SEND_DATA)
            first = a.u.send_data.stream_ref._v == refs[1] ? 2 :
                    a.u.send_data.stream_ref._v == refs[0] ? 1 : 0;
        moq_action_cleanup(&a);
    }
    return first;
}

static int test_scheduler(moq_group_order_t requested, const uint8_t *props, size_t len,
                          uint64_t want_first)
{
    int failures = 0;
    pair_t p;
    MOQ_TEST_CHECK(mk(&p, requested, props, len, NULL, NULL));
    if (!p.sp) return failures;
    MOQ_TEST_CHECK_EQ_U64(first_group(&p), want_first);
    moq_simpair_destroy(p.sp);
    return failures;
}

static int test_fill_inherits(const uint8_t *props, size_t len, uint32_t want)
{
    int failures = 0;
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init_sized(&loc, sizeof(loc));
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    loc.start_group = 1;
    moq_fill_cfg_t fill; moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_fill_request_event_t fr;
    memset(&fr, 0, sizeof(fr));
    pair_t p;
    MOQ_TEST_CHECK(mk(&p, MOQ_GROUP_ORDER_DEFAULT, props, len, &fill, &fr));
    if (!p.sp) return failures;
    MOQ_TEST_CHECK_EQ_U64(fr.group_order, want);
    moq_simpair_destroy(p.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 3; i++) {
        g_version = versions[i];
        int before = failures;
        /* Left out: the track's preference decides. */
        failures += test_scheduler(MOQ_GROUP_ORDER_DEFAULT, k_desc_props, sizeof(k_desc_props), 2);
        failures += test_scheduler(MOQ_GROUP_ORDER_DEFAULT, k_asc_props, sizeof(k_asc_props), 1);
        failures += test_scheduler(MOQ_GROUP_ORDER_DEFAULT, NULL, 0, 1);
        /* Requested: the subscriber's order stands over the track's. */
        failures += test_scheduler(MOQ_GROUP_ORDER_ASCENDING, k_desc_props, sizeof(k_desc_props), 1);
        failures += test_scheduler(MOQ_GROUP_ORDER_DESCENDING, NULL, 0, 2);
        if (failures != before)
            fprintf(stderr, "  version 0x%llx: %d failures\n",
                    (unsigned long long)versions[i], failures - before);
    }
    g_version = MOQ_VERSION_DRAFT_22;
    failures += test_fill_inherits(k_desc_props, sizeof(k_desc_props), MOQ_GROUP_ORDER_DESCENDING);
    failures += test_fill_inherits(NULL, 0, MOQ_GROUP_ORDER_ASCENDING);
    if (failures == 0) MOQ_TEST_PASS("group_order_default");
    return failures != 0;
}
