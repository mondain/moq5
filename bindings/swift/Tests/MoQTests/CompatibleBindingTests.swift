import Foundation
import Testing
import CMoQCore
import MoQTransportModel
@testable import MoQ

// The synchronous in-memory pumps implement stream and datagram delivery.
func bindInMemoryTransport(_ client: Session, _ server: Session) throws {
    var cb = TransportBinding(factsKnown: .all, factsSupported: .all)
    cb.authority = "sim.invalid"
    var sb = TransportBinding(factsKnown: .all, factsSupported: .all)
    sb.acceptURI = { _, _ in .accept }
    try client.bindTransport(cb)
    try server.bindTransport(sb)
}

@Suite("Compatible binding")
struct CompatibleBindingTests {
    // Deliberately exhaustive baseline cases, with no default or unknown-default.
    private func oldEvent(_ event: Event) -> Int {
        switch event {
        case .setupComplete: return 0
        case .sessionClosed(let code): return Int(code)
        case .subscribeRequest: return 2
        case .subscribeOk: return 3
        case .subscribeError: return 4
        case .namespacePublished: return 5
        case .namespaceAccepted: return 6
        case .namespaceRejected: return 7
        case .namespaceDone: return 8
        case .objectReceived: return 9
        case .subgroupFinished: return 10
        case .unknown: return 11
        }
    }

    private func oldError(_ error: MoQError) -> Bool {
        switch error {
        case .invalidArgument, .outOfMemory, .bufferTooSmall, .protocolViolation,
             .wrongState, .staleHandle, .wouldBlock, .sessionClosed,
             .requestBlocked, .abiMismatch, .goaway, .internal, .unknown: return true
        }
    }

    private func oldVersion(_ version: MoQTransportVersion) -> Int {
        switch version {
        case .draft16: return 16
        case .draft18: return 18
        }
    }

    @Test func oldEnumsAndExplicitVersions() throws {
        #expect(oldEvent(.setupComplete) == 0)
        #expect(oldError(.unsupported))
        #expect(MoQTransportVersion.allCases.map(oldVersion) == [16, 18])
        #expect(WireVersion.draft22.rawValue == MOQ_VERSION_DRAFT_22.rawValue)
        let config = Session.Configuration(perspective: .client)
        let legacy = try Session(configuration: config)
        #expect(legacy.wireVersion == .draft16)
        let extended = try Session(configuration: config, wireVersion: .draft22)
        #expect(extended.wireVersion == .draft22)
        #expect(extended.transportVersion == nil)
        #expect(Session.supportedVersions == [.draft18, .draft16])
        #expect(Session.supportedWireVersions.contains(.draft22))
        #expect(!Session.isWireVersionSupported(WireVersion(rawValue: 999)))
    }

    @Test func reliableResetSizeAndLegacyProjection() {
        var action = moq_action_t()
        action.kind = MOQ_ACTION_RESET_DATA
        action.detail_size = UInt32(MemoryLayout<moq_reset_data_action_t>.size)
        action.u.reset_data.stream_ref._v = 3
        action.u.reset_data.error_code = 7
        action.u.reset_data.reliable_size = .max
        let saved = action
        guard case .resetData(let stream, let code, let reliable) = ExtendedAction.from(&action) else {
            Issue.record("Missing extended reset"); return
        }
        #expect(stream.rawValue == 3 && code == 7 && reliable == .max)
        action = saved
        guard case .resetData(let oldStream, let oldCode) = Action.from(&action) else {
            Issue.record("Missing legacy reset"); return
        }
        #expect(oldStream == stream && oldCode == code)
        action = saved
        action.detail_size = 16
        guard case .resetData(_, _, let absent) = ExtendedAction.from(&action) else {
            Issue.record("Missing old-prefix reset"); return
        }
        #expect(absent == 0)
    }

    @Test func copiedUpdateOptions() {
        var options = moq_subscription_update_options_t()
        options.struct_size = UInt32(MemoryLayout.size(ofValue: options))
        options.flags = 7
        options.initiating_request_id = 91
        LocationFilter.objectEnd(startGroup: 1, startObject: 2, endGroup: 3, endObject: .max)
            .fill(&options.location)
        options.object_delivery_timeout_ms = 0
        options.subgroup_delivery_timeout_ms = .max
        var event = moq_event_t()
        event.kind = MOQ_EVENT_SUBSCRIBE_UPDATED
        event.detail_size = UInt32(MemoryLayout<moq_subscribe_updated_event_t>.size)
        let converted = withUnsafePointer(to: &options) { ptr in
            event.u.subscribe_updated.options = ptr
            return ExtendedEvent.from(&event)
        }
        options = moq_subscription_update_options_t()
        guard case .subscribeUpdated(let value) = converted else {
            Issue.record("Missing update"); return
        }
        #expect(value.update.options?.initiatingRequestID == 91)
        #expect(value.update.options?.objectDeliveryTimeoutMs == 0)
        #expect(value.update.options?.subgroupDeliveryTimeoutMs == .max)
        #expect(value.update.options?.location == .objectEnd(
            startGroup: 1, startObject: 2, endGroup: 3, endObject: .max))
        #expect(value.update.filter == nil && value.update.deliveryTimeoutUs == nil)
    }

    @Test func bindingRequiresMeasuredFactsAndPolicy() throws {
        let client = try Session(configuration: .init(perspective: .client), wireVersion: .draft22)
        var binding = TransportBinding()
        #expect(binding.factsKnown.isEmpty && binding.factsSupported.isEmpty)
        binding.authority = "sim.invalid"
        #expect(throws: MoQError.wouldBlock) { try client.bindTransport(binding) }
        binding.factsKnown = .all
        #expect(throws: MoQError.unsupported) { try client.bindTransport(binding) }
        let server = try Session(configuration: .init(perspective: .server), wireVersion: .draft22)
        let withoutPolicy = TransportBinding(factsKnown: .all, factsSupported: .all)
        #expect(throws: MoQError.invalidArgument) { try server.bindTransport(withoutPolicy) }
    }

    @Test func uriPolicyLifetime() throws {
        final class Token {}
        weak var retained: Token?
        var server: Session? = try Session(
            configuration: .init(perspective: .server), wireVersion: .draft22)
        do {
            let token = Token()
            retained = token
            var binding = TransportBinding(factsKnown: .all, factsSupported: .all)
            binding.acceptURI = { [token] _, _ in
                withExtendedLifetime(token) { .accept }
            }
            try server?.bindTransport(binding)
        }
        #expect(retained != nil)
        server = nil
        #expect(retained == nil)
        weak var failed: Token?
        do {
            let client = try Session(configuration: .init(perspective: .client), wireVersion: .draft22)
            let token = Token()
            failed = token
            var binding = TransportBinding()
            binding.acceptURI = { [token] _, _ in withExtendedLifetime(token) { .accept } }
            #expect(throws: MoQError.self) { try client.bindTransport(binding) }
        }
        #expect(failed == nil)
    }

    @Test func ownedActionsAndEventsSurviveCleanup() throws {
        let buffer = try Buffer(Data([1, 2, 3]))
        var action = moq_action_t()
        action.kind = MOQ_ACTION_SEND_DATA
        action.detail_size = UInt32(MemoryLayout<moq_send_data_action_t>.size)
        moq_rcbuf_incref(buffer.raw)
        action.u.send_data.payload = buffer.raw
        var keptAction: ExtendedAction? = ExtendedAction.from(&action)
        #expect(action.u.send_data.payload == nil)
        #expect(moq_rcbuf_refcount(buffer.raw) == 2)
        guard case .sendData(_, _, let payload, _) = keptAction else {
            Issue.record("Missing payload"); return
        }
        #expect(payload?.copyBytes() == Data([1, 2, 3]))
        keptAction = nil
        withExtendedLifetime(payload) {}

        var event = moq_event_t()
        event.kind = MOQ_EVENT_FILL_OBJECT
        event.detail_size = UInt32(MemoryLayout<moq_fill_object_event_t>.size)
        event.u.fill_object.initiating_request_id = 73
        moq_rcbuf_incref(buffer.raw)
        event.u.fill_object.payload = buffer.raw
        let kept = ExtendedEvent.from(&event)
        moq_event_cleanup(&event)
        guard case .fillObject(let object) = kept else { Issue.record("Missing fill object"); return }
        #expect(object.initiatingRequestID == 73)
        #expect(object.payload?.copyBytes() == Data([1, 2, 3]))
    }

    @Test("Throwing callbacks release every polled action", arguments: [false, true])
    func throwingDrain(_ extended: Bool) throws {
        let (client, server) = try makeEstablishedPair()
        _ = try client.subscribe(SubscribeConfiguration(track: .init(namespace: ["owned"], name: "v")))
        try pumpControl(from: client, to: server)
        let request = try #require(server.pollEvents().compactMap { event -> SubscribeRequest? in
            if case .subscribeRequest(let value) = event { return value }; return nil
        }.first)
        try server.acceptSubscribe(request.subscription)
        try pumpControl(from: server, to: client)
        _ = client.pollEvents()
        let group = try server.openSubgroup(for: request.subscription, groupID: 0)
        let buffer = try Buffer(Data([1, 2, 3]))
        try server.writeObject(to: group, objectID: 0, payload: buffer)
        try server.writeObject(to: group, objectID: 1, payload: buffer)
        #expect(moq_rcbuf_refcount(buffer.raw) > 1)
        #expect(throws: MoQError.internal) {
            if extended {
                try server.drainActionsExtended { _ in throw MoQError.internal }
            } else {
                try server.drainActions { _ in throw MoQError.internal }
            }
        }
        #expect(moq_rcbuf_refcount(buffer.raw) == 1)
        #expect(server.pollActionsExtended().isEmpty)
    }
}
