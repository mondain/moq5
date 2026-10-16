# `subscribe_fill_parameters.bin` - truncation and Length-mismatch behaviour

SUBSCRIBE (§9.6 / §9.20.15); full message 30 bytes: Type 1 byte(s) at [0,1), Length 2 bytes at [1,3), Body 27 bytes at [3,30).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1..2 | need more bytes | 16-bit Message Length incomplete |
| 3 | need more bytes | declared Length 27 not yet satisfied (0 of 27 body bytes present); the next byte needed belongs to field "Request ID" [3,4) - incomplete, not malformed |
| 4 | need more bytes | declared Length 27 not yet satisfied (1 of 27 body bytes present); the next byte needed belongs to field "Track Namespace: Number of Track Namespace Fields" [4,5) - incomplete, not malformed |
| 5 | need more bytes | declared Length 27 not yet satisfied (2 of 27 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Length" [5,6) - incomplete, not malformed |
| 6..9 | need more bytes | declared Length 27 not yet satisfied (3..6 of 27 body bytes present); the next byte needed belongs to field "Track Namespace Field[0] Value" [6,10) - incomplete, not malformed |
| 10 | need more bytes | declared Length 27 not yet satisfied (7 of 27 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Length" [10,11) - incomplete, not malformed |
| 11 | need more bytes | declared Length 27 not yet satisfied (8 of 27 body bytes present); the next byte needed belongs to field "Track Namespace Field[1] Value" [11,12) - incomplete, not malformed |
| 12 | need more bytes | declared Length 27 not yet satisfied (9 of 27 body bytes present); the next byte needed belongs to field "Track Name Length" [12,13) - incomplete, not malformed |
| 13..17 | need more bytes | declared Length 27 not yet satisfied (10..14 of 27 body bytes present); the next byte needed belongs to field "Track Name" [13,18) - incomplete, not malformed |
| 18 | need more bytes | declared Length 27 not yet satisfied (15 of 27 body bytes present); the next byte needed belongs to field "Number of Parameters" [18,19) - incomplete, not malformed |
| 19 | need more bytes | declared Length 27 not yet satisfied (16 of 27 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [19,20) - incomplete, not malformed |
| 20 | need more bytes | declared Length 27 not yet satisfied (17 of 27 body bytes present); the next byte needed belongs to field "Location Filter Type" [20,21) - incomplete, not malformed |
| 21 | need more bytes | declared Length 27 not yet satisfied (18 of 27 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [21,22) - incomplete, not malformed |
| 22 | need more bytes | declared Length 27 not yet satisfied (19 of 27 body bytes present); the next byte needed belongs to field "FILL_PARAMETERS Length" [22,23) - incomplete, not malformed |
| 23 | need more bytes | declared Length 27 not yet satisfied (20 of 27 body bytes present); the next byte needed belongs to field "  [inner] Number of Parameters" [23,24) - incomplete, not malformed |
| 24 | need more bytes | declared Length 27 not yet satisfied (21 of 27 body bytes present); the next byte needed belongs to field "  [inner] Parameter Type Delta" [24,25) - incomplete, not malformed |
| 25..26 | need more bytes | declared Length 27 not yet satisfied (22..23 of 27 body bytes present); the next byte needed belongs to field "  [inner] FILL_TIMEOUT Value" [25,27) - incomplete, not malformed |
| 27 | need more bytes | declared Length 27 not yet satisfied (24 of 27 body bytes present); the next byte needed belongs to field "  [inner] Parameter Type Delta" [27,28) - incomplete, not malformed |
| 28 | need more bytes | declared Length 27 not yet satisfied (25 of 27 body bytes present); the next byte needed belongs to field "  [inner] Location Filter Type" [28,29) - incomplete, not malformed |
| 29 | need more bytes | declared Length 27 not yet satisfied (26 of 27 body bytes present); the next byte needed belongs to field "  [inner] LOCATION_FILTER StartGroup" [29,30) - incomplete, not malformed |
| 30 | complete | all 27 declared body bytes present; body parses to exactly 27 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 28, one 0x00 appended) | `03 00 1C 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 07 02 0A 80 FA 17 01 01 00` | malformed | the body parses to the original 27 bytes and one declared byte is left unconsumed -> "the length does not match the length of the Message Body" -> PROTOCOL_VIOLATION (§9) |
| Length under-declared by 1 (Length = 26, last body byte dropped) | `03 00 1A 04 02 04 6C 69 76 65 01 61 05 76 69 64 65 6F 02 21 05 02 07 02 0A 80 FA 17 01` | malformed | the body needs 27 bytes but only 26 are declared: field "  [inner] LOCATION_FILTER StartGroup" is cut. A decoder MUST NOT read past the declared Length into the next message (the Length is the body boundary) and MUST NOT report "need more bytes" once 26 body bytes are present -> PROTOCOL_VIOLATION (§9) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
