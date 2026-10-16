import CMoQService
import Foundation
import MoQServiceCore
import Testing
@testable import MoQService

/// The endpoint configuration's SETUP authorization tokens reach the C cfg
/// in order with their bytes, and an empty list sends none.
@Suite("Endpoint control bridge")
struct EndpointControlBridgeTests {

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("SETUP authorization tokens reach the C cfg")
    func setupTokens() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "moqt://relay.example.com")!)
        withEndpointCFG(configuration) { cfg in
            #expect(cfg.setup_auth_tokens == nil)
            #expect(cfg.setup_auth_token_count == 0)
        }
        configuration.setupAuthorizationTokens = [
            .init(tokenType: 7, value: Data([1, 2, 3])),
            .init(tokenType: 0, value: Data()),
        ]
        withEndpointCFG(configuration) { cfg in
            #expect(cfg.setup_auth_token_count == 2)
            guard let t = cfg.setup_auth_tokens, cfg.setup_auth_token_count == 2 else { return }
            #expect(t[0].token_type == 7)
            #expect(t[0].token_value.len == 3)
            if let d = t[0].token_value.data {
                #expect(Array(UnsafeBufferPointer(start: d, count: 3)) == [1, 2, 3])
            }
            #expect(t[1].token_type == 0)
            #expect(t[1].token_value.len == 0)
        }
    }
}
