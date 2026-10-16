#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "media_relay_native_host.h"
#include "config.h"
#include "logevent.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Linked to the existing fill_deadline_cli_tu.c, not a substitute relay. */
extern int moqr_test_cmd_serve(const moqr_cli_config_t *cfg);
extern int moqr_test_cmd_serve_lanes(const moqr_cli_config_t *cfg);
extern void moqr_test_serve_log_set_io(const moqr_cli_log_io_t *io);
extern void moqr_test_raise_stop(void);

struct media_relay_native_host {
    moqr_cli_config_t cfg;
    moqr_cli_log_io_t io;
    pthread_t thread;
    atomic_bool ready;
    atomic_bool done;
    int result;
};

static uint64_t host_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)ts.tv_nsec / 1000;
}

static size_t host_write(void *ctx, FILE *f, const char *p, size_t n)
{
    (void)ctx;
    return fwrite(p, 1, n, f);
}

static int host_flush(void *ctx, FILE *f)
{
    media_relay_native_host_t *h = ctx;
    int rc = fflush(f);
    /* TEXT has exactly one sink flush, after ALL listener creations and
     * signal setup succeed. Startup errors return without that flush. */
    if (rc == 0) atomic_store(&h->ready, true);
    return rc;
}

static void *host_serve(void *ctx)
{
    media_relay_native_host_t *h = ctx;
    h->result = h->cfg.wt.enabled ? moqr_test_cmd_serve_lanes(&h->cfg)
                                 : moqr_test_cmd_serve(&h->cfg);
    atomic_store(&h->done, true);
    return NULL;
}

static int reserve_port(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET,
                            .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } };
    socklen_t size = sizeof(a);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &size) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

int media_relay_native_host_start(const char *cert, const char *key,
                                 unsigned draft, bool webtransport,
                                 media_relay_native_host_t **out)
{
    *out = NULL;
    if ((draft != 18 && draft != 22) || strlen(cert) >= 1024 ||
        strlen(key) >= 1024) return 1;
    media_relay_native_host_t *h = calloc(1, sizeof(*h));
    if (h == NULL) return 1;
    atomic_init(&h->ready, false);
    atomic_init(&h->done, false);
    uint16_t raw_port = 0, wt_port = 0;
    int raw_fd = reserve_port(&raw_port);
    int wt_fd = webtransport ? reserve_port(&wt_port) : -1;
    char json[512], error[256] = {0};
    int n = snprintf(json, sizeof(json),
        "{\"listener\":{\"host\":\"127.0.0.1\",\"port\":%u,"
        "\"cert\":\"fixture\",\"key\":\"fixture\",\"lanes\":1,"
        "\"versions\":[%u]}%s",
        (unsigned)raw_port, draft, webtransport ? "," : "}");
    if (webtransport && n > 0 && (size_t)n < sizeof(json)) {
        int tail = snprintf(json + n, sizeof(json) - (size_t)n,
            "\"webtransport\":{\"host\":\"127.0.0.1\",\"port\":%u,"
            "\"cert\":\"fixture\",\"key\":\"fixture\",\"lanes\":1,"
            "\"versions\":[%u]}}", (unsigned)wt_port, draft);
        n = tail < 0 ? -1 : n + tail;
    }
    bool configured = raw_fd >= 0 && (!webtransport || wt_fd >= 0) &&
        n > 0 && (size_t)n < sizeof(json) &&
        moqr_cli_config_parse(json, (size_t)n, &h->cfg, error,
                              sizeof(error)) == MOQR_OK;
    /* The CLI accepts fixed ports only. Keep both reservations until the
     * config is ready; listener creation must then acquire them itself. */
    if (raw_fd >= 0) close(raw_fd);
    if (wt_fd >= 0) close(wt_fd);
    if (!configured) {
        fprintf(stderr, "SETUP relay configuration: %s\n", error);
        free(h);
        return 1;
    }
    memcpy(h->cfg.cert, cert, strlen(cert) + 1);
    memcpy(h->cfg.key, key, strlen(key) + 1);
    memcpy(h->cfg.wt.cert, cert, strlen(cert) + 1);
    memcpy(h->cfg.wt.key, key, strlen(key) + 1);
    h->io = (moqr_cli_log_io_t){ .write = host_write, .flush = host_flush,
                                .ctx = h };
    moqr_test_serve_log_set_io(&h->io);
    if (pthread_create(&h->thread, NULL, host_serve, h) != 0) {
        moqr_test_serve_log_set_io(NULL);
        free(h);
        return 1;
    }
    uint64_t deadline = host_now_us() + UINT64_C(15000000);
    while (!atomic_load(&h->ready) && !atomic_load(&h->done) &&
           host_now_us() < deadline) {
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    if (!atomic_load(&h->ready) || atomic_load(&h->done)) {
        fprintf(stderr, "SETUP relay failed before listener readiness\n");
        (void)media_relay_native_host_stop(h);
        return 1;
    }
    *out = h;
    return 0;
}

uint16_t media_relay_native_host_port(const media_relay_native_host_t *h,
                                    bool webtransport)
{
    return (uint16_t)(webtransport ? h->cfg.wt.port : h->cfg.port);
}

bool media_relay_native_host_running(const media_relay_native_host_t *h)
{
    return h != NULL && atomic_load(&h->ready) && !atomic_load(&h->done);
}

int media_relay_native_host_stop(media_relay_native_host_t *h)
{
    if (h == NULL) return 0;
    moqr_test_raise_stop();
    if (pthread_join(h->thread, NULL) != 0) abort();
    int rc = h->result;
    moqr_test_serve_log_set_io(NULL);
    free(h);
    return rc;
}
