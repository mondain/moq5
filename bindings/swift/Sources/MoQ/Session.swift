import CMoQCore
import MoQTransportModel

/// Sans-I/O MoQ session. Not thread-safe. Not Sendable.
///
/// All mutating methods are "advancing calls" that invalidate
/// previously borrowed C pointers. The Swift bindings copy all
/// borrowed data into owned values before returning.
public final class Session {

    // MARK: - Configuration

    public struct Configuration: Sendable {
        public var perspective: Perspective
        public var sendRequestCapacity: Bool
        public var initialRequestCapacity: UInt64
        public var maxActions: UInt32
        public var maxEvents: UInt32
        /// The wire profile the session is created for; `nil` keeps the
        /// library default. Fixed for the session's lifetime.
        public var version: MoQTransportVersion?
        /// Explicit extended version; mutually exclusive with `version`.
        public var wireVersion: WireVersion?
        /// The fill pool: children a subscription may have open at once on
        /// either side; 0 selects the library default.
        public var maxFills: UInt32
        /// Surface subscribers' updates with `answerOwed` and answer them
        /// with `acceptSubscriptionUpdate` / `rejectSubscriptionUpdate` (or
        /// the publication pair) instead of the session answering itself.
        /// Switchable later with `setDeferSubscriptionUpdates(_:)`.
        public var deferSubscriptionUpdates: Bool
        /// Surface every standalone FETCH whose range is stated relative to
        /// the Largest Object unresolved (`FetchRequestOptions`) and answer it
        /// with the End the application resolved. Switchable later with
        /// `setDeferFetchRanges(_:)`.
        public var deferFetchRanges: Bool
        /// The SUBSCRIBE_TRACKS pool: requests open at once on either side;
        /// 0 selects the library default.
        public var maxTrackSubscriptions: UInt32
        /// Authorization tokens this side presents in its SETUP (the
        /// AUTHORIZATION TOKEN Setup Option), sent by value; copied by
        /// `init(configuration:)`.
        public var setupAuthorizationTokens: [AuthorizationToken]
        /// After this side sends GOAWAY, close with GOAWAY_TIMEOUT once this
        /// long has passed; on draft 18 and later the GOAWAY carries it as
        /// its Timeout (milliseconds). 0 disables it.
        public var goawayTimeoutMicroseconds: UInt64

        public init(perspective: Perspective) {
            self.perspective = perspective
            self.sendRequestCapacity = false
            self.initialRequestCapacity = 64
            self.maxActions = 64
            self.maxEvents = 16
            self.version = nil
            self.wireVersion = nil
            self.maxFills = 0
            self.deferSubscriptionUpdates = false
            self.deferFetchRanges = false
            self.maxTrackSubscriptions = 0
            self.setupAuthorizationTokens = []
            self.goawayTimeoutMicroseconds = 0
        }
    }

    public enum Perspective: Sendable {
        case client
        case server
    }

    public enum State: Sendable, Hashable {
        case idle
        case setupSent
        case established
        case draining
        case closed
        case unknown(Int32)
    }

    // MARK: - Transport version

    /// The MoQ transport draft this session speaks.
    ///
    /// Fixed when the session is created and immutable thereafter, so it is
    /// safe to read at any time and cannot go stale. It is the AUTHORITY for
    /// anything whose wire encoding depends on the draft -- notably the
    /// integer codec of object-property Key-Value-Pairs, which media parsing
    /// needs. `nil` means this build does not model the session's version,
    /// which callers must treat as "cannot encode/parse", never as draft-16.
    public var transportVersion: MoQTransportVersion? {
        switch moq_session_version(raw) {
        case MOQ_VERSION_DRAFT_16: return .draft16
        case MOQ_VERSION_DRAFT_18: return .draft18
        default: return nil
        }
    }

    public var wireVersion: WireVersion {
        WireVersion(rawValue: moq_session_version(raw).rawValue)
    }

    // MARK: - Storage

    internal let raw: OpaquePointer  // moq_session_t*
    private let owned: Bool
    /// A server's URI policy, borrowed by the session until it is destroyed.
    internal var uriPolicy: URIPolicyBox?

    // MARK: - Lifecycle

    public init(configuration: Configuration) throws {
        guard configuration.version == nil || configuration.wireVersion == nil else {
            throw MoQError.invalidArgument
        }
        /* Always-linked reference to the stack-guard canary: pulls its
         * object file into every consumer, so a dual link with the
         * installed-mode MoQService fails loudly (see StackGuard.swift). */
        _ = moqSwiftStackGuardSource()
        let alloc = moq_alloc_default()!
        var scfg = moq_session_cfg_t()
        // Sized init: this wrapper sets fields past the v0 prefix
        // (send_request_capacity, caps), so it must record the full struct_size
        // -- the pointer-only moq_session_cfg_init() would leave them ignored.
        moq_session_cfg_init_sized(
            &scfg, MemoryLayout<moq_session_cfg_t>.size, alloc,
            configuration.perspective == .client
                ? MOQ_PERSPECTIVE_CLIENT
                : MOQ_PERSPECTIVE_SERVER
        )
        scfg.send_request_capacity = configuration.sendRequestCapacity
        scfg.initial_request_capacity = configuration.initialRequestCapacity
        if configuration.maxActions > 0 {
            scfg.max_actions = configuration.maxActions
        }
        if configuration.maxEvents > 0 {
            scfg.max_events = configuration.maxEvents
        }
        if let v = configuration.version {
            scfg.version = v.cValue
        }
        if let v = configuration.wireVersion { scfg.version = v.cValue }
        scfg.max_fills = configuration.maxFills
        scfg.defer_subscription_updates = configuration.deferSubscriptionUpdates
        scfg.defer_fetch_ranges = configuration.deferFetchRanges
        scfg.max_track_subscriptions = configuration.maxTrackSubscriptions
        scfg.goaway_timeout_us = configuration.goawayTimeoutMicroseconds

        var ptr: OpaquePointer?
        let rc = withCAuthTokens(configuration.setupAuthorizationTokens) { tp -> moq_result_t in
            scfg.setup_auth_tokens = tp.baseAddress
            scfg.setup_auth_token_count = tp.count
            return moq_session_create(&scfg, 0, &ptr)
        }
        try MoQError.check(rc)
        guard let p = ptr else { throw MoQError.internal }
        self.raw = p
        self.owned = true
    }

    public convenience init(configuration: Configuration, wireVersion: WireVersion) throws {
        guard configuration.version == nil, configuration.wireVersion == nil else {
            throw MoQError.invalidArgument
        }
        var config = configuration
        config.wireVersion = wireVersion
        try self.init(configuration: config)
    }

    /// Wrap an externally-owned session without taking ownership.
    /// The caller (e.g. moq_pq_threaded_t) is responsible for the
    /// session's lifetime. This wrapper must not outlive the owner.
    package init(borrowing raw: OpaquePointer) {
        self.raw = raw
        self.owned = false
    }

    deinit {
        if owned {
            moq_session_destroy(raw)
        }
    }

    // MARK: - State

    public var state: State {
        let s = moq_session_state(raw)
        switch s {
        case MOQ_SESS_IDLE:        return .idle
        case MOQ_SESS_SETUP_SENT:  return .setupSent
        case MOQ_SESS_ESTABLISHED: return .established
        case MOQ_SESS_DRAINING:    return .draining
        case MOQ_SESS_CLOSED:      return .closed
        default:                   return .unknown(Int32(s.rawValue))
        }
    }

    // MARK: - Transport Input

    /// Start the session (client-side: sends CLIENT_SETUP).
    public func start() throws {
        try MoQError.check(moq_session_start(raw, 0))
    }

    /// Advance virtual time, fire deadlines.
    public func tick(now: UInt64) throws {
        try MoQError.check(moq_session_tick(raw, now))
    }

    /// Feed inbound control-stream bytes.
    public func receiveControlBytes(
        _ bytes: UnsafeRawBufferPointer, now: UInt64
    ) throws {
        try MoQError.check(
            moq_session_on_control_bytes(
                raw,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count, now))
    }

    /// Feed inbound data-stream bytes.
    public func receiveDataBytes(
        on stream: StreamRef,
        _ bytes: UnsafeRawBufferPointer,
        fin: Bool, now: UInt64
    ) throws {
        let ref = moq_stream_ref_t(_v: stream.rawValue)
        try MoQError.check(
            moq_session_on_data_bytes(
                raw, ref,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count, fin, now))
    }

    /// Feed inbound bytes on a bidirectional request stream (profiles that
    /// carry each request on its own stream).
    public func receiveBidiStreamBytes(
        on stream: StreamRef,
        _ bytes: UnsafeRawBufferPointer,
        fin: Bool, now: UInt64
    ) throws {
        let ref = moq_stream_ref_t(_v: stream.rawValue)
        try MoQError.check(
            moq_session_on_bidi_stream_bytes(
                raw, ref,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count, fin, now))
    }

    /// The peer reset its send half of a request stream.
    public func receiveBidiStreamReset(
        on stream: StreamRef, errorCode: UInt64, now: UInt64
    ) throws {
        try MoQError.check(moq_session_on_bidi_stream_reset(
            raw, moq_stream_ref_t(_v: stream.rawValue), errorCode, now))
    }

    /// The peer asked this side to stop sending on a request stream.
    public func receiveBidiStreamStop(
        on stream: StreamRef, errorCode: UInt64, now: UInt64
    ) throws {
        try MoQError.check(moq_session_on_bidi_stream_stop(
            raw, moq_stream_ref_t(_v: stream.rawValue), errorCode, now))
    }

    /// The peer reset a unidirectional data stream.
    public func receiveDataReset(
        on stream: StreamRef, errorCode: UInt64, now: UInt64
    ) throws {
        try MoQError.check(moq_session_on_data_reset(
            raw, moq_stream_ref_t(_v: stream.rawValue), errorCode, now))
    }

    /// The peer asked this side to stop sending on a unidirectional stream.
    public func receiveDataStop(
        on stream: StreamRef, errorCode: UInt64, now: UInt64
    ) throws {
        try MoQError.check(moq_session_on_data_stop(
            raw, moq_stream_ref_t(_v: stream.rawValue), errorCode, now))
    }

    // MARK: - Transport Binding

    /// The transport facts this session's profile needs before it starts
    /// (`MOQ_BINDING_*` bits); zero when it starts without one and accepts
    /// one optionally.
    public var bindingRequirements: UInt32 {
        moq_session_binding_requirements(raw)
    }

    /// Bind a raw-QUIC transport before `start()`. A client names its
    /// target (`authority`, optional `pathQuery`). Transport facts remain
    /// unknown; a server must supply an explicit URI policy via `bindTransport`.
    public func bindRawQUIC(
        authority: String?, pathQuery: String = ""
    ) throws {
        var binding = TransportBinding()
        binding.authority = authority
        binding.pathQuery = pathQuery
        try bindTransport(binding)
    }

    // MARK: - Namespace Publishing

    /// Announce a namespace to the peer.
    @discardableResult
    public func publishNamespace(
        _ namespace: Namespace, now: UInt64 = 0
    ) throws -> Announcement {
        try withCNamespace(namespace) { ns in
            var cfg = moq_publish_namespace_cfg_t()
            moq_publish_namespace_cfg_init(&cfg)
            cfg.track_namespace = ns

            var handle = moq_announcement_t(_opaque: 0)
            try MoQError.check(
                moq_session_publish_namespace(raw, &cfg, now, &handle))
            return Announcement(handle)
        }
    }

    /// Withdraw an established namespace announcement.
    public func publishNamespaceDone(
        _ announcement: Announcement, now: UInt64 = 0
    ) throws {
        try MoQError.check(
            moq_session_publish_namespace_done(raw, announcement.raw, now))
    }

    /// Accept an inbound namespace announcement from the peer.
    public func acceptNamespace(
        _ announcement: Announcement, now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_namespace_cfg_t()
        moq_accept_namespace_cfg_init(&cfg)
        try MoQError.check(
            moq_session_accept_namespace(raw, announcement.raw, &cfg, now))
    }

    /// Feed an inbound datagram.
    public func receiveDatagram(
        _ bytes: UnsafeRawBufferPointer, now: UInt64
    ) throws {
        try MoQError.check(
            moq_session_on_datagram(
                raw,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count, now))
    }

    // MARK: - Data Objects

    /// Open a subgroup data stream for writing objects.
    ///
    /// `firstObject` sets the subgroup header's FIRST_OBJECT bit: the first
    /// object written on this stream is the first object ever published in
    /// the subgroup. The Original Publisher sets it when it opens a new
    /// subgroup (drafts 18 and 22, section 2.2); draft-16 ignores it.
    @discardableResult
    public func openSubgroup(
        for subscription: Subscription,
        groupID: UInt64,
        subgroupID: UInt64 = 0,
        publisherPriority: UInt8 = 128,
        endOfGroup: Bool = false,
        objectProperties: Bool = false,
        firstObject: Bool = false,
        now: UInt64 = 0
    ) throws -> SubgroupHandle {
        var cfg = moq_subgroup_cfg_t()
        moq_subgroup_cfg_init_sized(&cfg, MemoryLayout<moq_subgroup_cfg_t>.size)
        cfg.group_id = groupID
        cfg.subgroup_id = subgroupID
        cfg.publisher_priority = publisherPriority
        cfg.end_of_group = endOfGroup
        cfg.object_properties = objectProperties
        cfg.first_object = firstObject

        var handle = moq_subgroup_handle_t(_opaque: 0)
        try MoQError.check(
            moq_session_open_subgroup(
                raw, subscription.raw, &cfg, now, &handle))
        return SubgroupHandle(handle)
    }

    /// Write a complete object to an open subgroup.
    /// The payload Buffer is retained by the session until the action
    /// is polled; the caller keeps its own reference.
    public func writeObject(
        to subgroup: SubgroupHandle,
        objectID: UInt64,
        payload: Buffer,
        now: UInt64 = 0
    ) throws {
        try MoQError.check(
            moq_session_write_object(
                raw, subgroup.raw, objectID, payload.raw, now))
    }

    /// Write an object with optional properties to an open subgroup.
    /// Both payload and properties Buffers are retained by the session
    /// until the actions are polled; the caller keeps its own references.
    public func writeObject(
        to subgroup: SubgroupHandle,
        _ config: ObjectConfiguration,
        now: UInt64 = 0
    ) throws {
        var cfg = moq_object_cfg_t()
        moq_object_cfg_init_sized(&cfg, MemoryLayout<moq_object_cfg_t>.size)
        cfg.object_id = config.objectID
        cfg.payload = config.payload.raw
        cfg.properties = config.properties?.raw
        if let g = config.priorGroupGap {
            cfg.has_prior_group_gap = true
            cfg.prior_group_gap = g
        }
        if let o = config.priorObjectGap {
            cfg.has_prior_object_gap = true
            cfg.prior_object_gap = o
        }
        try MoQError.check(
            moq_session_write_object_ex(raw, subgroup.raw, &cfg, now))
    }

    /// Close a subgroup stream (sends FIN).
    public func closeSubgroup(
        _ subgroup: SubgroupHandle, now: UInt64 = 0
    ) throws {
        try MoQError.check(
            moq_session_close_subgroup(raw, subgroup.raw, now))
    }

    // MARK: - Subscribe

    /// Initiate a subscription to a track.
    @discardableResult
    public func subscribe(
        _ config: SubscribeConfiguration, now: UInt64 = 0
    ) throws -> Subscription {
        try withCTrackName(config.track) { ns, name in
            var cfg = moq_subscribe_cfg_t()
            // The sized initializer stamps this struct's full size, so the
            // library reads `location` and `fill`; the unsized C name is a
            // macro Swift cannot see, and its exported function covers only
            // the original layout.
            moq_subscribe_cfg_init_sized(&cfg, MemoryLayout<moq_subscribe_cfg_t>.size)

            cfg.track_namespace = ns
            cfg.track_name = name

            switch config.filter {
            case .nextGroup:
                cfg.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP
            case .latestObject:
                cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT
            case .absoluteStart(let group, let object):
                cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START
                cfg.start_group = group
                cfg.start_object = object
            case .absoluteRange(let sg, let so, let eg):
                cfg.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE
                cfg.start_group = sg
                cfg.start_object = so
                cfg.end_group = eg
            case .unknown(let v):
                cfg.filter = v
            }

            if let priority = config.subscriberPriority {
                cfg.has_subscriber_priority = true
                cfg.subscriber_priority = priority
            }
            cfg.group_order = config.groupOrder.cValue

            if let timeout = config.objectDeliveryTimeoutMs {
                cfg.delivery_timeout_flags |= UInt32(MOQ_SUBSCRIBE_OBJECT_DELIVERY_TIMEOUT)
                cfg.object_delivery_timeout_ms = timeout
            }
            if let timeout = config.subgroupDeliveryTimeoutMs {
                cfg.delivery_timeout_flags |= UInt32(MOQ_SUBSCRIBE_SUBGROUP_DELIVERY_TIMEOUT)
                cfg.subgroup_delivery_timeout_ms = timeout
            }
            if let wait = config.rendezvousTimeoutMs {
                cfg.has_rendezvous_timeout = true
                cfg.rendezvous_timeout_ms = wait
            }
            if let include = config.includeProperties {
                cfg.has_include_properties = true
                cfg.include_properties = include
            }
            if let value = config.newGroupRequest {
                cfg.has_new_group_request = true
                cfg.new_group_request = value
            }

            var loc = moq_location_filter_cfg_t()
            var fillLoc = moq_location_filter_cfg_t()
            var fill = moq_fill_cfg_t()
            var handle = moq_subscription_t(_opaque: 0)
            return try withUnsafeMutablePointer(to: &loc) { locPtr in
                try withUnsafeMutablePointer(to: &fillLoc) { fillLocPtr in
                    try withUnsafeMutablePointer(to: &fill) { fillPtr in
                        if let l = config.location {
                            l.fill(locPtr)
                            cfg.location = UnsafePointer(locPtr)
                        }
                        if let f = config.fill {
                            f.fill(fillPtr, location: fillLocPtr)
                            cfg.fill = UnsafePointer(fillPtr)
                        }
                        try MoQError.check(
                            moq_session_subscribe(raw, &cfg, now, &handle))
                        return Subscription(handle)
                    }
                }
            }
        }
    }

    // MARK: - Fills (draft-22 section 3.4)

    /// Request a fill on an established subscription (a REQUEST_UPDATE
    /// carrying FILL_PARAMETERS). A new fill never cancels an earlier one.
    /// Throws `.unsupported` on a draft without fills, before anything is sent.
    public func requestFill(
        on subscription: Subscription, _ fill: FillConfiguration, now: UInt64 = 0
    ) throws {
        var ucfg = moq_subscription_update_cfg_t()
        moq_subscription_update_cfg_init_sized(
            &ucfg, MemoryLayout<moq_subscription_update_cfg_t>.size)
        var cfill = moq_fill_cfg_t()
        var loc = moq_location_filter_cfg_t()
        try withUnsafeMutablePointer(to: &loc) { locPtr in
            try withUnsafeMutablePointer(to: &cfill) { fillPtr in
                fill.fill(fillPtr, location: locPtr)
                ucfg.fill = UnsafePointer(fillPtr)
                try MoQError.check(moq_session_update_subscription(
                    raw, subscription.raw, &ucfg, now))
            }
        }
    }

    /// Publisher side: open the fill fetch stream with its header.
    public func openFill(_ fill: Fill, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_open_fill(raw, fill.raw, now))
    }

    /// Publisher side: write one fill-delivered object. The caller keeps its
    /// buffer references; queued actions hold their own.
    public func writeFillObject(
        _ fill: Fill, groupID: UInt64, subgroupID: UInt64 = 0,
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
        try MoQError.check(moq_session_write_fill_object(raw, fill.raw, &cfg, now))
    }

    /// Publisher side: mark a range the fill does not carry.
    public func writeFillRange(
        _ fill: Fill, kind: FetchRangeKind, groupID: UInt64, objectID: UInt64,
        now: UInt64 = 0
    ) throws {
        try MoQError.check(moq_session_write_fill_range(
            raw, fill.raw, kind.cValue, groupID, objectID, now))
    }

    /// Publisher side: finish the fill with a FIN.
    public func endFill(_ fill: Fill, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_end_fill(raw, fill.raw, now))
    }

    /// Publisher side: fail the fill on the wire (header, then RESET).
    public func abortFill(_ fill: Fill, errorCode: UInt64, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_abort_fill(raw, fill.raw, errorCode, now))
    }

    /// Subscriber side: stop one fill without touching the subscription.
    public func cancelFill(_ fill: Fill, now: UInt64 = 0) throws {
        try MoQError.check(moq_session_cancel_fill(raw, fill.raw, now))
    }

    /// Publisher side: a unilateral Forward change on a subscription it
    /// serves (PUBLISH_STATE_NOTIFY). Throws `.unsupported` on a draft
    /// without it.
    public func notifySubscriptionState(
        _ subscription: Subscription, forward: Bool, now: UInt64 = 0
    ) throws {
        try notifySubscriptionState(subscription, forward: forward, filter: nil, now: now)
    }

    /// Accept an inbound subscription request.
    public func acceptSubscribe(
        _ subscription: Subscription,
        config: AcceptSubscribeConfiguration = AcceptSubscribeConfiguration(),
        now: UInt64 = 0
    ) throws {
        var cfg = moq_accept_subscribe_cfg_t()
        moq_accept_subscribe_cfg_init(&cfg)

        if let expires = config.expiresMs {
            cfg.has_expires = true
            cfg.expires_ms = expires
        }
        if let largest = config.largest {
            cfg.has_largest = true
            cfg.largest_group = largest.group
            cfg.largest_object = largest.object
        }

        try withCBytes(config.trackProperties) { props in
            cfg.track_properties = props
            try MoQError.check(
                moq_session_accept_subscribe(raw, subscription.raw, &cfg, now))
        }
    }

    // MARK: - Output Polling

    /// Drain the legacy action view. Uni control bytes project to sendControl;
    /// reliable reset metadata requires the extended view. Request half-close
    /// operations cannot be represented here and fail a precondition instead
    /// of being silently discarded. New transport drivers use drainActionsExtended.
    public func drainActions(_ body: (Action) throws -> Void) rethrows {
        var buf = [moq_action_t](
            repeating: moq_action_t(), count: 16)
        while true {
            let n = moq_session_poll_actions(raw, &buf, 16)
            if n == 0 { break }
            let actions = (0..<n).compactMap { Action.from(&buf[$0]) }
            for action in actions { try body(action) }
        }
    }

    /// Convenience: collect all pending actions.
    public func pollActions() -> [Action] {
        var result: [Action] = []
        drainActions { result.append($0) }
        return result
    }

    /// Drain the legacy event view. New observations appear as `unknown(kind)`;
    /// use drainEventsExtended for their copied details.
    public func drainEvents(_ body: (Event) throws -> Void) rethrows {
        var buf = [moq_event_t](
            repeating: moq_event_t(), count: 16)
        while true {
            let n = moq_session_poll_events(raw, &buf, 16)
            if n == 0 { break }
            let events = (0..<n).map { Event.from(&buf[$0]) }
            for event in events { try body(event) }
        }
    }

    /// Convenience: collect all pending events.
    public func pollEvents() -> [Event] {
        var result: [Event] = []
        drainEvents { result.append($0) }
        return result
    }

    /// Complete actions, with copied bytes and owned payloads. Convert a whole
    /// batch before callbacks can advance the session or throw.
    public func drainActionsExtended(_ body: (ExtendedAction) throws -> Void) rethrows {
        var buf = [moq_action_t](repeating: moq_action_t(), count: 16)
        while true {
            let n = moq_session_poll_actions(raw, &buf, buf.count)
            if n == 0 { break }
            let actions = (0..<n).map { ExtendedAction.from(&buf[$0]) }
            for action in actions { try body(action) }
        }
    }

    public func pollActionsExtended() -> [ExtendedAction] {
        var result: [ExtendedAction] = []
        drainActionsExtended { result.append($0) }
        return result
    }

    public func drainEventsExtended(_ body: (ExtendedEvent) throws -> Void) rethrows {
        var buf = [moq_event_t](repeating: moq_event_t(), count: 16)
        while true {
            let n = moq_session_poll_events(raw, &buf, buf.count)
            if n == 0 { break }
            let events = (0..<n).map { ExtendedEvent.from(&buf[$0]) }
            for event in events { try body(event) }
        }
    }

    public func pollEventsExtended() -> [ExtendedEvent] {
        var result: [ExtendedEvent] = []
        drainEventsExtended { result.append($0) }
        return result
    }
}
