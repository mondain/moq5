# `fetch_ok_end_of_track_props.bin` - FETCH_OK (§9.12 / §8.4 / §10.3)

FETCH_OK End Of Track 1, End Location {5, 9}, no Parameters, Track Properties MAX_CACHE_DURATION (0x04) = 60000

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 11 bytes.

Wire: `18 00 08 01 05 09 00 04 C0 EA 60`

Envelope (§9 Fig. 5): Type `18` = 0x18, Length `00 08` = 8, Body 8 bytes `01 05 09 00 04 C0 EA 60`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `18` | Message Type | 0x18 (FETCH_OK) | vi64, 1-byte form (0b0xxxxxxx): 24 = 0x18 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 08` | Message Length | 8 (0x0008) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | End Of Track | 1 (End Location is the final Object in the Track) | uint8 (single byte, NOT a vi64): 0x01 | §9.12 Fig. 17 |
| 4 | `05` | End Location Group | 5 | vi64, 1-byte form (0b0xxxxxxx): 5 = 0x05 | §9.12 Fig. 17; §8.2 |
| 5 | `09` | End Location Object | 9 (inclusive) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.12 Fig. 17; §8.2 |
| 6 | `00` | Number of Parameters | 0 | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.12 Fig. 17 |
| 7 | `04` | Track Property Delta Type | 0x04 -> Type 0x04 MAX_CACHE_DURATION (first Track Property: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.3; §10.3; §16.8 |
| 8 | `C0 EA 60` | MAX_CACHE_DURATION Value | 60000 ms | vi64, 3-byte form (first byte starts 0b110): 60000 = 0xEA60; first byte = 0xC0 \| 0x00 | §10.3 (even type -> single vi64) |

## Reading notes

- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
- 60000 = 0xEA60 exceeds 16383, so the value takes the 3-byte vi64 form (prefix 0b110): 0xC0 | 0x00, 0xEA, 0x60 -> `C0 EA 60`.
