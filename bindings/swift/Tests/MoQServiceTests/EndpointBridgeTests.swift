import Testing
import Foundation
import CMoQService
@testable import MoQService

/* Deterministic, NO-SOCKET bridge tests: configuration mapping into the
 * real C structs, enum/version round-trips, result mapping, and synchronous
 * preflight rejection. Nothing here calls moq_endpoint_connect with a
 * dialable configuration; real connect/attach/close smokes are gated behind
 * MOQ_SERVICE_NET_SMOKE=1 (never default CI). */

@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
private func string(_ bytes: moq_bytes_t) -> String {
    guard let data = bytes.data, bytes.len > 0 else { return "" }
    return String(decoding: UnsafeBufferPointer(start: data, count: bytes.len),
                  as: UTF8.self)
}

/* MOQ_ENDPOINT_URI_STANDARD / MOQ_ENDPOINT_URI_LEGACY are UINT32_C()
 * macros, which Swift does not import; these are their header values.
 * "The SDK reads each URI mode as that mode" below proves the installed
 * library reads them the same way. */
private let uriStandard: UInt32 = 1
private let uriLegacy: UInt32 = 0

@Suite("Endpoint configuration mapping")
struct EndpointConfigurationMappingTests {

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The model configuration maps field-for-field into moq_endpoint_cfg_t")
    func fullMapping() throws {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com:4443/session")!)
        configuration.transportProtocol = .webTransport
        configuration.backend = .picoquic
        configuration.serverName = "relay.example.com"
        configuration.insecureSkipVerify = true
        configuration.uriMode = .legacy
        configuration.webTransportPath = "/moq"

        withEndpointCFG(configuration) { cfg in
            #expect(string(cfg.url) == "https://relay.example.com:4443/session")
            #expect(cfg.`protocol` == MOQ_TRANSPORT_PROTOCOL_WEBTRANSPORT)
            #expect(cfg.backend == MOQ_TRANSPORT_BACKEND_PICOQUIC)
            #expect(string(cfg.sni) == "relay.example.com")
            #expect(cfg.insecure_skip_verify == true)
            #expect(string(cfg.wt_path) == "/moq")
            #expect(cfg.uri_mode == uriLegacy)
            #expect(cfg.ca_file.len == 0)
            /* AUTO version offer: zero-init policy. */
            #expect(cfg.versions.policy == MOQ_VERSION_POLICY_AUTO)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The cfg is sized to cover every tail field; defaults are LEGACY with no preference")
    func sizedDefaults() {
        let configuration = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com/session")!)
        withEndpointCFG(configuration) { cfg in
            #expect(Int(cfg.struct_size) == MemoryLayout<moq_endpoint_cfg_t>.size)
            #expect(cfg.uri_mode == uriLegacy)
            #expect(cfg.preferred_version == moq_version_t(rawValue: 0))
            #expect(cfg.wt_path.len == 0)
            #expect(cfg.versions.policy == MOQ_VERSION_POLICY_AUTO)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("URI mode and preferred version reach the C cfg")
    func uriModeAndPreferredVersion() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "moqt://relay.example.com")!)
        configuration.uriMode = .standard
        withEndpointCFG(configuration) { cfg in
            #expect(cfg.uri_mode == uriStandard)
        }
        configuration.uriMode = .legacy
        withEndpointCFG(configuration) { cfg in
            #expect(cfg.uri_mode == uriLegacy)
            #expect(cfg.preferred_version == moq_version_t(rawValue: 0))
        }
        for version in MoQVersion.allCases {
            configuration.preferredVersion = version
            withEndpointCFG(configuration) { cfg in
                #expect(cfg.preferred_version == cVersion(version))
                #expect(cfg.versions.policy == MOQ_VERSION_POLICY_AUTO)
            }
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Every backend maps to its C constant (exhaustive)")
    func backendMapping() {
        // exhaustive by construction: a new enum case breaks this list at
        // compile time via the switch in cBackend, and at review time here
        let cases: [(MoQEndpoint.TransportBackend, moq_transport_backend_t)] = [
            (.automatic, MOQ_TRANSPORT_BACKEND_AUTO),
            (.picoquic, MOQ_TRANSPORT_BACKEND_PICOQUIC),
            (.mvfst, MOQ_TRANSPORT_BACKEND_MVFST),
            (.proxygen, MOQ_TRANSPORT_BACKEND_PROXYGEN),
            (.msquic, MOQ_TRANSPORT_BACKEND_MSQUIC),
            (.wtquicNetwork, MOQ_TRANSPORT_BACKEND_WTQUIC_NETWORK),
            (.wtquicMsquic, MOQ_TRANSPORT_BACKEND_WTQUIC_MSQUIC),
        ]
        for (swift, c) in cases {
            #expect(cBackend(swift) == c)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Version offers map to LIST/EXACT with the draft values")
    func versionOffers() throws {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "moqt://h:1")!)
        /* The offer's own struct_size: 0 means AUTO to the SDK, whatever
         * the policy field says. */
        let offerSize = MemoryLayout<moq_version_offer_t>.size

        withEndpointCFG(configuration) { cfg in
            #expect(cfg.versions.struct_size == 0)
        }
        configuration.versions = .list([.draft18, .draft16])
        withEndpointCFG(configuration) { cfg in
            #expect(Int(cfg.versions.struct_size) == offerSize)
            #expect(cfg.versions.policy == MOQ_VERSION_POLICY_LIST)
            #expect(cfg.versions.version_count == 2)
            #expect(cfg.versions.versions?[0] == MOQ_VERSION_DRAFT_18)
            #expect(cfg.versions.versions?[1] == MOQ_VERSION_DRAFT_16)
        }
        configuration.versions = .exactly(.draft16)
        withEndpointCFG(configuration) { cfg in
            #expect(Int(cfg.versions.struct_size) == offerSize)
            #expect(cfg.versions.policy == MOQ_VERSION_POLICY_EXACT)
            #expect(cfg.versions.version_count == 1)
            #expect(cfg.versions.versions?[0] == MOQ_VERSION_DRAFT_16)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Version values round-trip both directions")
    func versionRoundTrip() {
        #expect(cVersion(.draft16) == MOQ_VERSION_DRAFT_16)
        #expect(cVersion(.draft18) == MOQ_VERSION_DRAFT_18)
        #expect(swiftVersion(MOQ_VERSION_DRAFT_16) == .draft16)
        #expect(swiftVersion(MOQ_VERSION_DRAFT_18) == .draft18)
        #expect(WireVersion.draft22.rawValue == MOQ_VERSION_DRAFT_22.rawValue)
        #expect(swiftVersion(MOQ_VERSION_DRAFT_22) == nil)
        #expect(swiftVersion(moq_version_t(rawValue: 0)) == nil)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Extended wire offers are explicit and preserve native values")
    func extendedWireOffers() throws {
        var cfg = MoQEndpoint.Configuration(
            url: URL(string: "moqt://h:1")!, wireVersion: .draft22)
        try cfg.validate()
        withEndpointCFG(cfg) { native in
            #expect(native.versions.policy == MOQ_VERSION_POLICY_EXACT)
            #expect(native.versions.version_count == 1)
            #expect(native.versions.versions?[0] == MOQ_VERSION_DRAFT_22)
        }
        cfg.wireVersions = .list([.draft22, .draft18, .draft16])
        try cfg.validate()
        withEndpointCFG(cfg) { native in
            #expect(native.versions.policy == MOQ_VERSION_POLICY_LIST)
            #expect(native.versions.version_count == 3)
            #expect(native.versions.versions?[0] == MOQ_VERSION_DRAFT_22)
        }
        cfg.preferredWireVersion = .draft22
        #expect(throws: MoQServiceError.self) { try cfg.validate() }
        cfg.wireVersions = .automatic
        try cfg.validate()
        withEndpointCFG(cfg) { native in
            #expect(native.versions.struct_size == 0)
            #expect(native.preferred_version == MOQ_VERSION_DRAFT_22)
        }
    }
}

@Suite("Endpoint bridge preflight")
struct EndpointBridgePreflightTests {

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("connect() rejects malformed configurations before any I/O")
    func synchronousRejection() {
        #expect(throws: MoQServiceError.self) {
            _ = try MoQEndpoint.connect(
                to: URL(string: "ftp://relay.example.com:4443")!)
        }
        #expect(throws: MoQServiceError.self) {
            _ = try MoQEndpoint.connect(to: URL(string: "moqt://")!)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("connect() refuses a standard-mode WebTransport path with its own error")
    func standardModePathRefused() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com/session")!)
        configuration.uriMode = .standard
        configuration.webTransportPath = "/moq"
        #expect(throws: MoQServiceError.invalidArgument(
            "webTransportPath is a URIMode.legacy override; under "
            + "URIMode.standard the URL carries the WebTransport path")) {
            _ = try MoQEndpoint.connect(configuration: configuration)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("connect() refuses a preferred version beside an explicit offer with its own error")
    func preferenceBesideExplicitOfferRefused() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "moqt://relay.example.com")!)
        configuration.versions = .list([.draft18])
        configuration.preferredWireVersion = .draft22
        #expect(throws: MoQServiceError.invalidArgument(
            "preferredVersion applies only to an automatic version offer; "
            + "a list or exact offer already gives the order")) {
            _ = try MoQEndpoint.connect(configuration: configuration)
        }
    }

    /* The two Swift refusals above mirror rules the C resolver also
     * enforces. These pin that contract on the installed SDK: both cfgs
     * are refused during resolution, before any backend is chosen, so no
     * endpoint is created and nothing is dialed. */
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The SDK resolver refuses the same two configurations with INVAL")
    func resolverContract() {
        var standardPath = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com/session")!)
        standardPath.uriMode = .standard
        standardPath.webTransportPath = "/moq"
        var preferenceWithList = MoQEndpoint.Configuration(
            url: URL(string: "moqt://relay.example.com")!)
        preferenceWithList.versions = .list([.draft18])
        preferenceWithList.preferredVersion = .draft18
        for configuration in [standardPath, preferenceWithList] {
            var endpoint: OpaquePointer?
            let rc = withEndpointCFG(configuration) { cfg in
                withUnsafePointer(to: cfg) { moq_endpoint_connect($0, &endpoint) }
            }
            #expect(rc == moq_result_t(MOQ_ERR_INVAL))
            #expect(endpoint == nil)
        }
    }

    /* The same cfg under each mode: STANDARD refuses the path during URL
     * resolution (INVAL); LEGACY accepts it and resolution goes on to the
     * backend check, where mvfst cannot serve WebTransport (UNSUPPORTED,
     * whatever backends the SDK has). Nothing is dialed either way. */
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The SDK reads each URI mode as that mode")
    func sdkReadsURIModes() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com/session")!)
        configuration.transportProtocol = .webTransport
        configuration.backend = .mvfst
        configuration.webTransportPath = "/moq"
        let expected: [(MoQEndpoint.URIMode, Int32)] = [
            (.standard, MOQ_ERR_INVAL),
            (.legacy, MOQ_ERR_UNSUPPORTED),
        ]
        for (mode, code) in expected {
            configuration.uriMode = mode
            var endpoint: OpaquePointer?
            let rc = withEndpointCFG(configuration) { cfg in
                withUnsafePointer(to: cfg) { moq_endpoint_connect($0, &endpoint) }
            }
            #expect(rc == moq_result_t(code))
            #expect(endpoint == nil)
        }
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("An unchanged default client path passes the SDK URI checks without I/O")
    func defaultClientPathOverride() {
        var configuration = MoQEndpoint.Configuration(
            url: URL(string: "https://relay.example.com")!)
        configuration.transportProtocol = .webTransport
        configuration.backend = .mvfst
        configuration.webTransportPath = "/moq"
        #expect(throws: Never.self) { try configuration.validate() }
        var endpoint: OpaquePointer?
        let rc = withEndpointCFG(configuration) { cfg in
            withUnsafePointer(to: cfg) { moq_endpoint_connect($0, &endpoint) }
        }
        #expect(rc == moq_result_t(MOQ_ERR_UNSUPPORTED))
        #expect(endpoint == nil)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("The installed-stack canary is live")
    func canary() {
        #expect(moqSwiftStackGuardService() == 2)
    }

    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("C result codes map to service errors")
    func resultMapping() {
        #expect(serviceError(fromCreate: moq_result_t(MOQ_ERR_UNSUPPORTED),
                             what: "x") == .unsupported)
        #expect(serviceError(fromCreate: moq_result_t(MOQ_ERR_NOMEM),
                             what: "x") == .outOfMemory)
        #expect(serviceError(fromCreate: moq_result_t(MOQ_ERR_WRONG_STATE),
                             what: "x") == .wrongState)
    }
}

@Suite("Endpoint bridge network smoke",
       .enabled(if: ProcessInfo.processInfo.environment["MOQ_SERVICE_NET_SMOKE"] == "1"))
struct EndpointBridgeNetworkSmokeTests {

    /* Real sockets: opt-in only (MOQ_SERVICE_NET_SMOKE=1), never default
     * CI. Asserts clean teardown, not connection progress. */
    @available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
    @Test("Connect to a dead port, then close, tears down cleanly")
    func lifecycleSmoke() async throws {
        let endpoint = try MoQEndpoint.connect(
            to: URL(string: "moqt://127.0.0.1:1")!)
        #expect(endpoint.state == .connecting || endpoint.state == .closed)
        endpoint.interrupt()
        endpoint.resume()
        await endpoint.close()
        #expect(endpoint.state == .closed)
    }
}
