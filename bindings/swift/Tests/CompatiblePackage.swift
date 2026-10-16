// swift-tools-version: 6.0
import PackageDescription
import Foundation

// The runner links only existing local native archives and selected real Swift
// source directories. There are no dependencies or consumer example targets.
let service = ProcessInfo.processInfo.environment["MOQ_SWIFT_COMPATIBLE_SERVICE"] == "1"
let package = Package(
    name: "SwiftCompatible",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "MoQTransportModel", path: "MoQTransportModel"),
        .target(name: "MoQServiceCore", dependencies: ["MoQTransportModel"], path: "MoQServiceCore"),
        .testTarget(name: "MoQServiceCoreTests", dependencies: ["MoQServiceCore"], path: "MoQServiceCoreTests"),
    ])
if service {
    package.targets += [
        .systemLibrary(name: "CMoQService", path: "CMoQService"),
        .target(name: "MoQService", dependencies: ["CMoQService", "MoQServiceCore"], path: "MoQService"),
        .testTarget(name: "MoQServiceTests", dependencies: ["MoQService"], path: "MoQServiceTests"),
    ]
} else {
    package.targets += [
        .systemLibrary(name: "CMoQCore", path: "CMoQCore"),
        .systemLibrary(name: "CMoQSim", path: "CMoQSim"),
        .systemLibrary(name: "CMoQLOC", path: "CMoQLOC"),
        .systemLibrary(name: "CMoQCMAF", path: "CMoQCMAF"),
        .systemLibrary(name: "CMoQMediaObject", path: "CMoQMediaObject"),
        .systemLibrary(name: "CMoQPlayback", path: "CMoQPlayback"),
        .target(name: "MoQ", dependencies: ["CMoQCore", "MoQTransportModel"], path: "MoQ"),
        .target(name: "MoQCatalog", path: "MoQCatalog"),
        .target(name: "MoQMedia", dependencies: ["MoQ", "MoQCatalog", "CMoQLOC", "CMoQCMAF", "CMoQMediaObject", "CMoQPlayback"], path: "MoQMedia"),
        .testTarget(name: "MoQTests", dependencies: ["MoQ", "CMoQSim"], path: "MoQTests"),
        .testTarget(name: "MoQMediaTests", dependencies: ["MoQMedia", "CMoQSim"], path: "MoQMediaTests"),
        .testTarget(name: "TransportVersionAliasTests", dependencies: ["MoQMedia", "MoQServiceCore"], path: "TransportVersionAliasTests"),
    ]
}
