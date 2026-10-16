# `publish_done_stream_count_max.bin` - PUBLISH_DONE (§9.9 / §12.4)

PUBLISH_DONE Status TRACK_ENDED (0x2), Stream Count 2^64-1, Reason "ended"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 19 bytes.

Wire: `0B 00 10 02 FF FF FF FF FF FF FF FF FF 05 65 6E 64 65 64`

Envelope (§9 Fig. 5): Type `0B` = 0xB, Length `00 10` = 16, Body 16 bytes `02 FF FF FF FF FF FF FF FF FF 05 65 6E 64 65 64`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `0B` | Message Type | 0xB (PUBLISH_DONE) | vi64, 1-byte form (0b0xxxxxxx): 11 = 0x0B | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 10` | Message Length | 16 (0x0010) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `02` | Status Code | 0x2 TRACK_ENDED | vi64, 1-byte form (0b0xxxxxxx): 2 = 0x02 | §9.9 Fig. 14; §12.4; §16.11.3 |
| 4 | `FF FF FF FF FF FF FF FF FF` | Stream Count | 18446744073709551615 = 2^64 - 1 ("unable to set Stream Count to the exact number") | vi64, 9-byte form (prefix byte 0xFF, then 64 value bits): 18446744073709551615 | §9.9 |
| 13 | `05` | Reason Phrase Length | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §8.5 |
| 14 | `65 6E 64 65 64` | Reason Phrase Value | "ended" | 5 raw bytes (ASCII shown) | §8.5 |

## Reading notes

- Fig. 14: `PUBLISH_DONE Message { Type (vi64) = 0xB, Length (16), Status Code (vi64), Stream Count (vi64), Error Reason (Reason Phrase) }`.
- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- §9.9: "If the publisher is unable to set Stream Count to the exact number of streams opened for the subscription, it MUST set Stream Count to 2^64 - 1." §8.1 Table 3: the 9-byte form has leading bits 11111111 and 64 usable bits; Table 4 lists `0xffffffffffffffffff` = 18,446,744,073,709,551,615. So the field is nine 0xFF bytes: prefix byte 0xFF followed by the 8-byte big-endian value.
- The §9.20 ceiling on control messages (2^16-1 bytes) is irrelevant here; the body is 16 bytes.
