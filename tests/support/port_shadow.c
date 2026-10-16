/*
 * moq_port_shadow: hold IPv4-only UDP sockets on a large share of the
 * ephemeral port range while a loopback test runs.
 *
 *   moq_port_shadow [-n COUNT] -- COMMAND [ARGS...]
 *   moq_port_shadow [-n COUNT] --hold
 *
 * The first form holds the ports, runs COMMAND as a child process and exits
 * with its status (128 + signal when it was killed). The second form holds
 * the ports until SIGINT or SIGTERM, for runs that start many tests under one
 * holder; it prints "held N" on stdout once the ports are held.
 *
 * The sockets are what a system daemon's udp4 *:P socket is: an IPv4-only
 * wildcard bind. A dual-stack IPv6 listener can be handed one of these ports
 * on kernels that do not check IPv4 sockets when they pick a port for a
 * dual-stack socket (macOS), and an IPv4 client dialling 127.0.0.1:P then
 * reaches the holder instead of the listener. Under this fixture a test that
 * lets such a listener pick its own port fails often instead of rarely.
 *
 * The holder is a separate process from the test, as the daemon is, and its
 * sockets are close-on-exec so the test does not inherit thousands of
 * descriptors.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_COUNT 4000
#define MIN_HELD 1000

static volatile sig_atomic_t g_stop;
static volatile pid_t g_child = -1;

static void on_signal(int sig)
{
    g_stop = 1;
    if (g_child > 0)
        (void)kill(g_child, sig);
}

static void raise_nofile(int want)
{
    struct rlimit rl;
    rlim_t need = (rlim_t)want + 256;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur >= need)
        return;
    rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= need)
                      ? need : rl.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &rl);
}

/* Bind up to `want` IPv4 wildcard UDP sockets on ephemeral ports. */
static int hold_ports(int want)
{
    int held = 0;

    raise_nofile(want);
    while (held < want) {
        struct sockaddr_in sin;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);

        if (fd < 0)
            break;
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = 0;
        if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) != 0) {
            close(fd);
            break;
        }
        held++;
    }
    return held;
}

static int usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-n COUNT] -- COMMAND [ARGS...]\n"
            "       %s [-n COUNT] --hold\n", argv0, argv0);
    return 2;
}

int main(int argc, char **argv)
{
    int count = DEFAULT_COUNT;
    int i = 1;
    int hold_only = 0;

    while (i < argc) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "--hold") == 0) {
            hold_only = 1;
            i++;
            break;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            return usage(argv[0]);
        }
    }
    if (count < MIN_HELD || (!hold_only && i >= argc))
        return usage(argv[0]);

    int held = hold_ports(count);
    if (held < MIN_HELD) {
        fprintf(stderr, "port_shadow: only %d IPv4 ports held (need %d)\n",
                held, MIN_HELD);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    if (hold_only) {
        printf("held %d\n", held);
        fflush(stdout);
        while (!g_stop)
            pause();
        return 0;
    }

    fprintf(stderr, "port_shadow: %d IPv4 ports held\n", held);
    pid_t pid = fork();
    if (pid < 0) {
        perror("port_shadow: fork");
        return 2;
    }
    if (pid == 0) {
        execvp(argv[i], &argv[i]);
        perror("port_shadow: exec");
        _exit(127);
    }
    g_child = pid;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("port_shadow: waitpid");
            return 2;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 2;
}
