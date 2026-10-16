# `fetch_ok_end_of_track_props.bin` - truncation and Length-mismatch behaviour

FETCH_OK (§9.12 / §8.4 / §10.3); full message 11 bytes: Type 1 byte(s) at [0,1), Length 2 bytes at [1,3), Body 8 bytes at [3,11).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1..2 | need more bytes | 16-bit Message Length incomplete |
| 3 | need more bytes | declared Length 8 not yet satisfied (0 of 8 body bytes present); the next byte needed belongs to field "End Of Track" [3,4) - incomplete, not malformed |
| 4 | need more bytes | declared Length 8 not yet satisfied (1 of 8 body bytes present); the next byte needed belongs to field "End Location Group" [4,5) - incomplete, not malformed |
| 5 | need more bytes | declared Length 8 not yet satisfied (2 of 8 body bytes present); the next byte needed belongs to field "End Location Object" [5,6) - incomplete, not malformed |
| 6 | need more bytes | declared Length 8 not yet satisfied (3 of 8 body bytes present); the next byte needed belongs to field "Number of Parameters" [6,7) - incomplete, not malformed |
| 7 | need more bytes | declared Length 8 not yet satisfied (4 of 8 body bytes present); the next byte needed belongs to field "Track Property Delta Type" [7,8) - incomplete, not malformed |
| 8..10 | need more bytes | declared Length 8 not yet satisfied (5..7 of 8 body bytes present); the next byte needed belongs to field "MAX_CACHE_DURATION Value" [8,11) - incomplete, not malformed |
| 11 | complete | all 8 declared body bytes present; body parses to exactly 8 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 9, one 0x00 appended) | `18 00 09 01 05 09 00 04 C0 EA 60 00` | malformed | the body parses to the original 8 bytes and one declared byte is left unconsumed -> "the length does not match the length of the Message Body" -> PROTOCOL_VIOLATION (§9) |
| Length under-declared by 1 (Length = 7, last body byte dropped) | `18 00 07 01 05 09 00 04 C0 EA` | malformed | the body needs 8 bytes but only 7 are declared: field "MAX_CACHE_DURATION Value" is cut. A decoder MUST NOT read past the declared Length into the next message (the Length is the body boundary) and MUST NOT report "need more bytes" once 7 body bytes are present -> PROTOCOL_VIOLATION (§9) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
