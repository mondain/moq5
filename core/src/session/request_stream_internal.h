#ifndef MOQ_REQUEST_STREAM_INTERNAL_H
#define MOQ_REQUEST_STREAM_INTERNAL_H

#include "session_internal.h"

/* Merged range of peer request IDs that arrived ahead of the next expected
 * one. Storage follows the number of disjoint gaps, not the ID distance. */
typedef struct moq_request_id_range {
    uint64_t first;
    uint64_t last;
} moq_request_id_range_t;

/* Limits retained gaps, not the numeric distance to a reordered request. */
#define MOQ_REQUEST_PEER_ID_MAX_RANGES 1024u

/* Request-stream engine state. A profile that routes requests over
 * per-request bidirectional streams places this as the FIRST member of its
 * profile state; the engine reaches it through s->profile_state. Request IDs
 * are parity-allocated (client even, server odd) and never recycled. */
typedef struct moq_request_stream_state {
    uint64_t                next_local_request_id;
    uint64_t                peer_next_request_id;
    moq_request_id_range_t *peer_seen_later;
    size_t                  peer_seen_count;
    size_t                  peer_seen_cap;
    moq_alloc_t             alloc;
    bool                    peer_ids_exhausted;
    uint64_t                next_track_alias;
} moq_request_stream_state_t;

/* -- Codec table ------------------------------------------------------
 * The engine knows request streams; the profile knows bytes. A profile fills
 * one moq_request_codec_t and points moq_profile_ops_t.request_codec at it.
 * Every decoder reads a complete message body and writes the draft-neutral
 * record the core handlers consume; the engine owns admission, token
 * resolution, state guards, ordering rules and the close reasons. */

/* Decoded authorization tokens in wire order. The engine resolves them against
 * the session cache only after the request is admitted, so a refused request
 * never stages cache state. */
typedef struct moq_request_tokens {
    moq_decoded_auth_token_t in[MOQ_DECODED_MAX_TOKENS];
    size_t                   count;
} moq_request_tokens_t;

/* Control-message envelope as the engine sees it: type and body. */
typedef struct moq_request_envelope {
    uint64_t       msg_type;
    const uint8_t *payload;
    size_t         payload_len;
} moq_request_envelope_t;

typedef struct moq_request_goaway_msg {
    moq_bytes_t uri;
    uint64_t    timeout_ms;
} moq_request_goaway_msg_t;

typedef struct moq_request_error_msg {
    uint64_t    error_code;
    uint64_t    retry_interval;
    moq_bytes_t reason;
    bool        is_redirect;   /* the Redirect tail was present and decoded */
} moq_request_error_msg_t;

typedef struct moq_request_update_ok_msg {
    bool     has_largest;
    uint64_t largest_group;
    uint64_t largest_object;
    bool     has_expires;
    uint64_t expires_ms;
} moq_request_update_ok_msg_t;

typedef struct moq_request_publish_done_msg {
    uint64_t    status_code;
    uint64_t    stream_count;
    moq_bytes_t reason;
} moq_request_publish_done_msg_t;

/* A request decoder returns this when the message's parameter block (tokens
 * included) is structurally malformed; the engine closes with the
 * parameter-format code rather than the generic malformed-message code. */
#define MOQ_REQUEST_DECODE_BAD_PARAMS (-120)
/* The body decoded in full but carries a filter parameter this side cannot
 * honour (a Range Filter beyond the advertised limit, an undecodable range).
 * The record and request ID are filled; the engine answers the request with
 * INVALID_FILTER instead of surfacing it. */
#define MOQ_REQUEST_DECODE_INVALID_FILTER (-121)

typedef struct moq_request_codec {
    /* Envelope: on a complete message fills *env and *consumed (envelope plus
     * body) and returns MOQ_OK; MOQ_ERR_BUFFER when more bytes are needed;
     * another negative value when the envelope itself is malformed. */
    moq_result_t (*decode_envelope)(const uint8_t *buf, size_t len,
                                    moq_request_envelope_t *env,
                                    size_t *consumed);

    /* Message type codepoints this profile uses for each role. */
    struct {
        uint64_t subscribe, publish, fetch, publish_namespace, track_status,
                 subscribe_tracks, subscribe_namespace, request_update,
                 request_ok, request_error, publish_done, publish_blocked,
                 goaway, subscribe_ok, fetch_ok;
        /* Zero on profiles without the message. */
        uint64_t publish_state_notify;
    } type;

    /* Requests (first message on a peer-opened stream, or an update on an
     * established one). Each zeroes *out, fills it from the body, reports the
     * wire request ID and the token list. */
    moq_result_t (*decode_subscribe)(const uint8_t *p, size_t n,
                                     moq_decoded_subscribe_t *out,
                                     uint64_t *request_id, moq_request_tokens_t *tk);
    moq_result_t (*decode_publish)(const uint8_t *p, size_t n,
                                   moq_decoded_publish_t *out,
                                   uint64_t *request_id, moq_request_tokens_t *tk);
    moq_result_t (*decode_fetch)(const uint8_t *p, size_t n,
                                 moq_decoded_fetch_t *out,
                                 uint64_t *request_id, moq_request_tokens_t *tk);
    moq_result_t (*decode_publish_namespace)(const uint8_t *p, size_t n,
                                             moq_decoded_publish_namespace_t *out,
                                             uint64_t *request_id, moq_request_tokens_t *tk);
    moq_result_t (*decode_track_status)(const uint8_t *p, size_t n,
                                        moq_decoded_track_status_request_t *out,
                                        uint64_t *request_id, moq_request_tokens_t *tk);
    moq_result_t (*decode_subscribe_tracks)(const uint8_t *p, size_t n,
                                            moq_decoded_subscribe_tracks_request_t *out,
                                            uint64_t *request_id, moq_request_tokens_t *tk);
    /* Fills request_id and the parameter fields; the engine sets endpoint and
     * target after admission. `from_subscriber` names the sender's role on
     * the subscription the update modifies (the publisher of a PUBLISH may
     * update it too); the permitted parameters differ by role. */
    moq_result_t (*decode_request_update)(const uint8_t *p, size_t n,
                                          moq_request_family_t target_family,
                                          bool from_subscriber,
                                          moq_decoded_request_update_t *out,
                                          moq_request_tokens_t *tk);
    /* A namespace request's update (PUBLISH_NAMESPACE, SUBSCRIBE_NAMESPACE or
     * SUBSCRIBE_TRACKS, by `family`): identity, the family's parameters and
     * the tokens, not the much larger subscription/fill record. Returns
     * MOQ_REQUEST_DECODE_INVALID_FILTER, with the record filled, for a Range
     * Filter this side cannot serve. NULL preserves a profile's legacy
     * handling (no namespace updates). */
    moq_result_t (*decode_namespace_update)(const uint8_t *p, size_t n,
                                            moq_request_family_t family,
                                            moq_decoded_namespace_update_t *out,
                                            moq_request_tokens_t *tk);
    moq_result_t (*decode_goaway_request)(const uint8_t *p, size_t n,
                                          moq_request_goaway_msg_t *out);
    moq_result_t (*decode_publish_done)(const uint8_t *p, size_t n,
                                        moq_request_publish_done_msg_t *out);
    /* NULL on profiles without the message. */
    moq_result_t (*decode_publish_state_notify)(const uint8_t *p, size_t n,
                                                moq_decoded_state_notify_t *out);

    /* Responses. decode_request_error fills the redirect record (namespace
     * parts into rd_parts) only when out->is_redirect. */
    moq_result_t (*decode_request_error)(const uint8_t *p, size_t n,
                                         moq_bytes_t *rd_parts, size_t rd_parts_max,
                                         moq_request_error_msg_t *out,
                                         moq_decoded_redirect_t *rd);
    moq_result_t (*decode_subscribe_ok)(const uint8_t *p, size_t n,
                                        moq_decoded_subscribe_ok_t *out);
    moq_result_t (*decode_fetch_ok)(const uint8_t *p, size_t n,
                                    moq_decoded_fetch_ok_t *out);
    moq_result_t (*decode_request_ok)(const uint8_t *p, size_t n);
    /* The REQUEST_OK answering SUBSCRIBE_TRACKS, where it differs from the
     * namespace answers (NULL: decode_request_ok). */
    moq_result_t (*decode_subscribe_tracks_ok)(const uint8_t *p, size_t n);
    moq_result_t (*decode_track_status_ok)(const uint8_t *p, size_t n,
                                           moq_decoded_track_status_ok_t *out);
    moq_result_t (*decode_update_ok)(const uint8_t *p, size_t n,
                                     moq_request_update_ok_msg_t *out);
    moq_result_t (*decode_publish_blocked)(const uint8_t *p, size_t n,
                                           moq_decoded_publish_blocked_t *out);
    moq_result_t (*decode_publish_ok)(const uint8_t *p, size_t n,
                                      moq_decoded_publish_ok_t *out);
} moq_request_codec_t;

void moq_request_stream_state_init(moq_request_stream_state_t *rs,
                                   moq_perspective_t perspective,
                                   const moq_alloc_t *alloc);
void moq_request_stream_state_destroy(moq_request_stream_state_t *rs);

/* The next peer request ID this side expects; a session-level GOAWAY carries
 * it on profiles whose GOAWAY names a request ID. */
uint64_t moq_request_stream_peer_next_id(const moq_session_t *s);

/* Vtable entry points (see moq_profile_ops_t for each contract). */
moq_result_t moq_request_stream_prepare_request(moq_session_t *s,
                                                struct moq_request_endpoint *out);
void moq_request_stream_commit_request(moq_session_t *s,
                                       const struct moq_request_endpoint *ep);
void moq_request_stream_abort_request(moq_session_t *s,
                                      const struct moq_request_endpoint *ep);
void moq_request_stream_release_request(moq_session_t *s,
                                        const struct moq_request_endpoint *ep);
uint64_t moq_request_stream_next_track_alias(const moq_session_t *s);
void moq_request_stream_advance_track_alias(moq_session_t *s, uint64_t next_after);
moq_result_t moq_request_stream_validate_inbound(moq_session_t *s,
                                                 moq_stream_ref_t ref,
                                                 uint64_t msg_type,
                                                 uint64_t wire_request_id,
                                                 struct moq_request_endpoint *out);
void moq_request_stream_commit_inbound(moq_session_t *s,
                                       const struct moq_request_endpoint *ep);
/* Shared migration for request owners with separate receive-buffer routing. */
moq_result_t moq_request_stream_process_goaway(moq_session_t *s,
    moq_stream_ref_t ref, moq_request_family_t family, int slot,
    const moq_request_envelope_t *env, const uint8_t *buf, size_t off,
    size_t len, bool peer_fin_observed, size_t *out_consumed);
moq_result_t moq_request_stream_process_request(moq_session_t *s,
                                                moq_stream_ref_t stream_ref,
                                                int slot,
                                                const uint8_t *buf, size_t len,
                                                bool fin, size_t *out_consumed);
moq_result_t moq_request_stream_process_response(moq_session_t *s,
                                                 moq_stream_ref_t stream_ref,
                                                 int slot, uint32_t kind,
                                                 const uint8_t *buf, size_t len,
                                                 bool fin, size_t *out_consumed);

#endif /* MOQ_REQUEST_STREAM_INTERNAL_H */
