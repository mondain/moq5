# CMAF Packager

An optional single-track writer producing initialization bytes and independent
`moof+mdat` chunks. Enable `MOQ_BUILD_CMAF_PACKAGER` with the parent CMake wiring.
Production links `moq::core` publicly and `moq::codec-signaling` privately;
the CMAF parser is used only by the round-trip tests.

The public contract is in `include/moq/cmaf_packager.h`. Codec/sample formats are
explicit; all samples supply durations. Initialization records are checked using
the existing codec-signaling utilities. This is structural validation, not a
decoder or a complete codec conformance validator.
Private shared ASC inspection checks configuration-established rate/channel facts,
not decoder-verified properties; it does not exclude unsignaled implicit SBR.
PCE, SBR/PS, escaped AOT semantics, and uninspected sync-extension tails remain
unknown; RAW AAC accepts unknown properties rather than guessing from core fields.
Opus channels must match the shared-validator-checked dOps channel byte.

Supported entries are `avc1`, `hvc1`, `mp4a`, `av01`, and `Opus`. Recognized
`avc3`/`hev1` carriage returns `MOQ_ERR_UNSUPPORTED`; it is never remapped.
ADTS is limited to one frame matching a canonical two-byte ASC; raw AAC is never
sniffed. AV1/Opus packet syntax is caller-asserted. Caller retains responsibility
for CMSF catalog references, switching alignment, group boundaries and exact SAP.

Samples must be contiguous within a chunk. Gaps between successful chunks are
preserved by tfdt; overlap refuses. Failed writes do not commit the origin,
timeline or sequence. Borrowed fragment output expires on every attempted write,
including failures. Inputs aliasing instance buffers refuse before allocation.

Tests cover original PR defects, all five codecs, exact bytes/box offsets,
timestamp conversion, carriage and NAL-width limits, explicit AAC framing,
allocator failures/retries, config prefix canaries and frozen input usage.
CTest also checks the exact symbol set and installed C/C++ consumers.
The legacy regression mode compiles against the archived original PR header:
`MOQ_PACKAGER_ORIGINAL_REVIEW`. It is not enabled in ordinary CMake builds.

Original writer contribution: Joaquin Bartaburu <joaquinb@quqlabs.com>, PR12,
commit `efa008a71088b7e310cbd9f548e0009b444d203b`. The box writer and useful
conversion structure were retained and reworked to the approved utility contract.
