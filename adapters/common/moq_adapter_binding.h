#ifndef MOQ_ADAPTER_COMMON_BINDING_H
#define MOQ_ADAPTER_COMMON_BINDING_H

/*
 * Internal adapter helper: bind a session to the transport a managed facade
 * owns, when the session's wire profile requires it.
 *
 * NOT a public API: header-only, no MOQ_API, not installed. A managed facade
 * knows how its session is carried (natively over QUIC or inside a
 * WebTransport session), whether the handshake has completed and what the
 * two sides negotiated for QUIC DATAGRAM; it supplies those facts here, with
 * the connection target it was configured with (or one derived from the dial
 * parameters), before it starts the session. A profile that requires no
 * facts is bound whenever the facade has a target to give it, so a raw-QUIC
 * client names its target in its setup on every profile.
 */

#include <moq/connection_target.h>
#include <moq/session.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A raw-QUIC server configured without a URI policy serves every authority
 * and path a client names. */
static inline uint32_t moq_adapter_accept_any_uri(void *ctx, uint32_t present,
                                                  moq_bytes_t authority,
                                                  moq_bytes_t path_query)
{
    (void)ctx; (void)present; (void)authority; (void)path_query;
    return MOQ_URI_ACCEPT;
}

/*
 * Write the RFC 3986 authority "host:port" for a dial target into buf,
 * NUL-terminated, bracketing an IPv6 literal. The port is always explicit:
 * a dial carries a port, not a URI spelling to preserve. Returns the
 * length, or -1 when host is empty or buf is too small.
 */
static inline int moq_adapter_dial_authority(char *buf, size_t cap,
                                             const char *host, int port)
{
    if (!buf || !host || !host[0]) return -1;
    bool ipv6 = strchr(host, ':') != NULL && host[0] != '[';
    int n = snprintf(buf, cap, "%s%s%s:%d", ipv6 ? "[" : "", host,
                     ipv6 ? "]" : "", port);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

/*
 * Derive a client's connection target from its dial parameters: the
 * authority is "host:port" (an IPv6 literal bracketed), the path/query the
 * WebTransport path or empty. The spelling is written into buf; the target
 * borrows it. Returns false when buf is too small or host is empty.
 */
static inline bool moq_adapter_target_from_dial(char *buf, size_t cap,
                                                const char *host, int port,
                                                const char *path,
                                                moq_connection_target_t *out)
{
    if (!out) return false;
    int n = moq_adapter_dial_authority(buf, cap, host, port);
    if (n < 0) return false;
    size_t alen = (size_t)n;
    size_t plen = path ? strlen(path) : 0;
    if (plen >= cap - alen - 1) return false;
    if (plen) memcpy(buf + alen + 1, path, plen);
    buf[alen + 1 + plen] = '\0';
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->target_present = MOQ_BINDING_TARGET_COMPLETE;
    out->authority.data = (const uint8_t *)buf;
    out->authority.len = alen;
    out->path_query.data = (const uint8_t *)(buf + alen + 1);
    out->path_query.len = plen;
    return true;
}

/*
 * The :authority a WebTransport client's extended CONNECT names, written
 * NUL-terminated into buf: the target's authority as spelled when it has one
 * (the authority of the https URI the CONNECT dereferences -- RFC 9114
 * Section 4.3.1, draft-ietf-moq-transport-22 Section 6.2.1), else
 * moq_adapter_dial_authority(host, port). It is never the TLS server name:
 * SNI carries a bare DNS name, while :authority keeps the port and an IPv6
 * literal's brackets, and a server that routes by authority compares it
 * exactly. The CONNECT and the session's transport binding therefore name
 * the same authority. Returns false when buf is too small, host is empty
 * with no target authority, or the target authority embeds a NUL.
 */
static inline bool moq_adapter_connect_authority(
    char *buf, size_t cap, const moq_connection_target_t *target,
    const char *host, int port)
{
    if (!buf || cap == 0) return false;
    if (target && target->authority.len) {
        size_t n = target->authority.len;
        if (!target->authority.data || n >= cap ||
            memchr(target->authority.data, 0, n))
            return false;
        memcpy(buf, target->authority.data, n);
        buf[n] = '\0';
        return true;
    }
    return moq_adapter_dial_authority(buf, cap, host, port) >= 0;
}

/*
 * Bind `s` to its transport. `kind` is MOQ_BINDING_RAW_QUIC or
 * MOQ_BINDING_WEBTRANSPORT_H3; the fact masks are what the facade knows about
 * this connection; `target` carries the client's (or a WebTransport
 * session's) complete authority and path, or the raw server's policy (NULL
 * means none). A profile that requires facts is always bound. One that
 * requires none is bound when the target is one of those two shapes and left
 * unbound (MOQ_OK) otherwise, so a facade with nothing to name keeps the
 * profile's unbound behavior. The result is moq_session_bind_transport()'s.
 */
static inline moq_result_t moq_adapter_bind_session(
    moq_session_t *s, uint32_t kind, uint32_t facts_known,
    uint32_t facts_supported, const moq_connection_target_t *target)
{
    if (moq_session_binding_requirements(s) == 0) {
        bool named = target && !target->accept_uri && !target->accept_uri_ctx &&
                     target->target_present == MOQ_BINDING_TARGET_COMPLETE;
        bool policy = target && kind == MOQ_BINDING_RAW_QUIC &&
                      target->accept_uri && !target->target_present;
        if (!named && !policy) return MOQ_OK;
    }
    moq_transport_binding_t b;
    moq_transport_binding_init(&b);
    b.kind = kind;
    b.facts_known = facts_known;
    b.facts_supported = facts_supported;
    if (target) {
        b.target_present = target->target_present;
        b.authority = target->authority;
        b.path_query = target->path_query;
        b.accept_uri = target->accept_uri;
        b.accept_uri_ctx = target->accept_uri_ctx;
    }
    return moq_session_bind_transport(s, &b, sizeof(b));
}

/*
 * Copy a caller's connection target for a facade's lifetime: the two spans
 * land in one allocation `*out_bytes`; the policy pointers are borrowed.
 * A NULL input yields an empty target. Validates the v0 floor and presence
 * mask; returns MOQ_ERR_INVAL for a malformed target, MOQ_ERR_NOMEM when the
 * copy fails.
 */
static inline moq_result_t moq_adapter_target_copy(
    const moq_alloc_t *alloc, const moq_connection_target_t *in,
    moq_connection_target_t *out, uint8_t **out_bytes, size_t *out_bytes_len)
{
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    *out_bytes = NULL;
    *out_bytes_len = 0;
    if (!in) return MOQ_OK;
    if (in->struct_size < MOQ_CONNECTION_TARGET_V0_SIZE) return MOQ_ERR_INVAL;
    if (in->target_present & ~MOQ_BINDING_TARGET_COMPLETE) return MOQ_ERR_INVAL;
    if ((in->authority.len && !in->authority.data) ||
        (in->path_query.len && !in->path_query.data))
        return MOQ_ERR_INVAL;
    if (in->path_query.len > SIZE_MAX - in->authority.len)
        return MOQ_ERR_INVAL;
    size_t n = in->authority.len + in->path_query.len;
    uint8_t *copy = NULL;
    if (n) {
        copy = (uint8_t *)alloc->alloc(n, alloc->ctx);
        if (!copy) return MOQ_ERR_NOMEM;
        if (in->authority.len)
            memcpy(copy, in->authority.data, in->authority.len);
        if (in->path_query.len)
            memcpy(copy + in->authority.len, in->path_query.data,
                   in->path_query.len);
    }
    out->target_present = in->target_present;
    out->authority.data = in->authority.len ? copy : NULL;
    out->authority.len = in->authority.len;
    out->path_query.data = in->path_query.len ? copy + in->authority.len : NULL;
    out->path_query.len = in->path_query.len;
    out->accept_uri = in->accept_uri;
    out->accept_uri_ctx = in->accept_uri_ctx;
    *out_bytes = copy;
    *out_bytes_len = n;
    return MOQ_OK;
}

/*
 * A WebTransport client's one CONNECT identity, owned for the facade's
 * lifetime: the target it binds and the :authority and :path its extended
 * CONNECT sends are the same URI.
 *
 * Precedence. An explicit target (`in`) is the identity: it must be
 * complete, name an authority and carry no URI policy. Its bytes are kept as
 * spelled -- nothing is decoded, re-escaped or normalized. A path configured
 * alongside it (`cfg_path` non-NULL) must equal the target's :path
 * projection byte for byte; a conflict is MOQ_ERR_INVAL, never a choice
 * between two resources. Without a target the identity is derived from the
 * dial parameters: authority host:port (moq_adapter_dial_authority), path
 * `cfg_path`, else `default_path`.
 *
 * :path projection. An https URI with no path component sends ":path" "/"
 * (RFC 9114 Section 4.3.1); a query-only path/query keeps its query after
 * that "/" (RFC 9110 Section 4.2.3, RFC 3986 Section 6.2.3: the empty path
 * of an http(s) URI is "/"). The bound target keeps the URI's own path/query.
 *
 * Bound. The identity is refused (MOQ_ERR_INVAL) above the transport
 * binding's bound -- authority plus path/query at most 65535 bytes -- and
 * when a component embeds a NUL, so a failure is reported here, before any
 * transport effect, and never becomes an unbound session.
 *
 * Storage. One allocation `*out_bytes` (`*out_bytes_len` bytes) holds
 * "authority\0" and the NUL-terminated :path; `out`'s spans point into it and
 * `*out_authority` / `*out_path` are the C strings the CONNECT sends.
 * MOQ_ERR_NOMEM when it cannot be allocated; on any error nothing is owned.
 */
static inline moq_result_t moq_adapter_wt_client_target(
    const moq_alloc_t *alloc, const moq_connection_target_t *in,
    const char *host, int port, const char *cfg_path,
    const char *default_path, moq_connection_target_t *out,
    uint8_t **out_bytes, size_t *out_bytes_len, const char **out_authority,
    const char **out_path)
{
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    *out_bytes = NULL;
    *out_bytes_len = 0;
    *out_authority = NULL;
    *out_path = NULL;

    char dial[300];
    const uint8_t *auth;
    size_t alen;
    const uint8_t *pq;
    size_t plen;
    if (in) {
        if (in->struct_size < MOQ_CONNECTION_TARGET_V0_SIZE ||
            in->target_present != MOQ_BINDING_TARGET_COMPLETE ||
            in->accept_uri || in->accept_uri_ctx ||
            !in->authority.len || !in->authority.data ||
            (in->path_query.len && !in->path_query.data))
            return MOQ_ERR_INVAL;
        auth = in->authority.data;
        alen = in->authority.len;
        pq = in->path_query.data;
        plen = in->path_query.len;
    } else {
        /* An over-long host is no DNS name or IP literal (RFC 1035: 253
         * octets); refusing it keeps the derivation in bounded storage. */
        int n = moq_adapter_dial_authority(dial, sizeof(dial), host, port);
        if (n < 0) return MOQ_ERR_INVAL;
        auth = (const uint8_t *)dial;
        alen = (size_t)n;
        const char *p = cfg_path ? cfg_path : default_path;
        pq = (const uint8_t *)p;
        plen = p ? strlen(p) : 0;
    }
    if (alen > UINT16_MAX || plen > UINT16_MAX - alen ||
        memchr(auth, 0, alen) || (plen && memchr(pq, 0, plen)))
        return MOQ_ERR_INVAL;

    /* :path is the path/query, with "/" in front of an empty path. */
    size_t slash = (!plen || pq[0] == '?') ? 1 : 0;
    if (in && cfg_path) {
        size_t clen = strlen(cfg_path);
        if (clen != slash + plen ||
            (slash && cfg_path[0] != '/') ||
            (plen && memcmp(cfg_path + slash, pq, plen) != 0))
            return MOQ_ERR_INVAL;
    }

    size_t n = alen + 1 + slash + plen + 1;
    uint8_t *b = (uint8_t *)alloc->alloc(n, alloc->ctx);
    if (!b) return MOQ_ERR_NOMEM;
    memcpy(b, auth, alen);
    b[alen] = 0;
    uint8_t *path = b + alen + 1;
    if (slash) path[0] = '/';
    if (plen) memcpy(path + slash, pq, plen);
    path[slash + plen] = 0;

    out->target_present = MOQ_BINDING_TARGET_COMPLETE;
    out->authority.data = b;
    out->authority.len = alen;
    out->path_query.data = plen ? path + slash : NULL;
    out->path_query.len = plen;
    *out_bytes = b;
    *out_bytes_len = n;
    *out_authority = (const char *)b;
    *out_path = (const char *)path;
    return MOQ_OK;
}

#endif /* MOQ_ADAPTER_COMMON_BINDING_H */
