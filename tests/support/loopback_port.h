/*
 * Listener ports for loopback tests whose client dials 127.0.0.1.
 *
 * A listener that binds the dual-stack IPv6 wildcard (MsQuic binds every
 * listener that way, whatever address it is asked to listen on) and asks for
 * port 0 can be handed a port that an IPv4-only socket already holds: some
 * kernels (macOS) do not check IPv4 sockets when they pick a port for a
 * dual-stack socket. A system daemon's udp4 *:P socket then receives every
 * IPv4 datagram sent to 127.0.0.1:P, and the client's handshake never reaches
 * the listener.
 *
 * A port the IPv4 allocator returns is held by no IPv4 socket, and once a
 * dual-stack listener holds it no IPv4 socket can bind it. The reservation is
 * released before the listener binds, so a listener start that fails in that
 * window is retried on a fresh port.
 *
 * Stacks that bind an IPv4 socket for an IPv4 listen address take their port
 * from the IPv4 allocator already and do not need this.
 *
 * Header-only; usable from C and C++. tests/support/port_shadow.c is the
 * fixture that makes the hazard reproducible.
 */
#ifndef MOQ_TEST_LOOPBACK_PORT_H
#define MOQ_TEST_LOOPBACK_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define MOQ_TEST_LISTEN_ATTEMPTS 8

/* A UDP port no IPv4 socket holds right now; 0 when none could be reserved. */
static inline uint16_t moq_test_ipv4_free_port(void)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    uint16_t port = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return 0;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = 0;
    if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) == 0 &&
        getsockname(fd, (struct sockaddr *)&sin, &len) == 0)
        port = ntohs(sin.sin_port);
    close(fd);
    return port;
}

/* Starts a listener on `port`; true when it is listening. */
typedef bool (*moq_test_listen_fn)(void *ctx, uint16_t port);

/*
 * Start a listener on a port from the IPv4 allocator, retrying on a fresh
 * port when the start fails. Returns the port it listens on, or 0 when every
 * attempt failed (the last failure is left in whatever `start` records).
 */
static inline uint16_t moq_test_listen_ipv4_port(moq_test_listen_fn start,
                                                 void *ctx)
{
    for (int attempt = 0; attempt < MOQ_TEST_LISTEN_ATTEMPTS; attempt++) {
        uint16_t port = moq_test_ipv4_free_port();

        if (port != 0 && start(ctx, port))
            return port;
    }
    return 0;
}

#endif /* MOQ_TEST_LOOPBACK_PORT_H */
