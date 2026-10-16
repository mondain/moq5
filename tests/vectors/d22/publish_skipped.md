# `publish_skipped.bin` - PUBLISH_SKIPPED (§9.19)

PUBLISH_SKIPPED with one-field Track Namespace Suffix ["a"] and Track Name "x"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 8 bytes.

Wire: `0F 00 05 01 01 61 01 78`

Envelope (§9 Fig. 5): Type `0F` = 0xF, Length `00 05` = 5, Body 5 bytes `01 01 61 01 78`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `0F` | Message Type | 0xF (PUBLISH_SKIPPED) | vi64, 1-byte form (0b0xxxxxxx): 15 = 0x0F | §9 Fig. 5; type table §9 Table 5 |
| 1 | `00 05` | Message Length | 5 (0x0005) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 3 | `01` | Track Namespace Suffix: Number of Track Namespace Fields | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.19 Fig. 23; §8.7 |
| 4 | `01` | Track Namespace Suffix Field[0] Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.19 Fig. 23; §8.7 |
| 5 | `61` | Track Namespace Suffix Field[0] Value | "a" | 1 raw byte (ASCII shown) | §9.19 Fig. 23; §8.7 |
| 6 | `01` | Track Name Length | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.19 Fig. 23 |
| 7 | `78` | Track Name | "x" | 1 raw byte (ASCII shown) | §9.19 Fig. 23 |

## Reading notes

- Fig. 23: `PUBLISH_SKIPPED Message { Type (vi64) = 0xF, Length (16), Track Namespace Suffix (..), Track Name Length (vi64), Track Name (..), }`. Sent on the SUBSCRIBE_TRACKS response stream (§3.6.3), so there is no Request ID; the suffix is relative to the SUBSCRIBE_TRACKS prefix.
- The suffix uses the full §8.7 structure (count, then length-prefixed fields); a one-field suffix is `01 01 61`.
