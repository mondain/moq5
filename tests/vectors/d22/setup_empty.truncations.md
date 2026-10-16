# `setup_empty.bin` - truncation and Length-mismatch behaviour

SETUP (§9.1); full message 4 bytes: Type 2 byte(s) at [0,2), Length 2 bytes at [2,4), Body 0 bytes at [4,4).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1 | need more bytes | Message Type vi64 incomplete: first byte 0xAF has 1 leading 1-bit(s), so the integer is 2 bytes (§8.1) |
| 2..3 | need more bytes | 16-bit Message Length incomplete |
| 4 | complete | Length = 0, body empty: the message is complete at 4 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 1, one 0x00 appended) | `AF 00 00 01 00` | malformed | Setup Options span the whole body (§9.1), so the extra 0x00 is parsed as another Key-Value-Pair: Delta Type 0 (even) requires a vi64 Value, but the bounded sequence ends -> the KVP cannot be parsed within the declared Length -> malformed. (Unknown option types are ignored only when they can be parsed, §9.1/§13.) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
