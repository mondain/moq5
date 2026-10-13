# D16 Conformance Status

Factual status of draft-ietf-moq-transport-16 support in libmoq.

## Control Messages

| Message | Codec | Session API | Scenario | Notes |
|---------|-------|-------------|----------|-------|
| CLIENT_SETUP / SERVER_SETUP | Full | Full | lifecycle, auth | Request capacity, auth token cache negotiation |
| SUBSCRIBE | Full | Full | lifecycle, object, backpressure | All filter types; params validated per scope |
| SUBSCRIBE_OK | Full | Full | lifecycle | LARGEST_OBJECT, EXPIRES, track properties |
| REQUEST_ERROR | Full | Full | Multiple | Shared across all request families |
| REQUEST_OK | Full | Full | track_status, fetch | Per-target param validation |
| UNSUBSCRIBE | Full | Full | lifecycle | Publisher resets open subgroups |
| REQUEST_UPDATE | Full | Full | subscribe, fetch, publish | Subscription + PUBLISH: auto-accept priority/forward/timeout with outbound API + tombstones. Other targets: REQUEST_ERROR(NOT_SUPPORTED). |
| PUBLISH | Full | Full | publish | FORWARD enforcement, alias collision prevention |
| PUBLISH_OK | Full | Full | publish | Subscriber priority, group order, delivery timeout, expires |
| PUBLISH_DONE | Full | Full | publish | Status codes including UPDATE_FAILED |
| FETCH | Full | Full | fetch | Standalone + relative/absolute joining |
| FETCH_OK | Full | Full | fetch | End location validation, track properties |
| FETCH_CANCEL | Full | Full | fetch | Tombstone for late data race |
| TRACK_STATUS | Full | Full | track_status | Stateless; LARGEST_OBJECT/EXPIRES on OK |
| GOAWAY | Full | Full | goaway | Drain timeout, new-session URI |
| MAX_REQUEST_ID | Full | Full | request_credit | Via grant_request_capacity API |
| REQUESTS_BLOCKED | Full | Full | request_credit | Event: REQUEST_READY |
| PUBLISH_NAMESPACE | Full | Full | namespace_sub | Auth token staging |
| PUBLISH_NAMESPACE_DONE | Full | Full | namespace_sub | — |
| PUBLISH_NAMESPACE_CANCEL | Full | Full | namespace_sub | — |
| SUBSCRIBE_NAMESPACE | Full | Full | namespace_sub | Bidi stream, auth, forward |
| NAMESPACE | Full | Full | namespace_sub | Via send_namespace API |
| NAMESPACE_DONE | Full | Full | namespace_sub | Via send_namespace_done API |

## Data Plane

| Feature | Codec | Session API | Scenario | Notes |
|---------|-------|-------------|----------|-------|
| Subgroup streams (ZERO/FIRST_OBJ/PRESENT) | Full | Full | object, streaming_object | Incremental parser with chunked delivery |
| Object properties/extensions | Full | Full | object, streaming_object | Owned rcbuf on events |
| Object status (NORMAL/END_OF_GROUP/END_OF_TRACK) | Full | Full | object | — |
| Streaming objects (OBJECT_CHUNK) | Full | Full | streaming_object | begin/end/write_data API |
| Fetch data stream (FETCH_HEADER + objects) | Full | Full | fetch | Objects, gaps, FIN completion |
| Object datagrams | Full | Full | object_datagram | Payload, properties, status; budget-accounted |
| Delivery timeout (subgroup reset) | Full | Full | streaming_object | Per-subscription and per-update timeout |

## Auth / Security

| Feature | Status | Notes |
|---------|--------|-------|
| AUTH_TOKEN inbound processing | Full | Cache with register/delete/use, staged transactions |
| AUTH_TOKEN outbound sending | Full | All request types: subscribe, fetch, publish, publish_namespace, track_status |
| Setup auth token | Full | Processed and cached on both sides |

## Known Limitations

- **REQUEST_UPDATE**: Auto-accepts priority, forward, and delivery timeout for subscriptions and PUBLISH-created subscriptions. Outbound update API for both subscription and publication sides. Pending update tombstones across PUBLISH_DONE and UNSUBSCRIBE. Filter, auth token, new group request, and subscription filter params are rejected as unsupported. Other targets (fetch, namespace, track-status) return REQUEST_ERROR(NOT_SUPPORTED).
- **d18 profile**: Implemented in the tree (see the draft 22 section below for the newest profile and the interop runner results).
- **QUIC transport**: Sans-I/O core. In-tree adapters: picoquic (raw QUIC), mvfst (raw QUIC), picoquic WebTransport, and proxygen WebTransport. The WebTransport adapters are experimental.
- **Facades**: Publisher and subscriber facades exist; relay facade not started.
- **Joining fetch filter**: Only LARGEST_OBJECT filter is valid per spec; client and server both enforce this.

## Test Infrastructure

| Layer | Count | Coverage |
|-------|-------|----------|
| Unit tests (CTest) | 52 sim + 31 nosim | Codec, session, vectors |
| Interop vectors | 33 binary fixtures | Decode + byte-identical re-encode |
| Seeded scenarios | 20 runners x 1000 seeds | Deterministic trace hash verification |
| Boundary checks | 2 scripts | API vocabulary + profile vocabulary |
| OOM sweep | 14 scenarios | Fail-at-N allocator exhaustive |


## Draft 22 (draft-ietf-moq-transport-22)

The endpoint offers `{22,18,16}`. Draft 22 replaces the pending draft-21 profile.
The authority is [draft-ietf-moq-transport-22.txt](draft-ietf-moq-transport-22.txt),
with the [wire reference](draft22-wire-reference.md) and
[implementation notes](draft22-implementation-plan.md) describing the mapping.

| Area | Implementation |
|---|---|
| Negotiation and control | Draft-22 ALPN/protocol, SETUP, request streams, REQUEST_OK, PUBLISH_STATE_NOTIFY, PUBLISH_SKIPPED and GOAWAY |
| Location Filter | Explicit types 0 through 5, relative starts, absolute origin, inclusive end object, overflow and unknown-type rejection |
| FETCH and fills | Standalone FETCH; SUBSCRIBE/REQUEST_UPDATE fills; exact history resolution; Next Object FETCH rejection and relative-start clamping |
| Names | Binary fields, 0..32 namespace fields, nonempty individual fields, 4096-byte aggregate limits |
| Authorization | Opaque token payloads; alias registration/use/delete, transactional cache changes, duplicate resolved token checks |
| PUBLISH | Non-default accepted parameters sent as REQUEST_UPDATE; initial parameters queryable |
| Data | Subgroup/object layouts, FIRST_OBJECT and FETCH End of Range markers |
| Range Filters | Declined with INVALID_FILTER; MAX_FILTER_RANGES defaults to zero |
| Delivery timeouts | Subgroup timer starts at FIN; per-object clock remains unimplemented |
| FILL_TIMEOUT | Surfaced; relay waiting remains unimplemented |
| Bindings | C++ and Swift version mappings and public codec headers updated |

### Validation on 2026-10-13

| Configuration | Result |
|---|---|
| Core/service, warnings as errors | 193/193 CTest |
| ASan/UBSan with leak detection | 193/193 CTest; draft-22 allocator failure sweep passed |
| Shared libraries and C++ binding | 211/211 CTest |
| Pinned picoquic, native QUIC and WebTransport | 279/279 CTest |
| Pinned MsQuic | 229/229 CTest |
| Swift | 386/386 tests when run serially; the parallel run had 12 timing failures |
| Fuzzing | Draft-22 control codec and session-control targets completed 30 seconds each with ASan/UBSan |
| Dependency setup | Patch-list/idempotence and picotls compiler-policy self-tests passed |

The contribution tester was freshly built from `ae0364dfe013610ef4e399efae82ffb37a291f42`;
its previous binary identified the older `28b2974` source commit. Draft-22 audit:
170/170 required executable coverage, 3/97 optional coverage, static gate passed.
These are catalog/instrument checks, not a publisher conformance score.

The full follow-up sweep tested publisher commit `c53bf1435bdf8a8c60efd4a6f431dad3f34c0a32`
with an isolated copy of the tester's bundled `adapters/moq5` harness. Its only
invocation change was accepting draft 22 in place of draft 21; the command-line
mapping and placeholder publisher behavior were retained. The adapted harness's
contract test passed. Every advertised draft-22 driven profile ran (215 per
transport), plus the two unscored Location Filter probes. Six special namespace
fixtures per transport were supplied after the default fixture was rejected.

The sweep and follow-ups finalized 528 runs across 650 established connections
(325 per transport). Follow-ups included 45 groups of companion raw-probe
scenarios per transport and repeated duplicate request-stream GOAWAY probes.
There were 16 FAIL run verdicts and 512 INCOMPLETE, with no ERROR verdicts.
Across these separate runs, 58 distinct rows scored PASS over native QUIC and
60 over WebTransport; five distinct rows scored FAIL on each transport. These
counts describe observed rows, not an aggregated conformance score.

| Failed requirement | Draft section | Observed behavior on both transports |
|---|---|---|
| D22-9-2-MUST-339 | 9.2 | Duplicate GOAWAY on an established SUBSCRIBE_NAMESPACE stream resets that request but leaves the session serving fresh SUBSCRIBE requests; four reproductions per transport |
| D22-2-4-3-MUST-NOT-028 | 2.4.3 | Application namespace `.interop-unregistered` is published without an IANA-defined meaning |
| D22-2-4-3-MUST-NOT-030 | 2.4.3 | Namespace whose first field is `.` is used |
| D22-2-4-3-MUST-NOT-032 | 2.4.3 | Namespace whose first field is `.` is published |
| D22-6-5-MUST-NOT-181 | 6.5 | Application namespace whose first field is `.session` is published |

The GOAWAY result agrees with the publisher-side namespace-subscription path:
post-request bytes enter request-local teardown instead of the profile's GOAWAY
parser (`core/src/session/session_namespace_sub.c`). Reserved namespace results
identify missing validation of application publication configuration; the draft
still requires unrecognized reserved incoming requests to reach the Application.
Neither defect was repaired by this testing pass.

The tester's execution audit returned failure for two `scored_without_binding`
observations: `D22-9-1-1-MUST-304` in `d22-webtransport-server-sends-authority` and
`D22-9-1-2-MUST-311` in `d22-webtransport-server-sends-path`. These are tester
binding errors. Excluding those observations leaves the distinct PASS counts
above unchanged because both rows also passed in registered companion contexts.
Twelve contexts hit the tester's evidence limit and remain unscored. Other rows
require publisher modes, object layouts, or token-cache/policy configuration
that the placeholder application does not provide. Ordinary write probes
correctly record `operation_accepted=false`; this flag acknowledges STOP_SENDING
operations and is not a reason to classify ordinary writes as unaccepted.

The retained run requests, JSON/TAP exports, driver logs, executable hashes,
identity record and execution audit are in `/tmp/moq5-pr35-harness-test/` on the
validation host. The runner checkout was left unchanged.

The adapter ignores its media fixture and sends placeholder access units. These
runs do not establish rendered-media delivery. The live WebTransport run exposed
an omitted port in CONNECT `:authority`; the client now uses its connect host
and port independently of its TLS server name.
