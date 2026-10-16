import CMoQCore
import Foundation

// MARK: - Fill Handle

/// A fill child of a subscription (draft-22 section 3.4): past objects a
/// subscription asked for, delivered on a fill fetch stream and ended exactly
/// once. Identifier-only after its terminal.
public struct Fill: Sendable, Hashable {
    internal let raw: moq_fill_t

    internal init(_ raw: moq_fill_t) {
        self.raw = raw
    }

    public var isValid: Bool { moq_fill_is_valid(raw) }

    public static func == (lhs: Fill, rhs: Fill) -> Bool {
        moq_fill_eq(lhs.raw, rhs.raw)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(moq_fill_hash(raw))
    }
}

// MARK: - Location Filter

/// Semantic location filter (location.h). The enclosing operation defines
/// omission and inheritance.
public enum LocationFilter: Sendable, Hashable {
    /// Explicitly no location constraint.
    case none
    /// Relative to the largest group: 0 the next group, 1 the current
    /// group, 2 the one before it.
    case relativeGroup(offset: UInt64)
    case absoluteStart(group: UInt64, object: UInt64)
    case groupEnd(startGroup: UInt64, startObject: UInt64, endGroup: UInt64)
    case objectEnd(startGroup: UInt64, startObject: UInt64,
                   endGroup: UInt64, endObject: UInt64)
    /// Starts at the next object; delivery can begin mid-group.
    case nextObject
    /// A kind this build does not model, as the library reported it.
    case other(UInt32)

    /// The library's value, as a query or an event reported it.
    internal init(_ c: moq_location_filter_cfg_t) {
        switch c.kind {
        case MOQ_LOCATION_FILTER_NONE:           self = .none
        case MOQ_LOCATION_FILTER_RELATIVE_GROUP: self = .relativeGroup(offset: c.relative_group_offset)
        case MOQ_LOCATION_FILTER_ABSOLUTE_START:
            self = .absoluteStart(group: c.start_group, object: c.start_object)
        case MOQ_LOCATION_FILTER_GROUP_END:
            self = .groupEnd(startGroup: c.start_group, startObject: c.start_object,
                             endGroup: c.end_group)
        case MOQ_LOCATION_FILTER_OBJECT_END:
            self = .objectEnd(startGroup: c.start_group, startObject: c.start_object,
                              endGroup: c.end_group, endObject: c.end_object)
        case MOQ_LOCATION_FILTER_NEXT_OBJECT:    self = .nextObject
        default:                                 self = .other(c.kind)
        }
    }

    internal func fill(_ out: UnsafeMutablePointer<moq_location_filter_cfg_t>) {
        moq_location_filter_cfg_init_sized(
            out, MemoryLayout<moq_location_filter_cfg_t>.size)
        switch self {
        case .none:
            out.pointee.kind = MOQ_LOCATION_FILTER_NONE
        case .relativeGroup(let offset):
            out.pointee.kind = MOQ_LOCATION_FILTER_RELATIVE_GROUP
            out.pointee.relative_group_offset = offset
        case .absoluteStart(let g, let o):
            out.pointee.kind = MOQ_LOCATION_FILTER_ABSOLUTE_START
            out.pointee.start_group = g
            out.pointee.start_object = o
        case .groupEnd(let sg, let so, let eg):
            out.pointee.kind = MOQ_LOCATION_FILTER_GROUP_END
            out.pointee.start_group = sg
            out.pointee.start_object = so
            out.pointee.end_group = eg
        case .objectEnd(let sg, let so, let eg, let eo):
            out.pointee.kind = MOQ_LOCATION_FILTER_OBJECT_END
            out.pointee.start_group = sg
            out.pointee.start_object = so
            out.pointee.end_group = eg
            out.pointee.end_object = eo
        case .nextObject:
            out.pointee.kind = MOQ_LOCATION_FILTER_NEXT_OBJECT
        case .other(let k):
            out.pointee.kind = k
        }
    }
}

// MARK: - Fill Configuration

/// A fill request (fill.h). `nil` options inherit the subscription's value.
public struct FillConfiguration: Sendable, Hashable {
    public var subscriberPriority: UInt8?
    public var groupOrder: GroupOrder?
    public var fillTimeoutMs: UInt64?
    /// `nil` inherits the subscription's filter; `.none` removes it.
    public var location: LocationFilter?

    public init(location: LocationFilter? = nil) {
        self.subscriberPriority = nil
        self.groupOrder = nil
        self.fillTimeoutMs = nil
        self.location = location
    }

    /// The draft-22 section 3.5 join at the current group: the fill
    /// delivers that group from its start.
    public static var currentGroup: FillConfiguration {
        FillConfiguration(location: .relativeGroup(offset: 1))
    }

    internal func fill(_ out: UnsafeMutablePointer<moq_fill_cfg_t>,
                       location loc: UnsafeMutablePointer<moq_location_filter_cfg_t>) {
        moq_fill_cfg_init_sized(out, MemoryLayout<moq_fill_cfg_t>.size)
        if let p = subscriberPriority {
            out.pointee.has_subscriber_priority = true
            out.pointee.subscriber_priority = p
        }
        if let o = groupOrder {
            out.pointee.has_group_order = true
            out.pointee.group_order = o.cValue
        }
        if let t = fillTimeoutMs {
            out.pointee.has_fill_timeout = true
            out.pointee.fill_timeout_ms = t
        }
        if let l = location {
            l.fill(loc)
            out.pointee.location = UnsafePointer(loc)
        }
    }
}

// MARK: - Fetch Range Kind

public enum FetchRangeKind: Sendable, Hashable {
    case nonExistent
    case unknown
    /// A range a relay waited for and gave up on (draft-22's End of
    /// Timed-Out Range); a writer on another draft refuses it with
    /// `.invalidArgument`.
    case timedOut
    case other(UInt32)

    internal init(_ raw: UInt32) {
        switch raw {
        case UInt32(MOQ_FETCH_RANGE_NON_EXISTENT.rawValue): self = .nonExistent
        case UInt32(MOQ_FETCH_RANGE_UNKNOWN.rawValue):      self = .unknown
        case UInt32(MOQ_FETCH_RANGE_TIMED_OUT.rawValue):    self = .timedOut
        default:                                            self = .other(raw)
        }
    }

    internal var cValue: moq_fetch_range_kind_t {
        switch self {
        case .nonExistent:    return MOQ_FETCH_RANGE_NON_EXISTENT
        case .unknown:        return MOQ_FETCH_RANGE_UNKNOWN
        case .timedOut:       return MOQ_FETCH_RANGE_TIMED_OUT
        case .other(let v):   return moq_fetch_range_kind_t(rawValue: v)
        }
    }
}

// MARK: - Fill Event Payloads

/// The parent of a fill: exactly one of the two is set.
public enum FillParent: Sendable, Hashable {
    case subscription(Subscription)
    case publication(Publication)   // a PUBLISH-initiated parent

    internal init(sub: moq_subscription_t, pub: moq_publication_t) {
        if moq_publication_is_valid(pub) {
            self = .publication(Publication(pub))
        } else {
            self = .subscription(Subscription(sub))
        }
    }
}

public struct FillCreatedInfo: Sendable, Hashable {
    public let fill: Fill
    public let parent: FillParent
    public let initiatingRequestID: UInt64
}

/// The publisher owes a fill over the resolved inclusive range.
public struct FillRequestInfo: Sendable, Hashable {
    public let fill: Fill
    public let parent: FillParent
    public let initiatingRequestID: UInt64
    public let processedAtUs: UInt64
    public let startGroup: UInt64
    public let startObject: UInt64
    public let endGroup: UInt64
    public let endObject: UInt64
    public let subscriberPriority: UInt8
    public let groupOrder: GroupOrder
    public let fillTimeoutMs: UInt64?
}

/// A fill-delivered object. The buffers are owned by the value.
// Not Sendable: holds `Buffer` (non-atomic, shard-confined rcbuf).
public struct FillObject {
    public let fill: Fill
    public let parent: FillParent
    public let initiatingRequestID: UInt64
    public let groupID: UInt64
    public let subgroupID: UInt64
    public let objectID: UInt64
    public let publisherPriority: UInt8
    public let isDatagram: Bool
    public let payload: Buffer?
    public let properties: Buffer?
}

extension FillObject: Equatable {
    public static func == (lhs: FillObject, rhs: FillObject) -> Bool {
        lhs.fill == rhs.fill && lhs.parent == rhs.parent &&
        lhs.initiatingRequestID == rhs.initiatingRequestID &&
        lhs.groupID == rhs.groupID && lhs.subgroupID == rhs.subgroupID &&
        lhs.objectID == rhs.objectID &&
        lhs.publisherPriority == rhs.publisherPriority &&
        lhs.isDatagram == rhs.isDatagram &&
        lhs.payload === rhs.payload && lhs.properties === rhs.properties
    }
}

public struct FillGapInfo: Sendable, Hashable {
    public let fill: Fill
    public let parent: FillParent
    public let initiatingRequestID: UInt64
    public let groupID: UInt64
    public let objectID: UInt64
    public let kind: FetchRangeKind
}

/// Why a fill ended (MOQ_FILL_*), the domain its code belongs to
/// (MOQ_FILL_ERROR_*), and the code (zero is valid when the domain is set).
public struct FillTerminalInfo: Sendable, Hashable {
    public enum Reason: Sendable, Hashable {
        case complete, noStream, localCancelled, peerCancelledOrReset
        case localAborted, parentRejected, parentTerminated
        case other(UInt32)

        internal init(_ v: UInt32) {
            switch v {
            case UInt32(MOQ_FILL_COMPLETE):                self = .complete
            case UInt32(MOQ_FILL_NO_STREAM):               self = .noStream
            case UInt32(MOQ_FILL_LOCAL_CANCELLED):         self = .localCancelled
            case UInt32(MOQ_FILL_PEER_CANCELLED_OR_RESET): self = .peerCancelledOrReset
            case UInt32(MOQ_FILL_LOCAL_ABORTED):           self = .localAborted
            case UInt32(MOQ_FILL_PARENT_REJECTED):         self = .parentRejected
            case UInt32(MOQ_FILL_PARENT_TERMINATED):       self = .parentTerminated
            default:                                       self = .other(v)
            }
        }
    }

    public enum Domain: Sendable, Hashable {
        case none, stream, parentRequest, parentDone
        case other(UInt32)

        internal init(_ v: UInt32) {
            switch v {
            case UInt32(MOQ_FILL_ERROR_NONE):           self = .none
            case UInt32(MOQ_FILL_ERROR_STREAM):         self = .stream
            case UInt32(MOQ_FILL_ERROR_PARENT_REQUEST): self = .parentRequest
            case UInt32(MOQ_FILL_ERROR_PARENT_DONE):    self = .parentDone
            default:                                    self = .other(v)
            }
        }
    }

    public let fill: Fill
    public let parent: FillParent
    public let reason: Reason
    public let initiatingRequestID: UInt64
    public let domain: Domain
    public let errorCode: UInt64
}

/// The legacy scalar projection of a publisher's state notification.
/// Its per-notification location is in `ExtendedPublishStateInfo`; a latest
/// filter query cannot reconstruct this notification's historical value.
public struct PublishStateInfo: Sendable, Hashable {
    public let subscription: Subscription?
    public let publication: Publication?
    public let forward: Bool?
    public let largest: (group: UInt64, object: UInt64)?
    public let hasFilter: Bool

    public static func == (lhs: PublishStateInfo, rhs: PublishStateInfo) -> Bool {
        lhs.subscription == rhs.subscription && lhs.publication == rhs.publication &&
        lhs.forward == rhs.forward &&
        lhs.largest?.group == rhs.largest?.group &&
        lhs.largest?.object == rhs.largest?.object && lhs.hasFilter == rhs.hasFilter
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(subscription)
        hasher.combine(publication)
        hasher.combine(forward)
        hasher.combine(largest?.group)
        hasher.combine(largest?.object)
        hasher.combine(hasFilter)
    }
}

/// Copied state notification, including its own immutable location snapshot.
public struct ExtendedPublishStateInfo: Sendable, Hashable {
    public let legacy: PublishStateInfo
    /// nil means the notification carries no filter; explicit NONE is `.none`.
    public let location: LocationFilter?
    public var subscription: Subscription? { legacy.subscription }
    public var publication: Publication? { legacy.publication }
    public var forward: Bool? { legacy.forward }
    public var largest: (group: UInt64, object: UInt64)? { legacy.largest }
    public var hasFilter: Bool { legacy.hasFilter }

    internal static func from(_ event: moq_event_t) -> Self? {
        typealias T = moq_publish_state_event_t
        guard Int(event.detail_size) >= MemoryLayout<T>.offset(of: \T.location)! else { return nil }
        let value = event.u.publish_state
        guard value.family == MOQ_REQUEST_FAMILY_SUBSCRIBE || value.family == MOQ_REQUEST_FAMILY_PUBLISH else { return nil }
        var location: LocationFilter?
        if value.has_filter {
            guard detailCovers(event.detail_size, \T.location), let pointer = value.location else { return nil }
            let raw = UnsafeRawPointer(pointer)
            guard raw.load(as: UInt32.self) >= UInt32(MOQ_LOCATION_FILTER_CFG_V0_SIZE) else { return nil }
            var copy = moq_location_filter_cfg_t()
            withUnsafeMutableBytes(of: &copy) {
                $0.baseAddress!.copyMemory(from: raw, byteCount: Int(MOQ_LOCATION_FILTER_CFG_V0_SIZE))
            }
            location = LocationFilter(copy)
        }
        let isSub = value.family == MOQ_REQUEST_FAMILY_SUBSCRIBE
        return Self(legacy: PublishStateInfo(
            subscription: isSub ? Subscription(value.handle.subscription) : nil,
            publication: isSub ? nil : Publication(value.handle.publication),
            forward: value.has_forward ? value.forward : nil,
            largest: value.has_largest ? (value.largest_group, value.largest_object) : nil,
            hasFilter: value.has_filter), location: location)
    }
}
