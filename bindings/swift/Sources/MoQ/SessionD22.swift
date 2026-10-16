import CMoQCore
import Foundation
import MoQTransportModel

public typealias WireVersion = MoQTransportModel.WireVersion

extension WireVersion {
    internal var cValue: moq_version_t { moq_version_t(rawValue: rawValue) }
}

// The draft-22 session surface: capability and version queries, deferred
// update answers, filter queries, state notifications, FETCH, PUBLISH, the
// transport binding and the commit report. Every configuration is copied
// into C storage for the one call that borrows it.

extension MoQTransportVersion {
    internal var cValue: moq_version_t {
        switch self {
        case .draft16: return MOQ_VERSION_DRAFT_16
        case .draft18: return MOQ_VERSION_DRAFT_18
        }
    }

    internal init?(_ c: moq_version_t) {
        switch c {
        case MOQ_VERSION_DRAFT_16: self = .draft16
        case MOQ_VERSION_DRAFT_18: self = .draft18
        default: return nil
        }
    }
}

/// Holds a server's URI policy for the C callback; the session borrows the
/// context until it is destroyed, so the session keeps the box.
internal final class URIPolicyBox {
    let decide: (_ authority: Data?, _ pathQuery: Data?) -> URIVerdict
    init(_ decide: @escaping (Data?, Data?) -> URIVerdict) { self.decide = decide }
}

/// A raw-QUIC server's verdict on the URI its peer named.
public enum URIVerdict: Sendable, Hashable {
    case accept
    case invalidAuthority
    case invalidPath
}

/// How a session is carried, and the transport facts its profile may need
/// before the first message (transport_binding.h).
public struct TransportBinding {
    public enum Kind: Sendable, Hashable {
        case rawQUIC
        case webTransportH3
        case webTransportH2

        internal var cValue: UInt32 {
            switch self {
            case .rawQUIC:        return 1   // MOQ_BINDING_RAW_QUIC
            case .webTransportH3: return 2   // MOQ_BINDING_WEBTRANSPORT_H3
            case .webTransportH2: return 3   // MOQ_BINDING_WEBTRANSPORT_H2
            }
        }
    }

    /// Transport facts (`MOQ_BINDING_*` fact bits). transport_binding.h
    /// spells them with UINT32_C(), which Swift does not import; the values
    /// are the header's.
    public struct Facts: OptionSet, Sendable, Hashable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }
        public static let handshakeComplete  = Facts(rawValue: 1)
        public static let localQUICDatagram  = Facts(rawValue: 2)
        public static let peerQUICDatagram   = Facts(rawValue: 4)
        public static let datagramSend       = Facts(rawValue: 8)
        public static let datagramReceive    = Facts(rawValue: 16)
        public static let all: Facts = [.handshakeComplete, .localQUICDatagram,
                                        .peerQUICDatagram, .datagramSend,
                                        .datagramReceive]
    }

    public var kind: Kind
    /// Which facts carry an answer, and the answers (a subset of known).
    public var factsKnown: Facts
    public var factsSupported: Facts
    /// A client's (or any WebTransport binding's) target, kept in its
    /// encoded spelling. nil for a raw-QUIC server.
    public var authority: String?
    public var pathQuery: String
    /// A raw-QUIC server's URI policy, asked once per peer SETUP with the
    /// authority and path/query the peer named (nil when absent). It must
    /// not re-enter the session.
    public var acceptURI: ((Data?, Data?) -> URIVerdict)?

    public init(kind: Kind = .rawQUIC, factsKnown: Facts = [],
                factsSupported: Facts = []) {
        self.kind = kind
        self.factsKnown = factsKnown
        self.factsSupported = factsSupported
        self.authority = nil
        self.pathQuery = ""
        self.acceptURI = nil
    }
}

private func uriPolicyTrampoline(
    _ ctx: UnsafeMutableRawPointer?, _ present: UInt32,
    _ authority: moq_bytes_t, _ pathQuery: moq_bytes_t
) -> UInt32 {
    guard let ctx else { return 1 }
    let box = Unmanaged<URIPolicyBox>.fromOpaque(ctx).takeUnretainedValue()
    func bytes(_ b: moq_bytes_t, _ bit: UInt32) -> Data? {
        guard present & bit != 0 else { return nil }
        guard let p = b.data, b.len > 0 else { return Data() }
        return Data(bytes: p, count: b.len)
    }
    switch box.decide(bytes(authority, 1), bytes(pathQuery, 2)) {
    case .accept:           return 0   // MOQ_URI_ACCEPT
    case .invalidAuthority: return 1   // MOQ_URI_INVALID_AUTHORITY
    case .invalidPath:      return 2   // MOQ_URI_INVALID_PATH
    }
}

extension Session {

    // MARK: - Versions and capabilities

    /// Whether this build can offer `version` to a peer.
    public static func isVersionSupported(_ version: MoQTransportVersion) -> Bool {
        moq_version_supported(version.cValue)
    }

    /// Registered legacy versions. This is discovery, not a default offer.
    public static var supportedVersions: [MoQTransportVersion] {
        let n = moq_versions_supported(nil, 0)
        var raw = [moq_version_t](repeating: moq_version_t(rawValue: 0), count: n)
        let m = raw.withUnsafeMutableBufferPointer {
            moq_versions_supported($0.baseAddress, n)
        }
        return raw.prefix(min(n, m)).compactMap { MoQTransportVersion($0) }
    }

    public static func isWireVersionSupported(_ version: WireVersion) -> Bool {
        moq_version_supported(version.cValue)
    }

    /// Registered native profiles, including unknown future tokens. This does
    /// not certify transport qualification or change the default offer.
    public static var supportedWireVersions: [WireVersion] {
        let n = moq_versions_supported(nil, 0)
        var raw = [moq_version_t](repeating: moq_version_t(rawValue: 0), count: n)
        let m = moq_versions_supported(&raw, n)
        return raw.prefix(min(n, m)).map { WireVersion(rawValue: $0.rawValue) }
    }

    public static func bindingRequirements(forWireVersion version: WireVersion) -> UInt32 {
        moq_version_binding_requirements(version.cValue)
    }

    /// The transport facts a version's profile needs before it starts.
    public static func bindingRequirements(
        for version: MoQTransportVersion
    ) -> UInt32 {
        moq_version_binding_requirements(version.cValue)
    }

    /// The negotiated draft lets a subscription carry a fill (draft-22).
    public var supportsFill: Bool { moq_session_supports_fill(raw) }
    /// The negotiated draft has the joining forms of FETCH (drafts 16, 18).
    public var supportsJoiningFetch: Bool { moq_session_supports_joining_fetch(raw) }
    /// The negotiated draft allows several subscriptions to one track
    /// (draft-22).
    public var supportsConcurrentSubscriptions: Bool {
        moq_session_supports_concurrent_subscriptions(raw)
    }

    // MARK: - Transport binding

    /// Bind the transport before `start()`. The target bytes are copied; a
    /// URI policy is kept by this session until it is destroyed.
    public func bindTransport(_ binding: TransportBinding) throws {
        var b = moq_transport_binding_t()
        moq_transport_binding_init_sized(
            &b, MemoryLayout<moq_transport_binding_t>.size)
        b.kind = binding.kind.cValue
        b.facts_known = binding.factsKnown.rawValue
        b.facts_supported = binding.factsSupported.rawValue
        var box: URIPolicyBox?
        if let policy = binding.acceptURI {
            let made = URIPolicyBox(policy)
            box = made
            b.accept_uri = uriPolicyTrampoline
            b.accept_uri_ctx = Unmanaged.passUnretained(made).toOpaque()
        }
        let auth = Array((binding.authority ?? "").utf8)
        let path = Array(binding.pathQuery.utf8)
        try auth.withUnsafeBufferPointer { a in
            try path.withUnsafeBufferPointer { p in
                if binding.authority != nil {
                    b.target_present = 3   // MOQ_BINDING_TARGET_COMPLETE
                    b.authority = moq_bytes_t(data: a.baseAddress, len: a.count)
                    b.path_query = moq_bytes_t(data: p.baseAddress, len: p.count)
                }
                try MoQError.check(moq_session_bind_transport(
                    raw, &b, MemoryLayout<moq_transport_binding_t>.size))
            }
        }
        uriPolicy = box
    }

    /// A local data stream reached "all data committed" (every byte through
    /// its FIN acknowledged): stops a FIN-started subgroup delivery timer.
    public func receiveDataCommitted(on stream: StreamRef, now: UInt64) throws {
        try MoQError.check(moq_session_on_data_committed(
            raw, moq_stream_ref_t(_v: stream.rawValue), now))
    }

    // MARK: - Deferred update answers

    /// Switch `deferSubscriptionUpdates` after creation; an answer already
    /// owed stays owed.
    public func setDeferSubscriptionUpdates(_ enabled: Bool) throws {
        try MoQError.check(moq_session_set_defer_subscription_updates(raw, enabled))
    }

    /// Switch `deferFetchRanges` after creation; a FETCH already surfaced
    /// keeps its form.
    public func setDeferFetchRanges(_ enabled: Bool) throws {
        try MoQError.check(moq_session_set_defer_fetch_ranges(raw, enabled))
    }

    /// Accept the owed update of a subscription this side serves, naming
    /// the event's `updateRequestID`.
    public func acceptSubscriptionUpdate(
        _ subscription: Subscription, updateRequestID: UInt64, now: UInt64 = 0
    ) throws {
        var cfg = acceptUpdateCfg(updateRequestID)
        try MoQError.check(moq_session_accept_subscription_update(
            raw, subscription.raw, &cfg, now))
    }

    /// Reject the owed update: the subscription ends with UPDATE_FAILED and
    /// its handle is stale. `errorCode` is a `RequestErrorCode` (not redirect).
    public func rejectSubscriptionUpdate(
        _ subscription: Subscription, updateRequestID: UInt64,
        errorCode: UInt64, reason: String = "", now: UInt64 = 0
    ) throws {
        try withRejectUpdateCfg(updateRequestID, errorCode, reason) { cfg in
            try MoQError.check(moq_session_reject_subscription_update(
                raw, subscription.raw, cfg, now))
        }
    }

    /// The same answers for a subscription this side's PUBLISH began.
    public func acceptPublicationUpdate(
        _ publication: Publication, updateRequestID: UInt64, now: UInt64 = 0
    ) throws {
        var cfg = acceptUpdateCfg(updateRequestID)
        try MoQError.check(moq_session_accept_publication_update(
            raw, publication.raw, &cfg, now))
    }

    public func rejectPublicationUpdate(
        _ publication: Publication, updateRequestID: UInt64,
        errorCode: UInt64, reason: String = "", now: UInt64 = 0
    ) throws {
        try withRejectUpdateCfg(updateRequestID, errorCode, reason) { cfg in
            try MoQError.check(moq_session_reject_publication_update(
                raw, publication.raw, cfg, now))
        }
    }

    /// The update whose answer is owed on a publisher-role subscription.
    /// Throws `.wrongState` when none is owed.
    public func owedUpdate(on subscription: Subscription) throws -> OwedUpdate {
        var id: UInt64 = 0
        var hasFilter = false
        var f = moq_location_filter_cfg_t()
        moq_location_filter_cfg_init_sized(&f, MemoryLayout<moq_location_filter_cfg_t>.size)
        try MoQError.check(moq_session_subscription_owed_update_sized(
            raw, subscription.raw, &id, &hasFilter, &f,
            MemoryLayout<moq_location_filter_cfg_t>.size))
        return OwedUpdate(updateRequestID: id,
                          filter: hasFilter ? LocationFilter(f) : nil)
    }

    private func acceptUpdateCfg(_ id: UInt64) -> moq_accept_subscription_update_cfg_t {
        var cfg = moq_accept_subscription_update_cfg_t()
        moq_accept_subscription_update_cfg_init_sized(
            &cfg, MemoryLayout<moq_accept_subscription_update_cfg_t>.size)
        cfg.update_request_id = id
        return cfg
    }

    private func withRejectUpdateCfg(
        _ id: UInt64, _ code: UInt64, _ reason: String,
        _ body: (UnsafePointer<moq_reject_subscription_update_cfg_t>) throws -> Void
    ) throws {
        var cfg = moq_reject_subscription_update_cfg_t()
        moq_reject_subscription_update_cfg_init_sized(
            &cfg, MemoryLayout<moq_reject_subscription_update_cfg_t>.size)
        cfg.update_request_id = id
        cfg.error_code = code
        let r = Array(reason.utf8)
        try r.withUnsafeBufferPointer { rp in
            cfg.reason = moq_bytes_t(data: rp.baseAddress, len: rp.count)
            try body(&cfg)
        }
    }

    // MARK: - Filter queries

    /// The filter in effect on a subscription as this side knows it;
    /// `.none` when unfiltered.
    public func subscriptionFilter(_ subscription: Subscription) throws -> LocationFilter {
        var f = moq_location_filter_cfg_t()
        try MoQError.check(moq_session_subscription_filter_sized(
            raw, subscription.raw, &f, MemoryLayout<moq_location_filter_cfg_t>.size))
        return LocationFilter(f)
    }

    /// The filter in effect on a publication: the PUBLISH's initial filter,
    /// replaced by any accepted update.
    public func publicationFilter(_ publication: Publication) throws -> LocationFilter {
        var f = moq_location_filter_cfg_t()
        try MoQError.check(moq_session_publication_filter_sized(
            raw, publication.raw, &f, MemoryLayout<moq_location_filter_cfg_t>.size))
        return LocationFilter(f)
    }

    // MARK: - State notifications

    /// Publisher side: a unilateral Forward change and/or a new filter in
    /// effect on a subscription it serves. At least one is required.
    public func notifySubscriptionState(
        _ subscription: Subscription, forward: Bool?, filter: LocationFilter?,
        now: UInt64 = 0
    ) throws {
        try withPublishStateCfg(forward, filter) { cfg in
            try MoQError.check(moq_session_notify_subscription_state(
                raw, subscription.raw, cfg, now))
        }
    }

    /// The same notification from the publisher of a PUBLISH-initiated
    /// subscription.
    public func notifyPublicationState(
        _ publication: Publication, forward: Bool?, filter: LocationFilter?,
        now: UInt64 = 0
    ) throws {
        try withPublishStateCfg(forward, filter) { cfg in
            try MoQError.check(moq_session_notify_publication_state(
                raw, publication.raw, cfg, now))
        }
    }

    private func withPublishStateCfg(
        _ forward: Bool?, _ filter: LocationFilter?,
        _ body: (UnsafePointer<moq_publish_state_cfg_t>) throws -> Void
    ) throws {
        var cfg = moq_publish_state_cfg_t()
        moq_publish_state_cfg_init_sized(&cfg, MemoryLayout<moq_publish_state_cfg_t>.size)
        if let forward {
            cfg.has_forward = true
            cfg.forward = forward
        }
        var loc = moq_location_filter_cfg_t()
        try withUnsafeMutablePointer(to: &loc) { locPtr in
            if let filter {
                filter.fill(locPtr)
                cfg.filter = UnsafePointer(locPtr)
            }
            try body(&cfg)
        }
    }

    // MARK: - Subscription updates

    /// Update a subscription this side requested (REQUEST_UPDATE).
    public func updateSubscription(
        _ subscription: Subscription, _ config: SubscriptionUpdateConfiguration,
        now: UInt64 = 0
    ) throws {
        var ucfg = moq_subscription_update_cfg_t()
        moq_subscription_update_cfg_init_sized(
            &ucfg, MemoryLayout<moq_subscription_update_cfg_t>.size)
        if let value = config.newGroupRequest {
            ucfg.has_new_group_request = true
            ucfg.new_group_request = value
        }
        if let p = config.subscriberPriority {
            ucfg.has_subscriber_priority = true
            ucfg.subscriber_priority = p
        }
        if let f = config.forward {
            ucfg.has_forward = true
            ucfg.forward = f
        }
        if let t = config.deliveryTimeoutUs {
            ucfg.has_delivery_timeout = true
            ucfg.delivery_timeout_us = t
        }
        if let t = config.objectDeliveryTimeoutMs {
            ucfg.delivery_timeout_flags |= 1
            ucfg.object_delivery_timeout_ms = t
        }
        if let t = config.subgroupDeliveryTimeoutMs {
            ucfg.delivery_timeout_flags |= 2
            ucfg.subgroup_delivery_timeout_ms = t
        }
        if let filter = config.filter {
            ucfg.has_filter = true
            switch filter {
            case .nextGroup:
                ucfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP
            case .latestObject:
                ucfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT
            case .absoluteStart(let g, let o):
                ucfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START
                ucfg.start_group = g
                ucfg.start_object = o
            case .absoluteRange(let sg, let so, let eg):
                ucfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE
                ucfg.start_group = sg
                ucfg.start_object = so
                ucfg.end_group = eg
            case .unknown(let v):
                ucfg.filter = v
            }
        }
        var cfill = moq_fill_cfg_t()
        var loc = moq_location_filter_cfg_t()
        var fillLoc = moq_location_filter_cfg_t()
        try withUnsafeMutablePointer(to: &loc) { locPtr in
            try withUnsafeMutablePointer(to: &fillLoc) { fillLocPtr in
                try withUnsafeMutablePointer(to: &cfill) { fillPtr in
                    if let location = config.location {
                        location.fill(locPtr)
                        ucfg.location = UnsafePointer(locPtr)
                    }
                    if let fill = config.fill {
                        fill.fill(fillPtr, location: fillLocPtr)
                        ucfg.fill = UnsafePointer(fillPtr)
                    }
                    try MoQError.check(moq_session_update_subscription(
                        raw, subscription.raw, &ucfg, now))
                }
            }
        }
    }

    // MARK: - FETCH

    /// Send a standalone FETCH.
    @discardableResult
    public func fetch(_ config: FetchConfiguration, now: UInt64 = 0) throws -> Fetch {
        try withCTrackName(config.track) { ns, name in
            var cfg = moq_fetch_cfg_t()
            // Sized: location and FILL_TIMEOUT are past the original layout.
            moq_fetch_cfg_init_sized(&cfg, MemoryLayout<moq_fetch_cfg_t>.size)
            cfg.track_namespace = ns
            cfg.track_name = name
            cfg.start_group = config.startGroup
            cfg.start_object = config.startObject
            cfg.end_group = config.endGroup
            cfg.end_object = config.endObject
            cfg.group_order = config.groupOrder.cValue
            if let p = config.subscriberPriority {
                cfg.has_subscriber_priority = true
                cfg.subscriber_priority = p
            }
            if let t = config.fillTimeoutMs {
                cfg.has_fill_timeout = true
                cfg.fill_timeout_ms = t
            }
            if let include = config.includeProperties {
                cfg.has_include_properties = true
                cfg.include_properties = include
            }
            var loc = moq_location_filter_cfg_t()
            var handle = moq_fetch_t(_opaque: 0)
            try withUnsafeMutablePointer(to: &loc) { locPtr in
                if let l = config.location {
                    l.fill(locPtr)
                    cfg.location = UnsafePointer(locPtr)
                }
                try MoQError.check(moq_session_fetch(raw, &cfg, now, &handle))
            }
            return Fetch(handle)
        }
    }

    /// Publisher side: accept a FETCH. `endObject` is exclusive and zero
    /// means the whole `endGroup`. For a request surfaced unresolved the End
    /// must be one the request allows (`FetchRequestOptions`); a
    /// contradicting End throws `.invalidArgument` and the fetch stays
    /// pending.
    /// `trackProperties` are the track's, in the session's encoding (every
    /// draft's answer carries them); a request that asked for none gets none.
    public func acceptFetch(
        _ fetch: Fetch, endGroup: UInt64, endObject: UInt64,
        endOfTrack: Bool = false, empty: Bool = false,
        trackProperties: Data = Data(), now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_fetch_cfg_t()
        moq_accept_fetch_cfg_init(&cfg)
        cfg.end_of_track = endOfTrack
        cfg.end_group = endGroup
        cfg.end_object = endObject
        cfg.empty = empty
        try withCBytes(trackProperties) { props in
            cfg.track_properties = props
            try MoQError.check(moq_session_accept_fetch(raw, fetch.raw, &cfg, now))
        }
    }

    /// Publisher side: refuse a FETCH (`RequestErrorCode`).
    public func rejectFetch(
        _ fetch: Fetch, errorCode: UInt64, reason: String = "", now: UInt64 = 0
    ) throws {
        var cfg = moq_reject_fetch_cfg_t()
        moq_reject_fetch_cfg_init(&cfg)
        cfg.error_code = errorCode
        let r = Array(reason.utf8)
        try r.withUnsafeBufferPointer { rp in
            cfg.reason = moq_bytes_t(data: rp.baseAddress, len: rp.count)
            try MoQError.check(moq_session_reject_fetch(raw, fetch.raw, &cfg, now))
        }
    }

    /// Publisher side: write one object of an accepted FETCH. The caller
    /// keeps its buffer references; queued actions hold their own.
    public func writeFetchObject(
        _ fetch: Fetch, groupID: UInt64, subgroupID: UInt64 = 0,
        objectID: UInt64, publisherPriority: UInt8 = 128,
        payload: Buffer, properties: Buffer? = nil, now: UInt64 = 0
    ) throws {
        var cfg = moq_fetch_object_cfg_t()
        moq_fetch_object_cfg_init(&cfg)
        cfg.group_id = groupID
        cfg.subgroup_id = subgroupID
        cfg.object_id = objectID
        cfg.publisher_priority = publisherPriority
        cfg.payload = payload.raw
        cfg.properties = properties?.raw
        try MoQError.check(moq_session_write_fetch_object(raw, fetch.raw, &cfg, now))
    }

    /// Publisher side: mark a range the FETCH does not carry.
    public func writeFetchRange(
        _ fetch: Fetch, kind: FetchRangeKind, groupID: UInt64, objectID: UInt64,
        now: UInt64 = 0
    ) throws {
        try MoQError.check(moq_session_write_fetch_range(
            raw, fetch.raw, kind.cValue, groupID, objectID, now))
    }

    /// Publisher side: finish the FETCH's data stream.
    public func endFetch(_ fetch: Fetch, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_end_fetch(raw, fetch.raw, now))
    }

    /// Fetcher side: cancel a FETCH.
    public func cancelFetch(_ fetch: Fetch, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_fetch_cancel(raw, fetch.raw, now))
    }

    // MARK: - PUBLISH

    /// Offer a track to the peer (PUBLISH).
    @discardableResult
    public func publish(_ config: PublishConfiguration, now: UInt64 = 0) throws -> Publication {
        try withCTrackName(config.track) { ns, name in
            var cfg = moq_publish_cfg_t()
            moq_publish_cfg_init_sized(&cfg, MemoryLayout<moq_publish_cfg_t>.size)
            cfg.track_namespace = ns
            cfg.track_name = name
            if let a = config.trackAlias {
                cfg.has_track_alias = true
                cfg.track_alias = a
            }
            if let f = config.forward {
                cfg.has_forward = true
                cfg.forward = f
            }
            if let p = config.subscriberPriority {
                cfg.has_subscriber_priority = true
                cfg.subscriber_priority = p
            }
            if let order = config.groupOrder {
                cfg.has_group_order = true
                cfg.group_order = order.cValue
            }
            if let timeout = config.objectDeliveryTimeoutMs {
                cfg.has_object_delivery_timeout = true
                cfg.object_delivery_timeout_ms = timeout
            }
            if let timeout = config.subgroupDeliveryTimeoutMs {
                cfg.has_subgroup_delivery_timeout = true
                cfg.subgroup_delivery_timeout_ms = timeout
            }
            if let expiry = config.expiresMs {
                cfg.has_expires = true
                cfg.expires_ms = expiry
            }
            var handle = moq_publication_t(_opaque: 0)
            var location = moq_location_filter_cfg_t()
            try withUnsafeMutablePointer(to: &location) { ptr in
                if let value = config.location {
                    value.fill(ptr)
                    cfg.location = UnsafePointer(ptr)
                }
                try withCBytes(config.trackProperties) { props in
                    cfg.track_properties = props
                    try MoQError.check(moq_session_publish(raw, &cfg, now, &handle))
                }
            }
            return Publication(handle)
        }
    }

    /// Accept a PUBLISH from the peer with its initial settings.
    public func acceptPublish(_ publication: Publication, now: UInt64 = 0) throws {
        try acceptPublish(publication, expiresMs: nil, now: now)
    }

    public func acceptPublish(
        _ publication: Publication, expiresMs: UInt64?, now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_publish_cfg_t()
        moq_accept_publish_cfg_init_sized(&cfg, MemoryLayout<moq_accept_publish_cfg_t>.size)
        if let expiry = expiresMs {
            cfg.has_expires = true
            cfg.expires_ms = expiry
        }
        try MoQError.check(moq_session_accept_publish(raw, publication.raw, &cfg, now))
    }

    /// Cancel a publication this side accepted, resetting its request stream
    /// with `errorCode` (`moq_session_cancel_publication`); no event follows.
    public func cancelPublication(
        _ publication: Publication, errorCode: UInt64, now: UInt64 = 0
    ) throws {
        try MoQError.check(
            moq_session_cancel_publication(raw, publication.raw, errorCode, now))
    }

    /// Non-advancing immutable snapshot of an inbound draft-22 PUBLISH.
    public func publishInitialParameters(_ publication: Publication) throws -> PublishInitialParameters {
        var value = moq_publish_initial_parameters_t()
        try MoQError.check(moq_session_get_publish_initial_parameters(
            raw, publication.raw, &value, MemoryLayout<moq_publish_initial_parameters_t>.size))
        return PublishInitialParameters(
            hasSubscriberPriority: value.flags & 1 != 0,
            subscriberPriority: value.subscriber_priority,
            hasGroupOrder: value.flags & 2 != 0,
            groupOrder: GroupOrder(value.group_order),
            location: value.flags & 4 != 0 ? LocationFilter(value.location) : nil,
            objectDeliveryTimeoutMs: value.flags & 8 != 0 ? value.object_delivery_timeout_ms : nil,
            subgroupDeliveryTimeoutMs: value.flags & 16 != 0 ? value.subgroup_delivery_timeout_ms : nil)
    }

    /// Update a publication this side accepted (the subscriber of a
    /// PUBLISH-initiated subscription). Use `location` for a typed replacement;
    /// the legacy `filter` selection remains `.invalidArgument`. Typed options
    /// are `.unsupported` on legacy drafts; omitted values stay unchanged.
    /// A selected `deliveryTimeoutUs` is `.unsupported` on draft-22.
    /// On draft 22 the side that sent the PUBLISH may update it too, with
    /// only its own delivery timeouts (any other field is
    /// `.invalidArgument`); a refusal of that update arrives as a PUBLISH
    /// error and the publication must then be finished with status
    /// UPDATE_FAILED.
    public func updatePublication(
        _ publication: Publication, _ config: SubscriptionUpdateConfiguration,
        now: UInt64 = 0
    ) throws {
        if config.filter != nil { throw MoQError.invalidArgument }
        var ucfg = moq_publication_update_cfg_t()
        moq_publication_update_cfg_init_sized(
            &ucfg, MemoryLayout<moq_publication_update_cfg_t>.size)
        if let p = config.subscriberPriority {
            ucfg.has_subscriber_priority = true
            ucfg.subscriber_priority = p
        }
        if let f = config.forward {
            ucfg.has_forward = true
            ucfg.forward = f
        }
        if let t = config.deliveryTimeoutUs {
            ucfg.has_delivery_timeout = true
            ucfg.delivery_timeout_us = t
        }
        if let t = config.objectDeliveryTimeoutMs {
            ucfg.delivery_timeout_flags |= 1
            ucfg.object_delivery_timeout_ms = t
        }
        if let t = config.subgroupDeliveryTimeoutMs {
            ucfg.delivery_timeout_flags |= 2
            ucfg.subgroup_delivery_timeout_ms = t
        }
        var cfill = moq_fill_cfg_t()
        var loc = moq_location_filter_cfg_t()
        var fillLoc = moq_location_filter_cfg_t()
        try withUnsafeMutablePointer(to: &loc) { locPtr in
            try withUnsafeMutablePointer(to: &fillLoc) { fillLocPtr in
                try withUnsafeMutablePointer(to: &cfill) { fillPtr in
                    if let location = config.location {
                        location.fill(locPtr)
                        ucfg.location = UnsafePointer(locPtr)
                    }
                    if let fill = config.fill {
                        fill.fill(fillPtr, location: fillLocPtr)
                        ucfg.fill = UnsafePointer(fillPtr)
                    }
                    try MoQError.check(moq_session_update_publication(
                        raw, publication.raw, &ucfg, now))
                }
            }
        }
    }
}
