# `track_status.bin` - TRACK_STATUS (§9.13)

TRACK_STATUS Request ID 8, namespace ["live","a"], track "video", INCLUDE_PROPERTIES (0x35) = 1

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 21 bytes.

Wire: `0D 00 12 08 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 01 35 01`

Envelope (§9 Fig. 5): Type `0D` = 0xD, Length `00 12` = 18, Body 18 bytes `08 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 01 35 01`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `0D` | Message Type | 0xD (TRACK_STATUS) | vi64, 1-byte form (0b0xxxxxxx): 13 = 0x0D | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 12` | Message Length | 18 (0x0012) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `08` | Request ID | 8 (client-generated: even) | vi64, 1-byte form (0b0xxxxxxx): 8 = 0x08 | §9.13 -> §9.6 Fig. 11; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.13 -> §9.6 Fig. 11 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.13 -> §9.6 Fig. 11 |
| 18 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.13 -> §9.6 Fig. 11 |
| 19 | `35` | Parameter Type Delta | 0x35 -> Type 0x35 INCLUDE_PROPERTIES (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 53 = 0x35 | §9.20 Fig. 24 |
| 20 | `01` | INCLUDE_PROPERTIES Value | 1 (send Track Properties in the OK) | uint8 (single byte, NOT a vi64): 0x01 | §9.20.21 ("is a uint8") |

## Reading notes

- §9.13: "The TRACK_STATUS message format is identical to the SUBSCRIBE message (Section 9.6)" with Type 0xD (§9 Table 5). "The parameters that can appear in a TRACK_STATUS are AUTHORIZATION_TOKEN and INCLUDE_PROPERTIES."
- The response is a REQUEST_OK (§1.5 calls it TRACK_STATUS_OK) carrying LARGEST_OBJECT and Track Properties: see `track_status_ok.bin`. Errors use REQUEST_ERROR (TRACK_STATUS_ERROR).
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
