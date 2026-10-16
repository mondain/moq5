import Foundation
import Testing
@testable import MoQServiceCore

/// The session-wide GOAWAY's New Session URI is readable from the endpoint:
/// absent before one arrives, the backend's value while it lives, and the
/// value retired with it after close (the backend is not consulted then).
@Suite("Endpoint GOAWAY URI")
struct EndpointGoawayTests {

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The GOAWAY URI is read through the engine and survives close")
    func goawayURI() async throws {
        let backend = ScriptedEndpointBackend()
        let endpoint = MoQEndpoint(
            configuration: .init(url: URL(string: "moqt://relay.test:4443")!),
            engine: EndpointEngine(backend: backend))
        backend.script(state: .established, version: .draft18)
        try await endpoint.established()
        #expect(endpoint.goawayNewSessionURI == nil)

        let uri = Data("moqt://next.test/".utf8)
        backend.setGoawayURI(uri)
        #expect(endpoint.goawayNewSessionURI == uri)

        await endpoint.close()
        backend.setGoawayURI(Data("moqt://other.test/".utf8))
        #expect(endpoint.goawayNewSessionURI == uri)
        #expect(backend.snapshot().violations.isEmpty)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("SETUP authorization tokens default to none and are kept as given")
    func setupTokensConfiguration() {
        var configuration = MoQEndpoint.Configuration(url: URL(string: "moqt://relay.test")!)
        #expect(configuration.setupAuthorizationTokens.isEmpty)
        let token = MoQEndpoint.SetupAuthorizationToken(tokenType: 7, value: Data([1, 2]))
        configuration.setupAuthorizationTokens = [token]
        #expect(configuration.setupAuthorizationTokens == [token])
    }
}
