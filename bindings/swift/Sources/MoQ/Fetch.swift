import CMoQCore
import Foundation

// MARK: - Fetch Handle

/// Opaque FETCH identifier, on either side of the request.
public struct Fetch: Sendable, Hashable {
    internal let raw: moq_fetch_t

    internal init(_ raw: moq_fetch_t) {
        self.raw = raw
    }

    public var isValid: Bool { moq_fetch_is_valid(raw) }

    public static func == (lhs: Fetch, rhs: Fetch) -> Bool {
        moq_fetch_eq(lhs.raw, rhs.raw)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(moq_fetch_hash(raw))
    }
}

// MARK: - Fetch Configuration

/// A standalone FETCH. The explicit range (`startGroup` ... `endObject`,
/// `endObject` exclusive and zero for the whole end group) is used when
/// `location` is nil.
public struct FetchConfiguration: Sendable {
    public var track: TrackName
    public var startGroup: UInt64
    public var startObject: UInt64
    public var endGroup: UInt64
    public var endObject: UInt64
    public var groupOrder: GroupOrder
    public var subscriberPriority: UInt8?
    /// The range in its semantic form; when set it replaces the explicit
    /// range and admits a start relative to the publisher's Largest Object,
    /// an open end and `.none` (the whole track through the Largest Object).
    /// Throws `.unsupported` on a draft whose FETCH carries no Location
    /// Filter, before anything is sent.
    public var location: LocationFilter?
    /// The FETCH's FILL_TIMEOUT: the most a relay should wait for Objects
    /// not immediately available before reporting them Timed-Out; zero asks
    /// for only what is available at once. Same draft rule as `location`.
    public var fillTimeoutMs: UInt64?
    /// INCLUDE_PROPERTIES (draft-22 section 9.20.21): false asks for an
    /// accepting answer with empty Track Properties. Same draft rule as
    /// `location`.
    public var includeProperties: Bool?

    public init(track: TrackName) {
        self.track = track
        self.startGroup = 0
        self.startObject = 0
        self.endGroup = 0
        self.endObject = 0
        self.groupOrder = .default
        self.subscriberPriority = nil
        self.location = nil
        self.fillTimeoutMs = nil
        self.includeProperties = nil
    }
}

// MARK: - Fetch Event Payloads

/// What a FETCH request states beyond its range, on a draft whose FETCH
/// carries a Location Filter (draft-22). With `rangeUnresolved` (the
/// session's `deferFetchRanges`) the request's start/end are its explicit
/// values and `location` is the form to resolve against the Largest Object;
/// a nil location is the whole track through it.
public struct FetchRequestOptions: Sendable, Hashable {
    public let rangeUnresolved: Bool
    public let location: LocationFilter?
    public let fillTimeoutMs: UInt64?
    /// The effective request value, including the protocol's default true.
    public let includeProperties: Bool

    internal init(rangeUnresolved: Bool, location: LocationFilter?,
                  fillTimeoutMs: UInt64?, includeProperties: Bool = true) {
        self.rangeUnresolved = rangeUnresolved
        self.location = location
        self.fillTimeoutMs = fillTimeoutMs
        self.includeProperties = includeProperties
    }
}

public struct FetchRequestInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let track: TrackName
    public let startGroup: UInt64
    public let startObject: UInt64
    public let endGroup: UInt64
    public let endObject: UInt64
    public let subscriberPriority: UInt8
    public let groupOrder: GroupOrder
    /// nil on a draft whose FETCH carries no Location Filter.
    public let options: FetchRequestOptions?
}

public struct FetchOkInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let endOfTrack: Bool
    public let endGroup: UInt64
    public let endObject: UInt64
    /// The answer's Track Properties, copied out.
    public let trackProperties: Data
}

public struct FetchErrorInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let errorCode: UInt64
    public let reason: String
    public let canRetry: Bool
}

/// A fetch-delivered object. The buffers are owned by the value.
// Not Sendable: holds `Buffer` (non-atomic, shard-confined rcbuf).
public struct FetchObject {
    public let fetch: Fetch
    public let groupID: UInt64
    public let subgroupID: UInt64
    public let objectID: UInt64
    public let publisherPriority: UInt8
    public let isDatagram: Bool
    public let payload: Buffer?
    public let properties: Buffer?
    /// Prior Group ID Gap / Prior Object ID Gap as received (draft-22
    /// §10.8, §10.9); nil when absent.
    public let priorGroupGap: UInt64?
    public let priorObjectGap: UInt64?
}

extension FetchObject: Equatable {
    public static func == (lhs: FetchObject, rhs: FetchObject) -> Bool {
        lhs.fetch == rhs.fetch && lhs.groupID == rhs.groupID &&
        lhs.subgroupID == rhs.subgroupID && lhs.objectID == rhs.objectID &&
        lhs.publisherPriority == rhs.publisherPriority &&
        lhs.isDatagram == rhs.isDatagram &&
        lhs.priorGroupGap == rhs.priorGroupGap &&
        lhs.priorObjectGap == rhs.priorObjectGap &&
        lhs.payload === rhs.payload && lhs.properties === rhs.properties
    }
}

public struct FetchGapInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let kind: FetchRangeKind
    public let groupID: UInt64
    public let objectID: UInt64
}

/// A stream of a FETCH ended by a stream signal (fetcher side). `errorCode`
/// is the peer's stream error code, not a request error; `cause` tells a FIN
/// that failed the request from a RESET with code zero.
public struct FetchResetInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let errorCode: UInt64
    /// true: the response data stream; false: the request stream.
    public let dataStream: Bool
    /// Request stream only: STOP_SENDING rather than RESET_STREAM.
    public let stopSending: Bool
    public let cause: RequestTerminalCause
}

/// The fetcher cancelled the request; `cause` and `streamErrorCode` say
/// which stream signal did it when no cancel message did.
public struct FetchCancelledInfo: Sendable, Hashable {
    public let fetch: Fetch
    public let cause: RequestTerminalCause
    public let streamErrorCode: UInt64
}

extension FetchRequestOptions {
    /// Copy the borrowed C record out, when the event's detail and the
    /// record's own size cover what is read.
    internal static func from(_ ev: moq_event_t) -> FetchRequestOptions? {
        guard detailCovers(ev.detail_size, \moq_fetch_request_event_t.options),
              let pointer = ev.u.fetch_request.options else { return nil }
        let raw = UnsafeRawPointer(pointer)
        let size = raw.load(as: UInt32.self)
        guard size >= UInt32(MOQ_FETCH_REQUEST_OPTIONS_V0_SIZE) else { return nil }
        // Copy only covered fields, not the full current struct from a v0 producer.
        let deferredEnd = MemoryLayout<moq_fetch_request_options_t>.offset(
            of: \moq_fetch_request_options_t.range_unresolved)! + MemoryLayout<Bool>.size
        let count = Int(size) >= deferredEnd ? deferredEnd : Int(MOQ_FETCH_REQUEST_OPTIONS_V0_SIZE)
        var o = moq_fetch_request_options_t()
        withUnsafeMutableBytes(of: &o) {
            $0.baseAddress!.copyMemory(from: raw, byteCount: count)
        }
        guard o.location.struct_size >= UInt32(MOQ_LOCATION_FILTER_CFG_V0_SIZE) else { return nil }
        return FetchRequestOptions(
            rangeUnresolved: o.range_unresolved,
            location: o.location.kind == MOQ_LOCATION_FILTER_NONE ? nil : LocationFilter(o.location),
            fillTimeoutMs: o.flags & UInt32(MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT) != 0 ? o.fill_timeout_ms : nil,
            includeProperties: o.flags & UInt32(MOQ_FETCH_REQUEST_INCLUDE_PROPERTIES) != 0)
    }
}
