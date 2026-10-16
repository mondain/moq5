import Testing
import Foundation
import CMoQCore
@testable import MoQ
import MoQTransportModel

/// Draft-22 namespace updates, SUBSCRIBE_TRACKS options and RENDEZVOUS_TIMEOUT
/// through the Swift binding: configurations reach the C session through the
/// sized initializers, and every observation is copied out of the session's
/// borrowed record and compared exactly.
@Suite("Namespace updates")
struct NamespaceUpdateTests {

    private struct PumpError: Error {}

    private static let peerBit: [UInt64] = [1 << 40, 1 << 41]

    private func toPeer(_ ref: StreamRef, _ dir: Int) -> StreamRef {
        let back = Self.peerBit[dir ^ 1]
        if ref.rawValue & back != 0 { return StreamRef(rawValue: ref.rawValue & ~back) }
        return StreamRef(rawValue: ref.rawValue | Self.peerBit[dir])
    }

    private func pump(_ from: Session, _ to: Session, _ dir: Int) throws {
        try from.drainActionsExtended { action in
            switch action {
            case .sendControl(let d), .openUniControl(_, let d), .sendUniControl(_, let d, _):
                try d.withUnsafeBytes { try to.receiveControlBytes($0, now: 0) }
            case .openBidiStream(let ref, let d, let fin), .sendBidiStream(let ref, let d, let fin):
                try d.withUnsafeBytes {
                    try to.receiveBidiStreamBytes(on: toPeer(ref, dir), $0, fin: fin, now: 0)
                }
            case .resetBidiStream(let ref, let code):
                try to.receiveBidiStreamReset(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .stopBidiStream(let ref, let code):
                try to.receiveBidiStreamStop(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .closeBidiStream:
                break
            case .closeSession, .unknown:
                throw PumpError()
            default:
                break
            }
        }
    }

    private func settle(_ c: Session, _ s: Session) throws {
        for _ in 0..<6 { try pump(c, s, 0); try pump(s, c, 1) }
    }

    private func pair(_ version: WireVersion) throws -> (Session, Session) {
        var cc = Session.Configuration(perspective: .client)
        cc.sendRequestCapacity = true
        cc.initialRequestCapacity = 16
        cc.wireVersion = version
        var sc = Session.Configuration(perspective: .server)
        sc.sendRequestCapacity = true
        sc.initialRequestCapacity = 16
        sc.wireVersion = version
        let c = try Session(configuration: cc)
        let s = try Session(configuration: sc)
        if c.bindingRequirements != 0 {
            try bindInMemoryTransport(c, s)
        }
        try c.start()
        if version != .draft16 { try s.start() }
        try settle(c, s)
        #expect(c.state == .established)
        #expect(s.state == .established)
        _ = c.pollEventsExtended()
        _ = s.pollEventsExtended()
        return (c, s)
    }

    private func ns(_ parts: String...) -> Namespace {
        Namespace(parts.map { NamespacePart(Data($0.utf8)) })
    }

    @Test("An announcement update is observed, queried and accepted once")
    func announcementUpdate() throws {
        let (c, s) = try pair(.draft22)
        try s.setDeferNamespaceUpdates(true)
        let ann = try c.publishNamespace(ns("n"))
        try settle(c, s)
        var sann: Announcement?
        for ev in s.pollEventsExtended() {
            if case .namespacePublished(let p) = ev { sann = p.announcement }
        }
        let peerAnn = try #require(sann)
        try s.acceptNamespace(peerAnn)
        try settle(c, s)
        _ = c.pollEventsExtended()

        var cfg = NamespaceUpdateConfiguration()
        let token = AuthorizationToken(type: 0, value: Data([0xaa]))
        cfg.authorizationTokens = [token]
        let id = try c.updateNamespace(.announcement(ann), cfg)
        try settle(c, s)
        var requests: [NamespaceUpdateRequestInfo] = []
        for ev in s.pollEventsExtended() {
            if case .namespaceUpdateRequest(let r) = ev { requests.append(r) }
        }
        #expect(requests.count == 1)
        let r = try #require(requests.first)
        #expect(r.target == .announcement(peerAnn))
        #expect(r.update.updateRequestID == id)
        #expect(r.update.hasAuthorization)
        #expect(r.update.tokens == [token])
        #expect(r.update.prefix == nil && r.update.forward == nil)
        #expect(try s.owedNamespaceUpdate(.announcement(peerAnn)) == r.update)
        try s.acceptNamespaceUpdate(.announcement(peerAnn), updateRequestID: id)
        #expect(throws: MoQError.wrongState) {
            try s.owedNamespaceUpdate(.announcement(peerAnn))
        }
        try settle(c, s)
        var results: [NamespaceUpdateResultInfo] = []
        for ev in c.pollEventsExtended() {
            if case .namespaceUpdateResult(let r) = ev { results.append(r) }
        }
        #expect(results == [NamespaceUpdateResultInfo(
            target: .announcement(ann), updateRequestID: id, outcome: .ok,
            localRole: .requester, errorCode: 0, reason: "", expiresMs: nil)])
    }

    @Test("SUBSCRIBE_TRACKS options and a refused prefix/FORWARD update")
    func trackSubscriptionUpdate() throws {
        let (c, s) = try pair(.draft22)
        try s.setDeferNamespaceUpdates(true)
        var tc = SubscribeTracksConfiguration(prefix: ns("a"))
        tc.groupOrder = .descending
        tc.includeProperties = false
        let cts = try c.subscribeTracks(tc)
        try settle(c, s)
        var req: SubscribeTracksRequestInfo?
        for ev in s.pollEventsExtended() {
            if case .subscribeTracksRequest(let r) = ev { req = r }
        }
        let r = try #require(req)
        #expect(r.options == SubscribeTracksOptions(
            flags: UInt32(MOQ_SUBSCRIBE_TRACKS_GROUP_ORDER | MOQ_SUBSCRIBE_TRACKS_INCLUDE_PROPERTIES),
            groupOrder: .descending, includeProperties: false))
        try s.acceptSubscribeTracks(r.trackSubscription)
        try settle(c, s)
        _ = c.pollEventsExtended()

        var cfg = NamespaceUpdateConfiguration()
        cfg.prefix = ns("b")
        cfg.forward = false
        let id = try c.updateNamespace(.trackSubscription(cts), cfg)
        try settle(c, s)
        var update: NamespaceUpdateRecord?
        for ev in s.pollEventsExtended() {
            if case .namespaceUpdateRequest(let u) = ev,
               u.target == .trackSubscription(r.trackSubscription) {
                update = u.update
            }
        }
        let u = try #require(update)
        #expect(u.updateRequestID == id)
        #expect(u.prefix == ns("b"))
        #expect(u.forward == false)
        #expect(!u.hasAuthorization && u.tokens.isEmpty)
        try s.rejectNamespaceUpdate(.trackSubscription(r.trackSubscription),
                                    updateRequestID: id,
                                    errorCode: RequestErrorCode.unauthorized,
                                    reason: "no")
        try settle(c, s)
        var results: [NamespaceUpdateResultInfo] = []
        for ev in c.pollEventsExtended() {
            if case .namespaceUpdateResult(let r) = ev { results.append(r) }
        }
        #expect(results == [NamespaceUpdateResultInfo(
            target: .trackSubscription(cts), updateRequestID: id, outcome: .error,
            localRole: .requester, errorCode: RequestErrorCode.unauthorized,
            reason: "no", expiresMs: nil)])
    }

    @Test("RENDEZVOUS_TIMEOUT presence and value reach the publisher exactly")
    func rendezvousTimeout() throws {
        let (c, s) = try pair(.draft22)
        var cfg = SubscribeConfiguration(
            track: TrackName(namespace: ns("r"), name: Data("v".utf8)),
            filter: .latestObject)
        cfg.rendezvousTimeoutMs = 0
        try c.subscribe(cfg)
        try settle(c, s)
        var options: SubscriptionAdmissionOptions?
        for ev in s.pollEventsExtended() {
            if case .subscribeRequest(let r) = ev { options = r.options }
        }
        #expect(try #require(options).rendezvousTimeoutMs == 0)

        let (c18, _) = try pair(.draft18)
        cfg.rendezvousTimeoutMs = 25
        #expect(throws: MoQError.unsupported) { try c18.subscribe(cfg) }
    }
}
