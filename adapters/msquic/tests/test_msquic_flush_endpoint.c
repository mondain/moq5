/* Result translation and both terminal/loss checks in the actual endpoint TU. The
 * managed query is typed and scripted here; its real behavior has a separate
 * executor/provider test. No network backend is created by this test. */
#include "../../../service/src/endpoint.c"
#include <stdio.h>

static moq_result_t results[2];
static unsigned probes, failures;

moq_result_t moq_msquic_managed_poll_flush(const moq_msquic_managed_t *m)
{
    (void)m;
    unsigned slot = probes++;
    return results[slot < 2 ? slot : 1];
}

static bool not_terminal(void *f) { (void)f; return false; }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    static const ep_backend_vtable_t vt = {
        .is_fatal = not_terminal, .clean_closed = not_terminal,
        .drain_state = msquic_vt_drain_state,
    };
    moq_endpoint_t ep;
    memset(&ep, 0, sizeof(ep));
    pthread_mutex_init(&ep.mu, NULL);
    atomic_init(&ep.interrupted, false);
    ep.pump_state = MOQ_ENDPOINT_ESTABLISHED;
    ep.vt = &vt;
    ep.facade = &ep;
    const struct { moq_result_t input; int output; } mapping[] = {
        {MOQ_OK, EP_DRAIN_DONE}, {MOQ_DONE, EP_DRAIN_PENDING},
        {MOQ_ERR_WRONG_STATE, EP_DRAIN_WRONG_THREAD},
        {MOQ_ERR_UNSUPPORTED, EP_DRAIN_UNSUPPORTED},
        {MOQ_ERR_INTERNAL, EP_DRAIN_FAILED},
        {MOQ_ERR_CLOSED, EP_DRAIN_CLOSED}, {MOQ_ERR_INVAL, EP_DRAIN_PENDING},
    };
    for (size_t i = 0; i < sizeof(mapping) / sizeof(mapping[0]); ++i) {
        results[0] = results[1] = mapping[i].input;
        probes = 0;
        CHECK(msquic_vt_drain_state(ep.facade) == mapping[i].output);
    }
    results[0] = results[1] = MOQ_OK;
    probes = 0;
    CHECK(moq_endpoint_drain(&ep, 0) == MOQ_OK); /* healthy live connection */
    CHECK(probes == 2);
    results[0] = results[1] = MOQ_DONE;
    probes = 0;
    CHECK(moq_endpoint_drain(&ep, 0) == MOQ_DONE);
    const struct { moq_result_t first, second; unsigned calls; } loss[] = {
        {MOQ_ERR_INTERNAL, MOQ_OK, 1},
        {MOQ_DONE, MOQ_ERR_INTERNAL, 2},
        {MOQ_OK, MOQ_ERR_INTERNAL, 2},
    };
    for (size_t i = 0; i < sizeof(loss) / sizeof(loss[0]); ++i) {
        results[0] = loss[i].first;
        results[1] = loss[i].second;
        probes = 0;
        CHECK(moq_endpoint_drain(&ep, 0) == MOQ_ERR_INTERNAL);
        CHECK(probes == loss[i].calls);
        CHECK(moq_endpoint_state(&ep) == MOQ_ENDPOINT_ESTABLISHED);
        CHECK(!ep.stopped && !moq_endpoint_is_closed(&ep));
        CHECK(!moq_endpoint_is_fatal(&ep));
    }
    results[0] = MOQ_ERR_CLOSED;
    results[1] = MOQ_OK;
    probes = 0;
    CHECK(moq_endpoint_drain(&ep, 0) == MOQ_ERR_CLOSED);
    CHECK(probes == 1);
    results[0] = MOQ_OK;
    results[1] = MOQ_ERR_CLOSED;
    probes = 0;
    CHECK(moq_endpoint_drain(&ep, 0) == MOQ_ERR_CLOSED);
    CHECK(probes == 2);
    atomic_store(&ep.interrupted, true);
    probes = 0;
    CHECK(moq_endpoint_drain(&ep, 0) == MOQ_ERR_INTERRUPTED);
    CHECK(probes == 0);
    pthread_mutex_destroy(&ep.mu);
    printf("msquic endpoint mapping: %u failures\n", failures);
    return failures ? 1 : 0;
}
