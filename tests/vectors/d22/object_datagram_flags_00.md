# `object_datagram_flags_00.bin` - OBJECT_DATAGRAM (§11.2.1)

OBJECT_DATAGRAM Type Flags 0x00: Track Alias 7, Group 2, Object 5, Priority 3, payload "hi"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 7 bytes.

Wire: `00 07 02 05 03 68 69`

This is one complete transport datagram (no control envelope, §11.2).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `00` | Type Flags | 0x00: PROPERTIES=0, END_OF_GROUP=0, ZERO_OBJECT_ID=0 (Object ID present), DEFAULT_PRIORITY=0 (Priority present), STATUS=0 (payload present) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §11.2.1 Fig. 25 |
| 1 | `07` | Track Alias | 7 | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §11.2.1; §3.1.3 |
| 2 | `02` | Group ID | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.2.1 |
| 3 | `05` | Object ID | 5 (present because ZERO_OBJECT_ID bit 0x04 is clear) | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §11.2.1 |
| 4 | `03` | Publisher Priority | 3 (present because DEFAULT_PRIORITY bit 0x08 is clear) | uint8 (single byte, NOT a vi64): 0x03 | §11.2.1 ("Publisher Priority (8)") |
| 5 | `68 69` | Object Payload | "hi" | 2 raw bytes (ASCII shown) | §11.2.1 ("the entirety of the transport datagram following the Object header") |

## Reading notes

- Fig. 25: `OBJECT_DATAGRAM { Type Flags (vi64), Track Alias (vi64), Group ID (vi64), [Object ID (vi64),] [Publisher Priority (8),] [Properties (..),] [Object Status (vi64),] [Object Payload (..),] }`.
- Bits (§11.2.1): PROPERTIES 0x01, END_OF_GROUP 0x02, ZERO_OBJECT_ID 0x04, DEFAULT_PRIORITY 0x08, bit 4 (0x10) reserved and MUST be zero, STATUS 0x20. With all clear, Object ID and Priority are present and the payload runs to the end of the datagram (no length field).
- This whole vector is one transport datagram; there is no control envelope.
