/*
 * The negotiated version is readable no later than the established session is
 * observable through post().
 *
 * A posted task runs on the managed context with the live session. A client
 * that learns "set up" from inside such a task (as the interop client does)
 * and then reads moq_endpoint_negotiated_version() must get the negotiated
 * draft, never 0: the endpoint caches the version before any task of the
 * cycle runs. Pure, no network: a bare endpoint whose facade stand-in reports
 * a version, pumped with a session established against an in-process peer.
 */
#include "test_support.h"
#include <moq/endpoint.h>
#include <moq/session.h>
#include <moq/types.h>
#include <string.h>

moq_endpoint_t *moq_endpoint_test_make_bare(void);
void moq_endpoint_test_free_bare(moq_endpoint_t *ep);
void moq_endpoint_test_pump_cycle(moq_endpoint_t *ep, moq_session_t *session,
                                  uint64_t now_us);
void moq_endpoint_test_set_facade_version(moq_endpoint_t *ep, moq_version_t v);

static int failures = 0;

typedef struct {
    moq_endpoint_t *ep;
    int             runs;
    bool            established;
    moq_version_t   version_seen;
} observer_t;

/* What an application does when its task sees the session established:
 * publish "set up" and read the negotiated version. */
static moq_result_t observe_task(moq_endpoint_t *ep, moq_session_t *sess,
                                 uint64_t now, void *ctx)
{
    (void)now;
    observer_t *o = (observer_t *)ctx;
    o->runs++;
    o->established = sess && moq_session_state(sess) == MOQ_SESS_ESTABLISHED;
    if (o->established) o->version_seen = moq_endpoint_negotiated_version(ep);
    return MOQ_OK;
}

static void pump(moq_session_t *from, moq_session_t *to)
{
    moq_action_t acts[16];
    size_t n;
    while ((n = moq_session_poll_actions(from, acts, 16)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (acts[i].kind == MOQ_ACTION_SEND_CONTROL)
                (void)moq_session_on_control_bytes(to, acts[i].u.send_control.data,
                                                   acts[i].u.send_control.len, 0);
        }
    }
}

int main(void)
{
    const moq_alloc_t *alloc = moq_alloc_default();
    moq_session_cfg_t ccfg, scfg;
    moq_session_cfg_init_sized(&ccfg, sizeof(ccfg), alloc, MOQ_PERSPECTIVE_CLIENT);
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), alloc, MOQ_PERSPECTIVE_SERVER);
    moq_session_t *client = NULL, *server = NULL;
    MOQ_TEST_CHECK(moq_session_create(&ccfg, 0, &client) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_create(&scfg, 0, &server) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_start(client, 0) == MOQ_OK);
    for (int i = 0; i < 4 && moq_session_state(client) != MOQ_SESS_ESTABLISHED; i++) {
        pump(client, server);
        pump(server, client);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(client), (int)MOQ_SESS_ESTABLISHED);
    moq_version_t negotiated = moq_session_version(client);
    MOQ_TEST_CHECK(negotiated != 0);

    moq_endpoint_t *ep = moq_endpoint_test_make_bare();
    MOQ_TEST_CHECK(ep != NULL);
    moq_endpoint_test_set_facade_version(ep, negotiated);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(ep), 0);

    /* The first cycle with the established session: the task that observes
     * it reads the negotiated version. */
    observer_t o = { ep, 0, false, 0 };
    MOQ_TEST_CHECK(moq_endpoint_post(ep, observe_task, &o) == MOQ_OK);
    moq_endpoint_test_pump_cycle(ep, client, 1000);
    MOQ_TEST_CHECK_EQ_INT(o.runs, 1);
    MOQ_TEST_CHECK(o.established);
    MOQ_TEST_CHECK_EQ_INT((int)o.version_seen, (int)negotiated);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(ep), (int)negotiated);

    /* A later cycle keeps the same answer. */
    observer_t later = { ep, 0, false, 0 };
    MOQ_TEST_CHECK(moq_endpoint_post(ep, observe_task, &later) == MOQ_OK);
    moq_endpoint_test_pump_cycle(ep, client, 2000);
    MOQ_TEST_CHECK_EQ_INT(later.runs, 1);
    MOQ_TEST_CHECK_EQ_INT((int)later.version_seen, (int)negotiated);

    moq_endpoint_test_free_bare(ep);
    moq_session_destroy(client);
    moq_session_destroy(server);
    if (failures) {
        fprintf(stderr, "test_endpoint_negotiated_publication: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: endpoint_negotiated_publication\n");
    return 0;
}
