/*
 * A FETCH data stream whose decode runs out of memory part-way through a
 * chunk keeps every object it already surfaced consumed: the retry resumes
 * at the object that failed, so each object is surfaced exactly once and the
 * delta-coded IDs that follow decode against the right predecessor. Every
 * allocation point in the decode is tried, on every profile, and on a
 * draft-22 fill fetch stream.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "test_session_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static const uint64_t k_ids[3] = { 5, 9, 10 };

static void drain_events(moq_session_t *s);
static size_t collect_fill_objects(moq_session_t *s, uint64_t *out, size_t cap);

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

/* Collect FETCH_OBJECT object ids (and payload sizes) into out[], returning the count. */
static size_t collect_objects(moq_session_t *s, uint64_t *out, size_t cap)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_OBJECT && n < cap)
            out[n++] = ev.u.fetch_object.object_id;
        else if (ev.kind == MOQ_EVENT_FETCH_OBJECT)
            n++;
        moq_event_cleanup(&ev);
    }
    return n;
}

/* One run: returns 1 when fail_at hit an allocation inside the object decode
 * (the feed reported NOMEM), 0 when it did not, -1 on a setup failure. */
static int run_once(moq_version_t version, int fail_at, int *failures_out)
{
    int failures = 0;
    fail_alloc_state_t fas = { 0 };
    moq_alloc_t alloc = fail_allocator(&fas);
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = &alloc;
    scfg.version = version;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return -1;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return -1; }
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain_events(c); drain_events(sv);

    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t fcfg; moq_fetch_cfg_init(&fcfg);
    fcfg.track_namespace = (moq_namespace_t){ parts, 1 };
    fcfg.track_name = MOQ_BYTES_LITERAL("v");
    fcfg.start_group = 0; fcfg.start_object = 0;
    fcfg.end_group = 1; fcfg.end_object = 0;
    moq_fetch_t cf;
    MOQ_TEST_CHECK(moq_session_fetch(c, &fcfg, 0, &cf) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_fetch_t sf = { 0 };
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) sf = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    moq_accept_fetch_cfg_t acc; moq_accept_fetch_cfg_init(&acc);
    acc.end_group = 0; acc.end_object = 11;
    MOQ_TEST_CHECK(moq_session_accept_fetch(sv, sf, &acc, 0) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    drain_events(c);

    /* The client's data stream for this fetch (its FETCH_HEADER is in). */
    moq_stream_ref_t rx_ref = { 0 };
    for (size_t i = 0; i < c->rx_cap; i++)
        if (c->rx_streams[i].active) { rx_ref = c->rx_streams[i].stream_ref; break; }
    MOQ_TEST_CHECK(rx_ref._v != 0);

    /* Three objects in one group, written and taken straight off the
     * server's action queue as one contiguous chunk. */
    for (size_t k = 0; k < 3; k++) {
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(&alloc, (const uint8_t *)"payload", 7, &p);
        moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
        oc.group_id = 0; oc.object_id = k_ids[k]; oc.payload = p;
        moq_result_t wrc = moq_session_write_fetch_object(sv, sf, &oc, 0);
        MOQ_TEST_CHECK(wrc == MOQ_OK);
        moq_rcbuf_decref(p);
    }
    uint8_t chunk[512];
    size_t clen = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            size_t hl = a.u.send_data.header_len;
            memcpy(chunk + clen, a.u.send_data.header, hl); clen += hl;
            if (a.u.send_data.payload) {
                size_t pl = moq_rcbuf_len(a.u.send_data.payload);
                memcpy(chunk + clen, moq_rcbuf_data(a.u.send_data.payload), pl);
                clen += pl;
            }
        }
        moq_action_cleanup(&a);
    }

    fas.call_count = 0;
    fas.fail_at = fail_at;
    moq_result_t rc = moq_session_on_data_bytes(c, rx_ref, chunk, clen, false, 0);
    fas.fail_at = 0;
    int hit = 0;
    uint64_t ids[8];
    size_t n;
    if (rc == MOQ_ERR_NOMEM) {
        hit = 1;
        n = collect_objects(c, ids, 8);
        /* A refusal before the chunk was buffered leaves it with the caller,
         * to be fed again; once buffered, an empty feed resumes it. */
        size_t buffered = 0;
        for (size_t i = 0; i < c->rx_cap; i++)
            if (c->rx_streams[i].active &&
                c->rx_streams[i].stream_ref._v == rx_ref._v)
                buffered = c->rx_streams[i].input_len;
        rc = buffered ? moq_session_on_data_bytes(c, rx_ref, NULL, 0, false, 0)
                      : moq_session_on_data_bytes(c, rx_ref, chunk, clen, false, 0);
        MOQ_TEST_CHECK(rc == MOQ_OK);
        n += collect_objects(c, ids + (n < 8 ? n : 8), n < 8 ? 8 - n : 0);
    } else {
        MOQ_TEST_CHECK(rc == MOQ_OK);
        n = collect_objects(c, ids, 8);
    }
    MOQ_TEST_CHECK(c->state != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(n == 3);
    for (size_t k = 0; k < 3 && k < n; k++)
        MOQ_TEST_CHECK(ids[k] == k_ids[k]);
    if (failures)
        fprintf(stderr, "  version 0x%x fail_at %d: %zu objects\n",
                (unsigned)version, fail_at, n);

    moq_simpair_destroy(sp);
    *failures_out += failures;
    return hit;
}

static size_t collect_fill_objects(moq_session_t *s, uint64_t *out, size_t cap)
{
    size_t n = 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_OBJECT) {
            if (n < cap) out[n] = ev.u.fill_object.object_id;
            n++;
        }
        moq_event_cleanup(&ev);
    }
    return n;
}

/* The same on a fill fetch stream (draft-22). */
static int run_fill_once(int fail_at, int *failures_out)
{
    int failures = 0;
    fail_alloc_state_t fas = { 0 };
    moq_alloc_t alloc = fail_allocator(&fas);
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = &alloc;
    scfg.version = MOQ_VERSION_DRAFT_22;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return -1;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return -1; }
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain_events(c); drain_events(sv);

    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_location_filter_cfg_t loc;
    moq_location_filter_cfg_init_sized(&loc, sizeof(loc));
    loc.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    moq_fill_cfg_t fill; moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.fill = &fill;
    moq_subscription_t csub;
    MOQ_TEST_CHECK(moq_session_subscribe(c, &cfg, 0, &csub) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    moq_subscription_t ssub = { 0 };
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 0; ac.largest_object = 20;
    MOQ_TEST_CHECK(moq_session_accept_subscribe(sv, ssub, &ac, 0) == MOQ_OK);
    moq_fill_t sfill = { 0 };
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FILL_REQUEST) sfill = ev.u.fill_request.fill;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_session_open_fill(sv, sfill, 0) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    drain_events(c);
    moq_stream_ref_t rx_ref = { 0 };
    for (size_t i = 0; i < c->rx_cap; i++)
        if (c->rx_streams[i].active && c->rx_streams[i].fill_bound) {
            rx_ref = c->rx_streams[i].stream_ref; break;
        }
    MOQ_TEST_CHECK(rx_ref._v != 0);

    for (size_t k = 0; k < 3; k++) {
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(&alloc, (const uint8_t *)"payload", 7, &p);
        moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
        oc.group_id = 0; oc.object_id = k_ids[k]; oc.payload = p;
        MOQ_TEST_CHECK(moq_session_write_fill_object(sv, sfill, &oc, 0) == MOQ_OK);
        moq_rcbuf_decref(p);
    }
    uint8_t chunk[512];
    size_t clen = 0;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            size_t hl = a.u.send_data.header_len;
            memcpy(chunk + clen, a.u.send_data.header, hl); clen += hl;
            if (a.u.send_data.payload) {
                size_t pl = moq_rcbuf_len(a.u.send_data.payload);
                memcpy(chunk + clen, moq_rcbuf_data(a.u.send_data.payload), pl);
                clen += pl;
            }
        }
        moq_action_cleanup(&a);
    }

    fas.call_count = 0;
    fas.fail_at = fail_at;
    moq_result_t rc = moq_session_on_data_bytes(c, rx_ref, chunk, clen, false, 0);
    fas.fail_at = 0;
    int hit = 0;
    uint64_t ids[8];
    size_t n;
    if (rc == MOQ_ERR_NOMEM) {
        hit = 1;
        n = collect_fill_objects(c, ids, 8);
        size_t buffered = 0;
        for (size_t i = 0; i < c->rx_cap; i++)
            if (c->rx_streams[i].active &&
                c->rx_streams[i].stream_ref._v == rx_ref._v)
                buffered = c->rx_streams[i].input_len;
        rc = buffered ? moq_session_on_data_bytes(c, rx_ref, NULL, 0, false, 0)
                      : moq_session_on_data_bytes(c, rx_ref, chunk, clen, false, 0);
        MOQ_TEST_CHECK(rc == MOQ_OK);
        n += collect_fill_objects(c, ids + (n < 8 ? n : 8), n < 8 ? 8 - n : 0);
    } else {
        MOQ_TEST_CHECK(rc == MOQ_OK);
        n = collect_fill_objects(c, ids, 8);
    }
    MOQ_TEST_CHECK(c->state != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(n == 3);
    for (size_t k = 0; k < 3 && k < n; k++)
        MOQ_TEST_CHECK(ids[k] == k_ids[k]);
    if (failures)
        fprintf(stderr, "  fill fail_at %d: %zu objects\n", fail_at, n);
    moq_simpair_destroy(sp);
    *failures_out += failures;
    return hit;
}

int main(void)
{
    int failures = 0;
    const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        int hits = 0;
        for (int k = 1; k <= 12; k++) {
            int r = run_once(versions[v], k, &failures);
            MOQ_TEST_CHECK(r >= 0);
            if (r > 0) hits++;
        }
        /* At least the second and third objects' allocations were refused
         * mid-chunk on every profile. */
        MOQ_TEST_CHECK(hits >= 2);
    }
    int fill_hits = 0;
    for (int k = 1; k <= 12; k++) {
        int r = run_fill_once(k, &failures);
        MOQ_TEST_CHECK(r >= 0);
        if (r > 0) fill_hits++;
    }
    MOQ_TEST_CHECK(fill_hits >= 2);
    if (failures == 0) MOQ_TEST_PASS("fetch_receive_retry");
    return failures != 0;
}
