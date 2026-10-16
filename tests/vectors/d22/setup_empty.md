# `setup_empty.bin` - SETUP (§9.1)

SETUP with no Setup Options

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 4 bytes.

Wire: `AF 00 00 00`

Envelope (§9 Fig. 5): Type `AF 00` = 0x2F00, Length `00 00` = 0, Body 0 bytes `(empty)`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `AF 00` | Message Type | 0x2F00 (SETUP) | vi64, 2-byte form (first byte starts 0b10): 12032 = 0x2F00; first byte = 0x80 \| 0x2F | §9 Fig. 5; type table §9 Table 5 |
| 2 | `00 00` | Message Length | 0 (0x0000) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |

## Reading notes

- draft-22 has ONE SETUP message (Type 0x2F00) sent by both client and server (§9.1: "The SETUP message is the first message each endpoint sends on its control stream"). Types 0x20/0x21 are "RESERVED (CLIENT_SETUP/SERVER_SETUP in <= 16)" (§9 Table 5). There is no Supported Versions list and no Selected Version field anywhere in the message: §6.2 "MOQT uses ALPN in QUIC and \"WT-Available-Protocols\" in WebTransport ... to perform version negotiation."
- Type 0x2F00 = 12032 needs the 2-byte vi64 form (range 0-16383): first byte 0x80 | 0x2F = 0xAF, second byte 0x00 -> `AF 00`.
- Setup Options encoding, §9.1: "Setup Options are serialized as Key-Value-Pairs Figure 3, spanning the entire message payload, bounded by the message Length field." Fig. 6 has no count field: `SETUP Message { Type (vi64) = 0x2F00, Length (16), Setup Options (..) ..., }`. Each option is a §8.3 KVP: `Delta Type (vi64), [Length (vi64),] Value (..)` where "Length: Only present when Type is odd" and "Value: A single variable-length integer when Type is even, otherwise a sequence of Length bytes."
- The same 0x2F00 is also listed as the unidirectional stream type for the control stream (§6.4.1 Table 2). Reading: the control stream begins directly with this message; the vi64 0x2F00 is written once and serves as both (see README Open questions).
- With no options the body is empty and Length is 0. A decoder must accept a zero-length SETUP; nothing in §9.1 requires any option (PATH/AUTHORITY are only mandatory for native QUIC with a moqt URI, §9.1.1/§9.1.2).
