#!/usr/bin/env bash
#
# Build one core unit test for a 32-bit (ILP32) target and execute it.
#
# The test (tests/unit/<name>.c) is compiled together with every core/src and
# sim/src source for a Cortex-M3 with arm-none-eabi-gcc (newlib, rdimon
# semihosting), linked for QEMU's mps2-an385 board with the startup.c and
# an385.ld beside this script, and run under qemu-system-arm. size_t is 32
# bits there, so code that narrows a 64-bit wire value to size_t before
# range-checking it behaves as it would on any ILP32 device; 64-bit hosts
# cannot show that. The core sources are compiled the way the
# moq-core-test-internals target compiles them, so tests that link against
# it (and against moq::sim) build here unchanged. Arguments after `--` are
# passed to the test, and its file I/O goes to the host through
# semihosting, so relative paths resolve from the current directory.
#
# The heap is 15 MiB of the board's PSRAM and malloc returns NULL when it
# is exhausted; a CPU fault prints "ILP32 fault: ..." (exception, CFSR,
# faulting PC) and ends the run with status 1.
#
# Requires arm-none-eabi-gcc with newlib's rdimon.specs, qemu-system-arm
# with the mps2-an385 machine, and a timeout(1) with -k (GNU coreutils
# timeout or gtimeout) to bound the run. Exits 2 with a message when any is
# missing; otherwise exits with the test's own status.
#
# The bound holds even when QEMU does not act on SIGTERM (a guest wedged in
# a lockup can leave it unresponsive to it): past --timeout the run gets
# SIGTERM and, KILL_GRACE seconds later (default 10), SIGKILL. The status
# is then 124 (ended by SIGTERM) or 137 (needed SIGKILL). timeout(1) runs
# QEMU in its own process group; if this script is interrupted or
# terminated, it signals that whole group, SIGTERM then SIGKILL, before it
# exits, so no QEMU outlives the runner. Should the runner itself be
# SIGKILLed, the timeout(1) process stays behind and still enforces the
# bound.
#
# Usage:
#   scripts/ilp32_qemu/run_test.sh --test NAME [--build-dir DIR]
#                                  [--timeout SECS] [-- TEST-ARGS...]
#
# Example, from the repository root:
#   scripts/ilp32_qemu/run_test.sh --test test_vectors_d22_data -- tests/vectors/d22

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TEST=""
BUILD_DIR="build/ilp32-qemu"
TIMEOUT_SECS="600"
TEST_ARGS=()

usage() { sed -n '3,41p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --test)      TEST="${2:?--test needs a name}"; shift 2 ;;
        --build-dir) BUILD_DIR="${2:?--build-dir needs a directory}"; shift 2 ;;
        --timeout)   [ "$#" -ge 2 ] || { echo "run_test.sh: --timeout needs a positive number of seconds" >&2; exit 2; }
                     TIMEOUT_SECS="$2"; shift 2 ;;
        --help|-h)   usage; exit 0 ;;
        --)          shift; TEST_ARGS=("$@"); break ;;
        *)           echo "run_test.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$TEST" ]; then
    echo "run_test.sh: --test NAME is required" >&2
    exit 2
fi
TEST="${TEST%.c}"
TEST_SRC="$REPO_ROOT/tests/unit/$TEST.c"
if [ ! -f "$TEST_SRC" ]; then
    echo "run_test.sh: no such test source: tests/unit/$TEST.c" >&2
    exit 2
fi

CC="${ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}"
QEMU="${QEMU_SYSTEM_ARM:-qemu-system-arm}"
missing=0
TIMEOUT_CMD=""
for t in timeout gtimeout; do
    if command -v "$t" >/dev/null 2>&1; then TIMEOUT_CMD="$t"; break; fi
done
if [ -z "$TIMEOUT_CMD" ]; then
    echo "run_test.sh: neither timeout nor gtimeout found; install GNU coreutils so the run can be bounded" >&2
    missing=1
fi
# Durations are positive decimal seconds. timeout(1) reads every all-zero
# spelling ("0", "00", ...) as zero, which would remove the bound or the
# SIGKILL escalation, so at least one digit must be nonzero.
positive_secs() {
    case "$1" in ''|*[!0-9]*) return 1 ;; *[1-9]*) return 0 ;; *) return 1 ;; esac
}
KILL_GRACE="${KILL_GRACE:-10}"
positive_secs "$KILL_GRACE" || {
    echo "run_test.sh: KILL_GRACE needs a positive number of seconds" >&2; exit 2; }
if [ -n "$TIMEOUT_CMD" ] && ! "$TIMEOUT_CMD" -k 1 5 true >/dev/null 2>&1; then
    echo "run_test.sh: $TIMEOUT_CMD does not support -k (SIGKILL escalation); install GNU coreutils" >&2
    missing=1
fi
positive_secs "$TIMEOUT_SECS" || {
    echo "run_test.sh: --timeout needs a positive number of seconds" >&2; exit 2; }
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "run_test.sh: $CC not found; install the Arm GNU toolchain (arm-none-eabi) or set ARM_NONE_EABI_GCC" >&2
    missing=1
elif [ "$("$CC" -print-file-name=rdimon.specs)" = "rdimon.specs" ]; then
    echo "run_test.sh: $CC has no newlib rdimon.specs (semihosting runtime)" >&2
    missing=1
fi
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "run_test.sh: $QEMU not found; install QEMU's system emulators or set QEMU_SYSTEM_ARM" >&2
    missing=1
elif ! "$QEMU" -machine help 2>/dev/null | grep -q '^mps2-an385 '; then
    echo "run_test.sh: $QEMU does not provide the mps2-an385 machine" >&2
    missing=1
fi
[ "$missing" -eq 0 ] || exit 2

case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac
OBJ_DIR="$BUILD_DIR/obj"
mkdir -p "$OBJ_DIR"

CFLAGS=(-mcpu=cortex-m3 -mthumb -std=c11 -O1 -g
        -Wall -Wextra -Wpedantic -Werror
        -ffunction-sections -fdata-sections
        -DMOQ_STATIC -DMOQ_SIM_BUILDING
        -DMOQ_PUB_TESTING -DMOQ_SESSION_SWEEP_TESTING
        -I"$REPO_ROOT/core/include" -I"$REPO_ROOT/sim/include")

# The target must really be ILP32; fail the build otherwise.
printf '#include <stddef.h>\n#include <stdint.h>\n_Static_assert(sizeof(size_t) == 4 && SIZE_MAX == 0xFFFFFFFFu && sizeof(void *) == 4, "ILP32 target");\n' \
    | "$CC" "${CFLAGS[@]}" -x c -fsyntax-only -

OBJS=()
cd "$REPO_ROOT"
for src in core/src/*.c core/src/*/*.c sim/src/*.c "tests/unit/$TEST.c" \
           "${SCRIPT_DIR#"$REPO_ROOT"/}/startup.c"; do
    obj="$OBJ_DIR/${src//\//_}.o"
    "$CC" "${CFLAGS[@]}" -c "$REPO_ROOT/$src" -o "$obj"
    OBJS+=("$obj")
done
cd - >/dev/null

ELF="$BUILD_DIR/$TEST.elf"
"$CC" -mcpu=cortex-m3 -mthumb --specs=rdimon.specs -T "$SCRIPT_DIR/an385.ld" \
    -Wl,--gc-sections -Wl,-Map="$BUILD_DIR/$TEST.map" "${OBJS[@]}" -o "$ELF"
echo "built $ELF (Cortex-M3, sizeof(size_t) == 4)"

SEMI="enable=on,target=native,arg=$TEST"
for a in ${TEST_ARGS[@]+"${TEST_ARGS[@]}"}; do
    case "$a" in *,*) echo "run_test.sh: test arguments may not contain ','" >&2; exit 2 ;; esac
    SEMI="$SEMI,arg=$a"
done

# timeout(1) makes itself the leader of a new process group holding QEMU;
# RUN_PID names both the process and that group.
RUN_PID=""
stop_run() {
    [ -n "$RUN_PID" ] || return 0
    kill -TERM -- "-$RUN_PID" 2>/dev/null || kill -TERM "$RUN_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 -- "-$RUN_PID" 2>/dev/null || kill -0 "$RUN_PID" 2>/dev/null || { RUN_PID=""; return 0; }
        sleep 0.2
    done
    kill -KILL -- "-$RUN_PID" 2>/dev/null || kill -KILL "$RUN_PID" 2>/dev/null || true
    RUN_PID=""
}
trap 'stop_run' EXIT
trap 'stop_run; exit 130' INT
trap 'stop_run; exit 143' TERM
trap 'stop_run; exit 129' HUP

status=0
"$TIMEOUT_CMD" -k "$KILL_GRACE" "$TIMEOUT_SECS" "$QEMU" -M mps2-an385 -nographic \
    -monitor none -serial none -semihosting-config "$SEMI" -kernel "$ELF" &
RUN_PID=$!
wait "$RUN_PID" || status=$?
RUN_PID=""
echo "$TEST on mps2-an385 (ILP32): exit $status"
exit "$status"
