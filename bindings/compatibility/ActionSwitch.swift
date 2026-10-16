import MoQ

public func legacyActionSwitch(_ action: Action) -> Int {
    switch action {
    case .sendControl: return 0
    case .closeSession: return 1
    case .sendData: return 2
    case .resetData: return 3
    case .stopData: return 4
    case .openBidiStream: return 5
    case .sendBidiStream: return 6
    case .closeBidiStream: return 7
    case .sendDatagram: return 8
    }
}
