/* Publisher-default 0x22 range validation: D16 11.1.1.2, D18 12.5/12.7.
 * Narrowed from the SimPair/raw-input approach in PR46 test_group_order.c,
 * test donor b2d66ac97f5ece1051cc34b89832db13f1b6c0a2 by Joaquin Bartaburu
 * <joaquinb@quqlabs.com>. Carrier completion, raw builders, immutable,
 * omission, width, unrelated-property and cancellation controls are local
 * additions. No negotiation, scheduling or duplicate-precedence assertions.
 */
#include <moq/moq.h>
#include <moq/control.h>
#include <moq/control_d18.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

static int failures;
static unsigned rows;

typedef enum { SUB_OK, PUBLISH, FETCH_OK, STATUS_OK } carrier_t;
typedef enum {
    OUTER, IMMUTABLE, OMITTED, UNRELATED, PRECEDED, REPEATED,
    INVALID_INNER, INVALID_OUTER
} shape_t;

typedef struct {
    uint64_t id;
    moq_stream_ref_t ref;
    moq_subscription_t sub;
    moq_fetch_t fetch;
    moq_track_status_handle_t status;
} request_t;

static const char *const carriers[] = {
    "SUBSCRIBE_OK", "PUBLISH", "FETCH_OK", "TRACK_STATUS_OK"
};
static const char *const shapes[] = {
    "outer", "immutable", "omitted", "unrelated", "preceded", "repeated",
    "invalid-inner", "invalid-outer"
};

/* These builders only serialize framing/scalars. They deliberately do not
 * call a message encoder that could reject the property before peer input. */
#define PUT(call) do { \
    moq_result_t put_rc = (call); \
    MOQ_TEST_CHECK_EQ_INT((int)put_rc, (int)MOQ_OK); \
    if (put_rc != MOQ_OK) return 0; \
} while (0)

static size_t properties(uint8_t *buf, size_t cap, moq_version_t version,
                         shape_t shape, uint64_t value)
{
    moq_result_t (*write_int)(moq_buf_writer_t *, uint64_t) =
        version == MOQ_VERSION_DRAFT_18 ? moq_buf_write_vi64 : moq_buf_write_varint;
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    if (shape == OMITTED) return 0;
    if (shape == UNRELATED || shape == PRECEDED) {
        PUT(write_int(&w, 0x04)); /* MAX_CACHE_DURATION, not 0x22 */
        PUT(write_int(&w, shape == UNRELATED ? value : 0));
        if (shape == UNRELATED) return moq_buf_writer_offset(&w);
    }
    if (shape == IMMUTABLE || shape == INVALID_INNER || shape == INVALID_OUTER) {
        uint8_t inner[16];
        moq_buf_writer_t iw;
        moq_buf_writer_init(&iw, inner, sizeof(inner));
        PUT(write_int(&iw, 0x22));
        PUT(write_int(&iw, shape == INVALID_OUTER ? 1 : value));
        PUT(write_int(&w, 0x0b));
        PUT(write_int(&w, moq_buf_writer_offset(&iw)));
        PUT(moq_buf_write_raw(&w, inner, moq_buf_writer_offset(&iw)));
        if (shape == IMMUTABLE) return moq_buf_writer_offset(&w);
        PUT(write_int(&w, 0x22 - 0x0b));
        PUT(write_int(&w, shape == INVALID_INNER ? 1 : value));
    } else {
        PUT(write_int(&w, shape == PRECEDED ? 0x22 - 0x04 : 0x22));
        PUT(write_int(&w, shape == REPEATED ? 1 : value));
        if (shape == REPEATED) {
            PUT(write_int(&w, 0)); /* same type; no precedence claim */
            PUT(write_int(&w, value));
        }
    }
    return moq_buf_writer_offset(&w);
}

static size_t message(uint8_t *buf, size_t cap, moq_version_t version,
                      carrier_t carrier, uint64_t id,
                      const uint8_t *props, size_t plen)
{
    bool d18 = version == MOQ_VERSION_DRAFT_18;
    moq_result_t (*write_int)(moq_buf_writer_t *, uint64_t) =
        d18 ? moq_buf_write_vi64 : moq_buf_write_varint;
    uint64_t type = carrier == SUB_OK ? (d18 ? MOQ_D18_SUBSCRIBE_OK : MOQ_D16_SUBSCRIBE_OK)
                  : carrier == PUBLISH ? (d18 ? MOQ_D18_PUBLISH : MOQ_D16_PUBLISH)
                  : carrier == FETCH_OK ? (d18 ? MOQ_D18_FETCH_OK : MOQ_D16_FETCH_OK)
                  : MOQ_D18_REQUEST_OK;
    moq_buf_writer_t w;
    size_t length_offset = 0;
    moq_buf_writer_init(&w, buf, cap);
    PUT(write_int(&w, type));
    PUT(moq_buf_reserve_uint16(&w, &length_offset));
    size_t start = moq_buf_writer_offset(&w);
    if (!d18 || carrier == PUBLISH) PUT(write_int(&w, id));
    if (carrier == PUBLISH) {
        PUT(write_int(&w, 1));
        PUT(write_int(&w, 4));
        PUT(moq_buf_write_raw(&w, (const uint8_t *)"live", 4));
        PUT(write_int(&w, 1));
        PUT(moq_buf_write_raw(&w, (const uint8_t *)"v", 1));
    }
    if (carrier == SUB_OK || carrier == PUBLISH)
        PUT(write_int(&w, 7)); /* track alias */
    if (carrier == FETCH_OK) {
        const uint8_t eot = 0;
        PUT(moq_buf_write_raw(&w, &eot, 1));
        PUT(write_int(&w, 1)); /* exclusive end {1,0} */
        PUT(write_int(&w, 0));
    }
    PUT(write_int(&w, 0)); /* parameter count, not properties count */
    PUT(moq_buf_write_raw(&w, props, plen));
    PUT(moq_buf_patch_uint16(&w, length_offset,
                            (uint16_t)(moq_buf_writer_offset(&w) - start)));
    return moq_buf_writer_offset(&w);
}
#undef PUT

static void drain(moq_session_t *s)
{
    moq_action_t a;
    moq_event_t e;
    while (moq_session_poll_actions(s, &a, 1)) moq_action_cleanup(&a);
    while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}

static moq_simpair_t *pair(moq_version_t version)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.seed = 1;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    moq_result_t rc = moq_simpair_create(&cfg, &sp);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK || !sp) { MOQ_TEST_CHECK(sp != NULL); return NULL; }
    rc = moq_simpair_start(sp);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto fail;
    rc = moq_simpair_run_until_quiescent(sp, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto fail;
    if (moq_session_state(moq_simpair_client(sp)) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(moq_simpair_server(sp)) != MOQ_SESS_ESTABLISHED) {
        MOQ_TEST_CHECK(false);
        goto fail;
    }
    drain(moq_simpair_client(sp));
    drain(moq_simpair_server(sp));
    return sp;
fail:
    moq_simpair_destroy(sp);
    return NULL;
}

static bool open_request(moq_session_t *s, moq_version_t version,
                         carrier_t carrier, uint64_t now, request_t *r)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("live");
    moq_namespace_t ns = { &part, 1 };
    moq_result_t rc;
    if (carrier == SUB_OK) {
        moq_subscribe_cfg_t c;
        moq_subscribe_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = MOQ_BYTES_LITERAL("v");
        c.group_order = MOQ_GROUP_ORDER_DEFAULT;
        rc = moq_session_subscribe(s, &c, now, &r->sub);
    } else if (carrier == FETCH_OK) {
        moq_fetch_cfg_t c;
        moq_fetch_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = MOQ_BYTES_LITERAL("v");
        c.end_group = 1;
        rc = moq_session_fetch(s, &c, now, &r->fetch);
    } else {
        moq_track_status_cfg_t c;
        moq_track_status_cfg_init(&c);
        c.track_namespace = ns;
        c.track_name = MOQ_BYTES_LITERAL("v");
        rc = moq_session_track_status(s, &c, now, &r->status);
    }
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) return false;
    unsigned found = 0;
    bool decoded = true;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (version == MOQ_VERSION_DRAFT_18 && a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) {
            data = a.u.open_bidi_stream.data;
            len = a.u.open_bidi_stream.len;
            r->ref = a.u.open_bidi_stream.stream_ref;
        } else if (version == MOQ_VERSION_DRAFT_16 && a.kind == MOQ_ACTION_SEND_CONTROL) {
            data = a.u.send_control.data;
            len = a.u.send_control.len;
        }
        if (data) {
            moq_buf_reader_t rd;
            moq_control_envelope_t env;
            moq_buf_reader_init(&rd, data, len);
            rc = version == MOQ_VERSION_DRAFT_18
                ? moq_d18_decode_envelope(&rd, &env) : moq_control_decode_envelope(&rd, &env);
            uint64_t want = carrier == SUB_OK ? (version == MOQ_VERSION_DRAFT_18 ? MOQ_D18_SUBSCRIBE : MOQ_D16_SUBSCRIBE)
                          : carrier == FETCH_OK ? (version == MOQ_VERSION_DRAFT_18 ? MOQ_D18_FETCH : MOQ_D16_FETCH)
                          : MOQ_D18_TRACK_STATUS;
            MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
            if (rc == MOQ_OK && env.msg_type == want) {
                found++;
                if (carrier == SUB_OK) {
                    /* API DEFAULT is zero; its wire representation is an
                     * omitted request parameter, not property 0x22 = 0. */
                    moq_bytes_t parts[4];
                    if (version == MOQ_VERSION_DRAFT_18) {
                        moq_d18_subscribe_t sub;
                        rc = moq_d18_decode_subscribe(env.payload, env.payload_len,
                                                     parts, 4, &sub);
                        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
                        if (rc == MOQ_OK) MOQ_TEST_CHECK(!sub.params.has_group_order);
                    } else {
                        moq_kvp_entry_t params[8];
                        moq_d16_subscribe_t sub = { .params = params, .params_cap = 8 };
                        rc = moq_d16_decode_subscribe(env.payload, env.payload_len,
                                                     parts, 4, &sub);
                        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
                        if (rc == MOQ_OK)
                            for (size_t i = 0; i < sub.params_count; i++)
                                MOQ_TEST_CHECK(params[i].type != MOQ_MSG_PARAM_GROUP_ORDER);
                    }
                    if (rc != MOQ_OK) decoded = false;
                }
                moq_buf_reader_init(&rd, env.payload, env.payload_len);
                rc = version == MOQ_VERSION_DRAFT_18
                    ? moq_buf_read_vi64(&rd, &r->id) : moq_buf_read_varint(&rd, &r->id);
                MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
                if (rc != MOQ_OK) decoded = false;
            } else decoded = false;
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT((int)found, 1);
    return decoded && found == 1;
}

static void run_row(moq_version_t version, carrier_t carrier, shape_t shape,
                    uint64_t value, bool invalid, bool cancelled)
{
    int before = failures;
    rows++;
    printf("ROW d%d %s %s value=%llu cancelled=%d expect=%s\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18, carriers[carrier],
           shapes[shape], (unsigned long long)value, (int)cancelled,
           invalid ? "session-close-0x3" : cancelled ? "ignored-live-session" : "event-live-session");
    moq_simpair_t *sp = pair(version);
    if (!sp) return;
    moq_session_t *s = carrier == PUBLISH ? moq_simpair_server(sp) : moq_simpair_client(sp);
    moq_session_t *peer = carrier == PUBLISH ? moq_simpair_client(sp) : moq_simpair_server(sp);
    uint64_t now = moq_simpair_now_us(sp);
    request_t r = {0};
    /* Incoming PUBLISH is the client's first request on this fresh pair. */
    r.ref = moq_stream_ref_from_u64(0x7f000000u);
    if (carrier != PUBLISH && !open_request(s, version, carrier, now, &r)) goto done;
    if (cancelled) {
        moq_result_t rc = carrier == SUB_OK
            ? moq_session_unsubscribe(s, r.sub, now)
            : moq_session_fetch_cancel(s, r.fetch, now);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
        if (rc != MOQ_OK) goto done;
        drain(s);
    }
    uint8_t props[64], wire[256];
    size_t plen = properties(props, sizeof(props), version, shape, value);
    if (shape != OMITTED && plen == 0) goto done;
    size_t n = message(wire, sizeof(wire), version, carrier, r.id, props, plen);
    MOQ_TEST_CHECK(n > 0);
    if (!n || failures != before) goto done;
    moq_result_t rc = version == MOQ_VERSION_DRAFT_18
        ? moq_session_on_bidi_stream_bytes(s, r.ref, wire, n, false, now)
        : moq_session_on_control_bytes(s, wire, n, now);
    /* Session-close paths may report the protocol error directly or consume
     * it into CLOSE_SESSION. Neither return alone establishes the verdict. */
    MOQ_TEST_CHECK(rc == MOQ_OK || (invalid && rc == MOQ_ERR_PROTO));
    unsigned success = 0, closed = 0, other = 0, close_actions = 0;
    moq_event_t e;
    uint32_t want = carrier == SUB_OK ? MOQ_EVENT_SUBSCRIBE_OK
                  : carrier == PUBLISH ? MOQ_EVENT_PUBLISH_REQUEST
                  : carrier == FETCH_OK ? MOQ_EVENT_FETCH_OK : MOQ_EVENT_TRACK_STATUS_OK;
    while (moq_session_poll_events(s, &e, 1)) {
        if (e.kind == want) {
            moq_bytes_t actual = {0};
            success++;
            if (carrier == SUB_OK) {
                MOQ_TEST_CHECK(moq_subscription_eq(e.u.subscribe_ok.sub, r.sub));
                actual = e.u.subscribe_ok.track_properties;
            } else if (carrier == FETCH_OK) {
                MOQ_TEST_CHECK(moq_fetch_eq(e.u.fetch_ok.fetch, r.fetch));
                actual = e.u.fetch_ok.track_properties;
            } else if (carrier == STATUS_OK) {
                MOQ_TEST_CHECK(moq_track_status_handle_eq(e.u.track_status_ok.handle,
                                                        r.status));
                actual = e.u.track_status_ok.track_properties;
            } else {
                MOQ_TEST_CHECK_EQ_U64(e.u.publish_request.track_alias, 7);
                actual = e.u.publish_request.track_properties;
            }
            MOQ_TEST_CHECK_EQ_SIZE(actual.len, plen);
            if (plen && actual.len == plen)
                MOQ_TEST_CHECK(actual.data && memcmp(actual.data, props, plen) == 0);
        } else if (e.kind == MOQ_EVENT_SESSION_CLOSED) {
            closed++;
            MOQ_TEST_CHECK_EQ_U64(e.u.closed.code, 0x3);
        } else other++;
        moq_event_cleanup(&e);
    }
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1)) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION) {
            close_actions++;
            MOQ_TEST_CHECK_EQ_U64(a.u.close_session.code, 0x3);
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT((int)success, invalid || cancelled ? 0 : 1);
    MOQ_TEST_CHECK_EQ_INT((int)closed, invalid ? 1 : 0);
    MOQ_TEST_CHECK_EQ_INT((int)close_actions, invalid ? 1 : 0);
    MOQ_TEST_CHECK_EQ_INT((int)other, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(s), invalid ? MOQ_SESS_CLOSED : MOQ_SESS_ESTABLISHED);
    /* No sim pump after injection: do not mistake propagation of a local
     * close to the peer for a second independent decoder verdict. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(peer), MOQ_SESS_ESTABLISHED);
done:
    if (failures != before)
        fprintf(stderr, "ROW FAILED d%d %s %s value=%llu cancelled=%d\n",
                version == MOQ_VERSION_DRAFT_16 ? 16 : 18, carriers[carrier],
                shapes[shape], (unsigned long long)value, (int)cancelled);
    moq_simpair_destroy(sp);
}

static void selected_row(int mode, moq_version_t v, carrier_t c, shape_t s,
                         uint64_t value, bool invalid, bool cancelled)
{
    if ((mode == 1 && invalid) || (mode == 2 && !invalid)) return;
    run_row(v, c, s, value, invalid, cancelled);
}

static void default_request_control(moq_version_t version)
{
    rows++;
    printf("ROW d%d SUBSCRIBE API-default-zero expect=request-live-session\n",
           version == MOQ_VERSION_DRAFT_16 ? 16 : 18);
    moq_simpair_t *sp = pair(version);
    if (!sp) return;
    moq_session_t *client = moq_simpair_client(sp);
    moq_session_t *server = moq_simpair_server(sp);
    moq_bytes_t part = MOQ_BYTES_LITERAL("live");
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.group_order = MOQ_GROUP_ORDER_DEFAULT;
    MOQ_TEST_CHECK_EQ_INT((int)cfg.group_order, 0);
    moq_subscription_t sub = {0};
    moq_result_t rc = moq_session_subscribe(client, &cfg, moq_simpair_now_us(sp), &sub);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(sp, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
    if (rc != MOQ_OK) goto done;
    unsigned count = 0;
    moq_event_t e;
    while (moq_session_poll_events(server, &e, 1)) {
        MOQ_TEST_CHECK_EQ_INT((int)e.kind, (int)MOQ_EVENT_SUBSCRIBE_REQUEST);
        if (e.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
            count++;
            MOQ_TEST_CHECK_EQ_INT((int)e.u.subscribe_request.group_order,
                                  (int)MOQ_GROUP_ORDER_DEFAULT);
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK_EQ_INT((int)count, 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(client), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(server), (int)MOQ_SESS_ESTABLISHED);
done:
    moq_simpair_destroy(sp);
}

int main(int argc, char **argv)
{
    int mode = 0;
    if (argc == 2 && strcmp(argv[1], "--controls") == 0) mode = 1;
    else if (argc == 2 && strcmp(argv[1], "--invalid") == 0) mode = 2;
    else if (argc != 1) return 2;
    for (int draft = 0; draft < 2; draft++) {
        moq_version_t v = draft ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_16;
        for (int ci = SUB_OK; ci <= (draft ? STATUS_OK : FETCH_OK); ci++) {
            carrier_t c = (carrier_t)ci;
            selected_row(mode, v, c, OMITTED, 0, false, false);
            for (uint64_t value = 0; value <= 3; value++) {
                bool bad = value == 0 || value == 3;
                selected_row(mode, v, c, OUTER, value, bad, false);
                if (draft) selected_row(mode, v, c, IMMUTABLE, value, bad, false);
            }
            selected_row(mode, v, c, UNRELATED, 0, false, false);
            selected_row(mode, v, c, UNRELATED, 3, false, false);
            selected_row(mode, v, c, UNRELATED, 257, false, false);
            selected_row(mode, v, c, PRECEDED, 0, true, false);
            selected_row(mode, v, c, PRECEDED, 1, false, false);
            selected_row(mode, v, c, OUTER, 257, true, false);
            selected_row(mode, v, c, REPEATED, 2, false, false);
            selected_row(mode, v, c, REPEATED, 0, true, false);
            if (draft) {
                selected_row(mode, v, c, INVALID_INNER, 0, true, false);
                selected_row(mode, v, c, INVALID_OUTER, 0, true, false);
                selected_row(mode, v, c, IMMUTABLE, 257, true, false);
            }
        }
    }
    for (int ci = 0; ci < 2; ci++) {
        carrier_t c = ci ? FETCH_OK : SUB_OK;
        selected_row(mode, MOQ_VERSION_DRAFT_16, c, OMITTED, 0, false, true);
        for (uint64_t value = 0; value <= 3; value++)
            selected_row(mode, MOQ_VERSION_DRAFT_16, c, OUTER, value,
                         value == 0 || value == 3, true);
    }
    if (mode != 2) {
        default_request_control(MOQ_VERSION_DRAFT_16);
        default_request_control(MOQ_VERSION_DRAFT_18);
    }
    printf("publisher_group_order_wire: %u rows, %d failures\n", rows, failures);
    return failures ? 1 : 0;
}
