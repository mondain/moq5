import Foundation
import Testing
import CMoQCore
import CMoQSim
import MoQTransportModel
@testable import MoQ

@Suite("Subscriber initial ACK result")
struct SubscriberTrackResultTests {
    private final class Trace {
        var requestStream: StreamRef?
    }

    private func pump(_ pair: OpaquePointer) throws {
        try MoQError.check(moq_simpair_run_until_quiescent(pair, 16, nil))
    }

    private func withPair<T>(
        _ version: WireVersion,
        _ body: (OpaquePointer, Session, Session, Trace) throws -> T
    ) throws -> T {
        let trace = Trace()
        var cfg = moq_simpair_cfg_t()
        cfg.struct_size = UInt32(MemoryLayout<moq_simpair_cfg_t>.size)
        cfg.alloc = moq_alloc_default()
        cfg.version = version.cValue
        cfg.client_send_request_capacity = true
        cfg.server_send_request_capacity = true
        cfg.client_initial_request_capacity = 16
        cfg.server_initial_request_capacity = 16
        cfg.trace_ctx = Unmanaged.passUnretained(trace).toOpaque()
        cfg.trace_fn = { ctx, record in
            guard let ctx, let record else { return }
            let r = record.pointee
            if r.kind == MOQ_SIM_TRACE_ACTION && r.from == MOQ_PERSPECTIVE_CLIENT &&
                r.action_kind == MOQ_ACTION_OPEN_BIDI_STREAM {
                Unmanaged<Trace>.fromOpaque(ctx).takeUnretainedValue().requestStream =
                    StreamRef(rawValue: r.stream_ref._v)
            }
        }
        var pointer: OpaquePointer?
        try MoQError.check(moq_simpair_create(&cfg, &pointer))
        let pair = try #require(pointer)
        defer {
            moq_simpair_destroy(pair)
            withExtendedLifetime(trace) {}
        }
        let client = Session(borrowing: try #require(moq_simpair_client(pair)))
        let server = Session(borrowing: try #require(moq_simpair_server(pair)))
        try MoQError.check(moq_simpair_start(pair))
        try pump(pair)
        #expect(client.state == .established && server.state == .established)
        _ = client.pollEventsExtended()
        _ = server.pollEventsExtended()
        // Body-local facades must be destroyed before the pair's borrowed sessions.
        return try body(pair, client, server, trace)
    }

    private func subscribe(
        _ subscriber: Subscriber, _ pair: OpaquePointer, _ server: Session,
        name: String = "initial"
    ) throws -> (SubscribedTrack, Subscription) {
        let track = try subscriber.subscribe(
            track: .init(namespace: ["ack"], name: name), filter: .latestObject)
        try pump(pair)
        let request = try #require(server.pollEventsExtended().compactMap { event -> SubscribeRequest? in
            if case .subscribeRequest(let value) = event { return value.legacy }; return nil
        }.first)
        return (track, request.subscription)
    }

    private func accept(
        _ server: Session, _ subscription: Subscription,
        largest: (UInt64, UInt64)? = nil, expiry: UInt64? = nil,
        properties: Data = Data()
    ) throws {
        var cfg = moq_accept_subscribe_cfg_t()
        moq_accept_subscribe_cfg_init(&cfg)
        if let largest {
            cfg.has_largest = true
            cfg.largest_group = largest.0
            cfg.largest_object = largest.1
        }
        if let expiry {
            cfg.has_expires = true
            cfg.expires_ms = expiry
        }
        try properties.withUnsafeBytes { bytes in
            cfg.track_properties.data = bytes.bindMemory(to: UInt8.self).baseAddress
            cfg.track_properties.len = bytes.count
            let rc = moq_session_accept_subscribe(server.raw, subscription.raw, &cfg, 0)
            try #require(rc == MOQ_OK, "Native accept-subscribe returned \(rc)")
        }
    }

    private func properties(_ version: WireVersion) -> Data {
        // Application properties 120/121, with a heap-sized opaque byte value.
        let prefix: [UInt8] = version == .draft16 ? [0x40, 0x78] : [0x78]
        return Data(prefix + [0x01, 0x01, 0x20] + Array(UInt8(0)..<32))
    }

    @Test("Query distinguishes pending from accepted empty without pumping",
          arguments: [WireVersion.draft16, .draft18, .draft22])
    func pendingResult(_ version: MoQTransportModel.WireVersion) throws {
        try withPair(version) { pair, client, server, _ in
            let subscriber = try Subscriber(session: client)
            let (track, request) = try subscribe(subscriber, pair, server)
            let pending = try subscriber.trackResult(track)
            #expect(pending.state == .pending && !pending.responseSeen)
            #expect(pending.largest == nil && pending.expiresMs == nil)
            #expect(pending.trackProperties.isEmpty)
            try accept(server, request)
            try pump(pair)
            // ACK is queued at the core, but observation must not consume it.
            #expect(try subscriber.trackResult(track) == pending)
            #expect(track.state == .pending)
            try subscriber.tick(now: 0)
            let accepted = try subscriber.trackResult(track)
            #expect(accepted.state == .active && accepted.responseSeen)
            #expect(accepted.largest == nil && accepted.expiresMs == nil)
            #expect(accepted.trackProperties.isEmpty)
            #expect(accepted != pending)
        }
    }

    @Test("Initial scalar and property snapshots survive advancement and destruction",
          arguments: [WireVersion.draft16, .draft18, .draft22],
                     [nil, 0, 17] as [UInt64?])
    func ownedSnapshot(_ version: MoQTransportModel.WireVersion, _ expiry: UInt64?) throws {
        weak var facade: Subscriber?
        weak var session: Session?
        let expectedProperties = properties(version)
        let saved = try withPair(version) { pair, client, server, _ in
            let subscriber = try Subscriber(session: client)
            facade = subscriber
            session = client
            let (track, request) = try subscribe(subscriber, pair, server)
            try accept(server, request, largest: (0, 0), expiry: expiry,
                       properties: expectedProperties)
            try pump(pair)
            try subscriber.tick(now: 0)
            let initial = try subscriber.trackResult(track)
            #expect(initial.state == .active && initial.responseSeen)
            #expect(initial.largest?.group == 0 && initial.largest?.object == 0)
            // Draft16 encodes zero expiry as omission; retain the actual native observation.
            let expectedExpiry = version == .draft16 && expiry == 0 ? nil : expiry
            #expect(initial.expiresMs == expectedExpiry)
            #expect(initial.trackProperties == expectedProperties)

            let group = try server.openSubgroup(for: request, groupID: 1)
            try server.writeObject(to: group, objectID: 2, payload: Buffer(Data([1, 2, 3])))
            try server.closeSubgroup(group)
            try pump(pair)
            try subscriber.tick(now: 1)
            #expect(try subscriber.trackResult(track) == initial)
            // The query must not drain the received-object queue either.
            let object = try #require(try subscriber.pollObject())
            #expect(object.groupID == 1 && object.objectID == 2)

            let (_, second) = try subscribe(subscriber, pair, server, name: "later")
            try accept(server, second, properties: Data())
            try pump(pair)
            try subscriber.tick(now: 1)
            var edited = initial.trackProperties
            edited[0] ^= 0xff
            #expect(edited != initial.trackProperties)
            #expect(try subscriber.trackResult(track) == initial)
            try MoQError.check(moq_sub_unsubscribe(subscriber.raw, track.raw, 1))
            let terminal = try subscriber.trackResult(track)
            #expect(terminal.state == .done && terminal.responseSeen)
            #expect(terminal.largest?.group == 0 && terminal.largest?.object == 0)
            #expect(terminal.expiresMs == initial.expiresMs)
            #expect(terminal.trackProperties == initial.trackProperties)
            return initial
        }
        #expect(facade == nil && session == nil)
        #expect(saved.state == .active && saved.responseSeen)
        #expect(saved.trackProperties == expectedProperties)
        func requireSendable<T: Sendable>(_ value: T) {}
        requireSendable(saved)
    }

    @Test("Inbound D22 ACK retains full-width Largest and expiry")
    func fullWidthInbound() throws {
        let saved: SubscribedTrackResult = try withPair(.draft22) { pair, client, server, trace in
            let subscriber = try Subscriber(session: client)
            let (track, _) = try subscribe(subscriber, pair, server)
            let stream = try #require(trace.requestStream)
            // Valid ordinary ACK with full-width Largest and expiry values.
            // The sender API's separate expiry ceiling must not limit
            // observation.
            var bytes: [UInt8] = [0x04, 0x00, 0x26, 0x01, 0x02, 0x08]
            bytes.append(contentsOf: repeatElement(UInt8.max, count: 9))
            bytes.append(0x01)
            bytes.append(contentsOf: repeatElement(UInt8.max, count: 18))
            bytes.append(contentsOf: [0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb])
            try bytes.withUnsafeBytes {
                try client.receiveBidiStreamBytes(on: stream, $0, fin: false, now: 0)
            }
            #expect(!(try subscriber.trackResult(track)).responseSeen)
            try subscriber.tick(now: 0)
            return try subscriber.trackResult(track)
        }
        #expect(saved.state == .active && saved.responseSeen)
        #expect(saved.largest?.group == UInt64.max && saved.largest?.object == UInt64.max)
        #expect(saved.expiresMs == UInt64.max)
        #expect(saved.trackProperties == Data([0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb]))
    }

    @Test("Foreign owner throws the native invalidArgument error")
    func wrongOwner() throws {
        try withPair(.draft22) { pair, client, server, _ in
            let subscriber = try Subscriber(session: client)
            let (track, _) = try subscribe(subscriber, pair, server)
            let other = try Subscriber(session: server)
            #expect(throws: MoQError.invalidArgument) { try other.trackResult(track) }
            #expect(!(try subscriber.trackResult(track)).responseSeen)
        }
        // The wrapper delegates native failures through this existing mapping;
        // the native subscriber tests own allocator/capacity failure and
        // terminal progress.
        #expect(throws: MoQError.outOfMemory) { try MoQError.check(MOQ_ERR_NOMEM) }
        #expect(throws: MoQError.bufferTooSmall) { try MoQError.check(MOQ_ERR_BUFFER) }
    }

    @Test("A peer rejection is a current error state, not an initial ACK")
    func rejectedResult() throws {
        try withPair(.draft22) { pair, client, server, _ in
            let subscriber = try Subscriber(session: client)
            let (track, request) = try subscribe(subscriber, pair, server)
            var cfg = moq_reject_subscribe_cfg_t()
            moq_reject_subscribe_cfg_init(&cfg)
            cfg.error_code = 1
            try MoQError.check(moq_session_reject_subscribe(server.raw, request.raw, &cfg, 0))
            try pump(pair)
            try subscriber.tick(now: 0)
            let result = try subscriber.trackResult(track)
            #expect(result.state == .error && !result.responseSeen)
            #expect(result.largest == nil && result.expiresMs == nil && result.trackProperties.isEmpty)
        }
    }
}
