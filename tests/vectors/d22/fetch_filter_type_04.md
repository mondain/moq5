# `fetch_filter_type_04.bin` - FETCH (§9.11 / §9.20.9)

FETCH Request ID 6, namespace ["live","a"], track "video", LOCATION_FILTER type 0x04 {StartGroup 3, StartObject 4, EndGroupDelta 2, EndObject 9}

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 25 bytes.

Wire: `16 00 16 06 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 01 21 04 03 04 02 09`

Envelope (§9 Fig. 5): Type `16` = 0x16, Length `00 16` = 22, Body 22 bytes `06 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 01 21 04 03 04 02 09`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `16` | Message Type | 0x16 (FETCH) | vi64, 1-byte form (0b0xxxxxxx): 22 = 0x16 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 16` | Message Length | 22 (0x0016) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `06` | Request ID | 6 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 6 = 0x06 | §9.11 Fig. 16; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.11 Fig. 16 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.11 Fig. 16 |
| 18 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.11 Fig. 16 |
| 19 | `21` | Parameter Type Delta | 0x21 -> Type 0x21 LOCATION_FILTER (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 33 = 0x21 | §9.20 Fig. 24 |
| 20 | `04` | Location Filter Type | 0x04 (Absolute Range): StartGroup, StartObject, EndGroupDelta, and EndObject follow | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.20.9 Table 6 |
| 21 | `03` | LOCATION_FILTER StartGroup | 3 (absolute) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9.20.9 |
| 22 | `04` | LOCATION_FILTER StartObject | 4 (absolute) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.20.9 |
| 23 | `02` | LOCATION_FILTER EndGroupDelta | 2 (End Group = 3 + 2 = 5) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.20.9 |
| 24 | `09` | LOCATION_FILTER EndObject | 9 (End Location = {5, 9}, inclusive) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20.9 |

## Reading notes

- Fig. 16: `FETCH Message { Type (vi64) = 0x16, Length (16), Request ID (vi64), Track Namespace (..), Track Name Length (vi64), Track Name (..), Number of Parameters (vi64), Parameters (..) ... }`. Changelog A.3: "carry the FETCH range in LOCATION_FILTER instead of message fields (#1809)" and "Remove the Joining variant of FETCH".
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- §9.20.9: `LOCATION_FILTER Parameter { Parameter Type (vi64) = 0x21, Location Filter Type (vi64), [StartGroup (vi64),] [StartObject (vi64),] [EndGroupDelta (vi64),] [EndObject (vi64),] }` - "The Location Filter Type dictates which optional variable-length integer fields follow". The figure labels the first field "Parameter Type"; in a message it is the §9.20 Type Delta (here 0x01, since it follows SUBSCRIBER_PRIORITY 0x20). There is no length prefix on this parameter. Changelog A.1: the explicit type replaced "the length-inferred encoding (#1913, #1953)".
- Range requested: Start {3, 4}, End {3 + 2, 9} = {5, 9} inclusive (§9.20.9 Table 6, 0x04 Absolute Range). The matching FETCH_OK is `fetch_ok_end_location.bin` (End Location {5, 9}).
- The first (only) parameter's delta equals its type, 0x21.
