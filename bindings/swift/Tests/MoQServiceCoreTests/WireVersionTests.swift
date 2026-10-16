import Foundation
import Testing
@testable import MoQServiceCore

@Suite("Wire version model")
struct WireVersionTests {
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test func legacyOfferIsUnchanged() throws {
        func oldOffer(_ offer: VersionOffer) -> Int {
            switch offer {
            case .automatic: return 0
            case .list(let versions): return versions.count
            case .exactly(let version): return version.draftNumber
            }
        }
        let cfg = MoQEndpoint.Configuration(url: URL(string: "moqt://sim.invalid")!)
        #expect(oldOffer(cfg.versions) == 0)
        #expect(cfg.wireVersions == nil && cfg.preferredWireVersion == nil)
        #expect(MoQVersion.allCases.map(\.draftNumber) == [16, 18])
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test func conflictingOffersAreRefused() throws {
        var cfg = MoQEndpoint.Configuration(
            url: URL(string: "moqt://sim.invalid")!, wireVersion: .draft22)
        try cfg.validate()
        cfg.versions = .exactly(.draft18)
        #expect(throws: MoQServiceError.self) { try cfg.validate() }
        cfg.versions = .automatic
        cfg.wireVersions = .automatic
        cfg.preferredWireVersion = .draft22
        try cfg.validate()
        cfg.preferredVersion = .draft16
        #expect(throws: MoQServiceError.self) { try cfg.validate() }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test func wireTokenSurvivesClose() async {
        let backend = ScriptedEndpointBackend()
        backend.setWireVersion(.draft22)
        let endpoint = MoQEndpoint(
            configuration: .init(url: URL(string: "moqt://sim.invalid")!, wireVersion: .draft22),
            engine: EndpointEngine(backend: backend))
        #expect(endpoint.negotiatedVersion == nil)
        #expect(endpoint.negotiatedWireVersion == .draft22)
        await endpoint.close()
        #expect(endpoint.negotiatedWireVersion == .draft22)
        #expect(backend.snapshot().violations.isEmpty)
    }
}
