# `fetch_ok_end_location.bin` - FETCH_OK (§9.12)

FETCH_OK End Of Track 0, End Location {5, 9}, no Parameters, empty Track Properties

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 7 bytes.

Wire: `18 00 04 00 05 09 00`

Envelope (§9 Fig. 5): Type `18` = 0x18, Length `00 04` = 4, Body 4 bytes `00 05 09 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `18` | Message Type | 0x18 (FETCH_OK) | vi64, 1-byte form (0b0xxxxxxx): 24 = 0x18 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 04` | Message Length | 4 (0x0004) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `00` | End Of Track | 0 (not all Objects have been published) | uint8 (single byte, NOT a vi64): 0x00 | §9.12 Fig. 17 ("End Of Track (8)") |
| 4 | `05` | End Location Group | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.12 Fig. 17; §8.2 |
| 5 | `09` | End Location Object | 9 (inclusive) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.12 Fig. 17; §8.2 |
| 6 | `00` | Number of Parameters | 0 ("No parameters are currently defined for FETCH_OK") | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.12 Fig. 17 |
| 7 | `(none)` | Track Properties | (empty) | raw bytes | §8.4 |

## Reading notes

- Fig. 17: `FETCH_OK Message { Type (vi64) = 0x18, Length (16), End Of Track (8), End Location (Location), Number of Parameters (vi64), Parameters (..) ... Track Properties (..), }`.
- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- End Of Track is written `(8)`: a fixed single byte, not a vi64 (for the values 0 and 1 the two encodings coincide, but a decoder must consume exactly one byte).
- §9.12: "End Location: The end of the range covered by the FETCH response, inclusive."
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
