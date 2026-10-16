#ifndef MEDIA_RELAY_NATIVE_HOST_H
#define MEDIA_RELAY_NATIVE_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct media_relay_native_host media_relay_native_host_t;

/* One host per process. Success follows the production listener-ready flush,
 * not a delay. A port collision is a setup failure, never retried. */
int media_relay_native_host_start(const char *cert, const char *key,
                                 unsigned draft, bool webtransport,
                                 media_relay_native_host_t **out);
uint16_t media_relay_native_host_port(const media_relay_native_host_t *host,
                                    bool webtransport);
bool media_relay_native_host_running(const media_relay_native_host_t *host);
/* Stops and joins the owned serve thread, whose production teardown joins
 * its managed workers. Returns the serve result. NULL is harmless. */
int media_relay_native_host_stop(media_relay_native_host_t *host);

#endif
