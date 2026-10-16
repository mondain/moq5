#ifndef MOQ_REQUEST_LIFETIME_INTERNAL_H
#define MOQ_REQUEST_LIFETIME_INTERNAL_H

#include "moq/types.h"
#include "moq/session.h"

/* Request-stream lifetime machine, one value per request on a profile that
 * carries each request on its own bidirectional stream. Pure: the caller
 * reserves or queues output, then commits the matching transition.
 *
 * Private outcomes: premature peer FIN fails this request, not the session;
 * a second admitted incoming update violates our advertised limit of one;
 * an incoming UPDATE crossing local terminal requires request-local settlement. */
#define MOQ_REQUEST_LIFETIME_FAILED (-210)
#define MOQ_REQUEST_LIFETIME_UPDATE_LIMIT (-211)
#define MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING (-212)

typedef enum moq_request_initial_response {
    MOQ_REQUEST_RESPONSE_PENDING = 0,
    MOQ_REQUEST_RESPONSE_ACCEPTED,
    MOQ_REQUEST_RESPONSE_REJECTED
} moq_request_initial_response_t;

/* One value per request. This owns no stream, buffer, event, ID, or child.
 * Local/peer name the endpoint sending a direction, not its request role.
 * FIN/reset/STOP observations are separate facts; STOP does not prove that
 * the peer reset its send direction. Terminal notifications are idempotent.
 * Missing/duplicate control responses are NOT idempotent acknowledgements.
 * Even two FINs never prove external data completion or permission to retire
 * a parent pool slot; there is deliberately no such query in this primitive. */
typedef struct moq_request_lifetime {
    moq_request_family_t family;
    bool local_requester;
    moq_request_initial_response_t response;
    bool local_fin;
    bool peer_fin;
    bool local_reset;
    bool peer_reset;
    bool local_stop;
    bool peer_stop;
    bool cancelled;
    bool peer_fin_failed;
    bool publisher_done;
    bool incoming_update;
    bool outgoing_update;
    bool update_failed;
} moq_request_lifetime_t;

#define MOQ_REQUEST_CANCEL_RESET_LOCAL 1u
#define MOQ_REQUEST_CANCEL_STOP_PEER   2u

moq_result_t moq_request_lifetime_init(moq_request_lifetime_t *s,
    moq_request_family_t family, bool local_requester);

/* All check/query functions are pure. The caller reserves/queues output and
 * only THEN calls the matching committed function. A refused queue operation
 * must not call it. Committed functions revalidate and preserve the value on
 * refusal. These are known-obligation checks, not permission to ignore future
 * responses/updates required by an enclosing request-family state machine. */
/* Initial response remains owed even after pending PUBLISH publisher DONE/FIN.
 * A late success changes only response accounting, never the DONE fact. */
moq_result_t moq_request_lifetime_check_initial_response(
    const moq_request_lifetime_t *s, bool outgoing);
moq_result_t moq_request_lifetime_initial_response_committed(
    moq_request_lifetime_t *s, bool outgoing, bool accepted);
moq_result_t moq_request_lifetime_check_fin(const moq_request_lifetime_t *s);
moq_result_t moq_request_lifetime_fin_committed(moq_request_lifetime_t *s);

/* Called after preceding buffered messages and their events are committed.
 * REQUEST_FAILED commits the observed FIN and sticky failure, unlike an
 * argument/state refusal. Repeated FIN returns the same result; callers emit
 * failure events from the first transition, not every repeated return value. */
moq_result_t moq_request_lifetime_peer_fin(moq_request_lifetime_t *s);

/* Cancellation query does not begin cancellation or consume an action. It
 * returns the currently owed directions if cancellation is requested. Commit
 * each queued direction separately; never reset a direction already FIN'd.
 * STOP remains recorded until the peer FIN/reset arrives. */
unsigned moq_request_lifetime_cancel_actions(const moq_request_lifetime_t *s);
moq_result_t moq_request_lifetime_reset_committed(moq_request_lifetime_t *s);
moq_result_t moq_request_lifetime_stop_committed(moq_request_lifetime_t *s);
moq_result_t moq_request_lifetime_peer_reset(moq_request_lifetime_t *s);
moq_result_t moq_request_lifetime_peer_stop(moq_request_lifetime_t *s);

/* Parameters and Request IDs are external. Empty updates use the same credit.
 * Initial policy: advertise incoming limit=1 and voluntarily send at most one
 * outgoing update, also when peer advertises absent/zero (unlimited). An update
 * response travels opposite its update. Successful response commit retires
 * exactly that obligation; failure additionally blocks new updates.
 * The helper admits updates only after the initial response. Otherwise valid
 * incoming updates before that point return WOULD_BLOCK, not PROTO: retain the
 * suffix outside this value and retry after the family handler's response.
 * This local sequencing precondition is not a wire ban on pending updates.
 * An otherwise eligible incoming UPDATE after local FIN or locally sent DONE
 * returns TERMINAL_CROSSING without credit/state effects, not a wire violation.
 * The caller must settle locally and retain cancellation/STOP intent over output
 * refusal; it must not ACK after DONE/FIN or clear unacknowledged outgoing work.
 * Wrong family/role, incoming-direction FIN/DONE and exceeded credit remain
 * distinct violations. No failed/reset request is reclassified as a crossing. */
moq_result_t moq_request_lifetime_check_update(const moq_request_lifetime_t *s,
    bool outgoing);
moq_result_t moq_request_lifetime_update_committed(moq_request_lifetime_t *s,
    bool outgoing);
moq_result_t moq_request_lifetime_check_update_response(
    const moq_request_lifetime_t *s, bool outgoing);
moq_result_t moq_request_lifetime_update_response_committed(
    moq_request_lifetime_t *s, bool outgoing, bool accepted);

/* The caller proves ALL publisher data obligations clear, including deferred
 * opens/child fills, before outgoing DONE. False returns WOULD_BLOCK without
 * mutation. No child ownership/completion is inferred here. Received DONE does
 * not acknowledge or erase an outstanding update. Actual
 * stream-count accounting and mandatory UPDATE_FAILED status are external.
 * PUBLISH additionally permits publisher DONE while its response is pending;
 * this permits publisher FIN but does not discharge the initial response.
 * A subscriber may observe that pending publisher DONE after locally rejecting
 * PUBLISH, including after local FIN. Only publisher_done changes; REJECTED is
 * preserved. Observing rejection never permits a new local publisher DONE. */
moq_result_t moq_request_lifetime_check_done(const moq_request_lifetime_t *s,
    bool outgoing, bool data_obligations_clear);
moq_result_t moq_request_lifetime_done_committed(moq_request_lifetime_t *s,
    bool outgoing, bool data_obligations_clear);

/* Publisher-only, subscription-only notification; no acknowledgement credit
 * or state transition. Pending incoming notifications defer with WOULD_BLOCK;
 * that helper precondition is not an additional protocol violation.
 * The caller separately commits decoded parameter effects. */
moq_result_t moq_request_lifetime_check_notify(const moq_request_lifetime_t *s,
    bool outgoing);

#endif
