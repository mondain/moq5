import Foundation
import Testing
@testable import MoQ
@testable import MoQMedia

@Suite("Explicit media wire version")
struct WireVersionParserTests {
    @Test func unsupportedTokensAreNotReinterpretedAsLegacy() throws {
        let track = MediaTrackInfo(mediaType: .video, packaging: .raw,
                                   transportVersion: .draft16)
        let object = FacadeReceivedObject(
            track: SubscribedTrack(OpaquePointer(bitPattern: 1)!),
            groupID: 0, subgroupID: 0, objectID: 0,
            publisherPriority: 128, status: .normal,
            endOfGroup: false, isDatagram: false,
            payload: try Buffer(Data([1])), properties: nil)
        for token in [WireVersion.draft22, WireVersion(rawValue: 999)] {
            #expect(throws: MediaParseError.invalidArgument) {
                try MediaObjectParser.parse(track: track, object: object, wireVersion: token)
            }
        }
    }
}
