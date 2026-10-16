import Foundation
import Testing
import CMoQCore
@testable import MoQ

@Suite("Subscriber local failure")
struct SubscriberLocalFailureTests {
    @Test("Exact signed result, owner alternatives, unknown values and zero ID")
    func values() throws {
        let pointer = try #require(OpaquePointer(bitPattern: 0x1230))
        let track = SubscribedTrack(pointer)
        var c = moq_sub_local_failure_t()
        c.struct_size = UInt32(MemoryLayout.size(ofValue: c))
        c.track = pointer; c.fetch = OpaquePointer(bitPattern: 0x4560); c.fill._opaque = UInt64.max
        for owner in [UInt32(MOQ_SUB_LOCAL_FAILURE_TRACK), UInt32(MOQ_SUB_LOCAL_FAILURE_FETCH),
                      UInt32(MOQ_SUB_LOCAL_FAILURE_FILL), UInt32.max] {
            for code in [Int32.min, MOQ_ERR_BUFFER, 0, Int32.max] {
                for id in [UInt64(0), UInt64.max] {
                    c.owner_kind = owner; c.local_result = code; c.initiating_request_id = id
                    let v = try withUnsafeBytes(of: &c) {
                        try SubscriberLocalFailure.fromNative($0, tracksByRaw: [pointer: track])
                    }
                    #expect(v.localResult == code)
                    switch owner {
                    case UInt32(MOQ_SUB_LOCAL_FAILURE_TRACK):
                        #expect(v.ownerKind == .track && v.track === track)
                        #expect(v.fetchID == nil && v.fill == nil && v.initiatingRequestID == nil)
                    case UInt32(MOQ_SUB_LOCAL_FAILURE_FETCH):
                        #expect(v.ownerKind == .fetch && v.fetchID == 0x4560 && v.track == nil)
                        #expect(v.fill == nil && v.initiatingRequestID == nil)
                    case UInt32(MOQ_SUB_LOCAL_FAILURE_FILL):
                        #expect(v.ownerKind == .fill && v.track === track && v.fetchID == nil)
                        #expect(v.fill?.value == UInt64.max && v.initiatingRequestID == id)
                    default:
                        #expect(v.ownerKind == .other(owner) && v.track == nil && v.fetchID == nil)
                        #expect(v.fill == nil && v.initiatingRequestID == nil)
                    }
                }
            }
        }
        c.owner_kind = UInt32(MOQ_SUB_LOCAL_FAILURE_FILL)
        c.fill._opaque = 0
        let unknownTrack = try withUnsafeBytes(of: &c) {
            try SubscriberLocalFailure.fromNative($0, tracksByRaw: [:])
        }
        #expect(unknownTrack.track == nil && unknownTrack.fill == nil && unknownTrack.fetchID == nil)
        #expect(unknownTrack.initiatingRequestID == UInt64.max)
        c.owner_kind = UInt32(MOQ_SUB_LOCAL_FAILURE_FETCH); c.fetch = nil
        #expect(try withUnsafeBytes(of: &c) {
            try SubscriberLocalFailure.fromNative($0, tracksByRaw: [:])
        }.fetchID == nil)
    }

    @Test("Every v0 field requires physical and stamped coverage; future suffix ignored")
    func prefixes() throws {
        var c = moq_sub_local_failure_t()
        c.struct_size = UInt32(MemoryLayout.size(ofValue: c))
        c.owner_kind = UInt32(MOQ_SUB_LOCAL_FAILURE_FILL); c.local_result = MOQ_ERR_BUFFER
        c.fill._opaque = 42
        let floor = MemoryLayout<moq_sub_local_failure_t>.offset(of: \.initiating_request_id)! + 8
        withUnsafeBytes(of: &c) { bytes in
            for size in 0..<floor {
                let physical = Data(bytes.prefix(size))
                #expect(throws: MoQError.abiMismatch) {
                    try physical.withUnsafeBytes { try SubscriberLocalFailure.fromNative($0, tracksByRaw: [:]) }
                }
            }
        }
        for size in 0..<floor {
            c.struct_size = UInt32(size)
            #expect(throws: MoQError.abiMismatch) {
                try withUnsafeBytes(of: &c) { try SubscriberLocalFailure.fromNative($0, tracksByRaw: [:]) }
            }
        }
        c.struct_size = UInt32(MemoryLayout.size(ofValue: c) + 17)
        var future = withUnsafeBytes(of: &c) { Data($0) }
        future.append(contentsOf: repeatElement(UInt8(0xa5), count: 17))
        for size in [floor, MemoryLayout.size(ofValue: c), future.count] {
            let v = try future.prefix(size).withUnsafeBytes {
                try SubscriberLocalFailure.fromNative($0, tracksByRaw: [:])
            }
            #expect(v.fill?.value == 42 && v.initiatingRequestID == 0)
        }
    }

    @Test("The production C bridge propagates native INVAL, not empty or ABI mismatch")
    func nativeError() {
        #expect(throws: MoQError.invalidArgument) {
            try SubscriberLocalFailure.pollNative(nil, tracksByRaw: [:])
        }
    }

    private func makeSession(_ role: moq_perspective_t) throws -> OpaquePointer {
        var cfg = moq_session_cfg_t()
        moq_session_cfg_init_sized(&cfg, MemoryLayout.size(ofValue: cfg), moq_alloc_default(), role)
        cfg.version = MOQ_VERSION_DRAFT_22
        cfg.send_request_capacity = true; cfg.initial_request_capacity = 64
        cfg.max_object_payload_size = 32 * 1024 * 1024
        cfg.max_receive_buffer_bytes = 64 * 1024 * 1024
        var pointer: OpaquePointer?
        try MoQError.check(moq_session_create(&cfg, 0, &pointer))
        return try #require(pointer)
    }

    private func pump(_ client: Session, _ server: Session) throws {
        for _ in 0..<32 {
            var count = 0
            for (from, to, direction) in [(client, server, 0), (server, client, 1)] {
                func peer(_ ref: StreamRef) -> StreamRef {
                    let bits: [UInt64] = [1 << 40, 1 << 41]
                    let back = bits[direction ^ 1]
                    return StreamRef(rawValue: ref.rawValue & back != 0
                        ? ref.rawValue & ~back : ref.rawValue | bits[direction])
                }
                try from.drainActionsExtended { action in
                    count += 1
                    switch action {
                    case .sendControl(let data), .openUniControl(_, let data), .sendUniControl(_, let data, _):
                        try data.withUnsafeBytes { try to.receiveControlBytes($0, now: 0) }
                    case .sendData(let ref, let header, let payload, let fin):
                        var bytes = Data(header)
                        payload?.withUnsafeBytes { bytes.append(contentsOf: $0) }
                        try bytes.withUnsafeBytes { try to.receiveDataBytes(on: peer(ref), $0, fin: fin, now: 0) }
                    case .resetData(let ref, let code, _):
                        try to.receiveDataReset(on: peer(ref), errorCode: code, now: 0)
                    case .stopData(let ref, let code):
                        try to.receiveDataStop(on: peer(ref), errorCode: code, now: 0)
                    case .openBidiStream(let ref, let data, let fin), .sendBidiStream(let ref, let data, let fin):
                        try data.withUnsafeBytes { try to.receiveBidiStreamBytes(on: peer(ref), $0, fin: fin, now: 0) }
                    case .closeBidiStream(let ref):
                        try Data().withUnsafeBytes { try to.receiveBidiStreamBytes(on: peer(ref), $0, fin: true, now: 0) }
                    case .resetBidiStream(let ref, let code):
                        try to.receiveBidiStreamReset(on: peer(ref), errorCode: code, now: 0)
                    case .stopBidiStream(let ref, let code):
                        try to.receiveBidiStreamStop(on: peer(ref), errorCode: code, now: 0)
                    case .abortBidiStream(let ref, let code):
                        try to.receiveBidiStreamReset(on: peer(ref), errorCode: code, now: 0)
                        try to.receiveBidiStreamStop(on: peer(ref), errorCode: code, now: 0)
                    case .sendDatagram(let data):
                        try data.withUnsafeBytes { try to.receiveDatagram($0, now: 0) }
                    case .closeSession, .unknown:
                        throw MoQError.internal
                    }
                }
            }
            if count == 0 { return }
        }
        throw MoQError.internal
    }

    @Test("Native notice dispatch is exactly once and unlocks owner retirement", arguments: [1, 2, 3])
    func nativeRoute(owner: Int) throws {
        let clientRaw = try makeSession(MOQ_PERSPECTIVE_CLIENT)
        defer { moq_session_destroy(clientRaw) }
        let server = try makeSession(MOQ_PERSPECTIVE_SERVER)
        defer { moq_session_destroy(server) }
        let client = Session(borrowing: clientRaw)
        let peerSession = Session(borrowing: server)
        try bindInMemoryTransport(client, peerSession)
        try client.start(); try peerSession.start(); try pump(client, peerSession)
        _ = client.pollEventsExtended()
        var event = moq_event_t()
        while moq_session_poll_events(server, &event, 1) == 1 { moq_event_cleanup(&event) }
        try withExtendedLifetime(Subscriber(session: client)) { subscriber in
            #expect(try subscriber.pollLocalFailure() == nil)
            let name = TrackName(namespace: ["n21"], name: "video")
            let fill = owner == 3 ? FillConfiguration(location: .absoluteStart(group: 0, object: 0)) : nil
            let track = try subscriber.subscribe(track: name, location: owner == 3 ? .nextObject : nil, fill: fill)
            try pump(client, peerSession)
            var peer = moq_subscription_t()
            while moq_session_poll_events(server, &event, 1) == 1 {
                if event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST { peer = event.u.subscribe_request.sub }
                moq_event_cleanup(&event)
            }
            var accept = moq_accept_subscribe_cfg_t(); moq_accept_subscribe_cfg_init(&accept)
            accept.has_largest = true; accept.largest_group = 1; accept.largest_object = 1
            let properties: [UInt8] = [0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb]
            try properties.withUnsafeBytes { bytes in
                accept.track_properties = moq_bytes_t(data: bytes.baseAddress!.assumingMemoryBound(to: UInt8.self), len: bytes.count)
                try MoQError.check(moq_session_accept_subscribe(server, peer, &accept, 0))
            }
            try pump(client, peerSession); try subscriber.tick(now: 0)
            var child = moq_fill_t()
            var initiatingID = UInt64.max
            while moq_session_poll_events(server, &event, 1) == 1 {
                if event.kind == MOQ_EVENT_FILL_REQUEST {
                    child = event.u.fill_request.fill
                    initiatingID = event.u.fill_request.initiating_request_id
                }
                moq_event_cleanup(&event)
            }
            let token = subscriber.fill(of: track)
            if owner == 3 {
                let created = try #require(try subscriber.pollFill())
                guard case .created = created.kind else { Issue.record("missing CREATED"); return }
                #expect(initiatingID == 0 && created.track === track)
            }
            var fetch: OpaquePointer?
            var peerFetch = moq_fetch_t()
            func requestFetch() throws -> OpaquePointer {
                try withCTrackName(name) { ns, name in
                    var c = moq_sub_fetch_cfg_t(); moq_sub_fetch_cfg_init(&c)
                    c.track_namespace = ns; c.track_name = name
                    c.start_group = 1; c.end_group = 1; c.end_object = 2
                    var result: OpaquePointer?
                    try MoQError.check(moq_sub_fetch(subscriber.raw, &c, 0, &result))
                    return try #require(result)
                }
            }
            if owner == 2 {
                fetch = try requestFetch(); try pump(client, peerSession)
                while moq_session_poll_events(server, &event, 1) == 1 {
                    if event.kind == MOQ_EVENT_FETCH_REQUEST { peerFetch = event.u.fetch_request.fetch }
                    moq_event_cleanup(&event)
                }
                var c = moq_accept_fetch_cfg_t(); moq_accept_fetch_cfg_init(&c)
                c.end_group = 1; c.end_object = 2
                try MoQError.check(moq_session_accept_fetch(server, peerFetch, &c, 0))
            }
            // Keep the wrapper's default 16 MiB cap; retained ACK bytes make
            // this session-admissible object impossible for the facade.
            let data = Data(repeating: 0x61, count: 16 * 1024 * 1024 - 1)
            var payload: OpaquePointer?
            try data.withUnsafeBytes {
                try MoQError.check(moq_rcbuf_create(moq_alloc_default(), $0.baseAddress!.assumingMemoryBound(to: UInt8.self), $0.count, &payload))
            }
            defer { moq_rcbuf_decref(payload) }
            if owner == 1 {
                var c = moq_subgroup_cfg_t(); moq_subgroup_cfg_init(&c); c.group_id = 2
                var group = moq_subgroup_handle_t()
                try MoQError.check(moq_session_open_subgroup(server, peer, &c, 0, &group))
                var object = moq_object_cfg_t(); moq_object_cfg_init(&object); object.payload = payload
                try MoQError.check(moq_session_write_object_ex(server, group, &object, 0))
                try MoQError.check(moq_session_close_subgroup(server, group, 0))
            } else {
                var object = moq_fetch_object_cfg_t(); moq_fetch_object_cfg_init(&object)
                object.group_id = 1; object.payload = payload
                if owner == 3 {
                    try MoQError.check(moq_session_open_fill(server, child, 0))
                    try MoQError.check(moq_session_write_fill_object(server, child, &object, 0))
                    try MoQError.check(moq_session_end_fill(server, child, 0))
                } else {
                    try MoQError.check(moq_session_write_fetch_object(server, peerFetch, &object, 0))
                    try MoQError.check(moq_session_end_fetch(server, peerFetch, 0))
                }
            }
            try pump(client, peerSession)
            #expect(throws: MoQError.bufferTooSmall) { try subscriber.tick(now: 0) }
            try subscriber.tick(now: 0)
            if owner == 1 {
                #expect(moq_sub_release_track(subscriber.raw, track.raw) == MOQ_ERR_WRONG_STATE)
            } else if owner == 3 {
                let terminal = try #require(try subscriber.pollFill())
                guard case .terminal = terminal.kind else { Issue.record("missing terminal"); return }
                #expect(subscriber.fill(of: track) == token)
            } else {
                var item = moq_sub_fetch_item_t()
                while moq_sub_poll_fetch(subscriber.raw, &item) == MOQ_OK { moq_sub_fetch_item_cleanup(&item) }
                #expect(try requestFetch() != fetch)
            }
            let notice = try #require(try subscriber.pollLocalFailure())
            #expect(notice.localResult == MOQ_ERR_BUFFER)
            #expect(try subscriber.pollLocalFailure() == nil)
            switch owner {
            case 1:
                #expect(notice.ownerKind == .track && notice.track === track)
                #expect(notice.fill == nil && notice.fetchID == nil && notice.initiatingRequestID == nil)
                #expect(moq_sub_release_track(subscriber.raw, track.raw) == MOQ_OK)
            case 3:
                #expect(notice.ownerKind == .fill && notice.track === track)
                #expect(notice.fill == token && notice.initiatingRequestID == 0 && notice.fetchID == nil)
                #expect(subscriber.fill(of: track) == nil && track.isActive)
            default:
                #expect(notice.ownerKind == .fetch && notice.fetchID == fetch.map { UInt(bitPattern: $0) })
                #expect(notice.track == nil && notice.fill == nil && notice.initiatingRequestID == nil)
                #expect(try requestFetch() == fetch)
            }
            #expect(client.state == .established)
        }
    }
}
