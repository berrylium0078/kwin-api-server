#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End-to-end integration test for kwin-api-daemon.
#
# Runs the real daemon against a private session bus with a mock org.kde.KWin
# (kwin-api-mock) and verifies the whole lifecycle:
#
#   socket bind + line protocol (concurrent clients)
#   D-Bus name registration (--check)
#   script staging (kwinscript.js) + loadScript/run
#   graceful shutdown -> unloadScript + socket cleanup
#
# Usage: test/integration.sh          (after `xmake`)
# Env:   BIN_DIR=/path/to/binaries    (default: auto-detected from build/)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Locate the built binaries (build/linux/<arch>/<mode> by default).
if [ -n "${BIN_DIR:-}" ]; then
    DAEMON="${DAEMON:-$BIN_DIR/kwin-api-daemon}"
    MOCK="${MOCK:-$BIN_DIR/kwin-api-mock}"
else
    DAEMON_BIN="$(find "$ROOT/build" -type f -name kwin-api-daemon 2>/dev/null | head -1)"
    [ -n "$DAEMON_BIN" ] || { echo "error: kwin-api-daemon not built; run 'xmake' first" >&2; exit 1; }
    BIN_DIR="$(dirname "$DAEMON_BIN")"
    DAEMON="$DAEMON_BIN"
    MOCK="$BIN_DIR/kwin-api-mock"
fi

if [ ! -x "$DAEMON" ] || [ ! -x "$MOCK" ]; then
    echo "error: daemon/mock not built (looked in '$BIN_DIR'); run 'xmake' first" >&2
    exit 1
fi
command -v dbus-daemon >/dev/null 2>&1 || { echo "error: dbus-daemon not found" >&2; exit 1; }
command -v socat >/dev/null 2>&1 || { echo "error: socat not found (used to talk to the unix socket)" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/kas-integration.XXXXXX")"
LOG="$WORK/logs"
mkdir -p "$LOG"

DAEMON_PID=""
MOCK_PID=""
DBUS_PID=""

cleanup() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    [ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "integration test FAILED: $1" >&2
    echo "--- daemon.log ---" >&2
    [ -f "$LOG/daemon.log" ] && cat "$LOG/daemon.log" >&2 || true
    echo "--- mock.log ---" >&2
    [ -f "$LOG/mock.log" ] && cat "$LOG/mock.log" >&2 || true
    exit 1
}

# ---------------------------------------------------------------------------
# The sandbox/system may have a read-only /run/user; give dbus-daemon (and the
# daemon's XDG_RUNTIME_DIR fallback) a writable runtime directory of its own.
mkdir -p "$WORK/runtime"
export XDG_RUNTIME_DIR="$WORK/runtime"

echo "==> starting private session bus"
dbus-daemon --session --print-address=1 --print-pid=1 --fork > "$LOG/bus.txt"
DBUS_ADDR="$(sed -n '1p' "$LOG/bus.txt")"
DBUS_PID="$(sed -n '2p' "$LOG/bus.txt")"
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDR"

# ---------------------------------------------------------------------------
echo "==> starting mock KWin (org.kde.KWin)"
"$MOCK" > "$LOG/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 200); do
    grep -q "READY" "$LOG/mock.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "READY" "$LOG/mock.log" || fail "mock KWin did not come up"

# ---------------------------------------------------------------------------
echo "==> daemon --check (socket + D-Bus name, no KWin interaction)"
KWIN_API_SERVICE_NAME=org.example.KwinApiTest \
    "$DAEMON" --check > "$LOG/daemon-check.log" 2>&1 || fail "--check exited non-zero"
grep -q "check ok" "$LOG/daemon-check.log" || fail "--check did not report ok"

# ---------------------------------------------------------------------------
echo "==> fake kwinscript bundle + daemon run"
# The bundle carries the @DAEMON_DBUS_SERVICE@ placeholder; the daemon must
# rewrite it to its runtime service name while staging kwinscript.js.
printf 'print("[mock] kwinscript loaded service=@DAEMON_DBUS_SERVICE@");\n' > "$WORK/kwinscript.js"

# `exec` is important: $! must be the daemon's PID, not the subshell's.
(
    cd "$WORK"
    exec env \
        KWIN_API_SERVICE_NAME=org.example.KwinApiTest \
        KWIN_SCRIPT_PATH="$WORK/kwinscript.js" \
        KWIN_PLUGIN_NAME=kwin-api-server-test \
        KWIN_LOAD_RETRIES=5 \
        "$DAEMON" > "$LOG/daemon.log" 2>&1
) &
DAEMON_PID=$!

# wait until the daemon called loadScript on the mock
for _ in $(seq 1 300); do
    grep -q "CALL loadScript" "$LOG/mock.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "CALL loadScript" "$LOG/mock.log" || fail "daemon never called loadScript"

# staged copy + socket must exist in the working directory
[ -f "$WORK/kwinscript.js" ] || fail "staged kwinscript.js missing"
[ -S "$WORK/service.socket" ] || fail "service.socket missing"

# the daemon must have substituted the @DAEMON_DBUS_SERVICE@ placeholder with
# its runtime service name in the staged copy
grep -q "service=org.example.KwinApiTest" "$WORK/kwinscript.js" \
    || fail "placeholder @DAEMON_DBUS_SERVICE@ not substituted in staged script"

# the mock must have seen the absolute path of the staged file + plugin name
grep -q "CALL loadScript .*/kwinscript.js kwin-api-server-test" "$LOG/mock.log" \
    || fail "loadScript args wrong (see mock.log)"

# and the daemon must have run the script
for _ in $(seq 1 100); do
    grep -q "CALL run" "$LOG/mock.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "CALL run" "$LOG/mock.log" || fail "daemon never called Script.run"

# ---------------------------------------------------------------------------
echo "==> daemon log() D-Bus method (script-facing /daemon object)"
dbus-send --session --print-reply \
    --dest=org.example.KwinApiTest /daemon \
    org.example.KwinApiTest.log string:info string:hello-from-integration-test \
    > /dev/null 2>&1 || fail "dbus-send call to daemon log() failed"
for _ in $(seq 1 100); do
    grep -q "\[script\] hello-from-integration-test" "$LOG/daemon.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "\[script\] hello-from-integration-test" "$LOG/daemon.log" \
    || fail "daemon did not log the script message"

# ---------------------------------------------------------------------------
echo "==> unix socket line protocol (two concurrent clients)"
PING_A="$(printf 'ping\nquit\n' | socat - UNIX-CONNECT:"$WORK/service.socket" 2>/dev/null)"
[ "$PING_A" = "pong" ] || fail "ping/pong failed on client A (got: '$PING_A')"
PING_B="$(printf 'ping\nquit\n' | socat - UNIX-CONNECT:"$WORK/service.socket" 2>/dev/null)"
[ "$PING_B" = "pong" ] || fail "ping/pong failed on client B (got: '$PING_B')"
STATUS="$(printf 'status\nquit\n' | socat - UNIX-CONNECT:"$WORK/service.socket" 2>/dev/null)"
[ "$STATUS" = "service=org.example.KwinApiTest" ] || fail "status reply wrong (got: '$STATUS')"

# ---------------------------------------------------------------------------
echo "==> graceful shutdown (SIGTERM) -> unloadScript + cleanup"
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID" || fail "daemon exited non-zero"
DAEMON_PID=""

for _ in $(seq 1 100); do
    grep -q "CALL unloadScript kwin-api-server-test" "$LOG/mock.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "CALL unloadScript kwin-api-server-test" "$LOG/mock.log" \
    || fail "unloadScript not called on SIGTERM"
[ ! -e "$WORK/service.socket" ] || fail "service.socket not removed on shutdown"

echo "integration test passed"
