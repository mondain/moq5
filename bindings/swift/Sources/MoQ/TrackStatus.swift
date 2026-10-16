import Foundation
import CMoQCore

// MARK: - TRACK_STATUS

/// A TRACK_STATUS request: the requester's handle for the answer, or the
/// responder's handle for the request it must answer.
public struct TrackStatus: Sendable, Hashable {
    internal let raw: moq_track_status_handle_t

    internal init(_ raw: moq_track_status_handle_t) {
        self.raw = raw
    }

    /// The handle's identifying value (stable for the request's life).
    public var id: UInt64 { raw._opaque }

    public static func == (lhs: TrackStatus, rhs: TrackStatus) -> Bool {
        lhs.raw._opaque == rhs.raw._opaque
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(raw._opaque)
    }
}

/// Ask a publisher for a track's status (draft-22 section 9.13).
public struct TrackStatusConfiguration: Sendable {
    public var track: TrackName
    /// INCLUDE_PROPERTIES (draft-22 section 9.20.21): false asks for an
    /// answer with empty Track Properties. nil sends no parameter.
    /// `.unsupported` on an older draft.
    public var includeProperties: Bool?

    public init(track: TrackName) {
        self.track = track
        self.includeProperties = nil
    }
}

/// The positive answer: what a SUBSCRIBE_OK for the track would carry. The
/// session reports at least the Largest Object it observed for the track;
/// draft-16's answer has no Track Properties.
public struct AcceptTrackStatusConfiguration: Sendable {
    public var largest: (group: UInt64, object: UInt64)?
    public var expiresMs: UInt64?
    /// The track's Track Properties in the session's encoding (empty: none).
    /// A request that asked for none gets none.
    public var trackProperties: Data

    public init() {
        self.largest = nil
        self.expiresMs = nil
        self.trackProperties = Data()
    }
}

public struct TrackStatusRequestInfo: Sendable, Hashable {
    public let request: TrackStatus
    public let track: TrackName
}

public struct TrackStatusOkInfo: Sendable, Hashable {
    public let request: TrackStatus
    public let largestGroup: UInt64?
    public let largestObject: UInt64?
    public let expiresMs: UInt64?
    /// The answer's Track Properties, copied out (empty on draft-16).
    public let trackProperties: Data
}

public struct TrackStatusErrorInfo: Sendable, Hashable {
    public let request: TrackStatus
    public let errorCode: UInt64
    public let reason: String
    public let canRetry: Bool
}

/// Copy a borrowed span out of an event.
internal func copiedData(_ b: moq_bytes_t) -> Data {
    guard let data = b.data, b.len > 0 else { return Data() }
    return Data(bytes: data, count: b.len)
}

/// Lend `data` to a C call as a borrowed span.
internal func withCBytes<R>(_ data: Data, _ body: (moq_bytes_t) throws -> R) rethrows -> R {
    try data.withUnsafeBytes { raw in
        try body(moq_bytes_t(
            data: raw.baseAddress?.assumingMemoryBound(to: UInt8.self), len: raw.count))
    }
}

extension Session {
    /// Ask the peer for a track's status. The answer arrives as
    /// `.trackStatusOk` or `.trackStatusError` (extended events).
    @discardableResult
    public func trackStatus(
        _ config: TrackStatusConfiguration, now: UInt64 = 0
    ) throws -> TrackStatus {
        try withCTrackName(config.track) { ns, name in
            var cfg = moq_track_status_cfg_t()
            // Sized: INCLUDE_PROPERTIES is past the original layout, which
            // is all the exported function name stamps.
            moq_track_status_cfg_init_sized(&cfg, MemoryLayout<moq_track_status_cfg_t>.size)
            cfg.track_namespace = ns
            cfg.track_name = name
            if let include = config.includeProperties {
                cfg.has_include_properties = true
                cfg.include_properties = include
            }
            var handle = moq_track_status_handle_t(_opaque: 0)
            try MoQError.check(moq_session_track_status(raw, &cfg, now, &handle))
            return TrackStatus(handle)
        }
    }

    /// Publisher side: answer a TRACK_STATUS positively.
    public func acceptTrackStatus(
        _ request: TrackStatus,
        config: AcceptTrackStatusConfiguration = AcceptTrackStatusConfiguration(),
        now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_track_status_cfg_t()
        moq_accept_track_status_cfg_init_sized(
            &cfg, MemoryLayout<moq_accept_track_status_cfg_t>.size)
        if let largest = config.largest {
            cfg.has_largest = true
            cfg.largest_group = largest.group
            cfg.largest_object = largest.object
        }
        if let expires = config.expiresMs {
            cfg.has_expires = true
            cfg.expires_ms = expires
        }
        try withCBytes(config.trackProperties) { props in
            cfg.track_properties = props
            try MoQError.check(
                moq_session_accept_track_status(raw, request.raw, &cfg, now))
        }
    }

    /// Publisher side: refuse a TRACK_STATUS (`RequestErrorCode`).
    public func rejectTrackStatus(
        _ request: TrackStatus, errorCode: UInt64, reason: String = "", now: UInt64 = 0
    ) throws {
        var cfg = moq_reject_track_status_cfg_t()
        moq_reject_track_status_cfg_init(&cfg)
        cfg.error_code = errorCode
        let r = Array(reason.utf8)
        try r.withUnsafeBufferPointer { rp in
            cfg.reason = moq_bytes_t(data: rp.baseAddress, len: rp.count)
            try MoQError.check(
                moq_session_reject_track_status(raw, request.raw, &cfg, now))
        }
    }
}
