#!/usr/bin/env bash
#
# build_macos_openssl.sh - pinned, verified static OpenSSL for macOS at an
# explicit minimum OS.
#
# Homebrew pours OpenSSL bottles built for the host OS, so a consumer that
# links them cannot target an older macOS than the host. This script builds
# the pinned OpenSSL release from verified source as static libcrypto.a +
# libssl.a for one macOS architecture at MOQ_MACOS_DEPLOYMENT_TARGET, into
# a root the caller owns exclusively:
#
#     <root>/src       pinned OpenSSL checkout (fetched here, never shared)
#     <root>/build     out-of-tree build directory
#     <root>/prefix    headers + static libraries (+ OpenSSL's .pc files)
#     <root>/prefix/moq-openssl-receipt   the inputs this prefix was built from
#
# and prints the value for picotls and the LibMoQ configure:
#
#     OPENSSL_ROOT_DIR=<root>/prefix
#
# The prefix is an external provider for a build on this machine. LibMoQ's
# install does not copy it, and the installed libmoq.pc refers to it by
# absolute path.
#
# The receipt records the source commit and cleanliness, this script's
# digest, architecture, compiler, SDK, deployment target and the configure
# line. An existing prefix is reused only when its receipt matches the one
# this run would write; any difference is an error, never a silent reuse.
#
# Usage:
#     scripts/build_macos_openssl.sh <root>
#
# Env (optional):
#   MOQ_MACOS_DEPLOYMENT_TARGET  minimum macOS (default: 11.0)
#   MOQ_MACOS_ARCH               arm64 or x86_64 (default: host)
#   MOQ_MACOS_OPENSSL_JOBS       parallel build jobs (default: hw.ncpu)
#
# Requires: git, perl, make, xcrun (Xcode or Command Line Tools), python3.

set -euo pipefail

# Same pin as scripts/build_ios_openssl.sh: openssl-3.5.7 (3.5.x LTS),
# verified by commit because a tag can be re-pointed. No override: a
# different source is a different recipe.
OPENSSL_REF="openssl-3.5.7"
OPENSSL_REPO="https://github.com/openssl/openssl"
OPENSSL_PINNED_COMMIT="8cf17aaeb4599f8af87fefd810b5b5fee90fe69e"

script_dir=$(cd "$(dirname "$0")" && pwd)

log() { printf '[build_macos_openssl] %s\n' "$*" >&2; }
die() { printf '[build_macos_openssl] ERROR: %s\n' "$*" >&2; exit 1; }

# Keep both streams and the real command status, including successful commands
# that emit diagnostics. stdout of this script remains the prefix assignment.
run_logged() {
    local name=$1 status=0
    shift
    "$@" >"$root/$name.stdout" 2>"$root/$name.stderr" || status=$?
    printf '%s\n' "$status" >"$root/$name.status" || return $?
    cat "$root/$name.stdout" "$root/$name.stderr" >&2 || return $?
    [ "$status" -eq 0 ] || return "$status"
    if grep -Ei '(^|[^[:alnum:]_])(warning|error|fatal error)(:|[[:space:]])' \
            "$root/$name.stdout" "$root/$name.stderr" >&2; then
        die "$name emitted diagnostics"
    else
        status=$?
        [ "$status" -eq 1 ] || return "$status"
    fi
}

[ $# -eq 1 ] || die "usage: $0 <root>"
for tool in git perl make xcrun python3 shasum; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found"
done

mkdir -p "$1"
root=$(cd "$1" && pwd)
src="$root/src"
bdir="$root/build"
prefix="$root/prefix"
receipt="$prefix/moq-openssl-receipt"

deploy_target="${MOQ_MACOS_DEPLOYMENT_TARGET:-11.0}"
arch="${MOQ_MACOS_ARCH:-$(uname -m)}"
jobs="${MOQ_MACOS_OPENSSL_JOBS:-$(sysctl -n hw.ncpu)}"
case "$arch" in
arm64)  target=darwin64-arm64-cc ;;
x86_64) target=darwin64-x86_64-cc ;;
*)      die "unsupported MOQ_MACOS_ARCH '$arch' (want arm64 or x86_64)" ;;
esac

run_logged compiler-path xcrun --sdk macosx -f clang
cc=$(cat "$root/compiler-path.stdout")
run_logged sdk-path xcrun --sdk macosx --show-sdk-path
sdk_path=$(cat "$root/sdk-path.stdout")
run_logged sdk-version xcrun --sdk macosx --show-sdk-version
sdk_version=$(cat "$root/sdk-version.stdout")
run_logged compiler-version "$cc" --version
cc_version=$(head -1 "$root/compiler-version.stdout")

# -- Source: fetch the pin into the owned root, then verify it ---------
if [ ! -f "$src/Configure" ]; then
    log "fetching $OPENSSL_REF from $OPENSSL_REPO"
    mkdir -p "$src"
    run_logged git-init git -C "$src" init -q
    run_logged git-remote git -C "$src" remote add origin "$OPENSSL_REPO"
    run_logged git-fetch git -C "$src" fetch -q --depth 1 origin "refs/tags/$OPENSSL_REF"
    run_logged git-checkout git -C "$src" checkout -q FETCH_HEAD
fi
run_logged git-head git -C "$src" rev-parse HEAD
head_commit=$(cat "$root/git-head.stdout")
[ "$head_commit" = "$OPENSSL_PINNED_COMMIT" ] || die \
    "OpenSSL commit pin mismatch: $src is at ${head_commit:-?}, expected $OPENSSL_PINNED_COMMIT ($OPENSSL_REF)"
run_logged git-status git -C "$src" status --porcelain --ignored
[ ! -s "$root/git-status.stdout" ] \
    || die "OpenSSL source $src is not clean"
log "commit pin OK : $head_commit ($OPENSSL_REF), clean"

# -- Recipe identity ---------------------------------------------------
# no-shared: static only. no-tests/no-apps/no-docs: nothing a consumer
# links. Default algorithms stay on (picotls/picoquic use a broad set).
configure_args=("$target" no-shared no-tests no-apps no-docs
    "--prefix=$prefix" "--openssldir=$prefix/ssl"
    "-mmacosx-version-min=$deploy_target" "-isysroot" "$sdk_path")
expected_receipt=$(cat <<EOF
openssl_ref=$OPENSSL_REF
openssl_commit=$head_commit
openssl_source_clean=yes
recipe_sha256=$(shasum -a 256 "$script_dir/build_macos_openssl.sh" | cut -d' ' -f1)
arch=$arch
target=$target
cc=$cc
cc_version=$cc_version
sdk_path=$sdk_path
sdk_version=$sdk_version
deployment_target=$deploy_target
configure=${configure_args[*]}
env=CC=$cc MACOSX_DEPLOYMENT_TARGET=$deploy_target
EOF
)

if [ -e "$prefix" ]; then
    if [ -f "$receipt" ] && [ "$(cat "$receipt")" = "$expected_receipt" ]; then
        log "reusing $prefix (receipt matches)"
        printf 'OPENSSL_ROOT_DIR=%s\n' "$prefix"
        exit 0
    fi
    die "$prefix exists but was built from different inputs; use a fresh root"
fi

# -- Configure / build / install ---------------------------------------
rm -rf "$bdir"
mkdir -p "$bdir"
log "configure: ${configure_args[*]}"
( cd "$bdir" && run_logged configure env CC="$cc" MACOSX_DEPLOYMENT_TARGET="$deploy_target" \
    perl "$src/Configure" "${configure_args[@]}" )
log "build (-j$jobs)"
run_logged build env CC="$cc" MACOSX_DEPLOYMENT_TARGET="$deploy_target" make -C "$bdir" -j"$jobs"
# install_dev: headers, static libraries and pkg-config files.
run_logged install env CC="$cc" MACOSX_DEPLOYMENT_TARGET="$deploy_target" \
    make -C "$bdir" install_dev

[ -f "$prefix/include/openssl/ssl.h" ] || die "no ssl.h installed"
[ -f "$prefix/lib/libcrypto.a" ]       || die "no libcrypto.a"
[ -f "$prefix/lib/libssl.a" ]          || die "no libssl.a"
if find "$prefix" -name '*.dylib' | grep -q .; then
    die "a dylib was installed into $prefix; this prefix must be static-only"
fi

# Every member of both archives must be a macOS object at or below the
# deployment target.
run_logged audit-openssl python3 "$script_dir/macho_deploy_audit.py" archives --max-macos "$deploy_target" \
    "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a"

printf '%s\n' "$expected_receipt" > "$receipt"
log "done: $prefix"
printf 'OPENSSL_ROOT_DIR=%s\n' "$prefix"
