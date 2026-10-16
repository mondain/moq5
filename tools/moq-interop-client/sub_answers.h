#ifndef MOQ_INTEROP_SUB_ANSWERS_H
#define MOQ_INTEROP_SUB_ANSWERS_H

/*
 * Publisher-side answers to forwarded SUBSCRIBE requests.
 *
 * A relay sends a publisher every SUBSCRIBE whose track falls under a
 * namespace it published ("Relays MUST send SUBSCRIBE messages to all
 * matching publishers ... Namespace Prefix Matching against published
 * Namespaces": draft-16 §8.5, draft-18 §9.5, draft-22 §7.6), so requests for
 * other tracks in the namespace -- a catalog, say -- can arrive before the
 * one under test. Each needs exactly one answer ("A publisher MUST send
 * exactly one SUBSCRIBE_OK or REQUEST_ERROR [draft-22: SUBSCRIBE_ERROR] in
 * response to a SUBSCRIBE": §5.1 of draft-16 and draft-18, §3.1 of
 * draft-22).
 *
 * The first request naming the test track exactly (namespace and track name)
 * becomes the test subscription; the caller accepts it. Every other request
 * is refused here:
 *   - another track: DOES_NOT_EXIST, "The track or namespace is not
 *     available at the publisher" (0x10 in all three drafts: draft-16 §9.8,
 *     draft-18 §10.6.2, draft-22 §12.3);
 *   - a further request for the test track: this test publisher serves its
 *     track to a single subscription, because its object sequence is written
 *     and verified against one. Draft-16 and draft-18 allow only one ("MUST
 *     fail that request with a DUPLICATE_SUBSCRIPTION error", §5.1), and the
 *     session already refuses it before it is surfaced. Draft-22 lets a
 *     publisher serve concurrent subscriptions to a track ("An endpoint MAY
 *     have multiple concurrent subscriptions to the same Track", §3.1); a
 *     single-subscription test publisher like this one may still refuse the
 *     further request, which it does with NOT_SUPPORTED, the code the
 *     session's draft-22 profile names for a subscription it does not take.
 *     This is the test publisher's choice, not a protocol requirement.
 *
 * Refusals are queued and sent by interop_sub_answers_flush, which keeps a
 * refusal the session cannot take yet (MOQ_ERR_WOULD_BLOCK) for the next
 * call, so each request is answered exactly once. A publisher case passes
 * only while every forwarded request is answered: interop_publisher_verdict
 * fails it when one could not be (sticky) and holds it back while a refusal
 * is still queued. Single-threaded: the caller serializes all calls.
 */
#include <moq/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTEROP_SUB_ANSWERS_MAX 32u

typedef struct {
    moq_namespace_t     want_ns;      /* borrowed for the struct's life */
    moq_bytes_t         want_name;    /* borrowed for the struct's life */

    bool                have_test;    /* test_sub holds the test request */
    moq_subscription_t  test_sub;

    moq_subscription_t  pending[INTEROP_SUB_ANSWERS_MAX];
    moq_request_error_t pending_code[INTEROP_SUB_ANSWERS_MAX];
    size_t              n_pending;

    uint32_t            refused_other;     /* DOES_NOT_EXIST sent */
    uint32_t            refused_duplicate; /* duplicate refusal sent */
    /* A request left unanswered: the refusal queue was full, or the session
     * refused the refusal itself (failed_rc). Either fails the case. */
    bool                unanswered;
    moq_result_t        failed_rc;
} interop_sub_answers_t;

void interop_sub_answers_init(interop_sub_answers_t *a, moq_namespace_t want_ns,
                              moq_bytes_t want_name);

/* True when the two full track names are equal: same namespace tuple and
 * same track name, byte for byte. */
bool interop_full_track_name_eq(moq_namespace_t ns_a, moq_bytes_t name_a,
                                moq_namespace_t ns_b, moq_bytes_t name_b);

/* Classify one MOQ_EVENT_SUBSCRIBE_REQUEST. Returns true when it became the
 * test subscription (a->test_sub); otherwise its refusal is queued. Sends
 * nothing. */
bool interop_sub_answers_on_request(interop_sub_answers_t *a,
                                    const moq_session_t *s,
                                    const moq_subscribe_request_event_t *req);

/* Send the queued refusals, in arrival order, until the session cannot take
 * one now. Advancing call on s. */
void interop_sub_answers_flush(interop_sub_answers_t *a, moq_session_t *s,
                               uint64_t now_us);

typedef enum {
    INTEROP_VERDICT_PASS,     /* the step passed and every request is answered */
    INTEROP_VERDICT_WAIT,     /* the step passed; a refusal is still queued */
    INTEROP_VERDICT_FAIL,     /* the step failed, or a request went unanswered */
} interop_verdict_t;

/* A publisher step's verdict with the forwarded requests folded in. step_ok
 * is the step's own outcome (for example: the test SUBSCRIBE was accepted,
 * or the sequence was written). An unanswered request fails the step even
 * when step_ok holds, and *why (when why is non-NULL) names it; a refusal
 * still queued keeps a passing step at WAIT, so the case either sees it
 * sent or fails at its own deadline. A failing step reports FAIL and leaves
 * *why untouched unless a request went unanswered. */
interop_verdict_t interop_publisher_verdict(bool step_ok,
                                            const interop_sub_answers_t *a,
                                            const char **why);

#endif
