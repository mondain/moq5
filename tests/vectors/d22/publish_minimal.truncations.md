# `publish_minimal.bin` - truncation and Length-mismatch behaviour

PUBLISH (§9.8); full message 20 bytes: Type 1 byte(s) at [0,1), Length 2 bytes at [1,3), Body 17 bytes at [3,20).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1..2 | need more bytes | 16-bit Message Length incomplete |
| 3 | need more bytes | declared Length 17 not yet satisfied (0 of 17 body bytes present); the next byte needed belongs to field "Request ID" [3,4) - incomplete, not malformed |
| 4 | need more bytes | declared Length 17 not yet satisfied (1 of 17 body bytes present); the next byte needed belongs to field "Track Namespace: Number of Track Namespace Fields" [4,5) - incomplete, not malformed |
| 5 | need more bytes | declared Length 17 not yet satisfied (2 of 17 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Length" [5,6) - incomplete, not malformed |
| 6..9 | need more bytes | declared Length 17 not yet satisfied (3..6 of 17 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Value" [6,10) - incomplete, not malformed |
| 10 | need more bytes | declared Length 17 not yet satisfied (7 of 17 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Length" [10,11) - incomplete, not malformed |
| 11 | need more bytes | declared Length 17 not yet satisfied (8 of 17 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Value" [11,12) - incomplete, not malformed |
| 12 | need more bytes | declared Length 17 not yet satisfied (9 of 17 body bytes present); the next byte needed belongs to field "Track Name Length" [12,13) - incomplete, not malformed |
| 13..17 | need more bytes | declared Length 17 not yet satisfied (10..14 of 17 body bytes present); the next byte needed belongs to field "Track Name" [13,18) - incomplete, not malformed |
| 18 | need more bytes | declared Length 17 not yet satisfied (15 of 17 body bytes present); the next byte needed belongs to field "Track Alias" [18,19) - incomplete, not malformed |
| 19 | need more bytes | declared Length 17 not yet satisfied (16 of 17 body bytes present); the next byte needed belongs to field "Number of Parameters" [19,20) - incomplete, not malformed |
| 20 | complete | all 17 declared body bytes present; body parses to exactly 17 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 18, one 0x00 appended) | `1D 00 12 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 00 00` | malformed | the message ends in Track Properties, whose extent is "the remaining bytes of the message" (§8.4); the extra 0x00 is parsed as a Key-Value-Pair with Delta Type 0 (even) that lacks its vi64 Value -> cannot be parsed within the declared Length -> malformed |
| Length under-declared by 1 (Length = 16, last body byte dropped) | `1D 00 10 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07` | malformed | the body needs 17 bytes but only 16 are declared: field "Number of Parameters" is cut. A decoder MUST NOT read past the declared Length into the next message (the Length is the body boundary) and MUST NOT report "need more bytes" once 16 body bytes are present -> PROTOCOL_VIOLATION (§9) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
