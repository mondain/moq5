# `subgroup_header_flags_54.bin` - SUBGROUP_HEADER (§11.3.1) with FIRST_OBJECT

SUBGROUP_HEADER Type Flags 0x54: FIRST_OBJECT set, Track Alias 7, Group 2, Subgroup ID 1 (explicit), Priority 3

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 5 bytes.

Wire: `54 07 02 01 03`

These are the first bytes of a unidirectional data stream (no control envelope, §6.4.1/§11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `54` | Type Flags | 0x54 = 0b0101_0100: bit 4 (0x10) always 1; PROPERTIES (0x01)=0; SUBGROUP_ID_MODE (mask 0x06) = 0b10 -> field present -> 0b10<<1 = 0x04; END_OF_GROUP (0x08)=0; DEFAULT_PRIORITY (0x20)=0 -> Priority present; FIRST_OBJECT (0x40)=1 | vi64, 1-byte form (0b0xxxxxxx): 84 = 0x54 | §11.3.1; §6.4.1 Table 2 (0b0XX1XXXX) |
| 1 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §11.3.1 Fig. 26; §3.1.3 |
| 2 | `02` | Group ID | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.3.1 Fig. 26 |
| 3 | `01` | Subgroup ID | 1 (present: SUBGROUP_ID_MODE 0b10) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §11.3.1 |
| 4 | `03` | Publisher Priority | 3 (present: DEFAULT_PRIORITY clear) | uint8 (single byte, NOT a vi64): 0x03 | §11.3.1 ("Publisher Priority (8)") |

## Reading notes

- Same header as `subgroup_header_flags_14.bin` with the FIRST_OBJECT bit added: 0x14 | 0x40 = 0x54. 84 is below 128, so the Type Flags still take the 1-byte vi64 form, and the "Values of 128 or greater" invalid case does not apply.
- §11.3.1: "The *FIRST_OBJECT* bit (0x40) indicates that the first object in this subgroup stream is the first object published in the subgroup by the original publisher."
- §2.2: "When the Original Publisher opens a new subgroup, it MUST set the FIRST_OBJECT bit (Section 11.3.1) to indicate that the first object in the subgroup stream is the first object ever published in that subgroup. A relay forwarding a subgroup that begins with the first object ever published in that subgroup MUST set the FIRST_OBJECT bit."
- §6.4.1 Table 2 classifies the stream as SUBGROUP_HEADER by the form 0b0XX1XXXX; 0x54 = 0b0101_0100 matches (bit 7 clear, bit 4 set).
