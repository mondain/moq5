"""One owned relay, real peer exchanges, finite watchdogs. No retries.

The relay is started with PORT 0 and the peers dial the port its readiness
line names on 127.0.0.1. MsQuic binds the dual-stack wildcard; the transport
facade picks port 0 so that IPv4 clients reach it even where IPv4-only sockets
hold much of the ephemeral range (simple_relay_loopback_port_shadow).
"""
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import tempfile
import time


def read_readiness(child):
    """The relay's readiness line, or None when it exited before printing one."""
    with selectors.DefaultSelector() as selector:
        selector.register(child.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 10
        line = b""
        while b"\n" not in line:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise RuntimeError("relay readiness deadline")
            part = os.read(child.stdout.fileno(), 256)
            if not part and not line:
                try:
                    if child.wait(timeout=5) != 0:
                        return None
                except subprocess.TimeoutExpired:
                    pass
            if not part or len(line) + len(part) > 256:
                raise RuntimeError("relay readiness absent/malformed")
            line += part
    return line


def stop(child):
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)
    child.stdout.close()


def main():
    relay, peers, cert, key = sys.argv[1:]
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("LD_", "DYLD_"))}
    with tempfile.TemporaryDirectory(prefix="simple-relay-") as temp:
        log = Path(temp) / "relay.log"
        with log.open("wb") as errors:
            child = subprocess.Popen([relay, cert, key, "0"], env=env,
                                     stdout=subprocess.PIPE, stderr=errors)
            try:
                line = read_readiness(child)
                if line is None:
                    raise RuntimeError("relay exited before readiness")
                match = re.fullmatch(rb"Listening on 127\.0\.0\.1:([0-9]+)\n", line)
                if not match or int(match[1]) == 0:
                    raise RuntimeError(f"unexpected readiness: {line!r}")
                # Every pairing of drafts 16 and 18 and draft 22 with itself,
                # then enough fresh connections to exceed the 16-slot cap
                # cumulatively and exercise retirement.
                drafts = ("16", "18")
                pairings = [(pub, sub) for pub in drafts for sub in drafts]
                pairings += [("22", "22")]
                pairings += [("18", "18")] * 7
                for pub, sub in pairings:
                    result = subprocess.run([peers, match[1].decode(), pub, sub],
                                            env=env, capture_output=True, text=True, timeout=20)
                    print(result.stdout, end="")
                    if result.returncode != 0 or not result.stdout.startswith("PASS:"):
                        raise RuntimeError(f"peer result {result.returncode}: {result.stderr}")
                    if child.poll() is not None:
                        raise RuntimeError("relay exited during campaign")
                child.send_signal(signal.SIGTERM)
                if child.wait(timeout=10) != 0:
                    raise RuntimeError("relay shutdown failed")
                print("PASS: all draft pairs, 24 connections, clean relay shutdown")
            except BaseException:
                print(log.read_text(errors="replace"), file=sys.stderr)
                raise
            finally:
                stop(child)


if __name__ == "__main__":
    main()
