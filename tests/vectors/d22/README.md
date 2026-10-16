# MoQ Transport draft-22 golden wire vectors

Independent vectors derived from the specification text alone, so that they can
catch misreadings shared by more than one codec.

- **Spec revision:** `draft-ietf-moq-transport-22.txt`, dated **1 October 2026** on its
  first page ("Expires 4 April 2027"). All section numbers below refer to that file.
- **Sources consulted:** only that text, plus the clarification on
  moq-wg/moq-transport#1991 where the text is ambiguous (Open question 3). No
  implementation source, no other draft, no research reports. The `tests/vectors/d16/` directory was consulted for file-naming
  and manifest conventions only.
- **Layout:** one `<name>.bin` per vector (raw wire bytes), a sibling `<name>.md` with the
  byte-by-byte derivation (offset, bytes, field, value, encoding, defining section) and
  reading notes, and for every control message a `<name>.truncations.md` giving the
  incomplete-vs-malformed analysis. `manifest.json` indexes all vectors in the d16 style
  (`file`, `type`, `type_code`, `description`, `wire_length`, `wire_hex`, `expected`).
- Vectors whose name contains `_alt_` encode an **alternative reading** of ambiguous
  text; the file without `_alt_` is the primary reading. The manifest marks them with
  `alternate_reading_of`.

## Integer encoding (§8.1 vi64)

The encoding is **not** RFC 9000's 2-bit-prefix varint. §8.1:

> "The variable-length integer encoding uses the number of leading 1 bits of the first
> byte to indicate the length of the encoding in bytes. The remaining bits after the
> first 0 and subsequent bytes, if any, represent the integer value, encoded in network
> byte order."

> "Integers are encoded in 1 to 9 bytes and can encode up to 64 bit unsigned integers."

§8.1 Table 3 (summarised): `0xxxxxxx` 1 byte, 7 usable bits, 0–127; `10xxxxxx` 2 bytes,
14 bits, 0–16383; `110…` 3 bytes, 21 bits; `1110…` 4 bytes, 28 bits; `11110…` 5 bytes,
35 bits; `111110…` 6 bytes, 42 bits; `1111110…` 7 bytes, 49 bits; `11111110` 8 bytes,
56 bits; `11111111` 9 bytes, 64 bits (prefix byte is all ones, value in the following 8
bytes). Table 4 examples used as a self-check: `0x25` = 37, `0x8025` = 37, `0xbbbd` =
15,293, `0xffffffffffffffffff` = 2^64−1.

Concrete forms used in these vectors:

| Value | Bytes | Why |
|---|---|---|
| 0x2F00 (SETUP type) | `AF 00` | 12032 > 127 → 2-byte form; first byte 0x80 \| 0x2F |
| 128 (varint) | `80 80` | first value needing 2 bytes; contrast uint8 `80` |
| 250 | `80 FA` | FILL_TIMEOUT |
| 1024 | `84 00` | MAX_AUTH_TOKEN_CACHE_SIZE |
| 5000 | `93 88` | GOAWAY Timeout |
| 30000 | `C0 75 30` | > 16383 → 3-byte form (EXPIRES) |
| 0x20C = 524 | `82 0C` | fetch End of Timed-Out Range flags |
| 60000 | `C0 EA 60` | MAX_CACHE_DURATION Track Property |
| 2^64−1 | `FF FF FF FF FF FF FF FF FF` | 9-byte form (PUBLISH_DONE Stream Count) |

§8.1 also says non-minimal encodings are legal on receipt ("Variable-length integers do
not need to be encoded using the minimum number of bytes"); every vector here uses the
minimal form, which is what an encoder under test is expected to produce.

## Control message envelope (§9)

> ```
> MOQT Control Message {
>   Message Type (vi64),
>   Message Length (16),
>   Message Body (..),
> }
> ```

> "The length is set to the number of bytes in the Message Body, which is defined by each
> message type. If the length does not match the length of the Message Body, the receiver
> MUST close the session with a PROTOCOL_VIOLATION."

**Is Length a fixed 16-bit field, and is it big-endian?** `Message Length (16)` is a fixed
16-bit field. The MoQT text does not itself restate the byte order; §8 says "This document
uses the conventions detailed in ([RFC9000], Section 1.3) when describing the binary
encoding", and RFC 9000 §1.3 defines `x (A)` as a field A bits long with all numeric values
in network byte order (big-endian). §8.1 separately says vi64 values are "encoded in network
byte order". So Length is 2 bytes, big-endian: a 14-byte body is `00 0E`. §9.20 adds "the
total length of a control message is limited to 2^16-1 bytes".

Message type codes used (§9 Table 5): SETUP 0x2F00 · GOAWAY 0x10 · SUBSCRIBE 0x3 ·
SUBSCRIBE_OK 0x4 · REQUEST_ERROR 0x5 · REQUEST_OK 0x7 · REQUEST_UPDATE 0x2 · PUBLISH 0x1D ·
PUBLISH_DONE 0xB · PUBLISH_STATE_NOTIFY 0x22 · FETCH 0x16 · FETCH_OK 0x18 · TRACK_STATUS 0xD ·
PUBLISH_SKIPPED 0xF. (0x20/0x21 are "RESERVED (CLIENT_SETUP/SERVER_SETUP in <= 16)";
0x1E "RESERVED (PUBLISH_OK in <= 17)".)

**Request IDs.** Only request messages carry one. §6.4.2.1: "Only request messages include
a Request ID; response messages do not, since they are sent on the same bidirectional stream
as the request." Hence REQUEST_OK, REQUEST_ERROR, SUBSCRIBE_OK, FETCH_OK, PUBLISH_DONE and
PUBLISH_STATE_NOTIFY have no Request ID field; SUBSCRIBE, FETCH, PUBLISH, TRACK_STATUS and
REQUEST_UPDATE do. Client IDs are even from 0, server IDs odd from 1.

## SETUP and Setup Options (§6.3, §9.1, §8.3)

draft-22 has a single **SETUP** message, type 0x2F00, sent by both endpoints:

> ```
> SETUP Message {
>   Type (vi64) = 0x2F00,
>   Length (16),
>   Setup Options (..) ...,
> }
> ```

> "Setup Options are serialized as Key-Value-Pairs Figure 3, spanning the entire message
> payload, bounded by the message Length field."

So: **no option count**, and **no version list** (§6.2: "MOQT uses ALPN in QUIC and
"WT-Available-Protocols" in WebTransport ... to perform version negotiation"). Each option
is a §8.3 Key-Value-Pair:

> ```
> Key-Value-Pair {
>   Delta Type (vi64),
>   [Length (vi64),]
>   Value (..)
> }
> ```
> "Length: Only present when Type is odd." · "Value: A single variable-length integer when
> Type is even, otherwise a sequence of Length bytes." · "Key-Value-Pairs encode a Type value
> as a delta from the previous Type value, or from 0 if there is no previous Type value."

Setup Option types (§16.4): PATH 0x01 (odd → Length), AUTHORIZATION_TOKEN 0x03,
MAX_AUTH_TOKEN_CACHE_SIZE 0x04, AUTHORITY 0x05 (odd → Length), MAX_FILTER_RANGES 0x06,
MOQT_IMPLEMENTATION 0x07 (odd → Length), MAX_REQUEST_UPDATES 0x08. The "client" vector here
is the one carrying the client-only PATH/AUTHORITY options; the "server" vector carries only
direction-neutral options.

## Message Parameters (§9.20)

> ```
> Message Parameter {
>   Type Delta (vi64),
>   Value (..)
> }
> ```
> "Type Delta: The difference between this Parameter Type and the previous Parameter Type in
> the message, or the Parameter Type itself for the first parameter. Parameters MUST be
> serialized in ascending order by Type."

> "Value: The encoding is specified by each parameter definition. The encodings defined in
> this draft are: uint8 … varint … Location: Two consecutive varints (Group, Object) …
> Length-prefixed: A varint length followed by that many bytes … Track Namespace …"

> "Because unknown parameters cannot be skipped, the block is bounded by a parameter count
> rather than a length."

**Message Parameters are therefore not §8.3 KVPs**: the odd/even Length rule does not apply.
LARGEST_OBJECT (0x09, odd) is two bare varints; FORWARD (0x10), SUBSCRIBER_PRIORITY (0x20),
GROUP_ORDER (0x22) and INCLUDE_PROPERTIES (0x35) are single uint8 bytes (SUBSCRIBER_PRIORITY
128 is `80`, not `80 80`); EXPIRES (0x08), OBJECT/SUBGROUP_DELIVERY_TIMEOUT, NEW_GROUP_REQUEST
are varints; AUTHORIZATION_TOKEN (0x03) and FILL_PARAMETERS (0x23) are length-prefixed;
LOCATION_FILTER (0x21) is a Location Filter Type vi64 followed by 0–4 vi64 fields selected by
that type (§9.20.9 Table 6), with no length prefix; the Range filters 0x25–0x29 carry their own
`Length (vi64)` (§8.6).

Three delta chains that each restart from 0 and never mix: the message's Parameters; the
Parameters nested inside a FILL_PARAMETERS value ("a separate parameter scope", §9.20.15); and
the trailing Track Properties (§8.4 KVPs, a different type space, §16.8).

## Other common structures

- **Track Namespace (§8.7):** `Number of Track Namespace Fields (vi64)` then each field as
  `Length (vi64), Value`. 0–32 fields, each ≥ 1 byte.
- **Track Name:** `Track Name Length (vi64), Track Name (..)` inside the message figures.
- **Reason Phrase (§8.5):** `Reason Phrase Length (vi64), Reason Phrase Value (..)`, ≤ 1024.
- **Location (§8.2):** `Group (vi64), Object (vi64)`.
- **Track Properties (§8.4):** KVPs, "always appear as the final field … their length is the
  remaining bytes of the message".
- **Object Properties (§11.1.2):** `Properties Length (vi64)` then KVPs.

## Vectors

| File | Message | Section | Bytes | What it pins |
|---|---|---|---:|---|
| `setup_empty.bin` | SETUP | §9.1 | 4 | 2-byte vi64 type 0xAF00; Length 0; no option count field |
| `setup_max_request_updates.bin` | SETUP | §9.1 / §9.1.7 | 6 | even-type option = bare vi64 value, no Length |
| `setup_max_filter_ranges_zero.bin` | SETUP | §9.1 / §9.1.6 | 6 | explicit zero value is encoded (not elided); even-type option |
| `setup_path_authority.bin` | SETUP | §9.1 / §9.1.2 / §9.1.1 | 18 | odd-type options carry a vi64 Length; delta 0x04 (not 0x05) for AUTHORITY after PATH |
| `setup_server_options.bin` | SETUP | §9.1 / §9.1.3 / §9.1.5 / §9.1.7 | 14 | 2-byte vi64 option value; odd option between evens; delta 0x01 that is NOT PATH |
| `goaway_empty_uri.bin` | GOAWAY | §9.2 | 5 | Timeout field is present even with an empty URI; body is exactly 2 bytes |
| `goaway_uri_timeout.bin` | GOAWAY | §9.2 | 11 | URI length prefix; 2-byte vi64 Timeout after the URI |
| `request_ok_empty.bin` | REQUEST_OK | §9.3 | 4 | no Request ID; body is just the parameter count 00 |
| `request_ok_expires.bin` | REQUEST_OK | §9.3 / §9.20.16 | 8 | varint parameter value in 3-byte vi64 form (30000 > 16383); Track Properties empty |
| `request_error_invalid_filter.bin` | REQUEST_ERROR | §9.4 / §12.3 / §16.11.2 | 9 | no Request ID; INVALID_FILTER = 0x36; Reason Phrase = vi64 length + bytes; no Redirect |
| `request_error_redirect.bin` | REQUEST_ERROR | §9.4 / §9.4.1 | 19 | Redirect structure present only for 0x34; Connect URI length 0; namespace + track name inside Redirect |
| `request_update_forward_zero.bin` | REQUEST_UPDATE | §9.5 / §9.20.18 | 7 | REQUEST_UPDATE carries a Request ID (it consumes one); FORWARD is uint8 |
| `subscribe_minimal.bin` | SUBSCRIBE | §9.6 | 19 | field order Request ID / Namespace / Name / count; no fixed priority/order/filter fields |
| `subscribe_filter_type_00.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 25 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x00 field set: none |
| `subscribe_filter_type_01.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 26 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x01 field set: StartGroup |
| `subscribe_filter_type_02.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 27 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x02 field set: StartGroup, StartObject |
| `subscribe_filter_type_03.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 28 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x03 field set: StartGroup, StartObject, EndGroupDelta |
| `subscribe_filter_type_04.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 29 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x04 field set: StartGroup, StartObject, EndGroupDelta, EndObject |
| `subscribe_filter_type_05.bin` | SUBSCRIBE | §9.6 / §9.20.9 | 25 | deltas 0x10/0x10/0x01; uint8 priority 0x80; filter type 0x05 field set: none |
| `subscribe_fill_parameters.bin` | SUBSCRIBE | §9.6 / §9.20.15 | 30 | length-prefixed 0x23 holding Number of Parameters 2; inner delta chain restarts (0x0A then 0x17); 0x21 inside and outside |
| `subscribe_fill_parameters_empty.bin` | SUBSCRIBE | §9.6 / §9.20.15 | 24 | a fill with no settings of its own: 0x23 Length 1, Number of Parameters 0 |
| `subscribe_fill_parameters_alt_uncounted.bin` | SUBSCRIBE | §9.6 / §9.20.15 | 29 | refused alternate: the nested value without a count |
| `subscribe_ok_largest_object.bin` | SUBSCRIBE_OK | §9.7 / §9.20.17 | 8 | no Request ID; Track Alias first; LARGEST_OBJECT (odd type) has NO length prefix - two bare vi64s |
| `publish_minimal.bin` | PUBLISH | §9.8 | 20 | Track Alias sits between Track Name and the parameter count; nothing is mandatory when no Object has been published |
| `publish_largest_object_props.bin` | PUBLISH | §9.8 / §9.20.17 / §8.4 | 29 | parameter chain 0x09 then delta 0x07; Track Properties are KVPs with their own chain (0x0E, then 0x22) |
| `publish_done_stream_count_max.bin` | PUBLISH_DONE | §9.9 / §12.4 | 19 | 9-byte vi64 FF×9 for 2^64-1; no Request ID; Reason Phrase last |
| `publish_state_notify_largest_object.bin` | PUBLISH_STATE_NOTIFY | §9.10 / §9.20.17 | 7 | type 0x22 (single byte); count + one Location parameter; no Request ID, no Track Properties |
| `publish_state_notify_paused.bin` | PUBLISH_STATE_NOTIFY | §9.10 / §9.20.18 | 9 | ascending 0x09 then 0x10 (delta 0x07); uint8 FORWARD after a Location parameter |
| `fetch_filter_type_04.bin` | FETCH | §9.11 / §9.20.9 | 25 | FETCH has no Fetch Type / range / joining fields; the range lives in LOCATION_FILTER |
| `fetch_ok_end_location.bin` | FETCH_OK | §9.12 | 7 | End Of Track is a fixed 8-bit field; End Location = two vi64; no Request ID |
| `fetch_ok_end_of_track_props.bin` | FETCH_OK | §9.12 / §8.4 / §10.3 | 11 | 3-byte vi64 inside a Track Property; properties present with zero parameters |
| `track_status.bin` | TRACK_STATUS | §9.13 | 21 | same layout as SUBSCRIBE with type 0xD; uint8 INCLUDE_PROPERTIES; delta 0x35 |
| `track_status_ok.bin` | REQUEST_OK (TRACK_STATUS_OK) | §9.3 / §9.13 / §9.20.17 / §8.4 | 9 | REQUEST_OK with populated Track Properties (only legal for TRACK_STATUS_OK); Location param without length |
| `publish_skipped.bin` | PUBLISH_SKIPPED | §9.19 | 8 | no Request ID, no parameters; suffix is a §8.7 Track Namespace; name is length-prefixed |
| `object_datagram_flags_00.bin` | OBJECT_DATAGRAM | §11.2.1 | 7 | flags 0 -> Object ID present, Priority present, no Properties, payload to end of datagram |
| `object_datagram_properties.bin` | OBJECT_DATAGRAM | §11.2.1 / §11.1.2 | 10 | Object Properties = vi64 length + KVPs, placed after Priority and before the payload |
| `subgroup_header_flags_14.bin` | SUBGROUP_HEADER | §11.3.1 | 5 | flag byte 0x14 = bit4 \| SUBGROUP_ID_MODE 0b10<<1; Subgroup ID and Priority both present |
| `subgroup_header_flags_54.bin` | SUBGROUP_HEADER | §11.3.1 / §2.2 | 5 | flag byte 0x54 = 0x14 \| FIRST_OBJECT 0x40; still the 1-byte vi64 form (84 < 128) |
| `subgroup_object_first.bin` | Subgroup Object (Fig. 27) | §11.3.1 | 4 | first object's delta is the absolute ID; no Properties field when header bit 0x01 is clear; Status absent when payload length > 0 |
| `subgroup_object_status_eog.bin` | Subgroup Object (Fig. 27) | §11.3.1 / §11.1.1 | 3 | Status present only because Payload Length is 0; status 0x3 = End of Group |
| `fetch_header.bin` | FETCH_HEADER | §11.4.1 | 2 | stream type 0x05 then Request ID; nothing else in the header |
| `fetch_object_first.bin` | Fetch Object (Fig. 29) | §11.4.1 / §11.4.1.1 | 8 | field ORDER Group / Subgroup / Object / Priority; first object carries absolute Group and Object IDs |
| `fetch_object_eor_timed_out.bin` | Fetch End of Range (0x20C) | §11.4.1 / §11.4.1.2 | 4 | 2-byte flags 82 0C; exactly two vi64 fields follow; no Subgroup/Priority/Properties/Payload Length |
| `fetch_object_eor_timed_out_alt_payload_length.bin` | Fetch End of Range (0x20C) | §11.4.1 / §11.4.1.2 | 5 | alternate: trailing 00 Payload Length after Group/Object |

Mapping to the brief: the requested CLIENT_SETUP/SERVER_SETUP cases are the five `setup_*`
vectors (draft-22 has one SETUP type; see Open questions 1). REQUEST_OK/REQUEST_ERROR "for
Request ID 4" are `request_ok_*`/`request_error_*` (no Request ID on the wire; Open
questions 2). The TRACK_STATUS response is `track_status_ok.bin` (a REQUEST_OK, §1.5/§9.13).

## Truncation notes

Every control vector has a `.truncations.md`. The rule relied on is §9's Length/Body
mismatch → PROTOCOL_VIOLATION (quoted above). The spec does not define a "need more bytes"
state; the notes derive it from the envelope: any prefix shorter than Type + 2 + declared
Length is incomplete, never malformed; once exactly `Length` body bytes are present, a body
that consumes fewer or needs more than `Length` bytes is malformed. Each note also gives the
two derived mismatch variants (Length over-declared by one with a trailing `00`, and
under-declared by one) and explains, per message, which field breaks and why.

## Open questions and readings taken

1. **Brief vs. spec: SETUP.** The brief asked for CLIENT_SETUP and SERVER_SETUP with a
   version list. draft-22 has one SETUP (0x2F00) for both directions, no version field
   (negotiation is in ALPN / WT-Available-Protocols, §6.2) and Setup Options spanning the body
   without a count (§9.1). The vectors follow the spec; "client"/"server" flavour is only
   which options are present.
2. **Brief vs. spec: Request ID on responses.** REQUEST_OK/REQUEST_ERROR (and SUBSCRIBE_OK,
   FETCH_OK, PUBLISH_DONE) carry no Request ID (§6.4.2.1). The brief's "for Request ID 4" is
   satisfied by the stream, not by bytes.
3. **FILL_PARAMETERS nested count (§9.20.15) — counted (selected interpretation).**
   §9.20.15 (lines 5478-5482): the parameter "uses length-prefixed encoding. … Its value is
   a sequence of Parameters … encoded as if they were Parameters for a separate message."
   A message's Parameters are always `Number of Parameters (vi64)` followed by
   `Parameters (..) ...` (§9.6 Fig. 11 and every request figure), and §9.20 (lines
   5088-5090) says "the block is bounded by a parameter count rather than a length". The
   value is therefore `Number of Parameters, Parameters...` inside the draft-22 Length,
   with the Type Delta chain restarting at 0, and the count's parameters must end exactly
   at the Length. The text alone also admits a reading without the count, since the Length
   already bounds the sequence; moq-wg/moq-transport#1991 records that ambiguity. #1868
   had already changed the value from Key-Value-Pairs (length-bounded) to Parameters,
   "encoded as such", and the 2026-10-15 clarification on #1991 (https://github.com/moq-wg/moq-transport/issues/1991#issuecomment-6199709619) reads
   "it can be just { Num Parameter, Parameters }". The counted form is this suite's
   selected draft-22 interpretation, supported by that clarification; it is not published
   replacement text. Primary: `subscribe_fill_parameters.bin`
   and `subscribe_fill_parameters_empty.bin` (a fill with no settings is `23 01 00`, never
   an empty value). Refused alternate: `subscribe_fill_parameters_alt_uncounted.bin`. The
   same ruling expects a future draft to drop the outer Length; draft-22 keeps it, so these
   vectors do too.
4. **FILL_TIMEOUT (0x0A) and RENDEZVOUS_TIMEOUT (0x04) value encoding is not stated.**
   §9.20.5 and §9.20.6 give no encoding word, unlike every other parameter. Both are
   millisecond durations; read as **varint** by analogy with OBJECT_DELIVERY_TIMEOUT,
   SUBGROUP_DELIVERY_TIMEOUT and EXPIRES ("is a varint"). FILL_TIMEOUT 250 is therefore
   `80 FA`, not the uint8 `FA`.
5. **§3.4 "zero-length LOCATION_FILTER" vs §9.20.9 explicit type.** §3.4: "or the
   LOCATION_FILTER inside FILL_PARAMETERS is zero-length". §9.20.9 always requires a Location
   Filter Type and says "If Location Filter Type is 0x00, no fields follow and there is no
   Location Filter"; changelog A.1 records that the explicit type replaced "the length-inferred
   encoding (#1913, #1953)". §3.4 is stale text. The vectors follow §9.20.9: "no filter" is
   type 0x00 (`21 00`), never an empty value.
6. **Fetch End of Range: is Fig. 29's `Object Payload Length` present? — ambiguous, both
   provided.** §11.4.1.2 says "the Group ID and Object ID fields are present. Subgroup ID,
   Priority and Properties are not present" and is silent on Payload Length, which Fig. 29
   lists unbracketed for "Each Object". Primary (`fetch_object_eor_timed_out.bin`): absent —
   an indicator is contrasted with an Object and §11.4.1.2 reads as the complete field list.
   Alternate (`…_alt_payload_length.bin`): a trailing `00`.
7. **§9.20.9's figure labels its first field "Parameter Type (vi64) = 0x21"** while §9.20
   defines the first field of every parameter as a Type Delta. Read as the delta (0x01 when it
   follows SUBSCRIBER_PRIORITY 0x20; 0x21 when first). Likewise the Range-filter figures
   (§9.20.10–14) write `Type (vi64) = 0x25` etc.
8. **Message Parameters vs KVP odd/even rule.** Not a contradiction in the text, but the most
   likely shared misreading: Setup Options (§9.1) and Track/Object Properties (§8.4, §11.1.2)
   use §8.3's odd-type Length rule; Message Parameters (§9.20) do not. LARGEST_OBJECT (odd
   0x09) without a length byte is pinned in six vectors.
9. **Track Properties delta chain.** Read as restarting from 0 after the Parameters (§8.3
   "from 0 if there is no previous Type value" within the KVP sequence; Properties and
   Parameters are different registries). Pinned in `publish_largest_object_props.bin`
   (parameter chain ends at 0x10, first property delta is 0x0E, not 0x0E−0x10).
10. **SETUP type doubles as the control-stream type.** §6.4.1 Table 2 lists 0x2F00 as a
    unidirectional stream type ("SETUP"); §9.1 Fig. 6 gives the message Type 0x2F00. Read as
    one vi64 written once: the control stream begins directly with the SETUP message. The
    `setup_*` vectors are the message; nothing precedes `AF 00` on the stream. Same pattern as
    SUBGROUP_HEADER/FETCH_HEADER, whose first vi64 is both stream type and header type.
11. **PUBLISH "required parameters".** None is unconditional; LARGEST_OBJECT is required only
    "If Objects have been published on this Track" (§9.20.17). `publish_minimal.bin` has no
    parameters (fresh track); `publish_largest_object_props.bin` has the populated case.
12. **Which TRACK_STATUS response?** §9.13 says TRACK_STATUS is answered by TRACK_STATUS_OK,
    which per §1.5 is a REQUEST_OK (0x7) "with the same parameters and Track Properties it
    would have set in a SUBSCRIBE_OK. Track Alias is not used." So the response has no Track
    Alias field (unlike SUBSCRIBE_OK).
13. **§16.7 is missing its "repeatable" column** ("Message Parameters - List which params can
    be repeated in the table." is an unresolved editorial note). From the text,
    AUTHORIZATION_TOKEN (§8.9) and the Range filters (§3.3.2) may repeat, which implies a Type
    Delta of 0 is legal for those; no vector relies on it.
14. **Endianness of the 16-bit Length** is inherited from RFC 9000 §1.3 via §8, not restated
    in the MoQT text (see envelope section above).
15. **REQUEST_UPDATE's Request ID** is the update's own ID (it "consumes a Request ID",
    §6.4.2.1), not the ID of the request being modified; the latter is implied by the stream.
    `request_update_forward_zero.bin` uses 4 as that new ID.
16. **Byte strings.** Namespace fields and Track Names are opaque bytes (§2.4.1); the ASCII
    shown in the derivations is illustrative only.
