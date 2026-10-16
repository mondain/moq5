#ifndef MOQ_WTQUIC_ADAPTER_INTERNAL_H
#define MOQ_WTQUIC_ADAPTER_INTERNAL_H

/*
 * Private, non-installed lockstep SPI between the wtquic attach adapter and the
 * managed wtquic facades (which are SEPARATE DSOs). NOT part of the installed
 * public API (moq/wtquic.h) and NOT an application ABI.
 */

#include <moq/wtquic.h>
#include <moq/transport_binding.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The transport facts of a WebTransport session wtquic has just established,
 * for a session whose wire profile must be bound to its transport.
 *
 *   HANDSHAKE_COMPLETE  true: wtquic establishes a session only after the
 *                       QUIC handshake, the HTTP/3 SETTINGS exchange and the
 *                       CONNECT round trip.
 *   LOCAL_QUIC_DATAGRAM the facade's own provider configuration, passed in
 *                       (local_datagram): whether this side advertised a QUIC
 *                       max_datagram_frame_size.
 *   DATAGRAM_RECEIVE    equals LOCAL_QUIC_DATAGRAM: wtquic always sends
 *                       SETTINGS_H3_DATAGRAM=1, so receiving WebTransport
 *                       datagrams needs only the QUIC side.
 *   PEER_QUIC_DATAGRAM  answered from wtq_session_datagram_max_size(): a
 *                       nonzero size means the transport reports the peer
 *                       accepts DATAGRAM frames. A zero size is the peer's
 *                       refusal on a backend whose limit is the peer's
 *                       transport parameter (zero_is_refusal), and unknown on
 *                       a backend whose datagram flow may still be coming up.
 *   DATAGRAM_SEND       equals PEER_QUIC_DATAGRAM: wtquic selects a
 *                       WebTransport profile only when the peer's SETTINGS
 *                       carry H3_DATAGRAM=1, so at establishment the HTTP/3
 *                       half of sendability is already known true.
 */
static inline void moq_wtquic_binding_facts(const wtq_session_t *s,
                                            bool local_datagram,
                                            bool zero_is_refusal,
                                            uint32_t *facts_known,
                                            uint32_t *facts_supported)
{
    uint32_t known = MOQ_BINDING_HANDSHAKE_COMPLETE |
                     MOQ_BINDING_LOCAL_QUIC_DATAGRAM |
                     MOQ_BINDING_DATAGRAM_RECEIVE;
    uint32_t supported = MOQ_BINDING_HANDSHAKE_COMPLETE;
    const uint32_t peer_bits = MOQ_BINDING_PEER_QUIC_DATAGRAM |
                               MOQ_BINDING_DATAGRAM_SEND;

    if (local_datagram)
        supported |= MOQ_BINDING_LOCAL_QUIC_DATAGRAM |
                     MOQ_BINDING_DATAGRAM_RECEIVE;
    if (wtq_session_datagram_max_size(s) > 0) {
        known |= peer_bits;
        supported |= peer_bits;
    } else if (zero_is_refusal) {
        known |= peer_bits;
    }
    *facts_known = known;
    *facts_supported = supported;
}

/*
 * Read the attached session's coalesced-doorbell re-drive inputs in ONE call
 * across the DSO boundary: returns the opaque monotonic event-progress token
 * (see moq_transport_bridge_event_progress_token) and writes, through
 * out_has_events, whether the session still holds an undelivered event. The
 * managed facade snapshots the token before its app pump and, after the
 * post-pump service pass, re-arms a pump iff the token moved AND events remain.
 * This avoids exposing the bridge pointer or any application-facing session API.
 * Caller-serialized; token is equality-only (it wraps).
 */
MOQ_API uint64_t moq_wtquic_conn_event_progress(const moq_wtquic_conn_t *conn,
                                                bool *out_has_events);

/*
 * Read the attached session's independent monotonic TERMINAL facts in ONE call
 * across the DSO boundary (see moq_transport_bridge_terminal_facts):
 *
 *   returns        ENQUEUED -- MOQ_EVENT_SESSION_CLOSED was placed in the
 *                  session's event queue.
 *   *out_observed  OBSERVED -- a poll actually TRANSFERRED that event to the
 *                  application. Queued-but-unpolled reads false.
 *
 * A managed facade gates reclamation on OBSERVED (plus its own transport facts
 * and the application's acknowledgment) instead of on pump timing, which only
 * ever proved that a pump ran. Their order relative to transport terminal is
 * not fixed, so the facade must record its transport facts separately.
 * out_observed may be NULL. Caller-serialized. Keeps the bridge pointer out of
 * the facade exactly as the progress query above does.
 */
MOQ_API bool moq_wtquic_conn_terminal_facts(const moq_wtquic_conn_t *conn,
                                            bool *out_observed);

/* Passive reliable-output composition for the managed client facade. The
 * caller holds the actual WT/session execution domain and retains conn/ws for
 * the entire call. No allocation, callback, pump or provider advancement.
 * Datagrams and receive-only state are excluded. WT loss is INTERNAL, distinct
 * from pending (DONE); actual attach terminal is CLOSED. Not installed. */
MOQ_API moq_result_t moq_wtquic_conn_poll_flush(const moq_wtquic_conn_t *conn);

#endif /* MOQ_WTQUIC_ADAPTER_INTERNAL_H */
