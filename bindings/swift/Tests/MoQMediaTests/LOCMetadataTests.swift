import Testing
import Foundation
import MoQ
import MoQTransportModel
@testable import MoQMedia

/// LOC companion metadata is encoded and parsed with the codepoints of the
/// LOC profile the wire version selects, losslessly.
@Suite("LOC metadata")
struct LOCMetadataTests {

    @Test("LOC metadata round-trips through each wire version's codepoints",
          arguments: [MoQTransportModel.WireVersion.draft16, .draft18, .draft22])
    func roundTrip(_ version: MoQTransportModel.WireVersion) throws {
        var m = LOCMetadata(timestamp: 90_000, marking: Data([0x01, 0x02]),
                            audioLevel: 0x7f, videoConfig: Data([0xaa, 0xbb, 0xcc]))
        if version == .draft22 {
            // LOC-04 adds the timescale and the audio config.
            m.timescale = 90_000
            m.audioConfig = Data([0x11])
        }
        let bytes = try LOCMetadataCodec.encode(wireVersion: version, metadata: m)
        #expect(!bytes.isEmpty)
        #expect(try LOCMetadataCodec.parse(wireVersion: version, properties: bytes) == m)
        #expect(try LOCMetadataCodec.parse(wireVersion: version, properties: Data()) == LOCMetadata())
    }

    @Test("LOC-01 has no timescale: refused rather than dropped")
    func legacyTimescale() throws {
        let m = LOCMetadata(timestamp: 1, timescale: 1000)
        #expect(throws: MoQError.unsupported) {
            try LOCMetadataCodec.encode(wireVersion: .draft18, metadata: m)
        }
        _ = try LOCMetadataCodec.encode(wireVersion: .draft22, metadata: m)
    }
}
