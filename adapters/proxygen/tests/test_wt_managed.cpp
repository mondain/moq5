/*
 * Managed proxygen WebTransport facade — config/validation + ABI tests.
 *
 * These are the deterministic, no-network tests for the managed client
 * facade: every case here is rejected (or accepted) by create() BEFORE it
 * spawns the network thread or attempts a WT CONNECT, so they run without a
 * server. Real connection coverage is the E2E moqx WebTransport cell (a live
 * relay) — there is no cheap in-process proxygen WT
 * server for an in-tree loopback.
 */

#include <moq/proxygen_wt_managed.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int pump_noop(moq_proxygen_wt_managed_t *m, uint64_t now, void *ctx)
{
    (void)m; (void)now; (void)ctx;
    return 1;
}

static moq_proxygen_wt_managed_cfg_t base_cfg(void)
{
    moq_proxygen_wt_managed_cfg_t c;
    moq_proxygen_wt_managed_cfg_init(&c);
    c.perspective = MOQ_PERSPECTIVE_CLIENT;
    c.host = "127.0.0.1";
    c.port = 4433;
    c.on_pump = pump_noop;
    return c;
}

static void test_cfg_init(void)
{
    moq_proxygen_wt_managed_cfg_t c;
    moq_proxygen_wt_managed_cfg_init(&c);
    CHECK(c.struct_size == sizeof(moq_proxygen_wt_managed_cfg_t));
    CHECK(c.perspective == MOQ_PERSPECTIVE_CLIENT);
    CHECK(c.alpn_list == NULL);
    CHECK(c.alpn_count == 0);
}

/* All of these are rejected before any connect attempt. */
static void test_invalid_config(void)
{
    moq_proxygen_wt_managed_t *m = NULL;

    CHECK(moq_proxygen_wt_managed_create(NULL, &m) == MOQ_ERR_INVAL);

    moq_proxygen_wt_managed_cfg_t c = base_cfg();
    c.on_pump = NULL;                                   /* required */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.perspective = MOQ_PERSPECTIVE_SERVER;             /* client-only */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.host = NULL;                                      /* required */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.port = 0;                                         /* 1..65535 */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.port = 70000;
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.insecure_skip_verify = true;
    c.ca_file = "/some/ca.pem";                         /* mutually exclusive */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    c = base_cfg();
    c.struct_size = 4;                                  /* too small */
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);
}

/* WT-protocol offer validation (maps via the shared moq_alpn helper). */
static void test_alpn_offer(void)
{
    moq_proxygen_wt_managed_t *m = NULL;

    /* Unknown / non-MoQ token rejected before connect. */
    static const char *const bad[] = { "h3" };
    moq_proxygen_wt_managed_cfg_t c = base_cfg();
    c.alpn_list = bad;
    c.alpn_count = 1;
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_UNSUPPORTED);

    /* alpn_count > 0 with NULL list rejected. */
    c = base_cfg();
    c.alpn_list = NULL;
    c.alpn_count = 2;
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);

    /* A recognized multi-version offer ("moqt-18","moqt-16") passes config
     * validation. We deliberately do NOT exercise the accept path here: it
     * would spawn the network thread and attempt a real connect (DNS + 15s
     * timeout), making this deterministic test network-dependent. Real
     * connection + AUTO negotiation coverage is the E2E moqx WebTransport
     * cell (no cheap in-process proxygen WT server exists for a loopback). */
}

/* Old caller whose struct ends before the appended fields: create() must not
 * read past struct_size. Dirty the tail so an over-read would misbehave; an
 * absent host (offset within the old prefix) still yields a clean INVAL. */
static void test_old_struct_size_abi(void)
{
    moq_proxygen_wt_managed_cfg_t prefix;
    moq_proxygen_wt_managed_cfg_init(&prefix);
    prefix.perspective = MOQ_PERSPECTIVE_CLIENT;
    prefix.host = NULL;          /* -> INVAL, exercised without over-reading */
    prefix.port = 4433;
    prefix.on_pump = pump_noop;
    uint32_t old_size =
        (uint32_t)(offsetof(moq_proxygen_wt_managed_cfg_t, on_activity));
    prefix.struct_size = old_size;

    char dirty[sizeof(moq_proxygen_wt_managed_cfg_t)];
    std::memset(dirty, 0xFF, sizeof(dirty));
    std::memcpy(dirty, &prefix, old_size);   /* tail stays 0xFF */
    moq_proxygen_wt_managed_cfg_t *old =
        (moq_proxygen_wt_managed_cfg_t *)dirty;

    moq_proxygen_wt_managed_t *m = NULL;
    /* host is NULL within the prefix -> INVAL, and the 0xFF tail (alpn_count
     * etc.) must NOT be read (would otherwise look like a huge alpn_count). */
    CHECK(moq_proxygen_wt_managed_create(old, &m) == MOQ_ERR_INVAL);
}

/* A caller built before `target` was appended: its struct ends at the
 * layout through goaway_timeout_us. The initializer symbol it calls clears
 * and stamps exactly that, and create() reads nothing past it -- the poisoned
 * bytes there would be a wild target pointer. */
static void test_target_old_caller(void)
{
    constexpr size_t end = offsetof(moq_proxygen_wt_managed_cfg_t, goaway_timeout_us) +
                           sizeof(uint64_t);
    constexpr size_t align = alignof(moq_proxygen_wt_managed_cfg_t);
    constexpr size_t old = (end + align - 1) & ~(align - 1);
    static_assert(offsetof(moq_proxygen_wt_managed_cfg_t, target) >= old,
                  "target starts past the sizeof through goaway_timeout_us");

    unsigned char buf[sizeof(moq_proxygen_wt_managed_cfg_t) + 16];
    std::memset(buf, 0xA5, sizeof(buf));
    (moq_proxygen_wt_managed_cfg_init)(
        reinterpret_cast<moq_proxygen_wt_managed_cfg_t *>(buf));
    uint32_t stamped;
    std::memcpy(&stamped, buf, sizeof(stamped));
    CHECK(stamped == old);
    for (size_t i = old; i < sizeof(buf); i++) CHECK(buf[i] == 0xA5);

    moq_proxygen_wt_managed_cfg_t c = base_cfg();
    c.host = NULL;               /* -> INVAL before any connect attempt */
    std::memset(reinterpret_cast<unsigned char *>(&c) + end, 0xA5, sizeof(c) - end);
    c.struct_size = static_cast<uint32_t>(old);
    moq_proxygen_wt_managed_t *m = NULL;
    CHECK(moq_proxygen_wt_managed_create(&c, &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);
}

int main(void)
{
    test_cfg_init();
    test_invalid_config();
    test_alpn_offer();
    test_old_struct_size_abi();
    test_target_old_caller();
    std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
