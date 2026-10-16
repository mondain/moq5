/* Omitted D16/D18 SUBSCRIPTION_FILTER is unfiltered, not live-edge.
 * Reuse the real-session rig, with all state changes through public ingress. */
#include "relay_session_binding_harness.c"

static const uint8_t unfiltered_body[] = { 0xa1, 0x00, 0x73, 0xff, 0x20 };

static bool
unfiltered_send(rig_t *r, conn_t *pub, moq_subscription_t sub, uint64_t group)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(pub->peer, sub, &cfg, r->now, &sg) != MOQ_OK) {
        return false;
    }
    moq_rcbuf_t *payload = NULL;
    if (moq_rcbuf_create(&r->alloc->vt, unfiltered_body, sizeof(unfiltered_body),
                         &payload) != MOQ_OK) {
        return false;
    }
    moq_result_t rc = moq_session_write_object(pub->peer, sg, 0, payload, r->now);
    moq_rcbuf_decref(payload);
    return rc == MOQ_OK &&
           moq_session_close_subgroup(pub->peer, sg, r->now) == MOQ_OK;
}

static unsigned
unfiltered_receive(rig_t *r, conn_t *sub, uint64_t group)
{
    unsigned objects = 0;
    moq_event_t events[16];
    size_t count;
    while ((count = moq_session_poll_events(sub->peer, events, 16)) != 0) {
        for (size_t i = 0; i < count; ++i) {
            moq_event_t *ev = &events[i];
            if (ev->kind == MOQ_EVENT_OBJECT_RECEIVED) {
                const moq_object_received_event_t *o = &ev->u.object_received;
                ++objects;
                R_CHECK(r, o->group_id == group);
                R_CHECK(r, o->subgroup_id == 0 && o->object_id == 0);
                R_CHECK(r, o->status == MOQ_OBJECT_NORMAL);
                R_CHECK(r, o->payload != NULL);
                if (o->payload != NULL) {
                    R_CHECK(r, moq_rcbuf_len(o->payload) == sizeof(unfiltered_body));
                    if (moq_rcbuf_len(o->payload) == sizeof(unfiltered_body)) {
                        R_CHECK(r, memcmp(moq_rcbuf_data(o->payload), unfiltered_body,
                                          sizeof(unfiltered_body)) == 0);
                    }
                }
            } else {
                R_CHECK(r, ev->kind == MOQ_EVENT_SUBGROUP_FINISHED);
            }
            moq_event_cleanup(ev);
        }
    }
    return objects;
}

static int
unfiltered_omission(moq_version_t version, bool deferred, bool update)
{
    ca_t alloc;
    relay_bind_test_ca_init(&alloc);
    relay_bind_test_authorize = relay_bind_test_defer_hook;
    relay_bind_test_defer_action = UINT32_MAX;
    rig_t rig;
    if (relay_bind_test_rig_create(&rig, &alloc) != MOQR_OK) {
        relay_bind_test_authorize = NULL;
        printf("SETUP: rig create\n");
        return 1;
    }
    int causal = 0;
    conn_t *pub = relay_bind_test_rig_connect(&rig, version);
    conn_t *prime = relay_bind_test_rig_connect(&rig, version);
    conn_t *subs[3];
    for (size_t i = 0; i < 3; ++i) subs[i] = relay_bind_test_rig_connect(&rig, version);
#define REQUIRE(expr) do { \
    if (!(expr)) { printf("SETUP: %s\n", #expr); ++rig.failures; goto done; } \
} while (0)
    REQUIRE(pub && prime && subs[0] && subs[1] && subs[2]);
    relay_bind_test_rig_pump(&rig, 4);
    moq_bytes_t parts[] = { B("filter"), B("late") };
    moq_publish_namespace_cfg_t pcfg;
    moq_publish_namespace_cfg_init(&pcfg);
    pcfg.track_namespace = (moq_namespace_t){ .parts = parts, .count = 2 };
    moq_announcement_t announcement;
    REQUIRE(moq_session_publish_namespace(pub->peer, &pcfg, rig.now,
                                          &announcement) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = pcfg.track_namespace;
    cfg.track_name = B("video");
    cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t prime_handle;
    REQUIRE(moq_session_subscribe(prime->peer, &cfg, rig.now,
                                  &prime_handle) == MOQ_OK);
    relay_bind_test_rig_pump(&rig, 6);
    peer_state_t publisher = {0}, initial = {0};
    /* The upstream Largest filter resolves at acceptance before any content,
     * therefore {0,0}. A later lower-group object remains legal upstream. */
    relay_bind_test_peer_drain(&rig, pub, &publisher, true, false, 0, 0);
    REQUIRE(publisher.up_seen && publisher.up_subs == 1);
    relay_bind_test_rig_pump(&rig, 6);
    relay_bind_test_peer_drain(&rig, prime, &initial, false, false, 0, 0);
    REQUIRE(initial.subscribe_ok && !initial.has_largest);
    REQUIRE(unfiltered_send(&rig, pub, publisher.up_sub, 1));
    relay_bind_test_rig_pump(&rig, 8);
    REQUIRE(unfiltered_receive(&rig, prime, 1) == 1);

    const moq_subscribe_filter_t filters[] = {
        MOQ_SUBSCRIBE_FILTER_NONE,
        MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START,
        MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT
    };
    moq_subscription_t handles[3];
    if (deferred) relay_bind_test_defer_action = MOQR_AUTH_SUBSCRIBE;
    for (size_t i = 0; i < 3; ++i) {
        cfg.filter = filters[i];
        relay_bind_test_defer_last = 0;
        REQUIRE(moq_session_subscribe(subs[i]->peer, &cfg, rig.now,
                                      &handles[i]) == MOQ_OK);
        if (deferred) {
            relay_bind_test_rig_pump(&rig, 6);
            peer_state_t parked = {0};
            relay_bind_test_peer_drain(&rig, subs[i], &parked, false, false, 0, 0);
            REQUIRE(!parked.subscribe_ok && parked.subscribe_errors == 0 &&
                    parked.objects == 0 && relay_bind_test_defer_last != 0);
            REQUIRE(moqr_bind_auth_resolve(rig.bind, relay_bind_test_defer_last,
                &(moqr_auth_verdict_t){ .decision = MOQR_AUTH_ALLOW },
                rig.now) == MOQR_OK);
        }
    }
    relay_bind_test_defer_action = UINT32_MAX;
    relay_bind_test_rig_pump(&rig, 8);
    for (size_t i = 0; i < 3; ++i) {
        peer_state_t state = {0};
        relay_bind_test_peer_drain(&rig, subs[i], &state, false, false, 0, 0);
        REQUIRE(state.subscribe_ok && state.subscribe_errors == 0);
        REQUIRE(state.has_largest && state.largest_group == 1 &&
                state.largest_object == 0);
    }
    if (update) {
        for (size_t i = 0; i < 3; ++i) {
            moq_subscription_update_cfg_t ucfg;
            moq_subscription_update_cfg_init_sized(&ucfg, sizeof(ucfg));
            ucfg.has_subscriber_priority = true;
            ucfg.subscriber_priority = 117;
            REQUIRE(!ucfg.has_filter && ucfg.location == NULL);
            REQUIRE(moq_session_update_subscription(subs[i]->peer, handles[i],
                                                      &ucfg, rig.now) == MOQ_OK);
        }
        relay_bind_test_rig_pump(&rig, 8);
        for (size_t i = 0; i < 3; ++i) {
            peer_state_t updated = {0};
            relay_bind_test_peer_drain(&rig, subs[i], &updated, false, false, 0, 0);
            REQUIRE(updated.update_oks == 1 && updated.objects == 0 &&
                    !updated.session_closed && !updated.done_seen);
            moq_location_filter_cfg_t selected;
            moq_location_filter_cfg_init(&selected);
            REQUIRE(moq_session_subscription_filter(subs[i]->peer, handles[i],
                                                     &selected) == MOQ_OK);
            const uint32_t kinds[] = { MOQ_LOCATION_FILTER_NONE,
                MOQ_LOCATION_FILTER_ABSOLUTE_START, MOQ_LOCATION_FILTER_NEXT_OBJECT };
            REQUIRE(selected.kind == kinds[i]);
        }
    }
    /* No cached-history claim: this object is first published AFTER all three
     * new subscriptions were admitted at exactly the same Largest {1,0}. */
    REQUIRE(unfiltered_send(&rig, pub, publisher.up_sub, 0));
    relay_bind_test_rig_pump(&rig, 8);
    REQUIRE(unfiltered_receive(&rig, prime, 0) == 1);
    unsigned late[3];
    for (size_t i = 0; i < 3; ++i) late[i] = unfiltered_receive(&rig, subs[i], 0);
    printf("draft=%u deferred=%d update=%d late none=%u absolute=%u largest=%u\n",
           version == MOQ_VERSION_DRAFT_16 ? 16u : 18u,
           deferred, update,
           late[0], late[1], late[2]);
    if (late[0] != 1) { puts("FAIL: omitted filter lost future lower object"); ++causal; }
    R_CHECK(&rig, late[1] == 1 && late[2] == 0);
    REQUIRE(unfiltered_send(&rig, pub, publisher.up_sub, 2));
    relay_bind_test_rig_pump(&rig, 8);
    REQUIRE(unfiltered_receive(&rig, prime, 2) == 1);
    for (size_t i = 0; i < 3; ++i) R_CHECK(&rig, unfiltered_receive(&rig, subs[i], 2) == 1);
    moqr_bind_stats_t stats;
    moqr_bind_get_stats(rig.bind, &stats);
    R_CHECK(&rig, stats.session_errors == 0);
done:
    relay_bind_test_rig_destroy(&rig);
    relay_bind_test_authorize = NULL;
    relay_bind_test_defer_action = UINT32_MAX;
    R_CHECK(&rig, alloc.live == 0);
    printf("draft=%u deferred=%d update=%d causal=%d controls=%d\n",
           version == MOQ_VERSION_DRAFT_16 ? 16u : 18u,
           deferred, update, causal, rig.failures);
#undef REQUIRE
    return causal + rig.failures;
}

int main(void)
{
    int failures = 0;
    for (unsigned deferred = 0; deferred < 2; ++deferred) {
        for (unsigned update = 0; update < 2; ++update) {
            failures += unfiltered_omission(MOQ_VERSION_DRAFT_16, deferred != 0, update != 0);
            failures += unfiltered_omission(MOQ_VERSION_DRAFT_18, deferred != 0, update != 0);
        }
    }
    if (failures == 0) puts("PASS: relay_unfiltered (8 cases)");
    return failures == 0 ? 0 : 1;
}
