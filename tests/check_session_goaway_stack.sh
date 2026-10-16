#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/stack_gate_support.sh"
stack_init "$@"
stack_positive
stack_compile session "${MEASURE_TU:-$root/core/src/session/session.c}"
stack_owner session moq_session_goaway
stack_limit
awk -F '\t' '
    $2>=1024 {
        name=$1; sub(/^.*:/,"",name)
        if (name!="moq_session_goaway") {
            print "UNEXPECTED[session.frame]: " $0 > "/dev/stderr"; bad=1
        }
    }
    END {exit bad}
' "$out/session.su" || stack_fail inventory.unknown 'undeclared oversized session owner'
test "$gate_failed" -eq 0 || stack_fail goaway.frame 'GOAWAY exceeds1023 bytes'
printf 'PASS[goaway.frame.%s]: exact emitted owner below1024\n' "$compiler_kind"
