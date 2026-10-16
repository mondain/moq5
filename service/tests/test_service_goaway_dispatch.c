/*
 * The session-wide GOAWAY reaches the endpoint through the service's real
 * pump hook even when SETUP and the GOAWAY both precede that service's first
 * pass (or the service is attached after them): the hook still creates its
 * facade, the session's one event owner, which observes the GOAWAY and
 * records its New Session URI (moq_endpoint_get_goaway_uri); nothing is
 * published or subscribed on the draining session. Built once for the media
 * sender and once for the media receiver (SERVICE_GOAWAY_RECEIVER).
 */
#include <moq/moq.h>
#include <moq/endpoint.h>
#include <moq/media_sender.h>
#include <moq/media_receiver.h>
#include "test_support.h"
#include "test_raw_pair_support.h"
#include <string.h>

moq_endpoint_t *moq_endpoint_test_make_bare(void);
void moq_endpoint_test_free_bare(moq_endpoint_t *ep);

#ifdef SERVICE_GOAWAY_RECEIVER
moq_media_receiver_t *moq_media_receiver_test_new_cfg(const moq_media_receiver_cfg_t *cfg);
void moq_media_receiver_test_set_ep(moq_media_receiver_t *r, moq_endpoint_t *ep);
void moq_media_receiver_test_pump(moq_media_receiver_t *r, moq_session_t *session,
                                  uint64_t now_us);
void moq_media_receiver_test_destroy_with_session(moq_media_receiver_t *r,
                                                  moq_session_t *session, uint64_t now_us);
#define ROLE "receiver"
#else
moq_media_sender_t *moq_media_sender_test_new(void);
void moq_media_sender_test_free(moq_media_sender_t *s);
void moq_media_sender_test_set_ep(moq_media_sender_t *s, moq_endpoint_t *ep);
void moq_media_sender_test_pump(moq_media_sender_t *s, moq_session_t *session,
                                uint64_t now_us);
#define ROLE "sender"
#endif

static int failures;

/* uri: the GOAWAY's New Session URI; NULL is the empty one. */
static void scenario(moq_version_t v, const char *uri)
{
    moq_session_t *c, *sv;
    MOQ_TEST_CHECK(pair(&c, &sv, v));
    size_t ulen = uri ? strlen(uri) : 0;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_goaway(sv, (const uint8_t *)uri, ulen, 2), MOQ_OK);
    pump(sv, c, true, 2, 0);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(c), MOQ_SESS_DRAINING);

    moq_endpoint_t *ep = moq_endpoint_test_make_bare();
    uint8_t buf[64];
    size_t len = 99;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), &len), MOQ_DONE);
#ifdef SERVICE_GOAWAY_RECEIVER
    moq_bytes_t ns_parts[2] = { MOQ_BYTES_LITERAL("svc"), MOQ_BYTES_LITERAL("demo") };
    moq_media_receiver_cfg_t rcfg;
    moq_media_receiver_cfg_init_live(&rcfg);
    rcfg.namespace_.parts = ns_parts;
    rcfg.namespace_.count = 2;
    moq_media_receiver_t *r = moq_media_receiver_test_new_cfg(&rcfg);
    MOQ_TEST_CHECK(r != NULL);
    if (!r) { moq_endpoint_test_free_bare(ep); destroy(c, sv); return; }
    moq_media_receiver_test_set_ep(r, ep);
    for (uint64_t now = 3; now < 6; now++) moq_media_receiver_test_pump(r, c, now);
#else
    moq_media_sender_t *s = moq_media_sender_test_new();
    MOQ_TEST_CHECK(s != NULL);
    if (!s) { moq_endpoint_test_free_bare(ep); destroy(c, sv); return; }
    moq_media_sender_test_set_ep(s, ep);
    for (uint64_t now = 3; now < 6; now++) moq_media_sender_test_pump(s, c, now);
#endif
    len = 99;
    moq_result_t rc = moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), &len);
    MOQ_TEST_CHECK_EQ_INT((int)rc, MOQ_OK);
    if (rc == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_SIZE(len, ulen);
        if (len == ulen && ulen) MOQ_TEST_CHECK(memcmp(buf, uri, ulen) == 0);
    }
    /* Nothing was requested of the peer on the draining session. */
    int sent = 0;
    moq_action_t a;
    while (moq_session_poll_actions(c, &a, 1) > 0) {
        if (a.kind == MOQ_ACTION_OPEN_BIDI_STREAM || a.kind == MOQ_ACTION_SEND_CONTROL)
            sent++;
        moq_action_cleanup(&a);
    }
    MOQ_TEST_CHECK_EQ_INT(sent, 0);
#ifdef SERVICE_GOAWAY_RECEIVER
    moq_media_receiver_test_set_ep(r, NULL);
    moq_media_receiver_test_destroy_with_session(r, c, 7);
#else
    moq_media_sender_test_set_ep(s, NULL);
    moq_media_sender_test_free(s);
#endif
    moq_endpoint_test_free_bare(ep);
    destroy(c, sv);
}

int main(void)
{
    scenario(MOQ_VERSION_DRAFT_18, "moqt://next.example/");
    scenario(MOQ_VERSION_DRAFT_18, NULL);
    scenario(MOQ_VERSION_DRAFT_16, "moqt://next.example/");
    if (failures) {
        fprintf(stderr, "FAIL: service_goaway_dispatch (" ROLE ") %d\n", failures);
        return 1;
    }
    printf("PASS: service_goaway_dispatch (" ROLE ")\n");
    return 0;
}
