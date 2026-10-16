#ifndef MOQ_SESSION_EVENT_ADMISSION_H
#define MOQ_SESSION_EVENT_ADMISSION_H

#include <moq/session.h>

typedef enum {
    SESSION_EVENT_TAKE,
    SESSION_EVENT_LEAVE,
    SESSION_EVENT_DISCARD
} session_event_admission_t;

/* The predicate must not advance the session or retain borrowed event fields.
 * LEAVE preserves the head, references, receive credit and progress token.
 * DISCARD requires the caller to latch its local outcome before returning it.
 * The shared dequeue path releases the event and returns MOQ_ERR_BUFFER.
 * TAKE transfers one full event; DONE means no event is currently available. */
moq_result_t session_poll_event_admitted(moq_session_t *s, moq_event_t *out,
    session_event_admission_t (*admit)(void *ctx, const moq_event_t *event),
    void *ctx);

#endif
