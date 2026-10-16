# `subgroup_header_flags_14.bin` - SUBGROUP_HEADER (§11.3.1)

SUBGROUP_HEADER Type Flags 0x14: Track Alias 7, Group 2, Subgroup ID 1 (explicit), Priority 3

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 5 bytes.

Wire: `14 07 02 01 03`

These are the first bytes of a unidirectional data stream (no control envelope, §6.4.1/§11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `14` | Type Flags | 0x14 = 0b0001_0100: bit 4 (0x10) always 1; PROPERTIES (0x01)=0; SUBGROUP_ID_MODE (mask 0x06) = 0b10 -> field present -> 0b10<<1 = 0x04; END_OF_GROUP (0x08)=0; DEFAULT_PRIORITY (0x20)=0 -> Priority present; FIRST_OBJECT (0x40)=0 | vi64, 1-byte form (0b0xxxxxxx): 20 = 0x14 | §11.3.1; §6.4.1 Table 2 (0b0XX1XXXX) |
| 1 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §11.3.1 Fig. 26; §3.1.3 |
| 2 | `02` | Group ID | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.3.1 Fig. 26 |
| 3 | `01` | Subgroup ID | 1 (present: SUBGROUP_ID_MODE 0b10) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §11.3.1 |
| 4 | `03` | Publisher Priority | 3 (present: DEFAULT_PRIORITY clear) | uint8 (single byte, NOT a vi64): 0x03 | §11.3.1 ("Publisher Priority (8)") |

## Reading notes

- Fig. 26: `SUBGROUP_HEADER { Type Flags (vi64), Track Alias (vi64), Group ID (vi64), [Subgroup ID (vi64),] [Publisher Priority (8),] }`. These are the first bytes of a unidirectional stream; the Type Flags vi64 is simultaneously the stream type (§6.4.1 Table 2: `0b0XX1XXXX | SUBGROUP_HEADER`).
- Flag computation: "subgroup id present explicitly" -> SUBGROUP_ID_MODE = 0b10, which occupies bits 1-2 ("perform a bitwise AND with mask 0x06 and right-shift by 1 bit"), so the contribution is 0b10 << 1 = 0x04. "properties absent" -> 0x01 clear. "default priority absent" -> the DEFAULT_PRIORITY bit 0x20 is clear, so the Priority byte IS present. Bit 4 (0x10) MUST be 1. Total 0x10 | 0x04 = 0x14. Values >= 128 and bit 4 clear and mode 0b11 are PROTOCOL_VIOLATIONs.
- Objects that follow on this stream use Fig. 27 (`subgroup_object_*.bin`).
