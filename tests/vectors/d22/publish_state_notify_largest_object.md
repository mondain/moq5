# `publish_state_notify_largest_object.bin` - PUBLISH_STATE_NOTIFY (§9.10 / §9.20.17)

PUBLISH_STATE_NOTIFY with LARGEST_OBJECT (0x09) = {10, 20}

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 7 bytes.

Wire: `22 00 04 01 09 0A 14`

Envelope (§9 Fig. 5): Type `22` = 0x22, Length `00 04` = 4, Body 4 bytes `01 09 0A 14`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `22` | Message Type | 0x22 (PUBLISH_STATE_NOTIFY) | vi64, 1-byte form (0b0xxxxxxx): 34 = 0x22 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 04` | Message Length | 4 (0x0004) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.10 Fig. 15 |
| 4 | `09` | Parameter Type Delta | 0x09 -> Type 0x09 LARGEST_OBJECT (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20 Fig. 24 |
| 5 | `0A` | LARGEST_OBJECT Group | 10 | vi64, 1-byte form (0b0xxxxxxx): 10 = 0x0A | §9.20.17; §8.2 |
| 6 | `14` | LARGEST_OBJECT Object | 20 | vi64, 1-byte form (0b0xxxxxxx): 20 = 0x14 | §9.20.17; §8.2 |

## Reading notes

- Fig. 15: `PUBLISH_STATE_NOTIFY Message { Type (vi64) = 0x22, Length (16), Number of Parameters (vi64), Parameters (..) ... }` - no Request ID and, unlike SUBSCRIBE_OK/PUBLISH, no trailing Track Properties field. Any byte after the last parameter is a Length mismatch (§9).
- Message Type 0x22 and Parameter Type 0x22 (GROUP_ORDER) share a code point in different spaces; the message type is the first vi64 on the stream.
- §9.10: "The publisher MUST include the LARGEST_OBJECT parameter, if known, in PUBLISH_STATE_NOTIFY".
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
