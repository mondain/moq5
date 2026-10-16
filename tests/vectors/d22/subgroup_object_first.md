# `subgroup_object_first.bin` - Subgroup Object (Fig. 27) (§11.3.1)

First Object on a Subgroup stream (header without PROPERTIES bit): Object ID Delta 5 -> Object ID 5, payload "hi"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 4 bytes.

Wire: `05 02 68 69`

These bytes follow a stream header on a unidirectional data stream (no control envelope, §11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `05` | Object ID Delta | 5 -> Object ID 5 ("The Object ID is the Object ID Delta if it's the first Object in the Subgroup stream") | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §11.3.1 Fig. 27 |
| 1 | `02` | Object Payload Length | 2 | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §11.3.1 Fig. 27 |
| 2 | `68 69` | Object Payload | "hi" | 2 raw bytes (ASCII shown) | §11.3.1 Fig. 27 |

## Reading notes

- Fig. 27: `{ Object ID Delta (vi64), [Properties (..),] Object Payload Length (vi64), [Object Status (vi64),] [Object Payload (..),] }`. Properties appear only if the SUBGROUP_HEADER PROPERTIES bit is set (then on every Object, with Length 0 when there are none). "The Object Status field is only sent if the Object Payload Length is zero."
- Follows `subgroup_header_flags_14.bin` on the same stream. A second Object with delta 0 would be Object ID 5 + 0 + 1 = 6 ("The Object ID Delta + 1 is added to the previous Object ID").
