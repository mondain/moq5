# `request_ok_expires.bin` - REQUEST_OK (§9.3 / §9.20.16)

REQUEST_OK with EXPIRES (0x08) = 30000 ms

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 8 bytes.

Wire: `07 00 05 01 08 C0 75 30`

Envelope (§9 Fig. 5): Type `07` = 0x7, Length `00 05` = 5, Body 5 bytes `01 08 C0 75 30`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `07` | Message Type | 0x7 (REQUEST_OK) | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 05` | Message Length | 5 (0x0005) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.3 Fig. 8 |
| 4 | `08` | Parameter Type Delta | 0x08 -> Type 0x08 EXPIRES (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 8 = 0x08 | §9.20 Fig. 24 |
| 5 | `C0 75 30` | EXPIRES Value | 30000 ms | vi64, 3-byte form (first byte starts 0b110): 30000 = 0x7530; first byte = 0xC0 \| 0x00 | §9.20.16 ("is a varint") |
| 8 | `(none)` | Track Properties | (empty) | raw bytes | §8.4 |

## Reading notes

- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- 30000 = 0x7530 exceeds the 2-byte ceiling (16383), so the value takes the 3-byte vi64 form (prefix 0b110): 0xC0 | 0x00, 0x75, 0x30 -> `C0 75 30`.
- EXPIRES may appear in PUBLISH_OK, SUBSCRIBE_NAMESPACE_OK, SUBSCRIBE_TRACKS_OK, PUBLISH_NAMESPACE_OK and REQUEST_UPDATE_OK (§9.3 list; §9.20.16).
