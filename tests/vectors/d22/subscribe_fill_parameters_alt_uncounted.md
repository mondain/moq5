# `subscribe_fill_parameters_alt_uncounted.bin` - SUBSCRIBE (§9.6 / §9.20.15)

SUBSCRIBE Request ID 4 with LOCATION_FILTER (Next Object) and FILL_PARAMETERS containing FILL_TIMEOUT=250 and LOCATION_FILTER type 0x01 StartGroup=1 - REFUSED alternate reading: nested value without a parameter count

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 29 bytes.

Wire: `03 00 1A 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 06 0A 80 FA 17 01 01`

Envelope (§9 Fig. 5): Type `03` = 0x3, Length `00 1A` = 26, Body 26 bytes `04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 06 0A 80 FA 17 01 01`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `03` | Message Type | 0x3 (SUBSCRIBE) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 1A` | Message Length | 26 (0x001A) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
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
| 22 | `06` | FILL_PARAMETERS Length | 6 (byte count of the nested value) | vi64, 1-byte form (0b0xxxxxxx): 6 = 0x06 | §9.20.15 ("uses length-prefixed encoding") |
| 23 | `0A` | FILL_PARAMETERS value [rejected interpretation: [inner] Parameter Type Delta] | counted reading: Number of Parameters 10 (vi64 `0A`) [rejected interpretation: 0x0A -> Type 0x0A FILL_TIMEOUT (first parameter: delta = the type itself)] | vi64, 1-byte form (0b0xxxxxxx): 10 = 0x0A | §9.20.15; §9.20 [rejected interpretation: §9.20.15 Table 7; §9.20 Fig. 24 (inner chain restarts from 0)] |
| 24 | `80 FA` | FILL_PARAMETERS value [rejected interpretation: [inner] FILL_TIMEOUT Value] | counted reading: first nested Parameter Type Delta 250 (vi64 `80 FA`) -> Type 0xFA, outside the fill's scope (§9.20.15 Table 7): PROTOCOL_VIOLATION, decoding stops [rejected interpretation: 250 ms] | vi64, 2-byte form (first byte starts 0b10): 250 = 0xFA; first byte = 0x80 \| 0x00 | §9.20; §9.20.15 Table 7 [rejected interpretation: §9.20.5 (encoding unstated; read as varint)] |
| 26 | `17` | FILL_PARAMETERS value [rejected interpretation: [inner] Parameter Type Delta] | counted reading: not reached: the counted decoder has already refused the value [rejected interpretation: 0x17 -> Type 0x21 LOCATION_FILTER (0x21 - 0x0A = 0x17)] | vi64, 1-byte form (0b0xxxxxxx): 23 = 0x17 | - [rejected interpretation: §9.20.15 Table 7; §9.20 Fig. 24] |
| 27 | `01` | FILL_PARAMETERS value [rejected interpretation: [inner] Location Filter Type] | counted reading: not reached: the counted decoder has already refused the value [rejected interpretation: 0x01 (Relative Start): a relative StartGroup follows] | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | - [rejected interpretation: §9.20.9 Table 6] |
| 28 | `01` | FILL_PARAMETERS value [rejected interpretation: [inner] LOCATION_FILTER StartGroup] | counted reading: not reached: the counted decoder has already refused the value [rejected interpretation: 1 (relative: start at the current group, {Largest Object.Group, 0})] | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | - [rejected interpretation: §9.20.9] |

## Reading notes

- Track Namespace is §8.7: a vi64 field count, then each field as vi64 length + bytes. Track Name is a vi64 length + bytes (§9.6 Fig. 11). No field in draft-22 SUBSCRIBE carries Subscriber Priority, Group Order, Forward or a Filter Type as a fixed field; all of those are Message Parameters (§9.6: "The parameters that can appear in a SUBSCRIBE are ...").
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Outer parameters: LOCATION_FILTER 0x21 (type 0x05 Next Object) then FILL_PARAMETERS 0x23 with delta 0x23 - 0x21 = 0x02. This is the §3.4 exactly-once pattern: "the Next Object Subscription Location Filter coupled with an open-ended fill range, which the publisher will end at Largest Object".
- §9.20.15: "The FILL_PARAMETERS parameter (Parameter Type 0x23) uses length-prefixed encoding. ... Its value is a sequence of Parameters that apply to the fill fetch stream (see Section 3.4), encoded as if they were Parameters for a separate message (see Section 16.7)." and "The value of FILL_PARAMETERS is a separate parameter scope. Parameters inside it are not considered to appear in the enclosing message for the purposes of Section 9.20, so a Parameter Type MAY appear both in the message and inside FILL_PARAMETERS."
- REFUSED alternate reading: the value is taken as the bare `Parameters (..) ...` sequence bounded only by the Length, with no `Number of Parameters`. The draft-22 text alone admits this reading; the selected interpretation is the counted one, matching the `Number of Parameters` every message's parameter block carries (§9.20) and supported by the moq-wg/moq-transport#1991 clarification. A receiver taking the counted reading reads the first byte `0A` as Number of Parameters 10 and then `80 FA` as a Type Delta of 250, which is not a parameter allowed inside FILL_PARAMETERS: PROTOCOL_VIOLATION. The primary form is `subscribe_fill_parameters.bin`.
- Inner ordering/deltas: FILL_TIMEOUT 0x0A < LOCATION_FILTER 0x21, so FILL_TIMEOUT comes first with delta 0x0A (the type itself - the inner chain does not continue from the outer 0x23), then LOCATION_FILTER with delta 0x21 - 0x0A = 0x17. 0x21 appears both outside and inside, which §9.20.15 explicitly allows (separate scope). The inner LOCATION_FILTER uses type 0x01 (Relative Start) StartGroup=1 and "is evaluated using the rules for a Fetch" (§9.20.15), so its end is Largest Object (§9.20.9: "When they are omitted from a Fetch, the EndGroup and EndObject are Largest Object").
- FILL_TIMEOUT (0x0A) value encoding: §9.20.5 states no encoding (unlike §9.20.3/§9.20.4/§9.20.16 which say "is a varint"). It is a millisecond duration, which cannot fit the uint8 encoding in general, so it is read as a varint like every other millisecond-duration parameter. 250 as a varint is the 2-byte form `80 FA`; as a uint8 it would be `FA`. Recorded in README Open questions.
