# `fetch_object_eor_timed_out_alt_payload_length.bin` - Fetch End of Range (0x20C) (§11.4.1 / §11.4.1.2)

End of Timed-Out Range indicator: Serialization Flags 0x20C, Group ID 5, Object ID 9 - ALTERNATE reading: Fig. 29's Object Payload Length (0) is present

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 5 bytes.

Wire: `82 0C 05 09 00`

These bytes follow a stream header on a unidirectional data stream (no control envelope, §11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `82 0C` | Serialization Flags | 0x20C End of Timed-Out Range | vi64, 2-byte form (first byte starts 0b10): 524 = 0x20C; first byte = 0x80 \| 0x02 | §11.4.1 Table 8 |
| 2 | `05` | Group ID | 5 (absolute) | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §11.4.1.2 |
| 3 | `09` | Object ID | 9 (absolute) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §11.4.1.2 |
| 4 | `00` | Object Payload Length (ALTERNATE READING) | 0 (no payload) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §11.4.1 Fig. 29 (unbracketed field) |

## Reading notes

- §11.4.1: "When 128 or greater, the value signals a range indicator"; Table 8: 0x8C End of Non-Existent Range, 0x10C End of Unknown Range, 0x20C End of Timed-Out Range. 0x20C = 524 needs the 2-byte vi64 form: 0x80 | 0x02 = 0x82, then 0x0C -> `82 0C`.
- §11.4.1.2: "When Serialization Flags indicates an End of Range (e.g. values 0x8C, 0x10C, or 0x20C), the Group ID and Object ID fields are present. Subgroup ID, Priority and Properties are not present. All Objects with Locations between the last serialized Object, if any, and this Location, inclusive, either do not exist (when Serialization Flags is 0x8C), are unknown (0x10C), or timed out (0x20C)."
- The Group ID and Object ID are absolute: the next Object's "Prior Group ID and prior Object ID" are "The values from the End of Range indicator" (§11.4.1.2). The text names them "Group ID" / "Object ID", not "... Delta".
- Semantics here: following `fetch_object_first.bin` ({3,4}), every Location in ({3,4}, {5,9}] timed out waiting for upstream (FILL_TIMEOUT / §3.2.2), i.e. the rest of the requested range.
- ALTERNATE reading: Fig. 29 lists `Object Payload Length (vi64)` without brackets, i.e. unconditionally, and §11.4.1.2 only says which of the bracketed (optional) fields are present or absent; the low byte of the indicator values (0x0C = Group ID Delta present | Object ID Delta present, subgroup mode 0, no priority, no properties) matches the Fig. 29 flag semantics, suggesting the indicator is parsed with the ordinary field machinery, which then reads a Payload Length that must be 0. The primary reading (no Payload Length) is `fetch_object_eor_timed_out.bin`.
