import Testing
import CMoQService
@testable import MoQService
import MoQServiceCore

@Suite("Receiver local failure bridge")
struct ReceiverLocalFailureBridgeTests {
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The production copier retains full-stamped native local failure")
    func copiesLocalFailure() {
        for result in [MOQ_ERR_NOMEM, MOQ_ERR_BUFFER] {
            var event = moq_media_track_event_t()
            event.struct_size = UInt32(MemoryLayout<moq_media_track_event_t>.size)
            event.kind = MOQ_MEDIA_TRACK_LOCAL_FAILURE
            event.track = OpaquePointer(bitPattern: 0x2000)
            event.local_result = result
            #expect(copyReceiverTrackEvent(event) != nil)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Only a complete local-result field is observed, preserving every signed bit")
    func localResultPrefixes() throws {
        let offset = try #require(MemoryLayout<moq_media_track_event_t>.offset(of: \.local_result))
        let end = offset + MemoryLayout<moq_result_t>.size
        let full = MemoryLayout<moq_media_track_event_t>.size
        let stamps = [0, 4, offset - 1, offset, offset + 1, end - 1, end, full, full + 64, Int(UInt32.max)]
        let results: [Int32] = [MOQ_ERR_NOMEM, MOQ_ERR_BUFFER, 0, .min, .max, -12345, 12345]
        for stamp in stamps {
            for result in results {
                var event = moq_media_track_event_t()
                event.struct_size = UInt32(stamp)
                event.kind = MOQ_MEDIA_TRACK_LOCAL_FAILURE
                event.track = OpaquePointer(bitPattern: 0x123400)
                event.local_result = result
                // Unrelated tail values must not become fill or parse-drop facts.
                event.fill_error_code = .max
                event.parse_drops_total = .max
                let copy = try #require(copyReceiverTrackEvent(event))
                event.local_result = 99
                #expect(copy.handleID == 0x123400 && copy.trackDescription == nil)
                #expect(copy.parseDropClass == nil && copy.parseDropTotal == 0 && copy.parseDropDelta == 0)
                guard case .localFailure(let copied) = copy.kind else {
                    Issue.record("local failure kind lost"); continue
                }
                #expect(copied == (stamp >= end ? result : nil))
            }
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Local-result bytes do not reclassify legacy, fill, UPDATE_OK or unknown kinds")
    func kindIsolation() throws {
        var event = moq_media_track_event_t()
        event.struct_size = UInt32(MemoryLayout<moq_media_track_event_t>.size)
        event.track = OpaquePointer(bitPattern: 0x2000)
        event.local_result = MOQ_ERR_BUFFER
        event.kind = MOQ_MEDIA_TRACK_ENDED
        guard case .ended? = copyReceiverTrackEvent(event)?.kind else {
            Issue.record("legacy end changed"); return
        }
        event.kind = MOQ_MEDIA_TRACK_FILL_TERMINAL
        event.initiating_request_id = .max
        event.fill_reason = MOQ_FILL_COMPLETE
        guard case .fillTerminal(let terminal)? = copyReceiverTrackEvent(event)?.kind else {
            Issue.record("fill reclassified"); return
        }
        #expect(terminal.initiatingRequestID == .max && terminal.reason == .complete)
        event.kind = MOQ_MEDIA_TRACK_UPDATE_OK
        #expect(copyReceiverTrackEvent(event) == nil)
        event.kind = moq_media_track_event_kind_t(rawValue: .max)
        #expect(copyReceiverTrackEvent(event) == nil)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The production query forwards native pointers and maps every legacy/failed/future state")
    func stateQueryForwarding() throws {
        let rows: [(moq_media_track_state_t, ExtendedTrackSubscriptionState)] = [
            (MOQ_MEDIA_TRACK_STATE_DISCOVERED, .legacy(.discovered)),
            (MOQ_MEDIA_TRACK_STATE_PENDING, .legacy(.pending)),
            (MOQ_MEDIA_TRACK_STATE_ACTIVE, .legacy(.active)),
            (MOQ_MEDIA_TRACK_STATE_PAUSED_APP, .legacy(.pausedByApp)),
            (MOQ_MEDIA_TRACK_STATE_PAUSED_FLOW, .legacy(.pausedByFlowControl)),
            (MOQ_MEDIA_TRACK_STATE_ENDED, .legacy(.ended)),
            (MOQ_MEDIA_TRACK_STATE_FAILED, .failed),
            (.init(rawValue: 7), .unknown(7)),
            (.init(rawValue: 0x8000_0000), .unknown(0x8000_0000)),
            (.init(rawValue: .max), .unknown(.max)),
        ]
        let receiver = OpaquePointer(bitPattern: 0x1000)!
        for (raw, expected) in rows {
            var calls = 0
            let result = queryReceiverTrackState(receiver: receiver, handleID: 0x2000) { r, track, out in
                calls += 1
                #expect(r == receiver && track == OpaquePointer(bitPattern: 0x2000))
                #expect(out != nil)
                out?.pointee = raw
                return moq_result_t(MOQ_OK)
            }
            #expect(calls == 1)
            #expect(try result.get() == expected)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Query refusals are preserved and never read a poisoned native state output")
    func stateQueryRefusals() {
        let rows: [(Int32, MoQServiceError)] = [
            (MOQ_ERR_INVAL, .invalidArgument("track state: invalid track")),
            (MOQ_ERR_WRONG_STATE, .wrongState), (MOQ_ERR_CLOSED, .closed),
            (MOQ_ERR_UNSUPPORTED, .unsupported),
            (MOQ_ERR_NOMEM, .internalError(MOQ_ERR_NOMEM)),
            (MOQ_ERR_BUFFER, .internalError(MOQ_ERR_BUFFER)),
            (-12345, .internalError(-12345)), (.min, .internalError(.min)),
            (.max, .internalError(.max)), (1, .internalError(1)),
        ]
        for (raw, expected) in rows {
            var calls = 0
            let result = queryReceiverTrackState(receiver: OpaquePointer(bitPattern: 0x1000),
                                                 handleID: 0x2000) { _, _, out in
                calls += 1
                out?.pointee = MOQ_MEDIA_TRACK_STATE_FAILED
                return raw
            }
            #expect(calls == 1)
            #expect(throws: expected) { try result.get() }
        }
        let invalid = queryReceiverTrackState(receiver: OpaquePointer(bitPattern: 0x1000), handleID: 0) { _, _, _ in
            Issue.record("null track reached native"); return moq_result_t(MOQ_OK)
        }
        #expect(throws: MoQServiceError.invalidArgument("track state: invalid track")) { try invalid.get() }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Default query calls the released public C getter's NULL-receiver guard")
    func realNativeQueryGuard() {
        // This exercises the real getter boundary, not native failure production.
        let result = queryReceiverTrackState(receiver: nil, handleID: 0x2000)
        #expect(throws: MoQServiceError.invalidArgument("track state: invalid track")) { try result.get() }
    }
}
