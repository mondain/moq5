import Testing
import CMoQCore
@testable import MoQ

@Suite("Publication UPDATE observations")
struct PublicationUpdateObservationTests {
    private func extract(_ event: inout moq_event_t) throws -> UpdateRequestInfo {
        guard case .publishUpdated(let value) = ExtendedEvent.from(&event) else {
            throw MissingUpdate()
        }
        return value.update
    }
    private struct MissingUpdate: Error {}

    @Test("Copy the old80-byte record without borrowing or reading its future tail", arguments: 0..<6)
    func copiedPrefix(_ row: Int) throws {
        var options = moq_subscription_update_options_t()
        options.struct_size = 80
        options.initiating_request_id = UInt64(40 + row)
        options.object_delivery_timeout_ms = .max
        options.subgroup_delivery_timeout_ms = .max
        let location: LocationFilter? = row == 0 ? nil : row == 1 ? LocationFilter.none :
            row == 2 ? .relativeGroup(offset: .max) :
            .objectEnd(startGroup: 1, startObject: 2, endGroup: 3, endObject: .max)
        if let location {
            options.flags |= 1
            location.fill(&options.location)
        }
        if row == 3 || row == 5 {
            options.flags |= 2
            options.object_delivery_timeout_ms = 0
        }
        if row >= 4 { options.flags |= 4 }
        let storage = UnsafeMutableRawPointer.allocate(byteCount: 80, alignment: 8)
        defer { storage.deallocate() }
        withUnsafeBytes(of: options) { storage.copyMemory(from: $0.baseAddress!, byteCount: 80) }
        var event = moq_event_t()
        event.kind = MOQ_EVENT_PUBLISH_UPDATED
        event.detail_size = UInt32(MemoryLayout<moq_publish_updated_event_t>.size)
        event.u.publish_updated.options = UnsafePointer(storage.assumingMemoryBound(to: moq_subscription_update_options_t.self))
        event.u.publish_updated.answer_owed = true
        event.u.publish_updated.fill_requested = true
        event.u.publish_updated.update_count = 1
        event.u.publish_updated.update_request_id = options.initiating_request_id
        let value = try extract(&event)
        storage.initializeMemory(as: UInt8.self, repeating: 0, count: 80)
        #expect(value.options?.initiatingRequestID == UInt64(40 + row))
        #expect(value.options?.location == location)
        #expect(value.options?.objectDeliveryTimeoutMs == (row == 3 || row == 5 ? 0 : nil))
        #expect(value.options?.subgroupDeliveryTimeoutMs == (row >= 4 ? UInt64.max : nil))
        #expect(value.answerOwed && value.fillRequested && value.updateCount == 1)
        #expect(value.updateRequestID == UInt64(40 + row))
        #expect(value.filter == nil && value.deliveryTimeoutUs == nil && value.endObject == 0)
    }

    @Test("A torn options pointer is absent and never dereferenced")
    func oldDetail() throws {
        let offset = MemoryLayout<moq_publish_updated_event_t>.offset(of: \.options)!
        for size in offset..<(offset + MemoryLayout<UnsafeRawPointer>.size) {
            var event = moq_event_t()
            event.kind = MOQ_EVENT_PUBLISH_UPDATED
            event.detail_size = UInt32(size)
            event.u.publish_updated.options = UnsafePointer(bitPattern: 1)
            event.u.publish_updated.answer_owed = true
            event.u.publish_updated.fill_requested = true
            event.u.publish_updated.update_count = .max
            event.u.publish_updated.update_request_id = .max
            event.u.publish_updated.has_delivery_timeout = true
            event.u.publish_updated.delivery_timeout_us = 1000
            let value = try extract(&event)
            #expect(value.options == nil && value.deliveryTimeoutUs == 1000)
            #expect(!value.answerOwed && !value.fillRequested)
            #expect(value.updateCount == 0 && value.updateRequestID == 0)
        }
    }

    @Test("Only a complete raw-options prefix is readable",
          arguments: [0, 4, 79, 80, MemoryLayout<moq_subscription_update_options_t>.size])
    func optionsSize(_ size: Int) throws {
        var options = moq_subscription_update_options_t()
        options.struct_size = UInt32(size)
        options.object_delivery_timeout_ms = .max
        options.subgroup_delivery_timeout_ms = .max
        var event = moq_event_t()
        event.kind = MOQ_EVENT_PUBLISH_UPDATED
        event.detail_size = UInt32(MemoryLayout<moq_publish_updated_event_t>.size)
        let value = try withUnsafePointer(to: &options) {
            event.u.publish_updated.options = $0
            return try extract(&event)
        }
        #expect((value.options != nil) == (size >= 80))
        #expect(value.options?.location == nil)
        #expect(value.options?.objectDeliveryTimeoutMs == nil)
        #expect(value.options?.subgroupDeliveryTimeoutMs == nil)
    }

    @Test("A selected but short nested location is absent")
    func shortLocation() {
        var options = moq_subscription_update_options_t()
        options.struct_size = 80
        options.flags = 1
        options.location.struct_size = UInt32(MOQ_LOCATION_FILTER_CFG_V0_SIZE - 1)
        options.location.kind = MOQ_LOCATION_FILTER_OBJECT_END
        options.location.end_object = .max
        let copied = withUnsafePointer(to: &options) { SubscriptionUpdateOptions($0) }
        #expect(copied != nil && copied?.location == nil)
    }
}
