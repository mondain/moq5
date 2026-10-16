import CMoQCore
import Foundation

// Session control the session wrapper did not yet reach: closing with a code,
// unsubscribing, ending or refusing subscriptions and publications, GOAWAY,
// namespace subscriptions, the fetcher's own update and the resolved filter
// window. Events `ExtendedEvent` has no case for are delivered by
// `drainEventsWithDetail` as `.unknown(kind)` together with an `EventDetail`
// carrying their copied payload.

/// Pin each token's bytes for a C call that borrows a moq_auth_token_t array.
internal func withCAuthTokens<R>(
    _ tokens: [AuthorizationToken],
    _ body: (UnsafeBufferPointer<moq_auth_token_t>) throws -> R
) rethrows -> R {
    var blob: [UInt8] = []
    var spans: [(offset: Int, count: Int)] = []
    for t in tokens {
        spans.append((blob.count, t.value.count))
        blob.append(contentsOf: t.value)
    }
    return try blob.withUnsafeBufferPointer { bp in
        var cTokens: [moq_auth_token_t] = []
        cTokens.reserveCapacity(spans.count)
        for (i, s) in spans.enumerated() {
            cTokens.append(moq_auth_token_t(
                token_type: tokens[i].type,
                token_value: moq_bytes_t(
                    data: s.count > 0 ? bp.baseAddress.map { $0 + s.offset } : nil,
                    len: s.count)))
        }
        return try cTokens.withUnsafeBufferPointer { try body($0) }
    }
}

private func copyData(_ b: moq_bytes_t) -> Data {
    guard let d = b.data, b.len > 0 else { return Data() }
    return Data(bytes: d, count: b.len)
}

// MARK: - Window

/// The Location window a subscription's filter resolved to. Unresolved
/// (before the answer): every Location is inside.
public struct FilterWindow: Sendable, Hashable {
    public let resolved: Bool
    public let empty: Bool
    public let startGroup: UInt64
    public let startObject: UInt64
    /// The bounding group, when the window has one.
    public let endGroup: UInt64?
    /// The inclusive end object within `endGroup`, when the end is an object.
    public let endObject: UInt64?

    internal init(_ w: moq_filter_window_t) {
        resolved = w.resolved
        empty = w.empty
        startGroup = w.start_group
        startObject = w.start_object
        endGroup = w.has_end ? w.end_group : nil
        endObject = w.has_end_object ? w.end_object : nil
    }
}

// MARK: - Event details

/// The session-wide GOAWAY: where to reconnect (empty: the current URI) and
/// the sender's Timeout on drafts whose GOAWAY carries one.
public struct GoawayInfo: Sendable, Hashable {
    public let newSessionURI: Data
    public let timeoutMs: UInt64?
}

/// A GOAWAY on one request's stream: re-issue that request at
/// `newSessionURI` (empty: the current session). `request` names it by the
/// handle the request was made with (compare it with the saved value);
/// `trackStatus` is the typed handle of a TRACK_STATUS request. `family` and
/// `handle` are the C request family and raw handle.
public struct RequestGoawayInfo: Sendable, Hashable {
    public let family: UInt32
    public let handle: UInt64
    public let request: RedirectedRequest
    public let trackStatus: TrackStatus?
    public let newSessionURI: Data
    public let timeoutMs: UInt64
}

/// The subscriber refused this side's update of its own PUBLISH (draft-22
/// §9.5.1): `publication` stays established until this side ends it with
/// `finishPublish(_:statusCode: 0x8 ...)` (UPDATE_FAILED). Also the refusal
/// of a PUBLISH before it was accepted. The reason is empty when it did not
/// fit the session's event arena.
public struct PublishErrorInfo: Sendable, Hashable {
    public let publication: Publication
    public let errorCode: UInt64
    public let canRetry: Bool
    public let retryAfterMs: UInt64
    public let reason: Data
}

/// The publisher ended a subscription this side made (PUBLISH_DONE).
public struct SubscribeDoneInfo: Sendable, Hashable {
    public let subscription: Subscription
    public let statusCode: UInt64
    public let streamCount: UInt64
    public let reason: Data
}

/// A namespace subscription request from the peer.
public struct NamespaceSubscriptionRequestInfo: Sendable, Hashable {
    public let handle: UInt64
    public let prefix: Namespace
    public let forward: Bool
}

/// The answer to this side's namespace subscription, or a namespace under
/// it appearing or going away.
public struct NamespaceSubscriptionInfo: Sendable, Hashable {
    public let handle: UInt64
    public let errorCode: UInt64?
    public let reason: Data
    public let suffix: Namespace?
}

/// The copied payload of an event that `ExtendedEvent` reports as
/// `.unknown(kind)`, or whose case does not carry it (the peer's SETUP
/// authorization tokens on `.setupComplete`). Exactly one field is set.
public struct EventDetail: Sendable, Hashable {
    public let kind: UInt32
    /// The AUTHORIZATION TOKEN parameters the peer's SETUP carried, as the
    /// session resolved them.
    public var setupTokens: [AuthorizationToken]?
    public var goaway: GoawayInfo?
    public var requestGoaway: RequestGoawayInfo?
    public var publishError: PublishErrorInfo?
    public var subscribeDone: SubscribeDoneInfo?
    public var namespaceSubscriptionRequest: NamespaceSubscriptionRequestInfo?
    public var namespaceSubscriptionOk: NamespaceSubscriptionInfo?
    public var namespaceSubscriptionError: NamespaceSubscriptionInfo?
    public var namespaceFound: NamespaceSubscriptionInfo?
    public var namespaceGone: NamespaceSubscriptionInfo?

    internal init(kind: UInt32) { self.kind = kind }

    /// The detail of a C event before it is converted (and cleaned up).
    internal static func from(_ e: moq_event_t) -> EventDetail? {
        var d = EventDetail(kind: e.kind)
        let size = Int(e.detail_size)
        switch e.kind {
        case MOQ_EVENT_SETUP_COMPLETE:
            let c = e.u.setup_complete
            var tokens: [AuthorizationToken] = []
            if let t = c.tokens {
                for i in 0..<Int(c.token_count) {
                    tokens.append(AuthorizationToken(
                        type: t[i].token_type, value: copyData(t[i].token_value)))
                }
            }
            d.setupTokens = tokens
        case MOQ_EVENT_GOAWAY:
            let g = e.u.goaway
            let covers = size >= MemoryLayout<moq_goaway_event_t>.offset(
                of: \moq_goaway_event_t.timeout_ms)! + 8
            d.goaway = GoawayInfo(
                newSessionURI: copyData(g.new_session_uri),
                timeoutMs: covers && g.has_timeout ? g.timeout_ms : nil)
        case MOQ_EVENT_REQUEST_GOAWAY:
            let g = e.u.request_goaway
            d.requestGoaway = RequestGoawayInfo(
                family: g.family.rawValue, handle: g.handle.raw,
                request: RedirectedRequest(family: g.family, raw: g.handle.raw),
                trackStatus: g.family == MOQ_REQUEST_FAMILY_TRACK_STATUS
                    ? TrackStatus(g.handle.track_status) : nil,
                newSessionURI: copyData(g.new_session_uri), timeoutMs: g.timeout_ms)
        case MOQ_EVENT_PUBLISH_ERROR:
            let p = e.u.publish_error
            d.publishError = PublishErrorInfo(
                publication: Publication(p.pub), errorCode: UInt64(p.error_code),
                canRetry: p.can_retry, retryAfterMs: p.retry_after_ms,
                reason: copyData(p.reason))
        case MOQ_EVENT_SUBSCRIBE_DONE:
            let s = e.u.subscribe_done
            d.subscribeDone = SubscribeDoneInfo(
                subscription: Subscription(s.sub), statusCode: s.status_code,
                streamCount: s.stream_count, reason: copyData(s.reason))
        case MOQ_EVENT_NS_SUB_REQUEST:
            let r = e.u.ns_sub_request
            d.namespaceSubscriptionRequest = NamespaceSubscriptionRequestInfo(
                handle: r.handle._opaque, prefix: Namespace.fromC(r.track_namespace_prefix),
                forward: r.forward)
        case MOQ_EVENT_NS_SUB_OK:
            d.namespaceSubscriptionOk = NamespaceSubscriptionInfo(
                handle: e.u.ns_sub_ok.handle._opaque, errorCode: nil, reason: Data(),
                suffix: nil)
        case MOQ_EVENT_NS_SUB_ERROR:
            let r = e.u.ns_sub_error
            d.namespaceSubscriptionError = NamespaceSubscriptionInfo(
                handle: r.handle._opaque, errorCode: UInt64(r.error_code),
                reason: copyData(r.reason), suffix: nil)
        case MOQ_EVENT_NAMESPACE_FOUND:
            let r = e.u.namespace_found
            d.namespaceFound = NamespaceSubscriptionInfo(
                handle: r.handle._opaque, errorCode: nil, reason: Data(),
                suffix: Namespace.fromC(r.track_namespace_suffix))
        case MOQ_EVENT_NAMESPACE_GONE:
            let r = e.u.namespace_gone
            d.namespaceGone = NamespaceSubscriptionInfo(
                handle: r.handle._opaque, errorCode: nil, reason: Data(),
                suffix: Namespace.fromC(r.track_namespace_suffix))
        default:
            return nil
        }
        return d
    }
}

// MARK: - Session calls

extension Session {

    /// Like `drainEventsExtended`, with the copied payload `EventDetail`
    /// carries for the event when there is one.
    public func drainEventsWithDetail(
        _ body: (ExtendedEvent, EventDetail?) throws -> Void
    ) rethrows {
        var buf = [moq_event_t](repeating: moq_event_t(), count: 16)
        while true {
            let n = moq_session_poll_events(raw, &buf, buf.count)
            if n == 0 { break }
            var batch: [(ExtendedEvent, EventDetail?)] = []
            for i in 0..<n {
                let detail = EventDetail.from(buf[i])
                let event = ExtendedEvent.from(&buf[i])
                batch.append((event, detail))
            }
            for (event, detail) in batch { try body(event, detail) }
        }
    }

    /// Close the session with a termination code (draft-22 §12.2).
    public func close(code: UInt64, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_close(raw, code, nil, now))
    }

    /// Cancel a subscription this side made.
    public func unsubscribe(_ subscription: Subscription, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_unsubscribe(raw, subscription.raw, now))
    }

    /// End a subscription this side publishes (PUBLISH_DONE). streamCount
    /// is the data streams this side opened for it.
    public func doneSubscribe(
        _ subscription: Subscription, statusCode: UInt64, streamCount: UInt64,
        reason: Data = Data(), now: UInt64 = 0
    ) throws {
        try withCBytes(reason) { r in
            var cfg = moq_done_subscribe_cfg_t()
            moq_done_subscribe_cfg_init(&cfg)
            cfg.status_code = statusCode
            cfg.stream_count = streamCount
            cfg.reason = r
            try MoQError.check(moq_session_done_subscribe(raw, subscription.raw, &cfg, now))
        }
    }

    /// Refuse a PUBLISH (`RequestErrorCode`).
    public func rejectPublish(
        _ publication: Publication, errorCode: UInt64, reason: Data = Data(),
        now: UInt64 = 0
    ) throws {
        try withCBytes(reason) { r in
            var cfg = moq_reject_publish_cfg_t()
            moq_reject_publish_cfg_init(&cfg)
            cfg.error_code = errorCode
            cfg.reason = r
            try MoQError.check(moq_session_reject_publish(raw, publication.raw, &cfg, now))
        }
    }

    /// End a publication this side's PUBLISH began (PUBLISH_DONE).
    public func finishPublish(
        _ publication: Publication, statusCode: UInt64, streamCount: UInt64,
        reason: Data = Data(), now: UInt64 = 0
    ) throws {
        try withCBytes(reason) { r in
            var cfg = moq_finish_publish_cfg_t()
            moq_finish_publish_cfg_init(&cfg)
            cfg.status_code = statusCode
            cfg.stream_count = streamCount
            cfg.reason = r
            try MoQError.check(moq_session_finish_publish(raw, publication.raw, &cfg, now))
        }
    }

    /// Send a session-wide GOAWAY; a server may name where to reconnect.
    public func goaway(newSessionURI: Data = Data(), now: UInt64 = 0) throws {
        try newSessionURI.withUnsafeBytes { b in
            try MoQError.check(moq_session_goaway(
                raw, b.bindMemory(to: UInt8.self).baseAddress, b.count, now))
        }
    }

    /// Subscribe to the namespaces under a prefix (namespace-state interest,
    /// the form every supported draft carries). Returns the request's
    /// handle, which the detail events name.
    @discardableResult
    public func subscribeNamespace(
        prefix: Namespace, now: UInt64 = 0
    ) throws -> UInt64 {
        try withCNamespace(prefix) { ns in
            var cfg = moq_subscribe_namespace_cfg_t()
            moq_subscribe_namespace_cfg_init(&cfg)
            cfg.track_namespace_prefix = ns
            cfg.namespace_interest = moq_namespace_interest_t(MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE)
            var h = moq_ns_sub_handle_t(_opaque: 0)
            try MoQError.check(moq_session_subscribe_namespace(raw, &cfg, now, &h))
            return h._opaque
        }
    }

    public func acceptNamespaceSubscription(_ handle: UInt64, now: UInt64 = 0) throws {
        var cfg = moq_accept_ns_sub_cfg_t()
        moq_accept_ns_sub_cfg_init(&cfg)
        try MoQError.check(moq_session_accept_ns_sub(
            raw, moq_ns_sub_handle_t(_opaque: handle), &cfg, now))
    }

    public func rejectNamespaceSubscription(
        _ handle: UInt64, errorCode: UInt64, reason: Data = Data(), now: UInt64 = 0
    ) throws {
        try withCBytes(reason) { r in
            var cfg = moq_reject_ns_sub_cfg_t()
            moq_reject_ns_sub_cfg_init(&cfg)
            cfg.error_code = errorCode
            cfg.reason = r
            try MoQError.check(moq_session_reject_ns_sub(
                raw, moq_ns_sub_handle_t(_opaque: handle), &cfg, now))
        }
    }

    /// Announce a namespace (by its suffix under the request's prefix) on an
    /// accepted namespace subscription, or withdraw it.
    public func sendNamespace(
        on handle: UInt64, suffix: Namespace, done: Bool = false, now: UInt64 = 0
    ) throws {
        try withCNamespace(suffix) { ns in
            var s = ns
            let h = moq_ns_sub_handle_t(_opaque: handle)
            try MoQError.check(done
                ? moq_session_send_namespace_done(raw, h, &s, now)
                : moq_session_send_namespace(raw, h, &s, now))
        }
    }

    /// Update a FETCH this side sent (draft-22 §9.5): its subscriber priority
    /// and/or authorization tokens.
    public func updateFetch(
        _ fetch: Fetch, subscriberPriority: UInt8? = nil,
        authorizationTokens: [AuthorizationToken] = [], now: UInt64 = 0
    ) throws {
        try withCAuthTokens(authorizationTokens) { tp in
            var cfg = moq_fetch_update_cfg_t()
            moq_fetch_update_cfg_init(&cfg)
            if let p = subscriberPriority {
                cfg.has_subscriber_priority = true
                cfg.subscriber_priority = p
            }
            cfg.auth_tokens = tp.baseAddress
            cfg.auth_token_count = tp.count
            try MoQError.check(moq_session_update_fetch(raw, fetch.raw, &cfg, now))
        }
    }

    /// The Location window this side resolved a subscription's filter to.
    public func subscriptionWindow(_ subscription: Subscription) throws -> FilterWindow {
        var w = moq_filter_window_t()
        try MoQError.check(moq_session_subscription_window(
            raw, subscription.raw, &w, MemoryLayout<moq_filter_window_t>.size))
        return FilterWindow(w)
    }

    /// The window of a subscription a PUBLISH began, on either side.
    public func publicationWindow(_ publication: Publication) throws -> FilterWindow {
        var w = moq_filter_window_t()
        try MoQError.check(moq_session_publication_window(
            raw, publication.raw, &w, MemoryLayout<moq_filter_window_t>.size))
        return FilterWindow(w)
    }
}
