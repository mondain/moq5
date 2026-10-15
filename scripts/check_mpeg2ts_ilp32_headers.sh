#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CC="${CC_ILP32:-clang}"
INCLUDES=()
for dir in core/include media/loc/include media/object/include media/msf/include \
           media/cmaf/include media/mpeg2ts/include service/include; do
    INCLUDES+=("-I$ROOT/$dir")
done

status=0
for target in armv8m.main-none-eabi i686-linux-gnu; do
    for mode in normal short; do
        flags=(-std=c11 -ffreestanding -fsyntax-only
               -Wall -Wextra -Wpedantic -Werror)
        if [ "$mode" = short ]; then
            flags+=(-fshort-enums)
        fi
        if "$CC" --target="$target" "${flags[@]}" "${INCLUDES[@]}" \
            "$ROOT/tests/unit/test_abi_layout_mpeg2ts.c"; then
            echo "PASSED: MPEG-TS layouts for $target ($mode enums)"
        else
            echo "FAILED: MPEG-TS layouts for $target ($mode enums)"
            status=1
        fi
    done
done
exit "$status"
