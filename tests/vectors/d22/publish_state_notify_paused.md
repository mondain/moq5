# `publish_state_notify_paused.bin` - PUBLISH_STATE_NOTIFY (§9.10 / §9.20.18)

PUBLISH_STATE_NOTIFY with LARGEST_OBJECT {10, 20} and FORWARD 0 (subscription paused at the publisher)

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 9 bytes.

Wire: `22 00 06 02 09 0A 14 07 00`

Envelope (§9 Fig. 5): Type `22` = 0x22, Length `00 06` = 6, Body 6 bytes `02 09 0A 14 07 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `22` | Message Type | 0x22 (PUBLISH_STATE_NOTIFY) | vi64, 1-byte form (0b0xxxxxxx): 34 = 0x22 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 06` | Message Length | 6 (0x0006) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `02` | Number of Parameters | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.10 Fig. 15 |
| 4 | `09` | Parameter Type Delta | 0x09 -> Type 0x09 LARGEST_OBJECT (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20 Fig. 24 |
| 5 | `0A` | LARGEST_OBJECT Group | 10 | vi64, 1-byte form (0b0xxxxxxx): 10 = 0x0A | §9.20.17; §8.2 |
| 6 | `14` | LARGEST_OBJECT Object | 20 | vi64, 1-byte form (0b0xxxxxxx): 20 = 0x14 | §9.20.17; §8.2 |
| 7 | `07` | Parameter Type Delta | 0x07 -> Type 0x10 FORWARD (0x10 - 0x09 = 0x07) | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9.20 Fig. 24 |
| 8 | `00` | FORWARD Value | 0 ("reports whether the subscription is paused at the publisher") | uint8 (single byte, NOT a vi64): 0x00 | §9.20.18 |

## Reading notes

- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Allowed parameters in PUBLISH_STATE_NOTIFY are LARGEST_OBJECT, FORWARD and LOCATION_FILTER (§9.10).
