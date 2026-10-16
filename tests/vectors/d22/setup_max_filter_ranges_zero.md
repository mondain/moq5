# `setup_max_filter_ranges_zero.bin` - SETUP (§9.1 / §9.1.6)

SETUP with MAX_FILTER_RANGES (0x06) = 0

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 6 bytes.

Wire: `AF 00 00 02 06 00`

Envelope (§9 Fig. 5): Type `AF 00` = 0x2F00, Length `00 02` = 2, Body 2 bytes `06 00`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `AF 00` | Message Type | 0x2F00 (SETUP) | vi64, 2-byte form (first byte starts 0b10): 12032 = 0x2F00; first byte = 0x80 \| 0x2F | §9 Fig. 5; type table §9 Table 5 |
| 2 | `00 02` | Message Length | 2 (0x0002) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 4 | `06` | Setup Option Delta Type | 0x06 -> Type 0x06 MAX_FILTER_RANGES (first Setup Option: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 6 = 0x06 | §8.3; §9.1.6; §16.4 |
| 5 | `00` | MAX_FILTER_RANGES Value | 0 (peer MUST NOT send Range filter parameters) | vi64, 1-byte form (0b0xxxxxxx): 0 = 0x00 | §9.1.6 |

## Reading notes

- draft-22 has ONE SETUP message (Type 0x2F00) sent by both client and server (§9.1: "The SETUP message is the first message each endpoint sends on its control stream"). Types 0x20/0x21 are "RESERVED (CLIENT_SETUP/SERVER_SETUP in <= 16)" (§9 Table 5). There is no Supported Versions list and no Selected Version field anywhere in the message: §6.2 "MOQT uses ALPN in QUIC and \"WT-Available-Protocols\" in WebTransport ... to perform version negotiation."
- Type 0x2F00 = 12032 needs the 2-byte vi64 form (range 0-16383): first byte 0x80 | 0x2F = 0xAF, second byte 0x00 -> `AF 00`.
- Setup Options encoding, §9.1: "Setup Options are serialized as Key-Value-Pairs Figure 3, spanning the entire message payload, bounded by the message Length field." Fig. 6 has no count field: `SETUP Message { Type (vi64) = 0x2F00, Length (16), Setup Options (..) ..., }`. Each option is a §8.3 KVP: `Delta Type (vi64), [Length (vi64),] Value (..)` where "Length: Only present when Type is odd" and "Value: A single variable-length integer when Type is even, otherwise a sequence of Length bytes."
- The same 0x2F00 is also listed as the unidirectional stream type for the control stream (§6.4.1 Table 2). Reading: the control stream begins directly with this message; the vi64 0x2F00 is written once and serves as both (see README Open questions).
- §9.1.6 says the default is 0, so sending 0 is semantically identical to omitting the option; the vector pins that an explicitly-sent 0 is a 2-byte KVP `06 00`.
