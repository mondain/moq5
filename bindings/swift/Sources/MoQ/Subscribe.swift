import CMoQCore
import Foundation

// MARK: - Subscription Handle

/// Opaque subscription identifier.
public struct Subscription: Sendable, Hashable {
    internal let raw: moq_subscription_t

    internal init(_ raw: moq_subscription_t) {
        self.raw = raw
    }

    public var isValid: Bool {
        moq_subscription_is_valid(raw)
    }

    public static func == (lhs: Subscription, rhs: Subscription) -> Bool {
        moq_subscription_eq(lhs.raw, rhs.raw)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(moq_subscription_hash(raw))
    }
}

// MARK: - Group Order

public enum GroupOrder: Sendable, Hashable {
    case `default`
    case ascending
    case descending
    case unknown(UInt32)

    internal init(_ raw: moq_group_order_t) {
        switch raw {
        case MOQ_GROUP_ORDER_DEFAULT:    self = .default
        case MOQ_GROUP_ORDER_ASCENDING:  self = .ascending
        case MOQ_GROUP_ORDER_DESCENDING: self = .descending
        default:                         self = .unknown(raw)
        }
    }

    internal var cValue: moq_group_order_t {
        switch self {
        case .default:        return MOQ_GROUP_ORDER_DEFAULT
        case .ascending:      return MOQ_GROUP_ORDER_ASCENDING
        case .descending:     return MOQ_GROUP_ORDER_DESCENDING
        case .unknown(let v): return v
        }
    }
}

// MARK: - Subscribe Filter

public enum SubscribeFilter: Sendable, Hashable {
    case nextGroup
    case latestObject
    case absoluteStart(group: UInt64, object: UInt64)
    case absoluteRange(startGroup: UInt64, startObject: UInt64, endGroup: UInt64)
    case unknown(UInt32)

    internal static func fromC(
        _ raw: moq_subscribe_filter_t,
        startGroup: UInt64, startObject: UInt64, endGroup: UInt64
    ) -> SubscribeFilter {
        switch raw {
        case MOQ_SUBSCRIBE_FILTER_NEXT_GROUP:     return .nextGroup
        case MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT: return .latestObject
        case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START:
            return .absoluteStart(group: startGroup, object: startObject)
        case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE:
            return .absoluteRange(
                startGroup: startGroup, startObject: startObject,
                endGroup: endGroup)
        default: return .unknown(raw)
        }
    }
}

// MARK: - Subscribe Configuration

public struct SubscribeConfiguration: Sendable {
    public var track: TrackName
    public var filter: SubscribeFilter
    public var subscriberPriority: UInt8?
    public var groupOrder: GroupOrder
    /// The filter in its semantic form; when set it replaces `filter` and
    /// admits forms it cannot express (a Next Object start, say). Throws
    /// `.unsupported` on a draft that cannot carry the form.
    public var location: LocationFilter?
    /// A fill carried on the SUBSCRIBE; `.unsupported` on a draft without
    /// fills, before anything is sent.
    public var fill: FillConfiguration?
    /// Independently selected exact milliseconds; nil differs from zero/MAX.
    /// Selected values are unsupported on draft-16/18 before request effects.
    public var objectDeliveryTimeoutMs: UInt64?
    public var subgroupDeliveryTimeoutMs: UInt64?
    /// Wait this many milliseconds for a publisher when none is known
    /// (draft-22 RENDEZVOUS_TIMEOUT, section 9.20.6); 0 asks not to wait. nil
    /// sends no parameter. `.unsupported` on an older draft.
    public var rendezvousTimeoutMs: UInt64?
    /// INCLUDE_PROPERTIES (draft-22 section 9.20.21): false asks for a
    /// SUBSCRIBE_OK with empty Track Properties, true states the default.
    /// nil sends no parameter. `.unsupported` on an older draft.
    public var includeProperties: Bool?
    /// NEW_GROUP_REQUEST (draft-22 section 9.20.19): ask the original
    /// publisher to start a new group; the value is the largest Group ID the
    /// subscriber knows plus one, 0 when it knows none. A SUBSCRIBE may carry
    /// it without knowing whether the track supports dynamic groups.
    public var newGroupRequest: UInt64?

    public init(track: TrackName, filter: SubscribeFilter = .nextGroup) {
        self.track = track
        self.filter = filter
        self.subscriberPriority = nil
        self.groupOrder = .default
        self.location = nil
        self.fill = nil
        self.objectDeliveryTimeoutMs = nil
        self.subgroupDeliveryTimeoutMs = nil
        self.rendezvousTimeoutMs = nil
        self.includeProperties = nil
        self.newGroupRequest = nil
    }
}

// MARK: - Accept Subscribe Configuration

public struct AcceptSubscribeConfiguration: Sendable {
    public var expiresMs: UInt64?
    /// The Largest Object this side holds for the track, carried in
    /// SUBSCRIBE_OK; a fill and a joining fetch resolve against it.
    public var largest: (group: UInt64, object: UInt64)?
    /// The track's Track Properties in the session's encoding (empty: none).
    /// A request that asked for none gets none.
    public var trackProperties: Data

    public init() {
        self.expiresMs = nil
        self.largest = nil
        self.trackProperties = Data()
    }
}

// MARK: - Subscribe Event Payloads

public struct SubscribeRequest: Sendable, Equatable, Hashable {
    public let subscription: Subscription
    public let track: TrackName
    public let filter: SubscribeFilter
    public let subscriberPriority: UInt8
    public let groupOrder: GroupOrder
}

/// Copied initial parameters, independent of the event's native borrow epoch.
/// Flags preserve explicit NONE, initial FILL and unknown future presence bits.
public struct SubscriptionAdmissionOptions: Sendable, Hashable {
    public let flags: UInt32
    public let location: LocationFilter?
    public let fillRequested: Bool
    public let objectDeliveryTimeoutMs: UInt64?
    public let subgroupDeliveryTimeoutMs: UInt64?
    /// The exact RENDEZVOUS_TIMEOUT milliseconds, including zero, when the
    /// SUBSCRIBE carried it.
    public let rendezvousTimeoutMs: UInt64?

    internal static func from(_ event: moq_event_t) -> Self? {
        guard detailCovers(event.detail_size, \moq_subscribe_request_event_t.options),
              let pointer = event.u.subscribe_request.options else { return nil }
        let raw = UnsafeRawPointer(pointer)
        guard raw.load(as: UInt32.self) >= UInt32(MOQ_SUBSCRIPTION_ADMISSION_OPTIONS_V0_SIZE) else { return nil }
        var value = moq_subscription_admission_options_t()
        withUnsafeMutableBytes(of: &value) {
            $0.baseAddress!.copyMemory(from: raw, byteCount: Int(MOQ_SUBSCRIPTION_ADMISSION_OPTIONS_V0_SIZE))
        }
        return Self(flags: value.flags,
                    location: value.flags & UInt32(MOQ_SUBSCRIPTION_ADMISSION_LOCATION) != 0 &&
                        value.location.struct_size >= UInt32(MOQ_LOCATION_FILTER_CFG_V0_SIZE)
                        ? LocationFilter(value.location) : nil,
                    fillRequested: value.flags & UInt32(MOQ_SUBSCRIPTION_ADMISSION_FILL) != 0,
                    objectDeliveryTimeoutMs: value.flags & UInt32(MOQ_SUBSCRIPTION_ADMISSION_OBJECT_DELIVERY_TIMEOUT) != 0
                        ? value.object_delivery_timeout_ms : nil,
                    subgroupDeliveryTimeoutMs: value.flags & UInt32(MOQ_SUBSCRIPTION_ADMISSION_SUBGROUP_DELIVERY_TIMEOUT) != 0
                        ? value.subgroup_delivery_timeout_ms : nil,
                    rendezvousTimeoutMs: rendezvous(raw, flags: value.flags))
    }

    private static func rendezvous(_ raw: UnsafeRawPointer, flags: UInt32) -> UInt64? {
        let offset = MemoryLayout<moq_subscription_admission_options_t>.offset(
            of: \moq_subscription_admission_options_t.rendezvous_timeout_ms)!
        guard flags & UInt32(MOQ_SUBSCRIPTION_ADMISSION_RENDEZVOUS_TIMEOUT) != 0,
              Int(raw.load(as: UInt32.self)) >= offset + MemoryLayout<UInt64>.size
        else { return nil }
        return raw.loadUnaligned(fromByteOffset: offset, as: UInt64.self)
    }
}

/// Initial SUBSCRIBE observation, with the legacy projection kept separate.
public struct ExtendedSubscribeRequest: Sendable, Hashable {
    public let legacy: SubscribeRequest
    /// nil for old/short records and draft-16/18, not an empty D22 parameter set.
    public let options: SubscriptionAdmissionOptions?
    public var subscription: Subscription { legacy.subscription }
    public var track: TrackName { legacy.track }
    public var filter: SubscribeFilter { legacy.filter }
    public var subscriberPriority: UInt8 { legacy.subscriberPriority }
    public var groupOrder: GroupOrder { legacy.groupOrder }
}

public struct SubscribeOkInfo: Sendable, Equatable, Hashable {
    public let subscription: Subscription
    public let expiresMs: UInt64?
    /// The answer's Track Properties, copied out.
    public let trackProperties: Data
    /// The track advertises DYNAMIC_GROUPS: updates may ask for new groups.
    public let dynamicGroups: Bool
}

public struct SubscribeErrorInfo: Sendable, Equatable, Hashable {
    public let subscription: Subscription
    public let errorCode: UInt64
    public let reason: String
    public let canRetry: Bool
}

// MARK: - C Interop Helpers

extension Namespace {
    internal static func fromC(_ ns: moq_namespace_t) -> Namespace {
        var parts: [NamespacePart] = []
        if let partsPtr = ns.parts {
            for i in 0..<ns.count {
                let part = partsPtr[i]
                if let data = part.data {
                    parts.append(NamespacePart(Data(bytes: data, count: part.len)))
                } else {
                    parts.append(NamespacePart(Data()))
                }
            }
        }
        return Namespace(parts)
    }
}

extension TrackName {
    internal static func fromC(
        namespace ns: moq_namespace_t, name: moq_bytes_t
    ) -> TrackName {
        let namespace = Namespace.fromC(ns)
        let nameData: Data
        if let namePtr = name.data, name.len > 0 {
            nameData = Data(bytes: namePtr, count: name.len)
        } else {
            nameData = Data()
        }
        return TrackName(namespace: namespace, name: nameData)
    }
}

/// Pin a Namespace's bytes for a C call that borrows moq_namespace_t.
internal func withCNamespace<R>(
    _ namespace: Namespace,
    _ body: (moq_namespace_t) throws -> R
) rethrows -> R {
    let parts = namespace.parts
    var flat: [UInt8] = []
    var ranges: [(offset: Int, length: Int)] = []
    for part in parts {
        let offset = flat.count
        flat.append(contentsOf: part.bytes)
        ranges.append((offset: offset, length: part.bytes.count))
    }

    return try flat.withUnsafeBufferPointer { flatPtr in
        let base = flatPtr.baseAddress
        var cParts = ranges.map { range in
            moq_bytes_t(data: base.map { $0 + range.offset }, len: range.length)
        }

        return try cParts.withUnsafeMutableBufferPointer { cPartsPtr in
            let ns = moq_namespace_t(
                parts: cPartsPtr.baseAddress,
                count: cPartsPtr.count
            )
            return try body(ns)
        }
    }
}

/// Pin a TrackName's bytes for a C call that borrows namespace + name.
internal func withCTrackName<R>(
    _ track: TrackName,
    _ body: (moq_namespace_t, moq_bytes_t) throws -> R
) rethrows -> R {
    let nameBytes = [UInt8](track.name.bytes)
    return try nameBytes.withUnsafeBufferPointer { namePtr in
        let cName = moq_bytes_t(data: namePtr.baseAddress, len: namePtr.count)
        return try withCNamespace(track.namespace) { ns in
            try body(ns, cName)
        }
    }
}
