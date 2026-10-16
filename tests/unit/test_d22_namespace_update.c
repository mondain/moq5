/* Literal peer messages exercise D22 9.5 response obligations on an
 * established SUBSCRIBE_NAMESPACE. No product encoder defines the oracle. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

static void discard_events(moq_session_t *s)
{
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) moq_event_cleanup(&event);
}

static int discard_actions(moq_session_t *s)
{
    int failures = 0;
    moq_action_t action;
    while (moq_session_poll_actions(s, &action, 1)) {
        MOQ_TEST_CHECK(action.kind != MOQ_ACTION_CLOSE_SESSION);
        moq_action_cleanup(&action);
    }
    return failures;
}

static int early_update(bool peer_server, bool coalesced, bool fin, bool pressure)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_events = pressure ? 1 : 0;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&cfg, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(pair != NULL);
    if (rc != MOQ_OK || !pair) goto done;
    rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *s = peer_server ? moq_simpair_client(pair) : moq_simpair_server(pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(s), MOQ_VERSION_DRAFT_22);
    if (!pressure) discard_events(s);
    failures += discard_actions(s);
    const moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7900);
    const uint8_t id = peer_server ? 1 : 0;
    uint8_t wire[] = {0x80, 0x50, 0, 5, id, 1, 1, 'n', 0,
                     2, 0, 2, (uint8_t)(id + 2), 0};
    if (!coalesced) {
        rc = moq_session_on_bidi_stream_bytes(s, ref, wire, 9, false, 1);
        MOQ_TEST_CHECK_EQ_INT(rc, pressure ? MOQ_ERR_WOULD_BLOCK : MOQ_OK);
        if (rc != (pressure ? MOQ_ERR_WOULD_BLOCK : MOQ_OK)) goto done;
    }
    rc = moq_session_on_bidi_stream_bytes(s, ref,
        wire + (coalesced ? 0 : 9), coalesced ? sizeof(wire) : 5, fin, 2);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    if (rc != MOQ_ERR_WOULD_BLOCK || moq_session_state(s) != MOQ_SESS_ESTABLISHED)
        goto done;
    memset(wire, 0, sizeof(wire));
    if (pressure) {
        moq_event_t setup;
        size_t count = moq_session_poll_events(s, &setup, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 1);
        if (!count) goto done;
        MOQ_TEST_CHECK_EQ_INT(setup.kind, MOQ_EVENT_SETUP_COMPLETE);
        moq_event_cleanup(&setup);
        rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, false, 3);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
        if (rc != MOQ_ERR_WOULD_BLOCK) goto done;
    }
    moq_event_t event;
    moq_ns_sub_handle_t handle = {0};
    size_t requests = 0;
    while (moq_session_poll_events(s, &event, 1)) {
        MOQ_TEST_CHECK_EQ_INT(event.kind, MOQ_EVENT_NS_SUB_REQUEST);
        if (event.kind == MOQ_EVENT_NS_SUB_REQUEST) {
            ++requests;
            handle = event.u.ns_sub_request.handle;
            const moq_namespace_t prefix = event.u.ns_sub_request.track_namespace_prefix;
            MOQ_TEST_CHECK_EQ_SIZE(prefix.count, 1);
            if (prefix.count == 1) {
                MOQ_TEST_CHECK_EQ_SIZE(prefix.parts[0].len, 1);
                if (prefix.parts[0].len == 1) MOQ_TEST_CHECK(prefix.parts[0].data[0] == 'n');
            }
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    if (requests != 1) goto done;
    moq_action_t action;
    size_t count = moq_session_poll_actions(s, &action, 1);
    MOQ_TEST_CHECK_EQ_SIZE(count, 0);
    if (count) moq_action_cleanup(&action);
    moq_accept_ns_sub_cfg_t accept;
    moq_accept_ns_sub_cfg_init(&accept);
    rc = moq_session_accept_ns_sub(s, handle, &accept, 4);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, false, 5);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    size_t responses = 0;
    while (moq_session_poll_actions(s, &action, 1)) {
        MOQ_TEST_CHECK_EQ_INT(action.kind, MOQ_ACTION_SEND_BIDI_STREAM);
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            const uint8_t ok[] = {7, 0, 1, 0};
            const uint8_t error[] = {5, 0, 3, 3, 0, 0};
            const moq_send_bidi_stream_action_t *send = &action.u.send_bidi_stream;
            const uint8_t *expected = responses == 0 ? ok : error;
            const size_t expected_len = responses == 0 ? sizeof(ok) : sizeof(error);
            MOQ_TEST_CHECK(responses < 2);
            MOQ_TEST_CHECK_EQ_U64(send->stream_ref._v, ref._v);
            MOQ_TEST_CHECK_EQ_SIZE(send->len, expected_len);
            MOQ_TEST_CHECK(send->fin == (responses != 0));
            if (send->len == expected_len)
                MOQ_TEST_CHECK(memcmp(send->data, expected, expected_len) == 0);
            ++responses;
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(responses, 2);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(moq_session_accept_ns_sub(s, handle, &accept, 6), MOQ_ERR_STALE_HANDLE);
done:
    moq_simpair_destroy(pair);
    if (failures) fprintf(stderr, "namespace early update: peer_server=%d coalesced=%d fin=%d pressure=%d failures=%d\n",
        peer_server, coalesced, fin, pressure, failures);
    return failures;
}

static int truncated_early_update(bool peer_server, bool coalesced, bool pressure)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.max_events = pressure ? 1 : 0;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&cfg, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    MOQ_TEST_CHECK(pair != NULL);
    if (rc != MOQ_OK || !pair) goto done;
    rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *s = peer_server ? moq_simpair_client(pair) : moq_simpair_server(pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(s), MOQ_VERSION_DRAFT_22);
    if (!pressure) discard_events(s);
    failures += discard_actions(s);
    const moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7900);
    const uint8_t id = peer_server ? 1 : 0;
    /* UPDATE promises two payload bytes, but FIN replaces the parameter count. */
    uint8_t wire[] = {0x80, 0x50, 0, 5, id, 1, 1, 'n', 0,
                     2, 0, 2, (uint8_t)(id + 2)};
    if (!coalesced) {
        rc = moq_session_on_bidi_stream_bytes(s, ref, wire, 9, false, 1);
        MOQ_TEST_CHECK_EQ_INT(rc, pressure ? MOQ_ERR_WOULD_BLOCK : MOQ_OK);
        if (rc != (pressure ? MOQ_ERR_WOULD_BLOCK : MOQ_OK)) goto done;
    }
    rc = moq_session_on_bidi_stream_bytes(s, ref,
        wire + (coalesced ? 0 : 9), coalesced ? sizeof(wire) : 4, true, 2);
    MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_ERR_WOULD_BLOCK);
    memset(wire, 0, sizeof(wire));
    if (pressure) {
        moq_event_t setup;
        size_t count = moq_session_poll_events(s, &setup, 1);
        MOQ_TEST_CHECK_EQ_SIZE(count, 1);
        if (!count) goto done;
        MOQ_TEST_CHECK_EQ_INT(setup.kind, MOQ_EVENT_SETUP_COMPLETE);
        moq_event_cleanup(&setup);
        rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, false, 3);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    }
    moq_ns_sub_handle_t handle = {0};
    moq_event_t event;
    size_t requests = 0;
    size_t close_events = 0;
    while (moq_session_poll_events(s, &event, 1)) {
        if (event.kind == MOQ_EVENT_NS_SUB_REQUEST) {
            handle = event.u.ns_sub_request.handle;
            ++requests;
        } else if (event.kind == MOQ_EVENT_SESSION_CLOSED) {
            MOQ_TEST_CHECK_EQ_U64(event.u.closed.code, 3);
            ++close_events;
        } else {
            MOQ_TEST_CHECK(event.kind == MOQ_EVENT_NS_SUB_REQUEST ||
                           event.kind == MOQ_EVENT_SESSION_CLOSED);
        }
        moq_event_cleanup(&event);
    }
    /* Closing the session clears an otherwise queued initial-request event. */
    MOQ_TEST_CHECK_EQ_SIZE(requests,
        moq_session_state(s) == MOQ_SESS_CLOSED ? 0 : 1);
    MOQ_TEST_CHECK_EQ_SIZE(close_events,
        moq_session_state(s) == MOQ_SESS_CLOSED ? 1 : 0);
    if (moq_session_state(s) == MOQ_SESS_ESTABLISHED && requests == 1) {
        moq_accept_ns_sub_cfg_t accept;
        moq_accept_ns_sub_cfg_init(&accept);
        rc = moq_session_accept_ns_sub(s, handle, &accept, 4);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        if (rc != MOQ_OK) goto done;
        rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, false, 5);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    }
    size_t closes = 0;
    moq_action_t action;
    while (moq_session_poll_actions(s, &action, 1)) {
        if (action.kind == MOQ_ACTION_CLOSE_SESSION) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(action.u.close_session.code, 3);
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(closes, 1);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_CLOSED);
done:
    moq_simpair_destroy(pair);
    if (failures) fprintf(stderr,
        "namespace truncated early update: peer_server=%d coalesced=%d pressure=%d failures=%d\n",
        peer_server, coalesced, pressure, failures);
    return failures;
}

static int run_case(bool peer_server, bool split, bool fin, int invalid, bool pressure)
{
    int failures = 0;
    moq_simpair_cfg_t pair_cfg = MOQ_SIMPAIR_CFG_INIT;
    pair_cfg.alloc = moq_alloc_default();
    pair_cfg.version = MOQ_VERSION_DRAFT_22;
    pair_cfg.auth_token_cache_size = 1024;
    pair_cfg.max_actions = pressure ? 4 : 0;
    moq_simpair_t *pair = NULL;
    moq_result_t rc = moq_simpair_create(&pair_cfg, &pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK || !pair) return failures + (pair == NULL && rc == MOQ_OK);
    rc = moq_simpair_start(pair);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    rc = moq_simpair_run_until_quiescent(pair, 32, NULL);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_session_t *s = peer_server ? moq_simpair_client(pair) : moq_simpair_server(pair);
    MOQ_TEST_CHECK_EQ_INT(moq_session_version(s), MOQ_VERSION_DRAFT_22);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    if (moq_session_state(s) != MOQ_SESS_ESTABLISHED) goto done;
    discard_events(s);
    failures += discard_actions(s);

    const moq_stream_ref_t ref = moq_stream_ref_from_u64(0x7600);
    const uint8_t request_id = peer_server ? 1 : 0;
    /* SUBSCRIBE_NAMESPACE, one-part prefix "n", no parameters. */
    uint8_t request[] = {0x80, 0x50, 0, 5, request_id, 1, 1, 'n', 0};
    rc = moq_session_on_bidi_stream_bytes(s, ref, request, sizeof(request), false, 1);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    moq_ns_sub_handle_t handle = {0};
    size_t requests = 0;
    moq_event_t event;
    while (moq_session_poll_events(s, &event, 1)) {
        if (event.kind == MOQ_EVENT_NS_SUB_REQUEST) {
            handle = event.u.ns_sub_request.handle;
            ++requests;
            MOQ_TEST_CHECK_EQ_SIZE(event.u.ns_sub_request.track_namespace_prefix.count, 1);
        } else {
            MOQ_TEST_CHECK(event.kind == MOQ_EVENT_NS_SUB_REQUEST);
        }
        moq_event_cleanup(&event);
    }
    MOQ_TEST_CHECK_EQ_SIZE(requests, 1);
    if (requests != 1) goto done;
    moq_accept_ns_sub_cfg_t accept;
    moq_accept_ns_sub_cfg_init(&accept);
    rc = moq_session_accept_ns_sub(s, handle, &accept, 2);
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    if (rc != MOQ_OK) goto done;
    failures += discard_actions(s);

    if (invalid == 4) {
        /* Empty progress is not an update and must leave the handle live. */
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(s, ref, NULL, 0,
                                                             false, 3), MOQ_OK);
        const moq_bytes_t suffix_parts[] = {MOQ_BYTES_LITERAL("child")};
        const moq_namespace_t suffix = {suffix_parts, 1};
        MOQ_TEST_CHECK_EQ_INT(moq_session_send_namespace(s, handle, &suffix, 4), MOQ_OK);
        failures += discard_actions(s);
        goto done;
    }

    if (invalid == 5) {
        const uint8_t goaway[] = {0x10, 0, 2, 0, 9};
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(s, ref, goaway,
            sizeof(goaway), fin, 3), MOQ_OK);
        size_t migrations = 0, close_halves = 0;
        while (moq_session_poll_events(s, &event, 1)) {
            if (event.kind == MOQ_EVENT_REQUEST_GOAWAY) {
                ++migrations;
                MOQ_TEST_CHECK_EQ_INT(event.u.request_goaway.family, MOQ_REQUEST_FAMILY_NS_SUB);
                MOQ_TEST_CHECK_EQ_U64(event.u.request_goaway.handle.ns_sub._opaque, handle._opaque);
                MOQ_TEST_CHECK_EQ_SIZE(event.u.request_goaway.new_session_uri.len, 0);
                MOQ_TEST_CHECK_EQ_U64(event.u.request_goaway.timeout_ms, 9);
            } else {
                MOQ_TEST_CHECK(event.kind == MOQ_EVENT_REQUEST_GOAWAY);
            }
            moq_event_cleanup(&event);
        }
        moq_action_t action;
        while (moq_session_poll_actions(s, &action, 1)) {
            MOQ_TEST_CHECK(action.kind == MOQ_ACTION_CLOSE_BIDI_STREAM);
            if (action.kind == MOQ_ACTION_CLOSE_BIDI_STREAM) {
                ++close_halves;
                MOQ_TEST_CHECK_EQ_U64(action.u.close_bidi_stream.stream_ref._v, ref._v);
            }
            moq_action_cleanup(&action);
        }
        MOQ_TEST_CHECK_EQ_SIZE(migrations, 1);
        MOQ_TEST_CHECK_EQ_SIZE(close_halves, 1);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
        goto done;
    }

    size_t queued = 0;
    if (pressure) {
        const moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("pressure")};
        moq_subscribe_cfg_t cfg;
        moq_subscribe_cfg_init(&cfg);
        cfg.track_namespace = (moq_namespace_t){parts, 1};
        for (unsigned i = 0; i < 32; ++i) {
            const uint8_t name = (uint8_t)i;
            cfg.track_name = (moq_bytes_t){&name, 1};
            moq_subscription_t sub = {0};
            rc = moq_session_subscribe(s, &cfg, 3, &sub);
            if (rc != MOQ_OK) break;
            ++queued;
        }
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK(queued > 0 && queued < 32);
        if (rc != MOQ_ERR_WOULD_BLOCK) goto done;
    }
    bool auth = pressure || invalid == 7;
    if (invalid == 7) invalid = 0;
    uint8_t update[] = {2, 0, 2, (uint8_t)(request_id + 2), 0, 3, 4, 1, 7, 0, 0xaa};
    size_t len = 5;
    if (auth) {
        update[2] = 8;
        update[4] = 1;
        len = sizeof(update);
    }
    if (invalid == 1) update[3] ^= 1; /* Wrong sender parity. */
    if (invalid == 2) update[0] = 0x7f; /* Unexpected message kind. */
    if (invalid == 3) --len; /* Truncated body at FIN. */
    if (split) {
        rc = moq_session_on_bidi_stream_bytes(s, ref, update, len - 1, false, 3);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        moq_action_t action;
        size_t actions = moq_session_poll_actions(s, &action, 1);
        MOQ_TEST_CHECK_EQ_SIZE(actions, 0);
        if (actions) moq_action_cleanup(&action);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
    }
    rc = moq_session_on_bidi_stream_bytes(s, ref, update + (split ? len - 1 : 0),
        split ? 1 : len, fin, 4);
    if (pressure) {
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_ESTABLISHED);
        size_t retained = 0;
        moq_action_t prior;
        while (moq_session_poll_actions(s, &prior, 1)) {
            MOQ_TEST_CHECK(prior.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
            ++retained;
            moq_action_cleanup(&prior);
        }
        MOQ_TEST_CHECK_EQ_SIZE(retained, queued);
        memset(update, 0, sizeof(update));
        rc = moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, false, 5);
    }
    MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
    size_t responses = 0, closes = 0, aborts = 0;
    moq_action_t action;
    while (moq_session_poll_actions(s, &action, 1)) {
        if (action.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            ++responses;
            const moq_send_bidi_stream_action_t *send = &action.u.send_bidi_stream;
            /* REQUEST_ERROR NOT_SUPPORTED, no retry and empty reason. */
            const uint8_t expected[] = {5, 0, 3, 3, 0, 0};
            MOQ_TEST_CHECK_EQ_U64(send->stream_ref._v, ref._v);
            MOQ_TEST_CHECK(send->fin);
            MOQ_TEST_CHECK_EQ_SIZE(send->len, sizeof(expected));
            if (send->len == sizeof(expected))
                MOQ_TEST_CHECK(memcmp(send->data, expected, sizeof(expected)) == 0);
        } else if (action.kind == MOQ_ACTION_CLOSE_SESSION) {
            ++closes;
            MOQ_TEST_CHECK_EQ_U64(action.u.close_session.code, invalid == 1 ? 4 : 3);
        } else if (action.kind == MOQ_ACTION_ABORT_BIDI_STREAM) {
            ++aborts;
        } else {
            MOQ_TEST_CHECK(action.kind == MOQ_ACTION_SEND_BIDI_STREAM);
        }
        moq_action_cleanup(&action);
    }
    MOQ_TEST_CHECK_EQ_SIZE(aborts, 0);
    MOQ_TEST_CHECK_EQ_SIZE(responses, invalid ? 0 : 1);
    MOQ_TEST_CHECK_EQ_SIZE(closes, invalid ? 1 : 0);
    MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), invalid ? MOQ_SESS_CLOSED : MOQ_SESS_ESTABLISHED);
    if (auth && moq_session_state(s) == MOQ_SESS_ESTABLISHED) {
        /* REGISTER commits with the refusal; USE_ALIAS sees owned bytes even
         * after the original input was overwritten and the owner retired. */
        memset(update, 0, sizeof(update));
        const uint8_t use_alias[] = {0x80, 0x50, 0, 9, (uint8_t)(request_id + 4),
                                     1, 1, 'k', 1, 3, 2, 2, 7};
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(s,
            moq_stream_ref_from_u64(0x7800), use_alias, sizeof(use_alias), false, 6), MOQ_OK);
        size_t alias_requests = 0;
        while (moq_session_poll_events(s, &event, 1)) {
            MOQ_TEST_CHECK(event.kind == MOQ_EVENT_NS_SUB_REQUEST);
            if (event.kind == MOQ_EVENT_NS_SUB_REQUEST) {
                ++alias_requests;
                MOQ_TEST_CHECK_EQ_SIZE(event.u.ns_sub_request.token_count, 1);
                if (event.u.ns_sub_request.token_count == 1) {
                    const moq_resolved_token_t *token = event.u.ns_sub_request.tokens;
                    MOQ_TEST_CHECK_EQ_U64(token->token_type, 0);
                    MOQ_TEST_CHECK_EQ_SIZE(token->token_value.len, 1);
                    if (token->token_value.len == 1)
                        MOQ_TEST_CHECK(token->token_value.data[0] == 0xaa);
                }
            }
            moq_event_cleanup(&event);
        }
        MOQ_TEST_CHECK_EQ_SIZE(alias_requests, 1);
    }
    if (!invalid && moq_session_state(s) == MOQ_SESS_ESTABLISHED) {
        /* The refused update still consumed its ID. Reuse on another stream
         * must be INVALID_REQUEST_ID, not a new namespace request. */
        request[4] = (uint8_t)(request_id + 2);
        MOQ_TEST_CHECK_EQ_INT(moq_session_on_bidi_stream_bytes(s,
            moq_stream_ref_from_u64(0x7700), request, sizeof(request), false, 7), MOQ_OK);
        size_t duplicate_closes = 0;
        while (moq_session_poll_actions(s, &action, 1)) {
            if (action.kind == MOQ_ACTION_CLOSE_SESSION) {
                ++duplicate_closes;
                MOQ_TEST_CHECK_EQ_U64(action.u.close_session.code, 4);
            }
            moq_action_cleanup(&action);
        }
        MOQ_TEST_CHECK_EQ_SIZE(duplicate_closes, 1);
        MOQ_TEST_CHECK_EQ_INT(moq_session_state(s), MOQ_SESS_CLOSED);
    }
done:
    moq_simpair_destroy(pair);
    if (failures)
        fprintf(stderr, "namespace update: peer_server=%d split=%d fin=%d invalid=%d failures=%d\n",
                peer_server, split, fin, invalid, failures);
    return failures;
}

int main(int argc, char **argv)
{
    bool controls = argc == 2 && strcmp(argv[1], "--idle-control") == 0;
    if (argc != 1 && !controls) return 2;
    int failures = 0;
    for (int server = 0; server < 2; ++server) {
        failures += run_case(server != 0, false, false, 4, false);
        if (controls) continue;
        for (int coalesced = 0; coalesced < 2; ++coalesced)
            for (int fin = 0; fin < 2; ++fin)
                for (int pressure = 0; pressure < 2; ++pressure)
                    failures += early_update(server != 0, coalesced != 0,
                        fin != 0, pressure != 0);
        for (int coalesced = 0; coalesced < 2; ++coalesced)
            for (int pressure = 0; pressure < 2; ++pressure)
                failures += truncated_early_update(server != 0, coalesced != 0,
                    pressure != 0);
        failures += run_case(server != 0, false, false, 5, false);
        failures += run_case(server != 0, false, true, 5, false);
        failures += run_case(server != 0, false, false, 7, false);
        failures += run_case(server != 0, false, false, 0, true);
        failures += run_case(server != 0, false, true, 0, true);
        for (int split = 0; split < 2; ++split)
            for (int fin = 0; fin < 2; ++fin)
                failures += run_case(server != 0, split != 0, fin != 0, 0, false);
        for (int invalid = 1; invalid <= 3; ++invalid)
            failures += run_case(server != 0, false, true, invalid, false);
    }
    MOQ_TEST_PASS("D22 namespace UPDATE response and identity");
    return failures ? 1 : 0;
}
