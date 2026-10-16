import Testing
import CMoQCore
@testable import MoQ

@Suite("Admission and state sized observations")
struct AdmissionStateObservationTests {
    private struct MissingObservation: Error {}

    private func admission(_ event: inout moq_event_t) throws -> ExtendedSubscribeRequest {
        guard case .subscribeRequest(let value) = ExtendedEvent.from(&event) else { throw MissingObservation() }
        return value
    }

    @Test("Admission copies exact prefixes, unknown values and future records", arguments: [4, 71, 72, 88])
    func admissionPrefix(_ size: Int) throws {
        var input = moq_subscription_admission_options_t()
        input.struct_size = UInt32(size)
        input.flags = .max
        input.location.struct_size = 48
        input.location.kind = .max
        input.subgroup_delivery_timeout_ms = .max
        let storage = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { storage.deallocate() }
        storage.initializeMemory(as: UInt8.self, repeating: 0xa5, count: size)
        withUnsafeBytes(of: input) { storage.copyMemory(from: $0.baseAddress!, byteCount: min(size, 72)) }
        var event = moq_event_t()
        event.kind = MOQ_EVENT_SUBSCRIBE_REQUEST
        event.detail_size = UInt32(MemoryLayout<moq_subscribe_request_event_t>.size)
        event.u.subscribe_request.options = UnsafePointer(storage.assumingMemoryBound(to: moq_subscription_admission_options_t.self))
        let copied = try admission(&event)
        storage.initializeMemory(as: UInt8.self, repeating: 0, count: size)
        #expect((copied.options != nil) == (size >= 72))
        if size >= 72 {
            let options = try #require(copied.options)
            #expect(options.flags == .max && options.fillRequested)
            #expect(options.location == .other(.max))
            #expect(options.objectDeliveryTimeoutMs == 0 && options.subgroupDeliveryTimeoutMs == .max)
        }
    }

    @Test("Missing and torn admission pointers are never dereferenced")
    func admissionPointerGate() throws {
        let offset = MemoryLayout<moq_subscribe_request_event_t>.offset(of: \.options)!
        for size in offset..<(offset + MemoryLayout<UnsafeRawPointer>.size) {
            var event = moq_event_t()
            event.kind = MOQ_EVENT_SUBSCRIBE_REQUEST
            event.detail_size = UInt32(size)
            event.u.subscribe_request.options = UnsafePointer(bitPattern: 1)
            #expect(try admission(&event).options == nil)
        }
        var event = moq_event_t()
        event.kind = MOQ_EVENT_SUBSCRIBE_REQUEST
        event.detail_size = UInt32(MemoryLayout<moq_subscribe_request_event_t>.size)
        #expect(try admission(&event).options == nil)
        event.detail_size = UInt32(offset - 1)
        #expect(ExtendedEvent.from(&event) == .unknown(MOQ_EVENT_SUBSCRIBE_REQUEST))
    }

    @Test("Absent, explicit NONE and short nested locations stay distinct")
    func admissionPresence() throws {
        var input = moq_subscription_admission_options_t()
        input.struct_size = 72
        input.object_delivery_timeout_ms = .max
        input.subgroup_delivery_timeout_ms = .max
        var event = moq_event_t()
        event.kind = MOQ_EVENT_SUBSCRIBE_REQUEST
        event.detail_size = UInt32(MemoryLayout<moq_subscribe_request_event_t>.size)
        for row in 0..<3 {
            input.flags = row == 0 ? 0 : UInt32(MOQ_SUBSCRIPTION_ADMISSION_LOCATION)
            input.location.struct_size = row == 1 ? 47 : 48
            let copied = try withUnsafePointer(to: &input) {
                event.u.subscribe_request.options = $0
                return try admission(&event)
            }
            let options = try #require(copied.options)
            #expect(options.flags == (row == 0 ? 0 : 1))
            #expect(options.location == (row == 2 ? LocationFilter.none : nil))
            #expect(!options.fillRequested && options.objectDeliveryTimeoutMs == nil && options.subgroupDeliveryTimeoutMs == nil)
            var legacyEvent = event
            legacyEvent.u.subscribe_request.options = nil
            #expect(Event.from(&legacyEvent) == .subscribeRequest(copied.legacy))
        }
    }

    @Test("State snapshot copies only complete known prefixes", arguments: [4, 47, 48, 64])
    func statePrefix(_ size: Int) throws {
        var input = moq_location_filter_cfg_t()
        input.struct_size = UInt32(size)
        input.kind = .max
        let storage = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { storage.deallocate() }
        storage.initializeMemory(as: UInt8.self, repeating: 0xa5, count: size)
        withUnsafeBytes(of: input) { storage.copyMemory(from: $0.baseAddress!, byteCount: min(size, 48)) }
        var event = moq_event_t()
        event.kind = MOQ_EVENT_PUBLISH_STATE
        event.detail_size = UInt32(MemoryLayout<moq_publish_state_event_t>.size)
        event.u.publish_state.family = MOQ_REQUEST_FAMILY_SUBSCRIBE
        event.u.publish_state.handle.subscription = moq_subscription_t(_opaque: 17)
        event.u.publish_state.has_filter = true
        event.u.publish_state.has_forward = true
        event.u.publish_state.has_largest = true
        event.u.publish_state.largest_object = .max
        event.u.publish_state.location = UnsafePointer(storage.assumingMemoryBound(to: moq_location_filter_cfg_t.self))
        let copied = ExtendedEvent.from(&event)
        storage.initializeMemory(as: UInt8.self, repeating: 0, count: size)
        if size < 48 { #expect(copied == .unknown(MOQ_EVENT_PUBLISH_STATE)); return }
        guard case .publishState(let state) = copied else { throw MissingObservation() }
        #expect(state.location == .other(.max) && state.hasFilter)
        #expect(state.subscription == Subscription(moq_subscription_t(_opaque: 17)) && state.publication == nil)
        #expect(state.forward == false && state.largest?.group == 0 && state.largest?.object == .max)
        #expect(state.legacy.hasFilter && state.legacy.forward == false)
    }

    @Test("State old, short, absent, NONE and wrong-family controls")
    func statePointerGate() throws {
        let offset = MemoryLayout<moq_publish_state_event_t>.offset(of: \.location)!
        var event = moq_event_t()
        event.kind = MOQ_EVENT_PUBLISH_STATE
        event.u.publish_state.family = MOQ_REQUEST_FAMILY_PUBLISH
        for size in 0..<(offset + MemoryLayout<UnsafeRawPointer>.size) {
            event.detail_size = UInt32(size)
            event.u.publish_state.location = UnsafePointer(bitPattern: 1)
            event.u.publish_state.has_filter = false
            let absent = ExtendedEvent.from(&event)
            if size < offset { #expect(absent == .unknown(MOQ_EVENT_PUBLISH_STATE)) }
            else if case .publishState(let state) = absent { #expect(state.location == nil && !state.hasFilter) }
            else { Issue.record("Old complete state should preserve scalar fields") }
            event.u.publish_state.has_filter = true
            #expect(ExtendedEvent.from(&event) == .unknown(MOQ_EVENT_PUBLISH_STATE))
        }
        event.detail_size = UInt32(MemoryLayout<moq_publish_state_event_t>.size)
        event.u.publish_state.location = nil
        #expect(ExtendedEvent.from(&event) == .unknown(MOQ_EVENT_PUBLISH_STATE))
        var none = moq_location_filter_cfg_t()
        none.struct_size = 48
        let copied = withUnsafePointer(to: &none) {
            event.u.publish_state.location = $0
            return ExtendedEvent.from(&event)
        }
        guard case .publishState(let state) = copied else { throw MissingObservation() }
        #expect(state.location == LocationFilter.none && state.hasFilter)
        event.u.publish_state.has_filter = false
        event.u.publish_state.location = UnsafePointer(bitPattern: 1)
        event.u.publish_state.family = MOQ_REQUEST_FAMILY_FETCH
        #expect(ExtendedEvent.from(&event) == .unknown(MOQ_EVENT_PUBLISH_STATE))
        #expect(Event.from(&event) == .unknown(MOQ_EVENT_PUBLISH_STATE))
    }
}
