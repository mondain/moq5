import Testing
import Foundation
import CMoQCore
@testable import MoQ
import MoQTransportModel

/// Track Properties and TRACK_STATUS through the Swift binding: the session
/// sends Track Properties on SUBSCRIBE_OK, FETCH_OK, TRACK_STATUS_OK and
/// PUBLISH and reports them on the answers; INCLUDE_PROPERTIES empties an
/// answer and is unsupported before draft-22; the publisher facade states a
/// track's properties with DYNAMIC_GROUPS added, answers TRACK_STATUS on tick
/// and reports a subscriber's NEW_GROUP_REQUEST.
@Suite("Track Properties")
struct TrackPropertiesTests {

    private struct PumpError: Error {}

    private static let peerBit: [UInt64] = [1 << 40, 1 << 41]
    private static let props = Data([0x78, 0x01])

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
            case .closeBidiStream:
                break
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
        if c.bindingRequirements != 0 {
            try bindInMemoryTransport(c, s)
        }
        try c.start()
        if version != .draft16 { try s.start() }
        try settle(c, s)
        _ = c.pollEventsExtended()
        _ = s.pollEventsExtended()
        return (c, s)
    }

    private func track(_ name: String = "v") -> TrackName {
        TrackName(namespace: Namespace([NamespacePart(Data("tp".utf8))]),
                  name: Data(name.utf8))
    }

    /// The answers carry the responder's properties, or none when the request
    /// asked for none.
    @Test("Answers carry Track Properties; INCLUDE_PROPERTIES empties them",
          arguments: [nil, true, false] as [Bool?])
    func answers(include: Bool?) throws {
        let (c, s) = try pair(.draft22)
        let want = include ?? true ? Self.props : Data()

        var ts = TrackStatusConfiguration(track: track())
        ts.includeProperties = include
        try c.trackStatus(ts)
        try settle(c, s)
        var request: TrackStatus?
        for ev in s.pollEventsExtended() {
            if case .trackStatusRequest(let r) = ev {
                request = r.request
                #expect(r.track == track())
            }
        }
        var accept = AcceptTrackStatusConfiguration()
        accept.largest = (9, 0)
        accept.trackProperties = Self.props
        try s.acceptTrackStatus(try #require(request), config: accept)
        try settle(c, s)
        var statusProps: Data?
        for ev in c.pollEventsExtended() {
            if case .trackStatusOk(let ok) = ev {
                statusProps = ok.trackProperties
                #expect(ok.largestGroup == 9)
            }
        }
        #expect(statusProps == want)

        var sub = SubscribeConfiguration(track: track(), filter: .latestObject)
        sub.includeProperties = include
        try c.subscribe(sub)
        try settle(c, s)
        var ssub: Subscription?
        for ev in s.pollEventsExtended() {
            if case .subscribeRequest(let r) = ev { ssub = r.subscription }
        }
        var acc = AcceptSubscribeConfiguration()
        acc.trackProperties = Self.props
        try s.acceptSubscribe(try #require(ssub), config: acc)
        try settle(c, s)
        var subProps: Data?
        for ev in c.pollEventsExtended() {
            if case .subscribeOk(let ok) = ev { subProps = ok.trackProperties }
        }
        #expect(subProps == want)

        var fetch = FetchConfiguration(track: track())
        fetch.startGroup = 1
        fetch.endGroup = 1
        fetch.endObject = 1
        fetch.includeProperties = include
        try c.fetch(fetch)
        try settle(c, s)
        var sfetch: Fetch?
        for ev in s.pollEventsExtended() {
            if case .fetchRequest(let r) = ev { sfetch = r.fetch }
        }
        try s.acceptFetch(try #require(sfetch), endGroup: 1, endObject: 1,
                          trackProperties: Self.props)
        try settle(c, s)
        var fetchProps: Data?
        for ev in c.pollEventsExtended() {
            if case .fetchOk(let ok) = ev { fetchProps = ok.trackProperties }
        }
        #expect(fetchProps == want)
    }

    @Test("PUBLISH carries Track Properties")
    func publish() throws {
        let (c, s) = try pair(.draft22)
        var cfg = PublishConfiguration(track: track())
        cfg.trackProperties = Self.props
        try c.publish(cfg)
        try settle(c, s)
        var seen: Data?
        for ev in s.pollEventsExtended() {
            if case .publishRequest(let r) = ev { seen = r.trackProperties }
        }
        #expect(seen == Self.props)
    }

    @Test("INCLUDE_PROPERTIES is unsupported before draft-22")
    func unsupported() throws {
        let (c, _) = try pair(.draft18)
        var ts = TrackStatusConfiguration(track: track())
        ts.includeProperties = false
        #expect(throws: MoQError.unsupported) { try c.trackStatus(ts) }
        var sub = SubscribeConfiguration(track: track())
        sub.includeProperties = false
        #expect(throws: MoQError.unsupported) { try c.subscribe(sub) }
    }

    /// The facade states the track's properties with DYNAMIC_GROUPS added,
    /// answers a TRACK_STATUS for its track on tick, refuses one for a track
    /// it lacks, and reports the SUBSCRIBE's NEW_GROUP_REQUEST.
    @Test("Publisher facade: DYNAMIC_GROUPS, TRACK_STATUS, new-group requests")
    func publisherFacade() throws {
        let (c, s) = try pair(.draft22)
        let publisher = try Publisher(session: s)
        var requests: [(PublisherTrack, PublisherNewGroupRequest)] = []
        publisher.onNewGroupRequest = { track, result in
            if case .success(let r) = result { requests.append((track, r)) }
        }
        let pt = try publisher.addTrack(
            namespace: track().namespace, name: "v", advertiseNamespace: false,
            trackProperties: Self.props, dynamicGroups: true)

        var sub = SubscribeConfiguration(track: track(), filter: .latestObject)
        sub.newGroupRequest = 0
        try c.subscribe(sub)
        try c.trackStatus(TrackStatusConfiguration(track: track()))
        try c.trackStatus(TrackStatusConfiguration(track: track("absent")))
        for _ in 0..<4 {
            try settle(c, s)
            try publisher.tick(now: 0)
        }
        try settle(c, s)
        let stated = Data([0x30, 0x01, 0x48, 0x01])
        var subOk: SubscribeOkInfo?
        var statusOk: TrackStatusOkInfo?
        var statusError: TrackStatusErrorInfo?
        for ev in c.pollEventsExtended() {
            switch ev {
            case .subscribeOk(let ok): subOk = ok
            case .trackStatusOk(let ok): statusOk = ok
            case .trackStatusError(let e): statusError = e
            default: break
            }
        }
        #expect(subOk?.trackProperties == stated)
        #expect(subOk?.dynamicGroups == true)
        #expect(statusOk?.trackProperties == stated)
        #expect(statusError?.errorCode == 0x10)
        #expect(requests.count == 1)
        #expect(requests.first?.0 == pt)
        #expect(requests.first?.1.requestedGroup == 0)
        #expect(requests.first?.1.currentGroup == nil)
    }

    /// The TRACK_STATUS_OK properties are read only when the C detail covers
    /// them.
    @Test("TRACK_STATUS_OK properties gate on the detail size")
    func detailGate() {
        let bytes: [UInt8] = [0x78, 0x01]
        bytes.withUnsafeBufferPointer { bp in
            var ev = moq_event_t()
            ev.kind = MOQ_EVENT_TRACK_STATUS_OK
            ev.u.track_status_ok.track_properties = moq_bytes_t(data: bp.baseAddress, len: 2)
            ev.detail_size = UInt32(MemoryLayout<moq_track_status_ok_event_t>.size)
            if case .trackStatusOk(let full) = ExtendedEvent.from(&ev) {
                #expect(full.trackProperties == Data(bytes))
            } else {
                Issue.record("expected trackStatusOk")
            }
            ev = moq_event_t()
            ev.kind = MOQ_EVENT_TRACK_STATUS_OK
            ev.u.track_status_ok.track_properties = moq_bytes_t(data: bp.baseAddress, len: 2)
            ev.detail_size = UInt32(MemoryLayout<moq_track_status_ok_event_t>.offset(
                of: \moq_track_status_ok_event_t.track_properties)!)
            if case .trackStatusOk(let short) = ExtendedEvent.from(&ev) {
                #expect(short.trackProperties.isEmpty)
            } else {
                Issue.record("expected trackStatusOk")
            }
        }
    }
}
