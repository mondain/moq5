/*
 * REQUEST_UPDATE from the two senders the engine did not yet read: the
 * publisher updating its own PUBLISH (the subscriber answers REQUEST_OK and
 * surfaces the change), and the fetcher updating its FETCH (the publisher
 * applies the priority and answers REQUEST_OK once FETCH_OK has gone out).
 * Each update consumes a request ID like any request.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <string.h>

static moq_simpair_t *mk_pair(void)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = MOQ_VERSION_DRAFT_22;
    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&scfg, &sp) < 0) return NULL;
    if (moq_simpair_start(sp) < 0) { moq_simpair_destroy(sp); return NULL; }
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    return sp;
}

static void drain(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    bool found = false;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (!found && ev.kind == kind) { *out = ev; found = true; continue; }
        moq_event_cleanup(&ev);
    }
    return found;
}

/* Feed a REQUEST_UPDATE with one integer parameter on `ref`. */
static moq_result_t feed_update(moq_session_t *s, moq_stream_ref_t ref, uint64_t request_id,
                                uint64_t initial_request_type, bool from_subscriber,
                                uint64_t param_type, uint64_t value)
{
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_REQUEST_UPDATE;
    m.request_id = request_id;
    moq_d22_param_t p;
    memset(&p, 0, sizeof(p));
    p.type = param_type; p.value.integer = value;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request_type = initial_request_type;
    ctx.update_from_subscriber = from_subscriber;
    ctx.max_filter_ranges = 8;
    uint8_t buf[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    moq_result_t rc = moq_d22_encode_message(&w, &m, &p, 1, &ctx);
    if (rc < 0) return rc;
    return moq_session_on_bidi_stream_bytes(s, ref, buf, moq_buf_writer_offset(&w), false, 9);
}

/* Whether a REQUEST_OK was queued on `ref` (an update acknowledgement). */
static bool took_update_ok(moq_session_t *s, moq_stream_ref_t ref)
{
    bool found = false;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == ref._v) {
            /* Every message in the send, not only the first. */
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            while (moq_buf_reader_remaining(&r) > 0 &&
                   moq_d22_decode_envelope(&r, &env) == MOQ_OK)
                if (env.msg_type == MOQ_D22_REQUEST_OK)
                    found = true;
        }
        moq_action_cleanup(&a);
    }
    return found;
}

static int test_publisher_updates_publish(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t ph;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(c, &cfg, 1, &ph), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev));
    moq_publication_t sph = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(sv, sph, &ac, 2), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    int ss = pub_resolve_handle(sv, sph);
    MOQ_TEST_CHECK(ss >= 0);
    if (ss < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t sref = sv->publishes[ss].rs.ref;
    MOQ_TEST_CHECK(sref._v != 0);

    /* The publisher pauses its publication through an update on its own
     * PUBLISH stream (client parity: request ID 2). */
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(sv, sref, 2, MOQ_D22_PUBLISH, false, 0x10, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_PUBLISH_UPDATED, &ev));
    MOQ_TEST_CHECK(ev.u.publish_updated.pub._opaque == sph._opaque);
    MOQ_TEST_CHECK(ev.u.publish_updated.has_forward && !ev.u.publish_updated.forward);
    MOQ_TEST_CHECK(!ev.u.publish_updated.has_subscriber_priority);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(took_update_ok(sv, sref));

    /* The update consumed ID 2: the client's next request is seen as 4. */
    moq_subscribe_cfg_t scfg; moq_subscribe_cfg_init(&scfg);
    scfg.track_namespace = (moq_namespace_t){ parts, 1 };
    scfg.track_name = MOQ_BYTES_LITERAL("t");
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &scfg, 3, &ch), (int)MOQ_OK);
    /* The client itself has not consumed ID 2, so make its next ID 4 by hand:
     * the engine's parity counter is per side, and the test injected the
     * update. Feed the client's SUBSCRIBE bytes via the simulator and let the
     * server see whichever ID the client used; a duplicate (2) must close. */
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_CLOSED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_fetcher_updates_fetch(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_fetch_cfg_t cfg; moq_fetch_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.start_group = 0; cfg.start_object = 0; cfg.end_group = 10; cfg.end_object = 0;
    moq_fetch_t fh;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch(c, &cfg, 1, &fh), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_INT((int)ev.u.fetch_request.subscriber_priority, 128);
    moq_fetch_t sfh = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    drain(c);
    int fs = -1;
    for (size_t i = 0; i < sv->fetch_cap; i++)
        if (sv->fetches[i].handle._opaque == sfh._opaque) { fs = (int)i; break; }
    MOQ_TEST_CHECK(fs >= 0);
    if (fs < 0) { moq_simpair_destroy(sp); return failures; }
    moq_stream_ref_t sref = sv->fetches[fs].rs.ref;
    MOQ_TEST_CHECK(sref._v != 0);

    /* The fetcher raises the priority on its FETCH stream (request ID 2). */
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(sv, sref, 2, MOQ_D22_FETCH, true, 0x20, 3), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)sv->fetches[fs].subscriber_priority, 3);
    /* FETCH_OK is the first message on the stream: the acknowledgement
     * waits for the answer to the fetch. */
    MOQ_TEST_CHECK(!took_update_ok(sv, sref));
    /* The fetch itself is unaffected: the publisher can still accept it,
     * and the acknowledgement follows FETCH_OK. */
    moq_accept_fetch_cfg_t afc; moq_accept_fetch_cfg_init(&afc);
    afc.end_group = 10; afc.empty = false;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_fetch(sv, sfh, &afc, 10), (int)MOQ_OK);
    MOQ_TEST_CHECK(took_update_ok(sv, sref));
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_publisher_updates_publish();
    failures += test_fetcher_updates_fetch();
    if (failures) { fprintf(stderr, "FAIL: test_d22_update_peers (%d)\n", failures); return 1; }
    printf("PASS: test_d22_update_peers\n");
    return 0;
}
