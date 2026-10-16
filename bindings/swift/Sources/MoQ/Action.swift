import CMoQCore
import Foundation

/// Opaque stream identifier used by transport adapters.
public struct StreamRef: Sendable, Hashable {
    public let rawValue: UInt64

    internal init(_ ref: moq_stream_ref_t) {
        self.rawValue = ref._v
    }

    /// A ref the caller's transport chose for an inbound stream.
    public init(rawValue: UInt64) {
        self.rawValue = rawValue
    }
}

/// Outbound transport action produced by a session.
// Not Sendable: `sendData` carries `Buffer` (non-atomic, shard-confined rcbuf).
public enum Action {
    case sendControl(Data)
    case closeSession(code: UInt64, reason: String)
    case sendData(stream: StreamRef, header: Data, payload: Buffer?, fin: Bool)
    case resetData(stream: StreamRef, errorCode: UInt64)
    case stopData(stream: StreamRef, errorCode: UInt64)
    case openBidiStream(stream: StreamRef, data: Data, fin: Bool)
    case sendBidiStream(stream: StreamRef, data: Data, fin: Bool)
    case closeBidiStream(stream: StreamRef)
    case sendDatagram(Data)
}

/// Complete transport output, including reliable resets and request half-close
/// operations. Transport drivers using these operations must poll this view.
public enum ExtendedAction {
    case sendControl(Data)
    case closeSession(code: UInt64, reason: String)
    case sendData(stream: StreamRef, header: Data, payload: Buffer?, fin: Bool)
    /// Reset a data stream. `reliableSize` is how many leading bytes (the
    /// stream header) the reset must still deliver: a transport that
    /// negotiated RESET_STREAM_AT sends it with that size. Otherwise the
    /// driver waits for that prefix to be acknowledged before a plain reset,
    /// when its transport can provide that guarantee. Zero needs no wait.
    case resetData(stream: StreamRef, errorCode: UInt64, reliableSize: UInt64)
    case stopData(stream: StreamRef, errorCode: UInt64)
    case openBidiStream(stream: StreamRef, data: Data, fin: Bool)
    case sendBidiStream(stream: StreamRef, data: Data, fin: Bool)
    case closeBidiStream(stream: StreamRef)
    case sendDatagram(Data)
    /// Control bytes on a unidirectional control stream this side opens
    /// (drafts 18 and 22); the peer feeds them to `receiveControlBytes`.
    case openUniControl(stream: StreamRef, data: Data)
    case sendUniControl(stream: StreamRef, data: Data, fin: Bool)
    /// Request-stream teardown: reset the local send half, ask the peer to
    /// stop sending, or abort both halves with one code.
    case resetBidiStream(stream: StreamRef, errorCode: UInt64)
    case stopBidiStream(stream: StreamRef, errorCode: UInt64)
    case abortBidiStream(stream: StreamRef, errorCode: UInt64)
    case unknown(UInt32)
}

extension Action {
    internal static func from(_ cAction: inout moq_action_t) -> Action? {
        switch ExtendedAction.from(&cAction) {
        case .sendControl(let data): return .sendControl(data)
        case .closeSession(let code, let reason): return .closeSession(code: code, reason: reason)
        case .sendData(let stream, let header, let payload, let fin):
            return .sendData(stream: stream, header: header, payload: payload, fin: fin)
        case .resetData(let stream, let code, _): return .resetData(stream: stream, errorCode: code)
        case .stopData(let stream, let code): return .stopData(stream: stream, errorCode: code)
        case .openBidiStream(let stream, let data, let fin):
            return .openBidiStream(stream: stream, data: data, fin: fin)
        case .sendBidiStream(let stream, let data, let fin):
            return .sendBidiStream(stream: stream, data: data, fin: fin)
        case .closeBidiStream(let stream): return .closeBidiStream(stream: stream)
        case .sendDatagram(let data): return .sendDatagram(data)
        case .openUniControl(_, let data), .sendUniControl(_, let data, _):
            return .sendControl(data)
        case .resetBidiStream, .stopBidiStream, .abortBidiStream:
            preconditionFailure("Request half-close actions require pollActionsExtended()")
        case .unknown: return nil
        }
    }
}

extension ExtendedAction {
    /// Convert a C action to Swift, taking ownership of any owned refs.
    /// The caller must have already polled this action from the session.
    /// This function calls moq_action_cleanup on the C action.
    internal static func from(_ cAction: inout moq_action_t) -> ExtendedAction {
        let kind = cAction.kind
        let result: ExtendedAction

        switch kind {
        case MOQ_ACTION_SEND_CONTROL:
            let sc = cAction.u.send_control
            let data = Data(
                bytes: sc.data, count: sc.len
            )
            result = .sendControl(data)

        case MOQ_ACTION_CLOSE_SESSION:
            let cs = cAction.u.close_session
            let reason: String
            if cs.reason.data != nil && cs.reason.len > 0 {
                reason = String(
                    bytes: UnsafeBufferPointer(
                        start: cs.reason.data, count: cs.reason.len),
                    encoding: .utf8
                ) ?? ""
            } else {
                reason = ""
            }
            result = .closeSession(code: cs.code, reason: reason)

        case MOQ_ACTION_SEND_DATA:
            let sd = cAction.u.send_data
            let headerLen = Int(sd.header_len)
            let header: Data = withUnsafeBytes(of: sd.header) { buf in
                Data(buf.prefix(headerLen))
            }
            let payload: Buffer?
            if let p = sd.payload {
                // Take ownership of the transferred ref. Null it
                // out so moq_action_cleanup doesn't decref it.
                cAction.u.send_data.payload = nil
                payload = Buffer(adopting: p)
            } else {
                payload = nil
            }
            result = .sendData(
                stream: StreamRef(sd.stream_ref),
                header: header,
                payload: payload,
                fin: sd.fin
            )

        case MOQ_ACTION_RESET_DATA:
            let rd = cAction.u.reset_data
            // Read only when the producer's detail covers the field.
            var reliable: UInt64 = 0
            if let off = MemoryLayout<moq_reset_data_action_t>.offset(
                   of: \moq_reset_data_action_t.reliable_size),
               Int(cAction.detail_size) >= off + MemoryLayout<UInt64>.size {
                reliable = rd.reliable_size
            }
            result = .resetData(
                stream: StreamRef(rd.stream_ref),
                errorCode: rd.error_code,
                reliableSize: reliable
            )

        case MOQ_ACTION_STOP_DATA:
            let sd = cAction.u.stop_data
            result = .stopData(
                stream: StreamRef(sd.stream_ref),
                errorCode: sd.error_code
            )

        case MOQ_ACTION_OPEN_BIDI_STREAM:
            let ob = cAction.u.open_bidi_stream
            let data = Data(
                bytes: ob.data, count: ob.len
            )
            result = .openBidiStream(
                stream: StreamRef(ob.stream_ref), data: data, fin: ob.fin)

        case MOQ_ACTION_SEND_BIDI_STREAM:
            let sb = cAction.u.send_bidi_stream
            let data = Data(
                bytes: sb.data, count: sb.len
            )
            result = .sendBidiStream(
                stream: StreamRef(sb.stream_ref), data: data, fin: sb.fin)

        case MOQ_ACTION_CLOSE_BIDI_STREAM:
            let cb = cAction.u.close_bidi_stream
            result = .closeBidiStream(stream: StreamRef(cb.stream_ref))

        case MOQ_ACTION_SEND_DATAGRAM:
            let dg = cAction.u.send_datagram
            let data = Data(
                bytes: dg.data, count: dg.len
            )
            result = .sendDatagram(data)

        case MOQ_ACTION_OPEN_UNI_CONTROL:
            let uc = cAction.u.open_uni_control
            result = .openUniControl(
                stream: StreamRef(uc.stream_ref),
                data: Data(bytes: uc.data, count: uc.len))

        case MOQ_ACTION_SEND_UNI_CONTROL:
            let uc = cAction.u.send_uni_control
            result = .sendUniControl(
                stream: StreamRef(uc.stream_ref),
                data: Data(bytes: uc.data, count: uc.len), fin: uc.fin)

        case MOQ_ACTION_RESET_BIDI_STREAM:
            let rb = cAction.u.reset_bidi_stream
            result = .resetBidiStream(
                stream: StreamRef(rb.stream_ref), errorCode: rb.error_code)

        case MOQ_ACTION_STOP_BIDI_STREAM:
            let sb = cAction.u.stop_bidi_stream
            result = .stopBidiStream(
                stream: StreamRef(sb.stream_ref), errorCode: sb.error_code)

        case MOQ_ACTION_ABORT_BIDI_STREAM:
            let ab = cAction.u.abort_bidi_stream
            result = .abortBidiStream(
                stream: StreamRef(ab.stream_ref), errorCode: ab.error_code)

        default:
            result = .unknown(UInt32(kind))
        }

        moq_action_cleanup(&cAction)
        return result
    }
}
