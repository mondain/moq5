/* Private D16 regression proposal. No production internals are mutated. */
#include <moq/session.h>
#include <moq/control.h>
#include <moq/codec.h>
#include "test_alloc_support.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; } } while (0)
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "PREREQUISITE %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; goto cleanup; } } while (0)

typedef struct packet { uint8_t bytes[4096]; size_t len; } packet;

/* Copy before advancing either session; never retain borrowed action bytes. */
static int collect(moq_session_t *s, packet out[8])
{
    moq_action_t a;
    int n = 0;
    bool valid = true;
    while (moq_session_poll_actions(s, &a, 1) == 1) {
        if (a.kind != MOQ_ACTION_SEND_CONTROL || n == 8 ||
            a.u.send_control.len > sizeof(out[0].bytes)) {
            valid = false;
        } else {
            out[n].len = a.u.send_control.len;
            memcpy(out[n].bytes, a.u.send_control.data, out[n].len);
            n++;
        }
        moq_action_cleanup(&a);
    }
    return valid ? n : -1;
}

static bool deliver(moq_session_t *from, moq_session_t *to)
{
    packet p[8];
    int n = collect(from, p);
    if (n <= 0) return false;
    for (int i = 0; i < n; ++i)
        if (moq_session_on_control_bytes(to, p[i].bytes, p[i].len, 0) != MOQ_OK)
            return false;
    return true;
}

static bool event(moq_session_t *s, moq_event_kind_t kind, moq_event_t *e)
{
    if (moq_session_poll_events(s, e, 1) != 1) return false;
    if (e->kind == kind) return true;
    moq_event_cleanup(e);
    return false;
}

static bool envelope(const packet *p, moq_control_envelope_t *e)
{
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, p->bytes, p->len);
    return moq_control_decode_envelope(&r, e) == MOQ_OK &&
           moq_buf_reader_remaining(&r) == 0;
}

static bool no_events(moq_session_t *s)
{
    moq_event_t e;
    bool empty = true;
    while (moq_session_poll_events(s, &e, 1)) {
        empty = false;
        moq_event_cleanup(&e);
    }
    return empty;
}

static moq_result_t update(moq_session_t *s, bool publication,
                           moq_publication_t ph, moq_subscription_t sh)
{
    if (publication) {
        moq_publication_update_cfg_t cfg;
        moq_publication_update_cfg_init(&cfg);
        cfg.has_subscriber_priority = true;
        cfg.subscriber_priority = 50;
        return moq_session_update_publication(s, ph, &cfg, 0);
    }
    moq_subscription_update_cfg_t cfg;
    moq_subscription_update_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.has_subscriber_priority = true;
    cfg.subscriber_priority = 50;
    return moq_session_update_subscription(s, sh, &cfg, 0);
}

static void run_case(bool publication, bool reject)
{
    int before = failures;
    const char *name = publication ? (reject ? "publish-reject" : "publish-ok")
                                   : "subscribe-reject";
    test_alloc_state_t as = {0};
    moq_alloc_t alloc = test_allocator(&as);
    moq_session_t *client = NULL, *server = NULL;
    moq_session_t *publisher = NULL, *subscriber = NULL;
    moq_publication_t ph = {0}, remote_ph = {0};
    moq_subscription_t sh = {0}, remote_sh = {0};
    moq_event_t ev;
    packet request[8], replies[8], extra[8];
    moq_control_envelope_t env;
    moq_session_cfg_t cfg = MOQ_SESSION_CFG_INIT;
    cfg.alloc = &alloc;
    cfg.version = MOQ_VERSION_DRAFT_16;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 32;
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    REQUIRE(moq_session_create(&cfg, 0, &client) == MOQ_OK);
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    REQUIRE(moq_session_create(&cfg, 0, &server) == MOQ_OK);
    REQUIRE(moq_session_start(client, 0) == MOQ_OK);
    REQUIRE(deliver(client, server));
    REQUIRE(deliver(server, client));
    REQUIRE(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    REQUIRE(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
    /* Setup events are not part of the per-track inventory. */
    while (moq_session_poll_events(client, &ev, 1)) moq_event_cleanup(&ev);
    while (moq_session_poll_events(server, &ev, 1)) moq_event_cleanup(&ev);

    moq_bytes_t parts[] = { MOQ_BYTES_LITERAL("issue44") };
    moq_namespace_t ns = { parts, 1 };
    if (publication) {
        publisher = client; subscriber = server;
        moq_publish_cfg_t pc;
        moq_publish_cfg_init(&pc);
        pc.track_namespace = ns; pc.track_name = MOQ_BYTES_LITERAL("track");
        REQUIRE(moq_session_publish(client, &pc, 0, &ph) == MOQ_OK);
        REQUIRE(deliver(client, server));
        REQUIRE(event(server, MOQ_EVENT_PUBLISH_REQUEST, &ev));
        remote_ph = ev.u.publish_request.pub;
        moq_event_cleanup(&ev);
        moq_accept_publish_cfg_t ac;
        moq_accept_publish_cfg_init(&ac);
        REQUIRE(moq_session_accept_publish(server, remote_ph, &ac, 0) == MOQ_OK);
        REQUIRE(deliver(server, client));
        REQUIRE(event(client, MOQ_EVENT_PUBLISH_OK, &ev));
        moq_event_cleanup(&ev);
    } else {
        publisher = server; subscriber = client;
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = ns; sc.track_name = MOQ_BYTES_LITERAL("track");
        sc.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
        REQUIRE(moq_session_subscribe(client, &sc, 0, &sh) == MOQ_OK);
        REQUIRE(deliver(client, server));
        REQUIRE(event(server, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
        remote_sh = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
        moq_accept_subscribe_cfg_t ac;
        moq_accept_subscribe_cfg_init(&ac);
        REQUIRE(moq_session_accept_subscribe(server, remote_sh, &ac, 0) == MOQ_OK);
        REQUIRE(deliver(server, client));
        REQUIRE(event(client, MOQ_EVENT_SUBSCRIBE_OK, &ev));
        moq_event_cleanup(&ev);
    }
    REQUIRE(collect(publisher, extra) == 0);
    REQUIRE(collect(subscriber, extra) == 0);
    REQUIRE(no_events(publisher));
    REQUIRE(no_events(subscriber));
    REQUIRE(update(subscriber, publication, remote_ph, sh) == MOQ_OK);
    REQUIRE(collect(subscriber, request) == 1);
    REQUIRE(envelope(&request[0], &env));
    REQUIRE(env.msg_type == MOQ_D16_REQUEST_UPDATE);
    moq_kvp_entry_t params[8];
    moq_d16_request_update_t u = { .params = params, .params_cap = 8 };
    REQUIRE(moq_d16_decode_request_update(env.payload, env.payload_len, &u) == MOQ_OK);
    REQUIRE(u.request_id == (publication ? 1u : 2u));
    REQUIRE(u.existing_request_id == 0);
    REQUIRE(u.params_count == 1);
    REQUIRE(u.params[0].type == MOQ_MSG_PARAM_SUBSCRIBER_PRIORITY);
    uint64_t uid = u.request_id, oid = u.existing_request_id;

    if (reject) {
        /* Keep the real outbound pending request/IDs; replace only its wire
         * parameters with the existing missing-USE_ALIAS negative trigger. */
        uint8_t token[64];
        moq_buf_writer_t tw, w;
        moq_buf_writer_init(&tw, token, sizeof(token));
        moq_d16_auth_token_t t = {
            .alias_type = MOQ_AUTH_TOKEN_USE_ALIAS, .alias = 999 };
        REQUIRE(moq_d16_auth_token_encode(&tw, &t) == MOQ_OK);
        moq_kvp_entry_t p = { .type = MOQ_MSG_PARAM_AUTHORIZATION_TOKEN,
            .value = token, .value_len = moq_buf_writer_offset(&tw),
            .is_varint = false };
        moq_buf_writer_init(&w, request[0].bytes, sizeof(request[0].bytes));
        REQUIRE(moq_d16_encode_request_update(&w, uid, oid, &p, 1) == MOQ_OK);
        request[0].len = moq_buf_writer_offset(&w);
    }
    REQUIRE(moq_session_on_control_bytes(publisher, request[0].bytes,
                                        request[0].len, 0) == MOQ_OK);
    REQUIRE(moq_session_state(publisher) == MOQ_SESS_ESTABLISHED);
    int n = collect(publisher, replies);
    REQUIRE(n >= 1);
    REQUIRE(envelope(&replies[0], &env));
    if (reject) {
        REQUIRE(env.msg_type == MOQ_D16_REQUEST_ERROR);
        moq_d16_request_error_t error;
        REQUIRE(moq_d16_decode_request_error(env.payload, env.payload_len,
                                            &error) == MOQ_OK);
        REQUIRE(error.request_id == uid && error.error_code == 0x17);
        /* Do not abort on missing DONE: independently exercise the receiver. */
        CHECK(n == 2);
        REQUIRE(moq_session_on_control_bytes(subscriber, replies[0].bytes,
                                            replies[0].len, 0) == MOQ_OK);
        REQUIRE(moq_session_state(subscriber) == MOQ_SESS_ESTABLISHED);
        CHECK(update(subscriber, publication, remote_ph, sh) == MOQ_ERR_WRONG_STATE);
        CHECK(collect(subscriber, extra) == 0);
        if (n == 2) {
            REQUIRE(envelope(&replies[1], &env));
            REQUIRE(env.msg_type == MOQ_D16_PUBLISH_DONE);
            moq_d16_publish_done_t done;
            REQUIRE(moq_d16_decode_publish_done(env.payload, env.payload_len,
                                               &done) == MOQ_OK);
            REQUIRE(done.request_id == oid && done.status_code == 0x8);
            REQUIRE(done.stream_count == 0);
            REQUIRE(moq_session_on_control_bytes(subscriber, replies[1].bytes,
                                                replies[1].len, 0) == MOQ_OK);
            REQUIRE(event(subscriber, publication ? MOQ_EVENT_PUBLISH_FINISHED :
                                                    MOQ_EVENT_SUBSCRIBE_DONE, &ev));
            CHECK(publication ? ev.u.publish_finished.status_code == 0x8 :
                                ev.u.subscribe_done.status_code == 0x8);
            moq_event_cleanup(&ev);
            CHECK(update(subscriber, publication, remote_ph, sh) == MOQ_ERR_STALE_HANDLE);
            CHECK(collect(subscriber, extra) == 0);
        }
        CHECK(no_events(publisher));
    } else {
        REQUIRE(n == 1 && env.msg_type == MOQ_D16_REQUEST_OK);
        moq_kvp_entry_t okparams[8];
        moq_d16_request_ok_t ok = { .params = okparams, .params_cap = 8 };
        REQUIRE(moq_d16_decode_request_ok(env.payload, env.payload_len, &ok) == MOQ_OK);
        REQUIRE(ok.request_id == uid);
        REQUIRE(event(publisher, MOQ_EVENT_PUBLISH_UPDATED, &ev));
        CHECK(ev.u.publish_updated.has_subscriber_priority);
        CHECK(ev.u.publish_updated.subscriber_priority == 50);
        moq_event_cleanup(&ev);
        REQUIRE(moq_session_on_control_bytes(subscriber, replies[0].bytes,
                                            replies[0].len, 0) == MOQ_OK);
        REQUIRE(event(subscriber, MOQ_EVENT_PUBLICATION_UPDATE_OK, &ev));
        moq_event_cleanup(&ev);
        CHECK(update(subscriber, publication, remote_ph, sh) == MOQ_OK);
        CHECK(collect(subscriber, extra) == 1);
    }
    CHECK(no_events(publisher));
    CHECK(no_events(subscriber));
    CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    CHECK(moq_session_state(server) == MOQ_SESS_ESTABLISHED);
cleanup:
    if (client) {
        while (moq_session_poll_events(client, &ev, 1)) moq_event_cleanup(&ev);
        moq_session_destroy(client);
    }
    if (server) {
        while (moq_session_poll_events(server, &ev, 1)) moq_event_cleanup(&ev);
        moq_session_destroy(server);
    }
    CHECK(as.balance == 0);
    printf("%s: %s\n", name, failures == before ? "PASS" : "FAIL");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "publish-reject") == 0) run_case(true, true);
    else if (strcmp(argv[1], "publish-ok") == 0) run_case(true, false);
    else if (strcmp(argv[1], "subscribe-reject") == 0) run_case(false, true);
    else return 2;
    return failures ? 1 : 0;
}
