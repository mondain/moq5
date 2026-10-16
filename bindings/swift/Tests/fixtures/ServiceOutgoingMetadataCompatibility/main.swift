import Foundation
import MoQServiceCore

// Frozen old-client function type: the new overload must not replace it.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
func oldOutgoingClient() -> OutgoingObject {
    let make: (Data, Bool, Bool, Bool, Duration, Duration?, Date?, SAPType?, Data?)
        -> OutgoingObject = OutgoingObject.init(
            payload:isSync:startsGroup:endsGroup:presentationTime:
            decodeTime:captureTime:sapType:properties:)
    return make(Data([1]), true, true, false, .zero, nil, nil, .type1, nil)
}
