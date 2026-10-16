/* D22 6.4.2.2 / 9.2: a FETCH_OK does not forbid request-local GOAWAY.
 * Control/header bytes come from public peers; object and malformed-control
 * bytes are independent literal wire. No private retirement helper is called. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/session_outbound_internal.h"
#include "../../core/src/session/request_stream_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <string.h>

typedef struct fixture {
    moq_simpair_t *pair;
    moq_session_t *fetcher, *publisher;
    moq_fetch_t fetch, served;
    moq_stream_ref_t request, peer_request, data;
    int slot;
    uint8_t response[128], header[32], goaway[128];
    size_t response_len, header_len, goaway_len;
    moq_bytes_t uri;
    uint64_t now;
    bool ready;
} fixture_t;

static const uint8_t first[] = {0x1f, 10, 6, 6, 128, 1, 'A'};
static const uint8_t second[] = {0x01, 1, 'B'};
static const uint8_t rest[] = {
    0x09, 2, 1, 'B',       /* 10/6 -> 7/7, same actual subgroup/priority */
    0x81, 0x0c, 6, 4,     /* absolute UNKNOWN range end, metadata unchanged */
    0x09, 0, 1, 'C'       /* range end 6/4 -> 5/5 */
};

static void drain_events(moq_session_t *s)
{
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}

static bool pump(fixture_t *p)
{
    return moq_simpair_run_until_quiescent(p->pair, 64, NULL) == MOQ_OK;
}

static moq_rx_stream_t *rx_for(fixture_t *p)
{
    for (size_t i = 0; i < p->fetcher->rx_cap; ++i)
        if (p->fetcher->rx_streams[i].active &&
            p->fetcher->rx_streams[i].stream_ref._v == p->data._v)
            return &p->fetcher->rx_streams[i];
    return NULL;
}

static void fetch_config(moq_fetch_cfg_t *cfg, moq_group_order_t order)
{
    static const moq_bytes_t parts[] = {{.data = (const uint8_t *)"migration", .len = 9}};
    moq_fetch_cfg_init(cfg);
    cfg->track_namespace = (moq_namespace_t){parts, 1};
    cfg->track_name = MOQ_BYTES_LITERAL("track");
    cfg->group_order = order;
    cfg->end_group = 20;
    cfg->end_object = 10;
}

/* Snapshot borrowed production action bytes before the next advancing call. */
static bool capture_accept(fixture_t *p)
{
    bool ok = true;
    size_t responses = 0, headers = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p->publisher, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == p->peer_request._v &&
            !a.u.send_bidi_stream.fin &&
            a.u.send_bidi_stream.len <= sizeof(p->response)) {
            memcpy(p->response, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            p->response_len = a.u.send_bidi_stream.len;
            ++responses;
        } else if (a.kind == MOQ_ACTION_SEND_DATA && !a.u.send_data.payload &&
                   !a.u.send_data.fin &&
                   a.u.send_data.header_len <= sizeof(p->header)) {
            memcpy(p->header, a.u.send_data.header, a.u.send_data.header_len);
            p->header_len = a.u.send_data.header_len;
            p->data = a.u.send_data.stream_ref;
            ++headers;
        } else ok = false;
        moq_action_cleanup(&a);
    }
    return ok && responses == 1 && headers == 1 &&
        p->response_len > 0 && p->header_len > 0 && p->data._v != 0;
}

static bool make_impl(fixture_t *p, bool server_fetcher, moq_version_t version,
    bool deliver_response, uint32_t actions, uint32_t events, bool accept)
{
    memset(p, 0, sizeof(*p));
    p->now = 100;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.max_actions = actions;
    cfg.max_events = events;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 32;
    if (moq_simpair_create(&cfg, &p->pair) != MOQ_OK || !p->pair) return false;
    if (moq_simpair_start(p->pair) != MOQ_OK || !pump(p)) return false;
    p->fetcher = server_fetcher ? moq_simpair_server(p->pair) : moq_simpair_client(p->pair);
    p->publisher = server_fetcher ? moq_simpair_client(p->pair) : moq_simpair_server(p->pair);
    if (moq_session_version(p->fetcher) != version ||
        moq_session_version(p->publisher) != version ||
        moq_session_state(p->fetcher) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(p->publisher) != MOQ_SESS_ESTABLISHED) return false;
    drain_events(p->fetcher);
    drain_events(p->publisher);
    moq_fetch_cfg_t fc;
    fetch_config(&fc, version == MOQ_VERSION_DRAFT_22
                      ? MOQ_GROUP_ORDER_DESCENDING : MOQ_GROUP_ORDER_ASCENDING);
    if (moq_session_fetch(p->fetcher, &fc, ++p->now, &p->fetch) != MOQ_OK || !pump(p))
        return false;
    moq_event_t e;
    size_t requests = 0;
    while (moq_session_poll_events(p->publisher, &e, 1)) {
        if (e.kind == MOQ_EVENT_FETCH_REQUEST) { p->served = e.u.fetch_request.fetch; ++requests; }
        moq_event_cleanup(&e);
    }
    if (requests != 1) return false;
    p->slot = fetch_resolve_handle(p->fetcher, p->fetch);
    int ps = fetch_resolve_handle(p->publisher, p->served);
    if (p->slot < 0 || ps < 0) return false;
    p->request = p->fetcher->fetches[p->slot].rs.ref;
    p->peer_request = p->publisher->fetches[ps].rs.ref;
    if (!p->request._v || !p->peer_request._v) return false;
    if (!accept) { p->ready = true; return true; }
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 20;
    ac.end_object = 10;
    if (moq_session_accept_fetch(p->publisher, p->served, &ac, ++p->now) != MOQ_OK ||
        !capture_accept(p)) return false;
    if (deliver_response) {
        if (moq_session_on_bidi_stream_bytes(p->fetcher, p->request,
                p->response, p->response_len, false, ++p->now) != MOQ_OK) return false;
        size_t oks = 0;
        while (moq_session_poll_events(p->fetcher, &e, 1)) {
            if (e.kind == MOQ_EVENT_FETCH_OK &&
                moq_fetch_eq(e.u.fetch_ok.fetch, p->fetch)) ++oks;
            else { moq_event_cleanup(&e); return false; }
            moq_event_cleanup(&e);
        }
        if (oks != 1 || !p->fetcher->fetches[p->slot].control_response_seen) return false;
    }
    p->ready = true;
    return true;
}

static bool make(fixture_t *p, bool server_fetcher, moq_version_t version,
                 bool deliver_response, uint32_t actions, uint32_t events)
{
    return make_impl(p, server_fetcher, version, deliver_response, actions, events, true);
}

static bool send_goaway(fixture_t *p)
{
    moq_request_goaway_cfg_t gc;
    moq_request_goaway_cfg_init(&gc);
    if (moq_session_perspective(p->publisher) == MOQ_PERSPECTIVE_SERVER)
        p->uri = MOQ_BYTES_LITERAL("moq://next.invalid/moq");
    gc.new_session_uri = p->uri;
    gc.timeout_ms = 50;
    if (moq_session_request_goaway_fetch(p->publisher, p->served, &gc, ++p->now) != MOQ_OK)
        return false;
    size_t messages = 0;
    bool ok = true;
    moq_action_t a;
    while (moq_session_poll_actions(p->publisher, &a, 1)) {
        if (a.kind != MOQ_ACTION_SEND_BIDI_STREAM || a.u.send_bidi_stream.fin ||
            a.u.send_bidi_stream.stream_ref._v != p->peer_request._v ||
            a.u.send_bidi_stream.len > sizeof(p->goaway)) ok = false;
        else {
            memcpy(p->goaway, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            p->goaway_len = a.u.send_bidi_stream.len;
            ++messages;
        }
        moq_action_cleanup(&a);
    }
    return ok && messages == 1 && p->goaway_len > 0;
}

static int observations(fixture_t *p, size_t want_goaway, size_t want_ok, size_t want_object)
{
    int failures = 0;
    size_t goaways = 0, oks = 0, objects = 0;
    moq_event_t e;
    while (moq_session_poll_events(p->fetcher, &e, 1)) {
        if (e.kind == MOQ_EVENT_REQUEST_GOAWAY) {
            ++goaways;
            MOQ_TEST_CHECK_EQ_INT(e.u.request_goaway.family, MOQ_REQUEST_FAMILY_FETCH);
            MOQ_TEST_CHECK_EQ_U64(e.u.request_goaway.handle.raw, p->fetch._opaque);
            MOQ_TEST_CHECK_EQ_U64(e.u.request_goaway.timeout_ms, 50);
            MOQ_TEST_CHECK_EQ_SIZE(e.u.request_goaway.new_session_uri.len, p->uri.len);
            if (e.u.request_goaway.new_session_uri.len == p->uri.len && p->uri.len)
                MOQ_TEST_CHECK(memcmp(e.u.request_goaway.new_session_uri.data,
                                      p->uri.data, p->uri.len) == 0);
        } else if (e.kind == MOQ_EVENT_FETCH_OK) {
            ++oks;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_ok.fetch, p->fetch));
        } else if (e.kind == MOQ_EVENT_FETCH_OBJECT) {
            ++objects;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_object.fetch, p->fetch));
            MOQ_TEST_CHECK_EQ_U64(e.u.fetch_object.group_id, 10);
            MOQ_TEST_CHECK_EQ_U64(e.u.fetch_object.subgroup_id, 6);
            MOQ_TEST_CHECK_EQ_INT(e.u.fetch_object.publisher_priority, 128);
        } else {
            fprintf(stderr, "unexpected event kind %d\n", (int)e.kind);
            if (e.kind == MOQ_EVENT_SESSION_CLOSED)
                fprintf(stderr, "session close: %.*s\n", (int)e.u.closed.reason.len,
                        (const char *)e.u.closed.reason.data);
            MOQ_TEST_CHECK(false);
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(goaways, want_goaway);
    MOQ_TEST_CHECK_EQ_SIZE(oks, want_ok);
    MOQ_TEST_CHECK_EQ_SIZE(objects, want_object);
    return failures;
}

static int close_actions(fixture_t *p, size_t want)
{
    int failures = 0;
    size_t closes = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p->fetcher, &a, 1)) {
        if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p->request._v);
        } else {
            /* Pressure uses ordinary new FETCH requests, never synthetic queue slots. */
            MOQ_TEST_CHECK(a.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, want);
    return failures;
}

static int migration(bool server_fetcher, bool early_header, unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, server_fetcher, MOQ_VERSION_DRAFT_22, mode != 2, 0, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    if (early_header) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            first, sizeof(first), false, ++p.now), MOQ_OK);
        failures += observations(&p, 0, 0, 1);
    }
    ok = send_goaway(&p);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    uint8_t bytes[256];
    size_t n = 0;
    if (mode == 2) { memcpy(bytes, p.response, p.response_len); n = p.response_len; }
    memcpy(bytes + n, p.goaway, p.goaway_len);
    n += p.goaway_len;
    for (size_t off = 0; off < n; ) {
        size_t chunk = mode == 3 ? 1 : n;
        if (chunk > n - off) chunk = n - off;
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            bytes + off, chunk, mode == 1 && off + chunk == n, ++p.now), MOQ_OK);
        off += chunk;
        if (moq_session_state(p.fetcher) != MOQ_SESS_ESTABLISHED) break;
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    failures += observations(&p, 1, mode == 2 ? 1 : 0, 0);
    if (moq_session_state(p.fetcher) != MOQ_SESS_ESTABLISHED) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_fetch_cancel(p.fetcher, p.fetch, ++p.now), MOQ_ERR_STALE_HANDLE);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_INT(drain_ref_contains(p.fetcher, p.request), mode != 1);
    failures += close_actions(&p, 1);
    if (!early_header) {
        MOQ_TEST_CHECK_EQ_INT(p.fetcher->fetches[p.slot].state, MOQ_FETCH_GOAWAY_LOCAL);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, false, ++p.now), MOQ_OK);
    }
    moq_rx_stream_t *rx = rx_for(&p);
    MOQ_TEST_CHECK(rx && rx->fetch_order_known);
    if (!rx) goto done;
    moq_fetch_cfg_t cfg;
    fetch_config(&cfg, MOQ_GROUP_ORDER_ASCENDING);
    moq_fetch_t replacement;
    moq_result_t rc = moq_session_fetch(p.fetcher, &cfg, ++p.now, &replacement);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, replacement), p.slot);
    MOQ_TEST_CHECK_EQ_INT(p.fetcher->fetches[p.slot].effective_group_order, MOQ_GROUP_ORDER_ASCENDING);
    if (!early_header)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            first, sizeof(first), false, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        rest, sizeof(rest), false, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(rx->effective_fetch_group_order, MOQ_GROUP_ORDER_DESCENDING);
    MOQ_TEST_CHECK_EQ_U64(rx->fetch_prior.group_id, 5);
    MOQ_TEST_CHECK_EQ_U64(rx->fetch_prior.object_id, 5);
    MOQ_TEST_CHECK_EQ_U64(rx->fetch_prior.subgroup_id, 6);
    MOQ_TEST_CHECK_EQ_INT(rx->fetch_prior.publisher_priority, 128);
    MOQ_TEST_CHECK(rx->fetch_prior.has_actual && rx->input_len == 0);
    failures += observations(&p, 0, 0, 0);
    if (mode != 1) {
        /* Late framed messages are discarded until the old bidi's FIN. */
        const uint8_t ignored[] = {0x07, 0, 0};
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            ignored, sizeof(ignored), false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK(drain_ref_contains(p.fetcher, p.request));
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, true, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK(!drain_ref_contains(p.fetcher, p.request));
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        NULL, 0, true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK(!rx_for(&p));
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.publisher, p.peer_request,
        NULL, 0, true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.publisher, p.served) < 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.publisher), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    failures += observations(&p, 0, 0, 0);
    failures += close_actions(&p, 0);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int pressure(bool server_fetcher, unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, server_fetcher, MOQ_VERSION_DRAFT_22, mode != 3, 4, mode == 3 ? 1 : 2);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    if (mode == 0 || mode == 2) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            first, sizeof(first), false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            second, sizeof(second), false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK(event_queue_full(p.fetcher));
    }
    if (mode == 1 || mode == 2) {
        moq_fetch_cfg_t cfg;
        fetch_config(&cfg, MOQ_GROUP_ORDER_ASCENDING);
        for (size_t i = 0; i < p.fetcher->action_cap; ++i) {
            moq_fetch_t h;
            moq_result_t rc = moq_session_fetch(p.fetcher, &cfg, ++p.now, &h);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc != MOQ_OK) goto done;
        }
        MOQ_TEST_CHECK(action_queue_full(p.fetcher));
    }
    ok = send_goaway(&p);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    uint32_t generation = p.fetcher->fetches[p.slot].generation;
    size_t drains = p.fetcher->drain_ref_count;
    bool fin = mode != 1;
    uint8_t bytes[256];
    size_t len = 0;
    if (mode == 3) { memcpy(bytes, p.response, p.response_len); len = p.response_len; }
    memcpy(bytes + len, p.goaway, p.goaway_len);
    len += p.goaway_len;
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        bytes, len, fin, ++p.now), MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    if (moq_session_state(p.fetcher) != MOQ_SESS_ESTABLISHED) goto done;
    MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, generation);
    MOQ_TEST_CHECK_EQ_SIZE(p.fetcher->drain_ref_count, drains);
    MOQ_TEST_CHECK_EQ_SIZE(p.fetcher->fetches[p.slot].rs.recv_len, p.goaway_len);
    MOQ_TEST_CHECK_EQ_INT(p.fetcher->fetches[p.slot].rs.recv_fin, fin);
    MOQ_TEST_CHECK(p.fetcher->fetches[p.slot].control_response_seen);
    failures += observations(&p, 0, mode == 3 ? 1 : 0, mode == 0 || mode == 2 ? 2 : 0);
    failures += close_actions(&p, 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        NULL, 0, false, ++p.now), MOQ_OK);
    failures += observations(&p, 1, 0, 0);
    failures += close_actions(&p, 1);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_INT(drain_ref_contains(p.fetcher, p.request), !fin);
    if (!fin) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, true, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK(!drain_ref_contains(p.fetcher, p.request));
    }
    failures += observations(&p, 0, 0, 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int truncated_fin(bool server_fetcher)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, server_fetcher, MOQ_VERSION_DRAFT_22, true, 0, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    const uint8_t partial[] = {0x10, 0};
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        partial, sizeof(partial), true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    size_t terminals = 0;
    moq_event_t e;
    while (moq_session_poll_events(p.fetcher, &e, 1)) {
        MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_RESET);
        if (e.kind == MOQ_EVENT_FETCH_RESET) {
            ++terminals;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_reset.fetch, p.fetch));
            MOQ_TEST_CHECK_EQ_INT(e.u.fetch_reset.cause, MOQ_REQUEST_TERMINAL_TRUNCATED_RESPONSE);
            MOQ_TEST_CHECK_EQ_U64(e.u.fetch_reset.error_code, 0);
            MOQ_TEST_CHECK(!e.u.fetch_reset.data_stream && !e.u.fetch_reset.stop_sending);
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(terminals, 1);
    size_t resets = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.fetcher, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_RESET_BIDI_STREAM);
        if (a.kind == MOQ_ACTION_RESET_BIDI_STREAM) {
            ++resets;
            MOQ_TEST_CHECK_EQ_U64(a.u.reset_bidi_stream.stream_ref._v, p.request._v);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(resets, 1);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int protocol_error(bool server_fetcher, unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, server_fetcher, MOQ_VERSION_DRAFT_22, true, 0, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    static const uint8_t missing_uri[] = {0x10, 0, 0};
    static const uint8_t missing_timeout[] = {0x10, 0, 1, 0};
    static const uint8_t extra_tail[] = {0x10, 0, 3, 0, 0, 0};
    static const uint8_t invalid_uri[] = {0x10, 0, 3, 1, 'x', 0};
    static const uint8_t wrong_response[] = {0x07, 0, 0};
    static const uint8_t wrong_message[] = {0x1f, 0, 0};
    const uint8_t *bytes = missing_uri;
    size_t len = sizeof(missing_uri);
    switch (mode) {
    case 1: bytes = missing_timeout; len = sizeof(missing_timeout); break;
    case 2: bytes = extra_tail; len = sizeof(extra_tail); break;
    case 3: bytes = p.response; len = p.response_len; break;
    case 4: bytes = wrong_response; len = sizeof(wrong_response); break;
    case 5: bytes = wrong_message; len = sizeof(wrong_message); break;
    case 6: bytes = invalid_uri; len = sizeof(invalid_uri); break;
    case 7: case 8:
        ok = send_goaway(&p);
        MOQ_TEST_CHECK(ok);
        if (!ok) goto done;
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            p.goaway, p.goaway_len, false, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
        if (moq_session_state(p.fetcher) != MOQ_SESS_ESTABLISHED) goto done;
        failures += observations(&p, 1, 0, 0);
        bytes = p.goaway; len = p.goaway_len;
        break;
    default: break;
    }
    for (size_t off = 0; off < len; ) {
        size_t n = mode == 8 ? 1 : len;
        if (n > len - off) n = len - off;
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            bytes + off, n, false, ++p.now), MOQ_OK);
        off += n;
        if (moq_session_state(p.fetcher) == MOQ_SESS_CLOSED) break;
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_CLOSED);
    size_t closes = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.fetcher, &a, 1)) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_session.code, 0x3);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, 1);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int legacy_guard(bool server_fetcher)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, server_fetcher, MOQ_VERSION_DRAFT_18, true, 0, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    ok = send_goaway(&p);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        p.goaway, p.goaway_len, false, ++p.now), MOQ_OK);
    /* This task deliberately does not change the predecessor's D18 guard. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_CLOSED);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

/* D22 successful nonempty completion closes both request directions.
 * Only FINs actually polled from actions are delivered; missing output is
 * never repaired by injecting a synthetic peer FIN into the test. */
static int request_fin_completion(void)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, false, MOQ_VERSION_DRAFT_22, true, 4, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    int ps = fetch_resolve_handle(p.publisher, p.served);
    MOQ_TEST_CHECK(ps >= 0);
    if (ps < 0) goto done;
    uint32_t pg = p.publisher->fetches[ps].generation;
    uint32_t fg = p.fetcher->fetches[p.slot].generation;
    size_t pub_fins = 0, fetch_fins = 0, data_fins = 0, complete = 0;

    /* make() already proved FETCH_OK did not FIN the still-active request. */
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        p.header, p.header_len, false, ++p.now), MOQ_OK);
    moq_rcbuf_t *payload = NULL;
    static const uint8_t body[] = {'A'};
    moq_result_t rc = moq_rcbuf_create(moq_alloc_default(), body, sizeof(body), &payload);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_fetch_object_cfg_t oc;
    moq_fetch_object_cfg_init(&oc);
    oc.group_id = 10; oc.subgroup_id = 6; oc.object_id = 6;
    oc.publisher_priority = 128;
    oc.payload = payload;
    rc = moq_session_write_fetch_object(p.publisher, p.served, &oc, ++p.now);
    moq_rcbuf_decref(payload);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    size_t objects = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.publisher, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_SEND_DATA);
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            ++objects;
            MOQ_TEST_CHECK_EQ_U64(a.u.send_data.stream_ref._v, p.data._v);
            MOQ_TEST_CHECK(!a.u.send_data.fin && a.u.send_data.payload != NULL);
            MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
                a.u.send_data.header, a.u.send_data.header_len, false, ++p.now), MOQ_OK);
            if (a.u.send_data.payload)
                MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
                    moq_rcbuf_data(a.u.send_data.payload),
                    moq_rcbuf_len(a.u.send_data.payload), false, ++p.now), MOQ_OK);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(objects, 1);
    failures += observations(&p, 0, 0, 1);

    MOQ_TEST_CHECK_EQ_INT(moq_session_end_fetch(p.publisher, p.served, ++p.now), MOQ_OK);
    while (moq_session_poll_actions(p.publisher, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            ++data_fins;
            MOQ_TEST_CHECK_EQ_U64(a.u.send_data.stream_ref._v, p.data._v);
            MOQ_TEST_CHECK(a.u.send_data.fin && !a.u.send_data.payload &&
                           a.u.send_data.header_len == 0);
        } else if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++pub_fins;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p.peer_request._v);
        } else {
            MOQ_TEST_CHECK(false); /* no reset, stop, cancellation or extra message */
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(data_fins, 1);
    MOQ_TEST_CHECK_EQ_SIZE(pub_fins, 1);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.publisher, p.served) < 0);
    MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, pg + 1u);
    MOQ_TEST_CHECK(drain_ref_contains(p.publisher, p.peer_request));

    /* Data may arrive before the request FIN on the independent bidi. */
    if (data_fins == 1)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            NULL, 0, true, ++p.now), MOQ_OK);
    moq_event_t e;
    while (moq_session_poll_events(p.fetcher, &e, 1)) {
        MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_COMPLETE);
        if (e.kind == MOQ_EVENT_FETCH_COMPLETE) {
            ++complete;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_complete.fetch, p.fetch));
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(complete, 1);
    while (moq_session_poll_actions(p.fetcher, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_CLOSE_BIDI_STREAM);
        if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++fetch_fins;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p.request._v);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(fetch_fins, 1);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, fg + 1u);
    MOQ_TEST_CHECK(drain_ref_contains(p.fetcher, p.request));

    if (pub_fins == 1)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, true, ++p.now), MOQ_OK);
    if (fetch_fins == 1)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.publisher, p.peer_request,
            NULL, 0, true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK(!drain_ref_contains(p.fetcher, p.request));
    MOQ_TEST_CHECK(!drain_ref_contains(p.publisher, p.peer_request));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.publisher), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, pg + 1u);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, fg + 1u);
    failures += observations(&p, 0, 0, 0);
    failures += close_actions(&p, 0);

    moq_fetch_cfg_t fc;
    fetch_config(&fc, MOQ_GROUP_ORDER_ASCENDING);
    moq_fetch_t replacement;
    rc = moq_session_fetch(p.fetcher, &fc, ++p.now, &replacement);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc == MOQ_OK) {
        int slot = fetch_resolve_handle(p.fetcher, replacement);
        MOQ_TEST_CHECK_EQ_INT(slot, p.slot);
        if (slot >= 0)
            MOQ_TEST_CHECK(p.fetcher->fetches[slot].rs.ref._v != p.request._v);
        MOQ_TEST_CHECK(pump(&p));
        size_t requests = 0;
        while (moq_session_poll_events(p.publisher, &e, 1)) {
            MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_REQUEST);
            if (e.kind == MOQ_EVENT_FETCH_REQUEST) {
                ++requests;
                MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.publisher,
                    e.u.fetch_request.fetch), ps);
            }
            moq_event_cleanup(&e);
        }
        MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    }
    printf("MEASURED request_fin_nonempty data_fin=%zu publisher_fin=%zu requester_fin=%zu complete=%zu\n",
        data_fins, pub_fins, fetch_fins, complete);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int completion_events(fixture_t *p, size_t want_ok, size_t want_object,
                              size_t want_complete)
{
    int failures = 0;
    size_t oks = 0, objects = 0, complete = 0;
    moq_event_t e;
    while (moq_session_poll_events(p->fetcher, &e, 1)) {
        if (e.kind == MOQ_EVENT_FETCH_OK) {
            ++oks;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_ok.fetch, p->fetch));
        } else if (e.kind == MOQ_EVENT_FETCH_OBJECT) {
            ++objects;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_object.fetch, p->fetch));
        } else if (e.kind == MOQ_EVENT_FETCH_COMPLETE) {
            ++complete;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_complete.fetch, p->fetch));
        } else MOQ_TEST_CHECK(false);
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(oks, want_ok);
    MOQ_TEST_CHECK_EQ_SIZE(objects, want_object);
    MOQ_TEST_CHECK_EQ_SIZE(complete, want_complete);
    return failures;
}

static bool occupy_actions(fixture_t *p, moq_session_t *s, size_t count)
{
    moq_fetch_cfg_t cfg;
    fetch_config(&cfg, MOQ_GROUP_ORDER_ASCENDING);
    for (size_t i = 0; i < count; ++i) {
        moq_fetch_t h;
        if (moq_session_fetch(s, &cfg, ++p->now, &h) != MOQ_OK) return false;
    }
    return true;
}

static int discard_open_actions(moq_session_t *s, size_t want)
{
    int failures = 0;
    size_t n = 0;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_OPEN_BIDI_STREAM);
        ++n;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(n, want);
    return failures;
}

/* Capacity injection changes only available drain credit, never FIN/outcome
 * facts. Restore the real bound before cleanup or the successful retry. */
static int completion_pressure(unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool early_fin = mode == 3 || mode == 5;
    bool response_late = mode == 4 || mode == 5;
    bool empty_header = mode == 6;
    bool ok = make(&p, false, MOQ_VERSION_DRAFT_22, !response_late, 4,
                   mode == 1 || mode == 4 ? 1 : 2);
    size_t drain_cap = 0;
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    drain_cap = p.fetcher->drain_ref_cap;
    uint32_t gen = p.fetcher->fetches[p.slot].generation;
    MOQ_TEST_CHECK(!session_has_retained_outbound_obligation(p.fetcher));
    if (!empty_header)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, false, ++p.now), MOQ_OK);
    if (mode == 0 || early_fin || empty_header) {
        ok = occupy_actions(&p, p.fetcher, p.fetcher->action_cap);
        MOQ_TEST_CHECK(ok);
        if (!ok) goto done;
    }
    if (mode == 2) p.fetcher->drain_ref_cap = p.fetcher->drain_ref_count;
    moq_result_t rc;
    if (early_fin) {
        rc = moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            response_late ? p.response : NULL, response_late ? p.response_len : 0,
            true, ++p.now);
    } else {
        rc = moq_session_on_data_bytes(p.fetcher, p.data,
            empty_header ? p.header : first,
            empty_header ? p.header_len : sizeof(first), true, ++p.now);
        if (mode == 4) {
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            failures += completion_events(&p, 0, 1, 0);
            MOQ_TEST_CHECK(!session_has_retained_outbound_obligation(p.fetcher));
            rc = moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
                p.response, p.response_len, true, ++p.now);
        }
    }
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, gen);
    MOQ_TEST_CHECK(!p.fetcher->fetches[p.slot].rs.lifetime.local_fin);
    MOQ_TEST_CHECK(session_has_retained_outbound_obligation(p.fetcher));
    failures += completion_events(&p, response_late ? 1 : 0,
                                   !early_fin && !empty_header && mode != 4 ? 1 : 0, 0);
    failures += close_actions(&p, 0);
    /* Polling away unrelated accepted actions must not create false drain. */
    MOQ_TEST_CHECK(session_has_retained_outbound_obligation(p.fetcher));
    p.fetcher->drain_ref_cap = drain_cap;
    if (early_fin || mode == 4)
        rc = moq_session_on_bidi_stream_bytes(p.fetcher, p.request, NULL, 0, false, ++p.now);
    else
        rc = moq_session_on_data_bytes(p.fetcher, p.data, NULL, 0, false, ++p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    failures += close_actions(&p, 1);
    MOQ_TEST_CHECK(!session_has_retained_outbound_obligation(p.fetcher));
    if (early_fin) {
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
        MOQ_TEST_CHECK(p.fetcher->fetches[p.slot].rs.lifetime.local_fin);
        failures += completion_events(&p, 0, 0, 0);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, false, ++p.now), MOQ_OK);
        failures += close_actions(&p, 0);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            first, sizeof(first), true, ++p.now), MOQ_OK);
    }
    failures += completion_events(&p, 0, early_fin ? 1 : 0, 1);
    failures += close_actions(&p, 0);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, gen + 1u);
    MOQ_TEST_CHECK_EQ_INT(drain_ref_contains(p.fetcher, p.request), !early_fin && mode != 4);
    if (!early_fin && mode != 4)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            NULL, 0, true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK(!drain_ref_contains(p.fetcher, p.request));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
done:
    if (drain_cap) p.fetcher->drain_ref_cap = drain_cap;
    moq_simpair_destroy(p.pair);
    return failures;
}

static int publisher_end_pressure(unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, false, MOQ_VERSION_DRAFT_22, true, 4, 0);
    size_t drain_cap = 0;
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    int ps = fetch_resolve_handle(p.publisher, p.served);
    uint32_t gen = p.publisher->fetches[ps].generation;
    drain_cap = p.publisher->drain_ref_cap;
    if (mode == 0) {
        ok = occupy_actions(&p, p.publisher, p.publisher->action_cap - 1u);
        MOQ_TEST_CHECK(ok);
        if (!ok) goto done;
    } else p.publisher->drain_ref_cap = p.publisher->drain_ref_count;
    if (mode == 2)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.publisher, p.peer_request,
            NULL, 0, true, ++p.now), MOQ_OK);
    if (mode != 2) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_end_fetch(p.publisher, p.served, ++p.now), MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.publisher, p.served), ps);
        MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, gen);
        MOQ_TEST_CHECK(!p.publisher->fetches[ps].data_stream_fin);
        failures += discard_open_actions(p.publisher, mode == 0 ? p.publisher->action_cap - 1u : 0);
        p.publisher->drain_ref_cap = drain_cap;
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_end_fetch(p.publisher, p.served, ++p.now), MOQ_OK);
    size_t data = 0, request = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.publisher, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_DATA) {
            ++data;
            MOQ_TEST_CHECK(a.u.send_data.fin && !a.u.send_data.payload && !a.u.send_data.header_len);
            MOQ_TEST_CHECK_EQ_U64(a.u.send_data.stream_ref._v, p.data._v);
        } else if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++request;
            MOQ_TEST_CHECK_EQ_SIZE(data, 1);
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p.peer_request._v);
        } else MOQ_TEST_CHECK(false);
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(data, 1);
    MOQ_TEST_CHECK_EQ_SIZE(request, 1);
    MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, gen + 1u);
    MOQ_TEST_CHECK_EQ_INT(drain_ref_contains(p.publisher, p.peer_request), mode != 2);
    MOQ_TEST_CHECK_EQ_INT(moq_session_end_fetch(p.publisher, p.served, ++p.now), MOQ_ERR_STALE_HANDLE);
done:
    if (drain_cap) p.publisher->drain_ref_cap = drain_cap;
    moq_simpair_destroy(p.pair);
    return failures;
}

static int empty_completion(unsigned mode)
{
    int failures = 0;
    fixture_t p;
    bool ok = make_impl(&p, false, MOQ_VERSION_DRAFT_22, false, 4, 0, false);
    size_t drain_cap = 0;
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    int ps = fetch_resolve_handle(p.publisher, p.served);
    uint32_t pg = p.publisher->fetches[ps].generation;
    uint32_t fg = p.fetcher->fetches[p.slot].generation;
    drain_cap = p.publisher->drain_ref_cap;
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 20; ac.end_object = 10; ac.empty = true;
    if (mode == 2) {
        ok = occupy_actions(&p, p.publisher, p.publisher->action_cap - 1u);
        MOQ_TEST_CHECK(ok);
        if (!ok) goto done;
    }
    if (mode == 3) p.publisher->drain_ref_cap = p.publisher->drain_ref_count;
    if (mode >= 2) {
        uint64_t next_ref = p.publisher->next_stream_ref;
        size_t send_len = p.publisher->send_len;
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_fetch(p.publisher, p.served, &ac, ++p.now),
                              MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.publisher, p.served), ps);
        MOQ_TEST_CHECK_EQ_U64(p.publisher->next_stream_ref, next_ref);
        MOQ_TEST_CHECK_EQ_SIZE(p.publisher->send_len, send_len);
        MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, pg);
        failures += discard_open_actions(p.publisher, mode == 2 ? p.publisher->action_cap - 1u : 0);
        p.publisher->drain_ref_cap = drain_cap;
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_fetch(p.publisher, p.served, &ac, ++p.now), MOQ_OK);
    size_t responses = 0, headers = 0, closes = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.publisher, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            ++responses;
            MOQ_TEST_CHECK(a.u.send_bidi_stream.fin);
            MOQ_TEST_CHECK_EQ_U64(a.u.send_bidi_stream.stream_ref._v, p.peer_request._v);
            MOQ_TEST_CHECK(a.u.send_bidi_stream.len <= sizeof(p.response));
            if (a.u.send_bidi_stream.len <= sizeof(p.response)) {
                p.response_len = a.u.send_bidi_stream.len;
                memcpy(p.response, a.u.send_bidi_stream.data, p.response_len);
            }
        } else if (a.kind == MOQ_ACTION_SEND_DATA) {
            ++headers;
            MOQ_TEST_CHECK(a.u.send_data.fin && !a.u.send_data.payload);
            p.data = a.u.send_data.stream_ref;
            p.header_len = a.u.send_data.header_len;
            MOQ_TEST_CHECK(p.header_len > 0 && p.header_len <= sizeof(p.header));
            if (p.header_len <= sizeof(p.header))
                memcpy(p.header, a.u.send_data.header, p.header_len);
        } else MOQ_TEST_CHECK(false);
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(responses, 1);
    MOQ_TEST_CHECK_EQ_SIZE(headers, 1);
    if (responses != 1 || headers != 1 || p.header_len > sizeof(p.header)) goto done;
    MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, pg + 1u);
    MOQ_TEST_CHECK(drain_ref_contains(p.publisher, p.peer_request));
    if (mode == 1) {
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, true, ++p.now), MOQ_OK);
        failures += completion_events(&p, 0, 0, 0);
        MOQ_TEST_CHECK(!session_has_retained_outbound_obligation(p.fetcher));
    }
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        p.response, p.response_len, true, ++p.now), MOQ_OK);
    failures += completion_events(&p, 1, 0, mode == 1 ? 1 : 0);
    while (moq_session_poll_actions(p.fetcher, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_CLOSE_BIDI_STREAM);
        if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p.request._v);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, 1);
    if (mode != 1) {
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
            p.header, p.header_len, true, ++p.now), MOQ_OK);
        failures += completion_events(&p, 0, 0, 1);
        failures += close_actions(&p, 0);
    }
    if (closes == 1)
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.publisher, p.peer_request,
            NULL, 0, true, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, fg + 1u);
    MOQ_TEST_CHECK(!drain_ref_contains(p.publisher, p.peer_request));
    MOQ_TEST_CHECK(!drain_ref_contains(p.fetcher, p.request));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.publisher), MOQ_SESS_ESTABLISHED);
done:
    if (drain_cap) p.publisher->drain_ref_cap = drain_cap;
    moq_simpair_destroy(p.pair);
    return failures;
}

/* A completed data uni cannot retire the owner while a later buffered
 * GOAWAY still needs dispatch. Both coalesced and fragmented envelopes count. */
static int completion_goaway(bool partial)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, false, MOQ_VERSION_DRAFT_22, false, 4, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        p.header, p.header_len, false, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        first, sizeof(first), true, ++p.now), MOQ_OK);
    failures += completion_events(&p, 0, 1, 0);
    ok = send_goaway(&p);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    uint8_t bytes[256];
    memcpy(bytes, p.response, p.response_len);
    memcpy(bytes + p.response_len, p.goaway, p.goaway_len);
    size_t ga_len = partial ? 1 : p.goaway_len;
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        bytes, p.response_len + ga_len, false, ++p.now), MOQ_OK);
    if (partial) {
        failures += observations(&p, 0, 1, 0);
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
        MOQ_TEST_CHECK_EQ_SIZE(p.fetcher->fetches[p.slot].rs.recv_len, 1);
        failures += close_actions(&p, 0);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
            p.goaway + 1, p.goaway_len - 1, false, ++p.now), MOQ_OK);
    }
    failures += observations(&p, 1, partial ? 0 : 1, 0); /* never COMPLETE */
    failures += close_actions(&p, 1);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int completion_update(bool refused, bool empty)
{
    int failures = 0;
    fixture_t p;
    bool ok = make_impl(&p, false, MOQ_VERSION_DRAFT_22, !empty, 4, 0, !empty);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    int ps = fetch_resolve_handle(p.publisher, p.served);
    uint8_t bytes[128];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    moq_d22_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MOQ_D22_REQUEST_UPDATE;
    msg.request_id = 2; /* the client sent FETCH request 0 */
    moq_d22_param_t param;
    memset(&param, 0, sizeof(param));
    if (refused) {
        param.type = 0x03;
        param.value.token.alias_type = 2;
        param.value.token.alias = 99; /* unknown alias: actual auth refusal */
    } else {
        param.type = 0x20;
        param.value.integer = 3;
    }
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request_type = MOQ_D22_FETCH;
    ctx.update_from_subscriber = true;
    moq_result_t rc = moq_d22_encode_message(&w, &msg, &param, 1, &ctx);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    if (!empty) {
        ok = occupy_actions(&p, p.publisher, p.publisher->action_cap);
        MOQ_TEST_CHECK(ok);
        if (!ok) goto done;
    }
    rc = moq_session_on_bidi_stream_bytes(p.publisher, p.peer_request,
        bytes, moq_buf_writer_offset(&w), false, ++p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, empty ? MOQ_OK : MOQ_ERR_WOULD_BLOCK);
    if (!empty) {
        MOQ_TEST_CHECK_EQ_SIZE(p.publisher->fetches[ps].rs.recv_len, moq_buf_writer_offset(&w));
        MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.publisher, p.served), ps);
        failures += discard_open_actions(p.publisher, p.publisher->action_cap);
        /* No explicit refeed: end itself must resolve the retained UPDATE. */
        rc = moq_session_end_fetch(p.publisher, p.served, ++p.now);
        MOQ_TEST_CHECK_EQ_INT(rc, refused ? MOQ_ERR_STALE_HANDLE : MOQ_OK);
    } else {
        MOQ_TEST_CHECK_EQ_SIZE(p.publisher->fetches[ps].upd_owed, 1);
        moq_accept_fetch_cfg_t ac;
        moq_accept_fetch_cfg_init(&ac);
        ac.end_group = 20; ac.end_object = 10; ac.empty = true;
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_fetch(p.publisher, p.served, &ac, ++p.now), MOQ_OK);
    }
    const moq_request_codec_t *codec = p.publisher->profile->request_codec;
    uint64_t types[2] = {0, 0};
    size_t messages = 0, data = 0, fins = 0, resets = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.publisher, &a, 1)) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            MOQ_TEST_CHECK_EQ_U64(a.u.send_bidi_stream.stream_ref._v, p.peer_request._v);
            MOQ_TEST_CHECK_EQ_INT(a.u.send_bidi_stream.fin, refused || empty);
            size_t off = 0;
            while (off < a.u.send_bidi_stream.len) {
                moq_request_envelope_t env;
                size_t used = 0;
                rc = codec->decode_envelope(a.u.send_bidi_stream.data + off,
                    a.u.send_bidi_stream.len - off, &env, &used);
                MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
                MOQ_TEST_CHECK(used > 0);
                if (rc != MOQ_OK || !used) break;
                if (messages < 2) types[messages] = env.msg_type;
                ++messages;
                off += used;
            }
            if (a.u.send_bidi_stream.fin) ++fins;
        } else if (a.kind == MOQ_ACTION_SEND_DATA) {
            ++data;
            MOQ_TEST_CHECK_EQ_SIZE(messages, empty ? 2 : 1);
            MOQ_TEST_CHECK_EQ_INT(a.u.send_data.fin, !refused);
            MOQ_TEST_CHECK(!a.u.send_data.payload);
            MOQ_TEST_CHECK_EQ_INT(a.u.send_data.header_len > 0, empty);
        } else if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
            ++fins;
            MOQ_TEST_CHECK(!refused && !empty);
            MOQ_TEST_CHECK_EQ_SIZE(messages, 1);
            MOQ_TEST_CHECK_EQ_SIZE(data, 1);
            MOQ_TEST_CHECK_EQ_U64(a.u.close_bidi_stream.stream_ref._v, p.peer_request._v);
        } else if (a.kind == MOQ_ACTION_RESET_DATA) {
            ++resets;
            MOQ_TEST_CHECK(refused);
            if (!empty) MOQ_TEST_CHECK_EQ_U64(a.u.reset_data.stream_ref._v, p.data._v);
        } else MOQ_TEST_CHECK(false);
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(messages, empty ? 2 : 1);
    if (empty) MOQ_TEST_CHECK_EQ_U64(types[0], codec->type.fetch_ok);
    MOQ_TEST_CHECK_EQ_U64(types[empty ? 1 : 0], refused ? codec->type.request_error : codec->type.request_ok);
    MOQ_TEST_CHECK_EQ_SIZE(data, empty || !refused ? 1 : 0);
    MOQ_TEST_CHECK_EQ_SIZE(fins, 1);
    MOQ_TEST_CHECK_EQ_SIZE(resets, refused ? 1 : 0);
    size_t cancelled = 0;
    moq_event_t e;
    while (moq_session_poll_events(p.publisher, &e, 1)) {
        MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_CANCELLED);
        if (e.kind == MOQ_EVENT_FETCH_CANCELLED) {
            ++cancelled;
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_cancelled.fetch, p.served));
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(cancelled, refused ? 1 : 0);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.publisher, p.served) < 0);
    MOQ_TEST_CHECK(drain_ref_contains(p.publisher, p.peer_request));
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.publisher), MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int closed_request_data_terminal(bool reset)
{
    int failures = 0;
    fixture_t p;
    bool ok = make(&p, false, MOQ_VERSION_DRAFT_22, true, 4, 0);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_data_bytes(p.fetcher, p.data,
        p.header, p.header_len, false, ++p.now), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(p.fetcher, p.request,
        NULL, 0, true, ++p.now), MOQ_OK);
    failures += close_actions(&p, 1);
    MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
    MOQ_TEST_CHECK(p.fetcher->fetches[p.slot].rs.lifetime.local_fin);
    moq_result_t rc = reset
        ? moq_session_on_data_reset(p.fetcher, p.data, 7, ++p.now)
        : moq_session_fetch_cancel(p.fetcher, p.fetch, ++p.now);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    size_t stops = 0, resets = 0;
    moq_action_t a;
    while (moq_session_poll_actions(p.fetcher, &a, 1)) {
        MOQ_TEST_CHECK_EQ_INT(a.kind, MOQ_ACTION_STOP_DATA);
        if (a.kind == MOQ_ACTION_STOP_DATA) {
            ++stops;
            MOQ_TEST_CHECK_EQ_U64(a.u.stop_data.stream_ref._v, p.data._v);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_SIZE(stops, reset ? 0 : 1);
    moq_event_t e;
    while (moq_session_poll_events(p.fetcher, &e, 1)) {
        MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_RESET);
        if (e.kind == MOQ_EVENT_FETCH_RESET) {
            ++resets;
            MOQ_TEST_CHECK(e.u.fetch_reset.data_stream);
            MOQ_TEST_CHECK_EQ_U64(e.u.fetch_reset.error_code, 7);
            MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_reset.fetch, p.fetch));
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_SIZE(resets, reset ? 1 : 0);
    MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
    MOQ_TEST_CHECK_EQ_SIZE(p.fetcher->drain_ref_count, 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

static int completion_reuse(void)
{
    int failures = 0;
    fixture_t p;
    bool ok = make_impl(&p, false, MOQ_VERSION_DRAFT_22, false, 4, 0, false);
    MOQ_TEST_CHECK(ok);
    if (!ok) goto done;
    int ps = fetch_resolve_handle(p.publisher, p.served);
    for (unsigned i = 0; i < 8; ++i) {
        if (i != 0) {
            moq_fetch_cfg_t fc;
            fetch_config(&fc, MOQ_GROUP_ORDER_ASCENDING);
            moq_result_t rc = moq_session_fetch(p.fetcher, &fc, ++p.now, &p.fetch);
            MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
            if (rc != MOQ_OK) goto done;
            MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.fetcher, p.fetch), p.slot);
            MOQ_TEST_CHECK(pump(&p));
            size_t requests = 0;
            moq_event_t e;
            while (moq_session_poll_events(p.publisher, &e, 1)) {
                MOQ_TEST_CHECK_EQ_INT(e.kind, MOQ_EVENT_FETCH_REQUEST);
                if (e.kind == MOQ_EVENT_FETCH_REQUEST) {
                    ++requests;
                    p.served = e.u.fetch_request.fetch;
                }
                moq_event_cleanup(&e);
            }
            MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
            if (requests != 1) goto done;
            MOQ_TEST_CHECK_EQ_INT(fetch_resolve_handle(p.publisher, p.served), ps);
        }
        uint32_t fg = p.fetcher->fetches[p.slot].generation;
        uint32_t pg = p.publisher->fetches[ps].generation;
        moq_accept_fetch_cfg_t ac;
        moq_accept_fetch_cfg_init(&ac);
        ac.end_group = 20; ac.end_object = 10; ac.empty = true;
        MOQ_TEST_CHECK_EQ_INT(moq_session_accept_fetch(p.publisher, p.served, &ac, ++p.now), MOQ_OK);
        MOQ_TEST_CHECK(pump(&p));
        failures += completion_events(&p, 1, 0, 1);
        MOQ_TEST_CHECK(fetch_resolve_handle(p.fetcher, p.fetch) < 0);
        MOQ_TEST_CHECK(fetch_resolve_handle(p.publisher, p.served) < 0);
        MOQ_TEST_CHECK_EQ_U64(p.fetcher->fetches[p.slot].generation, fg + 1u);
        MOQ_TEST_CHECK_EQ_U64(p.publisher->fetches[ps].generation, pg + 1u);
        MOQ_TEST_CHECK_EQ_SIZE(p.fetcher->drain_ref_count, 0);
        MOQ_TEST_CHECK_EQ_SIZE(p.publisher->drain_ref_count, 0);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.fetcher), MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(p.publisher), MOQ_SESS_ESTABLISHED);
        if (failures) goto done;
    }
done:
    moq_simpair_destroy(p.pair);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--request-fin") == 0) {
        int n = request_fin_completion();
        printf("CASE public_fetch_request_fin_nonempty: %s (%d failed checks)\n",
            n ? "FAIL" : "PASS", n);
        return n != 0;
    }
    if (argc != 1) return 2;
    int failures = 0;
    size_t cases = 0;
    for (unsigned role = 0; role < 2; ++role) {
        for (unsigned header = 0; header < 2; ++header)
            for (unsigned mode = 0; mode < 4; ++mode) {
                int n = migration(role != 0, header != 0, mode);
                printf("CASE public_fetch_migration_%s_%s_%s: %s\n",
                    role ? "server" : "client", header ? "bound" : "late_header",
                    mode == 0 ? "split_fin" : mode == 1 ? "coalesced_fin" :
                    mode == 2 ? "coalesced_ok_goaway" : "bytewise_goaway", n ? "FAIL" : "PASS");
                failures += n; ++cases;
            }
        for (unsigned mode = 0; mode < 4; ++mode) {
            int n = pressure(role != 0, mode);
            printf("CASE public_fetch_goaway_%s_%s_pressure: %s\n", role ? "server" : "client",
                mode == 0 ? "event_fin" : mode == 1 ? "action" : mode == 2 ? "event_action_fin" :
                "coalesced_ok_goaway", n ? "FAIL" : "PASS");
            failures += n; ++cases;
        }
        for (unsigned mode = 0; mode < 9; ++mode) {
            if (mode == 6 && !role) continue;
            static const char *const names[] = {"missing_uri", "missing_timeout", "extra_tail",
                "duplicate_ok", "wrong_response", "wrong_message", "client_uri",
                "duplicate_goaway", "bytewise_duplicate_goaway"};
            int n = protocol_error(role != 0, mode);
            printf("CASE public_fetch_%s_%s: %s\n", role ? "server" : "client", names[mode], n ? "FAIL" : "PASS");
            failures += n; ++cases;
        }
        int n = truncated_fin(role != 0);
        printf("CASE public_fetch_%s_truncated_goaway_fin: %s\n", role ? "server" : "client", n ? "FAIL" : "PASS");
        failures += n; ++cases;
        n = legacy_guard(role != 0);
        printf("CASE unchanged_d18_%s_post_response_guard: %s\n", role ? "server" : "client", n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    int n = request_fin_completion();
    printf("CASE public_fetch_request_fin_nonempty: %s\n", n ? "FAIL" : "PASS");
    failures += n; ++cases;
    for (unsigned mode = 0; mode < 7; ++mode) {
        static const char *const names[] = {"data_action", "data_event", "data_drain",
            "early_request_fin_action", "data_before_control_event", "coalesced_request_fin_action",
            "empty_header_fin_action"};
        n = completion_pressure(mode);
        printf("CASE completion_%s: %s\n", names[mode], n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        static const char *const names[] = {"one_action_left", "no_drain_credit", "peer_fin_no_drain_credit"};
        n = publisher_end_pressure(mode);
        printf("CASE publisher_end_%s: %s\n", names[mode], n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        static const char *const names[] = {"control_first", "data_first", "one_action_left", "no_drain_credit"};
        n = empty_completion(mode);
        printf("CASE empty_completion_%s: %s\n", names[mode], n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    for (unsigned partial = 0; partial < 2; ++partial) {
        n = completion_goaway(partial != 0);
        printf("CASE completed_data_%s_goaway: %s\n", partial ? "partial" : "coalesced", n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    for (unsigned empty = 0; empty < 2; ++empty)
        for (unsigned refused = 0; refused < 2; ++refused) {
            n = completion_update(refused != 0, empty != 0);
            printf("CASE %s_%s_update_before_finish: %s\n", empty ? "empty" : "retained",
                refused ? "refused" : "accepted", n ? "FAIL" : "PASS");
            failures += n; ++cases;
        }
    for (unsigned reset = 0; reset < 2; ++reset) {
        n = closed_request_data_terminal(reset != 0);
        printf("CASE early_request_fin_then_data_%s: %s\n", reset ? "reset" : "cancel", n ? "FAIL" : "PASS");
        failures += n; ++cases;
    }
    n = completion_reuse();
    printf("CASE eight_completed_fetch_slot_reuses: %s\n", n ? "FAIL" : "PASS");
    failures += n; ++cases;
    printf("fetch_goaway_after_response: %zu cases, %d failed checks\n", cases, failures);
    return failures != 0;
}
