import Testing
import Foundation
@testable import MoQServiceCore

@Suite("Receiver owned observations")
struct ReceiverObservationModelTests {
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Old initializers leave new observations unobserved")
    func oldInitializers() {
        let storage = MockObjectStorage(media: [7])
        let polled = ReceiverPolledObject(
            handleID: 1, storage: storage, isKeyframe: false,
            endsGroup: false, isDatagram: false, presentationTime: .zero,
            decodeTime: .zero, compositionOffset: .zero, captureTime: nil)
        #expect(polled.wireIdentity == nil)
        #expect(polled.metadata == nil)
        #expect(polled.objectProperties == nil)
        #expect(polled.trackProperties == nil)
        #expect(polled.delivery == nil)
        do {
            let object = MediaObject(
                storage: storage,
                track: MediaTrack(description: TrackDescription(
                    name: "video", mediaType: .video, packaging: .raw)),
                isKeyframe: false, endsGroup: false, isDatagram: false)
            #expect(object.wireIdentity == nil)
            #expect(object.metadata == nil)
            #expect(object.objectProperties == nil)
            #expect(object.trackProperties == nil)
            #expect(object.delivery == nil)
        }
        #expect(storage.cleanupCount == 1)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Demand propagates owned values, surviving detach and a task hop")
    func demandPropagation() async throws {
        let endpointBackend = ScriptedEndpointBackend()
        let endpoint = MoQEndpoint(
            configuration: .init(url: URL(string: "moqt://relay.test:4443")!),
            engine: EndpointEngine(backend: endpointBackend))
        let backend = ScriptedReceiverBackend(latch: { endpointBackend.isLatched })
        let receiver = try MediaReceiver.attach(
            to: endpoint, configuration: .live(namespace: "live/cam1"), backend: backend)
        let storage = MockObjectStorage(media: [1, 2, 3])
        let identity = MediaObjectIdentity(
            groupID: .max, subgroupID: 0, objectID: .max, publisherPriority: .max)
        let loc = MediaLOCMetadata(
            rawPresence: .max, timestamp: .max, timescale: 0,
            marking: Data([0, 0x80, 0xff, 1]), audioLevel: .max,
            videoConfig: Data(), audioConfig: Data([9]))
        let metadata = MediaObjectMetadata(
            timeDomain: .unknown(.max), timeMicroseconds: .max,
            isIndependent: false, loc: loc)
        backend.scriptEvents([.added(7, name: "video")])
        backend.scriptObjects([ReceiverPolledObject(
            handleID: 7, storage: storage, isKeyframe: false,
            endsGroup: true, isDatagram: false,
            presentationTime: .microseconds(17), decodeTime: .microseconds(11),
            compositionOffset: .microseconds(6), captureTime: nil,
            wireIdentity: identity, metadata: metadata,
            objectProperties: Data([3, 4]), trackProperties: Data([5, 6]),
            delivery: .fill(initiatingRequestID: 0))])
        endpointBackend.wake()
        do {
            var iterator = receiver.objects.makeAsyncIterator()
            let object = try #require(try await iterator.next())
            await endpoint.close()
            #expect(backend.snapshot().detachCalls == 1)
            #expect(backend.snapshot().objectPollCalls == 1)
            #expect(backend.snapshot().violations.isEmpty)
            let received = await Task.detached { object }.value
            #expect(received.wireIdentity == identity)
            #expect(received.metadata == metadata)
            #expect(received.objectProperties == Data([3, 4]))
            #expect(received.trackProperties == Data([5, 6]))
            #expect(received.delivery == .fill(initiatingRequestID: 0))
            #expect(received.delivery?.initiatingRequestID == 0)
            #expect(received.presentationTime == .microseconds(17))
            #expect(received.decodeTime == .microseconds(11))
            #expect(received.compositionOffset == .microseconds(6))
            #expect(received.captureTime == nil)
            #expect(received.mediaData == Data([1, 2, 3]))
            #expect(storage.cleanupCount == 0)
        }
        #expect(storage.cleanupCount == 1)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Delivery distinguishes LIVE, both FILL initiators, and unknown raw values")
    func deliveryValues() {
        let live = MediaObjectDelivery.live
        let zero = MediaObjectDelivery.fill(initiatingRequestID: 0)
        let maximum = MediaObjectDelivery.fill(initiatingRequestID: .max)
        let unknown = MediaObjectDelivery.unknown(rawOrigin: .max, rawInitiatingRequestID: 17)
        #expect(live.initiatingRequestID == nil)
        #expect(zero.initiatingRequestID == 0)
        #expect(maximum.initiatingRequestID == .max)
        #expect(unknown.initiatingRequestID == nil)
        #expect(Set([live, zero, maximum, unknown]).count == 4)
    }
}
