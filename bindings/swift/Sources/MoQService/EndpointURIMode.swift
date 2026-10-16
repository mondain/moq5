import MoQServiceCore

/// The C header's UINT32_C URI macros are not imported by Swift. Keep this
/// internal mapping checked against the actual C constants and SDK resolver.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
func cURIMode(_ mode: MoQEndpoint.URIMode) -> UInt32 {
    switch mode {
    case .standard: 1
    case .legacy: 0
    }
}
