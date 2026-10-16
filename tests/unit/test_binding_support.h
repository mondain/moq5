#ifndef MOQ_TEST_BINDING_SUPPORT_H
#define MOQ_TEST_BINDING_SUPPORT_H

/*
 * Test transport binding: a native-QUIC binding with every fact known and
 * supported, so a session whose profile requires one can start. Servers
 * accept any connection target; clients name a fixed authority with an empty
 * path. Sessions whose profile needs no binding are left alone.
 */
#include <moq/moq.h>

static uint32_t test_binding_accept_uri(void *ctx, uint32_t present,
                                        moq_bytes_t authority,
                                        moq_bytes_t path_query)
{
    (void)ctx; (void)present; (void)authority; (void)path_query;
    return MOQ_URI_ACCEPT;
}

static moq_result_t test_bind_if_required(moq_session_t *s)
{
    if (moq_session_binding_requirements(s) == 0) return MOQ_OK;
    static const uint8_t authority[] = "test.invalid";
    moq_transport_binding_t b;
    moq_transport_binding_init(&b);
    b.kind = MOQ_BINDING_RAW_QUIC;
    b.facts_known = MOQ_BINDING_FACTS_ALL;
    b.facts_supported = MOQ_BINDING_FACTS_ALL;
    if (moq_session_perspective(s) == MOQ_PERSPECTIVE_SERVER) {
        b.accept_uri = test_binding_accept_uri;
    } else {
        b.target_present = MOQ_BINDING_TARGET_COMPLETE;
        b.authority.data = authority;
        b.authority.len = sizeof(authority) - 1;
    }
    return moq_session_bind_transport(s, &b, sizeof(b));
}

#endif /* MOQ_TEST_BINDING_SUPPORT_H */
