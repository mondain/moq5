#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
build="$root/build/d22-swift-compatible/compatibility"
phase=${1:-green}
case "$phase" in red|green) ;; *) exit 2 ;; esac
mkdir -p "$build/modules/CMoQCore"
printf 'module CMoQCore {\n header "%s"\n header "%s"\n export *\n}\n' \
    "$root/core/include/moq/session.h" "$root/core/include/moq/rcbuf.h" \
    > "$build/modules/CMoQCore/module.modulemap"
flags=(-swift-version 5 -warnings-as-errors -I "$build/modules"
       -module-cache-path "$build/cache" -Xcc -Werror)
swiftc "${flags[@]}" -package-name LibMoQ -emit-module -module-name MoQ \
    -emit-module-path "$build/modules/MoQ.swiftmodule" \
    "$root/bindings/swift/Sources/MoQ/Action.swift" \
    "$root/bindings/swift/Sources/MoQ/Buffer.swift" \
    "$root/bindings/swift/Sources/MoQ/MoQError.swift" > "$build/$phase-model.log" 2>&1
shasum -a 256 "$root"/bindings/compatibility/*.swift > "$build/$phase-before.sha256"
for fixture in ResetConstruction ResetPattern ActionSwitch; do
    rc=0
    swiftc "${flags[@]}" -package-name UnchangedClient -typecheck \
        "$root/bindings/compatibility/$fixture.swift" \
        > "$build/$phase-$fixture.log" 2>&1 || rc=$?
    printf '%s %s exit=%s\n' "$phase" "$fixture" "$rc"
    if [ "$phase" = green ]; then
        test "$rc" -eq 0
    else
        test "$rc" -eq 1
        case "$fixture" in
            ResetConstruction) expected="missing argument for parameter 'reliableSize'" ;;
            ResetPattern) expected='type of expression is ambiguous without a type annotation' ;;
            ActionSwitch) expected='switch must be exhaustive' ;;
        esac
        rg -q "$expected" "$build/$phase-$fixture.log"
        if rg 'error:' "$build/$phase-$fixture.log" | rg -v "$expected"; then exit 1; fi
    fi
done
if rg 'warning:' "$build/$phase-"*.log; then exit 1; fi
shasum -a 256 "$root"/bindings/compatibility/*.swift > "$build/$phase-after.sha256"
cmp "$build/$phase-before.sha256" "$build/$phase-after.sha256"
if [ "$phase" = green ] && [ -d "$root/build/d22-swift-compatible/swift-source" ]; then
    full_module=$(rg --files --no-ignore "$root/build/d22-swift-compatible/swift-source" -g MoQ.swiftmodule)
    test -n "$full_module"
    full_modules=$(dirname "$full_module")
    for fixture in ResetConstruction ResetPattern ActionSwitch; do
        swiftc -swift-version 5 -warnings-as-errors -Xcc -Werror \
            -I "$full_modules" -I "$root/build/d22-swift-compatible/package-source/CMoQCore" \
            -module-cache-path "$build/full-cache" -package-name UnchangedClient \
            -typecheck "$root/bindings/compatibility/$fixture.swift" \
            > "$build/full-$fixture.log" 2>&1
        printf 'full-module %s exit=0\n' "$fixture"
    done
fi
