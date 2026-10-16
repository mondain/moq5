# `subscribe_minimal.bin` - SUBSCRIBE (§9.6)

SUBSCRIBE Request ID 4, namespace ["live","a"], track "video", no Parameters

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 19 bytes.

Wire: `03 00 10 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 00`

Envelope (§9 Fig. 5): Type `03` = 0x3, Length `00 10` = 16, Body 16 bytes `04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `03` | Message Type | 0x3 (SUBSCRIBE) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 10` | Message Length | 16 (0x0010) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `04` | Request ID | 4 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.6 Fig. 11; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.6 Fig. 11 / §2.4.1 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.6 Fig. 11 / §2.4.1 |
| 18 | `00` | Number of Parameters | 0 | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.6 Fig. 11 |

## Reading notes

- Fig. 11: `SUBSCRIBE Message { Type (vi64) = 0x3, Length (16), Request ID (vi64), Track Namespace (..), Track Name Length (vi64), Track Name (..), Number of Parameters (vi64), Parameters (..) ... }`.
- Track Namespace is §8.7: a vi64 field count, then each field as vi64 length + bytes. Track Name is a vi64 length + bytes (§9.6 Fig. 11). No field in draft-22 SUBSCRIBE carries Subscriber Priority, Group Order, Forward or a Filter Type as a fixed field; all of those are Message Parameters (§9.6: "The parameters that can appear in a SUBSCRIBE are ...").
- With no LOCATION_FILTER the subscription is unfiltered (§9.20.9: "If omitted from FETCH or SUBSCRIBE, the fetch or subscription is unfiltered"); FORWARD defaults to 1 and SUBSCRIBER_PRIORITY to 128 (§9.20.18, §9.20.7).
