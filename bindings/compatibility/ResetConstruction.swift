import MoQ

public func legacyResetConstruction(_ stream: StreamRef) -> Action {
    .resetData(stream: stream, errorCode: 7)
}
