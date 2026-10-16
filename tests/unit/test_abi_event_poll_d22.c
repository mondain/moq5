/*
 * Size-negotiated event poll for every event whose detail grew with the
 * draft-22 surface.
 *
 * A caller whose detail struct ends where the layout before the extension
 * ended polls with an element of the event prefix plus that layout's
 * sizeof. It receives the event with every byte of that layout intact,
 * detail_size naming the library's full detail (so a reader of a larger
 * element knows which appended fields are present), and nothing written past
 * its element. A caller polling with the full event -- whose size never
 * changes -- receives the appended fields too. The producers zero each event
 * and stamp detail_size with the current detail sizeof, so an event built
 * that way is injected here directly, with a distinct byte in every position
 * of the detail.
 */
#include <moq/moq.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <stdlib.h>
#include <string.h>

#define FIELD_END(T, f) (offsetof(T, f) + sizeof(((T *)0)->f))
#define SIZEOF_THROUGH(T, last) \
    ((FIELD_END(T, last) + _Alignof(T) - 1) / _Alignof(T) * _Alignof(T))

#define GUARD 32u
#define FILL  0xEEu

typedef struct grown {
    const char *name;
    uint32_t    kind;
    size_t      detail;    /* current sizeof */
    size_t      old;       /* sizeof through the last field before the extension */
} grown_t;

#define GROWN(K, T, last) { #K, MOQ_EVENT_##K, sizeof(T), SIZEOF_THROUGH(T, last) }

static const grown_t grown[] = {
    GROWN(NAMESPACE_DONE,             moq_namespace_done_event_t,             ann),
    GROWN(NAMESPACE_CANCELLED,        moq_namespace_cancelled_event_t,        reason),
    GROWN(UNSUBSCRIBED,               moq_unsubscribed_event_t,               sub),
    GROWN(SUBSCRIBE_UPDATED,          moq_subscribe_updated_event_t,          end_group),
    GROWN(FETCH_CANCELLED,            moq_fetch_cancelled_event_t,            fetch),
    GROWN(FETCH_RESET,                moq_fetch_reset_event_t,                _pad),
    GROWN(PUBLISH_FINISHED,           moq_publish_finished_event_t,           reason),
    GROWN(PUBLISH_UNSUBSCRIBED,       moq_publish_unsubscribed_event_t,       pub),
    GROWN(SUBSCRIBE_TRACKS_CANCELLED, moq_subscribe_tracks_cancelled_event_t, handle),
    GROWN(PUBLISH_UPDATED,            moq_publish_updated_event_t,            end_group),
    GROWN(FETCH_REQUEST,              moq_fetch_request_event_t,              joining_pub),
    /* Same size: PUBLISH_REQUEST's settings took over its explicit padding,
     * REDIRECT's handle union names two more request kinds. */
    GROWN(PUBLISH_REQUEST,            moq_publish_request_event_t,            expires_ms),
    GROWN(REQUEST_REDIRECT,           moq_request_redirect_event_t,           reason),
};

static unsigned char pattern(size_t i) { return (unsigned char)(0x11u + i * 7u); }

static moq_session_t *mk_session(void)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_CLIENT);
    cfg.version = MOQ_VERSION_DRAFT_22;
    moq_session_t *s = NULL;
    return moq_session_create(&cfg, 0, &s) == MOQ_OK ? s : NULL;
}

static void inject(moq_session_t *s, const grown_t *g)
{
    moq_event_t e;
    memset(&e, 0, sizeof(e));
    e.kind = g->kind;
    e.detail_size = (uint32_t)g->detail;
    e.borrow_epoch = s->borrow_epoch;
    for (size_t i = 0; i < g->detail; i++)
        ((unsigned char *)&e.u)[i] = pattern(i);
    (void)push_event(s, &e);
}

static int check_kind(moq_session_t *s, const grown_t *g)
{
    int failures = 0;
    const size_t prefix = offsetof(moq_event_t, u);
    unsigned char buf[sizeof(moq_event_t) + GUARD];

    MOQ_TEST_CHECK(g->detail <= MOQ_EVENT_DETAIL_MAX);
    MOQ_TEST_CHECK(g->old <= g->detail);

    /* The caller's element ends at the layout before the extension. */
    inject(s, g);
    const size_t elem = prefix + g->old;
    memset(buf, FILL, sizeof(buf));
    size_t n = 99;
    MOQ_TEST_CHECK(moq_session_poll_events_ex(s, buf, 1, elem, &n) == MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(n, 1);
    moq_event_t head;
    memcpy(&head, buf, prefix);
    if (head.kind != g->kind || head.detail_size != g->detail) {
        fprintf(stderr, "FAIL: %s: kind %u detail_size %u, expected %u / %zu\n",
                g->name, (unsigned)head.kind, (unsigned)head.detail_size,
                (unsigned)g->kind, g->detail);
        failures++;
    }
    for (size_t i = 0; i < g->old; i++) {
        if (buf[prefix + i] != pattern(i)) {
            fprintf(stderr, "FAIL: %s: detail byte %zu lost in a %zu-byte element\n",
                    g->name, i, elem);
            failures++;
            break;
        }
    }
    for (size_t i = elem; i < sizeof(buf); i++) {
        if (buf[i] != FILL) {
            fprintf(stderr, "FAIL: %s: wrote byte %zu past a %zu-byte element\n",
                    g->name, i, elem);
            failures++;
            break;
        }
    }

    /* The full event, whose size every revision shares, carries it all. */
    inject(s, g);
    memset(buf, FILL, sizeof(buf));
    n = 99;
    MOQ_TEST_CHECK(moq_session_poll_events_ex(s, buf, 1, sizeof(moq_event_t), &n) == MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(n, 1);
    for (size_t i = 0; i < g->detail; i++) {
        if (buf[prefix + i] != pattern(i)) {
            fprintf(stderr, "FAIL: %s: detail byte %zu lost in the full event\n",
                    g->name, i);
            failures++;
            break;
        }
    }
    for (size_t i = sizeof(moq_event_t); i < sizeof(buf); i++) {
        if (buf[i] != FILL) {
            fprintf(stderr, "FAIL: %s: wrote past the full event\n", g->name);
            failures++;
            break;
        }
    }
    return failures;
}

int main(void)
{
    int failures = 0;
    moq_session_t *s = mk_session();
    MOQ_TEST_CHECK(s != NULL);
    if (s) {
        for (size_t i = 0; i < sizeof(grown) / sizeof(grown[0]); i++)
            failures += check_kind(s, &grown[i]);
        moq_event_t ev;
        MOQ_TEST_CHECK(moq_session_poll_events(s, &ev, 1) == 0);   /* drained */
        moq_session_destroy(s);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: abi_event_poll_d22\n");
    return 0;
}
