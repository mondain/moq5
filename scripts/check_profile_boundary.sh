#!/usr/bin/env bash
#
# Verify that generic session core files do not directly reference
# draft-16 request-admission machinery. These belong behind profile
# ops (profile_d16.c) or in codec/wire layers.
#
# This enforces the invariant that generic session code asks the
# profile to prepare/commit/validate/release requests;
# only the profile knows how that maps to draft-16 request IDs or
# future stream-bound requests.
#
# Exit 0 if clean, 1 if violations found.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Session core files (auto-discovered) plus internal helpers.
# Profile implementation files are excluded — they are allowed to
# use D16 wire vocabulary.
SESSION_CORE=()
for f in "$ROOT"/core/src/session/session*.c "$ROOT"/core/src/session/session_internal.h; do
    [ -f "$f" ] || continue
    SESSION_CORE+=( "${f#"$ROOT/"}" )
done
SESSION_CORE+=( core/src/internal/validate.h )

# The request-stream engine owns request-ID admission for the profiles that
# share it, so the admission-field rule does not apply to it; the draft-
# vocabulary rule does.
ENGINE_FILES=( core/src/session/request_stream.c core/src/session/request_stream_internal.h )

# Request-admission terms that must not appear in session core.
# These are D16-specific fields/constants moved behind profile ops.
# D16 wire vocabulary that must not appear in session core.
FORBIDDEN='next_local_request_id|peer_next_request_id|requests_blocked_at|request_was_blocked|has_max_request_id|\.max_request_id|MOQ_SETUP_PARAM_|moq_kvp_entry_t|MOQ_MSG_PARAM_|MOQ_AUTH_TOKEN_|moq_d16_|MOQ_D16_|moq_control_envelope_t|moq_control_decode_envelope|#include.*moq/control(_d[0-9]+)?\.h'

# Any draft's vocabulary, not only D16: session core asks the profile for a
# behaviour, never for a draft number. A draft-suffixed capability, a
# draft-named symbol, a version constant, or a comparison of the profile
# pointer itself are all violations.
DRAFT_FORBIDDEN='profile->[a-z_]*d(1[0-9]|2[0-9])\b|moq_d(1[0-9]|2[0-9])_|MOQ_D(1[0-9]|2[0-9])_|_d(1[0-9]|2[0-9])\b|MOQ_VERSION_DRAFT_|s->profile[[:space:]]*[!=]=|[!=]=[[:space:]]*s->profile([^-]|$)|#include.*control_d[0-9]+'
FORBIDDEN="$FORBIDDEN|$DRAFT_FORBIDDEN"

# Lines calling a profile op or defining struct fields are allowed. A
# capability read (no call parenthesis) stays subject to the draft-name rule.
# Comment lines may name files and codecs; the session default version is the
# one place the core legitimately spells a version number.
EXEMPT='profile-boundary-exempt|s->profile->[a-z_0-9]+\(|typedef struct|bool[[:space:]]*has_max_request_id;|uint64_t[[:space:]]*max_request_id;|^[0-9]+:[[:space:]]*(/\*|\*)|moq_version_t[[:space:]]+version[[:space:]]*=[[:space:]]*MOQ_VERSION_DRAFT_'

violations=0

for file in "${SESSION_CORE[@]}"; do
    path="$ROOT/$file"
    if [ ! -f "$path" ]; then
        continue
    fi
    hits=$(grep -nE "$FORBIDDEN" "$path" 2>/dev/null \
        | grep -vE "$EXEMPT" || true)
    if [ -n "$hits" ]; then
        echo "VIOLATION in $file:"
        echo "$hits" | sed 's/^/  /'
        violations=$((violations + 1))
    fi
done

for file in "${ENGINE_FILES[@]}"; do
    path="$ROOT/$file"
    [ -f "$path" ] || continue
    hits=$(grep -nE "$DRAFT_FORBIDDEN" "$path" 2>/dev/null | grep -vE "$EXEMPT" || true)
    if [ -n "$hits" ]; then
        echo "VIOLATION in $file:"
        echo "$hits" | sed 's/^/  /'
        violations=$((violations + 1))
    fi
done

# Capability fields describe behaviour. A field named after a draft is a
# version test in disguise.
cap_hits=$(grep -nE '^[[:space:]]+(bool|uint[0-9]+_t|size_t)[[:space:]]+[a-z_0-9]*(d[0-9]{2}|draft)[a-z_0-9]*;' \
    "$ROOT/core/src/session/profile.h" 2>/dev/null || true)
if [ -n "$cap_hits" ]; then
    echo "VIOLATION in core/src/session/profile.h (draft-named capability):"
    echo "$cap_hits" | sed 's/^/  /'
    violations=$((violations + 1))
fi

# A profile's operation table is reached from any thread (version queries,
# profile lookup at session create), so it is a constant initializer, never a
# writable table filled in on first use.
table_hits=$(grep -nE '^[[:space:]]*(static[[:space:]]+)?moq_profile_ops_t[[:space:]]+[A-Za-z_0-9]+[[:space:]]*(;|=|\[)' \
    "$ROOT"/core/src/session/profile_*.c 2>/dev/null || true)
if [ -n "$table_hits" ]; then
    echo "VIOLATION: writable profile operation table (declare it static const):"
    echo "$table_hits" | sed 's/^/  /'
    violations=$((violations + 1))
fi

echo ""
if [ "$violations" -gt 0 ]; then
    echo "FAILED: $violations file(s) reference draft-specific vocabulary."
    echo ""
    echo "Session core must not use any draft's wire types, constants, codecs,"
    echo "version numbers, or draft-named capabilities. Draft-specific logic"
    echo "belongs in the profile files behind moq_profile_ops_t."
    echo "Session core must not include moq/control.h."
    exit 1
else
    echo "PASSED: No draft-specific vocabulary in session core."
    exit 0
fi
