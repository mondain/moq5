#!/usr/bin/env bash
# Opaque object conformance through a separately launched moq5-relay.
#
# Starts one owned relay on loopback with a fresh certificate authority,
# server certificate, ports and configuration (raw QUIC and WebTransport
# listeners, each offering drafts 22, 18 and 16), runs the interop client's
# `object-relay` case for every requested draft over both transports, and
# tears the relay down. The client keeps TLS verification on and trusts only
# this run's CA, through SSL_CERT_FILE for its own process.
#
# The payload bytes are opaque: a pass shows Objects, their identities,
# lengths and bytes, the subgroup FINs and PUBLISH_DONE crossing the relay. It
# says nothing about catalogs, codecs or playback.
#
# Usage:
#   run_object_relay.sh --relay-bin PATH --client-bin PATH --out DIR
#                       [--drafts "22 18"] [--faults]
#
#   --out DIR   a directory that does not exist yet; every log, the TAP of
#               each cell and the relay's output are kept there
#   --faults    also run each publisher fault (corrupt-byte, drop-object,
#               extra-object) on the first draft over raw QUIC; each must be
#               rejected by the subscriber's verifier
#
# Exit status: 0 when every cell passes (and, with --faults, every fault run
# is rejected at the verification step); 1 otherwise; 2 for a usage or setup
# error. Every process this script starts is bounded and reaped before it
# exits.
set -u -o pipefail

relay_bin=
client_bin=
out=
drafts="22 18"
faults=0
while [ $# -gt 0 ]; do
    case "$1" in
        --relay-bin) relay_bin=${2:-}; shift 2 ;;
        --client-bin) client_bin=${2:-}; shift 2 ;;
        --out) out=${2:-}; shift 2 ;;
        --drafts) drafts=${2:-}; shift 2 ;;
        --faults) faults=1; shift ;;
        *) echo "run_object_relay: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -x "$relay_bin" ] || { echo "run_object_relay: --relay-bin is not executable" >&2; exit 2; }
[ -x "$client_bin" ] || { echo "run_object_relay: --client-bin is not executable" >&2; exit 2; }
[ -n "$out" ] || { echo "run_object_relay: --out is required" >&2; exit 2; }
[ ! -e "$out" ] || { echo "run_object_relay: $out already exists" >&2; exit 2; }
for d in $drafts; do
    case "$d" in 16|18|22) ;; *) echo "run_object_relay: draft $d is not 16, 18 or 22" >&2; exit 2 ;; esac
done
openssl_bin=${OPENSSL:-openssl}
python_bin=${PYTHON:-python3}
mkdir -p "$out" || exit 2
out=$(cd "$out" && pwd)

if command -v sha256sum >/dev/null 2>&1; then sha=(sha256sum); else sha=(shasum -a 256); fi
"${sha[@]}" "$relay_bin" "$client_bin" > "$out/binaries.sha256" 2>&1

relay_pid=
relay_watchdog=
cleanup() {
    if [ -n "$relay_pid" ] && kill -0 "$relay_pid" 2>/dev/null; then
        kill -TERM "$relay_pid" 2>/dev/null
        for _ in $(seq 1 50); do
            kill -0 "$relay_pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$relay_pid" 2>/dev/null
    fi
    if [ -n "$relay_pid" ]; then
        wait "$relay_pid" 2>/dev/null
        echo "$?" > "$out/relay.exit"
    fi
    if [ -n "$relay_watchdog" ]; then
        kill -TERM "$relay_watchdog" 2>/dev/null
        wait "$relay_watchdog" 2>/dev/null
    fi
    rm -f "$out/ca.key"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# bounded SECONDS CMD... : run CMD with a deadline; status 124 when it fired.
bounded() {
    local secs=$1 pid watchdog status
    shift
    "$@" &
    pid=$!
    # The watchdog's own sleep is killed with it, so nothing outlives a run.
    (
        trap 'kill "$nap" 2>/dev/null; exit 0' TERM
        sleep "$secs" & nap=$!
        wait "$nap"
        kill -TERM "$pid" 2>/dev/null
        sleep 2 & nap=$!
        wait "$nap"
        kill -KILL "$pid" 2>/dev/null
    ) &
    watchdog=$!
    wait "$pid"
    status=$?
    if kill -0 "$watchdog" 2>/dev/null; then
        kill "$watchdog" 2>/dev/null
        wait "$watchdog" 2>/dev/null
        return "$status"
    fi
    wait "$watchdog" 2>/dev/null
    return 124
}

# Credentials for this run only: a CA and a server certificate it signs, valid
# for the loopback address the client dials.
(
    cd "$out" &&
    "$openssl_bin" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout ca.key -out ca.pem -days 1 -subj "/CN=object-relay run CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" &&
    "$openssl_bin" req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout key.pem -out server.csr -subj "/CN=127.0.0.1" &&
    printf '%s\n' "subjectAltName=IP:127.0.0.1" "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature" "extendedKeyUsage=serverAuth" > server.ext &&
    "$openssl_bin" x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
        -out cert.pem -days 1 -extfile server.ext &&
    "$openssl_bin" verify -CAfile ca.pem -verify_ip 127.0.0.1 cert.pem
) > "$out/credentials.log" 2>&1 || { echo "run_object_relay: certificate setup failed (see credentials.log)" >&2; exit 2; }
rm -f "$out/ca.key"

# Two UDP ports free on loopback at this moment.
read -r raw_port wt_port < <("$python_bin" - <<'PY'
import socket
socks = []
for _ in range(2):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    socks.append(s)
print(*(s.getsockname()[1] for s in socks))
for s in socks:
    s.close()
PY
) || { echo "run_object_relay: no free ports" >&2; exit 2; }

cat > "$out/relay.json" <<EOF
{
  "listener": { "host": "127.0.0.1", "port": $raw_port, "versions": [22, 18, 16],
                "lanes": 1, "cert": "cert.pem", "key": "key.pem" },
  "webtransport": { "host": "127.0.0.1", "port": $wt_port, "versions": [22, 18, 16],
                    "lanes": 1, "path": "/moq", "profile": "current",
                    "cert": "cert.pem", "key": "key.pem" },
  "logging": { "format": "text" },
  "auth": { "mode": "allow_all" }
}
EOF

# The relay runs until this script stops it; a watchdog ends it after
# RELAY_SECONDS (default 600) should the script itself be stopped first.
printf '%q ' "$relay_bin" serve --config relay.json > "$out/relay.cmd"
echo >> "$out/relay.cmd"
( cd "$out" && exec "$relay_bin" serve --config relay.json ) \
    > "$out/relay.stdout" 2> "$out/relay.stderr" &
relay_pid=$!
(
    trap 'kill "$nap" 2>/dev/null; exit 0' TERM
    sleep "${RELAY_SECONDS:-600}" & nap=$!
    wait "$nap"
    kill -TERM "$relay_pid" 2>/dev/null
) &
relay_watchdog=$!
ready=0
for _ in $(seq 1 100); do
    if grep -q "WebTransport on 127.0.0.1:$wt_port" "$out/relay.stdout" 2>/dev/null &&
       grep -q "listening on 127.0.0.1:$raw_port" "$out/relay.stdout" 2>/dev/null; then
        ready=1
        break
    fi
    kill -0 "$relay_pid" 2>/dev/null || break
    sleep 0.1
done
if [ "$ready" != 1 ]; then
    echo "run_object_relay: the relay did not report both listeners (see relay.stdout/relay.stderr)" >&2
    exit 2
fi

status=0
summary="$out/summary.txt"
printf '%-6s %-13s %-6s %s\n' draft transport result detail > "$summary"
run_cell() {   # run_cell NAME DRAFT URL [FAULT]
    local name=$1 draft=$2 url=$3 fault=${4:-} rc
    local -a envs=("SSL_CERT_FILE=$out/ca.pem")
    [ -n "$fault" ] && envs+=("MOQ_INTEROP_OBJECT_RELAY_FAULT=$fault")
    printf '%q ' env "${envs[@]}" "$client_bin" --relay "$url" --draft "$draft" \
        --test object-relay > "$out/$name.cmd"
    echo >> "$out/$name.cmd"
    bounded 60 env "${envs[@]}" "$client_bin" --relay "$url" --draft "$draft" \
        --test object-relay > "$out/$name.tap" 2> "$out/$name.stderr"
    rc=$?
    echo "$rc" > "$out/$name.exit"
    return "$rc"
}

for d in $drafts; do
    for t in raw wt; do
        if [ "$t" = raw ]; then url="moqt://127.0.0.1:$raw_port"; tl=raw-quic
        else url="https://127.0.0.1:$wt_port/moq"; tl=webtransport; fi
        run_cell "d$d-$t" "$d" "$url"
        rc=$?
        if [ "$rc" = 0 ] && ! grep -q '^not ok' "$out/d$d-$t.tap" &&
           [ "$(grep -c '^ok ' "$out/d$d-$t.tap")" = 7 ]; then
            printf '%-6s %-13s %-6s %s\n' "$d" "$tl" PASS \
                "$(sed -n '/^ok 7 /,/\.\.\./s/^  message: //p' "$out/d$d-$t.tap")" >> "$summary"
        else
            status=1
            printf '%-6s %-13s %-6s exit %s: %s\n' "$d" "$tl" FAIL "$rc" \
                "$(grep -m1 -A3 '^not ok' "$out/d$d-$t.tap" | sed -n 's/^  message: //p')" >> "$summary"
        fi
    done
done

if [ "$faults" = 1 ]; then
    first=${drafts%% *}
    for f in corrupt-byte drop-object extra-object; do
        run_cell "fault-$f" "$first" "moqt://127.0.0.1:$raw_port" "$f"
        rc=$?
        # Rejected means: everything up to the delivery ran, and step 7 (the
        # verifier) is the step that failed.
        if [ "$rc" != 0 ] && grep -q '^not ok 7 ' "$out/fault-$f.tap" &&
           [ "$(grep -c '^ok [1-5] ' "$out/fault-$f.tap")" = 5 ]; then
            printf '%-6s %-13s %-6s %s\n' "$first" "fault:$f" REJECTED \
                "$(sed -n '/^not ok 7 /,/\.\.\./s/^  message: //p' "$out/fault-$f.tap")" >> "$summary"
        else
            status=1
            printf '%-6s %-13s %-6s exit %s (expected the verifier to reject)\n' \
                "$first" "fault:$f" ACCEPTED "$rc" >> "$summary"
        fi
    done
fi

cat "$summary"
exit "$status"
