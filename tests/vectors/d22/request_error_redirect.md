# `request_error_redirect.bin` - REQUEST_ERROR (§9.4 / §9.4.1)

REQUEST_ERROR with Error Code REDIRECT (0x34), Retry Interval 1, empty Reason, Redirect to ["live"] / "video" at the current URI

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 19 bytes.

Wire: `05 00 10 34 01 00 00 01 04 6C 69 76 65 05 76 69 64 65 6F`

Envelope (§9 Fig. 5): Type `05` = 0x5, Length `00 10` = 16, Body 16 bytes `34 01 00 00 01 04 6C 69 76 65 05 76 69 64 65 6F`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `05` | Message Type | 0x5 (REQUEST_ERROR) | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 10` | Message Length | 16 (0x0010) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `34` | Error Code | 0x34 REDIRECT | vi64, 1-byte form (0b0xxxxxxx): 52 = 0x34 | §12.3; §16.11.2 Table 20 |
| 4 | `01` | Retry Interval | 1 ("can be retried immediately") | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.4.2 Fig. 9 |
| 5 | `00` | Reason Phrase Length | 0 | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §8.5 |
| 6 | `00` | Redirect: Connect URI Length | 0 ("the requester SHOULD use the current session's URI") | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.4.1 |
| 7 | `01` | Redirect: Track Namespace: Number of Track Namespace Fields | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.4.1; §8.7 |
| 8 | `04` | Redirect: Track Namespace Field[0] Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §9.4.1; §8.7 |
| 9 | `6C 69 76 65` | Redirect: Track Namespace Field[0] Value | "live" | 4 raw bytes (ASCII shown) | §9.4.1; §8.7 |
| 13 | `05` | Track Name Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.4.1 |
| 14 | `76 69 64 65 6F` | Track Name | "video" | 5 raw bytes (ASCII shown) | §9.4.1 |

## Reading notes

- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- §9.4.1: `Redirect { Connect URI Length (vi64), Connect URI (..), Track Namespace (..), Track Name Length (vi64), Track Name (..), }`. It follows the Reason Phrase and is present only when Error Code is REDIRECT (§9.4.2).
- A Reason Phrase Length of 0 is followed by no value bytes (§8.5).
