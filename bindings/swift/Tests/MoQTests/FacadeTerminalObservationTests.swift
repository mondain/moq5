import Foundation
import Testing
import CMoQCore
import CMoQSim
import MoQTransportModel
@testable import MoQ

@Suite("Facade terminal observations")
struct FacadeTerminalObservationTests {
    private final class Trace { var stream: StreamRef? }
    private func pump(_ pair: OpaquePointer) throws {
        try MoQError.check(moq_simpair_run_until_quiescent(pair, 32, nil))
    }
    private func withPair(_ body: (OpaquePointer, Session, Session, Trace) throws -> Void) throws {
        let trace = Trace()
        var cfg = moq_simpair_cfg_t()
        cfg.struct_size = UInt32(MemoryLayout.size(ofValue: cfg)); cfg.alloc = moq_alloc_default()
        cfg.version = MOQ_VERSION_DRAFT_22
        cfg.client_initial_request_capacity = 128; cfg.server_initial_request_capacity = 128
        cfg.client_send_request_capacity = true; cfg.server_send_request_capacity = true
        cfg.trace_ctx = Unmanaged.passUnretained(trace).toOpaque()
        cfg.trace_fn = { ctx, r in
            guard let ctx, let r else { return }
            if r.pointee.kind == MOQ_SIM_TRACE_ACTION && r.pointee.from == MOQ_PERSPECTIVE_CLIENT &&
                r.pointee.action_kind == MOQ_ACTION_OPEN_BIDI_STREAM {
                Unmanaged<Trace>.fromOpaque(ctx).takeUnretainedValue().stream = StreamRef(rawValue: r.pointee.stream_ref._v)
            }
        }
        var pointer: OpaquePointer?
        try MoQError.check(moq_simpair_create(&cfg, &pointer))
        let pair = try #require(pointer)
        defer { moq_simpair_destroy(pair); withExtendedLifetime(trace) {} }
        let client = Session(borrowing: try #require(moq_simpair_client(pair)))
        let server = Session(borrowing: try #require(moq_simpair_server(pair)))
        try MoQError.check(moq_simpair_start(pair)); try pump(pair)
        _ = client.pollEventsExtended(); _ = server.pollEventsExtended()
        try body(pair, client, server, trace)
    }

    @Test("Actual namespace callback owns bytes and exact RESET/STOP codes", arguments: [false, true], [UInt64(0), UInt64.max])
    func namespaceSignal(stop: Bool, code: UInt64) throws {
        for accepted in [false, true] {
            try withPair { pair, client, server, trace in
                var observations: [Result<PublisherNamespaceTerminalObservation, MoQError>] = []
                do {
                    let publisher = try Publisher(session: client)
                    publisher.onNamespaceTerminal = { observations.append($0) }
                    _ = try publisher.addTrack(namespace: ["n24"], name: "v")
                    _ = try publisher.addTrack(namespace: ["n24"], name: "a")
                    try pump(pair)
                    var event = moq_event_t()
                    try #require(moq_session_poll_events(server.raw, &event, 1) == 1)
                    try #require(event.kind == MOQ_EVENT_NAMESPACE_PUBLISHED)
                    if accepted {
                        var ac = moq_accept_namespace_cfg_t(); moq_accept_namespace_cfg_init(&ac)
                        try MoQError.check(moq_session_accept_namespace(server.raw, event.u.namespace_published.ann, &ac, 1))
                    }
                    moq_event_cleanup(&event); try pump(pair); try publisher.tick(now: 1)
                    let stream = try #require(trace.stream)
                    if stop { try client.receiveBidiStreamStop(on: stream, errorCode: code, now: 1) }
                    else { try client.receiveBidiStreamReset(on: stream, errorCode: code, now: 1) }
                    try publisher.tick(now: 1); try publisher.tick(now: 1)
                    #expect(client.state == .established)
                }
                try #require(observations.count == 1)
                let value = try observations[0].get()
                #expect(value.kind == .terminated && value.namespace == Namespace(strings: "n24"))
                #expect(value.cause == (stop ? .peerStopSending : .peerResetStream))
                #expect(value.localRole == .requester && value.streamErrorCode == code)
                #expect(value.errorCode == 0 && !value.canRetry && value.retryAfterMs == 0 && value.reason.isEmpty)
            }
        }
    }

    @Test("Old poll refuses same head, new poll consumes it and frees the slot", arguments: [UInt64(0), UInt64.max])
    func statusRoute(code: UInt64) throws {
        try withPair { pair, client, server, trace in
            let subscriber = try Subscriber(session: client)
            let track = TrackName(namespace: ["n24"], name: "v")
            // SimPair's peer status pool is eight. Keep one terminal head and
            // fill the remaining facade slots with completed OK observations.
            // The C++ fixture separately holds sixteen terminal observations
            // with an explicitly larger public native peer pool.
            var requests: [SubscriberStatusRequest] = []
            for index in 0..<16 {
                requests.append(try subscriber.trackStatus(track: track))
                try pump(pair)
                var event = moq_event_t()
                try #require(moq_session_poll_events(server.raw, &event, 1) == 1)
                try #require(event.kind == MOQ_EVENT_TRACK_STATUS_REQUEST)
                if index == 0 {
                    try client.receiveBidiStreamReset(on: #require(trace.stream), errorCode: code, now: 1)
                } else {
                    var ac = moq_accept_track_status_cfg_t(); moq_accept_track_status_cfg_init(&ac)
                    try MoQError.check(moq_session_accept_track_status(server.raw, event.u.track_status_request.handle, &ac, 1))
                }
                moq_event_cleanup(&event); try pump(pair)
                try subscriber.tick(now: 1); try pump(pair)
            }
            #expect(throws: MoQError.wouldBlock) { try subscriber.trackStatus(track: track) }
            var old = moq_sub_status_result_t()
            #expect(moq_sub_poll_status(subscriber.raw, &old) == MOQ_ERR_UNSUPPORTED)
            #expect(moq_sub_poll_status(subscriber.raw, &old) == MOQ_ERR_UNSUPPORTED)
            #expect(throws: MoQError.wouldBlock) { try subscriber.trackStatus(track: track) }
            let first = try #require(try subscriber.pollStatusOutcome())
            #expect(first.request == requests[0] && first.kind == .terminated)
            #expect(first.cause == .peerResetStream && first.localRole == .requester && first.streamErrorCode == code)
            #expect(first.errorCode == 0 && first.largest == nil && first.expiresMs == nil && !first.canRetry)
            let reused = try subscriber.trackStatus(track: track)
            #expect(reused == requests[0]); try pump(pair)
            var event = moq_event_t()
            try #require(moq_session_poll_events(server.raw, &event, 1) == 1)
            try #require(event.kind == MOQ_EVENT_TRACK_STATUS_REQUEST)
            var ac = moq_accept_track_status_cfg_t(); moq_accept_track_status_cfg_init(&ac)
            ac.has_largest = true
            try MoQError.check(moq_session_accept_track_status(server.raw, event.u.track_status_request.handle, &ac, 1))
            moq_event_cleanup(&event); try pump(pair); try subscriber.tick(now: 1)
            for request in requests.dropFirst() {
                let out = try #require(try subscriber.pollStatusOutcome())
                #expect(out.request == request && out.kind == .ok && out.streamErrorCode == 0)
            }
            let ok = try #require(try subscriber.pollStatusOutcome())
            #expect(ok.kind == .ok && ok.request == reused && ok.largest?.group == 0 && ok.largest?.object == 0)
            #expect(ok.expiresMs == nil && ok.cause == nil && ok.localRole == nil)
            #expect(try subscriber.pollStatusOutcome() == nil)
        }
    }

    @Test("Every scalar needs complete physical and stamped coverage")
    func prefixes() throws {
        var c = moq_pub_namespace_terminal_info_t()
        c.struct_size = UInt32(MemoryLayout.size(ofValue: c))
        c.kind = MOQ_PUB_NAMESPACE_TERMINATED
        c.cause = UInt32(MOQ_NAMESPACE_TERMINAL_TRUNCATED_MESSAGE)
        c.local_role = UInt32(MOQ_NAMESPACE_ROLE_REQUESTER)
        c.stream_error_code = UInt64.max
        try withUnsafeBytes(of: &c) { bytes in
            for n in 0..<bytes.count {
                let physical = Data(bytes.prefix(n))
                #expect(throws: MoQError.abiMismatch) {
                    try physical.withUnsafeBytes { try PublisherNamespaceTerminalObservation.fromNative($0) }
                }
            }
            let full = try PublisherNamespaceTerminalObservation.fromNative(bytes)
            #expect(full.cause == .truncatedMessage && full.localRole == .requester)
            #expect(full.streamErrorCode == UInt64.max && full.errorCode == 0)
        }
        c.kind = MOQ_PUB_NAMESPACE_REJECTED
        let old = MemoryLayout<moq_pub_namespace_terminal_info_t>.offset(of: \.cause)!
        c.struct_size = UInt32(old)
        c.error_code = UInt64(MOQ_REQUEST_ERROR_UNAUTHORIZED)
        try withUnsafeBytes(of: &c) { bytes in
            let value = try PublisherNamespaceTerminalObservation.fromNative(UnsafeRawBufferPointer(rebasing: bytes[..<old]))
            #expect(value.kind == .rejected && value.cause == nil && value.localRole == nil)
            #expect(value.streamErrorCode == 0 && value.errorCode == UInt64(MOQ_REQUEST_ERROR_UNAUTHORIZED))
        }
        var status = moq_sub_status_outcome_t()
        status.struct_size = UInt32(MemoryLayout.size(ofValue: status))
        status.kind = UInt32(MOQ_SUB_STATUS_OUTCOME_TERMINATED)
        status.cause = UInt32(MOQ_TRACK_STATUS_PEER_STOP_SENDING)
        status.local_role = UInt32(MOQ_TRACK_STATUS_REQUESTER)
        status.stream_error_code = UInt64.max
        try withUnsafeBytes(of: &status) { bytes in
            for n in 0..<bytes.count {
                let physical = Data(bytes.prefix(n))
                #expect(throws: MoQError.abiMismatch) {
                    try physical.withUnsafeBytes { try SubscriberStatusOutcome.fromNative($0) }
                }
            }
            let value = try SubscriberStatusOutcome.fromNative(bytes)
            #expect(value.cause == .peerStopSending && value.localRole == .requester)
            #expect(value.streamErrorCode == UInt64.max && value.largest == nil && value.expiresMs == nil)
        }
        for size in 0..<MemoryLayout.size(ofValue: status) {
            status.struct_size = UInt32(size)
            #expect(throws: MoQError.abiMismatch) {
                try withUnsafeBytes(of: &status) { try SubscriberStatusOutcome.fromNative($0) }
            }
        }
        status.struct_size = UInt32(MemoryLayout.size(ofValue: status) + 8)
        status.kind = UInt32(MOQ_SUB_STATUS_OUTCOME_ERROR)
        status.flags = UInt32(MOQ_SUB_STATUS_OUTCOME_CAN_RETRY)
        status.error_code = UInt64.max
        var future = withUnsafeBytes(of: &status) { Data($0) }
        future.append(contentsOf: repeatElement(UInt8(0xa5), count: 8))
        let value = try future.withUnsafeBytes { try SubscriberStatusOutcome.fromNative($0) }
        #expect(value.kind == .error && value.errorCode == UInt64.max && value.canRetry && value.retryAfterMs == 0)
        #expect(value.cause == nil && value.localRole == nil && value.largest == nil && value.expiresMs == nil)
        c.kind = MOQ_PUB_NAMESPACE_TERMINATED
        for size in 0..<MemoryLayout.size(ofValue: c) {
            c.struct_size = UInt32(size)
            #expect(throws: MoQError.abiMismatch) {
                try withUnsafeBytes(of: &c) { try PublisherNamespaceTerminalObservation.fromNative($0) }
            }
        }
        c.struct_size = UInt32(MemoryLayout.size(ofValue: c) + 8)
        future = withUnsafeBytes(of: &c) { Data($0) }
        future.append(contentsOf: repeatElement(UInt8(0xa5), count: 8))
        #expect(try future.withUnsafeBytes { try PublisherNamespaceTerminalObservation.fromNative($0) }.cause == .truncatedMessage)
    }
}
