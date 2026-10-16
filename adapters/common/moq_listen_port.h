/*
 * The port an MsQuic-backed server listens on when it is configured with
 * port 0.
 *
 * MsQuic binds every listener to the dual-stack IPv6 wildcard, whatever
 * address it is asked to listen on, and filters on that address itself. Left
 * to pick the port, that bind takes whatever the kernel hands a dual-stack
 * socket, and some kernels (macOS) do not check IPv4-only sockets when they
 * do: the listener can land on a port an IPv4-only socket (a system daemon's
 * udp4 *:P, say) already holds, and every IPv4 datagram for that port then
 * reaches the other socket instead of the listener. IPv6 clients connect;
 * IPv4 clients time out.
 *
 * Port 0 means a port IPv4 and IPv6 clients both reach the listener on, so
 * the facade picks it from the IPv4 allocator instead: a port that allocator
 * returns is held by no IPv4 socket, and the IPv4 allocator also skips ports
 * a dual-stack socket holds. The reservation is released before the listener
 * binds, so a start that fails in that window is retried on a fresh port, at
 * most MOQ_LISTEN_PORT_ATTEMPTS times in all. A host with no IPv4 stack has
 * no IPv4 socket to collide with; there the transport picks the port itself.
 *
 * An explicit nonzero port is never replaced or retried.
 *
 * Header-only and internal to the adapters (never installed).
 */
#ifndef MOQ_ADAPTER_LISTEN_PORT_H
#define MOQ_ADAPTER_LISTEN_PORT_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define MOQ_LISTEN_PORT_ATTEMPTS 8

/*
 * The port one listener start should ask for. `configured` nonzero is
 * returned as is. For 0, *port is a UDP port no IPv4 socket holds right now,
 * or 0 on a host with no IPv4 stack (the transport then picks). Returns false
 * when no port could be reserved; that start attempt fails.
 */
static inline bool moq_listen_port_pick(uint16_t configured, uint16_t *port)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    bool ok = false;
    int fd;

    *port = configured;
    if (configured != 0)
        return true;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return errno == EAFNOSUPPORT;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = 0;
    if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) == 0 &&
        getsockname(fd, (struct sockaddr *)&sin, &len) == 0 &&
        ntohs(sin.sin_port) != 0) {
        *port = ntohs(sin.sin_port);
        ok = true;
    }
    close(fd);
    return ok;
}

/* Whether a failed start on a port from moq_listen_port_pick is retried. */
static inline bool moq_listen_port_retry(uint16_t configured, int attempt)
{
    return configured == 0 && attempt + 1 < MOQ_LISTEN_PORT_ATTEMPTS;
}

#endif /* MOQ_ADAPTER_LISTEN_PORT_H */
