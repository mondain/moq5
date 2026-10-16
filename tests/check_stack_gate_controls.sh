#!/bin/bash
set -euo pipefail
export LC_ALL=C
[[ $# -ge 3 && $# -le 4 && ( $# -eq 3 || $4 == --arm ) ]] || exit 2
root=$(cd "$1" && pwd -P)
cc=$2
mkdir -p "$(dirname "$3")"
out="$(cd "$(dirname "$3")" && pwd -P)/$(basename "$3")"
mkdir "$out"

for gate in d22_setup session_goaway; do
    unit=profile
    if [[ $gate == session_goaway ]]; then unit=session; fi
    for control in missing compile_failure; do
        fixture="$root/tests/stack_gate_mechanism_control.c"
        expected=owner.missing
        if [[ $control == compile_failure ]]; then
            fixture="$root/tests/stack_gate_compile_failure.c"
            expected="$unit.compile"
        fi
        label="$gate-$control"
        command=(env "MEASURE_TU=$fixture" bash "$root/tests/check_${gate}_stack.sh" "$root" "$cc" "$out/$label")
        if [[ ${4:-} == --arm ]]; then command+=(--arm); fi
        printf '%q ' "${command[@]}" > "$out/$label.command.log"
        printf '\n' >> "$out/$label.command.log"
        if "${command[@]}" > "$out/$label.stdout.log" 2> "$out/$label.diagnostics.log"; then
            rc=0
        else
            rc=$?
        fi
        printf '%s\n' "$rc" > "$out/$label.status.log"
        test "$rc" -eq 1 && test ! -s "$out/$label.diagnostics.log" || {
            printf 'FAIL[control.%s]: rc=%s or unexpected gate diagnostic\n' "$label" "$rc"
            exit 1
        }
        awk -v tag="FAIL[$expected]:" '
            index($0,"FAIL[")==1 {n++; if(index($0,tag)!=1) bad=1}
            /^MEASURE\[moq_frame_probe\]:/ {positive++}
            /^PASS\[/ {bad=1}
            END {exit (bad || n!=1 || positive!=1)}
        ' "$out/$label.stdout.log" || { printf 'FAIL[control.%s]: wrong failure mechanism\n' "$label"; exit 1; }
        if [[ $control == compile_failure ]]; then
            awk '
                /:[0-9]+(:[0-9]+)?: error:/ {
                    n++; if(!index($0,"stack-gate deliberate compile-failure control")) bad=1
                }
                /warning:|note:/ {bad=1}
                END {exit (bad || n!=1)}
            ' "$out/$label/$unit.diagnostics.log" || { printf 'FAIL[control.%s]: wrong deliberate diagnostic\n' "$label"; exit 1; }
            test "$(< "$out/$label/$unit.status.log")" -ne 0
        else
            test "$(< "$out/$label/$unit.status.log")" -eq 0
        fi
        printf 'PASS[control.%s]: live positive; exact %s rejection\n' "$label" "$expected"
    done
done
