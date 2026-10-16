import Testing
import CMoQService
@testable import MoQService
import MoQServiceCore

@Suite("Receiver delivery failure bridge")
struct ReceiverDeliveryFailureBridgeTests {
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Complete fields, distinct fill IDs, zero and unknown origins")
    func sizedDeliveryFailure() throws {
        let resultEnd = try #require(MemoryLayout<moq_media_track_event_t>.offset(of: \.local_result)) + 4
        let originEnd = try #require(MemoryLayout<moq_media_track_event_t>.offset(of: \.delivery_origin)) + 4
        let idEnd = try #require(MemoryLayout<moq_media_track_event_t>.offset(of: \.delivery_initiating_request_id)) + 8
        for origin: UInt32 in [MOQ_MEDIA_DELIVERY_LIVE, MOQ_MEDIA_DELIVERY_FILL, .max] {
            for id: UInt64 in [0, .max] {
                for stamp in 0...(MemoryLayout<moq_media_track_event_t>.size + 1) {
                    var event = moq_media_track_event_t()
                    event.struct_size = UInt32(stamp)
                    event.kind = MOQ_MEDIA_TRACK_DELIVERY_LOCAL_FAILURE
                    event.track = OpaquePointer(bitPattern: 0x123400)
                    event.local_result = MOQ_ERR_BUFFER
                    event.initiating_request_id = 123
                    event.fill_reason = MOQ_FILL_COMPLETE
                    event.delivery_origin = origin
                    event.delivery_initiating_request_id = id
                    let copied = try #require(copyReceiverTrackEvent(event))
                    guard case .deliveryLocalFailure(let result, let copiedOrigin, let copiedID) = copied.kind else {
                        Issue.record("delivery failure kind lost"); continue
                    }
                    #expect(copied.handleID == 0x123400)
                    #expect(result == (stamp >= resultEnd ? MOQ_ERR_BUFFER : nil))
                    #expect(copiedOrigin == (stamp >= originEnd ? origin : nil))
                    #expect(copiedID == (stamp >= idEnd && origin == MOQ_MEDIA_DELIVERY_FILL ? id : nil))
                }
            }
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Existing terminal correlation does not use the new delivery ID")
    func terminalIsolation() throws {
        var event = moq_media_track_event_t()
        event.struct_size = UInt32(MemoryLayout<moq_media_track_event_t>.size)
        event.kind = MOQ_MEDIA_TRACK_FILL_TERMINAL
        event.track = OpaquePointer(bitPattern: 0x123400)
        event.initiating_request_id = 0
        event.delivery_initiating_request_id = .max
        event.delivery_origin = MOQ_MEDIA_DELIVERY_FILL
        guard case .fillTerminal(let terminal)? = copyReceiverTrackEvent(event)?.kind else {
            Issue.record("terminal kind changed"); return
        }
        #expect(terminal.initiatingRequestID == 0)
    }
}
