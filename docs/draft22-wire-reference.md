# Draft-22 wire reference

The authority is [draft-ietf-moq-transport-22.txt](draft-ietf-moq-transport-22.txt).
Section references below refer to that text. moqxr and the contribution interop
runner are implementation references and test instruments.

## Negotiation and request framing

Native QUIC uses `moqt-22`; WebTransport negotiates the same MoQ version through
its application protocol. The supported transport drafts are 22, 18 and 16,
offered in that order. Draft 21 is replaced by draft 22.

Control messages use vi64 Type, a 16-bit Length and the message body. SETUP
(type 0x2F00) opens the unidirectional control channel. Requests use separate
bidirectional streams, with even client Request IDs and odd server Request IDs
(6.3). Responses are correlated by stream. A requester's FIN, with the initial
request or later, does not cancel it (6.4.2.2); RESET_STREAM and STOP_SENDING
cancel it (6.4.2.3).

## Location Filter (9.20.9)

Parameter type 0x21 is followed directly by a vi64 Location Filter Type.
There is no byte-length prefix. The type selects the optional vi64 fields:

| Type | Fields | Meaning |
|---|---|---|
| 0 | none | No filter; remove a previous filter on REQUEST_UPDATE |
| 1 | StartGroup | Relative start at `{Largest.Group + 1 - StartGroup, 0}` |
| 2 | StartGroup, StartObject | Absolute start, including `{0,0}` |
| 3 | StartGroup, StartObject, EndGroupDelta | Absolute range including the whole end group |
| 4 | StartGroup, StartObject, EndGroupDelta, EndObject | Absolute range with inclusive end object |
| 5 | none | Next Object |

Unknown types cause PROTOCOL_VIOLATION. Relative starts clamp to the vi64 range.
`StartGroup + EndGroupDelta` overflow causes PROTOCOL_VIOLATION. Omitting an end
makes subscriptions open-ended; FETCH and fill ranges end at Largest Object.

The semantic `MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT` API selects Next Object and
encodes type 5. Absolute `{0,0}` encodes type 2. The core preserves the exact
filter beside the older four-type semantic projection so relative starts and
inclusive end-object bounds are enforced precisely.

## Fill streams and FETCH

FILL_PARAMETERS (type 0x23, 9.20.15) contains a byte-length-delimited sequence of
nested parameters, without a parameter-count prefix. Its LOCATION_FILTER uses
the same explicit types above. Its presence requests a fill; its values do not
become persistent subscription state (3.4, 9.5.1).

A fill uses FETCH_HEADER with the SUBSCRIBE or REQUEST_UPDATE Request ID.
Next Object requests have an empty fill range. Type 0 requests the whole track
up to Largest Object. Fill streams count toward PUBLISH_DONE Stream Count (9.9).

FETCH carries its identity and parameters, without the old Joining FETCH fields
(9.11). A request starting beyond Largest Object is INVALID_RANGE; Next Object always meets that condition. Relative Next Group clamps to the
maximum group at the integer ceiling, as required by 9.20.9.
The core API represents an exclusive end object, with zero meaning the entire
end group; the profile translates to the wire's inclusive end object.

## Names and authorization

Namespace fields and Track Names are binary (8.7, 8.8). Namespace tuples have
0 through 32 nonempty fields. Namespace and Full Track Name limits are 4096
bytes, measured by payload bytes, excluding encoded lengths.

Names whose first namespace field starts with `.` are reserved (2.4.3).
Unregistered reserved names cannot be used; the first field `.` is prohibited
for any purpose. `.session` is implementation-owned, and applications must not
publish tracks or namespaces under it (6.5). The application publication APIs
return MOQ_ERR_INVAL for reserved namespaces, including publisher track
registration with namespace advertisement disabled. Dots in later fields and
binary names remain valid. Unrecognized reserved incoming requests must still
reach the application unless another draft rule specifies their treatment.

Token structure errors cause KEY_VALUE_FORMATTING_ERROR. Token payload syntax
belongs to the Token Type (8.9); the draft-22 profile forwards opaque payloads,
including NUL bytes and empty payloads, to the application. Duplicate resolved
(type, value) tokens remain MALFORMED_AUTH_TOKEN. REGISTER aliases survive a
request-level rejection, and USE_ALIAS resolves the registered bytes.

## Other control and data behavior

REQUEST_OK (0x07) is the response to PUBLISH and the other requests without a
dedicated OK type (9.3). Track Properties are empty except on TRACK_STATUS_OK.
PUBLISH_SKIPPED (0x0F) replaces PUBLISH_BLOCKED; PUBLISH_STATE_NOTIFY (0x22)
reports subscription state (9.10, 9.19). GOAWAY carries URI and timeout without
a Request ID (9.2). A request-stream GOAWAY migrates that request. More than one
GOAWAY on the control stream or on the same request stream requires a session
close with PROTOCOL_VIOLATION. Publisher-side namespace subscriptions retain
fragmented GOAWAY bytes and cumulative FIN state across capacity refusals;
an empty retry processes the retained input.

Range Filters are refused with INVALID_FILTER because the
implementation advertises the default MAX_FILTER_RANGES of zero (9.1.6).

The data plane uses draft-18-compatible subgroup/object layouts and includes
End of Range FETCH markers (11.4.1.2). Delivery timeout semantics are in 5.2.
See [conformance.md](conformance.md) for implementation limits and measured
validation; wire support alone does not establish complete conformance.
