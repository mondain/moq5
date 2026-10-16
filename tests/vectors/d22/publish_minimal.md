# `publish_minimal.bin` - PUBLISH (§9.8)

PUBLISH Request ID 1, namespace ["live","a"], track "video", Track Alias 7, no Parameters, empty Track Properties

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 20 bytes.

Wire: `1D 00 11 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 00`

Envelope (§9 Fig. 5): Type `1D` = 0x1D, Length `00 11` = 17, Body 17 bytes `01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `1D` | Message Type | 0x1D (PUBLISH) | vi64, 1-byte form (0b0xxxxxxx): 29 = 0x1D | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 11` | Message Length | 17 (0x0011) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Request ID | 1 (server-generated: odd; a publishing client would use an even ID) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.8 Fig. 13; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.8 Fig. 13 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.8 Fig. 13 |
| 18 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9.8 Fig. 13; §3.1.3 |
| 19 | `00` | Number of Parameters | 0 | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.8 Fig. 13 |
| 20 | `(none)` | Track Properties | (empty) | raw bytes | §8.4 |

## Reading notes

- Fig. 13: `PUBLISH Message { Type (vi64) = 0x1D, Length (16), Request ID (vi64), Track Namespace (..), Track Name Length (vi64), Track Name (..), Track Alias (vi64), Number of Parameters (vi64), Parameters (..) ..., Track Properties (..), }`.
- Required parameters: none is unconditionally required. LARGEST_OBJECT is conditionally required (§9.20.17: "If Objects have been published on this Track the Publisher MUST include this parameter"); this vector is a PUBLISH for a Track with no Objects yet ("If omitted from a message, the sending endpoint has not published or received any Objects in the Track"). `publish_largest_object_props.bin` covers the populated case.
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
