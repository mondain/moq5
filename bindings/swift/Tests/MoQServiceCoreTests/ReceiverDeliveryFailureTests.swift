import Testing
import Foundation
@testable import MoQServiceCore

@Suite("Receiver delivery failure model")
struct ReceiverDeliveryFailureTests {
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Discovery and delivery failures share the existing consuming FIFO")
    func deliveryIdentityAndOrdering() async throws {
        let endpointBackend = ScriptedEndpointBackend()
        let backend = ScriptedReceiverBackend(latch: { endpointBackend.isLatched })
        let endpoint = MoQEndpoint(configuration: .init(url: URL(string: "moqt://relay.test:4443")!),
                                   engine: EndpointEngine(backend: endpointBackend))
        let receiver = try MediaReceiver.attach(to: endpoint,
            configuration: .live(namespace: "live/video"), backend: backend)
        backend.scriptEvents([
            .added(7, name: "video"),
            .init(kind: .deliveryLocalFailure(localResult: -9, origin: 2, initiatingRequestID: 0),
                  handleID: 7, trackDescription: nil),
            .init(kind: .deliveryLocalFailure(localResult: nil, origin: nil, initiatingRequestID: nil),
                  handleID: 7, trackDescription: nil),
            .catalogReady,
        ])
        let attachment = receiver.attachment
        _ = try await endpoint.engine.perform { attachment.drainTrackEvents() }
        #expect(attachment.stagedTrackEventCount == 4)
        var events = receiver.trackEventsExtended.makeAsyncIterator()
        guard case .legacy(.added(let track))? = try await events.next(),
              case .deliveryLocalFailure(let first, let result, let origin, let id)? = try await events.next(),
              case .deliveryLocalFailure(let second, let absent, let absentOrigin, let absentID)? = try await events.next(),
              case .legacy(.catalogReady)? = try await events.next() else {
            Issue.record("event order or kind changed"); await endpoint.close(); return
        }
        #expect(track === first && first === second)
        #expect(result == -9 && origin == 2 && id == 0)
        #expect(absent == nil && absentOrigin == nil && absentID == nil)
        #expect(!backend.isFatal && !backend.isTerminal)
        #expect(backend.snapshot().subscribeCalls.isEmpty && backend.snapshot().cancelFillCalls.isEmpty)
        await receiver.close()
        await endpoint.close()
        #expect(try await events.next() == nil)
        #expect(backend.snapshot().violations.isEmpty)
    }
}
