#ifndef MOQ_TRANSPORT_BINDING_H
#define MOQ_TRANSPORT_BINDING_H

/*
 * Transport binding: the facts about the underlying connection that a wire
 * profile may need before its first message, told to the session once.
 *
 * The session core is sans-I/O and learns nothing about its transport on
 * its own. Every profile's setup depends on how the session is carried --
 * natively over QUIC or inside a WebTransport session: a raw-QUIC client
 * names its connection target (the URI authority, and the path with any
 * query), nothing else does, and a server refuses a target named over
 * WebTransport or one its URI policy does not serve. A profile that cannot
 * start without these facts declares them as required
 * (moq_session_binding_requirements(), nonzero for draft-22); the transport
 * host supplies them through moq_session_bind_transport() before the session
 * starts. A profile without requirements (draft-16, draft-18) accepts the
 * same binding optionally: bound, it behaves as above; never bound, it starts
 * and emits its setup as before, naming no target. Bound or not, a target
 * value outside URI syntax that a server receives closes the session.
 */

#include "export.h"
#include "types.h"
#include "session.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Binding kind: how the session is carried ----------------------- */
#define MOQ_BINDING_RAW_QUIC         UINT32_C(1)
#define MOQ_BINDING_WEBTRANSPORT_H3  UINT32_C(2)
#define MOQ_BINDING_WEBTRANSPORT_H2  UINT32_C(3)

/* -- Transport facts (bitmask) -------------------------------------- *
 * facts_known says which bits of facts_supported carry an answer; a bit set
 * in facts_supported must also be set in facts_known. */
#define MOQ_BINDING_HANDSHAKE_COMPLETE   UINT32_C(1)
#define MOQ_BINDING_LOCAL_QUIC_DATAGRAM  UINT32_C(2)
#define MOQ_BINDING_PEER_QUIC_DATAGRAM   UINT32_C(4)
#define MOQ_BINDING_DATAGRAM_SEND        UINT32_C(8)
#define MOQ_BINDING_DATAGRAM_RECEIVE     UINT32_C(16)
#define MOQ_BINDING_FACTS_ALL            UINT32_C(31)

/* -- Connection target presence (bitmask) --------------------------- *
 * Presence distinguishes an absent component from a present empty one. */
#define MOQ_BINDING_AUTHORITY_PRESENT  UINT32_C(1)
#define MOQ_BINDING_PATH_PRESENT       UINT32_C(2)
#define MOQ_BINDING_TARGET_COMPLETE    UINT32_C(3)

/* -- URI policy verdicts -------------------------------------------- */
#define MOQ_URI_ACCEPT             UINT32_C(0)
#define MOQ_URI_INVALID_AUTHORITY  UINT32_C(1)
#define MOQ_URI_INVALID_PATH       UINT32_C(2)

/*
 * A raw-QUIC server's URI policy: asked once per peer SETUP that the session
 * is about to accept, with the authority and path/query the peer named
 * (present per the `present` mask; syntax already validated). Serialized,
 * nonblocking, side-effect-free; it must not re-enter the session. The
 * session may ask again if accepting the SETUP has to be retried before it
 * commits. The slices are borrowed only for the call. A verdict other than
 * the three above fails closed (the session closes).
 */
typedef uint32_t (*moq_uri_accept_fn)(void *ctx, uint32_t present,
                                      moq_bytes_t authority,
                                      moq_bytes_t path_query);

typedef struct moq_transport_binding {
    uint32_t struct_size;
    uint32_t kind;             /* MOQ_BINDING_* */
    uint32_t facts_known;      /* MOQ_BINDING_* fact bits with an answer */
    uint32_t facts_supported;  /* answers; subset of facts_known */
    uint32_t target_present;   /* MOQ_BINDING_*_PRESENT bits */
    moq_bytes_t authority;     /* RFC 3986 authority, encoded spelling kept */
    moq_bytes_t path_query;    /* path-abempty [ "?" query ], encoded */
    moq_uri_accept_fn accept_uri;      /* raw-QUIC servers only */
    void *accept_uri_ctx;              /* borrowed until session destroy */
} moq_transport_binding_t;

/* Frozen through the last named v0 field, never the future sizeof: LP64 72
 * bytes, ILP32 44 bytes (Cortex-M33 included). No padding is a field. */
#define MOQ_TRANSPORT_BINDING_V0_SIZE \
    (offsetof(moq_transport_binding_t, accept_uri_ctx) + sizeof(void *))

/* Clears and stamps min(size, sizeof(moq_transport_binding_t)); a size below
 * the v0 floor can be initialized but never bound. */
MOQ_API void moq_transport_binding_init_sized(moq_transport_binding_t *binding,
                                              size_t size);
static inline void moq_transport_binding_init(moq_transport_binding_t *binding)
{ moq_transport_binding_init_sized(binding, sizeof(*binding)); }

/*
 * The fact mask a session's profile requires before it starts (zero: the
 * profile starts without a binding and accepts one optionally). Keyed by
 * session or by version; NULL or an unknown version returns 0. This describes
 * the profile, not the session's lifecycle state.
 */
MOQ_API uint32_t moq_session_binding_requirements(const moq_session_t *session);
MOQ_API uint32_t moq_version_binding_requirements(moq_version_t version);

/*
 * Bind the transport to a session that has not started and received no
 * input. Single-use. No borrow epoch, queue or cache effects. A profile
 * without requirements checks no facts: facts_known may be zero. The target
 * bytes are copied (the caller may release them on return); the policy
 * context is borrowed until the session is destroyed. No SNI or port is
 * inferred and nothing is normalized: the bytes are sent as given.
 *
 * Roles: a raw-QUIC client and any WebTransport binding supply a complete
 * target (both presence bits, syntactically valid) and no policy; a raw-QUIC
 * server supplies accept_uri and no target. A WebTransport-over-HTTP/2
 * binding is refused. URI syntax follows RFC 3986: the authority must name a
 * nonempty host, the path is path-abempty optionally followed by "?" query,
 * and neither component may carry a fragment. authority.len + path_query.len
 * must fit the 65535-byte SETUP body; the outgoing SETUP must also fit the
 * session's send buffer at start.
 *
 *   MOQ_ERR_INVAL        bad sizes, unknown bits, facts_supported outside
 *                        facts_known, unknown kind, role/target/policy
 *                        mismatch, or invalid URI syntax.
 *   MOQ_ERR_WOULD_BLOCK  a required fact is not known yet.
 *   MOQ_ERR_UNSUPPORTED  a required fact is known false, or the kind is
 *                        WebTransport-over-HTTP/2.
 *   MOQ_ERR_WRONG_STATE  already bound, started, or closed.
 *   MOQ_ERR_NOMEM        the target copy failed.
 * Every refusal leaves the session unchanged. explicit_size is the caller's
 * buffer size; both it and struct_size must cover the v0 floor and
 * struct_size must not exceed explicit_size. Bytes past the v0 fields are
 * ignored.
 */
MOQ_API moq_result_t moq_session_bind_transport(moq_session_t *session,
                                                const moq_transport_binding_t *binding,
                                                size_t explicit_size);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_TRANSPORT_BINDING_H */
