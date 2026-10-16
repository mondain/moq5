#include "../../core/src/session/request_lifetime_internal.h"
#include "test_support.h"
#include <string.h>

#define START(family_, requester_) \
    moq_request_lifetime_t s; \
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_init(&s, family_, requester_), MOQ_OK)
#define ACCEPT() \
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed( \
        &s, !s.local_requester, true), MOQ_OK)
#define UNCHANGED(expr_, expected_) do { \
    moq_request_lifetime_t saved_; \
    memcpy(&saved_, &s, sizeof(s)); \
    MOQ_TEST_CHECK_EQ_INT(expr_, expected_); \
    MOQ_TEST_CHECK(memcmp(&saved_, &s, sizeof(s)) == 0); \
} while (0)

static int test_initial_and_fin(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        for (int role = 0; role < 2; ++role) {
            START((moq_request_family_t)f, role != 0);
            MOQ_TEST_CHECK(s.family == (moq_request_family_t)f);
            MOQ_TEST_CHECK(s.local_requester == (role != 0));
            MOQ_TEST_CHECK(s.response == MOQ_REQUEST_RESPONSE_PENDING);
            MOQ_TEST_CHECK(!s.local_fin && !s.peer_fin && !s.cancelled);
            if (role && f != MOQ_REQUEST_FAMILY_PUBLISH) {
                UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
                MOQ_TEST_CHECK(s.local_fin && !s.cancelled);
                UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_OK);
            } else {
                UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
            }
            if (!role && f != MOQ_REQUEST_FAMILY_PUBLISH) {
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
                MOQ_TEST_CHECK(s.peer_fin && !s.cancelled && !s.peer_fin_failed);
                UNCHANGED(moq_request_lifetime_peer_fin(&s), MOQ_OK);
            }
            ACCEPT();
            MOQ_TEST_CHECK(s.response == MOQ_REQUEST_RESPONSE_ACCEPTED);
            MOQ_TEST_CHECK(!s.incoming_update && !s.outgoing_update);
            bool local_publisher = (f == MOQ_REQUEST_FAMILY_SUBSCRIBE && !role) ||
                                   (f == MOQ_REQUEST_FAMILY_PUBLISH && role);
            if (local_publisher) {
                UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_check_done(&s, true, false), MOQ_ERR_WOULD_BLOCK);
                UNCHANGED(moq_request_lifetime_done_committed(&s, true, false), MOQ_ERR_WOULD_BLOCK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
                MOQ_TEST_CHECK(s.publisher_done);
            }
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
            MOQ_TEST_CHECK(s.local_fin && !s.cancelled);
        }
    }
    MOQ_TEST_PASS("lifetime.seven_families_both_roles_fin");
    return failures;
}

static int test_peer_fin_obligations(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        START((moq_request_family_t)f, true);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(s.peer_fin && s.peer_fin_failed && !s.cancelled);
        UNCHANGED(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(s.peer_fin_failed);
    }
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(!s.publisher_done);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        MOQ_TEST_CHECK(!s.publisher_done && !s.cancelled);
        UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        MOQ_TEST_CHECK(s.local_fin && s.peer_fin && !s.cancelled && !s.peer_fin_failed);
        MOQ_TEST_CHECK(s.response == MOQ_REQUEST_RESPONSE_ACCEPTED);
    }
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, false, false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
    }
    MOQ_TEST_PASS("lifetime.peer_fin_required_messages_control_only");
    return failures;
}

static int test_cancellation(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        for (int role = 0; role < 2; ++role) {
            START((moq_request_family_t)f, role != 0);
            ACCEPT();
            for (int attempt = 0; attempt < 3; ++attempt) {
                UNCHANGED(moq_request_lifetime_cancel_actions(&s),
                    MOQ_REQUEST_CANCEL_RESET_LOCAL | MOQ_REQUEST_CANCEL_STOP_PEER);
                MOQ_TEST_CHECK(!s.local_reset && !s.local_stop && !s.cancelled);
            }
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_reset_committed(&s), MOQ_OK);
            MOQ_TEST_CHECK(s.local_reset && s.cancelled && !s.local_stop);
            UNCHANGED(moq_request_lifetime_reset_committed(&s), MOQ_OK);
            UNCHANGED(moq_request_lifetime_cancel_actions(&s), MOQ_REQUEST_CANCEL_STOP_PEER);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_stop_committed(&s), MOQ_OK);
            MOQ_TEST_CHECK(s.local_stop && !s.peer_reset);
            UNCHANGED(moq_request_lifetime_stop_committed(&s), MOQ_OK);
            UNCHANGED(moq_request_lifetime_cancel_actions(&s), 0);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_stop(&s), MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_reset(&s), MOQ_OK);
            MOQ_TEST_CHECK(s.peer_stop && s.peer_reset);
            UNCHANGED(moq_request_lifetime_peer_stop(&s), MOQ_OK);
            UNCHANGED(moq_request_lifetime_peer_reset(&s), MOQ_OK);
            UNCHANGED(moq_request_lifetime_cancel_actions(&s), 0);
            UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, true);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), MOQ_REQUEST_CANCEL_STOP_PEER);
        UNCHANGED(moq_request_lifetime_reset_committed(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_stop(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), MOQ_REQUEST_CANCEL_STOP_PEER);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_reset(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), 0);
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), MOQ_REQUEST_CANCEL_RESET_LOCAL);
        UNCHANGED(moq_request_lifetime_stop_committed(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_stop(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), MOQ_REQUEST_CANCEL_RESET_LOCAL);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_reset_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_cancel_actions(&s), 0);
    }
    MOQ_TEST_PASS("lifetime.directional_cancel_refusal_and_crossed_signals");
    return failures;
}

static int test_update_roles_and_credits(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        for (int role = 0; role < 2; ++role) {
            for (int outgoing = 0; outgoing < 2; ++outgoing) {
                START((moq_request_family_t)f, role != 0);
                ACCEPT();
                bool sender_requester = outgoing ? role != 0 : role == 0;
                bool allowed = f != MOQ_REQUEST_FAMILY_TRACK_STATUS &&
                    (sender_requester || f == MOQ_REQUEST_FAMILY_PUBLISH);
                moq_result_t refusal = outgoing ? MOQ_ERR_WRONG_STATE : MOQ_ERR_PROTO;
                if (!allowed) {
                    UNCHANGED(moq_request_lifetime_update_committed(&s, outgoing != 0), refusal);
                    continue;
                }
                /* A queue refusal after a successful query changes nothing.
                 * No parameter count is passed: even an empty update consumes credit. */
                UNCHANGED(moq_request_lifetime_check_update(&s, outgoing != 0), MOQ_OK);
                MOQ_TEST_CHECK(!s.incoming_update && !s.outgoing_update);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, outgoing != 0), MOQ_OK);
                MOQ_TEST_CHECK(s.outgoing_update == (outgoing != 0));
                MOQ_TEST_CHECK(s.incoming_update == (outgoing == 0));
                UNCHANGED(moq_request_lifetime_update_committed(&s, outgoing != 0),
                    outgoing ? MOQ_ERR_WOULD_BLOCK : MOQ_REQUEST_LIFETIME_UPDATE_LIMIT);
                if (!outgoing)
                    UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
                /* Initial response cannot masquerade as an update response. */
                UNCHANGED(moq_request_lifetime_initial_response_committed(
                    &s, !s.local_requester, true), s.local_requester ? MOQ_ERR_PROTO : MOQ_ERR_WRONG_STATE);
                MOQ_TEST_CHECK(s.outgoing_update == (outgoing != 0));
                MOQ_TEST_CHECK(s.incoming_update == (outgoing == 0));
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(
                    &s, outgoing == 0, true), MOQ_OK);
                MOQ_TEST_CHECK(!s.incoming_update && !s.outgoing_update);
                UNCHANGED(moq_request_lifetime_update_response_committed(&s, outgoing == 0, true),
                    outgoing ? MOQ_ERR_PROTO : MOQ_ERR_WRONG_STATE);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, outgoing != 0), MOQ_OK);
            }
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, false), MOQ_OK);
        MOQ_TEST_CHECK(s.outgoing_update && s.incoming_update);
        UNCHANGED(moq_request_lifetime_check_notify(&s, true), MOQ_OK);
        MOQ_TEST_CHECK(s.outgoing_update && s.incoming_update);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK(s.outgoing_update && !s.incoming_update);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(&s, false, true), MOQ_OK);
        MOQ_TEST_CHECK(!s.outgoing_update && !s.incoming_update);
    }
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, true), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_notify(&s, false), MOQ_OK);
        MOQ_TEST_CHECK(s.outgoing_update);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(&s, false, true), MOQ_OK);
        MOQ_TEST_CHECK(!s.outgoing_update && !s.cancelled);
    }
    MOQ_TEST_PASS("lifetime.update_roles_one_credit_each_direction_and_notify");
    return failures;
}

static int test_failure_and_terminal(void)
{
    int failures = 0;
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, false), MOQ_OK);
        UNCHANGED(moq_request_lifetime_done_committed(&s, true, true), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(&s, true, false), MOQ_OK);
        MOQ_TEST_CHECK(s.update_failed && !s.incoming_update);
        UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(s.outgoing_update && s.peer_fin_failed);
    }
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, false, false), MOQ_OK);
        MOQ_TEST_CHECK(s.outgoing_update);
        UNCHANGED(moq_request_lifetime_update_response_committed(&s, false, true), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(s.outgoing_update && s.peer_fin_failed && !s.cancelled);
    }
    for (int f = 1; f <= 7; ++f) {
        START((moq_request_family_t)f, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s, true, false), MOQ_OK);
        MOQ_TEST_CHECK(s.response == MOQ_REQUEST_RESPONSE_REJECTED);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
    }
    MOQ_TEST_PASS("lifetime.update_failure_done_and_crossed_terminal");
    return failures;
}

static int test_update_half_close_boundaries(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        if (f == MOQ_REQUEST_FAMILY_TRACK_STATUS || f == MOQ_REQUEST_FAMILY_PUBLISH) continue;
        {
            START((moq_request_family_t)f, true);
            ACCEPT();
            UNCHANGED(moq_request_lifetime_check_update(&s, true), MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
            MOQ_TEST_CHECK(s.local_fin && !s.peer_fin && !s.cancelled);
            UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
        }
        {
            START((moq_request_family_t)f, false);
            ACCEPT();
            UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
            MOQ_TEST_CHECK(!s.local_fin && s.peer_fin && !s.cancelled);
            UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, true);
        ACCEPT();
        UNCHANGED(moq_request_lifetime_check_update(&s, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        MOQ_TEST_CHECK(!s.local_fin && s.peer_fin && !s.cancelled);
        UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, true);
        ACCEPT();
        UNCHANGED(moq_request_lifetime_check_update(&s, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        MOQ_TEST_CHECK(!s.local_fin && s.peer_fin && !s.publisher_done && !s.cancelled);
        UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
        UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
        UNCHANGED(moq_request_lifetime_done_committed(&s, true, false), MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, false), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_response_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        MOQ_TEST_CHECK(s.local_fin && !s.peer_fin && !s.publisher_done && !s.cancelled);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
    }
    MOQ_TEST_PASS("lifetime.update_requires_both_directions_fin_preserves_done");
    return failures;
}

static int test_pending_suffix_deferral(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        for (int role = 0; role < 2; ++role) {
            START((moq_request_family_t)f, role != 0);
            bool incoming_allowed = f != MOQ_REQUEST_FAMILY_TRACK_STATUS &&
                (!role || f == MOQ_REQUEST_FAMILY_PUBLISH);
            for (int attempt = 0; attempt < 2; ++attempt) {
                UNCHANGED(moq_request_lifetime_check_update(&s, false),
                    incoming_allowed ? MOQ_ERR_WOULD_BLOCK : MOQ_ERR_PROTO);
                UNCHANGED(moq_request_lifetime_update_committed(&s, false),
                    incoming_allowed ? MOQ_ERR_WOULD_BLOCK : MOQ_ERR_PROTO);
            }
            MOQ_TEST_CHECK(!s.incoming_update && !s.outgoing_update);
            bool incoming_notify = (f == MOQ_REQUEST_FAMILY_SUBSCRIBE && role) ||
                                   (f == MOQ_REQUEST_FAMILY_PUBLISH && !role);
            UNCHANGED(moq_request_lifetime_check_notify(&s, false),
                incoming_notify ? MOQ_ERR_WOULD_BLOCK : MOQ_ERR_PROTO);
            ACCEPT();
            UNCHANGED(moq_request_lifetime_check_update(&s, false),
                incoming_allowed ? MOQ_OK : MOQ_ERR_PROTO);
            UNCHANGED(moq_request_lifetime_check_notify(&s, false),
                incoming_notify ? MOQ_OK : MOQ_ERR_PROTO);
        }
    }
    MOQ_TEST_PASS("lifetime.pending_suffix_defers_without_credit_or_protocol_error");
    return failures;
}

static int test_pending_publish_done(void)
{
    int failures = 0;
    for (int role = 0; role < 2; ++role) {
        for (int accepted = 0; accepted < 2; ++accepted) {
            for (int fin_before_response = 0; fin_before_response < 2; ++fin_before_response) {
                START(MOQ_REQUEST_FAMILY_PUBLISH, role != 0);
                bool outgoing_done = role != 0;
                moq_result_t duplicate_done = outgoing_done ? MOQ_ERR_WRONG_STATE : MOQ_ERR_PROTO;
                if (outgoing_done) {
                    UNCHANGED(moq_request_lifetime_check_done(&s, true, false), MOQ_ERR_WOULD_BLOCK);
                    UNCHANGED(moq_request_lifetime_done_committed(&s, true, false), MOQ_ERR_WOULD_BLOCK);
                }
                for (int attempt = 0; attempt < 2; ++attempt) {
                    /* A successful query followed by queue refusal records no DONE. */
                    UNCHANGED(moq_request_lifetime_check_done(&s, outgoing_done, true), MOQ_OK);
                    MOQ_TEST_CHECK(!s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_PENDING);
                }
                UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, outgoing_done,
                    outgoing_done), MOQ_OK);
                MOQ_TEST_CHECK(s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_PENDING);
                MOQ_TEST_CHECK(!s.local_fin && !s.peer_fin && !s.cancelled && !s.peer_fin_failed);
                UNCHANGED(moq_request_lifetime_done_committed(&s, outgoing_done, true), duplicate_done);
                if (!outgoing_done) {
                    /* Receiving publisher DONE never supplies our initial response. */
                    UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
                }
                if (fin_before_response) {
                    MOQ_TEST_CHECK_EQ_INT(outgoing_done ? moq_request_lifetime_fin_committed(&s) :
                        moq_request_lifetime_peer_fin(&s), MOQ_OK);
                    MOQ_TEST_CHECK(s.local_fin == outgoing_done && s.peer_fin == !outgoing_done);
                    MOQ_TEST_CHECK(!s.cancelled && !s.peer_fin_failed);
                }
                moq_request_lifetime_t expected;
                memcpy(&expected, &s, sizeof(s));
                expected.response = accepted ? MOQ_REQUEST_RESPONSE_ACCEPTED : MOQ_REQUEST_RESPONSE_REJECTED;
                UNCHANGED(moq_request_lifetime_check_initial_response(&s, !outgoing_done), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s,
                    !outgoing_done, accepted != 0), MOQ_OK);
                MOQ_TEST_CHECK(memcmp(&s, &expected, sizeof(s)) == 0);
                MOQ_TEST_CHECK(s.publisher_done && !s.incoming_update && !s.outgoing_update);
                UNCHANGED(moq_request_lifetime_initial_response_committed(&s, !outgoing_done,
                    accepted != 0), outgoing_done ? MOQ_ERR_PROTO : MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_initial_response_committed(&s, !outgoing_done,
                    accepted == 0), outgoing_done ? MOQ_ERR_PROTO : MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_update_committed(&s, false),
                    accepted && outgoing_done ? MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING : MOQ_ERR_PROTO);
                UNCHANGED(moq_request_lifetime_check_notify(&s, outgoing_done), duplicate_done);
                UNCHANGED(moq_request_lifetime_done_committed(&s, outgoing_done, true), duplicate_done);
                if (!fin_before_response) {
                    MOQ_TEST_CHECK_EQ_INT(outgoing_done ? moq_request_lifetime_fin_committed(&s) :
                        moq_request_lifetime_peer_fin(&s), MOQ_OK);
                }
                MOQ_TEST_CHECK_EQ_INT(outgoing_done ? moq_request_lifetime_peer_fin(&s) :
                    moq_request_lifetime_fin_committed(&s), MOQ_OK);
                MOQ_TEST_CHECK(s.local_fin && s.peer_fin && s.publisher_done);
                MOQ_TEST_CHECK(!s.cancelled && !s.peer_fin_failed);
            }
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, true);
        UNCHANGED(moq_request_lifetime_fin_committed(&s), MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        /* Even after our terminal, a missing peer response is still a failure. */
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_PENDING);
        MOQ_TEST_CHECK(s.peer_fin_failed && !s.cancelled);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        MOQ_TEST_CHECK(!s.publisher_done && s.peer_fin_failed && !s.cancelled);
    }
    for (int role = 0; role < 2; ++role) {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, role != 0);
        UNCHANGED(moq_request_lifetime_done_committed(&s, role == 0, true),
            role ? MOQ_ERR_PROTO : MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK(!s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_PENDING);
    }
    MOQ_TEST_PASS("lifetime.pending_publish_done_keeps_late_response_independent");
    return failures;
}

static int test_crossed_update_terminal(void)
{
    int failures = 0;
    static const moq_request_family_t subscription_families[] = {
        MOQ_REQUEST_FAMILY_SUBSCRIBE, MOQ_REQUEST_FAMILY_PUBLISH };
    for (size_t fi = 0; fi < 2; ++fi) {
        int f = (int)subscription_families[fi];
        for (int send_fin = 0; send_fin < 2; ++send_fin) {
            /* s is the publisher; peer is a separate subscriber value. */
            START((moq_request_family_t)f, f == MOQ_REQUEST_FAMILY_PUBLISH);
            moq_request_lifetime_t peer;
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_init(&peer,
                (moq_request_family_t)f, !s.local_requester), MOQ_OK);
            ACCEPT();
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&peer,
                !peer.local_requester, true), MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&peer, true), MOQ_OK);
            if (f == MOQ_REQUEST_FAMILY_PUBLISH) {
                /* Independent opposite-direction credit must survive the crossing. */
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, true), MOQ_OK);
            }
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, true, true), MOQ_OK);
            if (send_fin)
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
            moq_request_lifetime_t peer_saved;
            memcpy(&peer_saved, &peer, sizeof(peer));
            for (int attempt = 0; attempt < 3; ++attempt) {
                UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
                UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
                UNCHANGED(moq_request_lifetime_update_response_committed(&s, true, true), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_cancel_actions(&s), send_fin ? MOQ_REQUEST_CANCEL_STOP_PEER :
                    MOQ_REQUEST_CANCEL_RESET_LOCAL | MOQ_REQUEST_CANCEL_STOP_PEER);
                MOQ_TEST_CHECK(!s.incoming_update && !s.cancelled && !s.local_stop && !s.local_reset);
                MOQ_TEST_CHECK(s.outgoing_update == (f == MOQ_REQUEST_FAMILY_PUBLISH));
                MOQ_TEST_CHECK(memcmp(&peer_saved, &peer, sizeof(peer)) == 0);
            }
            MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_stop_committed(&s), MOQ_OK);
            UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
            MOQ_TEST_CHECK(s.cancelled && !s.incoming_update);
            MOQ_TEST_CHECK(s.outgoing_update == (f == MOQ_REQUEST_FAMILY_PUBLISH));
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, false);
        moq_request_lifetime_t peer;
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_init(&peer, MOQ_REQUEST_FAMILY_FETCH, true), MOQ_OK);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&peer, false, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&peer, true), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        for (int attempt = 0; attempt < 3; ++attempt) {
            UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
            UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
            UNCHANGED(moq_request_lifetime_update_response_committed(&s, true, true), MOQ_ERR_WRONG_STATE);
            MOQ_TEST_CHECK(!s.incoming_update && !s.cancelled && peer.outgoing_update);
        }
    }
    MOQ_TEST_PASS("lifetime.two_endpoint_updates_cross_local_done_or_fin");
    return failures;
}

static int test_crossing_violation_controls(void)
{
    int failures = 0;
    {
        START(MOQ_REQUEST_FAMILY_FETCH, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, false, false), MOQ_OK);
        /* DONE is on the incoming publisher direction; a later UPDATE is not crossed. */
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
    }
    {
        START(MOQ_REQUEST_FAMILY_SUBSCRIBE, true);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
    }
    {
        START(MOQ_REQUEST_FAMILY_TRACK_STATUS, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
    }
    {
        START(MOQ_REQUEST_FAMILY_FETCH, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_update_committed(&s, false), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_fin(&s), MOQ_ERR_WRONG_STATE);
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_REQUEST_LIFETIME_UPDATE_LIMIT);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_REQUEST_LIFETIME_UPDATE_LIMIT);
        MOQ_TEST_CHECK(s.incoming_update && !s.outgoing_update && !s.cancelled);
    }
    for (int reset = 0; reset < 2; ++reset) {
        START(MOQ_REQUEST_FAMILY_FETCH, false);
        ACCEPT();
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(reset ? moq_request_lifetime_peer_reset(&s) :
            moq_request_lifetime_peer_stop(&s), MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_update(&s, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(s.cancelled);
    }
    MOQ_TEST_PASS("lifetime.crossing_keeps_role_peer_terminal_credit_failures_distinct");
    return failures;
}

static int test_pending_done_crosses_rejection(void)
{
    int failures = 0;
    for (int accepted = 0; accepted < 2; ++accepted) {
        for (int response_first = 0; response_first < 2; ++response_first) {
            for (int subscriber_fin = 0; subscriber_fin < 2; ++subscriber_fin) {
                START(MOQ_REQUEST_FAMILY_PUBLISH, false);
                moq_request_lifetime_t publisher;
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_init(&publisher,
                    MOQ_REQUEST_FAMILY_PUBLISH, true), MOQ_OK);
                /* The publisher sends while pending, before it can see rejection. */
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&publisher, true, true), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&publisher), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s, true,
                    accepted != 0), MOQ_OK);
                if (subscriber_fin)
                    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
                if (response_first)
                    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&publisher,
                        false, accepted != 0), MOQ_OK);
                moq_request_lifetime_t expected;
                memcpy(&expected, &s, sizeof(s));
                expected.publisher_done = true;
                for (int attempt = 0; attempt < 2; ++attempt)
                    UNCHANGED(moq_request_lifetime_check_done(&s, false, false), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_done_committed(&s, false, false), MOQ_OK);
                MOQ_TEST_CHECK(memcmp(&s, &expected, sizeof(s)) == 0);
                MOQ_TEST_CHECK(s.response == (accepted ? MOQ_REQUEST_RESPONSE_ACCEPTED : MOQ_REQUEST_RESPONSE_REJECTED));
                MOQ_TEST_CHECK(!s.incoming_update && !s.outgoing_update && !s.cancelled && !s.peer_fin_failed);
                UNCHANGED(moq_request_lifetime_done_committed(&s, false, false), MOQ_ERR_PROTO);
                UNCHANGED(moq_request_lifetime_update_committed(&s, false), MOQ_ERR_PROTO);
                UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
                UNCHANGED(moq_request_lifetime_check_notify(&s, false), MOQ_ERR_PROTO);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
                MOQ_TEST_CHECK(s.publisher_done && s.peer_fin && !s.peer_fin_failed && !s.cancelled);
                if (!subscriber_fin)
                    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_fin_committed(&s), MOQ_OK);
                if (!response_first)
                    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&publisher,
                        false, accepted != 0), MOQ_OK);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&publisher), MOQ_OK);
                MOQ_TEST_CHECK(s.local_fin && s.peer_fin && publisher.local_fin && publisher.peer_fin);
                MOQ_TEST_CHECK(publisher.publisher_done && !publisher.cancelled && !publisher.peer_fin_failed);
                MOQ_TEST_CHECK(publisher.response == s.response);
            }
        }
    }
    MOQ_TEST_PASS("lifetime.pending_done_crosses_rejection_without_reactivation");
    return failures;
}

static int test_rejected_done_refusal_controls(void)
{
    int failures = 0;
    for (int f = 1; f <= 7; ++f) {
        for (int role = 0; role < 2; ++role) {
            for (int outgoing = 0; outgoing < 2; ++outgoing) {
                if (f == MOQ_REQUEST_FAMILY_PUBLISH && !role && !outgoing) continue;
                START((moq_request_family_t)f, role != 0);
                MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s,
                    !s.local_requester, false), MOQ_OK);
                moq_result_t refusal = outgoing ? MOQ_ERR_WRONG_STATE : MOQ_ERR_PROTO;
                UNCHANGED(moq_request_lifetime_check_done(&s, outgoing != 0, true), refusal);
                UNCHANGED(moq_request_lifetime_done_committed(&s, outgoing != 0, true), refusal);
                MOQ_TEST_CHECK(!s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_REJECTED);
            }
        }
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s, true, false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_OK);
        /* A peer that observes rejection can FIN without DONE, but not send after FIN. */
        UNCHANGED(moq_request_lifetime_check_done(&s, false, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_done_committed(&s, false, false), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(s.peer_fin && !s.publisher_done && !s.peer_fin_failed);
    }
    for (int cancelled_by = 0; cancelled_by < 4; ++cancelled_by) {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_initial_response_committed(&s, true, false), MOQ_OK);
        moq_result_t rc;
        switch (cancelled_by) {
        case 0: rc = moq_request_lifetime_reset_committed(&s); break;
        case 1: rc = moq_request_lifetime_stop_committed(&s); break;
        case 2: rc = moq_request_lifetime_peer_reset(&s); break;
        default: rc = moq_request_lifetime_peer_stop(&s); break;
        }
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        UNCHANGED(moq_request_lifetime_check_done(&s, false, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_done_committed(&s, false, false), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(s.cancelled && !s.publisher_done && s.response == MOQ_REQUEST_RESPONSE_REJECTED);
    }
    {
        START(MOQ_REQUEST_FAMILY_PUBLISH, false);
        MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(&s), MOQ_REQUEST_LIFETIME_FAILED);
        UNCHANGED(moq_request_lifetime_check_done(&s, false, false), MOQ_ERR_PROTO);
        UNCHANGED(moq_request_lifetime_done_committed(&s, false, false), MOQ_ERR_PROTO);
        MOQ_TEST_CHECK(s.peer_fin_failed && !s.publisher_done && !s.cancelled);
    }
    MOQ_TEST_PASS("lifetime.rejection_crossing_preserves_direction_role_and_failure_guards");
    return failures;
}

static int test_invalid(void)
{
    int failures = 0;
    START(MOQ_REQUEST_FAMILY_FETCH, true);
    UNCHANGED(moq_request_lifetime_init(&s, (moq_request_family_t)0, false), MOQ_ERR_INVAL);
    UNCHANGED(moq_request_lifetime_init(&s, (moq_request_family_t)8, false), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_init(NULL, MOQ_REQUEST_FAMILY_FETCH, true), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_check_fin(NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_peer_fin(NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_cancel_actions(NULL), 0);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_check_update(NULL, true), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_check_done(NULL, true, true), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT(moq_request_lifetime_check_notify(NULL, false), MOQ_ERR_INVAL);
    UNCHANGED(moq_request_lifetime_update_response_committed(&s, false, true), MOQ_ERR_PROTO);
    UNCHANGED(moq_request_lifetime_update_committed(&s, true), MOQ_ERR_WRONG_STATE);
    UNCHANGED(moq_request_lifetime_check_notify(&s, false), MOQ_ERR_PROTO);
    MOQ_TEST_PASS("lifetime.argument_and_unsolicited_refusals");
    return failures;
}

int main(void)
{
    int failures = test_initial_and_fin() + test_peer_fin_obligations() +
        test_cancellation() + test_update_roles_and_credits() +
        test_failure_and_terminal() + test_update_half_close_boundaries() +
        test_pending_suffix_deferral() + test_pending_publish_done() +
        test_crossed_update_terminal() + test_crossing_violation_controls() +
        test_pending_done_crosses_rejection() + test_rejected_done_refusal_controls() + test_invalid();
    printf("request lifetime: %d failures\n", failures);
    return failures ? 1 : 0;
}
