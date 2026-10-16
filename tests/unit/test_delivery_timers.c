/*
 * Publisher-side delivery timeouts under fake time, on the draft-18 and
 * draft-22 profiles.
 *
 * OBJECT_DELIVERY_TIMEOUT: the session retains the instant each object's
 * header was provided and refuses to hand an expired object to the transport:
 * the queued data for that subgroup is dropped and the stream is reset with
 * DELIVERY_TIMEOUT; an expired datagram is dropped. The check runs both when
 * the timer fires and when the adapter polls. Draft-22 starts the clock when
 * the header is provided; draft-18 starts it at the object's first payload
 * byte. On draft-22 the first object's OBJECT_DELIVERY_TIMEOUT property
 * overrides the track-level value for its subgroup.
 *
 * SUBGROUP_DELIVERY_TIMEOUT: on both profiles the timer starts when the
 * subgroup's FIN is queued and resets the stream unless the transport reports
 * it committed first. Only draft-22 honours the first object's override.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "test_deadline_support.h"
#include <string.h>

static int failures;

static const uint64_t MS = 1000;

typedef struct fx {
    moq_simpair_t      *sp;
    moq_session_t      *c;      /* subscriber */
    moq_session_t      *sv;     /* publisher: owns the timers */
    moq_subscription_t  ssub;
} fx_t;

/* Track Properties with the given timeouts (delta-typed key-value pairs,
 * OBJECT_DELIVERY_TIMEOUT 0x02 then SUBGROUP_DELIVERY_TIMEOUT 0x06). */
static size_t timeouts_kvp(uint8_t *buf, size_t cap, bool has_obj, uint64_t obj_ms,
                           bool has_sg, uint64_t sg_ms)
{
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    uint64_t prev = 0;
    if (has_obj) { moq_buf_write_vi64(&w, 0x02 - prev); moq_buf_write_vi64(&w, obj_ms); prev = 0x02; }
    if (has_sg)  { moq_buf_write_vi64(&w, 0x06 - prev); moq_buf_write_vi64(&w, sg_ms); }
    return moq_buf_writer_offset(&w);
}

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static void drain_actions(moq_session_t *s)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) moq_action_cleanup(&a);
}

static bool fx_open(fx_t *fx, moq_version_t version, const uint8_t *tp, size_t tp_len)
{
    memset(fx, 0, sizeof(*fx));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = version;
    if (moq_simpair_create(&scfg, &fx->sp) < 0) return false;
    fx->c = moq_simpair_client(fx->sp);
    fx->sv = moq_simpair_server(fx->sp);
    if (moq_simpair_start(fx->sp) < 0) return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);

    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("live") };
    cfg.track_namespace = (moq_namespace_t){ parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    moq_subscription_t csub;
    if (moq_session_subscribe(fx->c, &cfg, moq_simpair_now_us(fx->sp), &csub) < 0) return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    moq_event_t ev; bool got = false;
    while (moq_session_poll_events(fx->sv, &ev, 1) > 0) {
        if (!got && ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) { fx->ssub = ev.u.subscribe_request.sub; got = true; }
        moq_event_cleanup(&ev);
    }
    if (!got) return false;
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    acc.track_properties = (moq_bytes_t){ tp, tp_len };
    if (moq_session_accept_subscribe(fx->sv, fx->ssub, &acc, moq_simpair_now_us(fx->sp)) < 0) return false;
    moq_simpair_run_until_quiescent(fx->sp, 16, NULL);
    drain_events(fx->c); drain_events(fx->sv);
    drain_actions(fx->sv);
    return moq_session_state(fx->sv) == MOQ_SESS_ESTABLISHED;
}

static void fx_close(fx_t *fx) { if (fx->sp) moq_simpair_destroy(fx->sp); memset(fx, 0, sizeof(*fx)); }

static moq_rcbuf_t *payload(const char *text)
{
    moq_rcbuf_t *b = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)text, strlen(text), &b);
    return b;
}

/* Poll everything queued; count the SEND_DATA actions carrying a payload on
 * `ref`, the SEND_DATAGRAMs, and the RESET_DATA(DELIVERY_TIMEOUT)s on `ref`. */
typedef struct tally { int data_payloads; int datagrams; int timeout_resets; int other_resets; int fins; } tally_t;

static tally_t poll_tally(moq_session_t *s, moq_stream_ref_t ref)
{
    tally_t t; memset(&t, 0, sizeof(t));
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA && a.u.send_data.stream_ref._v == ref._v) {
            if (a.u.send_data.payload) t.data_payloads++;
            if (a.u.send_data.fin) t.fins++;
        } else if (a.kind == MOQ_ACTION_SEND_DATAGRAM) {
            t.datagrams++;
        } else if (a.kind == MOQ_ACTION_RESET_DATA && a.u.reset_data.stream_ref._v == ref._v) {
            if (a.u.reset_data.error_code == MOQ_RESET_DELIVERY_TIMEOUT) t.timeout_resets++;
            else t.other_resets++;
        }
        moq_action_cleanup(&a);
    }
    return t;
}

/* The stream ref of the subgroup's header action (polls it away). */
static moq_stream_ref_t take_header_ref(moq_session_t *s)
{
    moq_stream_ref_t ref = moq_stream_ref_from_u64(0);
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_SEND_DATA && a.u.send_data.header_len > 0 && ref._v == 0)
            ref = a.u.send_data.stream_ref;
        moq_action_cleanup(&a);
    }
    return ref;
}

static moq_subgroup_handle_t open_sg(fx_t *fx, uint64_t group, bool props, uint64_t now)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.publisher_priority = 128;
    cfg.object_properties = props;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    moq_session_open_subgroup(fx->sv, fx->ssub, &cfg, now, &sg);
    return sg;
}

/* -- OBJECT_DELIVERY_TIMEOUT ----------------------------------------------- */

static void test_object_timeout_tick(moq_version_t v)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }

    /* Not yet expired: the object is handed over, no reset. */
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    moq_stream_ref_t ref = take_header_ref(fx.sv);
    moq_rcbuf_t *p = payload("one");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, sg, 0, p, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), 110 * MS);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 110 * MS - 1), (int)MOQ_OK);
    tally_t t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.data_payloads, 1);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 0);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);

    /* Expired before the handover: dropped, stream reset with DELIVERY_TIMEOUT. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, sg, 1, p, 200 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), 300 * MS);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 300 * MS), (int)MOQ_OK);
    t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.data_payloads, 0);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 1);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    /* The subgroup is gone for further writes. */
    MOQ_TEST_CHECK(moq_session_write_object(fx.sv, sg, 2, p, 301 * MS) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.sv), (int)MOQ_SESS_ESTABLISHED);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* The handover check needs no timer: an advancing call that moves the clock
 * past the deadline makes the next poll drop the object and reset. */
static void test_object_timeout_at_poll(moq_version_t v)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t a = open_sg(&fx, 0, false, 0);
    moq_subgroup_handle_t b = open_sg(&fx, 1, false, 0);
    drain_actions(fx.sv);
    moq_rcbuf_t *p = payload("x");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, a, 0, p, 10 * MS), (int)MOQ_OK);
    /* Later work on another subgroup carries the clock to 200 ms. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, b, 0, p, 200 * MS), (int)MOQ_OK);
    moq_action_t acts[8]; size_t na = 0, payloads = 0;
    int resets = 0;
    while ((na = moq_session_poll_actions(fx.sv, acts, 8)) > 0) {
        for (size_t i = 0; i < na; i++) {
            if (acts[i].kind == MOQ_ACTION_SEND_DATA && acts[i].u.send_data.payload) payloads++;
            if (acts[i].kind == MOQ_ACTION_RESET_DATA &&
                acts[i].u.reset_data.error_code == MOQ_RESET_DELIVERY_TIMEOUT) resets++;
            moq_action_cleanup(&acts[i]);
        }
    }
    /* Exactly one object survived and one stream was reset; the writes
     * below show it is a's stream that is gone and b's that lives. */
    MOQ_TEST_CHECK_EQ_SIZE(payloads, 1);
    MOQ_TEST_CHECK_EQ_INT(resets, 1);
    MOQ_TEST_CHECK(moq_session_write_object(fx.sv, a, 1, p, 201 * MS) < 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, b, 1, p, 201 * MS), (int)MOQ_OK);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* Where the clock starts for a streamed object: draft-22 at the header,
 * draft-18 at the first payload byte. */
static void test_object_clock_start(moq_version_t v, bool at_header)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    drain_actions(fx.sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_begin_object(fx.sv, sg, 0, 2, 10 * MS), (int)MOQ_OK);
    moq_rcbuf_t *p = payload("hi");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_data(fx.sv, sg, p, 50 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_end_object(fx.sv, sg, 50 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), (at_header ? 110 : 150) * MS);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* Datagrams: the smaller of the two timeouts, dropped on expiry. */
static void test_datagram_timeout(moq_version_t v)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, true, 50);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_rcbuf_t *p = payload("dg");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_send_object_datagram(fx.sv, fx.ssub, 0, 0, 128, false,
                                                                 p, NULL, 0, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), 60 * MS);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 60 * MS - 1), (int)MOQ_OK);
    tally_t t = poll_tally(fx.sv, moq_stream_ref_from_u64(0));
    MOQ_TEST_CHECK_EQ_INT(t.datagrams, 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_send_object_datagram(fx.sv, fx.ssub, 0, 1, 128, false,
                                                                 p, NULL, 0, 100 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 150 * MS), (int)MOQ_OK);
    t = poll_tally(fx.sv, moq_stream_ref_from_u64(0));
    MOQ_TEST_CHECK_EQ_INT(t.datagrams, 0);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.sv), (int)MOQ_SESS_ESTABLISHED);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* The first object's OBJECT_DELIVERY_TIMEOUT property overrides the track
 * value on draft-22 (20 ms instead of 100); a later object's is ignored. */
static void test_object_first_object_override(moq_version_t v, bool honoured)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, true, 0);
    drain_actions(fx.sv);
    uint8_t ovr[8]; size_t on = timeouts_kvp(ovr, sizeof(ovr), true, 20, false, 0);
    moq_rcbuf_t *props = NULL;
    moq_rcbuf_create(moq_alloc_default(), ovr, on, &props);
    moq_rcbuf_t *p = payload("first");
    moq_object_cfg_t oc; moq_object_cfg_init(&oc);
    oc.object_id = 0; oc.payload = p; oc.properties = props;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(fx.sv, sg, &oc, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), (honoured ? 30 : 110) * MS);
    drain_actions(fx.sv);
    /* The override applies to the whole subgroup; a later object's property is ignored. */
    uint8_t later[8]; size_t ln = timeouts_kvp(later, sizeof(later), true, 5, false, 0);
    moq_rcbuf_t *props2 = NULL;
    moq_rcbuf_create(moq_alloc_default(), later, ln, &props2);
    oc.object_id = 1; oc.properties = props2;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(fx.sv, sg, &oc, 40 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), (honoured ? 60 : 140) * MS);
    moq_rcbuf_decref(props); moq_rcbuf_decref(props2); moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* -- SUBGROUP_DELIVERY_TIMEOUT --------------------------------------------- */

static void test_subgroup_timer_at_fin(void)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), false, 0, true, 100);
    fx_t fx;
    if (!fx_open(&fx, MOQ_VERSION_DRAFT_22, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    /* Not armed by opening the stream. */
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    moq_stream_ref_t ref = take_header_ref(fx.sv);
    moq_rcbuf_t *p = payload("one");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, sg, 0, p, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), UINT64_MAX);
    /* Armed by the FIN. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.sv, sg, 50 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), 150 * MS);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), 150 * MS);
    tally_t t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.data_payloads, 1);
    MOQ_TEST_CHECK_EQ_INT(t.fins, 1);
    /* The FIN has been handed over and the queue drained; the entry survives
     * the sweep while its timer runs, and still reports its deadline. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 150 * MS - 1), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), 150 * MS);
    t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 150 * MS), (int)MOQ_OK);
    t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 1);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.sv), (int)MOQ_SESS_ESTABLISHED);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

static void test_subgroup_timer_cleared_by_commit(void)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), false, 0, true, 100);
    fx_t fx;
    if (!fx_open(&fx, MOQ_VERSION_DRAFT_22, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    moq_stream_ref_t ref = take_header_ref(fx.sv);
    moq_rcbuf_t *p = payload("one");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, sg, 0, p, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.sv, sg, 50 * MS), (int)MOQ_OK);
    drain_actions(fx.sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 100 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_committed(fx.sv, ref, 120 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 150 * MS), (int)MOQ_OK);
    tally_t t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 0);
    /* A report for a stream nobody awaits is harmless. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_committed(fx.sv, moq_stream_ref_from_u64(0x9999), 160 * MS),
                          (int)MOQ_OK);
    moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* The first object's SUBGROUP_DELIVERY_TIMEOUT property (30 ms) overrides
 * the track value (100 ms) for the FIN-started timer. */
static void test_subgroup_timer_override(void)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), false, 0, true, 100);
    fx_t fx;
    if (!fx_open(&fx, MOQ_VERSION_DRAFT_22, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, true, 0);
    drain_actions(fx.sv);
    uint8_t ovr[8]; size_t on = timeouts_kvp(ovr, sizeof(ovr), false, 0, true, 30);
    moq_rcbuf_t *props = NULL;
    moq_rcbuf_create(moq_alloc_default(), ovr, on, &props);
    moq_rcbuf_t *p = payload("first");
    moq_object_cfg_t oc; moq_object_cfg_init(&oc);
    oc.object_id = 0; oc.payload = p; oc.properties = props;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object_ex(fx.sv, sg, &oc, 10 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.sv, sg, 50 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), 80 * MS);
    moq_rcbuf_decref(props); moq_rcbuf_decref(p);
    fx_close(&fx);
}

/* Draft-18 section 8 starts the timer at FIN, not while still publishing. */
static void test_subgroup_timer_d18_at_fin(void)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), false, 0, true, 100);
    fx_t fx;
    if (!fx_open(&fx, MOQ_VERSION_DRAFT_18, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), UINT64_MAX);
    moq_stream_ref_t ref = take_header_ref(fx.sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.sv, sg, 50 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(test_get_sg_deadline(fx.sv, sg), 150 * MS);
    drain_actions(fx.sv);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_committed(fx.sv, ref, 60 * MS), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(fx.sv), UINT64_MAX);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 200 * MS), (int)MOQ_OK);
    tally_t t = poll_tally(fx.sv, ref);
    MOQ_TEST_CHECK_EQ_INT(t.timeout_resets, 0);
    fx_close(&fx);
}

/* The FIN of a closed subgroup and then PUBLISH_DONE are queued; the
 * object before the FIN expires. The reset that replaces the FIN goes out
 * before the PUBLISH_DONE. */
static void test_timeout_reset_precedes_publish_done(moq_version_t v)
{
    uint8_t tp[16]; size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    fx_t fx;
    if (!fx_open(&fx, v, tp, n)) { MOQ_TEST_CHECK(false); fx_close(&fx); return; }
    moq_subgroup_handle_t sg = open_sg(&fx, 0, false, 0);
    moq_stream_ref_t ref = take_header_ref(fx.sv);
    moq_rcbuf_t *p = payload("one");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(fx.sv, sg, 0, p, 10 * MS), (int)MOQ_OK);
    moq_rcbuf_decref(p);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(fx.sv, sg, 10 * MS), (int)MOQ_OK);
    moq_done_subscribe_cfg_t dc; moq_done_subscribe_cfg_init(&dc);
    dc.status_code = 0x2;
    dc.stream_count = 1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_done_subscribe(fx.sv, fx.ssub, &dc, 20 * MS),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(fx.sv, 200 * MS), (int)MOQ_OK);
    int idx = 0, reset_at = -1, done_at = -1, payloads = 0;
    moq_action_t a;
    while (moq_session_poll_actions(fx.sv, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_RESET_DATA && a.u.reset_data.stream_ref._v == ref._v &&
            a.u.reset_data.error_code == MOQ_RESET_DELIVERY_TIMEOUT && reset_at < 0)
            reset_at = idx;
        if (a.kind == MOQ_ACTION_SEND_DATA && a.u.send_data.stream_ref._v == ref._v &&
            a.u.send_data.payload)
            payloads++;
        if (a.kind == MOQ_ACTION_SEND_BIDI_STREAM && a.u.send_bidi_stream.fin && done_at < 0)
            done_at = idx;
        idx++;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT(payloads, 0);
    MOQ_TEST_CHECK(reset_at >= 0);
    MOQ_TEST_CHECK(done_at >= 0);
    MOQ_TEST_CHECK(reset_at < done_at);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(fx.sv), (int)MOQ_SESS_ESTABLISHED);
    fx_close(&fx);
}

int main(void)
{
    const moq_version_t both[2] = { MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 2; i++) {
        test_object_timeout_tick(both[i]);
        test_object_timeout_at_poll(both[i]);
        test_datagram_timeout(both[i]);
        test_timeout_reset_precedes_publish_done(both[i]);
    }
    test_object_clock_start(MOQ_VERSION_DRAFT_22, true);
    test_object_clock_start(MOQ_VERSION_DRAFT_18, false);
    test_object_first_object_override(MOQ_VERSION_DRAFT_22, true);
    test_object_first_object_override(MOQ_VERSION_DRAFT_18, false);
    test_subgroup_timer_at_fin();
    test_subgroup_timer_cleared_by_commit();
    test_subgroup_timer_override();
    test_subgroup_timer_d18_at_fin();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    MOQ_TEST_PASS("delivery_timers");
    return 0;
}
