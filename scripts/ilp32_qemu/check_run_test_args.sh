#!/usr/bin/env bash
# The QEMU runner's duration arguments: --timeout and KILL_GRACE must be
# positive decimal seconds. GNU timeout reads "0", "00" or "000" as zero, which
# disables the run bound (or the SIGKILL escalation), so every all-zero
# spelling is refused before anything runs, as are empty and non-decimal
# values; positive values pass the check. Each runner call is itself bounded,
# and the accepted ones name a toolchain that does not exist, so nothing is
# built. Usage: check_run_test_args.sh [BUILD-DIR].
# The control shows why: with a zero KILL grace a child that ignores SIGTERM
# outlives its timeout, and with a positive one it is killed. Exit 0 when every
# case holds.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
runner="$here/run_test.sh"
# Where an accepted run would build; the checks stop before any build.
build="${1:-${TMPDIR:-/tmp}/check-run-test-args}"
T=""
for t in timeout gtimeout; do command -v "$t" >/dev/null 2>&1 && { T="$t"; break; }; done
[ -n "$T" ] || { echo "check_run_test_args: needs GNU timeout" >&2; exit 2; }
fail=0
refused() {   # $1 label, then the command; expects exit 2 and the argument message
    local label="$1"; shift
    local out rc
    out="$("$T" -s KILL 20 "$@" 2>&1)"; rc=$?
    if [ "$rc" -ne 2 ] || ! printf '%s' "$out" | grep -q "needs a positive number"; then
        echo "FAIL: $label: exit $rc: $out" >&2; fail=1
    fi
}
accepted() {  # the duration check passes (whatever the toolchain then says)
    local label="$1"; shift
    local out
    out="$("$T" -s KILL 20 "$@" 2>&1)"
    if printf '%s' "$out" | grep -q "needs a positive number"; then
        echo "FAIL: $label: refused: $out" >&2; fail=1
    fi
}
for v in 0 00 000 "" abc -1 1.5; do
    refused "--timeout '$v'" "$runner" --test test_abi_old_callers_d22 --build-dir "$build" --timeout "$v"
    # An empty KILL_GRACE is unset: the default applies.
    [ -n "$v" ] || continue
    refused "KILL_GRACE='$v'" env KILL_GRACE="$v" "$runner" --test test_abi_old_callers_d22 --build-dir "$build"
done
refused "--timeout without a value" "$runner" --test test_abi_old_callers_d22 --build-dir "$build" --timeout
accepted "--timeout 5" env ARM_NONE_EABI_GCC=/nonexistent QEMU_SYSTEM_ARM=/nonexistent \
    "$runner" --test test_abi_old_callers_d22 --build-dir "$build" --timeout 5
accepted "KILL_GRACE=010" env KILL_GRACE=010 ARM_NONE_EABI_GCC=/nonexistent \
    QEMU_SYSTEM_ARM=/nonexistent "$runner" --test test_abi_old_callers_d22 --build-dir "$build"

# Control, under an outer SIGKILL bound of 6 s: a TERM-ignoring child given a
# 1 s timeout survives with KILL grace 00 and is killed with KILL grace 1.
start=$(date +%s)
{ ( "$T" -s KILL 6 "$T" -k 00 1 sh -c 'trap "" TERM; sleep 30' ) >/dev/null 2>&1; } 2>/dev/null
zero_elapsed=$(( $(date +%s) - start ))
start=$(date +%s)
{ ( "$T" -s KILL 6 "$T" -k 1 1 sh -c 'trap "" TERM; sleep 30' ) >/dev/null 2>&1; } 2>/dev/null
one_elapsed=$(( $(date +%s) - start ))
if [ "$zero_elapsed" -lt 5 ] || [ "$one_elapsed" -gt 4 ]; then
    echo "FAIL: control: KILL grace 00 ran ${zero_elapsed}s, 1 ran ${one_elapsed}s" >&2; fail=1
fi
[ "$fail" -eq 0 ] && echo "check_run_test_args: ok"
exit "$fail"
