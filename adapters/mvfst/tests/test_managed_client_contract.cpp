/*
 * Managed mvfst client contract.
 *
 * Session lifetime and the configured version vector use the adapter-local
 * test-internals rail without network traffic. The actual offer check captures
 * the first client Initial over loopback UDP, without a responding TLS server.
 *
 *  - stop() must join the managed thread without freeing the client session;
 *    the service tier may still hold session-backed publisher state and destroy
 *    it after the endpoint has gone terminal.
 *  - the client facade must offer standard QUIC v1, not mvfst's default
 *    private version-first offer.
 */
#include <moq/mvfst.h>

#include "../src/mvfst_managed_testing.h"

#include <quic/codec/Decode.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int g_fail = 0;

#define TEST_CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } \
} while (0)

static int noop_pump(moq_mvfst_managed_t *, moq_mvfst_managed_lane_t *,
                     uint64_t, void *)
{
    return 0;
}

static moq_mvfst_managed_t *create_client(const char *host, int port)
{
    moq_mvfst_managed_cfg_t cfg;
    moq_mvfst_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg.host = host;
    cfg.port = port;
    cfg.insecure_skip_verify = true;
    cfg.on_lane_pump = noop_pump;

    moq_mvfst_managed_t *m = nullptr;
    if (moq_mvfst_managed_create(&cfg, &m) != MOQ_OK) return nullptr;
    return m;
}

static void test_session_survives_stop()
{
    int before = g_fail;
    moq_mvfst_managed_t *m = create_client(nullptr, 0);
    TEST_CHECK(m != nullptr);
    if (!m) return;

    TEST_CHECK(moq_mvfst_managed_test_has_client_session(m));
    TEST_CHECK(moq_mvfst_managed_stop(m) == MOQ_OK);
    TEST_CHECK(moq_mvfst_managed_test_has_client_session(m));
    moq_mvfst_managed_destroy(m);

    if (g_fail == before)
        std::printf("PASS: mvfst_client_session_survives_stop\n");
}

static void test_client_configured_version_vector()
{
    int before = g_fail;
    moq_mvfst_managed_t *m = create_client(nullptr, 0);
    TEST_CHECK(m != nullptr);
    if (!m) return;

    uint32_t versions[4] = {0, 0, 0, 0};
    size_t n = moq_mvfst_managed_test_client_quic_versions(
        m, versions, sizeof(versions) / sizeof(versions[0]));
    TEST_CHECK(n == 1);
    TEST_CHECK(versions[0] == 0x00000001u);

    moq_mvfst_managed_destroy(m);

    if (g_fail == before)
        std::printf("PASS: mvfst_client_configured_version_vector\n");
}

struct InitialCapture {
    int fd = -1;
    moq_mvfst_managed_t *client = nullptr;

    InitialCapture() = default;
    InitialCapture(const InitialCapture &) = delete;
    InitialCapture &operator=(const InitialCapture &) = delete;

    ~InitialCapture()
    {
        if (client) {
            TEST_CHECK(moq_mvfst_managed_stop(client) == MOQ_OK);
            moq_mvfst_managed_destroy(client);
        }
        if (fd >= 0) TEST_CHECK(close(fd) == 0);
    }
};

static bool socket_failure(const char *operation)
{
    std::perror(operation);
    ++g_fail;
    return false;
}

static bool receive_first_datagram(int fd, uint8_t *data, size_t capacity,
                                   size_t &length)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::fprintf(stderr, "FAIL: no client Initial before capture deadline\n");
            ++g_fail;
            return false;
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, static_cast<int>(remaining) + 1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return socket_failure("FAIL: Initial capture poll");
        }
        if (ready == 0) continue;
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (pfd.revents & POLLIN) == 0) {
            std::fprintf(stderr, "FAIL: Initial capture poll revents=%d\n",
                         static_cast<int>(pfd.revents));
            ++g_fail;
            return false;
        }

        struct iovec iov = {};
        iov.iov_base = data;
        iov.iov_len = capacity;
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        const ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return socket_failure("FAIL: Initial capture recvmsg");
        }
        if (n == 0 || (msg.msg_flags & MSG_TRUNC) != 0 ||
            static_cast<size_t>(n) > capacity) {
            std::fprintf(stderr, "FAIL: empty or truncated first datagram\n");
            ++g_fail;
            return false;
        }
        length = static_cast<size_t>(n);
        return true;
    }
}

static void test_client_initial_offer()
{
    const int before = g_fail;
    {
        InitialCapture capture;
        capture.fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (capture.fd < 0) {
            socket_failure("FAIL: Initial capture socket (environment)");
            return;
        }
        const int flags = fcntl(capture.fd, F_GETFL, 0);
        if (flags < 0 || fcntl(capture.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            socket_failure("FAIL: Initial capture nonblocking mode");
            return;
        }
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(capture.fd, reinterpret_cast<const struct sockaddr *>(&addr),
                 sizeof(addr)) != 0) {
            socket_failure("FAIL: Initial capture bind (environment)");
            return;
        }
        socklen_t addr_len = sizeof(addr);
        if (getsockname(capture.fd, reinterpret_cast<struct sockaddr *>(&addr),
                        &addr_len) != 0) {
            socket_failure("FAIL: Initial capture getsockname");
            return;
        }
        /* Keep the bound socket open: no released-port race and no server
         * response that could change the client's initial version choice. */
        capture.client = create_client("127.0.0.1", ntohs(addr.sin_port));
        TEST_CHECK(capture.client != nullptr);
        if (!capture.client) return;

        std::array<uint8_t, 65536> packet{};
        size_t length = 0;
        if (!receive_first_datagram(capture.fd, packet.data(), packet.size(),
                                    length)) return;
        TEST_CHECK((packet[0] & 0xc0u) == 0xc0u);  /* long header and fixed bit */
        quic::ContiguousReadCursor cursor(packet.data() + 1, length - 1);
        auto parsed = quic::parseLongHeader(packet[0], cursor);
        TEST_CHECK(parsed.has_value());
        if (!parsed) return;
        TEST_CHECK(!parsed->isVersionNegotiation);
        TEST_CHECK(parsed->parsedLongHeader.has_value());
        if (!parsed->parsedLongHeader) return;
        const auto &initial = *parsed->parsedLongHeader;
        TEST_CHECK(initial.header.getHeaderType() == quic::LongHeader::Types::Initial);
        TEST_CHECK(initial.packetLength.packetLength > 0);
        TEST_CHECK(initial.packetLength.packetLength <= cursor.remaining());
        const uint32_t version = static_cast<uint32_t>(initial.header.getVersion());
        std::printf("OBSERVED: first client Initial version=0x%08" PRIx32 "\n",
                    version);
        TEST_CHECK(version == 0x00000001u);
    }
    if (g_fail == before)
        std::printf("PASS: mvfst_client_initial_offer_quic_v1\n");
}

int main()
{
    test_session_survives_stop();
    test_client_configured_version_vector();
    try {
        test_client_initial_offer();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: Initial capture exception: %s\n", e.what());
        ++g_fail;
    } catch (...) {
        std::fprintf(stderr, "FAIL: Initial capture unknown exception\n");
        ++g_fail;
    }

    if (g_fail == 0) {
        std::printf("PASS: mvfst_managed_client_contract\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_fail);
    return 1;
}
