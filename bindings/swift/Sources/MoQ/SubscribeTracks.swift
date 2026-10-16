import CMoQCore
import Foundation

// SUBSCRIBE_TRACKS (track discovery) on both sides, and the REQUEST_REDIRECT
// event that names the redirected request by its handle. SUBSCRIBE_TRACKS has
// no draft-16 wire form: its calls throw `.invalidArgument` on a draft-16
// session.

// MARK: - Track Subscription Handle

/// Opaque SUBSCRIBE_TRACKS identifier, on either side of the request.
public struct TrackSubscription: Sendable, Hashable {
    internal let raw: moq_track_sub_handle_t

    internal init(_ raw: moq_track_sub_handle_t) {
        self.raw = raw
    }

    public var isValid: Bool { moq_track_sub_handle_is_valid(raw) }

    public static func == (lhs: TrackSubscription, rhs: TrackSubscription) -> Bool {
        moq_track_sub_handle_eq(lhs.raw, rhs.raw)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(moq_track_sub_handle_hash(raw))
    }
}

// MARK: - Configuration

/// A request for PUBLISH messages for every track, present and future, under
/// a namespace prefix.
public struct SubscribeTracksConfiguration: Sendable {
    /// The Track Namespace Prefix: 0 to 32 non-empty parts.
    public var prefix: Namespace
    /// FORWARD for the resulting PUBLISH messages; nil leaves the default
    /// (forwarding).
    public var forward: Bool?
    /// Draft-22 GROUP_ORDER for the resulting PUBLISH messages: `.ascending`
    /// or `.descending` (`.default` throws `.invalidArgument`); nil sends none.
    /// Any option on an older draft throws `.unsupported` before sending.
    public var groupOrder: GroupOrder?
    /// Draft-22 INCLUDE_PROPERTIES; nil sends none (the default, true).
    public var includeProperties: Bool?

    public init(prefix: Namespace) {
        self.prefix = prefix
        self.forward = nil
        self.groupOrder = nil
        self.includeProperties = nil
    }
}

/// The draft-22 options of a peer's SUBSCRIBE_TRACKS beyond its prefix and
/// FORWARD: the effective values, and in `flags` which ones it carried
/// (`MOQ_SUBSCRIBE_TRACKS_GROUP_ORDER` / `_INCLUDE_PROPERTIES`).
public struct SubscribeTracksOptions: Sendable, Hashable {
    public let flags: UInt32
    public let groupOrder: GroupOrder
    public let includeProperties: Bool

    internal static func from(_ event: moq_event_t) -> Self? {
        guard detailCovers(event.detail_size, \moq_subscribe_tracks_request_event_t.options),
              let pointer = event.u.subscribe_tracks_request.options else { return nil }
        let raw = UnsafeRawPointer(pointer)
        guard raw.load(as: UInt32.self) >= UInt32(MOQ_SUBSCRIBE_TRACKS_OPTIONS_V0_SIZE)
        else { return nil }
        var value = moq_subscribe_tracks_options_t()
        withUnsafeMutableBytes(of: &value) {
            $0.baseAddress!.copyMemory(from: raw,
                                       byteCount: Int(MOQ_SUBSCRIBE_TRACKS_OPTIONS_V0_SIZE))
        }
        return Self(flags: value.flags, groupOrder: GroupOrder(value.group_order),
                    includeProperties: value.include_properties)
    }
}

// MARK: - Event Payloads

/// Publisher side: a peer's SUBSCRIBE_TRACKS. Answer it with
/// `acceptSubscribeTracks` or `rejectSubscribeTracks`.
public struct SubscribeTracksRequestInfo: Sendable, Hashable {
    public let trackSubscription: TrackSubscription
    public let prefix: Namespace
    public let forward: Bool
    /// nil on draft-18.
    public let options: SubscribeTracksOptions?
}

/// Subscriber side: the publisher refused the SUBSCRIBE_TRACKS
/// (`RequestErrorCode`); the request is over.
public struct SubscribeTracksErrorInfo: Sendable, Hashable {
    public let trackSubscription: TrackSubscription
    public let errorCode: UInt64
    public let reason: String
    public let canRetry: Bool
    public let retryAfterMs: UInt64
}

/// Subscriber side: the publisher cannot open a PUBLISH for a matching
/// track; the subscriber may SUBSCRIBE to it directly. The suffix is relative
/// to the request's prefix.
public struct PublishBlockedInfo: Sendable, Hashable {
    public let trackSubscription: TrackSubscription
    public let trackNamespaceSuffix: Namespace
    public let trackName: Data
}

/// The SUBSCRIBE_TRACKS request stream ended. `cause` and `streamErrorCode`
/// name the stream signal that ended it, when one did.
public struct SubscribeTracksCancelledInfo: Sendable, Hashable {
    public let trackSubscription: TrackSubscription
    public let cause: RequestTerminalCause
    /// RESET/STOP: the peer's code; otherwise zero.
    public let streamErrorCode: UInt64
}

/// The request a REQUEST_REDIRECT names, by its handle. Families this binding
/// has no handle type for carry the opaque handle value.
public enum RedirectedRequest: Sendable, Hashable {
    case subscription(Subscription)
    case fetch(Fetch)
    case trackStatus(handle: UInt64)
    case announcement(Announcement)
    case namespaceSubscription(handle: UInt64)
    case publication(Publication)
    case trackSubscription(TrackSubscription)
    case other(family: UInt32, handle: UInt64)
}

/// A relay redirected a request (error code REDIRECT): the request is over;
/// retry it at `connectURI` (empty: the current session's URI).
/// `trackNamespace` and `trackName` are the Redirect target exactly as
/// received. On draft-18, when both are empty the retried request uses the
/// original request's namespace and name; when either is not, both are the
/// literal values to use. On draft-22 they are the literal target, empty
/// values included.
public struct RequestRedirectInfo: Sendable, Hashable {
    public let request: RedirectedRequest
    public let errorCode: UInt64
    public let canRetry: Bool
    /// The most a relay may cache the redirect for.
    public let retryAfterMs: UInt64
    public let connectURI: Data
    public let trackNamespace: Namespace
    public let trackName: Data
    public let reason: String
}

extension RedirectedRequest {
    /// The request a family and raw handle name (a request GOAWAY's).
    internal init(family: moq_request_family_t, raw: UInt64) {
        switch family {
        case MOQ_REQUEST_FAMILY_SUBSCRIBE:
            self = .subscription(Subscription(moq_subscription_t(_opaque: raw)))
        case MOQ_REQUEST_FAMILY_FETCH:
            self = .fetch(Fetch(moq_fetch_t(_opaque: raw)))
        case MOQ_REQUEST_FAMILY_TRACK_STATUS:
            self = .trackStatus(handle: raw)
        case MOQ_REQUEST_FAMILY_ANNOUNCEMENT:
            self = .announcement(Announcement(moq_announcement_t(_opaque: raw)))
        case MOQ_REQUEST_FAMILY_NS_SUB:
            self = .namespaceSubscription(handle: raw)
        case MOQ_REQUEST_FAMILY_PUBLISH:
            self = .publication(Publication(moq_publication_t(_opaque: raw)))
        case MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS:
            self = .trackSubscription(TrackSubscription(moq_track_sub_handle_t(_opaque: raw)))
        default:
            self = .other(family: family.rawValue, handle: raw)
        }
    }

    internal init(_ d: moq_request_redirect_event_t) {
        switch d.family {
        case MOQ_REQUEST_FAMILY_SUBSCRIBE:
            self = .subscription(Subscription(d.handle.subscription))
        case MOQ_REQUEST_FAMILY_FETCH:
            self = .fetch(Fetch(d.handle.fetch))
        case MOQ_REQUEST_FAMILY_TRACK_STATUS:
            self = .trackStatus(handle: d.handle.raw)
        case MOQ_REQUEST_FAMILY_ANNOUNCEMENT:
            self = .announcement(Announcement(d.handle.announcement))
        case MOQ_REQUEST_FAMILY_NS_SUB:
            self = .namespaceSubscription(handle: d.handle.raw)
        case MOQ_REQUEST_FAMILY_PUBLISH:
            self = .publication(Publication(d.handle.publication))
        case MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS:
            self = .trackSubscription(TrackSubscription(d.handle.track_sub))
        default:
            self = .other(family: d.family.rawValue, handle: d.handle.raw)
        }
    }
}

// MARK: - Session calls

extension Session {

    /// Subscriber side: ask for PUBLISH messages for every track under
    /// `config.prefix`. The answer arrives as `.subscribeTracksOk` or
    /// `.subscribeTracksError`.
    @discardableResult
    public func subscribeTracks(
        _ config: SubscribeTracksConfiguration, now: UInt64 = 0
    ) throws -> TrackSubscription {
        try withCNamespace(config.prefix) { ns in
            var cfg = moq_subscribe_tracks_cfg_t()
            // The sized initializer stamps this struct's full size, so the
            // library reads the draft-22 options; the unsized C name is a
            // macro Swift cannot see, and its exported function covers only
            // the original layout.
            moq_subscribe_tracks_cfg_init_sized(
                &cfg, MemoryLayout<moq_subscribe_tracks_cfg_t>.size)
            cfg.track_namespace_prefix = ns
            if let f = config.forward {
                cfg.has_forward = true
                cfg.forward = f
            }
            if let order = config.groupOrder {
                cfg.options_flags |= UInt32(MOQ_SUBSCRIBE_TRACKS_GROUP_ORDER)
                cfg.group_order = order.cValue
            }
            if let include = config.includeProperties {
                cfg.options_flags |= UInt32(MOQ_SUBSCRIBE_TRACKS_INCLUDE_PROPERTIES)
                cfg.include_properties = include
            }
            var handle = moq_track_sub_handle_t(_opaque: 0)
            try MoQError.check(moq_session_subscribe_tracks(raw, &cfg, now, &handle))
            return TrackSubscription(handle)
        }
    }

    /// Publisher side: accept a SUBSCRIBE_TRACKS. The request stream stays
    /// open for `sendPublishBlocked`.
    public func acceptSubscribeTracks(
        _ trackSubscription: TrackSubscription, now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_subscribe_tracks_cfg_t()
        moq_accept_subscribe_tracks_cfg_init(&cfg)
        try MoQError.check(moq_session_accept_subscribe_tracks(
            raw, trackSubscription.raw, &cfg, now))
    }

    /// Publisher side: refuse a SUBSCRIBE_TRACKS (`RequestErrorCode`).
    public func rejectSubscribeTracks(
        _ trackSubscription: TrackSubscription, errorCode: UInt64,
        reason: String = "", now: UInt64 = 0
    ) throws {
        var cfg = moq_reject_subscribe_tracks_cfg_t()
        moq_reject_subscribe_tracks_cfg_init(&cfg)
        cfg.error_code = errorCode
        let r = Array(reason.utf8)
        try r.withUnsafeBufferPointer { rp in
            cfg.reason = moq_bytes_t(data: rp.baseAddress, len: rp.count)
            try MoQError.check(moq_session_reject_subscribe_tracks(
                raw, trackSubscription.raw, &cfg, now))
        }
    }

    /// Publisher side, on an accepted SUBSCRIBE_TRACKS: tell the subscriber
    /// a PUBLISH for this track cannot be opened. The suffix is relative to
    /// the request's prefix.
    public func sendPublishBlocked(
        on trackSubscription: TrackSubscription,
        trackNamespaceSuffix: Namespace, trackName: Data, now: UInt64 = 0
    ) throws {
        try withCTrackName(TrackName(namespace: trackNamespaceSuffix, name: trackName)) {
            ns, name in
            var suffix = ns
            try MoQError.check(moq_session_send_publish_blocked(
                raw, trackSubscription.raw, &suffix, name, now))
        }
    }
}
