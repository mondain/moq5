# `track_status_ok.bin` - REQUEST_OK (TRACK_STATUS_OK) (§9.3 / §9.13 / §9.20.17 / §8.4)

REQUEST_OK answering TRACK_STATUS: LARGEST_OBJECT {10, 20} and Track Properties DEFAULT_PUBLISHER_PRIORITY (0x0E) = 100

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 9 bytes.

Wire: `07 00 06 01 09 0A 14 0E 64`

Envelope (§9 Fig. 5): Type `07` = 0x7, Length `00 06` = 6, Body 6 bytes `01 09 0A 14 0E 64`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `07` | Message Type | 0x7 (REQUEST_OK (TRACK_STATUS_OK)) | vi64, 1-byte form (0b0xxxxxxx): 7 = 0x07 | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 06` | Message Length | 6 (0x0006) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Number of Parameters | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.3 Fig. 8 |
| 4 | `09` | Parameter Type Delta | 0x09 -> Type 0x09 LARGEST_OBJECT (first parameter: delta = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 9 = 0x09 | §9.20 Fig. 24 |
| 5 | `0A` | LARGEST_OBJECT Group | 10 | vi64, 1-byte form (0b0xxxxxxx): 10 = 0x0A | §9.20.17; §8.2 |
| 6 | `14` | LARGEST_OBJECT Object | 20 | vi64, 1-byte form (0b0xxxxxxx): 20 = 0x14 | §9.20.17; §8.2 |
| 7 | `0E` | Track Property Delta Type | 0x0E -> Type 0x0E DEFAULT_PUBLISHER_PRIORITY (first Track Property: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 14 = 0x0E | §8.3; §10.4; §16.8 |
| 8 | `64` | DEFAULT_PUBLISHER_PRIORITY Value | 100 | vi64, 1-byte form (0b0xxxxxxx): 100 = 0x64 | §10.4 |

## Reading notes

- No Request ID field. §6.4.2.1: "Only request messages include a Request ID; response messages do not, since they are sent on the same bidirectional stream as the request." The request this answers is identified by the stream the message arrives on.
- §9.3: "TRACK_STATUS_OK: LARGEST_OBJECT" and "Track Properties are populated in TRACK_STATUS_OK; they are empty in PUBLISH_OK, REQUEST_UPDATE_OK, SUBSCRIBE_NAMESPACE_OK and PUBLISH_NAMESPACE_OK." There is no field saying which request this answers; the receiver knows from the stream.
- Message Parameters (§9.20 Fig. 24) are `Type Delta (vi64), Value (..)` where "The encoding is specified by each parameter definition" (uint8 / varint / Location / Length-prefixed / Track Namespace). They are NOT §8.3 Key-Value-Pairs: the odd-type-has-a-Length rule of §8.3 does not apply here. The block is bounded by `Number of Parameters`, not by a length (§9.20: "Because unknown parameters cannot be skipped, the block is bounded by a parameter count rather than a length.").
- Track Properties (§8.4) ARE §8.3 Key-Value-Pairs: `Delta Type (vi64), [Length (vi64) if Type is odd], Value` with an even-type Value being a single vi64. They "always appear as the final field in the messages that carry them; their length is the remaining bytes of the message after all preceding fields have been consumed" (§8.4) - there is no count and no length prefix. Their Delta Type chain starts from 0 independently of the Message Parameters chain: §8.3 deltas are "from the previous Type value, or from 0 if there is no previous Type value" and Properties live in a different type space (§16.8) than Message Parameters (§16.7), so a cross-chain delta would be meaningless (recorded as a reading in README Open questions).
