import Testing
import Foundation
@testable import MoQServiceCore

/*
 * Async object-waiter liveness under the track-event backlog bound. The
 * bounded event FIFO also gates the ASYNC object waiter: its level term
 * (`ReceiverAttachment.objectWaiterReady`) must not report a queued object as
 * ready while the Swift event FIFO is full. Otherwise the async `objects`
 * stream would wake, run a service-thread demand, hit the same full-FIFO gate,
 * return `.empty`, and re-park -- a hot retry. And because the readiness term
 * requires a free slot, a parked object waiter needs a wake when the
 * track-event consumer frees one, or it could sit until an unrelated engine
 * wake.
 *
 * Queued objects are therefore ready only when the FIFO has room, and
 * `nextTrackEvent()` nudges the engine when consuming an event drops the FIFO
 * out of its full state. This test drives the REAL public surfaces -- the
 * `objects` and `trackEvents` async streams over a live engine + service
 * thread -- with deterministic handshakes (poll/wake counters), no sleeps.
 */

@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
private struct AsyncBacklogRig {
    let endpointBackend: ScriptedEndpointBackend
    let receiverBackend: ScriptedReceiverBackend
    let endpoint: MoQEndpoint
    let receiver: MediaReceiver

    func failAndSettle(_ error: any Error, phase: String,
                       objectTask: Task<MediaObject?, any Error>?) async {
        let endpointBefore = endpointBackend.snapshot()
        let receiverBefore = receiverBackend.snapshot()
        let stagedBefore = receiver.attachment.stagedTrackEventCount
        Issue.record("""
            Backlog phase \(phase) failed: \(error); before cleanup:
            endpoint=\(endpointBefore)
            receiver=\(receiverBefore), staged=\(stagedBefore)
            """)
        // Cleanup can drain or wake the engine; never use it as progress evidence.
        objectTask?.cancel()
        await endpoint.close()
        if let objectTask {
            do {
                _ = try await objectTask.value
            } catch is CancellationError {
                // Expected when the failed phase left an object waiter pending.
            } catch {
                Issue.record("Unexpected object-task cleanup error: \(error)")
            }
        }
        let settled = endpointBackend.snapshot()
        #expect(settled.stopCount == 1)
        #expect(settled.destroyCount == 1)
        #expect(settled.violations.isEmpty)
        #expect(receiverBackend.snapshot().violations.isEmpty)
    }
}

@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
private func makeAsyncBacklogRig() throws -> AsyncBacklogRig {
    let endpointBackend = ScriptedEndpointBackend()
    let endpoint = MoQEndpoint(
        configuration: .init(url: URL(string: "moqt://relay.test:4443")!),
        engine: EndpointEngine(backend: endpointBackend))
    let receiverBackend = ScriptedReceiverBackend(
        latch: { endpointBackend.isLatched })
    let receiver = try MediaReceiver.attach(
        to: endpoint, configuration: .live(namespace: "live/cam1"),
        backend: receiverBackend)
    return AsyncBacklogRig(endpointBackend: endpointBackend,
                           receiverBackend: receiverBackend,
                           endpoint: endpoint, receiver: receiver)
}

private let kStagingCap = 64

@Suite("Track-event backlog async waiter")
struct TrackEventBacklogAsyncTests {

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("A legacy next skips a full fill backlog and wakes object demand without another backend event")
    func filteredFillFreesCapacity() async throws {
        let rig = try makeAsyncBacklogRig()
        rig.receiverBackend.scriptEvents([.added(7, name: "video")])
        var legacy = rig.receiver.trackEvents.makeAsyncIterator()
        rig.endpointBackend.wake()
        guard case .added(let track)? = try await legacy.next() else {
            Issue.record("missing track"); await rig.endpoint.close(); return
        }
        rig.receiverBackend.scriptEvents(
            (0..<64).map { .fill(7, requestID: UInt64($0)) } + [.catalogReady])
        let storage = MockObjectStorage(media: [0x42])
        rig.receiverBackend.scriptObjects([ReceiverPolledObject(
            handleID: 7, storage: storage, isKeyframe: false, endsGroup: false, isDatagram: false,
            presentationTime: .zero, decodeTime: .zero, compositionOffset: .zero, captureTime: nil)])
        let attachment = rig.receiver.attachment
        _ = try await rig.endpoint.engine.perform { attachment.drainTrackEvents() }
        #expect(attachment.stagedTrackEventCount == 64)
        #expect(rig.receiverBackend.snapshot().queuedEventsRemaining == 1)
        let polls = rig.receiverBackend.snapshot().pollCalls
        // One serialized demand plus two hook passes while full: still no polls.
        _ = try await rig.endpoint.engine.perform { attachment.demandObject() }
        _ = try await rig.endpoint.engine.perform { attachment.drainTrackEvents() }
        #expect(rig.receiverBackend.snapshot().pollCalls == polls)
        #expect(rig.receiverBackend.snapshot().objectPollCalls == 0)
        let wakes = rig.endpointBackend.snapshot().wakeCount
        let objectTask = Task {
            var iterator = rig.receiver.objects.makeAsyncIterator()
            return try await iterator.next()
        }
        #expect(await rig.endpointBackend.awaitCondition { $0.wakeCount > wakes })
        // A service-thread barrier ensures the earlier demand has hit the full gate.
        try await rig.endpoint.engine.perform { }
        #expect(rig.receiverBackend.snapshot().objectPollCalls == 0)
        // The only new input from here is consumption: skipping fill must nudge.
        if case .catalogReady? = try await legacy.next() {} else { Issue.record("legacy tail was not reached") }
        let object = try await objectTask.value
        #expect(object?.track === track && object?.mediaData == Data([0x42]))
        #expect(storage.cleanupCount == 0)
        #expect(rig.receiverBackend.snapshot().objectPollCalls == 1)
        #expect(rig.receiverBackend.snapshot().queuedEventsRemaining == 0)
        #expect(attachment.stagedTrackEventCount == 0)
        await rig.endpoint.close()
        #expect(rig.receiverBackend.snapshot().violations.isEmpty)
    }

    /// The public-surface liveness regression. With the FIFO full (64 staged)
    /// and handle 65's add retained behind it, an `objects` iterator must NOT
    /// poll while full; consuming events on the real `trackEvents` stream must
    /// drive it to completion using only the capacity-free wake -- no external
    /// backend wake after the FIFO is filled.
    ///
    /// Ledger asserted: the retained add is staged (backend surplus reaches 0)
    /// before the object is polled, the object is polled exactly once, the
    /// delivered object is not discarded, and the Swift FIFO never exceeds 64.
    ///
    /// Note on the consume count: the retained add is backend-owned behind the
    /// full ring, so the FIRST freed slot is spent staging it (refilling the
    /// ring to 64), and the object only becomes pollable once the backend
    /// surplus is exhausted -- two consumes for a surplus of one. Each consume's
    /// nudge is load-bearing: without the capacity-free wake the iterator never
    /// completes (see the report).
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Objects stream completes via the capacity-free wake, never polling while full",
          arguments: [false, true])
    func objectStreamProgressesOnFreedCapacity(extraEmptyPoll: Bool) async throws {
        let rig = try makeAsyncBacklogRig()
        let attachment = rig.receiver.attachment
        var objectTask: Task<MediaObject?, any Error>? = nil
        var phase = "empty-poll discriminator"
        do {
            let before = rig.receiverBackend.snapshot()
            let eventsBefore = before.eventPollsReturned
            if extraEmptyPoll {
                // Force the old poll-count handshake ahead of actual event returns.
                _ = try await rig.endpoint.engine.perform { attachment.drainTrackEvents() }
                let empty = rig.receiverBackend.snapshot()
                try #require(empty.pollCalls > before.pollCalls)
                try #require(empty.eventPollsReturned == eventsBefore)
            }

            // 64 adds (t1..t64) + handle 65's add retained; its object queued.
            var events: [ReceiverPolledEvent] =
                (1...kStagingCap).map { .added(UInt64($0), name: "t\($0)") }
            events.append(.added(UInt64(kStagingCap + 1), name: "t\(kStagingCap + 1)"))
            rig.receiverBackend.scriptEvents(events)
            let storage = MockObjectStorage(media: [0xAB, 0xCD])
            rig.receiverBackend.scriptObjects([ReceiverPolledObject(
                handleID: UInt64(kStagingCap + 1), storage: storage,
                isKeyframe: true, endsGroup: false, isDatagram: false,
                presentationTime: .milliseconds(40), decodeTime: .milliseconds(40),
                compositionOffset: .zero, captureTime: nil)])

            // Fill the FIFO, retaining handle 65's add behind its 64 staged adds.
            phase = "initial fill"
            rig.endpointBackend.wake()
            try #require(await rig.receiverBackend.awaitCondition {
                $0.eventPollsReturned >= eventsBefore + kStagingCap })
            var snap = rig.receiverBackend.snapshot()
            try #require(snap.eventPollsReturned == eventsBefore + kStagingCap)
            try #require(snap.queuedEventsRemaining == 1)
            try #require(snap.objectPollCalls == 0)

            // Settle filling before counting the perform and waitUntil wakes.
            phase = "initial quiescent park"
            try #require(await rig.endpointBackend.awaitParked(entries: 0))
            let beforeObject = rig.endpointBackend.snapshot()
            let pendingObject = Task { () -> MediaObject? in
                var it = rig.receiver.objects.makeAsyncIterator()
                return try await it.next()
            }
            objectTask = pendingObject
            phase = "object waiter registration"
            try #require(await rig.endpointBackend.awaitCondition {
                $0.wakeCount >= beforeObject.wakeCount + 2
                    && $0.waitEntries >= beforeObject.waitEntries + 2
                    && $0.parked
            })
            try #require(rig.receiverBackend.snapshot().objectPollCalls == 0)
            try #require(attachment.stagedTrackEventCount == kStagingCap)
            if extraEmptyPoll {
                // Old "65 polls" is true while only 64 actual adds were returned.
                snap = rig.receiverBackend.snapshot()
                try #require(snap.pollCalls >= before.pollCalls + kStagingCap + 1)
                try #require(snap.eventPollsReturned == eventsBefore + kStagingCap)
                try #require(snap.queuedEventsRemaining == 1)
            }

            var eventIterator = rig.receiver.trackEvents.makeAsyncIterator()

            // Consume #1 must nudge the retained add into the FIFO, refilling it.
            phase = "first capacity consume"
            let waitBeforeRefill = rig.endpointBackend.snapshot().waitEntries
            let first = try await eventIterator.next()
            try #require(first != nil)
            phase = "retained-add return after first consume"
            try #require(await rig.receiverBackend.awaitCondition {
                $0.eventPollsReturned >= eventsBefore + kStagingCap + 1 })
            // Removal precedes Swift integration; consume #2 needs its own wake.
            phase = "refilled quiescent park"
            try #require(await rig.endpointBackend.awaitParked(entries: waitBeforeRefill + 1))
            try #require(attachment.stagedTrackEventCount == kStagingCap)
            snap = rig.receiverBackend.snapshot()
            try #require(snap.eventPollsReturned == eventsBefore + kStagingCap + 1)
            try #require(snap.queuedEventsRemaining == 0)
            try #require(snap.objectPollCalls == 0)     // add before object

            // Consume #2 must nudge object demand; no unrelated wake is supplied.
            phase = "second capacity consume"
            let second = try await eventIterator.next()
            try #require(second != nil)
            phase = "object poll after second consume"
            try #require(await rig.receiverBackend.awaitCondition {
                $0.objectPollCalls >= 1 && !$0.objectPollBlocked
            })

            phase = "object delivery"
            let object = try await pendingObject.value
            #expect(object != nil)
            #expect(object?.mediaData == Data([0xAB, 0xCD]))
            #expect(storage.cleanupCount == 0)             // delivered, still owned

            snap = rig.receiverBackend.snapshot()
            #expect(snap.objectPollCalls == 1)             // polled exactly once
            #expect(snap.queuedObjectsRemaining == 0)      // not discarded, delivered

            await rig.endpoint.close()
            #expect(rig.receiverBackend.snapshot().violations.isEmpty)
        } catch {
            await rig.failAndSettle(error, phase: phase, objectTask: objectTask)
        }
    }

    /// Terminal liveness: a terminal behind a full event FIFO must not
    /// complete or hot-poll the `objects` stream while full; it completes
    /// (clean `nil`) only after the real `trackEvents` consumer frees enough
    /// capacity for `drainTrackEvents()` to reach and poll the terminal.
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Objects stream surfaces a terminal only after capacity frees, never hot-polling while full")
    func terminalSurfacesOnlyAfterFreedCapacity() async throws {
        let rig = try makeAsyncBacklogRig()
        var objectTask: Task<MediaObject?, any Error>? = nil
        var phase = "initial terminal fill"
        do {
            let eventsBefore = rig.receiverBackend.snapshot().eventPollsReturned

            // Exactly 64 events, then a clean terminal behind the full ring.
            rig.receiverBackend.scriptEvents(
                (1...kStagingCap).map { .added(UInt64($0), name: "t\($0)") })
            rig.receiverBackend.scriptTerminal()

            // Fill without polling the terminal or any object.
            rig.endpointBackend.wake()
            try #require(await rig.receiverBackend.awaitCondition {
                $0.eventPollsReturned >= eventsBefore + kStagingCap })
            var snap = rig.receiverBackend.snapshot()
            try #require(snap.eventPollsReturned == eventsBefore + kStagingCap)
            try #require(snap.terminalPollsReturned == 0)
            try #require(snap.objectPollCalls == 0)

            phase = "initial terminal quiescent park"
            try #require(await rig.endpointBackend.awaitParked(entries: 0))
            let beforeObject = rig.endpointBackend.snapshot()
            let pendingObject = Task { () -> MediaObject? in
                var it = rig.receiver.objects.makeAsyncIterator()
                return try await it.next()
            }
            objectTask = pendingObject
            phase = "terminal waiter registration"
            try #require(await rig.endpointBackend.awaitCondition {
                $0.wakeCount >= beforeObject.wakeCount + 2
                    && $0.waitEntries >= beforeObject.waitEntries + 2
                    && $0.parked
            })
            snap = rig.receiverBackend.snapshot()
            try #require(rig.receiver.attachment.stagedTrackEventCount == kStagingCap)
            try #require(snap.objectPollCalls == 0)        // no hot-poll while full
            try #require(snap.terminalPollsReturned == 0)

            // The real consumer frees capacity; only its nudge can reach terminal.
            phase = "terminal capacity consume"
            var eventIterator = rig.receiver.trackEvents.makeAsyncIterator()
            let first = try await eventIterator.next()
            try #require(first != nil)
            phase = "terminal poll after consume"
            try #require(await rig.receiverBackend.awaitCondition {
                $0.terminalPollsReturned >= 1
            })

            phase = "terminal delivery"
            let terminal = try await pendingObject.value
            #expect(terminal == nil)                       // clean terminal disposition

            snap = rig.receiverBackend.snapshot()
            #expect(snap.terminalPollsReturned >= 1)
            await rig.endpoint.close()
            #expect(rig.receiverBackend.snapshot().violations.isEmpty)
        } catch {
            await rig.failAndSettle(error, phase: phase, objectTask: objectTask)
        }
    }
}
