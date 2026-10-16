import Testing
import Foundation
import MoQ
import MoQTransportModel

/// Outcomes that name a request do so with the typed handle an ordinary
/// client (plain `import MoQ`) saved when it made the request: a request
/// GOAWAY names the subscription to re-issue, and the refusal of this side's
/// own PUBLISH update names the publication that must end with UPDATE_FAILED.
@Suite("Request identities through the public API")
struct RequestIdentityTests {

    private static let peerBit: [UInt64] = [1 << 40, 1 << 41]

    private func toPeer(_ ref: StreamRef, _ dir: Int) -> StreamRef {
        let back = Self.peerBit[dir ^ 1]
        if ref.rawValue & back != 0 { return StreamRef(rawValue: ref.rawValue & ~back) }
        return StreamRef(rawValue: ref.rawValue | Self.peerBit[dir])
    }

    /// Deliver `from`'s actions to `to`; the request streams `from` opened
    /// are appended to `opened`.
    private func pump(_ from: Session, _ to: Session, _ dir: Int,
                      _ opened: inout [StreamRef]) throws {
        var bidi: [StreamRef] = []
        try from.drainActionsExtended { action in
            switch action {
            case .sendControl(let d), .openUniControl(_, let d), .sendUniControl(_, let d, _):
                try d.withUnsafeBytes { try to.receiveControlBytes($0, now: 0) }
            case .sendData(let ref, let header, let payload, let fin):
                var bytes = [UInt8](header)
                payload?.withUnsafeBytes { bytes.append(contentsOf: $0) }
                try bytes.withUnsafeBytes {
                    try to.receiveDataBytes(on: toPeer(ref, dir), $0, fin: fin, now: 0)
                }
            case .openBidiStream(let ref, let d, let fin):
                bidi.append(ref)
                try d.withUnsafeBytes {
                    try to.receiveBidiStreamBytes(on: toPeer(ref, dir), $0, fin: fin, now: 0)
                }
            case .sendBidiStream(let ref, let d, let fin):
                try d.withUnsafeBytes {
                    try to.receiveBidiStreamBytes(on: toPeer(ref, dir), $0, fin: fin, now: 0)
                }
            case .resetBidiStream(let ref, let code):
                try to.receiveBidiStreamReset(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .stopBidiStream(let ref, let code):
                try to.receiveBidiStreamStop(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .abortBidiStream(let ref, let code):
                try to.receiveBidiStreamReset(on: toPeer(ref, dir), errorCode: code, now: 0)
                try to.receiveBidiStreamStop(on: toPeer(ref, dir), errorCode: code, now: 0)
            default:
                break
            }
        }
        opened.append(contentsOf: bidi)
    }

    private func settle(_ c: Session, _ s: Session, _ opened: inout [StreamRef]) throws {
        var serverOpened: [StreamRef] = []
        for _ in 0..<6 { try pump(c, s, 0, &opened); try pump(s, c, 1, &serverOpened) }
    }

    private func pair() throws -> (Session, Session) {
        var cc = Session.Configuration(perspective: .client)
        cc.sendRequestCapacity = true
        cc.initialRequestCapacity = 16
        cc.wireVersion = .draft22
        var sc = Session.Configuration(perspective: .server)
        sc.sendRequestCapacity = true
        sc.initialRequestCapacity = 16
        sc.wireVersion = .draft22
        let c = try Session(configuration: cc)
        let s = try Session(configuration: sc)
        var cb = TransportBinding(factsKnown: .all, factsSupported: .all)
        cb.authority = "sim.invalid"
        var sb = TransportBinding(factsKnown: .all, factsSupported: .all)
        sb.acceptURI = { _, _ in .accept }
        try c.bindTransport(cb)
        try s.bindTransport(sb)
        try c.start()
        try s.start()
        var opened: [StreamRef] = []
        try settle(c, s, &opened)
        c.drainEventsWithDetail { _, _ in }
        s.drainEventsWithDetail { _, _ in }
        return (c, s)
    }

    private let ns = Namespace([NamespacePart(Data("ids".utf8))])

    @Test("A request GOAWAY names the saved subscription")
    func requestGoawayIdentity() throws {
        let (c, s) = try pair()
        var opened: [StreamRef] = []
        let first = try c.subscribe(SubscribeConfiguration(
            track: TrackName(namespace: ns, name: Data("a".utf8)), filter: .latestObject))
        try settle(c, s, &opened)
        let second = try c.subscribe(SubscribeConfiguration(
            track: TrackName(namespace: ns, name: Data("b".utf8)), filter: .latestObject))
        try settle(c, s, &opened)
        #expect(opened.count == 2)
        for ev in s.pollEventsExtended() {
            if case .subscribeRequest(let r) = ev { try s.acceptSubscribe(r.subscription) }
        }
        try settle(c, s, &opened)
        c.drainEventsWithDetail { _, _ in }
        // The publisher migrates the second subscription: GOAWAY on its
        // request stream (type 0x10, empty URI, Timeout 0).
        let goaway: [UInt8] = [0x10, 0x00, 0x02, 0x00, 0x00]
        try goaway.withUnsafeBytes {
            try c.receiveBidiStreamBytes(on: opened[1], $0, fin: false, now: 0)
        }
        var named: [RedirectedRequest] = []
        var statuses: [TrackStatus?] = []
        c.drainEventsWithDetail { _, detail in
            if let g = detail?.requestGoaway {
                named.append(g.request)
                statuses.append(g.trackStatus)
            }
        }
        #expect(named == [.subscription(second)])
        #expect(named.first != .subscription(first))
        // A subscription is not a TRACK_STATUS request: no status handle.
        #expect(statuses.count == 1)
        #expect(statuses.first.map { $0 == nil } == true)
    }

    @Test("A request GOAWAY names the saved TRACK_STATUS request")
    func trackStatusGoawayIdentity() throws {
        let (c, s) = try pair()
        var opened: [StreamRef] = []
        let first = try c.trackStatus(TrackStatusConfiguration(
            track: TrackName(namespace: ns, name: Data("a".utf8))))
        try settle(c, s, &opened)
        let second = try c.trackStatus(TrackStatusConfiguration(
            track: TrackName(namespace: ns, name: Data("b".utf8))))
        try settle(c, s, &opened)
        #expect(opened.count == 2)
        guard opened.count == 2 else { return }
        s.drainEventsWithDetail { _, _ in }
        c.drainEventsWithDetail { _, _ in }
        // The responder migrates the second request before answering it:
        // GOAWAY on its request stream (type 0x10, empty URI, Timeout 0).
        let goaway: [UInt8] = [0x10, 0x00, 0x02, 0x00, 0x00]
        try goaway.withUnsafeBytes {
            try c.receiveBidiStreamBytes(on: opened[1], $0, fin: false, now: 0)
        }
        var infos: [RequestGoawayInfo] = []
        c.drainEventsWithDetail { _, detail in
            if let g = detail?.requestGoaway { infos.append(g) }
        }
        #expect(infos.count == 1)
        let g = try #require(infos.first)
        #expect(g.trackStatus == second)
        #expect(g.trackStatus != first)
        #expect(g.request == .trackStatus(handle: second.id))
    }

    @Test("A refused own PUBLISH update names its publication")
    func publishErrorIdentity() throws {
        let (c, s) = try pair()
        var opened: [StreamRef] = []
        let p1 = try c.publish(PublishConfiguration(
            track: TrackName(namespace: ns, name: Data("p1".utf8))))
        let p2 = try c.publish(PublishConfiguration(
            track: TrackName(namespace: ns, name: Data("p2".utf8))))
        try settle(c, s, &opened)
        #expect(opened.count == 2)
        var peers: [Publication] = []
        for ev in s.pollEventsExtended() {
            if case .publishRequest(let r) = ev { peers.append(r.publication) }
        }
        #expect(peers.count == 2)
        for p in peers { try s.acceptPublish(p) }
        try settle(c, s, &opened)
        c.drainEventsWithDetail { _, _ in }
        guard opened.count == 2 else { return }
        var update = SubscriptionUpdateConfiguration()
        update.objectDeliveryTimeoutMs = 100
        // The first update is refused by the subscriber (REQUEST_ERROR 0x3,
        // reason "no", on that publication's request stream).
        try c.updatePublication(p1, update)
        c.drainActionsExtended { _ in }
        let refusal: [UInt8] = [0x05, 0x00, 0x05, 0x03, 0x00, 0x02, 0x6e, 0x6f]
        try refusal.withUnsafeBytes {
            try c.receiveBidiStreamBytes(on: opened[0], $0, fin: false, now: 0)
        }
        var refused: [PublishErrorInfo] = []
        c.drainEventsWithDetail { _, detail in
            if let e = detail?.publishError { refused.append(e) }
        }
        #expect(refused.count == 1)
        let r = try #require(refused.first)
        #expect(r.publication == p1)
        #expect(r.publication != p2)
        #expect(r.errorCode == 0x3)
        #expect(r.reason == Data("no".utf8))
        // Only that publication ends, with UPDATE_FAILED; the other updates.
        try c.finishPublish(r.publication, statusCode: 0x8, streamCount: 0)
        try c.updatePublication(p2, update)
        try settle(c, s, &opened)
        var acknowledged: [Publication] = []
        c.drainEventsWithDetail { ev, _ in
            if case .publicationUpdateOk(let ok) = ev { acknowledged.append(ok.publication) }
        }
        #expect(acknowledged == [p2])
    }
}
