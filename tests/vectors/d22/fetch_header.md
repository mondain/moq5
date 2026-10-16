# `fetch_header.bin` - FETCH_HEADER (§11.4.1)

FETCH_HEADER Type 0x5, Request ID 6

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 2 bytes.

Wire: `05 06`

These are the first bytes of a unidirectional data stream (no control envelope, §6.4.1/§11).

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `05` | Type | 0x5 FETCH_HEADER (also the unidirectional stream type) | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §11.4.1 Fig. 28; §6.4.1 Table 2 |
| 1 | `06` | Request ID | 6 (the FETCH's Request ID; for a fill fetch stream, the SUBSCRIBE or REQUEST_UPDATE Request ID, §3.4) | vi64, 1-byte form (0b0xxxxxxx): 6 = 0x06 | §11.4.1 Fig. 28 |

## Reading notes

- Fig. 28: `FETCH_HEADER { Type (vi64) = 0x5, Request ID (vi64), }`. Matches `fetch_filter_type_04.bin`.
