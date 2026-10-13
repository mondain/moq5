#include <moq/moq.h>
#include <moq/control_d22.h>
#include <moq/publisher.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"

static int failures;

static void drain(moq_session_t *s)
{
    moq_event_t e;
    moq_action_t a;
    while (moq_session_poll_events(s, &e, 1) > 0) moq_event_cleanup(&e);
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

static moq_session_t *session_version(moq_perspective_t perspective,
                                      moq_version_t version)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), perspective);
    cfg.version = version;
    moq_session_t *s = NULL;
    MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
    if (!s) return NULL;
    MOQ_TEST_CHECK(moq_session_start(s, 0) == MOQ_OK);
    drain(s);
    uint8_t bytes[16];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    MOQ_TEST_CHECK(moq_d22_encode_setup_opts(&w, NULL) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_on_control_bytes(s, bytes,
        moq_buf_writer_offset(&w), 0) == MOQ_OK);
    drain(s);
    return s;
}

static moq_session_t *session(moq_perspective_t perspective)
{
    return session_version(perspective, MOQ_VERSION_DRAFT_22);
}

static moq_ns_sub_handle_t accept_namespace(moq_session_t *s,
                                           moq_stream_ref_t ref)
{
    moq_bytes_t part = MOQ_BYTES_LITERAL("live");
    moq_namespace_t ns = { &part, 1 };
    uint8_t bytes[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    moq_d22_msg_params_t params = {0};
    uint64_t id = s->perspective == MOQ_PERSPECTIVE_CLIENT ? 1 : 0;
    MOQ_TEST_CHECK(moq_d22_encode_subscribe_namespace(&w, id, &ns, &params) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
        moq_buf_writer_offset(&w), false, 1) == MOQ_OK);
    moq_ns_sub_handle_t handle = MOQ_NS_SUB_HANDLE_INVALID;
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1) > 0) {
        if (e.kind == MOQ_EVENT_NS_SUB_REQUEST) handle = e.u.ns_sub_request.handle;
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK(handle._opaque != MOQ_NS_SUB_HANDLE_INVALID._opaque);
    moq_accept_ns_sub_cfg_t accept;
    moq_accept_ns_sub_cfg_init(&accept);
    MOQ_TEST_CHECK(moq_session_accept_ns_sub(s, handle, &accept, 1) == MOQ_OK);
    drain(s);
    return handle;
}

static uint64_t close_code(moq_session_t *s)
{
    uint64_t code = UINT64_MAX;
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_CLOSE_SESSION) code = a.u.close_session.code;
        moq_action_cleanup(&a);
    }
    return code;
}

/* D22 9.2: publisher-side SUBSCRIBE_NAMESPACE must parse GOAWAY, migrate
 * exactly once, and retain its stream identity to reject a later duplicate. */
static void namespace_goaway(unsigned mode)
{
    moq_session_t *s = session(mode == 11 || mode == 12
        ? MOQ_PERSPECTIVE_SERVER : MOQ_PERSPECTIVE_CLIENT);
    if (!s) return;
    moq_stream_ref_t ref = moq_stream_ref_from_u64(101);
    moq_ns_sub_handle_t handle = accept_namespace(s, ref);
    uint8_t bytes[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    const char *uri = mode == 12 || mode == 13 ? "https://next.example/moq" : NULL;
    MOQ_TEST_CHECK(moq_d22_encode_goaway(&w, (const uint8_t *)uri,
        uri ? strlen(uri) : 0, 10000) == MOQ_OK);
    size_t n = moq_buf_writer_offset(&w);
    size_t slot = moq_handle_slot(handle._opaque);
    bool fin = mode == 6 || mode == 7 || mode == 9 || mode == 10;
    if (mode == 7 || mode == 8) {
        for (size_t i = 0; i < s->drain_ref_cap; i++)
            MOQ_TEST_CHECK(drain_ref_add(s, moq_stream_ref_from_u64(1000 + i)));
    }

    if (mode == 2) {
        /* Two GOAWAY messages coalesced in one transport callback. */
        memcpy(bytes + n, bytes, n);
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
            2 * n, false, 2) == MOQ_OK);
        MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED);
        MOQ_TEST_CHECK(close_code(s) == 3);
        moq_session_destroy(s);
        return;
    }
    if (mode == 3 || mode == 12) {
        /* A FIN cannot make a truncated GOAWAY disappear. */
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
            mode == 3 ? n - 1 : n, mode == 3, 2) == MOQ_OK);
        MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED);
        MOQ_TEST_CHECK(close_code(s) == 3);
        moq_session_destroy(s);
        return;
    }
    if (mode == 4 || mode == 5 || mode == 9 || mode == 10 || mode == 8) {
        /* Full event/action queue: no terminal event or retirement before
         * all capacity is reserved. Empty re-feed must retry owned bytes. */
        bool events_full = mode == 4 || mode == 10;
        while (mode != 8 && (events_full ? !event_queue_full(s) : !action_queue_full(s))) {
            if (events_full) {
                moq_event_t e = {0}; e.kind = MOQ_EVENT_REQUEST_READY;
                MOQ_TEST_CHECK(push_event(s, &e) == MOQ_OK);
            } else {
                moq_action_t a = {0}; a.kind = MOQ_ACTION_CLOSE_BIDI_STREAM;
                a.u.close_bidi_stream.stream_ref = moq_stream_ref_from_u64(999);
                MOQ_TEST_CHECK(push_action(s, &a) == MOQ_OK);
            }
        }
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
            n, fin, 2) == MOQ_ERR_WOULD_BLOCK);
        MOQ_TEST_CHECK(s->ns_subs[slot].state == MOQ_NS_SUB_ESTABLISHED);
        MOQ_TEST_CHECK(s->ns_subs[slot].handle._opaque == handle._opaque);
        drain(s);
        if (mode == 8) {
            for (size_t i = 0; i < s->drain_ref_cap; i++)
                drain_ref_remove(s, moq_stream_ref_from_u64(1000 + i));
        }
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, NULL,
            0, false, 3) == MOQ_OK);
    } else if (mode == 1) {
        for (size_t i = 0; i < n; i++) {
            MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes + i,
                1, false, 2 + i) == MOQ_OK);
            if (i + 1 < n) {
                MOQ_TEST_CHECK(s->ns_subs[slot].state == MOQ_NS_SUB_ESTABLISHED);
                MOQ_TEST_CHECK(!moq_session_has_events(s));
            }
        }
    } else {
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
            n, fin, 2) == MOQ_OK);
    }
    MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED);
    unsigned migrated = 0;
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1) > 0) {
        if (e.kind == MOQ_EVENT_REQUEST_GOAWAY) {
            migrated++;
            MOQ_TEST_CHECK(e.u.request_goaway.family == MOQ_REQUEST_FAMILY_NS_SUB);
            MOQ_TEST_CHECK(e.u.request_goaway.handle.raw == handle._opaque);
            MOQ_TEST_CHECK(e.u.request_goaway.timeout_ms == 10000);
            MOQ_TEST_CHECK(e.u.request_goaway.new_session_uri.len == (uri ? strlen(uri) : 0));
            if (uri) MOQ_TEST_CHECK(memcmp(e.u.request_goaway.new_session_uri.data,
                uri, strlen(uri)) == 0);
        }
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK(migrated == 1);
    MOQ_TEST_CHECK(fin ? !drain_ref_contains(s, ref) :
        drain_ref_reason(s, ref) == MOQ_DRAIN_GOAWAY_STRICT);
    drain(s);
    if (!fin) {
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, bytes,
            n, false, 20) == MOQ_OK);
        MOQ_TEST_CHECK(s->state == MOQ_SESS_CLOSED);
        MOQ_TEST_CHECK(close_code(s) == 3);
    }
    moq_session_destroy(s);
}

/* D22 2.4.3/6.5: public application publication cannot claim reserved names.
 * Dots elsewhere, binary names and root namespaces remain valid. */
static void application_publication(moq_namespace_t ns, bool allowed)
{
    moq_session_t *s = session(MOQ_PERSPECTIVE_CLIENT);
    if (!s) return;
    moq_publish_namespace_cfg_t nc;
    moq_publish_namespace_cfg_init(&nc); nc.track_namespace = ns;
    moq_announcement_t announcement;
    MOQ_TEST_CHECK(moq_session_publish_namespace(s, &nc, 1, &announcement) ==
        (allowed ? MOQ_OK : MOQ_ERR_INVAL));
    if (!allowed) {
        MOQ_TEST_CHECK(announcement._opaque == MOQ_ANNOUNCEMENT_INVALID._opaque);
        MOQ_TEST_CHECK(s->action_tail == s->action_head);
    }
    drain(s);
    moq_publish_cfg_t pc;
    moq_publish_cfg_init(&pc); pc.track_namespace = ns;
    pc.track_name = MOQ_BYTES_LITERAL("v");
    moq_publication_t publication;
    MOQ_TEST_CHECK(moq_session_publish(s, &pc, 2, &publication) ==
        (allowed ? MOQ_OK : MOQ_ERR_INVAL));
    if (!allowed) {
        MOQ_TEST_CHECK(publication._opaque == MOQ_PUBLICATION_INVALID._opaque);
        MOQ_TEST_CHECK(s->action_tail == s->action_head);
    }
    drain(s);
    moq_pub_cfg_t cfg; moq_pub_cfg_init(&cfg);
    moq_publisher_t *pub = NULL;
    MOQ_TEST_CHECK(moq_pub_create(s, &s->alloc, &cfg, &pub) == MOQ_OK);
    if (pub) {
        moq_pub_track_cfg_t tc; moq_pub_track_cfg_init(&tc);
        tc.track_namespace = ns; tc.track_name = MOQ_BYTES_LITERAL("facade");
        tc.advertise_namespace = false;
        moq_pub_track_t *track = NULL;
        MOQ_TEST_CHECK(moq_pub_add_track(pub, &tc, 3, &track) ==
            (allowed ? MOQ_OK : MOQ_ERR_INVAL));
        if (!allowed) {
            MOQ_TEST_CHECK(track == NULL);
            MOQ_TEST_CHECK(s->action_tail == s->action_head);
        }
        moq_pub_destroy(pub);
    }
    moq_session_destroy(s);
}

static void inbound_reserved_extension(void)
{
    moq_session_t *s = session(MOQ_PERSPECTIVE_SERVER);
    if (!s) return;
    moq_bytes_t part = MOQ_BYTES_LITERAL(".future-extension");
    moq_namespace_t ns = { &part, 1 };
    moq_d22_msg_params_t params = {0};
    uint8_t bytes[128]; moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    MOQ_TEST_CHECK(moq_d22_encode_publish_namespace(&w, 0, &ns, &params) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, moq_stream_ref_from_u64(4),
        bytes, moq_buf_writer_offset(&w), false, 1) == MOQ_OK);
    unsigned delivered = 0; moq_event_t e;
    while (moq_session_poll_events(s, &e, 1) > 0) {
        if (e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) delivered++;
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK(delivered == 1);
    MOQ_TEST_CHECK(s->state == MOQ_SESS_ESTABLISHED);
    moq_session_destroy(s);
}

/* A requester FIN does not cancel D22 SUBSCRIBE_NAMESPACE (6.4.2.2).
 * The cumulative FIN must survive initial handoff and event-queue refusal. */
static void namespace_request_fin(bool blocked)
{
    moq_session_t *s = session(MOQ_PERSPECTIVE_CLIENT);
    if (!s) return;
    moq_stream_ref_t ref = moq_stream_ref_from_u64(105);
    if (blocked) {
        while (!event_queue_full(s)) {
            moq_event_t e = {0}; e.kind = MOQ_EVENT_REQUEST_READY;
            MOQ_TEST_CHECK(push_event(s, &e) == MOQ_OK);
        }
    }
    moq_bytes_t part = MOQ_BYTES_LITERAL("live");
    moq_namespace_t ns = { &part, 1 };
    moq_d22_msg_params_t params = {0};
    uint8_t bytes[64]; moq_buf_writer_t w;
    moq_buf_writer_init(&w, bytes, sizeof(bytes));
    MOQ_TEST_CHECK(moq_d22_encode_subscribe_namespace(&w, 1, &ns, &params) == MOQ_OK);
    moq_result_t rc = moq_session_on_bidi_stream_bytes(s, ref, bytes,
        moq_buf_writer_offset(&w), true, 1);
    MOQ_TEST_CHECK(rc == (blocked ? MOQ_ERR_WOULD_BLOCK : MOQ_OK));
    if (blocked) {
        drain(s);
        MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, NULL, 0,
            false, 2) == MOQ_OK);
    }
    moq_ns_sub_handle_t h = MOQ_NS_SUB_HANDLE_INVALID;
    moq_event_t e;
    while (moq_session_poll_events(s, &e, 1) > 0) {
        if (e.kind == MOQ_EVENT_NS_SUB_REQUEST) h = e.u.ns_sub_request.handle;
        moq_event_cleanup(&e);
    }
    MOQ_TEST_CHECK(h._opaque != MOQ_NS_SUB_HANDLE_INVALID._opaque);
    moq_accept_ns_sub_cfg_t cfg; moq_accept_ns_sub_cfg_init(&cfg);
    MOQ_TEST_CHECK(moq_session_accept_ns_sub(s, h, &cfg, 3) == MOQ_OK);
    drain(s);
    /* An empty repeat of the FIN likewise leaves the accepted request alive. */
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(s, ref, NULL, 0, true, 4) == MOQ_OK);
    MOQ_TEST_CHECK(s->ns_subs[moq_handle_slot(h._opaque)].state == MOQ_NS_SUB_ESTABLISHED);
    MOQ_TEST_CHECK(!drain_ref_contains(s, ref));
    moq_session_destroy(s);
}

static void legacy_namespace_publication(void)
{
    moq_session_t *s = session_version(MOQ_PERSPECTIVE_CLIENT, MOQ_VERSION_DRAFT_18);
    if (!s) return;
    moq_bytes_t part = MOQ_BYTES_LITERAL(".session");
    moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ &part, 1 };
    moq_announcement_t h;
    MOQ_TEST_CHECK(moq_session_publish_namespace(s, &cfg, 1, &h) == MOQ_OK);
    moq_session_destroy(s);
}

int main(void)
{
    for (unsigned mode = 0; mode < 14; mode++) namespace_goaway(mode);
    const char *names[] = { ".", ".session", ".unregistered", "live.with.dots" };
    for (size_t i = 0; i < 4; i++) {
        moq_bytes_t part = { (const uint8_t *)names[i], strlen(names[i]) };
        application_publication((moq_namespace_t){ &part, 1 }, i == 3);
    }
    const uint8_t binary[] = { 0, '.', 255 };
    moq_bytes_t parts[] = { { binary, sizeof(binary) }, MOQ_BYTES_LITERAL(".session") };
    application_publication((moq_namespace_t){ parts, 2 }, true);
    application_publication((moq_namespace_t){ NULL, 0 }, true);
    inbound_reserved_extension();
    namespace_request_fin(false);
    namespace_request_fin(true);
    legacy_namespace_publication();
    if (failures) fprintf(stderr, "d22 namespace policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
