/*
 * INCLUDE_PROPERTIES (draft-22 §9.20.21): "It specifies whether the OK
 * message sent in response includes Track Properties ... If
 * INCLUDE_PROPERTIES is 0, the Track Properties are still present in the
 * message, but they SHOULD be empty." The session honours it for the answers
 * it sends with application-supplied properties: SUBSCRIBE_OK and FETCH_OK
 * carry none for a request that asked for none, and the application's
 * properties otherwise (the default is 1).
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <string.h>

/* One Track Property in the application range (type 0x78, value 1). */
static const uint8_t k_props[] = { 0x78, 0x01 };

static moq_simpair_t *mk_pair(void)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    return sp;
}

/* Feed the server a SUBSCRIBE or FETCH for live/v, with INCLUDE_PROPERTIES
 * set to `include` (or absent when include < 0). */
static moq_result_t feed_request(moq_session_t *sv, uint64_t type, uint64_t ref,
                                 int include)
{
    uint8_t ns_buf[16], buf[96];
    moq_buf_writer_t nw;
    moq_buf_writer_init(&nw, ns_buf, sizeof(ns_buf));
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { parts, 1 };
    if (moq_d22_encode_namespace(&nw, &ns) != MOQ_OK) return MOQ_ERR_INVAL;
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.request_id = 0;
    m.ns.encoded.data = ns_buf;
    m.ns.encoded.len = moq_buf_writer_offset(&nw);
    m.ns.count = 1;
    m.track_name = MOQ_BYTES_LITERAL("v");
    moq_d22_param_t p[2];
    memset(p, 0, sizeof(p));
    size_t np = 0;
    if (type == MOQ_D22_FETCH) {
        /* An explicit range, so nothing is resolved against track history. */
        p[np].type = 0x21;
        p[np].value.filter.type = MOQ_D22_FILTER_GROUP_END;
        p[np].value.filter.start_group = 1;
        np++;
    }
    if (include >= 0) { p[np].type = 0x35; p[np].value.integer = (uint64_t)include; np++; }
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    if (moq_d22_encode_message(&w, &m, np ? p : NULL, np, &ctx) != MOQ_OK) return MOQ_ERR_INVAL;
    return moq_session_on_bidi_stream_bytes(sv, moq_stream_ref_from_u64(ref), buf,
                                            moq_buf_writer_offset(&w), false, 10);
}

/* The Track Properties length of the `type` message the server queued on
 * `ref`, or SIZE_MAX when none was queued. */
static size_t sent_properties_len(moq_session_t *sv, uint64_t ref, uint64_t type)
{
    size_t len = SIZE_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK && env.msg_type == type &&
                moq_d22_decode_message(type, env.payload, env.payload_len, &ctx, &m) == MOQ_OK)
                len = m.properties.len;
        }
        moq_action_cleanup(&a);
    }
    return len;
}

static size_t subscribe_ok_properties(int include)
{
    moq_simpair_t *sp = mk_pair();
    if (!sp) return SIZE_MAX - 1;
    moq_session_t *sv = moq_simpair_server(sp);
    size_t len = SIZE_MAX - 2;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) moq_event_cleanup(&ev);
    if (feed_request(sv, MOQ_D22_SUBSCRIBE, 0x7000, include) != MOQ_OK) goto out;
    moq_subscription_t h = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) h = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t ac; moq_accept_subscribe_cfg_init(&ac);
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    if (moq_session_accept_subscribe(sv, h, &ac, 11) != MOQ_OK) goto out;
    len = sent_properties_len(sv, 0x7000, MOQ_D22_SUBSCRIBE_OK);
out:
    moq_simpair_destroy(sp);
    return len;
}

static size_t fetch_ok_properties(int include)
{
    moq_simpair_t *sp = mk_pair();
    if (!sp) return SIZE_MAX - 1;
    moq_session_t *sv = moq_simpair_server(sp);
    size_t len = SIZE_MAX - 2;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) moq_event_cleanup(&ev);
    if (feed_request(sv, MOQ_D22_FETCH, 0x7000, include) != MOQ_OK) goto out;
    moq_fetch_t h = { 0 };
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) h = ev.u.fetch_request.fetch;
        moq_event_cleanup(&ev);
    }
    moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 1;
    ac.track_properties = (moq_bytes_t){ k_props, sizeof(k_props) };
    if (moq_session_accept_fetch(sv, h, &ac, 11) != MOQ_OK) goto out;
    len = sent_properties_len(sv, 0x7000, MOQ_D22_FETCH_OK);
out:
    moq_simpair_destroy(sp);
    return len;
}

int main(void)
{
    int failures = 0;
    /* Asked for none: present but empty. */
    MOQ_TEST_CHECK_EQ_SIZE(subscribe_ok_properties(0), 0);
    MOQ_TEST_CHECK_EQ_SIZE(fetch_ok_properties(0), 0);
    /* Asked for them, or the default: the application's properties. */
    MOQ_TEST_CHECK_EQ_SIZE(subscribe_ok_properties(1), sizeof(k_props));
    MOQ_TEST_CHECK_EQ_SIZE(fetch_ok_properties(1), sizeof(k_props));
    MOQ_TEST_CHECK_EQ_SIZE(subscribe_ok_properties(-1), sizeof(k_props));
    MOQ_TEST_CHECK_EQ_SIZE(fetch_ok_properties(-1), sizeof(k_props));
    MOQ_TEST_PASS("d22_include_properties");
    return failures != 0;
}
