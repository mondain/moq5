import CMoQCore
import Foundation

// MARK: - Publication Handle

/// Opaque identifier of a PUBLISH-initiated subscription, on either side:
/// the side that sent the PUBLISH and the side that accepted it.
public struct Publication: Sendable, Hashable {
    internal let raw: moq_publication_t

    internal init(_ raw: moq_publication_t) {
        self.raw = raw
    }

    public var isValid: Bool { moq_publication_is_valid(raw) }

    public static func == (lhs: Publication, rhs: Publication) -> Bool {
        moq_publication_eq(lhs.raw, rhs.raw)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(moq_publication_hash(raw))
    }
}

// MARK: - Publish Configuration

public struct PublishConfiguration: Sendable {
    public var track: TrackName
    public var trackAlias: UInt64?
    /// The initial forwarding state; nil leaves it to the wire default.
    public var forward: Bool?
    public var subscriberPriority: UInt8?
    public var groupOrder: GroupOrder?
    public var location: LocationFilter?
    public var objectDeliveryTimeoutMs: UInt64?
    public var subgroupDeliveryTimeoutMs: UInt64?
    public var expiresMs: UInt64?
    /// The track's Track Properties in the session's encoding (empty: none).
    public var trackProperties: Data

    public init(track: TrackName) {
        self.track = track
        self.trackProperties = Data()
        self.trackAlias = nil
        self.forward = nil
        self.subscriberPriority = nil
        self.groupOrder = nil
        self.location = nil
        self.objectDeliveryTimeoutMs = nil
        self.subgroupDeliveryTimeoutMs = nil
        self.expiresMs = nil
    }
}

// MARK: - Update Configuration

/// A subscriber's update of a subscription (REQUEST_UPDATE). nil options
/// leave the value unchanged. The filter applies to the absolute forms.
public struct SubscriptionUpdateConfiguration: Sendable {
    public var subscriberPriority: UInt8?
    public var forward: Bool?
    public var deliveryTimeoutUs: UInt64?
    public var filter: SubscribeFilter?
    /// A fill carried on the update; `.unsupported` on a draft without
    /// fills. A new fill never cancels an earlier one.
    public var fill: FillConfiguration?
    public var location: LocationFilter?
    public var objectDeliveryTimeoutMs: UInt64?
    public var subgroupDeliveryTimeoutMs: UInt64?
    /// NEW_GROUP_REQUEST: only for a track that advertised DYNAMIC_GROUPS
    /// (`.invalidArgument` otherwise, draft-22 section 9.20.19).
    public var newGroupRequest: UInt64?

    public init() {
        self.newGroupRequest = nil
        self.subscriberPriority = nil
        self.forward = nil
        self.deliveryTimeoutUs = nil
        self.filter = nil
        self.fill = nil
        self.location = nil
        self.objectDeliveryTimeoutMs = nil
        self.subgroupDeliveryTimeoutMs = nil
    }
}

// MARK: - Event Payloads

/// A PUBLISH from the peer. Query immutable initial parameters with
/// `Session.publishInitialParameters(_:)` while the publication is live.
public struct PublishRequestInfo: Sendable, Hashable {
    public let publication: Publication
    public let track: TrackName
    public let trackAlias: UInt64
    public let forward: Bool
    /// Initial peer PUBLISH expiry, not the later PUBLISH_OK response expiry.
    /// nil means absent or outside the native detail prefix; zero is present.
    public let expiresMs: UInt64?
    /// The PUBLISH's Track Properties, copied out.
    public let trackProperties: Data
}

/// The peer accepted this side's PUBLISH. The expiry belongs to the response,
/// independently of any expiry this side sent in the initial PUBLISH.
public struct PublishOkInfo: Sendable, Hashable {
    public let publication: Publication
    public let expiresMs: UInt64?
}

/// The peer accepted this side's subscription update. Missing observations
/// remain nil; neither zero nor a previous track snapshot supplies a default.
public struct SubscriptionUpdateOkInfo: Sendable, Hashable {
    public let subscription: Subscription
    public let largest: (group: UInt64, object: UInt64)?
    public let expiresMs: UInt64?

    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.subscription == rhs.subscription &&
        lhs.largest?.group == rhs.largest?.group &&
        lhs.largest?.object == rhs.largest?.object &&
        lhs.expiresMs == rhs.expiresMs
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(subscription)
        hasher.combine(largest?.group)
        hasher.combine(largest?.object)
        hasher.combine(expiresMs)
    }
}

/// The peer accepted an update of a PUBLISH-established subscription.
/// The optional values have the same presence semantics as SubscriptionUpdateOkInfo.
public struct PublicationUpdateOkInfo: Sendable, Hashable {
    public let publication: Publication
    public let largest: (group: UInt64, object: UInt64)?
    public let expiresMs: UInt64?

    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.publication == rhs.publication &&
        lhs.largest?.group == rhs.largest?.group &&
        lhs.largest?.object == rhs.largest?.object &&
        lhs.expiresMs == rhs.expiresMs
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(publication)
        hasher.combine(largest?.group)
        hasher.combine(largest?.object)
        hasher.combine(expiresMs)
    }
}

/// Immutable initial peer PUBLISH values. Presence is distinct from the
/// effective initial priority and group order, and from later UPDATE state.
public struct PublishInitialParameters: Sendable, Hashable {
    public let hasSubscriberPriority: Bool
    public let subscriberPriority: UInt8
    public let hasGroupOrder: Bool
    public let groupOrder: GroupOrder
    public let location: LocationFilter?
    public let objectDeliveryTimeoutMs: UInt64?
    public let subgroupDeliveryTimeoutMs: UInt64?
}

/// Selected UPDATE parameters copied out of native event scratch.
public struct SubscriptionUpdateOptions: Sendable, Hashable {
    public let initiatingRequestID: UInt64
    public let location: LocationFilter?
    public let objectDeliveryTimeoutMs: UInt64?
    public let subgroupDeliveryTimeoutMs: UInt64?

    internal init?(_ pointer: UnsafePointer<moq_subscription_update_options_t>?) {
        guard let pointer else { return nil }
        let size = UnsafeRawPointer(pointer).load(as: UInt32.self)
        guard detailCovers(size, \moq_subscription_update_options_t.subgroup_delivery_timeout_ms)
        else { return nil }
        // Copy only the raw-options prefix, even when newer headers append fields.
        let prefixSize = MemoryLayout<moq_subscription_update_options_t>.offset(
            of: \moq_subscription_update_options_t.subgroup_delivery_timeout_ms)! +
            MemoryLayout<UInt64>.size
        var value = moq_subscription_update_options_t()
        withUnsafeMutableBytes(of: &value) {
            $0.baseAddress!.copyMemory(from: UnsafeRawPointer(pointer), byteCount: prefixSize)
        }
        initiatingRequestID = value.initiating_request_id
        location = value.flags & 1 != 0 && value.location.struct_size >= UInt32(MOQ_LOCATION_FILTER_CFG_V0_SIZE)
            ? LocationFilter(value.location) : nil
        objectDeliveryTimeoutMs = value.flags & 2 != 0 ? value.object_delivery_timeout_ms : nil
        subgroupDeliveryTimeoutMs = value.flags & 4 != 0 ? value.subgroup_delivery_timeout_ms : nil
    }
}

/// A subscriber's update of a subscription or publication this side serves.
/// With `answerOwed` (the session's `deferSubscriptionUpdates`) nothing the
/// update asks for is in effect until the application accepts or rejects
/// it, naming `updateRequestID`; `updateCount` is the number of coalesced
/// updates the answer covers.
public struct UpdateRequestInfo: Sendable, Hashable {
    public let subscriberPriority: UInt8?
    public let forward: Bool?
    public let deliveryTimeoutUs: UInt64?
    public let filter: SubscribeFilter?
    /// Legacy projection; typed inclusive ends are in `options.location`.
    public let endObject: UInt64
    public let answerOwed: Bool
    public let fillRequested: Bool
    public let updateCount: UInt32
    public let updateRequestID: UInt64
    public let options: SubscriptionUpdateOptions?
}

public struct SubscriptionUpdatedInfo: Sendable, Hashable {
    public let subscription: Subscription
    public let update: UpdateRequestInfo
}

public struct PublicationUpdatedInfo: Sendable, Hashable {
    public let publication: Publication
    public let update: UpdateRequestInfo
}

/// The subscriber ended a subscription this side serves; `cause` and
/// `streamErrorCode` say which stream signal did it when no message did.
public struct UnsubscribedInfo: Sendable, Hashable {
    public let subscription: Subscription
    public let cause: RequestTerminalCause
    public let streamErrorCode: UInt64
}

/// The publisher ended a publication this side accepted (PUBLISH_DONE), or a
/// stream signal did (`cause`; `statusCode` is then zero).
public struct PublishFinishedInfo: Sendable, Hashable {
    public let publication: Publication
    public let statusCode: UInt64
    public let streamCount: UInt64
    public let reason: String
    public let cause: RequestTerminalCause
    public let streamErrorCode: UInt64
}

/// The subscriber ended a publication this side's PUBLISH began.
public struct PublishUnsubscribedInfo: Sendable, Hashable {
    public let publication: Publication
    public let cause: RequestTerminalCause
    public let streamErrorCode: UInt64
}

/// The peer cancelled a namespace this side announced through the legacy
/// message path. Draft-22 stream outcomes use NamespaceTerminatedInfo.
public struct NamespaceCancelledInfo: Sendable, Hashable {
    public let announcement: Announcement
    public let errorCode: UInt64
    public let reason: String
}

public struct NamespaceDoneInfo: Sendable, Hashable {
    public let announcement: Announcement
}

/// The update whose answer is owed on a publisher-role subscription: the
/// Request ID the answer names and the replacement filter it carries, if any.
public struct OwedUpdate: Sendable, Hashable {
    public let updateRequestID: UInt64
    public let filter: LocationFilter?
}

extension UpdateRequestInfo {
    internal init<T>(
        _ d: T, detailSize: UInt32,
        hasPriority: KeyPath<T, Bool>, priority: KeyPath<T, UInt8>,
        hasForward: KeyPath<T, Bool>, forward: KeyPath<T, Bool>,
        hasTimeout: KeyPath<T, Bool>, timeout: KeyPath<T, UInt64>,
        hasFilter: KeyPath<T, Bool>, filter: KeyPath<T, moq_subscribe_filter_t>,
        startGroup: KeyPath<T, UInt64>, startObject: KeyPath<T, UInt64>,
        endGroup: KeyPath<T, UInt64>, endObject: KeyPath<T, UInt64>?,
        answerOwed: KeyPath<T, Bool>, fillRequested: KeyPath<T, Bool>,
        updateCount: KeyPath<T, UInt32>, updateRequestID: KeyPath<T, UInt64>,
        options: UnsafePointer<moq_subscription_update_options_t>? = nil
    ) {
        self.subscriberPriority = d[keyPath: hasPriority] ? d[keyPath: priority] : nil
        self.forward = d[keyPath: hasForward] ? d[keyPath: forward] : nil
        self.deliveryTimeoutUs = d[keyPath: hasTimeout] ? d[keyPath: timeout] : nil
        self.filter = d[keyPath: hasFilter]
            ? SubscribeFilter.fromC(d[keyPath: filter],
                                    startGroup: d[keyPath: startGroup],
                                    startObject: d[keyPath: startObject],
                                    endGroup: d[keyPath: endGroup])
            : nil
        self.endObject = endObject.flatMap { detailCovers(detailSize, $0) ? d[keyPath: $0] : nil } ?? 0
        self.options = SubscriptionUpdateOptions(options)
        self.answerOwed = detailCovers(detailSize, answerOwed) && d[keyPath: answerOwed]
        self.fillRequested = detailCovers(detailSize, fillRequested) && d[keyPath: fillRequested]
        self.updateCount = detailCovers(detailSize, updateCount) ? d[keyPath: updateCount] : 0
        self.updateRequestID =
            detailCovers(detailSize, updateRequestID) ? d[keyPath: updateRequestID] : 0
    }
}
