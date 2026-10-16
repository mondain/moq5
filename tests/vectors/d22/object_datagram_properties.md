# `object_datagram_properties.bin` - OBJECT_DATAGRAM (§11.2.1 / §11.1.2)

OBJECT_DATAGRAM Type Flags 0x01 (PROPERTIES): Track Alias 7, Group 2, Object 5, Priority 3, Object Property PRIOR_OBJECT_ID_GAP (0x3E) = 2, payload "hi"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 10 bytes.

Wire: `01 07 02 05 03 02 3E 02 68 69`

This is one complete transport datagram (no control envelope, §11.2).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `01` | Type Flags | 0x01: PROPERTIES=1; other bits 0 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §11.2.1 |
| 1 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §11.2.1 |
| 2 | `02` | Group ID | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.2.1 |
| 3 | `05` | Object ID | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §11.2.1 |
| 4 | `03` | Publisher Priority | 3 | uint8 (single byte, NOT a vi64): 0x03 | §11.2.1 |
| 5 | `02` | Properties Length | 2 (bytes of Key-Value-Pairs that follow; MUST NOT be 0 when PROPERTIES is set) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.1.2; §11.2.1 |
| 6 | `3E` | Object Property Delta Type | 0x3E -> Type 0x3E PRIOR_OBJECT_ID_GAP (first Object Property: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 62 = 0x3E | §8.3; §10.9; §16.8 |
| 7 | `02` | PRIOR_OBJECT_ID_GAP Value | 2 (Objects 3 and 4 of Group 2 will never exist) | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §10.9 (even type -> single vi64) |
| 8 | `68 69` | Object Payload | "hi" | 2 raw bytes (ASCII shown) | §11.2.1 |

## Reading notes

- §11.1.2: `Object Properties { Properties Length (vi64), Properties (..), }` - "serialized as a length in bytes followed by Key-Value-Pairs". §11.2.1: a PROPERTIES bit with Properties Length 0 is a PROTOCOL_VIOLATION.
- Object Properties are §8.3 KVPs (delta-typed, odd types carry a Length). 0x3E is even -> bare vi64 value.
