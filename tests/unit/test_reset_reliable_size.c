/*
 * The reliable size every data-stream reset carries.
 *
 * A reset the session originates on a data stream asks the transport to keep
 * the stream header: RESET_DATA's reliable_size is the length of the header
 * as encoded on that very stream (SUBGROUP_HEADER or FETCH_HEADER), read
 * through detail_size. Pinned on draft-16, draft-18 and draft-22, whose text
 * all say "the reliable_size SHOULD include the stream header", for every
 * reset path:
 *
 *   subgroups  the application's reset, the answer to STOP_SENDING, a
 *              draft-16 UNSUBSCRIBE, the peer abandoning a PUBLISH, an object
 *              dropped past its OBJECT_DELIVERY_TIMEOUT, and the
 *              SUBGROUP_DELIVERY_TIMEOUT (both draft-18 and -22 only);
 *   fetches    the publisher's abort, FETCH_CANCEL / a torn-down request
 *              stream, a REQUEST_UPDATE on the fetch refused for its token
 *              (draft-22);
 *   fills      (draft-22) a fill failed on the wire (header then reset), an
 *              aborted open fill, a fill reset with its parent, STOP_SENDING.
 *
 * Two subgroups whose group ids encode at different widths show the value
 * follows the encoding on each stream rather than a constant. Each check
 * also requires the reset to follow its header on the same stream, so the
 * reliable size never names bytes the session did not queue.
 */
#include <moq/moq.h>
#include <moq/control.h>
#include "test_support.h"
#include "test_alloc_support.h"
#include "test_binding_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <stddef.h>
#include <string.h>

static int failures;

/* -- Ledger of what one side sent ------------------------------------------ */

#define LEDGER_MAX 64

typedef struct ledger {
    struct { uint64_t ref; uint8_t len; } hdr[LEDGER_MAX];
    size_t nhdr;
    struct { uint64_t ref, code, reliable; uint32_t detail; bool after_hdr; } rst[LEDGER_MAX];
    size_t nrst;
} ledger_t;

static int hdr_index(const ledger_t *L, uint64_t ref)
{
    for (size_t i = 0; i < L->nhdr; i++)
        if (L->hdr[i].ref == ref) return (int)i;
    return -1;
}

/* The first SEND_DATA on a stream carries exactly its stream header. */
static void ledger_note(ledger_t *L, const moq_action_t *a)
{
    if (a->kind == MOQ_ACTION_SEND_DATA && a->u.send_data.header_len > 0 &&
        hdr_index(L, a->u.send_data.stream_ref._v) < 0 && L->nhdr < LEDGER_MAX) {
        L->hdr[L->nhdr].ref = a->u.send_data.stream_ref._v;
        L->hdr[L->nhdr].len = a->u.send_data.header_len;
        L->nhdr++;
    } else if (a->kind == MOQ_ACTION_RESET_DATA && L->nrst < LEDGER_MAX) {
        bool covers = a->detail_size >= offsetof(moq_reset_data_action_t, reliable_size) +
                                        sizeof(a->u.reset_data.reliable_size);
        L->rst[L->nrst].ref = a->u.reset_data.stream_ref._v;
        L->rst[L->nrst].code = a->u.reset_data.error_code;
        L->rst[L->nrst].reliable = covers ? a->u.reset_data.reliable_size : 0;
        L->rst[L->nrst].detail = a->detail_size;
        L->rst[L->nrst].after_hdr = hdr_index(L, a->u.reset_data.stream_ref._v) >= 0;
        L->nrst++;
    }
}

/* Every reset in the ledger carries its own stream's header length; returns
 * how many there were. */
static size_t check_resets(const ledger_t *L, const char *what, moq_version_t v)
{
    for (size_t i = 0; i < L->nrst; i++) {
        int h = hdr_index(L, L->rst[i].ref);
        if (!L->rst[i].after_hdr || h < 0) {
            fprintf(stderr, "  %s v%u: reset on ref %llu precedes any header\n",
                    what, (unsigned)v, (unsigned long long)L->rst[i].ref);
            failures++;
            continue;
        }
        if (L->rst[i].reliable != L->hdr[h].len) {
            fprintf(stderr, "  %s v%u: reset reliable_size %llu, header %u bytes\n",
                    what, (unsigned)v, (unsigned long long)L->rst[i].reliable,
                    (unsigned)L->hdr[h].len);
            failures++;
        }
    }
    return L->nrst;
}

static uint64_t reliable_of(const ledger_t *L, uint64_t ref)
{
    for (size_t i = 0; i < L->nrst; i++)
        if (L->rst[i].ref == ref) return L->rst[i].reliable;
    return UINT64_MAX;
}

static uint8_t hdr_of(const ledger_t *L, uint64_t ref)
{
    int h = hdr_index(L, ref);
    return h < 0 ? 0 : L->hdr[h].len;
}

/* -- Hand-pumped pair ------------------------------------------------------- */

#define DATA_REF_OFFSET UINT64_C(0x100000)

typedef struct env {
    moq_version_t  v;
    test_alloc_state_t allocations;
    moq_alloc_t    alloc;
    moq_session_t *c;
    moq_session_t *sv;
    ledger_t       lc;   /* what the client sent */
    ledger_t       ls;   /* what the server sent */
    unsigned       tracks;
} env_t;

static void deliver(moq_session_t *to, const moq_action_t *a, bool from_server, uint64_t now)
{
    uint64_t off = from_server ? DATA_REF_OFFSET : 0;
    switch (a->kind) {
    case MOQ_ACTION_OPEN_UNI_CONTROL:
        moq_session_on_control_bytes(to, a->u.open_uni_control.data, a->u.open_uni_control.len, now);
        break;
    case MOQ_ACTION_SEND_UNI_CONTROL:
        moq_session_on_control_bytes(to, a->u.send_uni_control.data, a->u.send_uni_control.len, now);
        break;
    case MOQ_ACTION_SEND_CONTROL:
        moq_session_on_control_bytes(to, a->u.send_control.data, a->u.send_control.len, now);
        break;
    case MOQ_ACTION_OPEN_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.open_bidi_stream.stream_ref,
            a->u.open_bidi_stream.data, a->u.open_bidi_stream.len, a->u.open_bidi_stream.fin, now);
        break;
    case MOQ_ACTION_SEND_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.send_bidi_stream.stream_ref,
            a->u.send_bidi_stream.data, a->u.send_bidi_stream.len, a->u.send_bidi_stream.fin, now);
        break;
    case MOQ_ACTION_CLOSE_BIDI_STREAM:
        moq_session_on_bidi_stream_bytes(to, a->u.close_bidi_stream.stream_ref, NULL, 0, true, now);
        break;
    case MOQ_ACTION_ABORT_BIDI_STREAM:
        moq_session_on_bidi_stream_reset(to, a->u.abort_bidi_stream.stream_ref,
            a->u.abort_bidi_stream.error_code, now);
        moq_session_on_bidi_stream_stop(to, a->u.abort_bidi_stream.stream_ref,
            a->u.abort_bidi_stream.error_code, now);
        break;
    case MOQ_ACTION_RESET_BIDI_STREAM:
        moq_session_on_bidi_stream_reset(to, a->u.reset_bidi_stream.stream_ref,
            a->u.reset_bidi_stream.error_code, now);
        break;
    case MOQ_ACTION_STOP_BIDI_STREAM:
        moq_session_on_bidi_stream_stop(to, a->u.stop_bidi_stream.stream_ref,
            a->u.stop_bidi_stream.error_code, now);
        break;
    case MOQ_ACTION_SEND_DATA: {
        moq_stream_ref_t r = moq_stream_ref_from_u64(a->u.send_data.stream_ref._v + off);
        bool hp = a->u.send_data.payload != NULL;
        if (a->u.send_data.header_len > 0)
            moq_session_on_data_bytes(to, r, a->u.send_data.header, a->u.send_data.header_len,
                                      a->u.send_data.fin && !hp, now);
        if (hp)
            moq_session_on_data_bytes(to, r, moq_rcbuf_data(a->u.send_data.payload),
                                      moq_rcbuf_len(a->u.send_data.payload), a->u.send_data.fin, now);
        if (!hp && a->u.send_data.header_len == 0 && a->u.send_data.fin)
            moq_session_on_data_bytes(to, r, NULL, 0, true, now);
        break;
    }
    case MOQ_ACTION_RESET_DATA:
        moq_session_on_data_reset(to, moq_stream_ref_from_u64(a->u.reset_data.stream_ref._v + off),
                                  a->u.reset_data.error_code, now);
        break;
    case MOQ_ACTION_STOP_DATA:
        /* The peer's STOP lands on our own (sender-side) ref. */
        moq_session_on_data_stop(to, moq_stream_ref_from_u64(a->u.stop_data.stream_ref._v -
            (from_server ? 0 : DATA_REF_OFFSET)), a->u.stop_data.error_code, now);
        break;
    default: break;
    }
}

static void pump_side(moq_session_t *from, moq_session_t *to, ledger_t *L, bool from_server,
                      uint64_t now)
{
    moq_action_t a;
    while (moq_session_poll_actions(from, &a, 1) > 0) {
        ledger_note(L, &a);
        deliver(to, &a, from_server, now);
        moq_action_cleanup(&a);
    }
}

static void run(env_t *e, uint64_t now)
{
    for (int i = 0; i < 8; i++) {
        pump_side(e->c, e->sv, &e->lc, false, now);
        pump_side(e->sv, e->c, &e->ls, true, now);
    }
}

/* Poll one side's actions into its ledger without delivering them. */
static void collect(moq_session_t *s, ledger_t *L)
{
    moq_action_t a;
    while (moq_session_poll_actions(s, &a, 1) > 0) {
        ledger_note(L, &a);
        moq_action_cleanup(&a);
    }
}

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == kind) { *out = ev; return true; }
        moq_event_cleanup(&ev);
    }
    return false;
}

static moq_session_t *mk_session(moq_alloc_t *alloc, moq_perspective_t persp,
                                 moq_version_t v, uint32_t max_fills)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), alloc, persp);
    cfg.version = v;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 64;
    cfg.max_fills = max_fills;
    moq_session_t *s = NULL;
    if (moq_session_create(&cfg, 0, &s) < 0) return NULL;
    if (test_bind_if_required(s) < 0) { moq_session_destroy(s); return NULL; }
    /* Bidi-control profiles start the client only; the server reacts. */
    bool start = persp == MOQ_PERSPECTIVE_CLIENT || moq_session_uses_uni_control(s);
    if (start && moq_session_start(s, 0) < 0) { moq_session_destroy(s); return NULL; }
    return s;
}

static bool env_open(env_t *e, moq_version_t v, uint32_t sv_fills)
{
    memset(e, 0, sizeof(*e));
    e->v = v;
    e->alloc = test_allocator(&e->allocations);
    e->c = mk_session(&e->alloc, MOQ_PERSPECTIVE_CLIENT, v, 4);
    e->sv = mk_session(&e->alloc, MOQ_PERSPECTIVE_SERVER, v, sv_fills);
    if (!e->c || !e->sv) return false;
    run(e, 1);
    drain_events(e->c);
    drain_events(e->sv);
    return moq_session_state(e->c) == MOQ_SESS_ESTABLISHED &&
           moq_session_state(e->sv) == MOQ_SESS_ESTABLISHED;
}

static void env_close(env_t *e)
{
    if (e->c) moq_session_destroy(e->c);
    if (e->sv) moq_session_destroy(e->sv);
    MOQ_TEST_CHECK_EQ_U64(e->allocations.balance, 0);
    memset(e, 0, sizeof(*e));
}

static moq_bytes_t ns_parts[] = { { (const uint8_t *)"live", 4 } };

/* SUBSCRIBE (with an optional fill) accepted with Largest (5,2) and the given
 * Track Properties. */
static bool subscribe(env_t *e, const moq_fill_cfg_t *fill, const uint8_t *tp, size_t tp_len,
                      moq_subscription_t *ch, moq_subscription_t *sh)
{
    static const char names[] = "abcdefgh";
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ ns_parts, 1 };
    cfg.track_name = (moq_bytes_t){ (const uint8_t *)&names[e->tracks++ % 8], 1 };
    cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    cfg.fill = fill;
    if (moq_session_subscribe(e->c, &cfg, 2, ch) != MOQ_OK) return false;
    run(e, 3);
    moq_event_t ev;
    if (!take(e->sv, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev)) return false;
    *sh = ev.u.subscribe_request.sub;
    moq_event_cleanup(&ev);
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_largest = true; ac.largest_group = 5; ac.largest_object = 2;
    ac.track_properties = (moq_bytes_t){ tp, tp_len };
    if (moq_session_accept_subscribe(e->sv, *sh, &ac, 4) != MOQ_OK) return false;
    run(e, 5);
    return true;
}

static moq_rcbuf_t *mk_payload(const char *text)
{
    moq_rcbuf_t *b = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)text, strlen(text), &b);
    return b;
}

static moq_subgroup_handle_t open_sg(moq_session_t *s, moq_subscription_t sub, uint64_t group,
                                     uint64_t now)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = group;
    cfg.subgroup_id = 0;
    cfg.publisher_priority = 128;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_subgroup(s, sub, &cfg, now, &sg), (int)MOQ_OK);
    return sg;
}

static void write_obj(moq_session_t *s, moq_subgroup_handle_t sg, uint64_t id, uint64_t now)
{
    moq_rcbuf_t *p = mk_payload("object");
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_write_object(s, sg, id, p, now), (int)MOQ_OK);
    moq_rcbuf_decref(p);
}

/* The server-side ref of the most recently opened data stream. */
static uint64_t last_hdr_ref(const ledger_t *L)
{
    return L->nhdr ? L->hdr[L->nhdr - 1].ref : 0;
}

#define GROUP_WIDE UINT64_C(0x10000000000)   /* an eight-byte varint */

/* -- Subgroups --------------------------------------------------------------- */

/* The application resets two subgroups whose headers encode at different
 * widths: each reset keeps its own header. */
static void test_subgroup_app_reset(moq_version_t v)
{
    env_t e;
    moq_subscription_t ch, sh;
    if (!env_open(&e, v, 0) || !subscribe(&e, NULL, NULL, 0, &ch, &sh)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    moq_subgroup_handle_t a = open_sg(e.sv, sh, 6, 10);
    write_obj(e.sv, a, 0, 10);
    run(&e, 10);
    uint64_t ra = last_hdr_ref(&e.ls);
    moq_subgroup_handle_t b = open_sg(e.sv, sh, GROUP_WIDE, 11);
    write_obj(e.sv, b, 0, 11);
    run(&e, 11);
    uint64_t rb = last_hdr_ref(&e.ls);
    MOQ_TEST_CHECK(ra != rb);
    MOQ_TEST_CHECK(hdr_of(&e.ls, ra) != hdr_of(&e.ls, rb));

    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reset_subgroup(e.sv, a, 0x9, 12), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_reset_subgroup(e.sv, b, 0x9, 12), (int)MOQ_OK);
    run(&e, 12);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "app reset", v), 2);
    MOQ_TEST_CHECK_EQ_U64(reliable_of(&e.ls, ra), hdr_of(&e.ls, ra));
    MOQ_TEST_CHECK_EQ_U64(reliable_of(&e.ls, rb), hdr_of(&e.ls, rb));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(e.c), (int)MOQ_SESS_ESTABLISHED);
    env_close(&e);
}

/* The subscriber's STOP_SENDING is answered with a reset that keeps the
 * header and nothing beyond it. */
static void test_subgroup_stop_answer(moq_version_t v)
{
    env_t e;
    moq_subscription_t ch, sh;
    if (!env_open(&e, v, 0) || !subscribe(&e, NULL, NULL, 0, &ch, &sh)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    moq_subgroup_handle_t a = open_sg(e.sv, sh, 6, 10);
    write_obj(e.sv, a, 0, 10);
    write_obj(e.sv, a, 1, 10);
    run(&e, 10);
    uint64_t ra = last_hdr_ref(&e.ls);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_data_stop(e.sv, moq_stream_ref_from_u64(ra), 0x1, 11),
                          (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "stop answer", v), 1);
    MOQ_TEST_CHECK_EQ_U64(reliable_of(&e.ls, ra), hdr_of(&e.ls, ra));
    env_close(&e);
}

/* Cancelling the subscription resets its open subgroups. */
static void test_subgroup_unsubscribe(moq_version_t v)
{
    env_t e;
    moq_subscription_t ch, sh;
    if (!env_open(&e, v, 0) || !subscribe(&e, NULL, NULL, 0, &ch, &sh)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    moq_subgroup_handle_t a = open_sg(e.sv, sh, 6, 10);
    write_obj(e.sv, a, 0, 10);
    moq_subgroup_handle_t b = open_sg(e.sv, sh, GROUP_WIDE, 10);
    write_obj(e.sv, b, 0, 10);
    run(&e, 10);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(e.c, ch, 11), (int)MOQ_OK);
    run(&e, 11);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "unsubscribe", v), 2);
    env_close(&e);
}

/* The subscriber abandons a PUBLISH: the publisher resets the publication's
 * subgroups. */
static void test_publication_unsubscribed(moq_version_t v)
{
    env_t e;
    if (!env_open(&e, v, 0)) { MOQ_TEST_CHECK(false); env_close(&e); return; }
    moq_publish_cfg_t pc;
    moq_publish_cfg_init(&pc);
    pc.track_namespace = (moq_namespace_t){ ns_parts, 1 };
    pc.track_name = MOQ_BYTES_LITERAL("p");
    moq_publication_t cp;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publish(e.c, &pc, 2, &cp), (int)MOQ_OK);
    run(&e, 2);
    moq_event_t ev;
    if (!take(e.sv, MOQ_EVENT_PUBLISH_REQUEST, &ev)) { MOQ_TEST_CHECK(false); env_close(&e); return; }
    moq_publication_t sp = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac;
    moq_accept_publish_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_accept_publish(e.sv, sp, &ac, 3), (int)MOQ_OK);
    run(&e, 3);
    drain_events(e.c);

    moq_subgroup_cfg_t sc;
    moq_subgroup_cfg_init(&sc);
    sc.group_id = GROUP_WIDE;
    sc.publisher_priority = 128;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_pub_subgroup(e.c, cp, &sc, 4, &sg), (int)MOQ_OK);
    write_obj(e.c, sg, 0, 4);
    run(&e, 4);

    int slot = pub_resolve_handle(e.c, cp);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) { env_close(&e); return; }
    if (v == MOQ_VERSION_DRAFT_16) {
        /* UNSUBSCRIBE for the PUBLISH's request id. */
        uint8_t buf[32];
        moq_buf_writer_t w;
        moq_buf_writer_init(&w, buf, sizeof(buf));
        MOQ_TEST_CHECK_EQ_INT((int)moq_d16_encode_varint_msg(&w, MOQ_D16_UNSUBSCRIBE,
                              e.c->publishes[slot].request_id), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_control_bytes(e.c, buf,
                              moq_buf_writer_offset(&w), 5), (int)MOQ_OK);
    } else {
        /* The subscriber resets its PUBLISH request stream. */
        MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_reset(e.c,
                              e.c->publishes[slot].rs.ref, 0x1, 5), (int)MOQ_OK);
    }
    collect(e.c, &e.lc);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.lc, "publication unsubscribed", v), 1);
    env_close(&e);
}

/* Track Properties carrying OBJECT_DELIVERY_TIMEOUT (0x02) and/or
 * SUBGROUP_DELIVERY_TIMEOUT (0x06), delta-encoded. */
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

static const uint64_t MS = 1000;

/* An object still queued past OBJECT_DELIVERY_TIMEOUT is dropped and its
 * stream reset with DELIVERY_TIMEOUT. */
static void test_object_timeout(moq_version_t v)
{
    uint8_t tp[16];
    size_t n = timeouts_kvp(tp, sizeof(tp), true, 100, false, 0);
    env_t e;
    moq_subscription_t ch, sh;
    if (!env_open(&e, v, 0) || !subscribe(&e, NULL, tp, n, &ch, &sh)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    moq_subgroup_handle_t a = open_sg(e.sv, sh, GROUP_WIDE, 10 * MS);
    collect(e.sv, &e.ls);
    uint64_t ra = last_hdr_ref(&e.ls);
    write_obj(e.sv, a, 0, 20 * MS);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, 500 * MS), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "object timeout", v), 1);
    MOQ_TEST_CHECK(e.ls.nrst == 1 && e.ls.rst[0].code == MOQ_RESET_DELIVERY_TIMEOUT);
    MOQ_TEST_CHECK_EQ_U64(reliable_of(&e.ls, ra), hdr_of(&e.ls, ra));
    env_close(&e);
}

/* D18 section 8 and D22 section 5.2 start the subgroup timer at FIN.
 * Handing off the queued object/FIN is not a transport commit. */
static void test_subgroup_timeout(moq_version_t v)
{
    uint8_t tp[16];
    size_t n = timeouts_kvp(tp, sizeof(tp), true, 20, true, 100);
    env_t e;
    moq_subscription_t ch, sh;
    if (!env_open(&e, v, 0) || !subscribe(&e, NULL, tp, n, &ch, &sh)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    moq_subgroup_handle_t a = open_sg(e.sv, sh, 6, 10 * MS);
    moq_rcbuf_t *payload = NULL;
    MOQ_TEST_CHECK(moq_rcbuf_create(&e.alloc, (const uint8_t *)"object", 6, &payload)
                   == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_write_object(e.sv, a, 0, payload, 10 * MS) == MOQ_OK);
    moq_rcbuf_decref(payload);
    collect(e.sv, &e.ls);
    uint64_t ra = last_hdr_ref(&e.ls);
    MOQ_TEST_CHECK(ra != 0 && hdr_of(&e.ls, ra) > 0);
    int slot = -1;
    for (size_t i = 0; i < e.sv->sg_cap; i++) {
        if (e.sv->subgroups[i].state != MOQ_SG_OPEN ||
            e.sv->subgroups[i].stream_ref._v != ra) continue;
        MOQ_TEST_CHECK(slot < 0);
        slot = (int)i;
    }
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) { env_close(&e); return; }
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroups[slot].stream_ref._v, ra);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroups[slot].delivery_deadline_us, UINT64_MAX);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroup_deadline_us, UINT64_MAX);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 0);
    /* Beyond both open-age delays, an unfinished subgroup still has no timer. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, 500 * MS), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 0);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroups[slot].delivery_deadline_us, UINT64_MAX);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroup_deadline_us, UINT64_MAX);

    const uint64_t fin_us = 750 * MS;
    const uint64_t deadline_us = fin_us + 100 * MS;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_close_subgroup(e.sv, a, fin_us), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK(e.sv->subgroups[slot].state == MOQ_SG_CLOSING);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroups[slot].delivery_deadline_us, deadline_us);
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroup_deadline_us, deadline_us);
    MOQ_TEST_CHECK_EQ_U64(moq_session_next_deadline_us(e.sv), deadline_us);
    /* The smaller OBJECT timeout must not become the subgroup deadline. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, fin_us + 20 * MS), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, deadline_us - 1), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, deadline_us), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "subgroup timeout", v), 1);
    MOQ_TEST_CHECK_EQ_U64(reliable_of(&e.ls, ra), hdr_of(&e.ls, ra));
    MOQ_TEST_CHECK(e.ls.nrst == 1 && e.ls.rst[0].ref == ra &&
                   e.ls.rst[0].code == MOQ_RESET_DELIVERY_TIMEOUT &&
                   e.ls.rst[0].detail >= offsetof(moq_reset_data_action_t, reliable_size) +
                                         sizeof(e.ls.rst[0].reliable));
    MOQ_TEST_CHECK_EQ_U64(e.sv->subgroup_deadline_us, UINT64_MAX);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, deadline_us), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_tick(e.sv, deadline_us + 100 * MS), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(e.ls.nrst, 1);
    MOQ_TEST_CHECK(moq_session_state(e.sv) == MOQ_SESS_ESTABLISHED);
    env_close(&e);
}

/* -- Fetches ----------------------------------------------------------------- */

/* A standalone FETCH accepted by the server; returns both handles. */
static bool fetch_accepted(env_t *e, moq_fetch_t *cf, moq_fetch_t *sf)
{
    moq_fetch_cfg_t cfg;
    moq_fetch_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    cfg.start_group = 0; cfg.start_object = 0;
    cfg.end_group = 4; cfg.end_object = 0;
    if (moq_session_fetch(e->c, &cfg, 2, cf) != MOQ_OK) return false;
    run(e, 2);
    moq_event_t ev;
    if (!take(e->sv, MOQ_EVENT_FETCH_REQUEST, &ev)) return false;
    *sf = ev.u.fetch_request.fetch;
    moq_event_cleanup(&ev);
    moq_accept_fetch_cfg_t ac;
    moq_accept_fetch_cfg_init(&ac);
    ac.end_group = 4;
    if (moq_session_accept_fetch(e->sv, *sf, &ac, 3) != MOQ_OK) return false;
    moq_fetch_object_cfg_t oc;
    moq_fetch_object_cfg_init(&oc);
    oc.group_id = 1; oc.object_id = 0; oc.publisher_priority = 5;
    oc.payload = mk_payload("fetched");
    moq_result_t rc = moq_session_write_fetch_object(e->sv, *sf, &oc, 3);
    moq_rcbuf_decref(oc.payload);
    if (rc != MOQ_OK) return false;
    run(e, 3);
    return true;
}

/* The publisher aborts its fetch response stream. */
static void test_fetch_abort(moq_version_t v)
{
    env_t e;
    moq_fetch_t cf, sf;
    if (!env_open(&e, v, 0) || !fetch_accepted(&e, &cf, &sf)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_abort_fetch(e.sv, sf, 0x4, 4), (int)MOQ_OK);
    run(&e, 4);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fetch abort", v), 1);
    env_close(&e);
}

/* The fetcher cancels: FETCH_CANCEL on draft-16, the request stream torn
 * down on the stream-correlated profiles. */
static void test_fetch_cancel(moq_version_t v)
{
    env_t e;
    moq_fetch_t cf, sf;
    if (!env_open(&e, v, 0) || !fetch_accepted(&e, &cf, &sf)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_fetch_cancel(e.c, cf, 4), (int)MOQ_OK);
    run(&e, 4);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fetch cancel", v), 1);
    env_close(&e);
}

/* Draft-22: a REQUEST_UPDATE on the fetch's request stream whose token the
 * publisher rejects (DELETE of an alias it never registered) is answered with
 * REQUEST_ERROR and the fetch response stream reset. */
static void test_fetch_update_refused(void)
{
    env_t e;
    moq_fetch_t cf, sf;
    if (!env_open(&e, MOQ_VERSION_DRAFT_22, 0) || !fetch_accepted(&e, &cf, &sf)) {
        MOQ_TEST_CHECK(false); env_close(&e); return;
    }
    int slot = fetch_resolve_handle(e.sv, sf);
    MOQ_TEST_CHECK(slot >= 0);
    if (slot < 0) { env_close(&e); return; }
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_REQUEST_UPDATE;
    m.request_id = e.sv->fetches[slot].request_id + 2;
    moq_d22_param_t p;
    memset(&p, 0, sizeof(p));
    p.type = 3;                    /* AUTHORIZATION_TOKEN */
    p.value.token.alias_type = 0;  /* DELETE */
    p.value.token.alias = 99;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request_type = MOQ_D22_FETCH;
    ctx.update_from_subscriber = true;
    ctx.max_filter_ranges = 8;
    uint8_t buf[64];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    MOQ_TEST_CHECK_EQ_INT((int)moq_d22_encode_message(&w, &m, &p, 1, &ctx), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_on_bidi_stream_bytes(e.sv, e.sv->fetches[slot].rs.ref,
                          buf, moq_buf_writer_offset(&w), false, 4), (int)MOQ_OK);
    collect(e.sv, &e.ls);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fetch update refused", MOQ_VERSION_DRAFT_22), 1);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(e.sv), (int)MOQ_SESS_ESTABLISHED);
    env_close(&e);
}

/* -- Fills (draft-22) ------------------------------------------------------ */

static moq_location_filter_cfg_t abs_start(uint64_t g, uint64_t o)
{
    moq_location_filter_cfg_t l;
    moq_location_filter_cfg_init(&l);
    l.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START;
    l.start_group = g; l.start_object = o;
    return l;
}

/* A fill the publisher's pool cannot hold fails on the wire: the header,
 * then the reset, keeping the header. */
static void test_fill_failed_on_wire(void)
{
    env_t e;
    if (!env_open(&e, MOQ_VERSION_DRAFT_22, 1)) { MOQ_TEST_CHECK(false); env_close(&e); return; }
    moq_location_filter_cfg_t loc = abs_start(3, 0);
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_subscription_t ch1, sh1, ch2, sh2;
    MOQ_TEST_CHECK(subscribe(&e, &fill, NULL, 0, &ch1, &sh1));   /* takes the one slot */
    drain_events(e.c); drain_events(e.sv);
    MOQ_TEST_CHECK(subscribe(&e, &fill, NULL, 0, &ch2, &sh2));   /* failed on the wire */
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fill failed on wire", MOQ_VERSION_DRAFT_22), 1);
    MOQ_TEST_CHECK(e.ls.nrst == 1 && e.ls.rst[0].reliable > 0);
    env_close(&e);
}

/* An open fill aborted by the publisher, a fill reset with its parent, and a
 * fill stream the subscriber stops. */
static void test_fill_stream_resets(void)
{
    env_t e;
    if (!env_open(&e, MOQ_VERSION_DRAFT_22, 4)) { MOQ_TEST_CHECK(false); env_close(&e); return; }
    moq_location_filter_cfg_t loc = abs_start(3, 0);
    moq_fill_cfg_t fill;
    moq_fill_cfg_init(&fill);
    fill.location = &loc;
    moq_event_t ev;

    /* Aborted after opening. */
    moq_subscription_t ch1, sh1;
    MOQ_TEST_CHECK(subscribe(&e, &fill, NULL, 0, &ch1, &sh1));
    MOQ_TEST_CHECK(take(e.sv, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_t f1 = ev.u.fill_request.fill;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(e.sv, f1, 6), (int)MOQ_OK);
    run(&e, 6);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_abort_fill(e.sv, f1, 0x7, 7), (int)MOQ_OK);
    run(&e, 7);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fill abort", MOQ_VERSION_DRAFT_22), 1);
    drain_events(e.c); drain_events(e.sv);

    /* Reset with its parent subscription. */
    moq_subscription_t ch2, sh2;
    MOQ_TEST_CHECK(subscribe(&e, &fill, NULL, 0, &ch2, &sh2));
    MOQ_TEST_CHECK(take(e.sv, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_t f2 = ev.u.fill_request.fill;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(e.sv, f2, 8), (int)MOQ_OK);
    run(&e, 8);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_unsubscribe(e.c, ch2, 9), (int)MOQ_OK);
    run(&e, 9);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fill parent cancel", MOQ_VERSION_DRAFT_22), 2);
    drain_events(e.c); drain_events(e.sv);

    /* Stopped by the subscriber. */
    moq_subscription_t ch3, sh3;
    MOQ_TEST_CHECK(subscribe(&e, &fill, NULL, 0, &ch3, &sh3));
    MOQ_TEST_CHECK(take(e.c, MOQ_EVENT_FILL_CREATED, &ev));
    moq_fill_t cf3 = ev.u.fill_created.fill;
    MOQ_TEST_CHECK(take(e.sv, MOQ_EVENT_FILL_REQUEST, &ev));
    moq_fill_t f3 = ev.u.fill_request.fill;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_open_fill(e.sv, f3, 10), (int)MOQ_OK);
    run(&e, 10);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_cancel_fill(e.c, cf3, 11), (int)MOQ_OK);
    run(&e, 11);
    MOQ_TEST_CHECK_EQ_SIZE(check_resets(&e.ls, "fill stopped", MOQ_VERSION_DRAFT_22), 3);
    env_close(&e);
}

int main(void)
{
    static const moq_version_t all[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        test_subgroup_app_reset(all[i]);
        test_subgroup_stop_answer(all[i]);
        test_publication_unsubscribed(all[i]);
        test_fetch_abort(all[i]);
        test_fetch_cancel(all[i]);
    }
    /* UNSUBSCRIBE is the draft-16 cancellation message. */
    test_subgroup_unsubscribe(MOQ_VERSION_DRAFT_16);
    /* Track-property delivery timeouts are the draft-18/22 mechanism. */
    test_object_timeout(MOQ_VERSION_DRAFT_18);
    test_object_timeout(MOQ_VERSION_DRAFT_22);
    test_subgroup_timeout(MOQ_VERSION_DRAFT_18);
    test_subgroup_timeout(MOQ_VERSION_DRAFT_22);
    test_fetch_update_refused();
    test_fill_failed_on_wire();
    test_fill_stream_resets();

    if (failures) {
        fprintf(stderr, "test_reset_reliable_size: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_reset_reliable_size: all passed\n");
    return 0;
}
