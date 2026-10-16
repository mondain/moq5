/*
 * What a fill takes from its subscription when FILL_PARAMETERS leaves a value
 * out (draft-22 §3.4, §9.20.15: "A parameter that is omitted from
 * FILL_PARAMETERS takes the value it has for the subscription").
 *
 * A subscription a PUBLISH began: a fill carried on a subscriber's
 * REQUEST_UPDATE that also replaces the Location filter covers that new
 * filter, end object included; the replacement becomes the subscription's
 * filter, so a later fill with no filter of its own inherits it, and
 * moq_session_publication_filter() reports it.
 */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include "../../core/src/wire/params_d22_internal.h"
#include <string.h>

static moq_bytes_t g_ns_parts[] = { { (const uint8_t *)"live", 4 } };

static void pump(moq_simpair_t *sp) { moq_simpair_run_until_quiescent(sp, 64, NULL); }

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

typedef struct {
    moq_simpair_t *sp;
    moq_session_t *c, *sv;
    moq_publication_t spub, cpub;
    moq_stream_ref_t sref;
} pub_pair_t;

/* The server publishes "v" to the client, which accepts; objects exist up to
 * {5,0}. */
static bool mk_publication(pub_pair_t *p)
{
    memset(p, 0, sizeof(*p));
    moq_simpair_cfg_t scfg = MOQ_SIMPAIR_CFG_INIT;
    scfg.alloc = moq_alloc_default();
    scfg.version = MOQ_VERSION_DRAFT_22;
    scfg.client_send_request_capacity = scfg.server_send_request_capacity = true;
    scfg.client_initial_request_capacity = scfg.server_initial_request_capacity = 64;
    if (moq_simpair_create(&scfg, &p->sp) < 0) return false;
    if (moq_simpair_start(p->sp) < 0) return false;
    pump(p->sp);
    p->c = moq_simpair_client(p->sp);
    p->sv = moq_simpair_server(p->sp);
    drain(p->c); drain(p->sv);
    moq_publish_cfg_t cfg; moq_publish_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("v");
    if (moq_session_publish(p->sv, &cfg, 1, &p->spub) != MOQ_OK) return false;
    pump(p->sp);
    moq_event_t ev;
    if (!take(p->c, MOQ_EVENT_PUBLISH_REQUEST, &ev)) return false;
    p->cpub = ev.u.publish_request.pub;
    moq_event_cleanup(&ev);
    moq_accept_publish_cfg_t ac; moq_accept_publish_cfg_init(&ac);
    if (moq_session_accept_publish(p->c, p->cpub, &ac, 2) != MOQ_OK) return false;
    pump(p->sp);
    moq_subgroup_cfg_t sg; moq_subgroup_cfg_init(&sg);
    sg.group_id = 5;
    moq_subgroup_handle_t h;
    if (moq_session_open_pub_subgroup(p->sv, p->spub, &sg, 2, &h) != MOQ_OK) return false;
    moq_rcbuf_t *pl = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"x", 1, &pl);
    moq_result_t rc = moq_session_write_object(p->sv, h, 0, pl, 2);
    moq_rcbuf_decref(pl);
    if (rc != MOQ_OK) return false;
    (void)moq_session_close_subgroup(p->sv, h, 2);
    pump(p->sp);
    drain(p->c); drain(p->sv);
    int slot = pub_resolve_handle(p->sv, p->spub);
    if (slot < 0) return false;
    p->sref = p->sv->publishes[slot].rs.ref;
    return p->sref._v != 0;
}

/* Feed the publisher a subscriber's REQUEST_UPDATE on the publication's
 * stream carrying a fill with no parameters of its own and, optionally, a
 * replacement Location filter. */
static moq_result_t feed_fill_update(pub_pair_t *p, uint64_t request_id,
                                     const moq_d22_location_filter_t *filter)
{
    moq_d22_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = MOQ_D22_REQUEST_UPDATE;
    m.request_id = request_id;
    moq_d22_param_t params[2];
    size_t n = 0;
    memset(params, 0, sizeof(params));
    if (filter) {
        params[n].type = 0x21;   /* LOCATION_FILTER */
        params[n].value.filter = *filter;
        n++;
    }
    static const uint8_t no_settings[] = { 0 };   /* Number of Parameters 0 */
    params[n].type = 0x23;       /* FILL_PARAMETERS with no settings of its own */
    params[n].value.fill = (moq_bytes_t){ no_settings, sizeof(no_settings) };
    n++;
    moq_d22_message_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request_type = MOQ_D22_PUBLISH;
    ctx.update_from_subscriber = true;
    ctx.max_filter_ranges = 8;
    uint8_t buf[128];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, sizeof(buf));
    moq_result_t rc = moq_d22_encode_message(&w, &m, params, n, &ctx);
    if (rc < 0) return rc;
    return moq_session_on_bidi_stream_bytes(p->sv, p->sref, buf,
                                            moq_buf_writer_offset(&w), false, 3);
}

static int test_publication_fill_range(void)
{
    int failures = 0;
    pub_pair_t p;
    MOQ_TEST_CHECK(mk_publication(&p));
    if (!p.sp) return failures;

    /* {1,0} through {3,3}, inclusive. */
    moq_d22_location_filter_t f = {
        .type = MOQ_D22_FILTER_RANGE, .start_group = 1, .start_object = 0,
        .end_group_delta = 2, .end_object = 3 };
    MOQ_TEST_CHECK_EQ_INT((int)feed_fill_update(&p, 0, &f), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_ESTABLISHED);
    moq_event_t ev;
    MOQ_TEST_CHECK(take(p.sv, MOQ_EVENT_FILL_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.start_group, 1);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.start_object, 0);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.end_group, 3);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.end_object, 3);
    moq_fill_t first = ev.u.fill_request.fill;
    moq_event_cleanup(&ev);

    /* The replacement is the subscription's filter from now on. */
    moq_location_filter_cfg_t out;
    moq_location_filter_cfg_init_sized(&out, sizeof(out));
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_publication_filter(p.sv, p.spub, &out),
                          (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)out.kind, (int)MOQ_LOCATION_FILTER_OBJECT_END);
    MOQ_TEST_CHECK_EQ_U64(out.start_group, 1);
    MOQ_TEST_CHECK_EQ_U64(out.end_group, 3);
    MOQ_TEST_CHECK_EQ_U64(out.end_object, 3);

    /* A later fill with no filter of its own inherits it, end object
     * included. */
    (void)moq_session_abort_fill(p.sv, first, 0, 3);
    drain(p.sv);
    MOQ_TEST_CHECK_EQ_INT((int)feed_fill_update(&p, 2, NULL), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(p.sv), (int)MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(take(p.sv, MOQ_EVENT_FILL_REQUEST, &ev));
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.start_group, 1);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.end_group, 3);
    MOQ_TEST_CHECK_EQ_U64(ev.u.fill_request.end_object, 3);
    moq_event_cleanup(&ev);
    moq_simpair_destroy(p.sp);
    return failures;
}

/* The subscriber's own fill on a publication, with no filter of its own,
 * inherits the publication's filter end object included -- here a filter
 * holding an end object, as a PUBLISH carrying one leaves it. */
static int test_local_publication_fill_range(void)
{
    int failures = 0;
    pub_pair_t p;
    MOQ_TEST_CHECK(mk_publication(&p));
    if (!p.sp) return failures;
    int cs = pub_resolve_handle(p.c, p.cpub);
    MOQ_TEST_CHECK(cs >= 0);
    if (cs < 0) { moq_simpair_destroy(p.sp); return failures; }
    moq_pub_entry_t *ce = &p.c->publishes[cs];
    ce->filter_type = MOQ_SUBSCRIBE_FILTER_OBJECT_END_INTERNAL;
    ce->req_start_group = 1; ce->req_start_object = 0;
    ce->req_end_group = 3; ce->req_end_object = 3;
    moq_fill_cfg_t fill; moq_fill_cfg_init(&fill);
    moq_publication_update_cfg_t up; moq_publication_update_cfg_init(&up);
    up.fill = &fill;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_update_publication(p.c, p.cpub, &up, 3),
                          (int)MOQ_OK);
    size_t live = 0;
    for (size_t i = 0; i < p.c->fills.capacity; i++) {
        if (!p.c->fill_storage[i].active) continue;
        live++;
        MOQ_TEST_CHECK_EQ_U64(p.c->fill_side[i].input.end_group, 3);
        MOQ_TEST_CHECK_EQ_U64(p.c->fill_side[i].input.end_object, 3);
    }
    MOQ_TEST_CHECK_EQ_SIZE(live, 1);
    moq_simpair_destroy(p.sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += test_publication_fill_range();
    failures += test_local_publication_fill_range();
    if (failures == 0) MOQ_TEST_PASS("fill_inheritance");
    return failures != 0;
}
