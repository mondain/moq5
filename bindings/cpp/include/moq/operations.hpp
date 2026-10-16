#ifndef MOQ_OPERATIONS_HPP
#define MOQ_OPERATIONS_HPP

#include <moq/buffer.hpp>
#include <moq/types.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace moq {

/* Semantic location filter (location.h). Copied by value into the C config
 * the operation borrows for its call; the wrapper owns the storage, so no
 * pointer in a config outlives the call. */
struct location_filter_config {
    location_filter_kind kind                  = location_filter_kind::none;
    uint64_t             relative_group_offset = 0;
    uint64_t             start_group           = 0;
    uint64_t             start_object          = 0;
    uint64_t             end_group             = 0;
    uint64_t             end_object            = 0;
};

/* The C form's values copied out, for a filter the library reports (a
 * query result or a FETCH request's options). */
inline location_filter_config location_from_c(
    const moq_location_filter_cfg_t &c) noexcept
{
    return location_filter_config{location_filter_kind_from_c(c.kind),
                                  c.relative_group_offset, c.start_group,
                                  c.start_object, c.end_group, c.end_object};
}

/* A fill request (fill.h, draft-22 section 3.4) carried on a SUBSCRIBE or an
 * update. An absent option inherits the subscription's value; an absent
 * location inherits its filter, an explicit `none` removes the constraint. */
struct fill_config {
    bool     has_subscriber_priority = false;
    uint8_t  subscriber_priority     = 128;
    bool     has_group_order         = false;
    moq::group_order group_order     = moq::group_order::ascending;
    bool     has_fill_timeout        = false;
    uint64_t fill_timeout_ms         = 0;
    std::optional<location_filter_config> location = {};
};

struct subscribe_config {
    namespace_name   ns;
    bytes_view       track;
    subscribe_filter filter   = subscribe_filter::largest_object;
    uint8_t          priority = 128;
    moq::group_order group_order = moq::group_order::default_order;
    bool             forward    = true;
    uint64_t         start_group  = 0;
    uint64_t         start_object = 0;
    uint64_t         end_group    = 0;
    bool             has_new_group_request = false;
    uint64_t         new_group_request     = 0;
};

struct subscribe_options {
    /* The subscription's filter in its semantic form; when set it replaces
     * `filter`/start/end and admits forms they cannot express. A form the
     * session's draft cannot carry is errc::unsupported before anything is
     * sent. */
    std::optional<location_filter_config> location = {};
    /* A fill carried on the SUBSCRIBE; errc::unsupported on a draft without
     * fills, before anything is sent. */
    std::optional<fill_config> fill = {};
    // Independently selected exact milliseconds; absent differs from 0/MAX.
    // Selected values are unsupported on draft-16/18 before request effects.
    std::optional<uint64_t> object_delivery_timeout_ms = {};
    std::optional<uint64_t> subgroup_delivery_timeout_ms = {};
    /* Wait this many milliseconds for a publisher when none is known
     * (draft-22 RENDEZVOUS_TIMEOUT, §9.20.6); 0 asks not to wait. Absent
     * sends no parameter. errc::unsupported on an older draft. */
    std::optional<uint64_t> rendezvous_timeout_ms = {};
    /* INCLUDE_PROPERTIES (draft-22 §9.20.21): false asks for a SUBSCRIBE_OK
     * with empty Track Properties, true states the default. Absent sends no
     * parameter. errc::unsupported on an older draft. */
    std::optional<bool> include_properties = {};
};

struct accept_subscribe_config {
    bool             has_track_alias  = false;
    uint64_t         track_alias      = 0;
    bool             has_largest      = false;
    uint64_t         largest_group    = 0;
    uint64_t         largest_object   = 0;
    bool             has_expires      = false;
    uint64_t         expires_ms       = 0;
    bytes_view       track_properties = {};
};

/* REDIRECT (§10.6.1) target on a reject; set error_code to request_error::redirect
 * to send it. All fields may stay empty (an all-empty Redirect reuses the session
 * and the original namespace/name). A client must leave connect_uri empty;
 * namespace-scoped families must leave track empty. */
struct redirect_target {
    std::string_view connect_uri = {};
    namespace_name   ns          = {};
    bytes_view       track       = {};
};

struct reject_subscribe_config {
    request_error    error_code      = request_error::internal_error;
    std::string_view reason          = {};
    bool             can_retry       = false;
    uint64_t         retry_after_ms  = 0;
    redirect_target  redirect        = {};
};

struct subgroup_config {
    uint64_t group_id           = 0;
    uint64_t subgroup_id        = 0;
    uint8_t  publisher_priority = 128;
    bool     object_properties  = false;
    bool     end_of_group       = false;
    /* Set FIRST_OBJECT: the stream begins with the first object ever
     * published in this subgroup (see moq_subgroup_cfg_t.first_object). */
    bool     first_object       = false;
};

struct datagram_config {
    uint64_t  group_id           = 0;
    uint64_t  object_id          = 0;
    uint8_t   publisher_priority = 128;
    bool      end_of_group       = false;
    bytes_view properties        = {};
};

struct status_datagram_config {
    uint64_t      group_id           = 0;
    uint64_t      object_id          = 0;
    uint8_t       publisher_priority = 128;
    object_status status             = object_status::end_of_group;
};

struct subscription_update_config {
    bool     has_subscriber_priority = false;
    uint8_t  subscriber_priority     = 128;
    bool     has_forward             = false;
    bool     forward                 = true;
    bool     has_delivery_timeout    = false;
    uint64_t delivery_timeout_us     = 0;
    std::span<const moq_auth_token_t> auth_tokens = {};
    bool     has_new_group_request   = false;   /* requires dynamic_groups */
    uint64_t new_group_request       = 0;
    /* update the subscription's filter. Locations apply to the
     * ABSOLUTE_* forms only (ignored otherwise, matching subscribe). */
    bool                   has_filter   = false;
    moq_subscribe_filter_t filter       = MOQ_SUBSCRIBE_FILTER_NONE;
    uint64_t               start_group  = 0;
    uint64_t               start_object = 0;
    uint64_t               end_group    = 0;
};

struct update_options {
    std::optional<location_filter_config> location = {};
    /* A fill carried on this update; a new fill never cancels an earlier
     * one. errc::unsupported on a draft without fills. */
    std::optional<fill_config> fill = {};
    std::optional<uint64_t> object_delivery_timeout_ms = {};
    std::optional<uint64_t> subgroup_delivery_timeout_ms = {};
};

struct done_subscribe_config {
    uint64_t         status_code  = 0;
    uint64_t         stream_count = 0;
    std::string_view reason       = {};
};

struct fetch_config {
    namespace_name ns;
    bytes_view     track;
    uint64_t       start_group     = 0;
    uint64_t       start_object    = 0;
    uint64_t       end_group       = 0;
    uint64_t       end_object      = 0;
    moq::group_order group_order   = moq::group_order::default_order;
    bool           has_subscriber_priority = false;
    uint8_t        subscriber_priority     = 128;
    bool           is_joining      = false;
    bool           joining_relative = false;
    subscription   joining_sub     = {};
    /* Join a PUBLISH-initiated subscription instead of a SUBSCRIBE-initiated
     * one. For a joining fetch exactly one of joining_sub / joining_pub must
     * be set; both or neither is errc::invalid. */
    publication    joining_pub     = {};
    uint64_t       joining_start   = 0;
    std::span<const moq_auth_token_t> auth_tokens = {};
};

struct fetch_options {
    /* The range of a standalone fetch in its semantic form; when set it
     * replaces start/end and admits a start relative to the publisher's
     * Largest Object, an open end and `none` (the whole track through the
     * Largest). errc::unsupported on a draft whose FETCH carries no Location
     * Filter, errc::invalid for a joining fetch, before anything is sent. */
    std::optional<location_filter_config> location = {};
    /* The fetch's FILL_TIMEOUT: the most a relay should wait for Objects not
     * immediately available before reporting them Timed-Out; zero asks for
     * only what is available at once. Same draft rule as `location`. */
    bool           has_fill_timeout = false;
    uint64_t       fill_timeout_ms  = 0;
    /* INCLUDE_PROPERTIES (draft-22 §9.20.21): false asks for an accepting
     * answer with empty Track Properties. Same draft rule as `location`. */
    std::optional<bool> include_properties = {};
};

struct accept_fetch_config {
    bool     end_of_track = false;
    uint64_t end_group    = 0;
    uint64_t end_object   = 0;
    bool     empty        = false;
    /* The track's properties stated in the answer (every draft's fetch
     * answer carries them). */
    bytes_view track_properties = {};
};

struct reject_fetch_config {
    request_error    error_code     = request_error::internal_error;
    std::string_view reason         = {};
    bool             can_retry      = false;
    uint64_t         retry_after_ms = 0;
    redirect_target  redirect       = {};
};

/* Per-request GOAWAY (§10.4): migrate one active request to new_session_uri
 * (empty => current session; a client must leave it empty). */
struct request_goaway_config {
    std::string_view new_session_uri = {};
    uint64_t         timeout_ms      = 0;
};

struct fetch_object_config {
    uint64_t      group_id           = 0;
    uint64_t      subgroup_id        = 0;   // ignored when datagram is true
    uint64_t      object_id          = 0;
    uint8_t       publisher_priority = 128;
    const buffer *properties         = nullptr;
    // Appended after properties to keep positional aggregate init stable.
    bool          datagram           = false;  // forwarding preference Datagram (draft-18)
};

struct publish_config {
    namespace_name ns;
    bytes_view     track;
    bool           has_track_alias = false;
    uint64_t       track_alias     = 0;
    bool           has_forward     = false;
    bool           forward         = true;
    bytes_view     track_properties = {};
    std::span<const moq_auth_token_t> auth_tokens = {};
};

struct publish_options {
    std::optional<location_filter_config> location = {};
    std::optional<uint8_t> subscriber_priority = {};
    std::optional<moq::group_order> group_order = {};
    std::optional<uint64_t> object_delivery_timeout_ms = {};
    std::optional<uint64_t> subgroup_delivery_timeout_ms = {};
    std::optional<uint64_t> expires_ms = {};
};

// Presence flags describe the initial wire values; priority/order also carry
// the effective defaults. This query result owns all its values.
struct publish_initial_parameters {
    bool has_subscriber_priority = false;
    uint8_t subscriber_priority = 128;
    bool has_group_order = false;
    moq::group_order group_order = moq::group_order::default_order;
    std::optional<location_filter_config> location = {};
    std::optional<uint64_t> object_delivery_timeout_ms = {};
    std::optional<uint64_t> subgroup_delivery_timeout_ms = {};
};

struct accept_publish_options {
    std::optional<uint64_t> expires_ms = {};
};

struct accept_publish_config {
    bool        has_subscriber_priority = false;
    uint8_t     subscriber_priority     = 128;
    moq::group_order group_order        = moq::group_order::default_order;
    bool        has_new_group_request   = false;   /* requires dynamic_groups */
    uint64_t    new_group_request       = 0;
    /* Subscription filter sent on the response (none = unfiltered). */
    subscribe_filter filter       = subscribe_filter::none;
    uint64_t         start_group  = 0;
    uint64_t         start_object = 0;
    uint64_t         end_group    = 0;
    /* On D16/18 an unset value resolves to forwarding. On D22 overrides are
     * refused; default acceptance preserves the initial PUBLISH state. */
    bool             has_forward  = false;
    bool             forward      = true;
};

struct reject_publish_config {
    request_error    error_code     = request_error::internal_error;
    std::string_view reason         = {};
    bool             can_retry      = false;
    uint64_t         retry_after_ms = 0;
};

struct publish_namespace_config {
    namespace_name ns;
    std::span<const moq_auth_token_t> auth_tokens = {};
};

struct reject_namespace_config {
    request_error    error_code     = request_error::internal_error;
    std::string_view reason         = {};
    bool             can_retry      = false;
    uint64_t         retry_after_ms = 0;
    redirect_target  redirect       = {};  /* namespace-scoped: leave track empty */
};

struct cancel_namespace_config {
    request_error    error_code = request_error::internal_error;
    std::string_view reason     = {};
};

struct track_status_config {
    namespace_name ns;
    bytes_view     track;
    std::span<const moq_auth_token_t> auth_tokens = {};
    /* INCLUDE_PROPERTIES (draft-22 §9.20.21): false asks for a
     * TRACK_STATUS_OK with empty Track Properties. Absent sends no parameter.
     * errc::unsupported on an older draft. */
    std::optional<bool> include_properties = {};
};

/* The positive answer to a TRACK_STATUS: what a SUBSCRIBE_OK for the track
 * would carry (§9.13). The session reports at least the Largest Object it
 * observed for the track; draft-16's answer has no Track Properties. */
struct accept_track_status_config {
    bool       has_largest      = false;
    uint64_t   largest_group    = 0;
    uint64_t   largest_object   = 0;
    bool       has_expires      = false;
    uint64_t   expires_ms       = 0;
    bytes_view track_properties = {};
};

struct reject_track_status_config {
    request_error    error_code     = request_error::internal_error;
    std::string_view reason         = {};
    bool             can_retry      = false;
    uint64_t         retry_after_ms = 0;
    redirect_target  redirect       = {};
};

struct subscribe_namespace_config {
    namespace_name      ns_prefix;
    namespace_interest  interest = namespace_interest::both;
    std::span<const moq_auth_token_t> auth_tokens = {};
};

struct reject_ns_sub_config {
    request_error    error_code     = request_error::internal_error;
    std::string_view reason         = {};
    bool             can_retry      = false;
    uint64_t         retry_after_ms = 0;
    redirect_target  redirect       = {};  /* namespace-scoped: leave track empty */
};

/* SUBSCRIBE_TRACKS (draft-22 §9.18): the namespace prefix whose tracks to
 * subscribe to, borrowed for the call. forward = false asks for the resulting
 * PUBLISH messages to carry FORWARD 0. group_order / include_properties are
 * draft-22 options (errc::unsupported on an older draft). */
struct subscribe_tracks_config {
    namespace_name                    ns_prefix;
    std::optional<bool>               forward            = {};
    std::span<const moq_auth_token_t> auth_tokens        = {};
    std::optional<moq::group_order>   group_order        = {};
    std::optional<bool>               include_properties = {};
};

struct reject_subscribe_tracks_config {
    request_error    error_code = request_error::internal_error;
    std::string_view reason     = {};
};

/* An update of a FETCH this side sent (draft-22 §9.5): its subscriber
 * priority and/or authorization tokens. One is required. */
struct fetch_update_config {
    std::optional<uint8_t>            subscriber_priority = {};
    std::span<const moq_auth_token_t> auth_tokens         = {};
};

/* The Location window a subscription's filter resolved to
 * (session::subscription_window). Unresolved: every Location is inside. */
struct filter_window {
    bool     resolved       = false;
    bool     empty          = false;
    bool     has_end        = false;
    bool     has_end_object = false;
    uint64_t start_group    = 0;
    uint64_t start_object   = 0;
    uint64_t end_group      = 0;
    uint64_t end_object     = 0;
};

/* Where a request went when it ended by migration (moq_request_migration_t):
 * a REDIRECT refusal or a GOAWAY on its request stream. connect_uri empty
 * means the current session's URI; track_namespace / track_name (REDIRECT
 * only) empty mean the original. The spans borrow facade storage until the
 * track is released (or published again). */
struct request_migration {
    bool            redirect       = false;
    bool            goaway         = false;
    bool            can_retry      = false;
    uint64_t        retry_after_ms = 0;
    uint64_t        timeout_ms     = 0;
    bytes_view      connect_uri    = {};
    moq_namespace_t track_namespace = {};
    bytes_view      track_name     = {};
};

inline request_migration migration_from_c(const moq_request_migration_t &m) noexcept
{
    return request_migration{m.redirect, m.goaway, m.can_retry, m.retry_after_ms,
                             m.timeout_ms, bytes_view(m.target.connect_uri),
                             m.target.track_namespace, bytes_view(m.target.track_name)};
}

struct publication_update_config {
    bool     has_subscriber_priority = false;
    uint8_t  subscriber_priority     = 128;
    bool     has_forward             = false;
    bool     forward                 = true;
    bool     has_delivery_timeout    = false;
    uint64_t delivery_timeout_us     = 0;
    std::span<const moq_auth_token_t> auth_tokens = {};
    bool     has_new_group_request   = false;   /* requires dynamic_groups */
    uint64_t new_group_request       = 0;
};

struct publication_update_options {
    /* A fill carried on this update (the subscriber of a PUBLISH-initiated
     * subscription asks for past objects); errc::unsupported on a draft
     * without fills. */
    std::optional<fill_config> fill = {};
    std::optional<location_filter_config> location = {};
    std::optional<uint64_t> object_delivery_timeout_ms = {};
    std::optional<uint64_t> subgroup_delivery_timeout_ms = {};
};

/* The answer to a subscriber's update owed under
 * session_options::defer_subscription_updates (extended_event::subscribe_updated
 * or extended_event::publish_updated with answer_owed). update_request_id names the
 * newest covered update, as the event (or session::subscription_owed_update) reports it. */
struct accept_subscription_update_config {
    uint64_t update_request_id = 0;
};

struct reject_subscription_update_config {
    uint64_t         update_request_id = 0;
    request_error    error_code        = request_error::internal_error;  /* not redirect */
    std::string_view reason            = {};  /* at most 1024 bytes */
};

/* The update whose answer is owed on a publisher-role subscription
 * (session::subscription_owed_update): the Request ID the answer names and
 * the replacement filter the cumulative update carries, if any. */
struct owed_update {
    uint64_t                              update_request_id = 0;
    std::optional<location_filter_config> filter            = {};
};

/* A draft-22 update of a namespace announcement, namespace subscription or
 * SUBSCRIBE_TRACKS (§9.5; session::update_namespace / update_ns_sub /
 * update_subscribe_tracks). An unset field keeps its value. `prefix` applies
 * to the two subscription kinds and `forward` to SUBSCRIBE_TRACKS only; a
 * field the request does not take is errc::invalid before anything is sent.
 * The prefix and tokens are borrowed for the call. */
struct namespace_update_config {
    std::optional<namespace_name>     prefix      = {};
    std::optional<bool>               forward     = {};
    std::span<const moq_auth_token_t> auth_tokens = {};
};

/* One namespace update as this side observes it (an owed-update query or a
 * *_update_request event): the selected fields only. Borrowed: valid until
 * the next advancing session call. */
struct namespace_update_record {
    uint64_t                              update_request_id = 0;
    std::optional<moq_namespace_t>        prefix_raw        = {};
    std::optional<bool>                   forward           = {};
    bool                                  has_authorization = false;
    std::span<const moq_resolved_token_t> tokens            = {};

    std::optional<namespace_name> prefix() const
    {
        if (!prefix_raw) return std::nullopt;
        return namespace_name(*prefix_raw);
    }
};

/* A publisher's unilateral state notification (PUBLISH_STATE_NOTIFY,
 * draft-22 section 9.10): a Forward change and/or a new filter in effect on
 * a subscription it serves. errc::unsupported on a draft without it. */
struct publish_state_config {
    bool has_forward = false;
    bool forward     = true;
    std::optional<location_filter_config> filter = {};
};

/* Transport binding (transport_binding.h): the facts about the connection a
 * wire profile needs before its first message. Copied by value into the C
 * struct for the single bind call. */
struct transport_binding_config {
    uint32_t          kind            = MOQ_BINDING_RAW_QUIC;
    uint32_t          facts_known     = 0;
    uint32_t          facts_supported = 0;
    uint32_t          target_present  = 0;
    bytes_view        authority       = {};
    bytes_view        path_query      = {};
    moq_uri_accept_fn accept_uri      = nullptr;
    void             *accept_uri_ctx  = nullptr;
};

struct finish_publish_config {
    uint64_t         status_code  = 0;
    uint64_t         stream_count = 0;
    std::string_view reason       = {};
};

struct stream_object_config {
    uint64_t object_id      = 0;
    uint64_t payload_length = 0;
};

} // namespace moq

#endif // MOQ_OPERATIONS_HPP
