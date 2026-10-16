# `subgroup_object_status_eog.bin` - Subgroup Object (Fig. 27) (§11.3.1 / §11.1.1)

Subgroup Object with zero-length payload and Object Status End of Group (0x3), Object ID Delta 0

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 3 bytes.

Wire: `00 00 03`

These bytes follow a stream header on a unidirectional data stream (no control envelope, §11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `00` | Object ID Delta | 0 (-> previous Object ID + 1 when not first; 0 if first) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §11.3.1 Fig. 27 |
| 1 | `00` | Object Payload Length | 0 (so Object Status follows) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §11.3.1 Fig. 27 |
| 2 | `03` | Object Status | 0x3 End of Group ("no objects with the specified Group ID and the Object ID that is greater than or equal to the one specified exist") | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §11.1.1; §16.9 Table 17 |

## Reading notes

- §11.1.1 statuses: 0x0 Normal (payload permitted), 0x3 End of Group, 0x4 End of Track. "Zero-length objects explicitly encode the Normal status", so a zero-length Normal object is `<delta> 00 00`.
- If the header had PROPERTIES set, a Properties Length of 0 would sit between Object ID Delta and Payload Length (§11.3.1: "Objects with no properties or non-Normal status set Properties Length to 0").
