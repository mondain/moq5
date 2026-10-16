# `subscribe_fill_parameters_empty.bin` - SUBSCRIBE (§9.6 / §9.20.15)

SUBSCRIBE Request ID 4 with LOCATION_FILTER (Next Object) and FILL_PARAMETERS with no settings of its own: Number of Parameters 0

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 24 bytes.

Wire: `03 00 15 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 01 00`

Envelope (§9 Fig. 5): Type `03` = 0x3, Length `00 15` = 21, Body 21 bytes `04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 01 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `03` | Message Type | 0x3 (SUBSCRIBE) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 15` | Message Length | 21 (0x0015) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `04` | Request ID | 4 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.6 Fig. 11; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.6 Fig. 11 / §2.4.1 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.6 Fig. 11 / §2.4.1 |
| 18 | `02` | Number of Parameters | 2 (outer scope) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.6 Fig. 11 |
| 19 | `21` | Parameter Type Delta | 0x21 -> Type 0x21 LOCATION_FILTER (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 33 = 0x21 | §9.20 Fig. 24 |
| 20 | `05` | Location Filter Type | 0x05 (Next Object): no fields follow | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.20.9 Table 6 |
| 21 | `02` | Parameter Type Delta | 0x02 -> Type 0x23 FILL_PARAMETERS (0x23 - 0x21 = 0x02) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.20 Fig. 24 |
| 22 | `01` | FILL_PARAMETERS Length | 1 (byte count of the nested value) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.20.15 ("uses length-prefixed encoding") |
| 23 | `00` |   [inner] Number of Parameters | 0 (nested scope) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.20.15 ("as if they were Parameters for a separate message"); §9.20 ("bounded by a parameter count"); #1991 |

## Reading notes

- Track Namespace is §8.7: a vi64 field count, then each field as vi64 length + bytes. Track Name is a vi64 length + bytes (§9.6 Fig. 11). No field in draft-22 SUBSCRIBE carries Subscriber Priority, Group Order, Forward or a Filter Type as a fixed field; all of those are Message Parameters (§9.6: "The parameters that can appear in a SUBSCRIBE are ...").
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Outer parameters: LOCATION_FILTER 0x21 (type 0x05 Next Object) then FILL_PARAMETERS 0x23 with delta 0x23 - 0x21 = 0x02. This is the §3.4 exactly-once pattern: "the Next Object Subscription Location Filter coupled with an open-ended fill range, which the publisher will end at Largest Object".
- §9.20.15: "The FILL_PARAMETERS parameter (Parameter Type 0x23) uses length-prefixed encoding. ... Its value is a sequence of Parameters that apply to the fill fetch stream (see Section 3.4), encoded as if they were Parameters for a separate message (see Section 16.7)." and "The value of FILL_PARAMETERS is a separate parameter scope. Parameters inside it are not considered to appear in the enclosing message for the purposes of Section 9.20, so a Parameter Type MAY appear both in the message and inside FILL_PARAMETERS."
- Nested count: every message encodes its Parameters as `Number of Parameters (vi64)` followed by `Parameters (..) ...` (§9.6 Fig. 11 and every other request figure), and §9.20 bounds a parameter block by that count. "Encoded as if they were Parameters for a separate message" therefore puts a `Number of Parameters` at the start of the value, inside the draft-22 Length, and restarts the Type Delta chain. The count's parameters must end exactly at the Length. The text alone admits a reading without the count; moq-wg/moq-transport#1991 records that ambiguity. The counted reading is the selected draft-22 interpretation, supported by the 2026-10-15 clarification on that issue (https://github.com/moq-wg/moq-transport/issues/1991#issuecomment-6199709619: "it can be just { Num Parameter, Parameters }") and by #1868, which made the value Parameters rather than Key-Value-Pairs; it is not published replacement text. The same clarification proposes dropping the outer Length in a later draft; draft-22 keeps it.
- A fill that overrides nothing still carries its count: the value is the single byte `00` (Number of Parameters 0), so the parameter is `23 01 00` in absolute-type terms (here delta `02`, Length `01`, count `00`). An empty value (Length 0) has no count and is malformed (PROTOCOL_VIOLATION). Every setting the fill omits is taken from the subscription (§9.20.15).
