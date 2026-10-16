# `request_ok_empty.bin` - REQUEST_OK (§9.3)

REQUEST_OK with no Parameters and empty Track Properties

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 4 bytes.

Wire: `07 00 01 00`

Envelope (§9 Fig. 5): Type `07` = 0x7, Length `00 01` = 1, Body 1 bytes `00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `07` | Message Type | 0x7 (REQUEST_OK) | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 01` | Message Length | 1 (0x0001) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `00` | Number of Parameters | 0 | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.3 Fig. 8 |
| 4 | `(none)` | Track Properties | (empty: zero remaining bytes) | raw bytes | §9.3 Fig. 8; §8.4 |

## Reading notes

- Fig. 8: `REQUEST_OK Message { Type (vi64) = 0x7, Length (16), Number of Parameters (vi64), Parameters (..) ..., Track Properties (..), }`.
- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- The brief asked for a REQUEST_OK "for Request ID 4": in draft-22 that identity is carried only by the request stream, so this body is what a REQUEST_OK answering Request ID 4 (or any other) looks like.
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
- This shape is a valid PUBLISH_OK, REQUEST_UPDATE_OK, SUBSCRIBE_NAMESPACE_OK, SUBSCRIBE_TRACKS_OK or PUBLISH_NAMESPACE_OK (§1.5 shorthand names). For those, Track Properties MUST be empty (§9.3: "If an endpoint receives Track Properties in one of these messages it MUST close the session with a PROTOCOL_VIOLATION").
