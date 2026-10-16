import MoQService

// Intentionally exhaustive: the six legacy cases must remain unchanged.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
func legacyEvent(_ event: TrackEvent) -> Int {
    switch event {
    case .added: 1
    case .updated: 2
    case .removed: 3
    case .ended: 4
    case .catalogReady: 5
    case .parseDrop: 6
    }
}

@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
func legacySubscribe(_ receiver: MediaReceiver, _ track: MediaTrack) async throws {
    let command: (MediaTrack, StartMode, UInt8?) async throws -> Void = receiver.subscribe
    try await receiver.subscribe(track)
    try await receiver.subscribe(track, start: .nextGroup)
    try await receiver.subscribe(track, priority: 0)
    try await command(track, .current, nil)
}
