/* Reuse the established packet harnesses, keeping their original entry point
 * as an ordinary regression. No callback or native ACK state is fabricated. */
#define main retained_reliable_reset_main
#ifdef MOQ_TERMINAL_WT
#include "../../pico_wt/tests/test_pico_wt_reliable_reset.c"
typedef pico_wt_harness_t terminal_pair_t;
#define terminal_pump pico_wt_harness_pump
#define terminal_cleanup pico_wt_harness_cleanup
static int terminal_setup(terminal_pair_t *p, moq_version_t v)
{ return open_harness(p, 0x71, v, true, 0) ? 0 : -1; }
#else
#include "test_pq_sim_reliable_reset.c"
typedef pq_sim_t terminal_pair_t;
#define terminal_pump sim_pump
#define terminal_cleanup sim_cleanup
static int terminal_setup(terminal_pair_t *p, moq_version_t v)
{ return sim_setup(p, 0x71, v, true, 0) == 0 ? sim_handshake(p) : -1; }
#endif
#undef main

typedef struct {
    picoquic_stream_data_cb_fn next;
    void *context;
    uint64_t sid, reset_code;
    unsigned commits, resets, stops;
} terminal_spy_t;

static int terminal_callback(picoquic_cnx_t *cnx, uint64_t sid, uint8_t *bytes,
    size_t len, picoquic_call_back_event_t event, void *context, void *stream_context)
{
    terminal_spy_t *spy = context;
    if (sid == spy->sid) {
        if (event == picoquic_callback_stream_send_committed) spy->commits++;
        if (event == picoquic_callback_stop_sending) spy->stops++;
        if (event == picoquic_callback_stream_reset) {
            spy->resets++;
            spy->reset_code = picoquic_get_remote_stream_error(cnx, sid);
        }
    }
    return spy->next(cnx, sid, bytes, len, event, spy->context, stream_context);
}

static void spy_attach(picoquic_cnx_t *cnx, terminal_spy_t *spy, uint64_t sid)
{
    memset(spy, 0, sizeof(*spy));
    spy->sid = sid;
    spy->next = picoquic_get_callback_function(cnx);
    spy->context = picoquic_get_callback_context(cnx);
    picoquic_set_callback(cnx, terminal_callback, spy);
}

static moq_subscription_t terminal_subscribe(terminal_pair_t *p, moq_version_t v)
{
    moq_bytes_t ns = MOQ_BYTES_LITERAL("terminal");
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ &ns, 1 };
    cfg.track_name = MOQ_BYTES_LITERAL("track");
    cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START;
    moq_subscription_t client, server = MOQ_SUBSCRIPTION_INVALID;
    CHECK(moq_session_subscribe(p->client_session, &cfg, p->now, &client) == MOQ_OK);
    terminal_pump(p, 100);
    moq_event_t ev;
    while (moq_session_poll_events(p->server_session, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) server = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    CHECK(moq_subscription_is_valid(server));
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    static const uint8_t timeout[] = { 6, 100 }; /* MOQT varints: 100ms */
    if (v != MOQ_VERSION_DRAFT_16) ac.track_properties = (moq_bytes_t){timeout, sizeof(timeout)};
    moq_result_t rc = moq_session_accept_subscribe(p->server_session, server, &ac, p->now);
    CHECK(rc == MOQ_OK);
    if (rc != MOQ_OK) return MOQ_SUBSCRIPTION_INVALID;
    terminal_pump(p, 100);
    CHECK(client_saw(p, MOQ_EVENT_SUBSCRIBE_OK));
    return server;
}

static void terminal_row(moq_version_t v, unsigned mode)
{
    terminal_pair_t p;
    scenario = mode == 0 ? "adapter-ack-clears-deadline" :
        mode == 1 ? "adapter-fin-loss-timer-reset" : "adapter-stop-after-fin";
    if (terminal_setup(&p, v) != 0) { CHECK(false); terminal_cleanup(&p); return; }
    moq_subscription_t sub = terminal_subscribe(&p, v);
    if (!moq_subscription_is_valid(sub)) { terminal_cleanup(&p); return; }
    picoquic_cnx_t *sender = p.test_ctx->cnx_server;
    uint64_t sid = picoquic_get_next_local_stream_id(sender, 1);
    terminal_spy_t sent, received;
    spy_attach(sender, &sent, sid);
    spy_attach(p.test_ctx->cnx_client, &received, sid);
    moq_subgroup_cfg_t sgc;
    moq_subgroup_cfg_init(&sgc);
    sgc.group_id = 1;
    moq_subgroup_handle_t sg = {0};
    CHECK(moq_session_open_subgroup(p.server_session, sub, &sgc, p.now, &sg) == MOQ_OK);
    moq_rcbuf_t *data = NULL;
    CHECK(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"object", 6, &data) == MOQ_OK);
    CHECK(moq_session_write_object(p.server_session, sg, 0, data, p.now) == MOQ_OK);
    moq_rcbuf_decref(data);
    terminal_pump(&p, 50);
    CHECK(sent.commits == 0);
    uint64_t loss = mode == 0 ? 0 : UINT64_MAX;
    p.test_ctx->s_to_c_link->loss_mask = &loss;
    CHECK(moq_session_close_subgroup(p.server_session, sg, p.now) == MOQ_OK);
    if (v != MOQ_VERSION_DRAFT_16)
        CHECK(moq_session_next_deadline_us(p.server_session) != UINT64_MAX);
    terminal_pump(&p, 50);
    if (mode == 0) {
        CHECK(sent.commits == 1);
        CHECK(moq_session_next_deadline_us(p.server_session) == UINT64_MAX);
        terminal_pump(&p, 300);
        CHECK(received.resets == 0 && sent.commits == 1);
    } else {
        picoquic_stream_head_t *st = picoquic_find_stream(sender, sid);
        CHECK(st && st->fin_sent && !picoquic_check_sack_list(&st->sack_list, 0, st->sent_offset));
        CHECK(sent.commits == 0);
        uint64_t expected = 2;
        if (mode == 2) {
            expected = 17;
#ifdef MOQ_TERMINAL_WT
            CHECK(pico_wt_moq_err_to_wt(17, &expected));
#endif
            CHECK(picoquic_stop_sending(p.test_ctx->cnx_client, sid, expected) == 0);
            terminal_pump(&p, 40);
            CHECK(sent.stops == 1);
        } else {
            terminal_pump(&p, 200);
#ifdef MOQ_TERMINAL_WT
            CHECK(pico_wt_moq_err_to_wt(2, &expected));
#endif
        }
        loss = 0;
        terminal_pump(&p, 1500);
        CHECK(sent.commits == 0 && received.resets == 1);
        CHECK(received.reset_code == expected);
        CHECK(moq_session_next_deadline_us(p.server_session) == UINT64_MAX);
    }
    CHECK(moq_session_state(p.server_session) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(p.client_session) == MOQ_SESS_ESTABLISHED);
#ifdef MOQ_TERMINAL_WT
    CHECK(h3zero_find_stream(p.server_conn->h3_ctx, sid) == NULL);
#endif
    picoquic_set_callback(sender, sent.next, sent.context);
    picoquic_set_callback(p.test_ctx->cnx_client, received.next, received.context);
    printf("d%u %s commits=%u resets=%u\n", (unsigned)v, scenario, sent.commits, received.resets);
    terminal_cleanup(&p);
}

int main(void)
{
    const moq_version_t versions[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        terminal_row(versions[i], 0);
        if (versions[i] != MOQ_VERSION_DRAFT_16) terminal_row(versions[i], 1);
        terminal_row(versions[i], 2);
    }
    return failures != 0;
}
