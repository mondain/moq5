/*
 * Track Properties on a draft-22 REQUEST_OK (§9.3): "Track Properties are
 * populated in TRACK_STATUS_OK; they are empty in PUBLISH_OK,
 * REQUEST_UPDATE_OK, SUBSCRIBE_NAMESPACE_OK and PUBLISH_NAMESPACE_OK. If an
 * endpoint receives Track Properties in one of these messages it MUST close
 * the session with a PROTOCOL_VIOLATION." SUBSCRIBE_TRACKS_OK is in neither
 * list, so properties on it do not close the session: the subscriber reads
 * past them and the subscription is established. PUBLISH_NAMESPACE_OK with
 * properties still closes.
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

static int count_kind(moq_session_t *s, moq_event_kind_t kind)
{
    moq_event_t ev; int n = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) { if (ev.kind == kind) n++; moq_event_cleanup(&ev); }
    return n;
}

/* A REQUEST_OK with one Track Property (type 0x78, application range). */
static size_t encode_ok_with_property(uint8_t *buf, size_t cap)
{
    /* Envelope: type 0x7, 16-bit length, zero parameters, then properties. */
    static const uint8_t body[] = { 0x00, 0x78, 0x01 };
    if (cap < 3 + sizeof(body)) return 0;
    buf[0] = 0x07; buf[1] = 0; buf[2] = (uint8_t)sizeof(body);
    memcpy(buf + 3, body, sizeof(body));
    return 3 + sizeof(body);
}

static int test_subscribe_tracks_ok(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    (void)count_kind(c, 0);
    moq_subscribe_tracks_cfg_t cfg; moq_subscribe_tracks_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace_prefix = (moq_namespace_t){ parts, 1 };
    moq_track_sub_handle_t th;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_subscribe_tracks(c, &cfg, 1, &th), (int)MOQ_OK);
    int slot = track_sub_resolve_handle(c, th);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) { moq_simpair_destroy(sp); return failures; }
    uint8_t buf[16];
    size_t n = encode_ok_with_property(buf, sizeof(buf));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(c, c->track_subs[slot].rs.ref,
                                                                buf, n, false, 5), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT(count_kind(c, MOQ_EVENT_SUBSCRIBE_TRACKS_OK), 1);
    MOQ_TEST_CHECK(c->track_subs[slot].state == MOQ_TRACK_SUB_ESTABLISHED);
    moq_simpair_destroy(sp);
    return failures;
}

static int test_publish_namespace_ok(void)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair();
    MOQ_TEST_CHECK(sp != NULL);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp);
    (void)count_kind(c, 0);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    moq_announcement_t ah;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish_namespace(c, &cfg, 1, &ah), (int)MOQ_OK);
    int slot = -1;
    for (size_t i = 0; i < c->ann_cap; i++)
        if (c->announcements[i].handle._opaque == ah._opaque) { slot = (int)i; break; }
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) { moq_simpair_destroy(sp); return failures; }
    uint8_t buf[16];
    size_t n = encode_ok_with_property(buf, sizeof(buf));
    (void)moq_session_on_bidi_stream_bytes(c, c->announcements[slot].rs.ref, buf, n, false, 5);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_CLOSED);
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_subscribe_tracks_ok();
    failures += test_publish_namespace_ok();
    MOQ_TEST_PASS("d22_tracks_ok_properties");
    return failures != 0;
}
