#ifndef MOQ_CONNECTION_TARGET_H
#define MOQ_CONNECTION_TARGET_H

/*
 * Connection target: the immutable URI identity a managed transport host
 * binds a session with. It carries no transport evidence -- a host that
 * owns the connection supplies the handshake and datagram facts itself and
 * copies these fields into the moq_transport_binding_t it binds.
 */

#include "transport_binding.h"

/*
 * Zero-initialize and stamp struct_size with the caller's sizeof. A managed
 * constructor copies the selected fields and the target bytes before it
 * returns; accept_uri_ctx is borrowed until the facade is destroyed.
 * Client: a complete authority and path_query, no policy. Raw-QUIC server:
 * a policy, no local target. The presence and verdict values are the
 * binding's. Authority and path_query keep their encoded spelling; no SNI or
 * port is inferred from them. Nothing here asserts handshake completion or
 * DATAGRAM negotiation.
 */
typedef struct moq_connection_target {
    uint32_t struct_size;
    uint32_t target_present;     /* MOQ_BINDING_*_PRESENT bits */
    moq_bytes_t authority;
    moq_bytes_t path_query;
    moq_uri_accept_fn accept_uri;
    void *accept_uri_ctx;
} moq_connection_target_t;

/* Frozen last-field floor: LP64 56 bytes, ILP32 32 bytes. */
#define MOQ_CONNECTION_TARGET_V0_SIZE \
    (offsetof(moq_connection_target_t, accept_uri_ctx) + sizeof(void *))

#endif /* MOQ_CONNECTION_TARGET_H */
