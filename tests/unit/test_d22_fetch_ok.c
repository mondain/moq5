/*
 * FETCH_OK End Location on draft-22 is inclusive: "The end of the range
 * covered by the FETCH response, inclusive" (§9.12). The session API keeps
 * one form for every draft: an exclusive end object, zero meaning the whole
 * end group. Accepting a fetch with end (G, O) therefore sends {G, O - 1},
 * and a whole end group is sent as {G, 2^64-1}, the last Location of the
 * group (§3.2.2 names {Group, 2^64-1} the last expected Object of a group);
 * a received End Location {G, O} surfaces as (G, O + 1), and {G, 2^64-1} as
 * (G, 0). "If End Location is smaller than the Start Location in the
 * corresponding FETCH the receiver MUST close the session with a
 * PROTOCOL_VIOLATION" -- and only then: a range of one object ends where it
 * starts.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <moq/control_d18.h>
#include <stdio.h>
#include <string.h>

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

typedef struct {
    moq_simpair_t *sp;
    moq_fetch_t client_h;
    moq_fetch_t server_h;
} fetch_pair_t;

/* A draft-22 pair whose server holds an unanswered FETCH for
 * [(sg, so), (eg, eo)) -- exclusive end, eo == 0 the whole end group. */
static bool pending_fetch(fetch_pair_t *fp, uint64_t sg, uint64_t so,
                          uint64_t eg, uint64_t eo)
{
    memset(fp, 0, sizeof(*fp));
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    if (moq_simpair_create(&cfg, &fp->sp) < 0) return false;
    if (moq_simpair_start(fp->sp) < 0) return false;
    moq_simpair_run_until_quiescent(fp->sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(fp->sp), *sv = moq_simpair_server(fp->sp);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t f; moq_fetch_cfg_init(&f);
    f.track_namespace = (moq_namespace_t){ parts, 1 };
    f.track_name = MOQ_BYTES_LITERAL("v");
    f.start_group = sg; f.start_object = so;
    f.end_group = eg; f.end_object = eo;
    if (moq_session_fetch(c, &f, 1, &fp->client_h) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(fp->sp, 16, NULL);
    moq_event_t ev;
    if (!take(sv, MOQ_EVENT_FETCH_REQUEST, &ev)) return false;
    fp->server_h = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    return true;
}

/* The End Location of the FETCH_OK the server queued, as sent. */
static bool sent_end_location(moq_session_t *sv, moq_d22_location_t *out)
{
    bool found = false;
    moq_action_t a;
    while (moq_session_poll_actions(sv, &a, 1) > 0) {
        if (!found && a.kind == MOQ_ACTION_SEND_BIDI_STREAM) {
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, a.u.send_bidi_stream.data, a.u.send_bidi_stream.len);
            moq_control_envelope_t env;
            moq_d22_message_context_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            moq_d22_message_t m;
            if (moq_d22_decode_envelope(&r, &env) == MOQ_OK &&
                env.msg_type == MOQ_D22_FETCH_OK &&
                moq_d22_decode_message(MOQ_D22_FETCH_OK, env.payload,
                                       env.payload_len, &ctx, &m) == MOQ_OK) {
                *out = m.end_location;
                found = true;
            }
        }
        moq_action_cleanup(&a);
    }
    return found;
}

/* Accepting with the session's exclusive end puts the inclusive Location on
 * the wire. */
static int accept_sends(uint64_t eg, uint64_t eo, uint64_t wire_g, uint64_t wire_o)
{
    int failures = 0;
    fetch_pair_t fp;
    MOQ_TEST_CHECK(pending_fetch(&fp, 3, 0, 3, 0));
    if (fp.sp) {
        moq_session_t *sv = moq_simpair_server(fp.sp);
        moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
        ac.end_group = eg; ac.end_object = eo;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_fetch(sv, fp.server_h, &ac, 5),
                              (int)MOQ_OK);
        moq_d22_location_t loc = { 0, 0 };
        MOQ_TEST_CHECK(sent_end_location(sv, &loc));
        MOQ_TEST_CHECK_EQ_U64(loc.group, wire_g);
        MOQ_TEST_CHECK_EQ_U64(loc.object, wire_o);
    }
    moq_simpair_destroy(fp.sp);
    return failures;
}

/* Feed the fetcher a FETCH_OK with the given End Location on its request
 * stream. */
static moq_result_t feed_fetch_ok(moq_session_t *c, moq_fetch_t h,
                                  uint64_t g, uint64_t o)
{
    int slot = fetch_resolve_handle(c, h);
    if (slot < 0) return MOQ_ERR_STALE_HANDLE;
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_FETCH_OK;
    m.end_location.group = g;
    m.end_location.object = o;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint8_t buf[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    if (moq_d22_encode_message(&w, &m, NULL, 0, &ctx) != MOQ_OK) return MOQ_ERR_INVAL;
    return moq_session_on_bidi_stream_bytes(c, c->fetches[slot].rs.ref, buf,
                                            moq_buf_writer_offset(&w), false, 7);
}

/* A received End Location surfaces in the session's exclusive form, or the
 * session closes when it lies before the requested start. */
static int receive(uint64_t sg, uint64_t so, uint64_t eg, uint64_t eo,
                   uint64_t wire_g, uint64_t wire_o,
                   bool exp_close, uint64_t exp_eg, uint64_t exp_eo)
{
    int failures = 0;
    fetch_pair_t fp;
    MOQ_TEST_CHECK(pending_fetch(&fp, sg, so, eg, eo));
    if (fp.sp) {
        moq_session_t *c = moq_simpair_client(fp.sp);
        (void)feed_fetch_ok(c, fp.client_h, wire_g, wire_o);
        if (exp_close) {
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_CLOSED);
            moq_event_t ev;
            memset(&ev, 0, sizeof(ev));
            MOQ_TEST_CHECK(take(c, MOQ_EVENT_SESSION_CLOSED, &ev));
            if (ev.kind == MOQ_EVENT_SESSION_CLOSED) {
                MOQ_TEST_CHECK_EQ_U64(ev.u.closed.code, 0x3);
                moq_event_cleanup(&ev);
            }
        } else {
            MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
            moq_event_t ev;
            memset(&ev, 0, sizeof(ev));
            MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_OK, &ev));
            if (ev.kind == MOQ_EVENT_FETCH_OK) {
                MOQ_TEST_CHECK(ev.u.fetch_ok.fetch._opaque == fp.client_h._opaque);
                MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_group, exp_eg);
                MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_object, exp_eo);
                moq_event_cleanup(&ev);
            }
        }
    }
    moq_simpair_destroy(fp.sp);
    return failures;
}

/* Accept echoes the request's end, the fetcher sees the same end back. */
static int round_trip(uint64_t sg, uint64_t so, uint64_t eg, uint64_t eo)
{
    int failures = 0;
    fetch_pair_t fp;
    MOQ_TEST_CHECK(pending_fetch(&fp, sg, so, eg, eo));
    if (fp.sp) {
        moq_session_t *c = moq_simpair_client(fp.sp), *sv = moq_simpair_server(fp.sp);
        moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
        ac.end_group = eg; ac.end_object = eo;
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_fetch(sv, fp.server_h, &ac, 5),
                              (int)MOQ_OK);
        moq_simpair_run_until_quiescent(fp.sp, 16, NULL);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
        moq_event_t ev;
        memset(&ev, 0, sizeof(ev));
        MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_OK, &ev));
        if (ev.kind == MOQ_EVENT_FETCH_OK) {
            MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_group, eg);
            MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_ok.end_object, eo);
            moq_event_cleanup(&ev);
        }
    }
    moq_simpair_destroy(fp.sp);
    return failures;
}

/* A FETCH object larger than this side's max_object_payload_size is a local
 * resource limit, not a peer error: the fetcher cancels the fetch. §3.2.4: a
 * subscriber cancelling a FETCH "MAY send STOP_SENDING for the data stream as
 * well as the bidi request stream. It MUST send STOP_SENDING for the bidi
 * request stream." The request stream is aborted (RESET_STREAM and
 * STOP_SENDING, §6.4.2.3) and the data stream stopped, both with CANCELLED
 * (§12.5); nothing is delivered for the object; FETCH_RESET with cause
 * LOCAL_CANCEL reports the fetch's end at once; the session stays up, and
 * the publisher's later bytes and resets are absorbed without a second
 * terminal. Draft-18 shares the fetch reader and the rule. */
/* max_actions: the action queue the sessions have (0: the default). With
 * `pressure`, all but one entry is taken when the object arrives: the cancel
 * waits whole, with nothing queued, until the queue drains. The cancel needs
 * exactly two entries (the request-stream abort, or a draft-16 FETCH_CANCEL,
 * and the data-stream STOP), so a two-entry queue completes it. */
static int oversized_fetch_object_q(moq_version_t version, uint32_t max_actions,
                                    bool pressure)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.max_actions = max_actions;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.max_object_payload_size = 4;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    if (!sp) return failures;
    MOQ_TEST_CHECK(moq_simpair_start(sp) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(sp);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) moq_event_cleanup(&ev);

    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t f; moq_fetch_cfg_init(&f);
    f.track_namespace = (moq_namespace_t){ parts, 1 };
    f.track_name = MOQ_BYTES_LITERAL("v");
    f.start_group = 0; f.end_group = 1;
    moq_fetch_t h;
    MOQ_TEST_CHECK(moq_session_fetch(c, &f, 1, &h) == MOQ_OK);
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0);
    moq_action_t a;
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM) ref = a.u.open_bidi_stream.stream_ref;
        moq_action_cleanup(&a);
    }
    int slot = fetch_resolve_handle(c, h);
    MOQ_TEST_CHECK(slot >= 0 && ref._v != 0);
    if (slot < 0) { moq_simpair_destroy(sp); return failures; }
    uint64_t request_id = c->fetches[slot].request_id;

    /* FETCH_OK: End Location {1, 2^64-1} (draft-22) / {1, 0} (draft-18). */
    uint8_t ok[32];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, ok, sizeof(ok));
    if (version == MOQ_VERSION_DRAFT_22) {
        static const uint8_t d22_ok[] = {0x18, 0, 12, 0, 1,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
        moq_buf_write_raw(&w, d22_ok, sizeof(d22_ok));
    } else {
        moq_d18_encode_fetch_ok(&w, false, (moq_d18_location_t){ 1, 0 }, (moq_bytes_t){0});
    }
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_bytes(c, ref, ok,
        moq_buf_writer_offset(&w), false, 2) == MOQ_OK);
    MOQ_TEST_CHECK(take(c, MOQ_EVENT_FETCH_OK, &ev));
    moq_event_cleanup(&ev);

    /* FETCH_HEADER, then one object: flags 0x1F (group, explicit subgroup,
     * object, priority), {0, 0, 0}, priority 3, payload length 8. */
    const moq_stream_ref_t data = moq_stream_ref_from_u64(0x9003);
    uint8_t stream[32];
    moq_buf_writer_init(&w, stream, sizeof(stream));
    moq_buf_write_vi64(&w, 0x05);
    moq_buf_write_vi64(&w, request_id);
    static const uint8_t object[] = {0x1f, 0, 0, 0, 3, 8, 1, 2, 3, 4, 5, 6, 7, 8};
    moq_buf_write_raw(&w, object, sizeof(object));
    moq_track_status_handle_t occupier;
    if (pressure) {
        moq_track_status_cfg_t tc;
        moq_track_status_cfg_init(&tc);
        tc.track_namespace = (moq_namespace_t){ parts, 1 };
        tc.track_name = MOQ_BYTES_LITERAL("w");
        for (uint32_t i = 0; i + 1 < max_actions; i++)
            MOQ_TEST_CHECK(moq_session_track_status(c, &tc, 3, &occupier) == MOQ_OK);
    }
    moq_result_t frc = moq_session_on_data_bytes(c, data, stream,
        moq_buf_writer_offset(&w), false, 3);
    MOQ_TEST_CHECK(frc == MOQ_OK || (pressure && frc == MOQ_ERR_WOULD_BLOCK));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    if (pressure) {
        /* Nothing of the cancel is queued yet; the occupiers drain. */
        while (moq_session_poll_actions(c, &a, 1) > 0) {
            MOQ_TEST_CHECK(a.kind == MOQ_ACTION_OPEN_BIDI_STREAM);
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK(fetch_resolve_handle(c, h) >= 0);
        while (moq_session_poll_events(c, &ev, 1) > 0) {
            MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_FETCH_RESET);
            moq_event_cleanup(&ev);
        }
        (void)moq_session_on_data_bytes(c, data, NULL, 0, false, 3);
    }

    unsigned stops = 0, aborts = 0;
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        if (a.kind == MOQ_ACTION_STOP_DATA && a.u.stop_data.stream_ref._v == data._v) {
            MOQ_TEST_CHECK_EQ_U64(a.u.stop_data.error_code, 0x1);
            ++stops;
        }
        if (a.kind == MOQ_ACTION_ABORT_BIDI_STREAM &&
            a.u.abort_bidi_stream.stream_ref._v == ref._v) {
            MOQ_TEST_CHECK_EQ_U64(a.u.abort_bidi_stream.error_code, 0x1);
            ++aborts;
        }
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT((int)stops, 1);
    MOQ_TEST_CHECK_EQ_INT((int)aborts, 1);
    unsigned resets = 0;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_FETCH_OBJECT &&
                       ev.kind != MOQ_EVENT_SESSION_CLOSED);
        if (ev.kind == MOQ_EVENT_FETCH_RESET) {
            MOQ_TEST_CHECK(ev.u.fetch_reset.fetch._opaque == h._opaque);
            MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.cause, MOQ_REQUEST_TERMINAL_LOCAL_CANCEL);
            MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.error_code, 0x1);
            MOQ_TEST_CHECK(ev.u.fetch_reset.data_stream);
            ++resets;
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_INT((int)resets, 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch_cancel(c, h, 4), (int)MOQ_ERR_STALE_HANDLE);

    /* The publisher may still deliver what it had sent, then answer the
     * cancel with resets (§3.2.4): absorbed, no second terminal. */
    static const uint8_t next[] = {0x04, 1, 2, 0xaa, 0xbb};
    MOQ_TEST_CHECK(moq_session_on_data_bytes(c, data, next, sizeof(next),
                                             false, 4) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_on_data_reset(c, data, 0x1, 5) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_on_bidi_stream_reset(c, ref, 0x1, 5) == MOQ_OK);
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_FETCH_OBJECT &&
                       ev.kind != MOQ_EVENT_FETCH_RESET &&
                       ev.kind != MOQ_EVENT_SESSION_CLOSED);
        moq_event_cleanup(&ev);
    }
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        MOQ_TEST_CHECK(a.kind != MOQ_ACTION_CLOSE_SESSION);
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(fetch_resolve_handle(c, h) < 0);
    moq_simpair_destroy(sp);
    if (failures) fprintf(stderr, "oversized_fetch_object v=%u actions=%u pressure=%d\n",
                          (unsigned)version, (unsigned)max_actions, (int)pressure);
    return failures;
}

static int oversized_fetch_object(moq_version_t version)
{
    return oversized_fetch_object_q(version, 0, false) +
           oversized_fetch_object_q(version, 2, false) +
           oversized_fetch_object_q(version, 2, true);
}

/* The same cancel end to end on every draft: the publisher accepts, writes
 * an object over the fetcher's limit, and the fetcher's cancel reaches it;
 * both sessions stay up and the fetcher sees exactly one terminal. On
 * draft-16 the cancel is the FETCH_CANCEL message. */
static int oversized_fetch_pair(moq_version_t version)
{
    int failures = 0;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    cfg.max_object_payload_size = 4;
    moq_simpair_t *sp = NULL;
    MOQ_TEST_CHECK(moq_simpair_create(&cfg, &sp) == MOQ_OK);
    if (!sp) return failures;
    MOQ_TEST_CHECK(moq_simpair_start(sp) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_event_t ev;
    while (moq_session_poll_events(c, &ev, 1) > 0) moq_event_cleanup(&ev);
    while (moq_session_poll_events(sv, &ev, 1) > 0) moq_event_cleanup(&ev);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t f; moq_fetch_cfg_init(&f);
    f.track_namespace = (moq_namespace_t){ parts, 1 };
    f.track_name = MOQ_BYTES_LITERAL("v");
    f.start_group = 0; f.end_group = 1;
    moq_fetch_t h;
    MOQ_TEST_CHECK(moq_session_fetch(c, &f, 1, &h) == MOQ_OK);
    moq_simpair_run_until_quiescent(sp, 16, NULL);
    MOQ_TEST_CHECK(take(sv, MOQ_EVENT_FETCH_REQUEST, &ev));
    moq_fetch_t sh = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 1;
    MOQ_TEST_CHECK(moq_session_accept_fetch(sv, sh, &ac, 2) == MOQ_OK);
    moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
    oc.group_id = 0; oc.object_id = 0; oc.publisher_priority = 3;
    static const uint8_t big[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    MOQ_TEST_CHECK(moq_rcbuf_create(moq_alloc_default(), big, sizeof(big), &oc.payload) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_write_fetch_object(sv, sh, &oc, 2) == MOQ_OK);
    moq_rcbuf_decref(oc.payload);
    moq_simpair_run_until_quiescent(sp, 32, NULL);
    unsigned resets = 0;
    while (moq_session_poll_events(c, &ev, 1) > 0) {
        MOQ_TEST_CHECK(ev.kind != MOQ_EVENT_FETCH_OBJECT &&
                       ev.kind != MOQ_EVENT_SESSION_CLOSED);
        if (ev.kind == MOQ_EVENT_FETCH_RESET) {
            MOQ_TEST_CHECK_EQ_U64(ev.u.fetch_reset.cause, MOQ_REQUEST_TERMINAL_LOCAL_CANCEL);
            ++resets;
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK_EQ_INT((int)resets, 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(sp);
    if (failures) fprintf(stderr, "oversized_fetch_pair v=%u\n", (unsigned)version);
    return failures;
}

int main(void)
{
    int failures = 0;

    /* Sending: exclusive end object 6 is the inclusive Location {3, 5}; a
     * whole end group is the group's last Location. */
    failures += accept_sends(3, 6, 3, 5);
    failures += accept_sends(3, 1, 3, 0);
    failures += accept_sends(4, 0, 4, UINT64_MAX);

    /* Receiving, for a FETCH of the single object (3, 5): End {3, 5} is the
     * range itself, not before its start. */
    failures += receive(3, 5, 3, 6, 3, 5, false, 3, 6);
    /* End {3, 9} for the same start: exclusive (3, 10). */
    failures += receive(3, 5, 3, 6, 3, 9, false, 3, 10);
    /* The group's last Location is the whole end group. */
    failures += receive(3, 5, 4, 0, 4, UINT64_MAX, false, 4, 0);
    /* End {3, 0} for start (3, 2) lies before the start: the MUST close.
     * So does an end in an earlier group. */
    failures += receive(3, 2, 3, 6, 3, 0, true, 0, 0);
    failures += receive(3, 2, 3, 6, 3, 1, true, 0, 0);
    failures += receive(3, 2, 3, 6, 2, 9, true, 0, 0);

    /* Both ends of one session agree on the API form. */
    failures += round_trip(3, 5, 3, 6);
    failures += round_trip(3, 0, 5, 0);

    failures += oversized_fetch_object(MOQ_VERSION_DRAFT_22);
    failures += oversized_fetch_object(MOQ_VERSION_DRAFT_18);
    failures += oversized_fetch_pair(MOQ_VERSION_DRAFT_16);
    failures += oversized_fetch_pair(MOQ_VERSION_DRAFT_18);
    failures += oversized_fetch_pair(MOQ_VERSION_DRAFT_22);

    MOQ_TEST_PASS("d22_fetch_ok");
    return failures != 0;
}
