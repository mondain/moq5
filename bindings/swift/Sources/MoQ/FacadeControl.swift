import CMoQCore
import Foundation

// Facade control: where a track went when the peer moved its request
// elsewhere, the subscriber's delivery timeouts and Location Filter updates.

/// Where a request went when it ended by migration: a REDIRECT refusal or a
/// GOAWAY on its own request stream. `connectURI` empty means the current
/// session's URI; `trackNamespace` / `trackName` (REDIRECT only) empty mean
/// the original request's.
public struct RequestMigration: Sendable, Hashable {
    public let redirect: Bool
    public let goaway: Bool
    public let canRetry: Bool
    public let retryAfterMs: UInt64
    public let timeoutMs: UInt64
    public let connectURI: Data
    public let trackNamespace: Namespace
    public let trackName: Data

    internal init(_ m: moq_request_migration_t) {
        redirect = m.redirect
        goaway = m.goaway
        canRetry = m.can_retry
        retryAfterMs = m.retry_after_ms
        timeoutMs = m.timeout_ms
        let uri = m.target.connect_uri
        connectURI = uri.data.map { Data(bytes: $0, count: uri.len) } ?? Data()
        trackNamespace = Namespace.fromC(m.target.track_namespace)
        let name = m.target.track_name
        trackName = name.data.map { Data(bytes: $0, count: name.len) } ?? Data()
    }
}

private func readMigration(
    _ query: (UnsafeMutablePointer<moq_request_migration_t>, Int) -> moq_result_t
) throws -> RequestMigration? {
    var m = moq_request_migration_t()
    let rc = query(&m, MemoryLayout<moq_request_migration_t>.size)
    if rc == MOQ_DONE { return nil }
    try MoQError.check(rc)
    return RequestMigration(m)
}

extension Subscriber {

    /// Where the track went when a REDIRECT or a per-request GOAWAY ended
    /// it; nil when it did not end by migration.
    public func migration(of track: SubscribedTrack) throws -> RequestMigration? {
        try readMigration { moq_sub_get_track_migration(raw, track.raw, $0, $1) }
    }

    /// Update an active track: its priority, the replacement filter in its
    /// semantic form, and the delivery timeouts (exact milliseconds). A
    /// field the draft cannot carry throws `.unsupported` before anything is
    /// sent.
    public func updateSubscription(
        _ track: SubscribedTrack, subscriberPriority: UInt8? = nil,
        location: LocationFilter? = nil, objectDeliveryTimeoutMs: UInt64? = nil,
        subgroupDeliveryTimeoutMs: UInt64? = nil, now: UInt64 = 0
    ) throws {
        var cfg = moq_sub_update_cfg_t()
        moq_sub_update_cfg_init_sized(&cfg, MemoryLayout<moq_sub_update_cfg_t>.size)
        if let p = subscriberPriority {
            cfg.has_subscriber_priority = true
            cfg.subscriber_priority = p
        }
        if let o = objectDeliveryTimeoutMs {
            cfg.delivery_timeout_flags |= 1   // MOQ_SUBSCRIPTION_UPDATE_OBJECT_DELIVERY_TIMEOUT
            cfg.object_delivery_timeout_ms = o
        }
        if let g = subgroupDeliveryTimeoutMs {
            cfg.delivery_timeout_flags |= 2   // MOQ_SUBSCRIPTION_UPDATE_SUBGROUP_DELIVERY_TIMEOUT
            cfg.subgroup_delivery_timeout_ms = g
        }
        var loc = moq_location_filter_cfg_t()
        try withUnsafeMutablePointer(to: &loc) { locPtr in
            if let l = location {
                l.fill(locPtr)
                cfg.location = UnsafePointer(locPtr)
            }
            try MoQError.check(moq_sub_update_subscription(raw, track.raw, &cfg, now))
        }
    }

    /// Subscribe asking for delivery timeouts (exact milliseconds) besides
    /// the filter; a draft without them throws `.unsupported`.
    @discardableResult
    public func subscribe(
        track: TrackName, filter: SubscribeFilter = .nextGroup,
        objectDeliveryTimeoutMs: UInt64?, subgroupDeliveryTimeoutMs: UInt64?,
        now: UInt64 = 0
    ) throws -> SubscribedTrack {
        let cFilter: moq_subscribe_filter_t
        switch filter {
        case .nextGroup: cFilter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP
        case .latestObject: cFilter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT
        case .absoluteStart, .absoluteRange, .unknown: throw MoQError.invalidArgument
        }
        var objectMs = objectDeliveryTimeoutMs ?? 0
        var subgroupMs = subgroupDeliveryTimeoutMs ?? 0
        return try withCTrackName(track) { ns, cName in
            try withUnsafePointer(to: &objectMs) { op in
                try withUnsafePointer(to: &subgroupMs) { gp in
                    var cfg = moq_sub_track_cfg_t()
                    moq_sub_track_cfg_init_sized(&cfg, MemoryLayout<moq_sub_track_cfg_t>.size)
                    cfg.track_namespace = ns
                    cfg.track_name = cName
                    cfg.filter = cFilter
                    if objectDeliveryTimeoutMs != nil { cfg.object_delivery_timeout_ms = op }
                    if subgroupDeliveryTimeoutMs != nil { cfg.subgroup_delivery_timeout_ms = gp }
                    var ptr: OpaquePointer?
                    try MoQError.check(moq_sub_subscribe(raw, &cfg, now, &ptr))
                    guard let p = ptr else { throw MoQError.internal }
                    return adoptTrack(p)
                }
            }
        }
    }
}

extension Publisher {

    /// Where the track's publication went when a REDIRECT or a GOAWAY on its
    /// request stream ended it; nil when it did not end by migration.
    public func migration(of track: PublisherTrack) throws -> RequestMigration? {
        try readMigration { moq_pub_get_track_migration(raw, track.raw, $0, $1) }
    }
}
