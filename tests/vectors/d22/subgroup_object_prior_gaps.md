# `subgroup_object_prior_gaps.bin` - Subgroup Object (Fig. 27) (§11.3.1 / §11.1.2 / §10.8 / §10.9)

Subgroup Object on a PROPERTIES subgroup following Object 5: Object ID Delta 4 (Object 10), Object Properties Prior Group ID Gap (0x3C) = 3 and Prior Object ID Gap (0x3E) = 2, payload "hi"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 9 bytes.

Wire: `04 04 3C 03 02 02 02 68 69`

These bytes follow a stream header whose Type sets PROPERTIES, on a unidirectional data stream (no control envelope, §11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `04` | Object ID Delta | 4 (-> previous Object ID 5 + 1 + 4 = Object 10) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §11.3.1 Fig. 27 |
| 1 | `04` | Properties Length | 4 (bytes of Key-Value-Pairs that follow) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §11.1.2; §11.3.1 |
| 2 | `3C` | Object Property Delta Type | 0x3C -> Type 0x3C Prior Group ID Gap (first property: delta from 0) | vi64, 1-byte form: 60 = 0x3C | §8.3; §10.8; §16.8 |
| 3 | `03` | Prior Group ID Gap Value | 3 (the 3 Groups before this Object's Group do not and will never exist) | vi64, 1-byte form: 3 = 0x03 | §10.8 (even type -> single vi64) |
| 4 | `02` | Object Property Delta Type | 2 -> Type 0x3C + 2 = 0x3E Prior Object ID Gap | vi64, 1-byte form: 2 = 0x02 | §8.3; §10.9; §16.8 |
| 5 | `02` | Prior Object ID Gap Value | 2 (Objects 8 and 9 of this Group do not and will never exist) | vi64, 1-byte form: 2 = 0x02 | §10.9 (even type -> single vi64) |
| 6 | `02` | Object Payload Length | 2 | vi64, 1-byte form: 2 = 0x02 | §11.3.1 Fig. 27 |
| 7 | `68 69` | Object Payload | "hi" | 2 raw bytes (ASCII shown) | §11.3.1 |

## Reading notes

- §10.8 and §10.9: a gap larger than the Group ID or the Object ID, a second instance of either property on one Object, or a Group whose Objects disagree on the Prior Group ID Gap makes the Track malformed (§12.1). Here the Object is 10 and the gap is 2, so the Group must be at least 3.
- §10.7: the properties may also appear inside Immutable Properties; a processor searches both lists, and the Object may still carry only one instance of each.
