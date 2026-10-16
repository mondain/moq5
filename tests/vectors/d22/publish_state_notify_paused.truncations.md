# `publish_state_notify_paused.bin` - truncation and Length-mismatch behaviour

PUBLISH_STATE_NOTIFY (§9.10 / §9.20.18); full message 9 bytes: Type 1 byte(s) at [0,1), Length 2 bytes at [1,3), Body 6 bytes at [3,9).

## Rule relied on

> §9: "Control messages have a length to simplify parsing, but no control messages are intended to be ignored. The length is set to the number of bytes in the Message Body, which is defined by each message type. If the length does not match the length of the Message Body, the receiver MUST close the session with a PROTOCOL_VIOLATION."

The specification defines no "need more bytes" outcome and no rule about partially received messages. That state follows from the envelope alone: the §9 rule compares the declared Length with the parsed body, and a receiver cannot perform that comparison until the Type, the 16-bit Length and `Length` body bytes have all arrived. A prefix shorter than the full declared message is therefore "incomplete" and never "malformed" on its own, even if the bytes seen so far could not possibly complete into a valid body. (Where this file says what a mismatched body would do, that is derived from the field layout, not from any spec text about truncation.)

## Prefixes of the vector (bytes available -> required decoder result)

| Bytes available | Result | Why |
|---:|---|---|
| 0 | need more bytes | nothing read |
| 1..2 | need more bytes | 16-bit Message Length incomplete |
| 3 | need more bytes | declared Length 6 not yet satisfied (0 of 6 body bytes present); the next byte needed belongs to field "Number of Parameters" [3,4) - incomplete, not malformed |
| 4 | need more bytes | declared Length 6 not yet satisfied (1 of 6 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [4,5) - incomplete, not malformed |
| 5 | need more bytes | declared Length 6 not yet satisfied (2 of 6 body bytes present); the next byte needed belongs to field "LARGEST_OBJECT Group" [5,6) - incomplete, not malformed |
| 6 | need more bytes | declared Length 6 not yet satisfied (3 of 6 body bytes present); the next byte needed belongs to field "LARGEST_OBJECT Object" [6,7) - incomplete, not malformed |
| 7 | need more bytes | declared Length 6 not yet satisfied (4 of 6 body bytes present); the next byte needed belongs to field "Parameter Type Delta" [7,8) - incomplete, not malformed |
| 8 | need more bytes | declared Length 6 not yet satisfied (5 of 6 body bytes present); the next byte needed belongs to field "FORWARD Value" [8,9) - incomplete, not malformed |
| 9 | complete | all 6 declared body bytes present; body parses to exactly 6 bytes |

## Length mismatches that MUST be reported as malformed (PROTOCOL_VIOLATION)

These are derived variants of the vector, not separate files. They only become detectable once the declared number of body bytes has arrived; before that they are indistinguishable from an incomplete message.

| Variant | Bytes | Expected | Why |
|---|---|---|---|
| Length over-declared by 1 (Length = 7, one 0x00 appended) | `22 00 07 02 09 0A 14 07 00 00` | malformed | the body parses to the original 6 bytes and one declared byte is left unconsumed -> "the length does not match the length of the Message Body" -> PROTOCOL_VIOLATION (§9) |
| Length under-declared by 1 (Length = 5, last body byte dropped) | `22 00 05 02 09 0A 14 07` | malformed | the body needs 6 bytes but only 5 are declared: field "FORWARD Value" is cut. A decoder MUST NOT read past the declared Length into the next message (the Length is the body boundary) and MUST NOT report "need more bytes" once 5 body bytes are present -> PROTOCOL_VIOLATION (§9) |

A message whose declared Length exceeds 2^16-1 cannot be expressed (16-bit field); §9.20 notes "the total length of a control message is limited to 2^16-1 bytes".
