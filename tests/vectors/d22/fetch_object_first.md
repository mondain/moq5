# `fetch_object_first.bin` - Fetch Object (Fig. 29) (§11.4.1 / §11.4.1.1)

First Fetch Object, Serialization Flags 0x1F: Group 3, Subgroup 1 (explicit), Object 4, Priority 3, no Properties, payload "hi"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 8 bytes.

Wire: `1F 03 01 04 03 02 68 69`

These bytes follow a stream header on a unidirectional data stream (no control envelope, §11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `1F` | Serialization Flags | 0x1F = 0b0001_1111: Subgroup ID encoding (bits 0-1) = 0x03 'field is present'; 0x04 Object ID Delta present; 0x08 Group ID Delta present; 0x10 Priority present; 0x20 Properties absent; 0x40 clear (not Datagram) | vi64, 1-byte form (0b0xxxxxxx): 31 = 0x1F | §11.4.1.1 Tables 9-10 |
| 1 | `03` | Group ID Delta | 3 -> Group ID 3 (first Object: "these values are the absolute Group ID and Object ID") | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §11.4.1 Fig. 29; §11.4.1.1 |
| 2 | `01` | Subgroup ID | 1 (present: low bits 0x03) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §11.4.1 Fig. 29 |
| 3 | `04` | Object ID Delta | 4 -> Object ID 4 (absolute on the first Object) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §11.4.1 Fig. 29; §11.4.1.1 |
| 4 | `03` | Publisher Priority | 3 | uint8 (single byte, NOT a vi64): 0x03 | §11.4.1 Fig. 29 ("Publisher Priority (8)") |
| 5 | `02` | Object Payload Length | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.4.1 Fig. 29 |
| 6 | `68 69` | Object Payload | "hi" | 2 raw bytes (ASCII shown) | §11.4.1 Fig. 29 |

## Reading notes

- Fig. 29: `{ Serialization Flags (vi64), [Group ID Delta (vi64),] [Subgroup ID (vi64),] [Object ID Delta (vi64),] [Publisher Priority (8),] [Properties (..),] Object Payload Length (vi64), [Object Payload (..),] }`. Note Subgroup ID is serialized BETWEEN Group ID Delta and Object ID Delta.
- §11.4.1.1: "The first Object MUST include a Group ID Delta and Object ID Delta, and these values are the absolute Group ID and Object ID. If the first Object in the FETCH response uses a flag that references fields in the prior Object, the Subscriber MUST close the session with a PROTOCOL_VIOLATION." Flags 0x1F reference nothing prior (subgroup present, priority present).
- Follows `fetch_header.bin`; the Object is {3, 4}, the start of the range requested in `fetch_filter_type_04.bin`.
