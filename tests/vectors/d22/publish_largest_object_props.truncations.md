# `publish_largest_object_props.bin` - truncation and Length-mismatch behaviour

PUBLISH (§9.8 / §9.20.17 / §8.4); full message 29 bytes: Type 1 byte(s) at [0,1), Length 2 bytes at [1,3), Body 26 bytes at [3,29).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1..2 | need more bytes | 16-bit Message Length incomplete |
| 3 | need more bytes | declared Length 26 not yet satisfied (0 of 26 body bytes present); the next byte needed belongs to field "Request ID" [3,4) - incomplete, not malformed |
| 4 | need more bytes | declared Length 26 not yet satisfied (1 of 26 body bytes present); the next byte needed belongs to field "Track Namespace: Number of Track Namespace Fields" [4,5) - incomplete, not malformed |
| 5 | need more bytes | declared Length 26 not yet satisfied (2 of 26 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Length" [5,6) - incomplete, not malformed |
| 6..9 | need more bytes | declared Length 26 not yet satisfied (3..6 of 26 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Value" [6,10) - incomplete, not malformed |
| 10 | need more bytes | declared Length 26 not yet satisfied (7 of 26 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Length" [10,11) - incomplete, not malformed |
| 11 | need more bytes | declared Length 26 not yet satisfied (8 of 26 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Value" [11,12) - incomplete, not malformed |
| 12 | need more bytes | declared Length 26 not yet satisfied (9 of 26 body bytes present); the next byte needed belongs to field "Track Name Length" [12,13) - incomplete, not malformed |
| 13..17 | need more bytes | declared Length 26 not yet satisfied (10..14 of 26 body bytes present); the next byte needed belongs to field "Track Name" [13,18) - incomplete, not malformed |
| 18 | need more bytes | declared Length 26 not yet satisfied (15 of 26 body bytes present); the next byte needed belongs to field "Track Alias" [18,19) - incomplete, not malformed |
| 19 | need more bytes | declared Length 26 not yet satisfied (16 of 26 body bytes present); the next byte needed belongs to field "Number of Parameters" [19,20) - incomplete, not malformed |
| 20 | need more bytes | declared Length 26 not yet satisfied (17 of 26 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [20,21) - incomplete, not malformed |
| 21 | need more bytes | declared Length 26 not yet satisfied (18 of 26 body bytes present); the next byte needed belongs to field "LARGEST_OBJECT Group" [21,22) - incomplete, not malformed |
| 22 | need more bytes | declared Length 26 not yet satisfied (19 of 26 body bytes present); the next byte needed belongs to field "LARGEST_OBJECT Object" [22,23) - incomplete, not malformed |
| 23 | need more bytes | declared Length 26 not yet satisfied (20 of 26 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [23,24) - incomplete, not malformed |
| 24 | need more bytes | declared Length 26 not yet satisfied (21 of 26 body bytes present); the next byte needed belongs to field "FORWARD Value" [24,25) - incomplete, not malformed |
| 25 | need more bytes | declared Length 26 not yet satisfied (22 of 26 body bytes present); the next byte needed belongs to field "Track Property Delta Type" [25,26) - incomplete, not malformed |
| 26 | need more bytes | declared Length 26 not yet satisfied (23 of 26 body bytes present); the next byte needed belongs to field "DEFAULT_PUBLISHER_PRIORITY Value" [26,27) - incomplete, not malformed |
| 27 | need more bytes | declared Length 26 not yet satisfied (24 of 26 body bytes present); the next byte needed belongs to field "Track Property Delta Type" [27,28) - incomplete, not malformed |
| 28 | need more bytes | declared Length 26 not yet satisfied (25 of 26 body bytes present); the next byte needed belongs to field "DYNAMIC_GROUPS Value" [28,29) - incomplete, not malformed |
| 29 | complete | all 26 declared body bytes present; body parses to exactly 26 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 27, one 0x00 appended) | `1D 00 1B 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 02 09 02 05 07 01 0E 03 22 01 00` | malformed | the body parses to the original 26 bytes and one declared byte is left unconsumed -> "the length does not match the length of the Message Body" -> PROTOCOL_VIOLATION (§9) |
| Length under-declared by 1 (Length = 25, last body byte dropped) | `1D 00 19 01 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 07 02 09 02 05 07 01 0E 03 22` | malformed | the body needs 26 bytes but only 25 are declared: field "DYNAMIC_GROUPS Value" is cut. A decoder MUST NOT read past the declared Length into the next message (the Length is the body boundary) and MUST NOT report "need more bytes" once 25 body bytes are present -> PROTOCOL_VIOLATION (§9) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
