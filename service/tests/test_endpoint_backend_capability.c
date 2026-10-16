/*
 * The version offer follows what the selected backend can carry.
 *
 * A picoquic without its stream-terminal contract cannot host draft-22: the
 * picoquic and pico_wt adapters refuse to attach such a session. The default
 * AUTO offer on that backend therefore leaves draft-22 out, over raw QUIC and
 * WebTransport alike, a preferred_version of draft-22 only orders the rest,
 * and an EXACT or LIST offer naming draft-22 is MOQ_ERR_UNSUPPORTED from the
 * resolver moq_endpoint_connect runs before it starts anything. A backend
 * outside that rule is unaffected. The test seam replaces the build's picoquic capability
 * in both directions.
 */
#include "endpoint_internal.h"
#include "test_support.h"
#include <picoquic.h>
#include <string.h>

void moq_endpoint_test_set_picoquic_draft_22(int hosts);

static int failures = 0;

static moq_endpoint_cfg_t mkcfg(const char *url)
{
    moq_endpoint_cfg_t c;
    moq_endpoint_cfg_init_sized(&c, sizeof(c));
    c.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    return c;
}

static bool offers(const moq_endpoint_resolved_t *r, moq_version_t v)
{
    for (size_t i = 0; i < r->version_count; i++)
        if (r->versions[i] == v) return true;
    return false;
}

static void offer_policy(moq_endpoint_cfg_t *c, moq_version_policy_t policy,
                         const moq_version_t *v, size_t n)
{
    c->versions.struct_size = sizeof(moq_version_offer_t);
    c->versions.policy = policy;
    c->versions.versions = v;
    c->versions.version_count = n;
}

int main(void)
{
    static const char *const urls[2] = {
        "moqt://relay.example:4443/moq", "https://relay.example:4443/moq",
    };
    static const moq_version_t d22[1] = { MOQ_VERSION_DRAFT_22 };
    static const moq_version_t d18[1] = { MOQ_VERSION_DRAFT_18 };
    static const moq_version_t d22_d18[2] = { MOQ_VERSION_DRAFT_22,
                                              MOQ_VERSION_DRAFT_18 };
    moq_endpoint_resolved_t r;

    /* == A picoquic without the contract. =============================== */
    moq_endpoint_test_set_picoquic_draft_22(0);
    for (int u = 0; u < 2; u++) {
        moq_endpoint_cfg_t c = mkcfg(urls[u]);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)r.backend, (int)MOQ_TRANSPORT_BACKEND_PICOQUIC);
        MOQ_TEST_CHECK(!offers(&r, MOQ_VERSION_DRAFT_22));
        MOQ_TEST_CHECK(offers(&r, MOQ_VERSION_DRAFT_18));
        MOQ_TEST_CHECK(r.version_count > 0 && r.versions[0] == MOQ_VERSION_DRAFT_18);
        MOQ_TEST_CHECK_EQ_INT((int)r.preferred_version, (int)MOQ_VERSION_DRAFT_18);

        c = mkcfg(urls[u]);
        offer_policy(&c, MOQ_VERSION_POLICY_EXACT, d22, 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r),
                              (int)MOQ_ERR_UNSUPPORTED);

        c = mkcfg(urls[u]);
        offer_policy(&c, MOQ_VERSION_POLICY_LIST, d22_d18, 2);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r),
                              (int)MOQ_ERR_UNSUPPORTED);

        /* A preference for draft-22 orders the eligible versions; it does
         * not make draft-22 required. */
        c = mkcfg(urls[u]);
        c.preferred_version = MOQ_VERSION_DRAFT_22;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(!offers(&r, MOQ_VERSION_DRAFT_22));
        MOQ_TEST_CHECK(r.version_count > 0 && r.versions[0] == MOQ_VERSION_DRAFT_18);

        /* A preference for an eligible version still leads the offer. */
        c = mkcfg(urls[u]);
        c.preferred_version = MOQ_VERSION_DRAFT_16;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(r.version_count == 2 && r.versions[0] == MOQ_VERSION_DRAFT_16 &&
                       r.versions[1] == MOQ_VERSION_DRAFT_18);

        c = mkcfg(urls[u]);
        offer_policy(&c, MOQ_VERSION_POLICY_EXACT, d18, 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(r.version_count == 1 && r.versions[0] == MOQ_VERSION_DRAFT_18);
    }
    {
        /* The same rule as the URI filter: a URL that yields no connection
         * target drops draft-22 from AUTO even when it is preferred, and an
         * EXACT draft-22 is refused. Checked with the capability present, so
         * only the URI filter is in play. */
        moq_endpoint_test_set_picoquic_draft_22(1);
        moq_endpoint_cfg_t c = mkcfg("moqt://relay.example/a b");
        c.preferred_version = MOQ_VERSION_DRAFT_22;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(!offers(&r, MOQ_VERSION_DRAFT_22));
        MOQ_TEST_CHECK(r.version_count > 0 && r.versions[0] == MOQ_VERSION_DRAFT_18);
        c = mkcfg("moqt://relay.example/a b");
        offer_policy(&c, MOQ_VERSION_POLICY_EXACT, d22, 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r),
                              (int)MOQ_ERR_UNSUPPORTED);
        moq_endpoint_test_set_picoquic_draft_22(0);
    }
#ifdef MOQ_SERVICE_HAVE_MSQUIC_MANAGED
    {
        /* The rule is the picoquic backend's: MsQuic still carries draft-22. */
        moq_endpoint_cfg_t c = mkcfg(urls[0]);
        c.backend = MOQ_TRANSPORT_BACKEND_MSQUIC;
        offer_policy(&c, MOQ_VERSION_POLICY_EXACT, d22, 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(r.version_count == 1 && r.versions[0] == MOQ_VERSION_DRAFT_22);
    }
#endif

    /* == A picoquic with the contract: draft-22 is offered first. ======== */
    moq_endpoint_test_set_picoquic_draft_22(1);
    for (int u = 0; u < 2; u++) {
        moq_endpoint_cfg_t c = mkcfg(urls[u]);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
        MOQ_TEST_CHECK(r.version_count > 0 && r.versions[0] == MOQ_VERSION_DRAFT_22);

        c = mkcfg(urls[u]);
        offer_policy(&c, MOQ_VERSION_POLICY_EXACT, d22, 1);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
    }

    /* == The build's own picoquic: the offer follows its contract. ======= */
    moq_endpoint_test_set_picoquic_draft_22(-1);
    {
        moq_endpoint_cfg_t c = mkcfg(urls[0]);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)MOQ_OK);
#if defined(PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION) && \
    PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION >= 1
        MOQ_TEST_CHECK(r.version_count > 0 && r.versions[0] == MOQ_VERSION_DRAFT_22);
        printf("build picoquic: stream-terminal contract present\n");
#else
        MOQ_TEST_CHECK(!offers(&r, MOQ_VERSION_DRAFT_22));
        printf("build picoquic: stream-terminal contract absent\n");
#endif
    }

    if (failures) {
        fprintf(stderr, "test_endpoint_backend_capability: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: endpoint_backend_capability\n");
    return 0;
}
