#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
build="$root/build/d22-swift-compatible"
native="$build/native"
lane=source
export MOQ_SWIFT_COMPATIBLE_SERVICE=0
if [ "${1:-}" = --service ]; then
    export MOQ_SWIFT_COMPATIBLE_SERVICE=1
    lane=service
    shift
fi
package="$build/package-$lane"
mkdir -p "$package/CMoQCore" "$package/CMoQSim"
attempt=$(date +%Y%m%dT%H%M%S)
for log in configure native-build swift-clean swift-test; do
    if [ -f "$build/$log.log" ]; then cp "$build/$log.log" "$build/$attempt-previous-$log.log"; fi
done
export CLANG_MODULE_CACHE_PATH="$build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$build/module-cache"
available=$(df -Pk "$root" | awk 'NR == 2 {print $4}')
test "$available" -ge 1048576
cmake -S "$root" -B "$native" -DMOQ_BUILD_TESTS=OFF -DMOQ_BUILD_SIM=ON \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DMOQ_BUILD_EXAMPLES=OFF -DMOQ_BUILD_LOC=ON -DMOQ_BUILD_CMAF=ON \
    -DMOQ_BUILD_CODEC_SIGNALING=OFF -DMOQ_BUILD_MEDIA_OBJECT=ON \
    -DMOQ_BUILD_PLAYBACK=ON -DMOQ_BUILD_MSF=ON -DMOQ_BUILD_SERVICE=ON \
    -DMOQ_WARNINGS_AS_ERRORS=ON > "$build/configure.log" 2>&1
cmake --build "$native" --target moq-core moq-sim moq-playback moq-service -j 2 > "$build/native-build.log" 2>&1
for module in MoQ MoQTransportModel MoQServiceCore MoQCatalog MoQMedia MoQService; do
    test -e "$package/$module" || ln -s "$root/bindings/swift/Sources/$module" "$package/$module"
done
for module in MoQTests MoQServiceCoreTests MoQMediaTests TransportVersionAliasTests MoQServiceTests; do
    test -e "$package/$module" || ln -s "$root/bindings/swift/Tests/$module" "$package/$module"
done
test -e "$package/Package.swift" || ln -s "$root/bindings/swift/Tests/CompatiblePackage.swift" "$package/Package.swift"
printf 'module CMoQCore {\n umbrella "%s"\n export *\n}\n' \
    "$root/core/include" > "$package/CMoQCore/module.modulemap"
printf 'module CMoQSim {\n header "%s"\n export *\n}\n' \
    "$root/sim/include/moq/sim.h" > "$package/CMoQSim/module.modulemap"
for pair in CMoQLOC:loc CMoQCMAF:cmaf CMoQMediaObject:object CMoQPlayback:playback; do
    module=${pair%%:*}
    component=${pair#*:}
    mkdir -p "$package/$module"
    printf 'module %s {\n umbrella "%s"\n export *\n}\n' \
        "$module" "$root/media/$component/include" > "$package/$module/module.modulemap"
done
mkdir -p "$package/CMoQService"
printf 'module CMoQService {\n header "%s"\n export *\n}\n' \
    "$root/bindings/swift/SystemModules/CMoQService/shim.h" > "$package/CMoQService/module.modulemap"
native_flags=()
for component in loc cmaf object msf playback; do
    native_flags+=(-Xcc -I"$root/media/$component/include")
done
native_flags+=(-Xcc -I"$root/service/include")
for archive in service/libmoq-service.a media/msf/libmoq-msf.a media/playback/libmoq-playback.a \
    media/object/libmoq-media-object.a media/loc/libmoq-loc.a media/cmaf/libmoq-cmaf.a; do
    native_flags+=(-Xlinker "$native/$archive")
done
# SwiftPM does not track the external archives passed through -Xlinker.
swift package --package-path "$package" --scratch-path "$build/swift-$lane" \
    --cache-path "$build/cache" --config-path "$build/config" \
    --security-path "$build/security" --disable-sandbox clean > "$build/swift-clean.log" 2>&1
swift test --package-path "$package" --scratch-path "$build/swift-$lane" \
    --cache-path "$build/cache" --config-path "$build/config" \
    --security-path "$build/security" --disable-sandbox -j 2 \
    -Xswiftc -warnings-as-errors -Xcc -Werror -Xcc -I"$root/core/include" \
    "${native_flags[@]}" \
    -Xlinker "$native/sim/libmoq-sim.a" -Xlinker "$native/core/libmoq-core.a" \
    "$@" > "$build/swift-test.log" 2>&1
if rg -i 'warning:|CMake Warning' "$build/configure.log" "$build/native-build.log" "$build/swift-clean.log" "$build/swift-test.log"; then
    exit 1
fi
cp "$build/swift-test.log" "$build/$lane-tests.log"
tail -8 "$build/swift-test.log"
