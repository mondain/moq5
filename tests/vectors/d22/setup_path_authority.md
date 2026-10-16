# `setup_path_authority.bin` - SETUP (§9.1 / §9.1.2 / §9.1.1)

SETUP (client, native QUIC) with PATH (0x01) = "/moq" and AUTHORITY (0x05) = "ex.net"

Spec: draft-ietf-moq-transport-22 (1 October 2026). Total 18 bytes.

Wire: `AF 00 00 0E 01 04 2F 6D 6F 71 04 06 65 78 2E 6E 65 74`

Envelope (§9 Fig. 5): Type `AF 00` = 0x2F00, Length `00 0E` = 14, Body 14 bytes `01 04 2F 6D 6F 71 04 06 65 78 2E 6E 65 74`.

## Byte-by-byte derivation

| Offset | Bytes | Field | Value | Encoding | Defined in |
|---:|---|---|---|---|---|
| 0 | `AF 00` | Message Type | 0x2F00 (SETUP) | vi64, 2-byte form (first byte starts 0b10): 12032 = 0x2F00; first byte = 0x80 \| 0x2F | §9 Fig. 5; type table §9 Table 5 |
| 2 | `00 0E` | Message Length | 14 (0x000E) | 16-bit unsigned, network byte order (RFC 9000 §1.3 conventions, cited by §8) | §9 Fig. 5 |
| 4 | `01` | Setup Option Delta Type | 0x01 -> Type 0x01 PATH (first Setup Option: delta from 0 = the type itself) | vi64, 1-byte form (0b0xxxxxxx): 1 = 0x01 | §8.3; §9.1.2; §16.4 |
| 5 | `04` | PATH Length | 4 | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.3 ("Only present when Type is odd") |
| 6 | `2F 6D 6F 71` | PATH Value | "/moq" | 4 raw bytes (ASCII shown) | §9.1.2 (path-abempty [+ ?query] of the moqt URI) |
| 10 | `04` | Setup Option Delta Type | 0x04 -> Type 0x05 AUTHORITY (0x05 - 0x01 = 0x04) | vi64, 1-byte form (0b0xxxxxxx): 4 = 0x04 | §8.3; §9.1.1; §16.4 |
| 11 | `06` | AUTHORITY Length | 6 | vi64, 1-byte form (0b0xxxxxxx): 6 = 0x06 | §8.3 |
| 12 | `65 78 2E 6E 65 74` | AUTHORITY Value | "ex.net" | 6 raw bytes (ASCII shown) | §9.1.1 (authority component of the moqt URI) |

## Reading notes

- draft-22 has ONE SETUP message (Type 0x2F00) sent by both client and server (§9.1: "The SETUP message is the first message each endpoint sends on its control stream"). Types 0x20/0x21 are "RESERVED (CLIENT_SETUP/SERVER_SETUP in <= 16)" (§9 Table 5). There is no Supported Versions list and no Selected Version field anywhere in the message: §6.2 "MOQT uses ALPN in QUIC and \"WT-Available-Protocols\" in WebTransport ... to perform version negotiation."
- Type 0x2F00 = 12032 needs the 2-byte vi64 form (range 0-16383): first byte 0x80 | 0x2F = 0xAF, second byte 0x00 -> `AF 00`.
- Setup Options encoding, §9.1: "Setup Options are serialized as Key-Value-Pairs Figure 3, spanning the entire message payload, bounded by the message Length field." Fig. 6 has no count field: `SETUP Message { Type (vi64) = 0x2F00, Length (16), Setup Options (..) ..., }`. Each option is a §8.3 KVP: `Delta Type (vi64), [Length (vi64),] Value (..)` where "Length: Only present when Type is odd" and "Value: A single variable-length integer when Type is even, otherwise a sequence of Length bytes."
- The same 0x2F00 is also listed as the unidirectional stream type for the control stream (§6.4.1 Table 2). Reading: the control stream begins directly with this message; the vi64 0x2F00 is written once and serves as both (see README Open questions).
- Both PATH (0x01) and AUTHORITY (0x05) are odd types, so each carries a vi64 Length before its bytes. Options are emitted in ascending Type order and the second Delta Type is 0x05 - 0x01 = 0x04. A decoder that reads the second delta as an absolute type would see 0x04 (MAX_AUTH_TOKEN_CACHE_SIZE, even) and mis-parse the Length byte as the value.
- These two options are client-only (§9.1.1/§9.1.2: "It MUST NOT be used by the server, or when WebTransport is used"), so this vector is the draft-22 counterpart of a "CLIENT_SETUP".
