/*
 * Managed MsQuic servers that an IPv4 loopback client can reach.
 *
 * MsQuic binds every listener to the dual-stack IPv6 wildcard, so a port
 * picked for that bind can be one an IPv4-only socket holds. The facade's
 * port 0 already accounts for it (adapters/common/moq_listen_port.h): this
 * helper creates the server on port 0, the way an application does, and
 * writes the port it listens on back into cfg->port.
 */
#ifndef MOQ_MSQ_LOOPBACK_PORT_H
#define MOQ_MSQ_LOOPBACK_PORT_H

#include <moq/msquic_managed.h>

/*
 * moq_msquic_managed_create() for a SERVER on port 0, with cfg->port set to
 * the port it listens on. Returns the create result.
 */
static inline moq_result_t msqt_server_create(moq_msquic_managed_cfg_t *cfg,
                                              moq_msquic_managed_t **out)
{
    moq_result_t rc;

    cfg->port = 0;
    *out = NULL;
    rc = moq_msquic_managed_create(cfg, out);
    if (rc != MOQ_OK)
        *out = NULL;
    else
        cfg->port = moq_msquic_managed_port(*out);
    return rc;
}

#endif /* MOQ_MSQ_LOOPBACK_PORT_H */
