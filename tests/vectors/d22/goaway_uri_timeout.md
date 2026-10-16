# `goaway_uri_timeout.bin` - GOAWAY (§9.2)

GOAWAY with 5-byte New Session URI "x://y" and Timeout 5000 ms

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 11 bytes.

Wire: `10 00 08 05 78 3A 2F 2F 79 93 88`

Envelope (§9 Fig. 5): Type `10` = 0x10, Length `00 08` = 8, Body 8 bytes `05 78 3A 2F 2F 79 93 88`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `10` | Message Type | 0x10 (GOAWAY) | vi64, 1-byte form (0b0xxxxxxx): 16 = 0x10 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 08` | Message Length | 8 (0x0008) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `05` | New Session URI Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.2 Fig. 7 |
| 4 | `78 3A 2F 2F 79` | New Session URI | "x://y" | 5 raw bytes (ASCII shown) | §9.2 Fig. 7 |
| 9 | `93 88` | Timeout | 5000 ms | vi64, 2-byte form (first byte starts 0b10): 5000 = 0x1388; first byte = 0x80 \| 0x13 | §9.2 Fig. 7 |

## Reading notes

- 5000 = 0x1388 needs the 2-byte vi64 form: 0x80 | 0x13 = 0x93, then 0x88 -> `93 88`.
- Only a server may send a non-zero URI length (§9.2: "If a server receives a GOAWAY with a non-zero New Session URI Length it MUST close the session with a PROTOCOL_VIOLATION"). The maximum URI length is 8,192 bytes.
