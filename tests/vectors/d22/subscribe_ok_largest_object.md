# `subscribe_ok_largest_object.bin` - SUBSCRIBE_OK (§9.7 / §9.20.17)

SUBSCRIBE_OK Track Alias 7 with LARGEST_OBJECT (0x09) = {2, 5}, empty Track Properties

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 8 bytes.

Wire: `04 00 05 07 01 09 02 05`

Envelope (§9 Fig. 5): Type `04` = 0x4, Length `00 05` = 5, Body 5 bytes `07 01 09 02 05`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `04` | Message Type | 0x4 (SUBSCRIBE_OK) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 05` | Message Length | 5 (0x0005) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9.7 Fig. 12; §3.1.3 |
| 4 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.7 Fig. 12 |
| 5 | `09` | Parameter Type Delta | 0x09 -> Type 0x09 LARGEST_OBJECT (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20 Fig. 24 |
| 6 | `02` | LARGEST_OBJECT Group | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.20.17 ("is a Location"); §8.2 |
| 7 | `05` | LARGEST_OBJECT Object | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.20.17; §8.2 |
| 8 | `(none)` | Track Properties | (empty) | raw bytes | §8.4 |

## Reading notes

- Fig. 12: `SUBSCRIBE_OK Message { Type (vi64) = 0x4, Length (16), Track Alias (vi64), Number of Parameters (vi64), Parameters (..) ..., Track Properties (..), }`.
- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- LARGEST_OBJECT has Parameter Type 0x9 (odd). Because Message Parameters are not §8.3 KVPs there is no Length byte: the value is the Location `Group (vi64), Object (vi64)` (§9.20: "Location: Two consecutive varints (Group, Object)"). A decoder applying the KVP odd-type rule would read `02` as a length and `05` as the value.
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
