# `publish_largest_object_props.bin` - PUBLISH (§9.8 / §9.20.17 / §8.4)

PUBLISH Request ID 1, Track Alias 7, LARGEST_OBJECT {2,5}, FORWARD 1, Track Properties DEFAULT_PUBLISHER_PRIORITY=3 and DYNAMIC_GROUPS=1

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 29 bytes.

Wire: `1D 00 1A 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 02 09 02 05 07 01 0E 03 22 01`

Envelope (§9 Fig. 5): Type `1D` = 0x1D, Length `00 1A` = 26, Body 26 bytes `01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 02 09 02 05 07 01 0E 03 22 01`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `1D` | Message Type | 0x1D (PUBLISH) | vi64, 1-byte form (0b0xxxxxxx): 29 = 0x1D | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 1A` | Message Length | 26 (0x001A) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Request ID | 1 (server-generated: odd; a publishing client would use an even ID) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.8 Fig. 13; §6.4.2.1 |
| 4 | `02` | Track Namespace: Number of Track Namespace Fields | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §8.7 |
| 5 | `04` | Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.7 |
| 6 | `6C 69 76 65` | Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §8.7 |
| 10 | `01` | Track Namespace Field[1] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.7 |
| 11 | `61` | Track Namespace Field[1] Value | "a" | 1 raw byte (ASCII shown) | §8.7 |
| 12 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.8 Fig. 13 |
| 13 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.8 Fig. 13 |
| 18 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9.8 Fig. 13; §3.1.3 |
| 19 | `02` | Number of Parameters | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.8 Fig. 13 |
| 20 | `09` | Parameter Type Delta | 0x09 -> Type 0x09 LARGEST_OBJECT (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20 Fig. 24 |
| 21 | `02` | LARGEST_OBJECT Group | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.20.17; §8.2 |
| 22 | `05` | LARGEST_OBJECT Object | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.20.17; §8.2 |
| 23 | `07` | Parameter Type Delta | 0x07 -> Type 0x10 FORWARD (0x10 - 0x09 = 0x07) | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9.20 Fig. 24 |
| 24 | `01` | FORWARD Value | 1 | uint8 (single byte, NOT a vi64): 0x01 | §9.20.18 ("is a uint8") |
| 25 | `0E` | Track Property Delta Type | 0x0E -> Type 0x0E DEFAULT_PUBLISHER_PRIORITY (first Track Property: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 14 = 0x0E | §8.3; §10.4; §16.8 |
| 26 | `03` | DEFAULT_PUBLISHER_PRIORITY Value | 3 | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §10.4 (0-255; even type -> single vi64 per §8.3) |
| 27 | `22` | Track Property Delta Type | 0x22 -> Type 0x30 DYNAMIC_GROUPS (0x30 - 0x0E = 0x22) | vi64, 1-byte form (0b0xxxxxxx): 34 = 0x22 | §8.3; §10.6; §16.8 |
| 28 | `01` | DYNAMIC_GROUPS Value | 1 (subscriber may send NEW_GROUP_REQUEST) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §10.6 |

## Reading notes

- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
- Two independent delta chains: Message Parameters 0x09 -> 0x10 (delta 0x07); Track Properties 0x0E -> 0x30 (delta 0x22). The first Track Property delta is 0x0E, not 0x0E - 0x10.
- Track Properties have no count: the decoder knows they start after the last Parameter and end at the message Length. An even Property type carries a single vi64 value; these two are even, so no Length bytes appear. Note that Property type 0x22 here would be DEFAULT_PUBLISHER_GROUP_ORDER if it were an absolute type; it is the delta to 0x30.
