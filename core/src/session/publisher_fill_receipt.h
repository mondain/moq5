#ifndef MOQ_PUBLISHER_FILL_RECEIPT_H
#define MOQ_PUBLISHER_FILL_RECEIPT_H

#include <moq/session.h>

/* Private commit output, never an independently recomputed acceptance plan.
 * It is populated only after ACK commitment, before a NO_STREAM is reclaimed.
 * STREAM carries exactly the delivery later surfaced by FILL_REQUEST. */
typedef struct moq_pub_fill_receipt {
    bool requested;
    bool has_largest;
    uint64_t largest_group, largest_object;
    uint32_t outcome; /* 0 NONE, 1 NO_STREAM, 2 STREAM */
    moq_fill_request_event_t request;
} moq_pub_fill_receipt_t;

moq_result_t session_accept_subscribe_receipt(moq_session_t *s,
    moq_subscription_t sub, const moq_accept_subscribe_cfg_t *cfg,
    uint64_t now_us, moq_pub_fill_receipt_t *receipt);
moq_result_t session_accept_subscription_update_receipt(moq_session_t *s,
    moq_subscription_t sub, const moq_accept_subscription_update_cfg_t *cfg,
    uint64_t now_us, moq_pub_fill_receipt_t *receipt);
moq_result_t session_accept_publication_update_receipt(moq_session_t *s,
    moq_publication_t pub, const moq_accept_subscription_update_cfg_t *cfg,
    uint64_t now_us, moq_pub_fill_receipt_t *receipt);

#endif
