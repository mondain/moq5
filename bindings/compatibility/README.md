# Unchanged Action Clients

These fixed client sources derive from baseline
`9dfdd3073af68edb79efb5f8235dacff8e2c921d`. They must not be migrated to make
candidate binding changes pass. C pins the old 96-byte action envelope and
16-byte reset detail and compiles an old-sized poll consumer. C++ pins reset
decomposition and the ten-alternative visitor. Swift separately pins reset
construction, two-value matching, and the nine-case exhaustive switch.

From the candidate root:

```sh
bash bindings/compatibility/run.sh --expect-imported-breaks
```

This measurement mode asserts the five expected incompatibilities (two C++,
three Swift); exit 0 means their diagnostics were observed, **not**
compatible bindings. To require source compatibility, use the same
unchanged clients:

```sh
bash bindings/compatibility/run.sh --require-compatible
```

Both modes require a warning-clean baseline. Logs, exact commands/statuses,
the minimum 13 baseline headers/model sources and local Swift importer/module
cache stay under `build/d22-binding-compat`. No library is built or linked;
this is source compilation/typechecking, not runtime, installed SDK, binary ABI,
transport, or full binding coverage. Swift needs a local Swift 5.9+ compiler
and Foundation SDK; tested host is macOS. No packages or network are fetched.
The expected-incompatibility diagnostics are pinned to AppleClang 17 /
Swift 6.2.4; other diagnostic versions must be checked by hand, not treated
as matching the expected incompatibilities.
