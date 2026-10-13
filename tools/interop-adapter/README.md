# Interop-runner adapter for the libmoq publisher

`run.sh` lets `moq-contribution-interop-runner` (sibling checkout) start
`examples/service/media_send` as a contribution publisher and score what it puts
on the wire against draft 18 or draft 22. The checked-in drafts, not this
publisher and not the runner, decide what is compliant.

## Use

Use a build configured with the service examples and managed picoquic backends
(`MOQ_BUILD_PQ_THREADED` for native QUIC and `MOQ_BUILD_PICO_WT_MANAGED` for
WebTransport). Build the publisher, create the certificate/key as described in
the tester's `docs/publisher-harness-guide.md`, then start the runner with this
adapter as its driver. Run these commands from the moq5 repository root:

```sh
cmake --build build/dev --target moq_example_media_send
export MOQ5_MEDIA_SEND_BIN="$PWD/build/dev/examples/service/moq_example_media_send"
../moq-contribution-interop-runner/build/moq-interop-runner \
  --bind 127.0.0.1 --port 8080 --database work/runs.sqlite3 \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 4443 --publisher-port-end 4452 \
  --tls-cert "$PWD/work/cert.pem" --tls-key "$PWD/work/key.pem" \
  --driver-executable "$PWD/tools/interop-adapter/run.sh" \
  --driver-fixture /path/to/any/readable/file --driver-log-root work/drv-logs
```

Then create driven runs with `POST /api/v1/runs` as described in the runner's
`docs/publisher-harness-guide.md`. Select `"draft": 22` and
`"transport": "native-quic"` or `"webtransport"`. For example, create a native
QUIC context from another terminal:

```sh
curl --silent --show-error --fail http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' --data-binary '{
    "draft": 22, "transport": "native-quic", "mode": "driven",
    "scenarios": ["d22-publisher-request-stream-placement"],
    "timeout_ms": 12000,
    "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}
  }'
```

Read the returned run ID through `/api/v1/runs/ID` and `/results/ID.json`.
An INCOMPLETE verdict records unscored requirements; it is not a passing
conformance score. At tester revision `ae0364d`, its bundled `adapters/moq5`
accepts only drafts 18 and 21 and refuses 22. Use this repository's `run.sh`
for draft 22; the validated isolated copy of the older harness retained its
command-line mapping while replacing draft 21 selection with 22.

`test-adapter.sh` checks the request-to-command-line mapping and the refusals
without any network (a capture stub replaces the publisher); it is registered with
ctest as `interop_adapter_contract` when `jq` is installed.

## What it maps

| Request field | media_send argument |
|---|---|
| `endpoint` | URL (`moqt://` native QUIC, `https://` WebTransport) |
| `namespace_hex` (fields, joined with `/`) | namespace |
| `track_name_hex` | track name |
| `draft` (18 or 22) | `--draft N` (an exact offer, never a fallback) |
| `tls_ca` | `--ca FILE` |
| `scenario_timeout_ms` | `timeout --signal=INT` (ceiling, in seconds) |

A malformed or unsupported request exits 64 before the publisher starts.
Reaching the scenario timeout is the normal end of a context and exits 0.

## Limits worth knowing

- **The fixture is ignored.** `media_send` publishes placeholder access units in
  30-frame GOPs, attempting 300 frames over about 10 seconds at 30 fps. A scenario that needs a specific object
  layout (for example "two small groups") will not get it from this publisher.
  Score such rows as publisher gaps, then decide whether to add a mode.
- **No per-scenario modes yet.** The moqxr adapter passes scenario-specific
  options so its publisher originates or withholds messages. This adapter does
  not; the roughly 100 draft-22 probes in which the runner acts as the subscriber
  score whatever `media_send` does by default.
- **Both transports were exercised on draft 22.** The pinned picoquic native
  QUIC/WebTransport build established sessions with the current contribution
  tester. The WebTransport client supplies CONNECT authority from its connection
  host and port, independently of TLS SNI.
- **Draft-22 namespace guards and GOAWAY handling are enforced.** Application
  publication APIs reject reserved namespaces with MOQ_ERR_INVAL. Duplicate
  GOAWAY on a namespace-subscription request closes the session with
  PROTOCOL_VIOLATION. The live rerun observed these corrections over both
  transports; some requirements remain unscored because this example exits
  after refusing invalid configuration or lacks the companion publisher mode.
  See [conformance.md](../../docs/conformance.md) for measured results and limits.
- **Name mapping is limited by the example CLI.** Namespace fields must be
  nonempty printable text without `/`; the track name must be nonempty printable
  text. Root namespaces and binary names are exercised through API tests.
