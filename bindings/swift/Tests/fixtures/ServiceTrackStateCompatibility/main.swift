import MoQServiceCore

// Frozen old-state client; no default or new state case may be required.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
func oldTrackState(_ state: MediaTrack.SubscriptionState) -> Int {
    switch state {
    case .discovered: 0
    case .pending: 1
    case .active: 2
    case .pausedByApp: 3
    case .pausedByFlowControl: 4
    case .ended: 5
    }
}
