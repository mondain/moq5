/*
 * A service endpoint connecting natively over QUIC to a moqt URI names that
 * URI in its setup on every profile: PATH carries the path-abempty with "?"
 * and the query, AUTHORITY the authority, exactly as the URI spells them
 * (draft-16 §9.3.1.1-2, draft-18 §10.3.1.1-2, draft-22 §9.1.1-2). A managed
 * MsQuic server (the endpoint is client-only) records what its URI policy is
 * asked; the endpoint runs over each raw-QUIC backend built here.
 *
 * Built only when MOQ_BUILD_MSQUIC_MANAGED is on.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <moq/endpoint.h>
#include <moq/msquic_managed.h>
#include "test_support.h"
#include "msq_loopback_port.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

struct seen {
    pthread_mutex_t mu;
    int asked;
    int setup;
    uint32_t present;
    char authority[128];
    size_t authority_len;
    char path[128];
    size_t path_len;
};

static uint32_t record_uri(void *ctx, uint32_t present, moq_bytes_t authority,
                           moq_bytes_t path_query)
{
    struct seen *sv = ctx;
    pthread_mutex_lock(&sv->mu);
    sv->asked++;
    sv->present = present;
    sv->authority_len = authority.len;
    if (authority.len < sizeof(sv->authority))
        memcpy(sv->authority, authority.data, authority.len);
    sv->path_len = path_query.len;
    if (path_query.len < sizeof(sv->path))
        memcpy(sv->path, path_query.data, path_query.len);
    pthread_mutex_unlock(&sv->mu);
    return MOQ_URI_ACCEPT;
}

static int server_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                       uint64_t now, void *ctx)
{
    struct seen *sv = ctx;
    (void)m; (void)now;
    for (moq_msquic_managed_conn_t *c = moq_msquic_lane_next_conn(lane, NULL);
         c != NULL; c = moq_msquic_lane_next_conn(lane, c)) {
        moq_session_t *s = moq_msquic_managed_conn_session(c);
        moq_event_t ev;
        while (s != NULL && moq_session_poll_events(s, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) {
                pthread_mutex_lock(&sv->mu);
                sv->setup++;
                pthread_mutex_unlock(&sv->mu);
            }
            moq_event_cleanup(&ev);
        }
    }
    return 0;
}

static void run(const char *cert, const char *key, moq_version_t version,
                moq_transport_backend_t backend, const char *backend_name)
{
    int before = failures;
    struct seen sv;
    memset(&sv, 0, sizeof(sv));
    pthread_mutex_init(&sv.mu, NULL);

    moq_connection_target_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.struct_size = (uint32_t)sizeof(policy);
    policy.accept_uri = record_uri;
    policy.accept_uri_ctx = &sv;

    moq_msquic_managed_cfg_t scfg;
    moq_msquic_managed_cfg_init_sized(&scfg, sizeof(scfg));
    scfg.alloc = moq_alloc_default();
    scfg.perspective = MOQ_PERSPECTIVE_SERVER;
    scfg.host = "127.0.0.1";
    scfg.cert_path = cert;
    scfg.key_path = key;
    scfg.on_lane_pump = server_pump;
    scfg.on_lane_pump_user = &sv;
    scfg.send_request_capacity = true;
    scfg.initial_request_capacity = 16;
    scfg.max_events = 64;
    scfg.version = version;
    scfg.target = &policy;
    moq_msquic_managed_t *srv = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)msqt_server_create(&scfg, &srv), (int)MOQ_OK);
    if (srv == NULL) { pthread_mutex_destroy(&sv.mu); return; }
    uint16_t port = moq_msquic_managed_port(srv);
    MOQ_TEST_CHECK(port != 0);

    char url[96], authority[32];
    snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned)port);
    snprintf(url, sizeof(url), "moqt://%s/moq?interop=1", authority);
    moq_endpoint_cfg_t c;
    moq_endpoint_cfg_init(&c);
    c.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    c.insecure_skip_verify = true;
    c.backend = backend;
    c.versions.struct_size = sizeof(moq_version_offer_t);
    c.versions.policy = MOQ_VERSION_POLICY_EXACT;
    c.versions.versions = &version;
    c.versions.version_count = 1;

    moq_endpoint_t *ep = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&c, &ep), (int)MOQ_OK);
    bool established = false;
    for (int i = 0; ep != NULL && i < 300 && !established; i++) {
        if (moq_endpoint_state(ep) == MOQ_ENDPOINT_ESTABLISHED) established = true;
        else if (moq_endpoint_wait(ep, 100000) == MOQ_ERR_CLOSED) break;
    }
    MOQ_TEST_CHECK(established);
    if (ep != NULL) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(ep), (int)version);
        (void)moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
    }
    (void)moq_msquic_managed_stop(srv);
    moq_msquic_managed_destroy(srv);

    pthread_mutex_lock(&sv.mu);
    MOQ_TEST_CHECK_EQ_INT(sv.asked, 1);
    MOQ_TEST_CHECK_EQ_INT(sv.setup, 1);
    MOQ_TEST_CHECK_EQ_U64(sv.present, MOQ_BINDING_TARGET_COMPLETE);
    MOQ_TEST_CHECK(sv.authority_len == strlen(authority) &&
                   memcmp(sv.authority, authority, sv.authority_len) == 0);
    MOQ_TEST_CHECK(sv.path_len == strlen("/moq?interop=1") &&
                   memcmp(sv.path, "/moq?interop=1", sv.path_len) == 0);
    pthread_mutex_unlock(&sv.mu);
    pthread_mutex_destroy(&sv.mu);
    if (failures == before)
        printf("PASS: endpoint_target_setup %s draft-%u\n", backend_name,
               (unsigned)version);
    else
        fprintf(stderr, "FAIL: endpoint_target_setup %s draft-%u\n",
                backend_name, (unsigned)version);
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cert") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: --cert <pem> --key <pem>\n");
        return 1;
    }
    static const moq_version_t versions[] = {
        MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22,
    };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        run(cert, key, versions[i], MOQ_TRANSPORT_BACKEND_MSQUIC, "msquic");
#ifdef MOQ_SERVICE_HAVE_PQ_THREADED
        run(cert, key, versions[i], MOQ_TRANSPORT_BACKEND_PICOQUIC, "picoquic");
#endif
    }
    if (failures == 0) printf("PASS: endpoint_target_setup\n");
    return failures != 0;
}
