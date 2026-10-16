# `setup_server_options.bin` - SETUP (§9.1 / §9.1.3 / §9.1.5 / §9.1.7)

SETUP (server-style) with MAX_AUTH_TOKEN_CACHE_SIZE (0x04) = 1024, MOQT_IMPLEMENTATION (0x07) = "lib", MAX_REQUEST_UPDATES (0x08) = 1

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 14 bytes.

Wire: `AF 00 00 0A 04 84 00 03 03 6C 69 62 01 01`

Envelope (§9 Fig. 5): Type `AF 00` = 0x2F00, Length `00 0A` = 10, Body 10 bytes `04 84 00 03 03 6C 69 62 01 01`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `AF 00` | Message Type | 0x2F00 (SETUP) | vi64, 2-byte form (first byte starts 0b10): 12032 = 0x2F00; first byte = 0x80 \| 0x2F | §9 Fig. 5; type table §9 Table 5 |
| 2 | `00 0A` | Message Length | 10 (0x000A) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 4 | `04` | Setup Option Delta Type | 0x04 -> Type 0x04 MAX_AUTH_TOKEN_CACHE_SIZE (first Setup Option: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.3; §9.1.3; §16.4 |
| 5 | `84 00` | MAX_AUTH_TOKEN_CACHE_SIZE Value | 1024 bytes | vi64, 2-byte form (first byte starts 0b10): 1024 = 0x400; first byte = 0x80 \| 0x04 | §9.1.3 |
| 7 | `03` | Setup Option Delta Type | 0x03 -> Type 0x07 MOQT_IMPLEMENTATION (0x07 - 0x04 = 0x03) | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §8.3; §9.1.5; §16.4 |
| 8 | `03` | MOQT_IMPLEMENTATION Length | 3 | vi64, 1-byte form (0b0xxxxxxx): 3 = 0x03 | §8.3 |
| 9 | `6C 69 62` | MOQT_IMPLEMENTATION Value | "lib" | 3 raw bytes (ASCII shown) | §9.1.5 (UTF-8 implementation name/version) |
| 12 | `01` | Setup Option Delta Type | 0x01 -> Type 0x08 MAX_REQUEST_UPDATES (0x08 - 0x07 = 0x01) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.3; §9.1.7; §16.4 |
| 13 | `01` | MAX_REQUEST_UPDATES Value | 1 | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §9.1.7 |

## Reading notes

- draft-22 has ONE SETUP message (Type 0x2F00) sent by both client and server (§9.1: "The SETUP message is the first message each endpoint sends on its control stream"). Types 0x20/0x21 are "RESERVED (CLIENT_SETUP/SERVER_SETUP in <= 16)" (§9 Table 5). There is no Supported Versions list and no Selected Version field anywhere in the message: §6.2 "MOQT uses ALPN in QUIC and \"WT-Available-Protocols\" in WebTransport ... to perform version negotiation."
- Type 0x2F00 = 12032 needs the 2-byte vi64 form (range 0-16383): first byte 0x80 | 0x2F = 0xAF, second byte 0x00 -> `AF 00`.
- Setup Options encoding, §9.1: "Setup Options are serialized as Key-Value-Pairs Figure 3, spanning the entire message payload, bounded by the message Length field." Fig. 6 has no count field: `SETUP Message { Type (vi64) = 0x2F00, Length (16), Setup Options (..) ..., }`. Each option is a §8.3 KVP: `Delta Type (vi64), [Length (vi64),] Value (..)` where "Length: Only present when Type is odd" and "Value: A single variable-length integer when Type is even, otherwise a sequence of Length bytes."
- The same 0x2F00 is also listed as the unidirectional stream type for the control stream (§6.4.1 Table 2). Reading: the control stream begins directly with this message; the vi64 0x2F00 is written once and serves as both (see README Open questions).
- 1024 = 0x400 exceeds 127, so the Value uses the 2-byte vi64 form `84 00` (0x80 | 0x04, 0x00).
- The third Delta Type is 0x01 (0x08 - 0x07). It is a delta, not the PATH option; a decoder that treats it as PATH (odd) would read the following 0x01 as a Length and run off the end.
- No option here is client-only, so this is the draft-22 counterpart of a "SERVER_SETUP". The same bytes are equally valid from a client.
