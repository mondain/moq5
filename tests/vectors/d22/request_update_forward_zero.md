# `request_update_forward_zero.bin` - REQUEST_UPDATE (§9.5 / §9.20.18)

REQUEST_UPDATE for Request ID 4 with FORWARD (0x10) = 0 (pause)

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 7 bytes.

Wire: `02 00 04 04 01 10 00`

Envelope (§9 Fig. 5): Type `02` = 0x2, Length `00 04` = 4, Body 4 bytes `04 01 10 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `02` | Message Type | 0x2 (REQUEST_UPDATE) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 04` | Message Length | 4 (0x0004) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `04` | Request ID | 4 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.5 Fig. 10; §6.4.2.1 |
| 4 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.5 Fig. 10 |
| 5 | `10` | Parameter Type Delta | 0x10 -> Type 0x10 FORWARD (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 16 = 0x10 | §9.20 Fig. 24 |
| 6 | `00` | FORWARD Value | 0 (don't forward: subscription paused) | uint8 (single byte, NOT a vi64): 0x00 | §9.20.18 ("is a uint8") |

## Reading notes

- Fig. 10: `REQUEST_UPDATE Message { Type (vi64) = 0x2, Length (16), Request ID (vi64), Number of Parameters (vi64), Parameters (..) ... }`. Unlike responses, REQUEST_UPDATE is a request: §6.4.2.1 lists it among the messages that consume a Request ID.
- The Request ID in REQUEST_UPDATE is the update's own new ID, not the ID of the request being updated; the target request is identified by the stream the update is sent on (§9.5: "on the same bidi stream as the request"). §3.4 uses this ID for fill fetch streams opened by the update.
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
