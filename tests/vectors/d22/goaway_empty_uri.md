# `goaway_empty_uri.bin` - GOAWAY (§9.2)

GOAWAY with zero-length New Session URI and Timeout 0

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 5 bytes.

Wire: `10 00 02 00 00`

Envelope (§9 Fig. 5): Type `10` = 0x10, Length `00 02` = 2, Body 2 bytes `00 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `10` | Message Type | 0x10 (GOAWAY) | vi64, 1-byte form (0b0xxxxxxx): 16 = 0x10 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 02` | Message Length | 2 (0x0002) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `00` | New Session URI Length | 0 (client form: "A client MUST send a zero-length New Session URI") | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.2 Fig. 7 |
| 4 | `00` | Timeout | 0 ms ("no specific timeout, but the recipient SHOULD migrate as quickly as possible") | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.2 Fig. 7 |

## Reading notes

- Fig. 7: `GOAWAY Message { Type (vi64) = 0x10, Length (16), New Session URI Length (vi64), New Session URI (..), Timeout (vi64), }`. GOAWAY has no Request ID in draft-22 (changelog: "Remove Request ID from GOAWAY (#1623)").
- A GOAWAY whose body is the single byte `00` (URI length only) is malformed: Timeout is mandatory, so Length 1 would not match the body (§9).
