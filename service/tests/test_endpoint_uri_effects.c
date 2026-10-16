/*
 * The real connect path compiled against a typed managed-backend recorder in
 * place of a network provider: what reaches the facade in each URI mode (the
 * host, SNI, CA file and the WebTransport path projection), the exact
 * allocation inventory, that every allocation refusal settles cleanly, and
 * that a refused configuration has no effect before the facade is reached.
 */
#define MOQ_SERVICE_HAVE_PROXYGEN_WT_MANAGED 1
#ifndef MOQ_ENDPOINT_URI_TEST_SOURCE
#define MOQ_ENDPOINT_URI_TEST_SOURCE "../src/endpoint.c"
#endif
#include MOQ_ENDPOINT_URI_TEST_SOURCE

#include <stdio.h>
#include <stdlib.h>

static unsigned failures, creates, calls, live, fail_at;
static const char *expected_path = "/original";
#define CHECK(row, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL[%s] line %d: %s\n", row, __LINE__, #condition); ++failures; \
} } while (0)

void moq_proxygen_wt_managed_cfg_init_sized(moq_proxygen_wt_managed_cfg_t *cfg,
                                            size_t cfg_size)
{ memset(cfg, 0, cfg_size); cfg->struct_size = (uint32_t)cfg_size; }
moq_result_t moq_proxygen_wt_managed_create(const moq_proxygen_wt_managed_cfg_t *cfg,
    moq_proxygen_wt_managed_t **out)
{
    ++creates;
    CHECK("callee.host", !strcmp(cfg->host, "example.com"));
    CHECK("callee.sni", !strcmp(cfg->sni, "tls.example"));
    CHECK("callee.path", !strcmp(cfg->path, expected_path));
    CHECK("callee.ca", !strcmp(cfg->ca_file, "/never/opened.pem"));
    *out = NULL;
    return MOQ_ERR_INTERNAL; /* reached boundary; no thread or TLS provider */
}
moq_result_t moq_proxygen_wt_managed_stop(moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.stop", false); return MOQ_ERR_INTERNAL; }
void moq_proxygen_wt_managed_destroy(moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.destroy", false); }
moq_session_t *moq_proxygen_wt_managed_session(moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.session", false); return NULL; }
moq_result_t moq_proxygen_wt_managed_wake(moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.wake", false); return MOQ_ERR_INTERNAL; }
moq_result_t moq_proxygen_wt_managed_wait(moq_proxygen_wt_managed_t *m, uint64_t t)
{ (void)m; (void)t; CHECK("unexpected.wait", false); return MOQ_ERR_INTERNAL; }
bool moq_proxygen_wt_managed_is_fatal(const moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.fatal", false); return true; }
uint64_t moq_proxygen_wt_managed_fatal_code(const moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.code", false); return 0; }
bool moq_proxygen_wt_managed_is_closed(const moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.closed", false); return true; }
moq_version_t moq_proxygen_wt_managed_negotiated_version(const moq_proxygen_wt_managed_t *m)
{ (void)m; CHECK("unexpected.version", false); return 0; }

static void *record_alloc(size_t size, void *ctx)
{
    (void)ctx; ++calls;
    if (calls == fail_at) return NULL;
    /* max_align_t preserves endpoint/mutex alignment under sanitizers. */
    max_align_t *p = malloc(sizeof(max_align_t) + size);
    if (!p) return NULL;
    memcpy(p, &size, sizeof(size)); ++live;
    return p + 1;
}
static void record_free(void *ptr, size_t size, void *ctx)
{
    (void)ctx;
    CHECK("free.owned", ptr && live);
    if (!ptr) return;
    max_align_t *p = (max_align_t *)ptr - 1;
    size_t original; memcpy(&original, p, sizeof(original));
    CHECK("free.size", size == original);
    --live; free(p);
}
static void *record_realloc(void *ptr, size_t old, size_t size, void *ctx)
{ (void)ptr; (void)old; (void)size; (void)ctx; CHECK("unexpected.realloc", false); return NULL; }
static moq_bytes_t bytes(const char *s)
{ return (moq_bytes_t){(const uint8_t *)s, strlen(s)}; }

/* Every allocation the connect path makes, failed one at a time, is a
 * MOQ_ERR_NOMEM with nothing left allocated and the facade never created. */
static void allocation_sweep(const moq_endpoint_cfg_t *cfg, const char *row)
{
    moq_endpoint_t *ep = NULL;
    calls = creates = 0; fail_at = 0;
    CHECK(row, moq_endpoint_connect(cfg, &ep) == MOQ_ERR_INTERNAL);
    CHECK(row, creates == 1 && !ep && !live);
    unsigned allocations = calls;
    for (unsigned i = 1; i <= allocations; ++i) {
        calls = creates = 0; fail_at = i;
        CHECK(row, moq_endpoint_connect(cfg, &ep) == MOQ_ERR_NOMEM);
        CHECK(row, !ep && !live && !creates);
    }
    fail_at = calls = creates = 0;
}

int main(void)
{
    moq_alloc_t allocator = {.alloc=record_alloc, .realloc=record_realloc, .free=record_free};
    moq_endpoint_cfg_t cfg;
    moq_endpoint_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.url = bytes("https://example.com:00443/original");
    cfg.sni = bytes("tls.example");
    cfg.ca_file = bytes("/never/opened.pem");
    cfg.backend = MOQ_TRANSPORT_BACKEND_PROXYGEN;
    cfg.alloc = &allocator;
    cfg.uri_mode = MOQ_ENDPOINT_URI_LEGACY;
    moq_endpoint_t *ep = NULL;
    CHECK("legacy.reaches.callee", moq_endpoint_connect(&cfg, &ep) == MOQ_ERR_INTERNAL);
    CHECK("legacy.exact.call", creates == 1 && !ep && !live);
    /* endpoint, owned target, host, sni, path, ca_file, WT offer string */
    CHECK("legacy.inventory", calls == 7);
    allocation_sweep(&cfg, "legacy.allocation");

    cfg.uri_mode = MOQ_ENDPOINT_URI_STANDARD;
    CHECK("standard.reaches.callee", moq_endpoint_connect(&cfg, &ep) == MOQ_ERR_INTERNAL);
    CHECK("standard.exact.call", creates == 1 && !ep && !live);
    CHECK("standard.inventory", calls == 7);
    allocation_sweep(&cfg, "standard.allocation");

    /* An empty STANDARD path reaches the facade as the HTTP projection "/". */
    cfg.url = bytes("https://example.com");
    expected_path = "/";
    CHECK("standard.projection", moq_endpoint_connect(&cfg, &ep) == MOQ_ERR_INTERNAL);
    CHECK("standard.projection.call", creates == 1 && !ep && !live);
    creates = 0;
    cfg.url = bytes("https://example.com?q=1");
    expected_path = "/?q=1";
    CHECK("standard.projection.query", moq_endpoint_connect(&cfg, &ep) == MOQ_ERR_INTERNAL);
    CHECK("standard.projection.query.call", creates == 1 && !ep && !live);
    expected_path = "/original";
    cfg.url = bytes("https://example.com:00443/original");

    calls = creates = 0;
    cfg.uri_mode = 2;
    CHECK("unknown.mode", moq_endpoint_connect(&cfg, &ep) == MOQ_ERR_INVAL);
    CHECK("unknown.no.effects", !ep && !calls && !creates && !live);
    cfg.uri_mode = MOQ_ENDPOINT_URI_STANDARD;
    const struct { const char *url; moq_result_t result; } rows[] = {
        {"moqt://user@host/a", MOQ_ERR_UNSUPPORTED},
        {"moqt://host:65536/a", MOQ_ERR_UNSUPPORTED},
        {"moqt://host#future:x", MOQ_ERR_UNSUPPORTED},
        {"moqt://host/a%ZZ", MOQ_ERR_INVAL},
        {"moqt://host#Bad:x", MOQ_ERR_INVAL},
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i) {
        cfg.url = bytes(rows[i].url);
        CHECK(rows[i].url, moq_endpoint_connect(&cfg, &ep) == rows[i].result);
        CHECK("refusal.no.effects", !ep && !calls && !creates && !live);
    }
    printf("endpoint_uri_effects: %u failures\n", failures);
    return failures ? 1 : 0;
}
