/*
 * wtquic-over-MsQuic listeners that an IPv4 loopback client can reach.
 *
 * MsQuic binds every listener to the dual-stack IPv6 wildcard, so a listener
 * started on port 0 can land on a port an IPv4-only socket holds, and a
 * client dialling 127.0.0.1 then reaches that socket instead (see
 * tests/support/loopback_port.h). wtquic's own listener leaves port 0 to
 * MsQuic, so its helper starts it on a port from the IPv4 allocator instead,
 * retrying on a fresh port when the start fails; the managed facade picks
 * its port 0 that way itself. Each helper is defined when its API's header
 * was included first: <wtquic/wtquic_msquic.h> for the listener,
 * <moq/wtquic_msquic_managed.h> for the managed server.
 */
#ifndef MOQ_WTQ_LOOPBACK_PORT_H
#define MOQ_WTQ_LOOPBACK_PORT_H

#include "loopback_port.h"

#ifdef WTQ_WTQUIC_MSQUIC_H
typedef struct wtqt_listen {
    wtq_msquic_env_t *env;
    wtq_msquic_listener_cfg_t *cfg;
    wtq_msquic_listener_t **out;
    wtq_result_t rc;
} wtqt_listen_t;

static inline bool wtqt_listen_start(void *ctx, uint16_t port)
{
    wtqt_listen_t *l = (wtqt_listen_t *)ctx;

    l->cfg->port = port;
    *l->out = NULL;
    l->rc = wtq_msquic_listener_start(l->env, l->cfg, l->out);
    if (l->rc != WTQ_OK)
        *l->out = NULL;
    return l->rc == WTQ_OK;
}

/*
 * wtq_msquic_listener_start() with cfg->port replaced by the port it
 * listens on. Returns the last start result.
 */
static inline wtq_result_t wtqt_listener_start(wtq_msquic_env_t *env,
                                               wtq_msquic_listener_cfg_t *cfg,
                                               wtq_msquic_listener_t **out)
{
    wtqt_listen_t l = { env, cfg, out, WTQ_ERR_INVALID_ARG };

    (void)moq_test_listen_ipv4_port(wtqt_listen_start, &l);
    return l.rc;
}
#endif

#ifdef MOQ_WTQUIC_MSQUIC_MANAGED_H
/*
 * moq_wtquic_msquic_managed_create() for a SERVER on port 0, with cfg->port
 * set to the port it listens on. The facade's port 0 is already reachable
 * over IPv4 (adapters/common/moq_listen_port.h), so this creates the server
 * the way an application does. Returns the create result.
 */
static inline moq_result_t
wtqt_managed_server_create(moq_wtquic_msquic_managed_cfg_t *cfg,
                           moq_wtquic_msquic_managed_t **out)
{
    moq_result_t rc;

    cfg->port = 0;
    *out = NULL;
    rc = moq_wtquic_msquic_managed_create(cfg, out);
    if (rc != MOQ_OK)
        *out = NULL;
    else
        cfg->port = moq_wtquic_msquic_managed_port(*out);
    return rc;
}
#endif

#endif /* MOQ_WTQ_LOOPBACK_PORT_H */
