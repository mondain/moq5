# Draft-22 implementation and validation

PR 35 targets MoQ Transport draft 22, replacing its pending draft-21 profile
while preserving draft-16 and draft-18 support. The existing PR branch remains
`feature/draft21` so the review stays on the same pull request.

## Source of truth

Use [draft-ietf-moq-transport-22.txt](draft-ietf-moq-transport-22.txt) first.
The text matches the moqxr and contribution interop runner copies, with SHA-256
`1a725f10db526d94f915b30db9004821d86e57afcdde10789c54fee64b3056b5`.
The [wire reference](draft22-wire-reference.md) describes the relevant layouts.

## Implementation scope

- Register draft 22 as the newest profile and offer `{22,18,16}` through the
  shared endpoint version list, native QUIC ALPN and WebTransport protocols.
- Use explicit LOCATION_FILTER types 0 through 5 (9.20.9), also inside
  FILL_PARAMETERS. Preserve absolute `{0,0}` and Next Object as distinct values.
- Resolve relative filters and fills from per-track Largest Object history.
  Reject Next Object FETCH requests at the integer ceiling and clamp relative
  starts, including Next Group, as required by 9.20.9.
- Enforce binary namespace field and aggregate byte limits (8.7), and pass
  opaque token payloads to the application (8.9). Keep token-cache transactions
  and duplicate-token checks.
- Carry forward the request, fill, timeout and data-plane work, integrating
  current main's request reset, held-input and lifecycle changes.
- Update public headers, C++/Swift bindings, examples and the maintained
  contribution adapter to use draft 22.

## Validation workflow

Build core/service with warnings as errors and run all CTest tests. Repeat with
ASan/UBSan and with shared-library/C++ bindings. Build the pinned picoquic native
QUIC and WebTransport adapters and run their tests with loopback networking.
The dependency setup self-test checks patch idempotence and compiler policy.

Run the contribution tester's draft-22 catalog audit, then live driven contexts
using `tools/interop-adapter/run.sh` and the freshly built `media_send` executable.
Exercise native QUIC and WebTransport separately, including typed Location Filter
and malformed-input cases. Retain exact outcomes and gaps in
[conformance.md](conformance.md); a catalog audit is not a live publisher result.
The adapter publishes placeholder access units and ignores its media fixture,
so those results do not demonstrate rendered-media delivery.

## Remaining implementation limits

The per-object delivery clock is not implemented. FILL_TIMEOUT is surfaced but
has no relay waiting implementation. Range Filters are declined. Adapter-specific
validation depends on the available dependency builds; Swift runtime tests need
a Swift toolchain. These limits must remain explicit in the conformance report.
