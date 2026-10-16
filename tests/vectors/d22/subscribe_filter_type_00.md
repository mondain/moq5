# `subscribe_filter_type_00.bin` - SUBSCRIBE (§9.6 / §9.20.9)

SUBSCRIBE Request ID 4 with FORWARD=1, SUBSCRIBER_PRIORITY=128, LOCATION_FILTER type 0x00 (None)

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 25 bytes.

Wire: `03 00 16 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 03 10 01 10 80 01 00`

Envelope (§9 Fig. 5): Type `03` = 0x3, Length `00 16` = 22, Body 22 bytes `04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 03 10 01 10 80 01 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `03` | Message Type | 0x3 (SUBSCRIBE) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 16` | Message Length | 22 (0x0016) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `04` | Request ID | 4 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.6 Fig. 11; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.6 Fig. 11 / §2.4.1 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.6 Fig. 11 / §2.4.1 |
| 18 | `03` | Number of Parameters | 3 | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9.6 Fig. 11 |
| 19 | `10` | Parameter Type Delta | 0x10 -> Type 0x10 FORWARD (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 16 = 0x10 | §9.20 Fig. 24 |
| 20 | `01` | FORWARD Value | 1 (forward) | uint8 (single byte, NOT a vi64): 0x01 | §9.20.18 ("is a uint8") |
| 21 | `10` | Parameter Type Delta | 0x10 -> Type 0x20 SUBSCRIBER_PRIORITY (0x20 - 0x10 = 0x10) | vi64, 1-byte form (0b0xxxxxxx): 16 = 0x10 | §9.20 Fig. 24 |
| 22 | `80` | SUBSCRIBER_PRIORITY Value | 128 | uint8 (single byte, NOT a vi64): 0x80 | §9.20.7 ("is a uint8") |
| 23 | `01` | Parameter Type Delta | 0x01 -> Type 0x21 LOCATION_FILTER (0x21 - 0x20 = 0x01) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.20 Fig. 24 |
| 24 | `00` | Location Filter Type | 0x00 (None): no fields follow; "there is no Location Filter" | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.20.9 Table 6 |

## Reading notes

- Track Namespace is §8.7: a vi64 field count, then each field as vi64 length + bytes. Track Name is a vi64 length + bytes (§9.6 Fig. 11). No field in draft-22 SUBSCRIBE carries Subscriber Priority, Group Order, Forward or a Filter Type as a fixed field; all of those are Message Parameters (§9.6: "The parameters that can appear in a SUBSCRIBE are ...").
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Type Delta arithmetic (§9.20: "The difference between this Parameter Type and the previous Parameter Type in the message, or the Parameter Type itself for the first parameter. Parameters MUST be serialized in ascending order by Type."): FORWARD 0x10 is first so its delta is 0x10; SUBSCRIBER_PRIORITY 0x20 follows with delta 0x20 - 0x10 = 0x10; LOCATION_FILTER 0x21 follows with delta 0x21 - 0x20 = 0x01. A decoder reading absolute types would see 0x10, 0x10, 0x01 and fail (duplicate FORWARD, then an unknown type 0x01).
- SUBSCRIBER_PRIORITY is a uint8 (§9.20.7), so 128 is the single byte `80`. Encoded as a vi64 it would be `80 80`; this vector distinguishes the two.
- §9.20.9: `LOCATION_FILTER Parameter { Parameter Type (vi64) = 0x21, Location Filter Type (vi64), [StartGroup (vi64),] [StartObject (vi64),] [EndGroupDelta (vi64),] [EndObject (vi64),] }` - "The Location Filter Type dictates which optional variable-length integer fields follow". The figure labels the first field "Parameter Type"; in a message it is the §9.20 Type Delta (here 0x01, since it follows SUBSCRIBER_PRIORITY 0x20). There is no length prefix on this parameter. Changelog A.1: the explicit type replaced "the length-inferred encoding (#1913, #1953)".
- Type 0x00 is the explicit "no filter" form. §3.4 still speaks of a "zero-length LOCATION_FILTER"; under §9.20.9 there is no zero-length form - the parameter always carries a Location Filter Type byte. This vector follows §9.20.9 (see README Open questions).
