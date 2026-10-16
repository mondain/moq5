/*
 * REQUEST_UPDATE on a FETCH this side serves (draft-18 and draft-22).
 *
 * - "A publisher sends a FETCH_OK as the first message on the bidi stream in
 *   response to a successful fetch" (draft-18 §10.13, draft-22 §9.12): an
 *   update that arrives before the fetch is answered is answered after
 *   FETCH_OK, in arrival order, never ahead of it.
 * - "When a REQUEST_UPDATE fails for a FETCH, the publisher MUST reset the
 *   FETCH data stream" (draft-18 §10.9.1, draft-22 §9.5.1): a refused update
 *   ends the fetch. The refusal is the last message on the request stream,
 *   the data stream is reset, the serving application sees
 *   MOQ_EVENT_FETCH_CANCELLED, and the handle is retired.
 * - Draft-22 §9.1.7: an update arriving while MAX_REQUEST_UPDATES answers
 *   are owed on the stream closes the session with TOO_MANY_REQUEST_UPDATES.
 * - Draft-18 §10.2.1: a parameter outside the message types its definition
 *   names closes the session with PROTOCOL_VIOLATION. FORWARD,
 *   SUBSCRIPTION_FILTER and NEW_GROUP_REQUEST appear in a REQUEST_UPDATE
 *   only "for a subscription"; SUBSCRIBER_PRIORITY "for a subscription or
 *   FETCH".
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/request_stream_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <moq/control_d18.h>
#include <string.h>

#define UNKNOWN_ALIAS 99u

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static int count_kind(moq_session_t *s, moq_event_kind_t kind, uint64_t handle)
{
    moq_event_t ev;
    int n = 0;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == kind &&
            (handle == 0 || ev.u.fetch_cancelled.fetch._opaque == handle)) n++;
        moq_event_cleanup(&ev);
    }
    return n;
}

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *sv;
    moq_version_t version;
    moq_fetch_t sfh;          /* the server's handle */
    moq_stream_ref_t sref;    /* the server's view of the request stream */
} fixture_t;

static bool setup(fixture_t *f, moq_version_t version)
{
    memset(f, 0, sizeof(*f));
    f->version = version;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = version;
    cfg.client_send_request_capacity = true; cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true; cfg.server_initial_request_capacity = 32;
    if (moq_simpair_create(&cfg, &f->sp) < 0) return false;
    if (moq_simpair_start(f->sp) < 0) return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(f->sp);
    f->sv = moq_simpair_server(f->sp);
    drain_events(c); drain_events(f->sv);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_fetch_cfg_t fc; moq_fetch_cfg_init(&fc);
    fc.track_namespace = (moq_namespace_t){ parts, 1 };
    fc.track_name = MOQ_BYTES_LITERAL("v");
    fc.start_group = 0; fc.start_object = 0; fc.end_group = 10; fc.end_object = 0;
    moq_fetch_t ch;
    if (moq_session_fetch(c, &fc, 1, &ch) != MOQ_OK) return false;
    moq_simpair_run_until_quiescent(f->sp, 16, NULL);
    moq_event_t ev;
    bool got = false;
    while (moq_session_poll_events(f->sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_FETCH_REQUEST) { f->sfh = ev.u.fetch_request.fetch; got = true; }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    int slot = fetch_resolve_handle(f->sv, f->sfh);
    if (slot < 0) return false;
    f->sref = f->sv->fetches[slot].rs.ref;
    return f->sref._v != 0;
}

/* Feed the server a REQUEST_UPDATE on the fetch's request stream: a priority,
 * an unknown token alias (refused), or, on draft-18, one subscription-only
 * parameter. */
enum upd_kind { UPD_PRIORITY, UPD_BAD_TOKEN, UPD_FORWARD, UPD_FILTER,
                UPD_NEW_GROUP, UPD_DELIVERY_TIMEOUT };

static moq_result_t feed_update(fixture_t *f, moq_stream_ref_t ref,
                                uint64_t request_id, enum upd_kind k,
                                uint64_t request_type_d22)
{
    uint8_t buf[128];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    if (f->version == MOQ_VERSION_DRAFT_22) {
        moq_d22_message_t m;
        memset(&m, 0, sizeof(m));
        m.type = MOQ_D22_REQUEST_UPDATE;
        m.request_id = request_id;
        moq_d22_param_t p;
        memset(&p, 0, sizeof(p));
        if (k == UPD_PRIORITY) { p.type = 0x20; p.value.integer = 3; }
        else if (k == UPD_BAD_TOKEN) {
            p.type = 0x03; p.value.token.alias_type = 2; p.value.token.alias = UNKNOWN_ALIAS;
        } else return MOQ_ERR_INVAL;
        moq_d22_message_context_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.request_type = request_type_d22;
        ctx.update_from_subscriber = true;
        moq_result_t rc = moq_d22_encode_message(&w, &m, &p, 1, &ctx);
        if (rc < 0) return rc;
    } else {
        moq_d18_msg_params_t p;
        memset(&p, 0, sizeof(p));
        switch (k) {
        case UPD_PRIORITY: p.has_subscriber_priority = true; p.subscriber_priority = 3; break;
        case UPD_BAD_TOKEN:
            p.auth_token_count = 1;
            p.auth_tokens[0].alias_type = 2; p.auth_tokens[0].alias = UNKNOWN_ALIAS; break;
        case UPD_FORWARD: p.has_forward = true; p.forward = 0; break;
        case UPD_FILTER: p.has_filter = true; p.filter_type = 2; break;
        case UPD_NEW_GROUP: p.has_new_group_request = true; p.new_group_request = 1; break;
        case UPD_DELIVERY_TIMEOUT:
            p.has_object_delivery_timeout = true; p.object_delivery_timeout_ms = 500; break;
        }
        moq_result_t rc = moq_d18_encode_request_update(&w, request_id, &p);
        if (rc < 0) return rc;
    }
    return moq_session_on_bidi_stream_bytes(f->sv, ref, buf, moq_buf_writer_offset(&w),
                                            false, 9);
}

/* What the server queued: the message types on the request stream in order
 * (and the REQUEST_ERROR code), whether that stream was finished, the fetch
 * data streams opened and reset. */
typedef struct {
    uint64_t msgs[8];
    size_t   n;
    uint64_t error_code;
    bool     fin;
    int      headers;
    int      resets;
    uint64_t data_ref;
    uint64_t reset_ref;
} out_t;

static void collect(fixture_t *f, out_t *o)
{
    memset(o, 0, sizeof(*o));
    o->error_code = UINT64_MAX;
    const moq_request_codec_t *codec = f->sv->profile->request_codec;
    moq_action_t a;
    while (moq_session_poll_actions(f->sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM &&
            a.u.send_bidi_stream.stream_ref._v == f->sref._v) {
            const uint8_t *p = a.u.send_bidi_stream.data;
            size_t len = a.u.send_bidi_stream.len;
            while (len > 0 && o->n < 8) {
                moq_request_envelope_t env;
                size_t off = 0;
                if (codec->decode_envelope(p, len, &env, &off) != MOQ_OK) break;
                o->msgs[o->n++] = env.msg_type;
                if (env.msg_type == codec->type.request_error) {
                    moq_bytes_t parts[MOQ_DECODED_MAX_NAMESPACE_PARTS];
                    moq_request_error_msg_t er;
                    moq_decoded_redirect_t rd;
                    if (codec->decode_request_error(env.payload, env.payload_len, parts,
                            MOQ_DECODED_MAX_NAMESPACE_PARTS, &er, &rd) == MOQ_OK)
                        o->error_code = er.error_code;
                }
                p += off; len -= off;
            }
            if (a.u.send_bidi_stream.fin) o->fin = true;
        } else if (a.kind == MOQ_ACTION_CLOSE_BIDI_STREAM &&
                   a.u.close_bidi_stream.stream_ref._v == f->sref._v) {
            o->fin = true;
        } else if (a.kind == MOQ_ACTION_SEND_DATA && a.u.send_data.header_len > 0) {
            o->headers++;
            o->data_ref = a.u.send_data.stream_ref._v;
        } else if (a.kind == MOQ_ACTION_RESET_DATA) {
            o->resets++;
            o->reset_ref = a.u.reset_data.stream_ref._v;
        }
        moq_action_cleanup(&a);
    }
}

static moq_result_t accept(fixture_t *f)
{
    moq_accept_fetch_cfg_t ac; moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 10; ac.end_object = 0;
    return moq_session_accept_fetch(f->sv, f->sfh, &ac, 10);
}

/* The fetch is gone: its handle is stale and the fetcher's later FIN on the
 * request stream is absorbed. */
static int check_retired(fixture_t *f)
{
    int failures = 0;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_end_fetch(f->sv, f->sfh, 11),
                          (int)MOQ_ERR_STALE_HANDLE);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(f->sv, f->sref, NULL, 0,
                                                                true, 12), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f->sv), (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

static uint64_t fetch_type(moq_version_t v) { (void)v; return MOQ_D22_FETCH; }

/* An update before the answer is acknowledged after FETCH_OK. */
static int test_pending_update_answered_after_fetch_ok(moq_version_t v)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, v));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    const moq_request_codec_t *codec = f.sv->profile->request_codec;
    out_t o;
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(&f, f.sref, 2, UPD_PRIORITY, fetch_type(v)), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
    /* Nothing precedes FETCH_OK on the stream. */
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 0);
    /* The priority is in effect for the data stream to come. */
    int slot = fetch_resolve_handle(f.sv, f.sfh);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot >= 0) MOQ_TEST_CHECK_EQ_INT((int)f.sv->fetches[slot].subscriber_priority, 3);
    MOQ_TEST_CHECK_EQ_INT((int)accept(&f), (int)MOQ_OK);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 2);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[0], codec->type.fetch_ok);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[1], codec->type.request_ok);
    MOQ_TEST_CHECK(!o.fin);
    MOQ_TEST_CHECK_EQ_INT(o.headers, 1);
    MOQ_TEST_CHECK_EQ_INT(o.resets, 0);
    /* The fetch is served as usual. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_end_fetch(f.sv, f.sfh, 11), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT(count_kind(f.sv, MOQ_EVENT_FETCH_CANCELLED, 0), 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
    moq_simpair_destroy(f.sp);
    return failures;
}

/* A refused update before the answer: FETCH_OK, then the refusal ending the
 * request stream, the data stream reset, the fetch retired with an event. */
static int test_pending_refused_update_ends_fetch(moq_version_t v)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, v));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    const moq_request_codec_t *codec = f.sv->profile->request_codec;
    out_t o;
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(&f, f.sref, 2, UPD_BAD_TOKEN, fetch_type(v)), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 0);
    MOQ_TEST_CHECK_EQ_INT(count_kind(f.sv, MOQ_EVENT_FETCH_CANCELLED, 0), 0);
    MOQ_TEST_CHECK_EQ_INT((int)accept(&f), (int)MOQ_OK);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 2);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[0], codec->type.fetch_ok);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[1], codec->type.request_error);
    MOQ_TEST_CHECK_EQ_U64(o.error_code, 0x17);
    MOQ_TEST_CHECK(o.fin);
    MOQ_TEST_CHECK_EQ_INT(o.headers, 1);
    MOQ_TEST_CHECK_EQ_INT(o.resets, 1);
    MOQ_TEST_CHECK_EQ_U64(o.reset_ref, o.data_ref);
    MOQ_TEST_CHECK_EQ_INT(count_kind(f.sv, MOQ_EVENT_FETCH_CANCELLED, f.sfh._opaque), 1);
    failures += check_retired(&f);
    moq_simpair_destroy(f.sp);
    return failures;
}

/* A refused update on an accepted fetch ends it the same way. */
static int test_accepted_refused_update_ends_fetch(moq_version_t v)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, v));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    const moq_request_codec_t *codec = f.sv->profile->request_codec;
    out_t o;
    MOQ_TEST_CHECK_EQ_INT((int)accept(&f), (int)MOQ_OK);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_INT(o.headers, 1);
    uint64_t data_ref = o.data_ref;
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(&f, f.sref, 2, UPD_BAD_TOKEN, fetch_type(v)), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 1);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[0], codec->type.request_error);
    MOQ_TEST_CHECK_EQ_U64(o.error_code, 0x17);
    MOQ_TEST_CHECK(o.fin);
    MOQ_TEST_CHECK_EQ_INT(o.resets, 1);
    MOQ_TEST_CHECK_EQ_U64(o.reset_ref, data_ref);
    MOQ_TEST_CHECK_EQ_INT(count_kind(f.sv, MOQ_EVENT_FETCH_CANCELLED, f.sfh._opaque), 1);
    /* No header-less data stream can follow: the handle is retired. */
    uint8_t d[] = { 1 };
    moq_rcbuf_t *pay = NULL;
    moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &pay);
    moq_fetch_object_cfg_t oc; moq_fetch_object_cfg_init(&oc);
    oc.group_id = 0; oc.object_id = 0; oc.payload = pay;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_fetch_object(f.sv, f.sfh, &oc, 11),
                          (int)MOQ_ERR_STALE_HANDLE);
    moq_rcbuf_decref(pay);
    failures += check_retired(&f);
    moq_simpair_destroy(f.sp);
    return failures;
}

/* A rejected fetch is answered by its REQUEST_ERROR alone; an update it
 * carried ends with it. */
static int test_rejected_fetch_drops_owed_answers(moq_version_t v)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, v));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    const moq_request_codec_t *codec = f.sv->profile->request_codec;
    out_t o;
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(&f, f.sref, 2, UPD_PRIORITY, fetch_type(v)), (int)MOQ_OK);
    moq_reject_fetch_cfg_t rc; moq_reject_fetch_cfg_init(&rc);
    rc.error_code = MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reject_fetch(f.sv, f.sfh, &rc, 10), (int)MOQ_OK);
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_SIZE(o.n, 1);
    MOQ_TEST_CHECK_EQ_U64(o.msgs[0], codec->type.request_error);
    MOQ_TEST_CHECK_EQ_U64(o.error_code, MOQ_REQUEST_ERROR_DOES_NOT_EXIST);
    MOQ_TEST_CHECK(o.fin);
    MOQ_TEST_CHECK_EQ_INT(o.headers, 0);
    failures += check_retired(&f);
    moq_simpair_destroy(f.sp);
    return failures;
}

/* Owed answers count against MAX_REQUEST_UPDATES (draft-22 advertises 1);
 * draft-18 advertises no bound and answers each in order. */
static int test_owed_answers_bound(moq_version_t v)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, v));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    const moq_request_codec_t *codec = f.sv->profile->request_codec;
    out_t o;
    collect(&f, &o);
    MOQ_TEST_CHECK_EQ_INT((int)feed_update(&f, f.sref, 2, UPD_PRIORITY, fetch_type(v)), (int)MOQ_OK);
    (void)feed_update(&f, f.sref, 4, UPD_PRIORITY, fetch_type(v));
    if (v == MOQ_VERSION_DRAFT_22) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_CLOSED);
        moq_event_t ev;
        bool closed = false;
        while (moq_session_poll_events(f.sv, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SESSION_CLOSED) {
                closed = true;
                MOQ_TEST_CHECK_EQ_U64(ev.u.closed.code, 0x1b);
            }
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK(closed);
    } else {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
        MOQ_TEST_CHECK_EQ_INT((int)accept(&f), (int)MOQ_OK);
        collect(&f, &o);
        MOQ_TEST_CHECK_EQ_SIZE(o.n, 3);
        MOQ_TEST_CHECK_EQ_U64(o.msgs[0], codec->type.fetch_ok);
        MOQ_TEST_CHECK_EQ_U64(o.msgs[1], codec->type.request_ok);
        MOQ_TEST_CHECK_EQ_U64(o.msgs[2], codec->type.request_ok);
        MOQ_TEST_CHECK(!o.fin);
    }
    moq_simpair_destroy(f.sp);
    return failures;
}

/* Draft-18: subscription-only parameters on a FETCH update close the
 * session; the priority, a token and a delivery timeout (whose definitions
 * name REQUEST_UPDATE without a family) do not. */
static int test_d18_fetch_update_scope(enum upd_kind k, bool closes)
{
    int failures = 0;
    fixture_t f;
    MOQ_TEST_CHECK(setup(&f, MOQ_VERSION_DRAFT_18));
    if (!f.sv) { moq_simpair_destroy(f.sp); return failures; }
    MOQ_TEST_CHECK_EQ_INT((int)accept(&f), (int)MOQ_OK);
    out_t o;
    collect(&f, &o);
    (void)feed_update(&f, f.sref, 2, k, 0);
    if (closes) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_CLOSED);
        moq_event_t ev;
        bool closed = false;
        while (moq_session_poll_events(f.sv, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SESSION_CLOSED) {
                closed = true;
                MOQ_TEST_CHECK_EQ_U64(ev.u.closed.code, 0x3);
            }
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK(closed);
    } else {
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(f.sv), (int)MOQ_SESS_ESTABLISHED);
        collect(&f, &o);
        MOQ_TEST_CHECK_EQ_SIZE(o.n, 1);
        MOQ_TEST_CHECK_EQ_U64(o.msgs[0], f.sv->profile->request_codec->type.request_ok);
    }
    moq_simpair_destroy(f.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        failures += test_pending_update_answered_after_fetch_ok(versions[i]);
        failures += test_pending_refused_update_ends_fetch(versions[i]);
        failures += test_accepted_refused_update_ends_fetch(versions[i]);
        failures += test_rejected_fetch_drops_owed_answers(versions[i]);
        failures += test_owed_answers_bound(versions[i]);
    }
    failures += test_d18_fetch_update_scope(UPD_FORWARD, true);
    failures += test_d18_fetch_update_scope(UPD_FILTER, true);
    failures += test_d18_fetch_update_scope(UPD_NEW_GROUP, true);
    failures += test_d18_fetch_update_scope(UPD_PRIORITY, false);
    failures += test_d18_fetch_update_scope(UPD_DELIVERY_TIMEOUT, false);
    MOQ_TEST_PASS("fetch_request_update");
    return failures != 0;
}
