# `request_error_invalid_filter.bin` - REQUEST_ERROR (§9.4 / §12.3 / §16.11.2)

REQUEST_ERROR with Error Code INVALID_FILTER (0x36), Retry Interval 0, Reason "bad"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 9 bytes.

Wire: `05 00 06 36 00 03 62 61 64`

Envelope (§9 Fig. 5): Type `05` = 0x5, Length `00 06` = 6, Body 6 bytes `36 00 03 62 61 64`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `05` | Message Type | 0x5 (REQUEST_ERROR) | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 06` | Message Length | 6 (0x0006) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `36` | Error Code | 0x36 INVALID_FILTER ("A filter parameter is invalid.") | vi64, 1-byte form (0b0xxxxxxx): 54 = 0x36 | §12.3; §16.11.2 Table 20 |
| 4 | `00` | Retry Interval | 0 ("the request SHOULD NOT be retried") | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.4.2 Fig. 9 |
| 5 | `03` | Reason Phrase Length | 3 | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §8.5 |
| 6 | `62 61 64` | Reason Phrase Value | "bad" | 3 raw bytes (ASCII shown) | §8.5 |
| 9 | `(none)` | Redirect | (absent: only present when Error Code is REDIRECT 0x34) | raw bytes | §9.4.2; §9.4.1 |

## Reading notes

- Fig. 9: `REQUEST_ERROR Message { Type (vi64) = 0x5, Length (16), Error Code (vi64), Retry Interval (vi64), Error Reason (Reason Phrase), [Redirect (Redirect),] }`.
- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- INVALID_FILTER is 0x36 in the IANA table §16.11.2 (Table 20); §12.3 gives it no inline code, so the registry is the only source. 0x36 = 54 fits the 1-byte vi64 form.
- Reason Phrase (§8.5) is `Reason Phrase Length (vi64), Reason Phrase Value (..)`, max 1024 bytes.
