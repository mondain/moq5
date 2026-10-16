#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
fixtures="$root/bindings/compatibility"
build="$root/build/d22-binding-compat"
baseline=9dfdd3073af68edb79efb5f8235dacff8e2c921d
mode=${1:---require-compatible}
case "$mode" in --require-compatible|--expect-imported-breaks) ;; *) exit 2;; esac

guard() {
    local available
    available=$(df -Pk "$root" | awk 'NR == 2 {print $4}')
    case "$available" in ''|*[!0-9]*) exit 2;; esac
    if [ "$available" -lt 1048576 ]; then
        printf 'resource hold: %s KiB available\n' "$available" >&2
        exit 2
    fi
    printf 'headroom_kib=%s\n' "$available"
}

guard
mkdir -p "$build/baseline"
test "$(git -C "$root" rev-parse "$baseline^{commit}")" = "$baseline"
# Exact minimal include closure and three real Swift model sources, no core build.
paths=(
    core/include/moq/export.h core/include/moq/types.h
    core/include/moq/session.h core/include/moq/rcbuf.h core/include/moq/subscriber.h
    bindings/cpp/include/moq/action.hpp bindings/cpp/include/moq/buffer.hpp
    bindings/cpp/include/moq/result.hpp bindings/cpp/include/moq/types.hpp
    bindings/cpp/include/moq/visit.hpp
    bindings/swift/Sources/MoQ/Action.swift bindings/swift/Sources/MoQ/Buffer.swift
    bindings/swift/Sources/MoQ/MoQError.swift
)
git -C "$root" archive "$baseline" "${paths[@]}" | tar -xf - -C "$build/baseline"
for path in "${paths[@]}"; do
    git -C "$root" show "$baseline:$path" | cmp - "$build/baseline/$path"
done
printf 'baseline=%s\nmode=%s\n' "$baseline" "$mode" > "$build/results.log"
shasum -a 256 "$fixtures"/*.c "$fixtures"/*.cpp "$fixtures"/*.swift > "$build/fixtures-before.sha256"
incompatible=0
failed=0
rc=0

capture() {
    local tag=$1
    shift
    {
        guard
        printf 'command='
        printf '%q ' "$@"
        printf '\n'
    } > "$build/$tag.log"
    if "$@" >> "$build/$tag.log" 2>&1; then rc=0; else rc=$?; fi
    printf '\nexit=%s\n' "$rc" >> "$build/$tag.log"
    if grep -E 'warning:|CMake Warning' "$build/$tag.log"; then
        printf '%s: unexpected warning\n' "$tag" >&2
        exit 3
    fi
}

record() {
    local tag=$1 expected=$2 pattern=${3:-}
    printf '%s exit=%s\n' "$tag" "$rc" | tee -a "$build/results.log"
    if [ "$expected" = green ]; then
        if [ "$rc" -ne 0 ]; then failed=$((failed + 1)); fi
    else
        if [ "$rc" -eq 0 ]; then
            failed=$((failed + 1))
        elif [ "$rc" -eq 1 ] && grep -Eq "$pattern" "$build/$tag.log"; then
            local allowed
            case "$tag" in
                candidate-cpp-reset) allowed='decomposes into 3 elements';;
                candidate-cpp-visitor)
                    allowed='unchanged action_variant has ten alternatives|unchanged unknown action index|visitor to be exhaustive|attempt to use a deleted function|cannot deduce return type'
                    grep -q 'visitor to be exhaustive' "$build/$tag.log" || failed=$((failed + 1));;
                candidate-swift-ResetConstruction) allowed="missing argument for parameter 'reliableSize'";;
                candidate-swift-ResetPattern) allowed='type of expression is ambiguous without a type annotation';;
                candidate-swift-ActionSwitch)
                    allowed='switch must be exhaustive'
                    for missing in openUniControl sendUniControl resetBidiStream stopBidiStream abortBidiStream; do
                        grep -Fq "add missing case: '.$missing(" "$build/$tag.log" || failed=$((failed + 1))
                    done;;
                *) exit 2;;
            esac
            if grep 'error:' "$build/$tag.log" | grep -Ev "$allowed"; then
                printf '%s: additional unexpected diagnostic\n' "$tag" >&2
                failed=$((failed + 1))
            else
                incompatible=$((incompatible + 1))
            fi
        else
            printf '%s: unexpected failure, not an expected incompatibility\n' "$tag" >&2
            failed=$((failed + 1))
        fi
    fi
}

for lane in baseline candidate; do
    source="$root"
    if [ "$lane" = baseline ]; then source="$build/baseline"; fi
    mkdir -p "$build/$lane-out"
    includes=(-I"$source/core/include" -I"$source/bindings/cpp/include")
    expected=green
    if [ "$lane" = candidate ] && [ "$mode" = --expect-imported-breaks ]; then expected=incompatible; fi
    capture "$lane-c-action" clang -std=c11 -Wall -Wextra -Wpedantic -Werror \
        "${includes[@]}" -c "$fixtures/legacy_action.c" -o "$build/$lane-out/action.o"
    record "$lane-c-action" green
    capture "$lane-cpp-model" clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
        "${includes[@]}" -x c++ -include moq/action.hpp -fsyntax-only /dev/null
    record "$lane-cpp-model" green
    if [ "$rc" -ne 0 ]; then exit 1; fi
    capture "$lane-cpp-reset" clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
        "${includes[@]}" -fsyntax-only "$fixtures/reset_decomposition.cpp"
    record "$lane-cpp-reset" "$expected" 'decomposes into 3 elements'
    capture "$lane-cpp-visitor" clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
        "${includes[@]}" -fsyntax-only "$fixtures/action_visitor.cpp"
    record "$lane-cpp-visitor" "$expected" 'unchanged action_variant has ten alternatives'

    if ! command -v swiftc >/dev/null; then
        printf '%s-swift: unavailable host compiler\n' "$lane" | tee -a "$build/results.log"
        failed=$((failed + 1))
        continue
    fi
    modules="$build/$lane-out/modules"
    mkdir -p "$modules/CMoQCore"
    # Only an importer map: declarations are the unmodified real public headers.
    printf 'module CMoQCore {\n header "%s"\n header "%s"\n export *\n}\n' \
        "$source/core/include/moq/session.h" "$source/core/include/moq/rcbuf.h" \
        > "$modules/CMoQCore/module.modulemap"
    swift_flags=(-swift-version 5 -warnings-as-errors -I "$modules"
                 -module-cache-path "$build/$lane-out/cache" -Xcc -Werror)
    capture "$lane-swift-model" swiftc "${swift_flags[@]}" -package-name LibMoQ \
        -emit-module -module-name MoQ -emit-module-path "$modules/MoQ.swiftmodule" \
        "$source/bindings/swift/Sources/MoQ/Action.swift" \
        "$source/bindings/swift/Sources/MoQ/Buffer.swift" \
        "$source/bindings/swift/Sources/MoQ/MoQError.swift"
    record "$lane-swift-model" green
    if [ "$rc" -ne 0 ]; then continue; fi
    for fixture in ResetConstruction ResetPattern ActionSwitch; do
        capture "$lane-swift-$fixture" swiftc "${swift_flags[@]}" \
            -package-name UnchangedClient -typecheck "$fixtures/$fixture.swift"
        case "$fixture" in
            ResetConstruction) pattern="missing argument for parameter 'reliableSize'";;
            ResetPattern) pattern='type of expression is ambiguous without a type annotation';;
            ActionSwitch) pattern='switch must be exhaustive';;
        esac
        record "$lane-swift-$fixture" "$expected" "$pattern"
    done
done
shasum -a 256 "$fixtures"/*.c "$fixtures"/*.cpp "$fixtures"/*.swift > "$build/fixtures-after.sha256"
cmp "$build/fixtures-before.sha256" "$build/fixtures-after.sha256"
printf 'expected_incompatibilities=%s assertion_failures=%s\n' "$incompatible" "$failed" | tee -a "$build/results.log"
if [ "$failed" -ne 0 ]; then exit 1; fi
if [ "$mode" = --expect-imported-breaks ]; then
    test "$incompatible" -eq 5
    printf 'Measured five expected incompatibilities; the candidate is not source-compatible.\n'
fi
