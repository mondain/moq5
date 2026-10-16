import MoQ

public func legacyResetPattern(_ action: Action) -> (StreamRef, UInt64)? {
    if case .resetData(let stream, let code) = action {
        return (stream, code)
    }
    return nil
}
