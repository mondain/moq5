/*
 * A request stream this side cancelled is kept as a drain reference until the
 * peer's sending direction ends. The peer's STOP_SENDING speaks only to our
 * direction (draft-22 §6.4.2.2-6.4.2.3, draft-18 §3.3.2, RFC 9000 §3.5), so on
 * both drafts the reference stays until the peer's FIN or RESET arrives.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include <string.h>

static moq_simpair_t *mk_pair(moq_version_t version)
{
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
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

static int test_stop_on_drained_request(moq_version_t version, bool then_reset)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(version);
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain(c); drain(sv);
    moq_subscribe_cfg_t cfg; moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t ch;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe(c, &cfg, 1, &ch), (int)MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain(c); drain(sv);
    moq_stream_ref_t ref = { 0 };
    for (size_t i = 0; i < c->sub_cap; i++)
        if (c->subs[i].rs.ref._v != 0) { ref = c->subs[i].rs.ref; break; }
    MOQ_TEST_CHECK(ref._v != 0);

    /* Cancel before the response: our direction is aborted and the stream is
     * kept as a drain reference for the peer's direction. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(c, ch, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK(drain_ref_contains(c, ref));

    /* The peer's STOP_SENDING concerns our direction only. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_stop(c, ref, 0x1, 6), (int)MOQ_OK);
    MOQ_TEST_CHECK(drain_ref_contains(c, ref));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);

    /* The peer's direction ends: its late bytes are absorbed, and its FIN (or
     * RESET) releases the reference. */
    static const uint8_t late[2] = { 0x07, 0x00 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, ref, late, sizeof(late), false, 7),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK(drain_ref_contains(c, ref));
    if (then_reset)
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_reset(c, ref, 0x1, 8), (int)MOQ_OK);
    else
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, ref, NULL, 0, true, 8),
                              (int)MOQ_OK);
    MOQ_TEST_CHECK(!drain_ref_contains(c, ref));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_stop_on_drained_request(MOQ_VERSION_DRAFT_22, false);
    failures += test_stop_on_drained_request(MOQ_VERSION_DRAFT_22, true);
    failures += test_stop_on_drained_request(MOQ_VERSION_DRAFT_18, false);
    failures += test_stop_on_drained_request(MOQ_VERSION_DRAFT_18, true);
    if (failures) { fprintf(stderr, "FAIL: test_request_drain_stop (%d)\n", failures); return 1; }
    printf("PASS: test_request_drain_stop\n");
    return 0;
}
