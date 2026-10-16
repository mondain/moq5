import Testing
import Foundation
@testable import MoQ
import MoQTransportModel

/// Fills (draft-22 section 3.4) through the Swift session wrapper: a
/// SUBSCRIBE that joins at the current group (Next Object plus a fill of the
/// current group) is answered by the publisher with the fill operations; the
/// subscriber sees the fill-delivered object, a gap and the COMPLETE
/// terminal; a failed fill carries its stream code; a draft without fills
/// refuses before anything is sent.
@Suite("Fills")
struct FillTests {

    private struct PumpError: Error {}

    /// Streams reach the peer under a ref of its own: the opener's ref with a
    /// per-direction bit, and a reply on that ref comes back without it.
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
            case .sendData(let ref, let header, let payload, let fin):
                var bytes = [UInt8](header)
                payload?.withUnsafeBytes { bytes.append(contentsOf: $0) }
                try bytes.withUnsafeBytes {
                    try to.receiveDataBytes(on: toPeer(ref, dir), $0, fin: fin, now: 0)
                }
            case .resetData(let ref, let code, _):
                try to.receiveDataReset(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .stopData(let ref, let code):
                try to.receiveDataStop(on: toPeer(ref, dir), errorCode: code, now: 0)
            case .openBidiStream(let ref, let d, let fin), .sendBidiStream(let ref, let d, let fin):
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
            case .sendDatagram(let d):
                try d.withUnsafeBytes { try to.receiveDatagram($0, now: 0) }
            case .closeBidiStream: break
            case .closeSession, .unknown:
                throw PumpError()
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
        #expect(c.wireVersion == version)
        if c.bindingRequirements != 0 {
            try bindInMemoryTransport(c, s)
        }
        try c.start()
        if version != .draft16 { try s.start() }   // both sides open control
        try settle(c, s)
        #expect(c.state == .established)
        #expect(s.state == .established)
        _ = c.pollEventsExtended()
        _ = s.pollEventsExtended()
        return (c, s)
    }

    private func track() -> TrackName {
        TrackName(namespace: Namespace([NamespacePart(Data("fill".utf8))]),
                  name: Data("v".utf8))
    }

    /// Subscribe joining at the current group; the server accepts with
    /// Largest (3,2). Returns the server's request and the client's child.
    private func joinCurrentGroup(_ c: Session, _ s: Session) throws
        -> (FillRequestInfo?, Fill?)
    {
        var cfg = SubscribeConfiguration(track: track(), filter: .latestObject)
        cfg.location = .nextObject
        cfg.fill = .currentGroup
        try c.subscribe(cfg)
        try settle(c, s)
        var ssub: Subscription?
        for ev in s.pollEventsExtended() {
            if case .subscribeRequest(let r) = ev { ssub = r.subscription }
        }
        guard let sub = ssub else { return (nil, nil) }
        var acc = AcceptSubscribeConfiguration()
        acc.largest = (3, 2)
        try s.acceptSubscribe(sub, config: acc)
        try settle(c, s)
        var created: Fill?
        for ev in c.pollEventsExtended() {
            if case .fillCreated(let fc) = ev { created = fc.fill }
        }
        var request: FillRequestInfo?
        for ev in s.pollEventsExtended() {
            if case .fillRequest(let fr) = ev { request = fr }
        }
        return (request, created)
    }

    @Test("A fill joining the current group is served and ends COMPLETE")
    func served() throws {
        let (c, s) = try pair(.draft22)
        let (req, cfill) = try joinCurrentGroup(c, s)
        let r = try #require(req)
        #expect(cfill != nil)
        #expect(r.startGroup == 3 && r.startObject == 0)
        #expect(r.endGroup == 3 && r.endObject == 2)

        try s.openFill(r.fill)
        try s.writeFillObject(r.fill, groupID: 3, objectID: 0, publisherPriority: 5,
                              payload: try Buffer(Data("one".utf8)))
        try s.writeFillRange(r.fill, kind: .nonExistent, groupID: 3, objectID: 1)
        try s.endFill(r.fill)
        try settle(c, s)

        var objects = 0, gaps = 0, terminals = 0
        for ev in c.pollEventsExtended() {
            switch ev {
            case .fillObject(let o):
                objects += 1
                #expect(o.fill == cfill)
                #expect(o.groupID == 3 && o.objectID == 0)
                #expect(o.payload.map { Data($0.copyBytes()) } == Data("one".utf8))
            case .fillGap(let g):
                gaps += 1
                #expect(g.kind == .nonExistent && g.objectID == 1)
            case .fillTerminal(let t):
                terminals += 1
                #expect(t.reason == .complete && t.domain == .none)
            default: break
            }
        }
        #expect(objects == 1 && gaps == 1 && terminals == 1)
        #expect(throws: MoQError.staleHandle) { try c.cancelFill(cfill!) }
    }

    @Test("A failed fill carries its stream code; the subscription continues")
    func failed() throws {
        let (c, s) = try pair(.draft22)
        let (req, _) = try joinCurrentGroup(c, s)
        let r = try #require(req)
        try s.abortFill(r.fill, errorCode: 0x6)
        try settle(c, s)
        var terminal: FillTerminalInfo?
        for ev in c.pollEventsExtended() {
            if case .fillTerminal(let t) = ev { terminal = t }
        }
        let t = try #require(terminal)
        #expect(t.reason == .peerCancelledOrReset)
        #expect(t.domain == .stream)
        #expect(t.errorCode == 0x6)
        #expect(c.state == .established)
    }

    @Test("A draft without fills refuses before anything is sent")
    func refusedOnDraft18() throws {
        let (c, _) = try pair(.draft18)
        var cfg = SubscribeConfiguration(track: track(), filter: .latestObject)
        cfg.fill = .currentGroup
        #expect(throws: MoQError.unsupported) { try c.subscribe(cfg) }
        #expect(c.pollActionsExtended().isEmpty)
    }
}
