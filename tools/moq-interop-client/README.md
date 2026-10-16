# moq-interop-client

MoQ Transport interop test runner. Connects to a relay and runs
named test cases, reporting results in TAP format.

## Role: protocol conformance, not a consumer example

This is a low-level **protocol/conformance** tool. It uses the service-tier
endpoint (`moq_endpoint_t`) only for transport selection and version
negotiation, then drives the sans-I/O core (`moq_session_*`) directly through
the endpoint's `post()` executor so it can exercise and assert exact wire-level
behavior across implementations. It is **not** an example of how to integrate
libmoq into an application, and it does **not** use the high-level media
surface (`moq_media_receiver_t` / `moq_media_sender_t`).

If you are building a media consumer (player, encoder, plugin), start with
`examples/service/` instead — that is the supported high-level API. A
*service-level* interop client, should we need one, belongs in a separate
tool/example so this conformance runner stays focused on protocol behavior.

## Current status

All six interop tests implemented.

## Build (local)

Build trees live under `build/` (see the top-level `CMakePresets.json`):

```
cmake -B build/interop \
    -DMOQ_BUILD_ADAPTER_PICOQUIC=ON \
    -DMOQ_BUILD_PQ_THREADED=ON \
    -DMOQ_BUILD_INTEROP_CLIENT=ON \
    -DMOQ_PICOQUIC_SOURCE_DIR=/path/to/picoquic \
    -DMOQ_PICOTLS_PREFIX=/path/to/picotls/build
cmake --build build/interop --target moq-interop-client
```

## Build (Docker)

Build context must be the parent directory containing `libmoq/`,
`picoquic/`, and `picotls/` as siblings:

```
cd /path/to/parent
docker build -f libmoq/tools/moq-interop-client/Dockerfile -t moq-interop-client .
```


## Usage

```
# Run one named case:
moq-interop-client --relay moqt://relay.example.com:4443 --test setup-only

# Run the whole suite (no --test / TESTCASE): one TAP line per case.
moq-interop-client --relay moqt://relay.example.com:4443

# Pin the offered draft to 16, 18 or 22 (default: auto-negotiate, offer all):
moq-interop-client --relay moqt://relay:4443 --draft 18

# With TLS verification disabled (for test relays with self-signed certs):
moq-interop-client --relay moqt://localhost:4443 --test setup-only --tls-disable-verify

# Via environment (for CI/Docker):
RELAY_URL=moqt://relay:4443 TESTCASE=setup-only MOQT_DRAFT=16 TLS_DISABLE_VERIFY=1 moq-interop-client
```

CLI flags override environment variables. With no test named (neither `--test`
nor `TESTCASE`) the client runs every case and exits 0 as long as it ran;
per-case pass/fail is in the TAP. `--draft` / `MOQT_DRAFT` must be exactly `16`,
`18` or `22`.

## Backend selection (testing)

`--backend NAME` (or `MOQT_BACKEND`; the flag wins over the environment) pins
the transport backend so the same suite can exercise libmoq's own adapters. It
is a testing knob, not part of the interop-runner interface, and defaults to
`auto` (the picoquic family). Each backend is transport-restricted; the client
cross-checks the name against the URL scheme and rejects a mismatch up front:

| `--backend` | Transport | Notes |
|---|---|---|
| `auto` | raw QUIC + WebTransport | picoquic family (default) |
| `picoquic` | raw QUIC + WebTransport | |
| `msquic` | raw QUIC only | exact-version — pin `--draft 16`, `18` or `22` (a single ALPN) |
| `mvfst` | raw QUIC only | exact-version; when compiled in |
| `proxygen` | WebTransport only | when compiled in |
| `wtquic-msquic` | WebTransport only | WebTransport over MsQuic (cross-platform) |
| `wtquic-network` | WebTransport only | **experimental** — WebTransport over Apple Network.framework; unreliable against eager third-party relays |

Bare `wtquic` is **not** accepted (it names no upstream transport). A backend
that is not compiled into the build is rejected at connect with
`MOQ_ERR_UNSUPPORTED` — the resolver never silently falls back to picoquic.

The TAP stream names the exercised backend, WebTransport profile, and draft:
`# request: backend=… wt-profile=… draft=…` up front (visible even when the
connection fails), and `# negotiated: draft-N over TRANSPORT (backend: …)` once
setup completes.

### WebTransport profile (`--wt-profile`)

`--wt-profile NAME` (or `MOQT_WT_PROFILE`; the flag wins) selects the
WebTransport-over-HTTP/3 wire dialect for the **wtquic-msquic** backend. The two
dialects are mutually exclusive and never auto-negotiated:

| `--wt-profile` | extended CONNECT `:protocol` | peers |
|---|---|---|
| `current` (default) | `webtransport-h3` (current WebTransport-H3 draft) | e.g. imquic |
| `d13-14` | `webtransport` (drafts 13/14 + max-sessions signal) | proxygen/moxygen/moqx and the picoquic h3zero family |

It is **rejected** for any other backend (`picoquic` has a fixed WebTransport
dialect; raw-QUIC `msquic`/`mvfst` have no WebTransport) rather than silently
ignored. In the TAP `# request:` identity, those backends report their truthful
dialect — `native` for a fixed-dialect WebTransport backend, `n/a` for raw QUIC —
never a `current`/`d13-14` they did not speak.

```
# Direct MsQuic, raw QUIC, draft 18:
moq-interop-client --backend msquic --draft 18 --relay moqt://relay:4443 --test setup-only

# WebTransport over MsQuic against a proxygen/moxygen/moqx-style relay:
moq-interop-client --backend wtquic-msquic --wt-profile d13-14 --relay https://relay:4443/ --test setup-only
```

To build a native matrix binary that has every backend compiled in, enable the
adapters explicitly (each requires its dependency): `MOQ_BUILD_PQ_THREADED`,
`MOQ_BUILD_PICO_WT_MANAGED`, `MOQ_BUILD_MSQUIC_MANAGED`,
`MOQ_BUILD_WTQUIC_MSQUIC_MANAGED`, `MOQ_BUILD_WTQUIC_NETWORK_MANAGED`, alongside
`MOQ_BUILD_SERVICE` and `MOQ_BUILD_INTEROP_CLIENT`.

### Docker

```
# CLI args:
docker run --rm moq-interop-client \
    --relay moqt://relay:4443 --test setup-only --tls-disable-verify

# Environment:
docker run --rm \
    -e RELAY_URL=moqt://relay:4443 \
    -e TESTCASE=setup-only \
    -e TLS_DISABLE_VERIFY=1 \
    moq-interop-client
```

## URL format

- `moqt://host:port` — raw QUIC (ALPN `moqt-16` / `moqt-18` / `moqt-22`, negotiated)
- `moqt://[::1]:port` — IPv6 literal
- `https://host:port/path` — WebTransport with an explicit CONNECT path
- `https://host:port` — WebTransport at `/`, for relays that mount the root
  (with no path in the URL the library default `/moq` would be wrong)

## TAP output

Results go to stdout in [TAP version 14](https://testanything.org/)
format. All logs go to stderr.

Pass:
```
TAP version 14
1..1
ok 1 - setup handshake
  ---
  duration_ms: 42
  message: "MoQ setup complete"
  ...
```

Fail:
```
TAP version 14
1..1
not ok 1 - setup handshake
  ---
  duration_ms: 3000
  message: "timeout"
  ...
```

Unknown test:
```
TAP version 14
1..1
ok 1 - unknown-test # SKIP unknown test case
```

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Named test passed; or run-all completed (per-case pass/fail is in the TAP) |
| 1 | Named test failed, run-all ran no case, or bad arguments |
| 127 | Unsupported or unknown test case |

## Supported tests

| Test | Status |
|------|--------|
| `setup-only` | Implemented |
| `announce-only` | Implemented |
| `publish-namespace-done` | Implemented |
| `subscribe-error` | Implemented |
| `announce-subscribe` | Implemented |
| `subscribe-before-announce` | Implemented |

## Named-only case: `object-relay` (opaque object conformance)

`object-relay` runs only when named (`--test object-relay`), never in the run-all
suite, because it needs a relay that forwards Objects. It opens two
connections to the relay:

1. The publisher announces a namespace unique to the run
   (`moq-test/object-relay/<pid>-<clock>`).
2. The subscriber subscribes to `orl-track` in that namespace through the relay.
3. The publisher accepts the forwarded SUBSCRIBE. Once the subscriber holds
   SUBSCRIBE_OK, the publisher writes 3 groups of 4 Objects, each group a
   subgroup that it closes with a FIN.
4. The publisher ends the subscription with PUBLISH_DONE TRACK_ENDED.

**What the subscriber checks:** every Object's group, subgroup and Object ID,
its status, its length and every payload byte, against a sequence derived only
from (group, object). Every payload length is distinct and nonzero. Also
required:

- each subgroup's FIN comes after that group's tail;
- the PUBLISH_DONE comes after all of them;
- no duplicate, extra, missing or reordered Object.

Every wait is bounded, and a timeout is a failure. The TAP names the
negotiated draft and transport for both connections.

**Scope:** the payloads are opaque bytes. A pass says nothing about catalog
discovery, codecs or playback.

**Fault injection:** `MOQ_INTEROP_OBJECT_RELAY_FAULT=corrupt-byte|drop-object|extra-object`
makes the publisher deviate from the sequence. The subscriber's unchanged
verifier must then fail step 7. The verifier also has a hermetic unit test
(`interop_object_relay_verify`).

`MOQ_INTEROP_OBJECT_RELAY_DONE_DELAY_MS=N` is diagnostic. It holds the
publisher's PUBLISH_DONE back by N ms after the last FIN, which separates a
relay that ends its downstream before forwarding the tail from a tail that was
never sent. A passing run never sets it.

**Running it against an owned relay:**
`scripts/run_object_relay.sh` starts one `moq5-relay` on loopback with:

- a fresh CA and server certificate;
- free ports;
- both listeners offering drafts 22, 18 and 16.

It runs the case for each requested draft over raw QUIC and WebTransport, and
stops the relay. The client trusts only the run's CA, through `SSL_CERT_FILE`,
with verification on.

```
tools/moq-interop-client/scripts/run_object_relay.sh \
    --relay-bin build/tools/moq5-relay/moq5-relay \
    --client-bin build/tools/moq-interop-client/moq-interop-client \
    --out object-relay-run --drafts "22 18" --faults
```
