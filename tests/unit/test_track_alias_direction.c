/* Publisher-chosen aliases are independent in the two sending directions.
 * Exercise both establishment orders using only the public session lifecycle. */
#include <moq/moq.h>
#include <moq/sim.h>
#include "test_support.h"
#include <string.h>

static const moq_bytes_t g_ns_parts[] = { { (const uint8_t *)"live", 4 } };

static bool check(bool ok, const char *what, int *failures)
{
    if (!ok) {
        fprintf(stderr, "  FAIL: %s\n", what);
        (*failures)++;
    }
    return ok;
}

static bool result(moq_result_t rc, moq_result_t expected, const char *what,
                   int *failures)
{
    if (rc == expected) return true;
    fprintf(stderr, "  FAIL: %s returned %d, expected %d\n", what,
            (int)rc, (int)expected);
    (*failures)++;
    return false;
}

#define NEED(expr) do { if (!check((expr), #expr, failures)) return false; } while (0)
#define RC(expr, expected) do { \
    if (!result((expr), (expected), #expr, failures)) return false; \
} while (0)

static bool pump(moq_simpair_t *sp, int *failures)
{
    RC(moq_simpair_run_until_quiescent(sp, 64, NULL), MOQ_OK);
    NEED(moq_session_state(moq_simpair_client(sp)) == MOQ_SESS_ESTABLISHED);
    NEED(moq_session_state(moq_simpair_server(sp)) == MOQ_SESS_ESTABLISHED);
    return true;
}

static moq_simpair_t *mk_pair(moq_version_t v, int *failures)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.version = v;
    cfg.client_send_request_capacity = cfg.server_send_request_capacity = true;
    cfg.client_initial_request_capacity = cfg.server_initial_request_capacity = 64;
    moq_simpair_t *sp = NULL;
    if (!result(moq_simpair_create(&cfg, &sp), MOQ_OK, "pair create", failures))
        return NULL;
    if (!result(moq_simpair_start(sp), MOQ_OK, "pair start", failures) ||
        !pump(sp, failures)) {
        moq_simpair_destroy(sp);
        return NULL;
    }
    return sp;
}

static bool take(moq_session_t *s, moq_event_kind_t kind, moq_event_t *out)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        /* Do not poll again while the selected event's scratch is borrowed. */
        if (ev.kind == kind) { *out = ev; return true; }
        moq_event_cleanup(&ev);
    }
    return false;
}

static bool track_matches(moq_namespace_t ns, moq_bytes_t name, const char *track)
{
    return ns.count == 1 && ns.parts != NULL && ns.parts[0].data != NULL &&
        ns.parts[0].len == 4 && memcmp(ns.parts[0].data, "live", 4) == 0 &&
        name.data != NULL && name.len == strlen(track) &&
        memcmp(name.data, track, name.len) == 0;
}

static void publish_cfg(moq_publish_cfg_t *cfg, const char *track, uint64_t alias)
{
    moq_publish_cfg_init(cfg);
    cfg->track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg->track_name = moq_bytes_cstr(track);
    cfg->has_track_alias = true;
    cfg->track_alias = alias;
}

static bool finish_publish(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                           const char *track, uint64_t alias, moq_publication_t pub,
                           moq_publication_t *received, int *failures)
{
    if (!pump(sp, failures)) return false;
    moq_event_t ev;
    NEED(take(to, MOQ_EVENT_PUBLISH_REQUEST, &ev));
    *received = ev.u.publish_request.pub;
    bool ok = moq_publication_is_valid(*received) &&
        ev.u.publish_request.track_alias == alias &&
        track_matches(ev.u.publish_request.track_namespace,
                      ev.u.publish_request.track_name, track);
    moq_event_cleanup(&ev);
    NEED(ok);
    moq_accept_publish_cfg_t ac;
    moq_accept_publish_cfg_init(&ac);
    RC(moq_session_accept_publish(to, *received, &ac, 2), MOQ_OK);
    if (!pump(sp, failures)) return false;
    NEED(take(from, MOQ_EVENT_PUBLISH_OK, &ev));
    ok = moq_publication_eq(ev.u.publish_ok.pub, pub) && ev.u.publish_ok.send_allowed;
    moq_event_cleanup(&ev);
    NEED(ok);
    return true;
}

static bool publish_track(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                          const char *track, uint64_t alias, moq_publication_t *pub,
                          moq_publication_t *received, int *failures)
{
    moq_publish_cfg_t cfg;
    publish_cfg(&cfg, track, alias);
    *pub = *received = MOQ_PUBLICATION_INVALID;
    RC(moq_session_publish(from, &cfg, 1, pub), MOQ_OK);
    NEED(moq_publication_is_valid(*pub));
    return finish_publish(sp, from, to, track, alias, *pub, received, failures);
}

static bool request_subscribe(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                              const char *track, moq_subscription_t *sub,
                              moq_subscription_t *received, int *failures)
{
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){ g_ns_parts, 1 };
    cfg.track_name = moq_bytes_cstr(track);
    *sub = *received = MOQ_SUBSCRIPTION_INVALID;
    RC(moq_session_subscribe(from, &cfg, 1, sub), MOQ_OK);
    NEED(moq_subscription_is_valid(*sub));
    if (!pump(sp, failures)) return false;
    moq_event_t ev;
    NEED(take(to, MOQ_EVENT_SUBSCRIBE_REQUEST, &ev));
    *received = ev.u.subscribe_request.sub;
    bool ok = moq_subscription_is_valid(*received) &&
        track_matches(ev.u.subscribe_request.track_namespace,
                      ev.u.subscribe_request.track_name, track);
    moq_event_cleanup(&ev);
    NEED(ok);
    return true;
}

static bool finish_subscribe(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                             moq_subscription_t sub, moq_subscription_t received,
                             uint64_t alias, int *failures)
{
    moq_accept_subscribe_cfg_t ac;
    moq_accept_subscribe_cfg_init(&ac);
    ac.has_track_alias = true;
    ac.track_alias = alias;
    RC(moq_session_accept_subscribe(to, received, &ac, 2), MOQ_OK);
    if (!pump(sp, failures)) return false;
    moq_event_t ev;
    NEED(take(from, MOQ_EVENT_SUBSCRIBE_OK, &ev));
    bool ok = moq_subscription_eq(ev.u.subscribe_ok.sub, sub) &&
        ev.u.subscribe_ok.track_alias == alias;
    moq_event_cleanup(&ev);
    NEED(ok);
    return true;
}

static bool subscribe_track(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                            const char *track, uint64_t alias, moq_subscription_t *sub,
                            moq_subscription_t *received, int *failures)
{
    return request_subscribe(sp, from, to, track, sub, received, failures) &&
        finish_subscribe(sp, from, to, *sub, *received, alias, failures);
}

static bool send_object(moq_session_t *s, moq_subscription_t sub, moq_publication_t pub,
                        const char *payload, int *failures)
{
    moq_subgroup_cfg_t cfg;
    moq_subgroup_cfg_init(&cfg);
    cfg.group_id = 1;
    cfg.subgroup_id = 0;
    moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
    moq_result_t rc = moq_subscription_is_valid(sub)
        ? moq_session_open_subgroup(s, sub, &cfg, 3, &sg)
        : moq_session_open_pub_subgroup(s, pub, &cfg, 3, &sg);
    RC(rc, MOQ_OK);
    NEED(moq_subgroup_is_valid(sg));
    moq_rcbuf_t *buf = NULL;
    RC(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)payload,
                       strlen(payload), &buf), MOQ_OK);
    rc = moq_session_write_object(s, sg, 0, buf, 3);
    moq_rcbuf_decref(buf);
    RC(rc, MOQ_OK);
    RC(moq_session_close_subgroup(s, sg, 3), MOQ_OK);
    return true;
}

static bool received_object(moq_session_t *s, moq_subscription_t sub,
                            moq_publication_t pub, const char *payload, int *failures)
{
    size_t count = 0;
    bool ok = true;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            count++;
            const moq_object_received_event_t *obj = &ev.u.object_received;
            ok = ok && moq_subscription_eq(obj->sub, sub) &&
                moq_publication_eq(obj->pub, pub) && obj->group_id == 1 &&
                obj->subgroup_id == 0 &&
                obj->object_id == 0 && obj->status == MOQ_OBJECT_NORMAL &&
                obj->payload != NULL && moq_rcbuf_len(obj->payload) == strlen(payload) &&
                memcmp(moq_rcbuf_data(obj->payload), payload, strlen(payload)) == 0;
        }
        moq_event_cleanup(&ev);
    }
    NEED(count == 1 && ok);
    return true;
}

static bool publication_data(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                             moq_publication_t pub, moq_publication_t received,
                             const char *payload, int *failures)
{
    return send_object(from, MOQ_SUBSCRIPTION_INVALID, pub, payload, failures) &&
        pump(sp, failures) &&
        received_object(to, MOQ_SUBSCRIPTION_INVALID, received, payload, failures);
}

static bool subscription_data(moq_simpair_t *sp, moq_session_t *from, moq_session_t *to,
                              moq_subscription_t received, moq_subscription_t sub,
                              const char *payload, int *failures)
{
    return send_object(from, received, MOQ_PUBLICATION_INVALID, payload, failures) &&
        pump(sp, failures) &&
        received_object(to, sub, MOQ_PUBLICATION_INVALID, payload, failures);
}

/* Keep the original local-PUBLISH-first controls for both peer responses. */
static int forward_order(moq_version_t v, bool peer_publish)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(v, &failures);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t cpub, spub;
    if (!publish_track(sp, c, sv, "local-first", 42, &cpub, &spub, &failures)) goto done;
    if (peer_publish) {
        moq_publication_t pub, received;
        if (!publish_track(sp, sv, c, "peer-second", 42, &pub, &received, &failures))
            goto done;
        if (!publication_data(sp, sv, c, pub, received, "peer-publish", &failures))
            goto done;
    } else {
        moq_subscription_t sub, received;
        if (!subscribe_track(sp, c, sv, "peer-second", 42, &sub, &received, &failures))
            goto done;
        if (!subscription_data(sp, sv, c, received, sub, "peer-subscribe", &failures))
            goto done;
    }
    (void)publication_data(sp, c, sv, cpub, spub, "local-publish", &failures);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* The client learns the server's alias first. Its own different track may
 * use that number; a second client-sent track may not use a client-owned alias. */
static int subscription_first(moq_version_t v, bool local_publish, bool same_direction)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(v, &failures);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_session_t *first_subscriber = same_direction ? sv : c;
    moq_session_t *first_publisher = same_direction ? c : sv;
    moq_subscription_t first_sub, first_received;
    if (!subscribe_track(sp, first_subscriber, first_publisher, "first-track", 42,
                         &first_sub, &first_received, &failures)) goto done;
    uint64_t second_alias = same_direction ? 43 : 42;
    if (local_publish) {
        moq_publish_cfg_t cfg;
        publish_cfg(&cfg, "second-track", 42);
        moq_publication_t pub = MOQ_PUBLICATION_INVALID, received;
        moq_result_t rc = moq_session_publish(c, &cfg, 2, &pub);
        if (!result(rc, same_direction ? MOQ_ERR_INVAL : MOQ_OK,
                    "subscription-first local explicit PUBLISH", &failures)) goto done;
        if (same_direction) {
            if (!pump(sp, &failures)) goto done;
            moq_event_t ev;
            bool unexpected = take(sv, MOQ_EVENT_PUBLISH_REQUEST, &ev);
            if (unexpected) moq_event_cleanup(&ev);
            if (!check(!unexpected, "rejected PUBLISH queues no request", &failures)) goto done;
            cfg.track_alias = second_alias;
            if (!result(moq_session_publish(c, &cfg, 2, &pub), MOQ_OK,
                        "retry PUBLISH with unused alias", &failures)) goto done;
        }
        if (!check(moq_publication_is_valid(pub), "local publication handle", &failures) ||
            !finish_publish(sp, c, sv, "second-track", second_alias, pub,
                            &received, &failures) ||
            !publication_data(sp, c, sv, pub, received, "second-publication", &failures))
            goto done;
    } else {
        moq_subscription_t sub, received;
        if (!request_subscribe(sp, sv, c, "second-track", &sub, &received, &failures))
            goto done;
        if (same_direction) {
            moq_accept_subscribe_cfg_t ac;
            moq_accept_subscribe_cfg_init(&ac);
            ac.has_track_alias = true;
            ac.track_alias = 42;
            if (!result(moq_session_accept_subscribe(c, received, &ac, 2), MOQ_ERR_INVAL,
                        "same-direction SUBSCRIBE acceptance conflict", &failures) ||
                !pump(sp, &failures)) goto done;
            moq_event_t ev;
            bool unexpected = take(sv, MOQ_EVENT_SUBSCRIBE_OK, &ev);
            if (unexpected) moq_event_cleanup(&ev);
            if (!check(!unexpected, "rejected acceptance queues no OK", &failures)) goto done;
        }
        /* On conflict, retry the very same pending request with an unused alias. */
        if (!finish_subscribe(sp, sv, c, sub, received, second_alias, &failures) ||
            !subscription_data(sp, c, sv, received, sub, "second-subscription", &failures))
            goto done;
    }
    (void)subscription_data(sp, first_publisher, first_subscriber, first_received,
                            first_sub, "first-subscription", &failures);
done:
    moq_simpair_destroy(sp);
    return failures;
}

/* A peer-owned alias must not veto D22's same-track sharing among our own
 * subscriptions. Establish the PUBLISH first so this isolates shareability
 * from the subscription-first collision check above. */
static int shared_outbound_with_opposite_alias(bool local_publish)
{
    int failures = 0;
    moq_simpair_t *sp = mk_pair(MOQ_VERSION_DRAFT_22, &failures);
    if (!sp) return failures;
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    moq_publication_t first_pub, first_received;
    moq_subscription_t opposite_sub, opposite_received;
    if (!publish_track(sp, c, sv, "shared-outbound", 42,
                       &first_pub, &first_received, &failures) ||
        !subscribe_track(sp, c, sv, "opposite", 42,
                         &opposite_sub, &opposite_received, &failures) ||
        !subscription_data(sp, sv, c, opposite_received, opposite_sub,
                           "opposite-direction", &failures)) goto done;

    moq_publication_t second_pub = MOQ_PUBLICATION_INVALID;
    moq_publication_t second_received = MOQ_PUBLICATION_INVALID;
    moq_subscription_t second_sub = MOQ_SUBSCRIPTION_INVALID;
    moq_subscription_t second_publisher = MOQ_SUBSCRIPTION_INVALID;
    if (local_publish) {
        if (!publish_track(sp, c, sv, "shared-outbound", 42,
                           &second_pub, &second_received, &failures)) goto done;
    } else if (!subscribe_track(sp, sv, c, "shared-outbound", 42,
                                &second_sub, &second_publisher, &failures)) {
        goto done;
    }

    const char *payload = "shared-alias";
    if (!send_object(c, MOQ_SUBSCRIPTION_INVALID, first_pub, payload, &failures) ||
        !pump(sp, &failures)) goto done;
    size_t first_count = 0, second_count = 0, other_count = 0;
    bool bytes_match = true;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *obj = &ev.u.object_received;
            if (moq_publication_eq(obj->pub, first_received) &&
                moq_subscription_eq(obj->sub, MOQ_SUBSCRIPTION_INVALID)) {
                first_count++;
            } else if ((local_publish &&
                        moq_publication_eq(obj->pub, second_received) &&
                        moq_subscription_eq(obj->sub, MOQ_SUBSCRIPTION_INVALID)) ||
                       (!local_publish && moq_subscription_eq(obj->sub, second_sub) &&
                        moq_publication_eq(obj->pub, MOQ_PUBLICATION_INVALID))) {
                second_count++;
            } else {
                other_count++;
            }
            bytes_match = bytes_match && obj->group_id == 1 &&
                obj->subgroup_id == 0 && obj->object_id == 0 &&
                obj->status == MOQ_OBJECT_NORMAL && obj->payload != NULL &&
                moq_rcbuf_len(obj->payload) == strlen(payload) &&
                memcmp(moq_rcbuf_data(obj->payload), payload, strlen(payload)) == 0;
        }
        moq_event_cleanup(&ev);
    }
    (void)check(first_count == 1 && second_count == 1 && other_count == 0 && bytes_match,
                "shared alias attributes exact payload to both outbound owners", &failures);
done:
    moq_simpair_destroy(sp);
    return failures;
}

int main(void)
{
    int failures = 0;
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
    };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        for (int mode = 0; mode < 2; mode++) {
            int n = forward_order(versions[i], mode != 0);
            fprintf(stderr, "draft 0x%llx forward-%s: %s (%d)\n",
                    (unsigned long long)versions[i], mode ? "publish" : "subscribe",
                    n ? "FAIL" : "PASS", n);
            failures += n;
            for (int same = 0; same < 2; same++) {
                n = subscription_first(versions[i], mode != 0, same != 0);
                fprintf(stderr, "draft 0x%llx %s-%s: %s (%d)\n",
                        (unsigned long long)versions[i], same ? "same-direction" : "reverse",
                        mode ? "publish" : "accept-subscribe", n ? "FAIL" : "PASS", n);
                failures += n;
            }
        }
    }
    for (int mode = 0; mode < 2; mode++) {
        int n = shared_outbound_with_opposite_alias(mode != 0);
        fprintf(stderr, "draft22 shared-with-opposite-%s: %s (%d)\n",
                mode ? "publish" : "accept-subscribe", n ? "FAIL" : "PASS", n);
        failures += n;
    }
    if (failures == 0) MOQ_TEST_PASS("track_alias_direction");
    return failures != 0;
}
