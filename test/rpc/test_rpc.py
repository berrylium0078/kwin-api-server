#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# test/rpc/test_rpc.py — end-to-end test of the JSON-RPC window-claim token
# protocol (doc/RPC.md §2) in a REAL systemd + KDE (KWin) desktop session.
#
# It starts the real service through run.py (`./run.py --no-follow`, the
# systemd user unit kwin-api-server.service), then:
#
#   * plays several JSON-RPC clients over the unix socket
#     ($XDG_RUNTIME_DIR/kwin-api-server/service.socket), and
#   * creates / renames / closes REAL windows with PyQt6 (Wayland), one per
#     client, driving the token protocol through its actual window events.
#
# Covered boundary cases (doc/RPC.md §2):
#   1. token.request -> token (fixed 36-char UUID length)
#   2. window caption gets the token prefix -> token.validated (window info)
#   3. never validated within timeout -> token.invalidated reason "timeout"
#   4. bound window closed            -> token.invalidated reason "window_closed"
#   5. a second window shares the prefix -> token.invalidated reason "ambiguous"
#   6. another token claims the same window -> old token invalidated
#      reason "superseded" (possibly owned by a different client)
#   7. free rename after validation does NOT invalidate the token
#   8. multi-client isolation (each client only hears about its own tokens)
#   9. disconnect cleanup: a disconnected client's tokens are dropped, so a
#      later claim of the same window is clean
#  10. invalid params -> -32602; unknown method -> -32601
#
# Requirements: a running systemd user manager, a session bus and the real
# KWin of a Plasma session (org.kde.KWin must be owned), plus PyQt6. The
# daemon must be built and staged (`./run.py --build` once, or pass --build).
#
# Usage:
#   python3 test/rpc/test_rpc.py [--build] [--keep-running]
#
# On exit the service is stopped and the user is reminded to remove the
# `systemctl --user link` (~/.config/systemd/user/kwin-api-server.service).

import argparse
import json
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

from PyQt6.QtCore import QTimer
from PyQt6.QtWidgets import QApplication, QWidget

ROOT = Path(__file__).resolve().parents[2]
RUN_PY = ROOT / "run.py"
UNIT_NAME = "kwin-api-server.service"
SERVICE_NAME = "org.example.KwinApiServer"
XDG_RUNTIME = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}")
SOCKET_PATH = XDG_RUNTIME / "kwin-api-server" / "service.socket"

TOKEN_LENGTH = 36
UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
# Generous: window mapping + event delivery take a few seconds in this
# environment (see the dev notes), so every wait must tolerate that.
WAIT_TIMEOUT = 60.0

RESULT = {"passed": 0, "failed": 0, "failures": []}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, *, check=True, timeout=120, capture=True):
    return subprocess.run([str(c) for c in cmd], capture_output=capture,
                          text=capture, check=check, timeout=timeout)


def systemctl(*args, check=True, timeout=60):
    return run(["systemctl", "--user", *args], check=check, timeout=timeout)


# ---------------------------------------------------------------------------
# framing (PROTOCOL.md §2.2: uint32 native-endian length + UTF-8 JSON)
# ---------------------------------------------------------------------------

def frame(payload: bytes) -> bytes:
    return struct.pack("=I", len(payload)) + payload


# ---------------------------------------------------------------------------
# Qt main-thread window manager
# ---------------------------------------------------------------------------

class WindowManager:
    """Window operations must run on the Qt main thread (Wayland). The socket
    worker thread submits commands through a queue; the main thread's timer
    tick drains it."""

    def __init__(self):
        self._cmd = queue.Queue()
        self._windows = {}
        self._next = 0
        self._lock = threading.Lock()

    def create(self, title):
        with self._lock:
            handle = self._next
            self._next += 1
        self._cmd.put(("create", handle, title))
        return handle

    def rename(self, handle, title):
        self._cmd.put(("rename", handle, title))

    def close(self, handle):
        self._cmd.put(("close", handle))

    def pump(self):
        try:
            while True:
                op = self._cmd.get_nowait()
                if op[0] == "create":
                    _, handle, title = op
                    w = QWidget()
                    w.setWindowTitle(title)
                    w.resize(240, 150)
                    w.show()
                    self._windows[handle] = w
                elif op[0] == "rename":
                    _, handle, title = op
                    if handle in self._windows:
                        self._windows[handle].setWindowTitle(title)
                elif op[0] == "close":
                    _, handle = op
                    w = self._windows.pop(handle, None)
                    if w is not None:
                        w.close()
        except queue.Empty:
            pass


# ---------------------------------------------------------------------------
# JSON-RPC client over the unix socket
# ---------------------------------------------------------------------------

class RpcClient:
    """One socket client: sends requests, a reader thread collects responses
    (by id) and server notifications (by method)."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(str(SOCKET_PATH))
        self._send_lock = threading.Lock()
        self._cond = threading.Condition()
        self._responses = {}
        self._notifications = []
        self._next_id = 0
        self._closed = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        buf = b""
        while not self._closed:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            buf += data
            while True:
                if len(buf) < 4:
                    break
                (n,) = struct.unpack("=I", buf[:4])
                if len(buf) < 4 + n:
                    break
                payload = buf[4:4 + n]
                buf = buf[4 + n:]
                try:
                    msg = json.loads(payload.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    continue
                with self._cond:
                    if "method" in msg:
                        self._notifications.append(msg)
                    elif "id" in msg:
                        self._responses[msg["id"]] = msg
                    self._cond.notify_all()

    def _send_request(self, method, params):
        rid = self._next_id
        self._next_id += 1
        body = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params}
        with self._send_lock:
            self.sock.sendall(frame(json.dumps(body).encode("utf-8")))
        deadline = time.monotonic() + WAIT_TIMEOUT
        with self._cond:
            while rid not in self._responses:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"no response to {method} within {WAIT_TIMEOUT}s")
                self._cond.wait(remaining)
            return self._responses.pop(rid)

    def request_ok(self, method, params=None):
        msg = self._send_request(method, params if params is not None else {})
        if "error" in msg:
            raise AssertionError(f"{method} returned error: {msg['error']}")
        return msg.get("result")

    def request_error(self, method, params=None, code=-32602):
        msg = self._send_request(method, params if params is not None else {})
        if "error" not in msg:
            raise AssertionError(f"expected error {code} from {method}, got result {msg}")
        assert msg["error"]["code"] == code, \
            f"expected error code {code}, got {msg['error']}"
        return msg["error"]

    def request_token(self, timeout_ms=60000):
        result = self.request_ok("token.request", {"timeout": timeout_ms})
        token = result["token"]
        assert len(token) == TOKEN_LENGTH, f"token length {len(token)} != {TOKEN_LENGTH}"
        assert UUID_RE.match(token), f"token not UUID-shaped: {token!r}"
        return token

    def wait_notification(self, predicate, what, timeout=WAIT_TIMEOUT):
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for i, n in enumerate(self._notifications):
                    if predicate(n):
                        return self._notifications.pop(i)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(
                        f"timeout waiting for {what}; pending notifications: "
                        f"{self._notifications}")
                self._cond.wait(min(remaining, 0.5))

    def wait_token_validated(self, token, timeout=WAIT_TIMEOUT):
        return self.wait_notification(
            lambda n: n.get("method") == "token.validated"
            and n.get("params", {}).get("token") == token,
            f"token.validated({token})", timeout)

    def wait_token_invalidated(self, token, reason, timeout=WAIT_TIMEOUT):
        return self.wait_notification(
            lambda n: n.get("method") == "token.invalidated"
            and n.get("params", {}).get("token") == token
            and n.get("params", {}).get("reason") == reason,
            f"token.invalidated({token}, {reason})", timeout)

    def drain_notifications(self):
        with self._cond:
            out = self._notifications
            self._notifications = []
            return out

    def close(self):
        self._closed = True
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# the tests (run on the worker thread)
# ---------------------------------------------------------------------------

def test(name):
    def deco(fn):
        def wrapper(*args):
            try:
                print(f"  RUN   {name}", flush=True)
                fn(*args)
                RESULT["passed"] += 1
                print(f"  PASS  {name}", flush=True)
            except Exception as e:  # noqa: BLE001 - test harness
                RESULT["failed"] += 1
                RESULT["failures"].append((name, e))
                print(f"  FAIL  {name}: {type(e).__name__}: {e}", flush=True)
        return wrapper
    return deco


@test("basic: request token, validate via window title, free rename kept")
def t_basic_validation(wm):
    c = RpcClient()
    try:
        token = c.request_token(60000)
        handle = wm.create(token)                      # title == token exactly
        validated = c.wait_token_validated(token)
        win = validated["params"]["window"]
        assert win["caption"].startswith(token), f"caption mismatch: {win}"
        assert win["pid"] == os.getpid(), f"pid mismatch: {win}"
        assert len(win["internalId"]) == TOKEN_LENGTH, f"bad internalId: {win}"
        # after validation the client may rename the window freely
        wm.rename(handle, "my nice title (no token prefix anymore)")
        time.sleep(5)                                  # give the server a chance
        leftovers = c.drain_notifications()
        assert not leftovers, f"unexpected notifications after free rename: {leftovers}"
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("timeout: token never validated -> invalidated(timeout)")
def t_validate_timeout(wm):
    c = RpcClient()
    try:
        token = c.request_token(1500)                  # short validate window
        c.wait_token_invalidated(token, "timeout", timeout=20)
    finally:
        c.close()


@test("window closed: bound window destroyed -> invalidated(window_closed)")
def t_window_closed(wm):
    c = RpcClient()
    try:
        token = c.request_token(60000)
        handle = wm.create(token)
        c.wait_token_validated(token)
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("ambiguous: a second window shares the prefix -> invalidated(ambiguous)")
def t_ambiguous(wm):
    c = RpcClient()
    try:
        token = c.request_token(60000)
        handle_a = wm.create(token)
        c.wait_token_validated(token)
        handle_b = wm.create(token)                    # second window, same prefix
        c.wait_token_invalidated(token, "ambiguous")
        wm.close(handle_a)
        wm.close(handle_b)
    finally:
        c.close()


@test("superseded: another client claims the same window")
def t_superseded_cross_client(wm):
    a = RpcClient()
    b = RpcClient()
    try:
        token_a = a.request_token(60000)
        handle = wm.create(token_a)
        a.wait_token_validated(token_a)

        token_b = b.request_token(60000)
        wm.rename(handle, token_b)                     # B claims A's window
        b.wait_token_validated(token_b)
        inv = a.wait_token_invalidated(token_a, "superseded")
        assert inv["params"]["window"]["internalId"], "window missing in superseded"
        # The window now belongs to B: closing it must notify B (not A — A's
        # token is gone).
        wm.close(handle)
        b.wait_token_invalidated(token_b, "window_closed")
        time.sleep(4)
        leftovers_a = a.drain_notifications()
        assert not leftovers_a, f"A got unexpected notifications: {leftovers_a}"
    finally:
        a.close()
        b.close()


@test("multi-client isolation: each client only hears about its own token")
def t_multi_client_isolation(wm):
    a = RpcClient()
    b = RpcClient()
    try:
        token_a = a.request_token(60000)
        token_b = b.request_token(60000)
        handle_a = wm.create(token_a)
        handle_b = wm.create(token_b)
        a.wait_token_validated(token_a)
        b.wait_token_validated(token_b)
        # close A's window: only A must be notified
        wm.close(handle_a)
        a.wait_token_invalidated(token_a, "window_closed")
        time.sleep(4)
        assert not b.drain_notifications(), "B must not hear about A's window"
        # B's token is still valid afterwards
        wm.close(handle_b)
        b.wait_token_invalidated(token_b, "window_closed")
    finally:
        a.close()
        b.close()


@test("disconnect cleanup: a dead client's token is dropped, window reclaimable")
def t_disconnect_cleanup(wm):
    a = RpcClient()
    b = RpcClient()
    try:
        token_a = a.request_token(60000)
        handle = wm.create(token_a)
        a.wait_token_validated(token_a)
        a.close()                                      # A disappears

        # B claims the same window cleanly: no stale token from A interferes
        token_b = b.request_token(60000)
        wm.rename(handle, token_b)
        b.wait_token_validated(token_b)
        time.sleep(4)
        assert not b.drain_notifications(), "B got spurious notifications"
        wm.close(handle)
        b.wait_token_invalidated(token_b, "window_closed")
    finally:
        a.close()
        b.close()


@test("errors: invalid params -> -32602, unknown method -> -32601")
def t_rpc_errors(wm):
    c = RpcClient()
    try:
        for bad in (-1, 0, "x", None, {}):
            err = c.request_error("token.request", {"timeout": bad}, -32602)
            assert "invalid params" in err["message"], err
        err = c.request_error("token.request", {}, -32602)
        assert "invalid params" in err["message"], err
        err = c.request_error("bogus.method", {}, -32601)
        assert err["message"] == "Method not found", err
    finally:
        c.close()


@test("tokens: every request yields the same-length UUID token")
def t_token_lengths(wm):
    c = RpcClient()
    try:
        tokens = [c.request_token(60000) for _ in range(5)]
        assert len({len(t) for t in tokens}) == 1 and len(tokens[0]) == TOKEN_LENGTH
        assert len(set(tokens)) == 5, "tokens must be unique"
    finally:
        c.close()


# ---------------------------------------------------------------------------
# harness: Qt main loop + worker thread + service lifecycle
# ---------------------------------------------------------------------------

def service_active():
    r = systemctl("is-active", UNIT_NAME, check=False)
    return r.stdout.strip() == "active"


def kwin_owned():
    r = run(["busctl", "--user", "--no-pager", "status", "org.kde.KWin"],
            check=False)
    return r.returncode == 0


def start_service(build):
    args = [sys.executable, str(RUN_PY), "--no-follow"]
    if build:
        args.append("--build")
    print(f"[service] {' '.join(str(a) for a in args)}", flush=True)
    subprocess.run(args, check=True, timeout=600)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if SOCKET_PATH.is_socket() and service_active():
            time.sleep(1.0)   # let the daemon load/run the script in KWin
            return
        time.sleep(0.5)
    die(f"{UNIT_NAME} did not become ready (socket at {SOCKET_PATH}); "
        "run `./run.py --build` first and check the journal")


def stop_service():
    print("[service] stopping the service", flush=True)
    systemctl("stop", UNIT_NAME, check=False)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and service_active():
        time.sleep(0.1)
    print("[service] stopped", flush=True)


def run_all_tests(wm, done):
    try:
        t_basic_validation(wm)
        t_validate_timeout(wm)
        t_window_closed(wm)
        t_ambiguous(wm)
        t_superseded_cross_client(wm)
        t_multi_client_isolation(wm)
        t_disconnect_cleanup(wm)
        t_rpc_errors(wm)
        t_token_lengths(wm)
        print("[worker] all tests done", flush=True)
    except Exception as e:  # noqa: BLE001
        import traceback
        RESULT["failed"] += 1
        RESULT["failures"].append(("harness", e))
        print(f"  FAIL  harness: {type(e).__name__}: {e}", flush=True)
        traceback.print_exc()
    finally:
        done.set()


def main():
    parser = argparse.ArgumentParser(
        description="E2E test of the window-claim token protocol (real "
                    "systemd + KWin + PyQt6).")
    parser.add_argument("--build", action="store_true",
                        help="rebuild + staged-install first (forwards to run.py)")
    parser.add_argument("--keep-running", action="store_true",
                        help="leave the service running on exit (debugging)")
    args = parser.parse_args()

    if not kwin_owned():
        die("org.kde.KWin is not owned on the session bus — this test needs "
            "a Plasma session with KWin running")
    print("[kwin] org.kde.KWin is owned on the session bus")

    app = QApplication(sys.argv)
    # Closing the last test window must not quit the Qt loop (the default
    # quitOnLastWindowClosed would stop the event loop — and with it the
    # window-command pump — while the worker is still running).
    app.setQuitOnLastWindowClosed(False)
    wm = WindowManager()

    try:
        start_service(args.build)
    except Exception as e:  # noqa: BLE001
        die(f"startup failed: {e}")

    print("=" * 72)
    print(f"test/rpc: window-claim token protocol (unit {UNIT_NAME})")
    print("=" * 72)

    done = threading.Event()
    worker = threading.Thread(target=run_all_tests, args=(wm, done), daemon=True)
    worker.start()

    # Qt main loop: drain window commands and quit when the worker finished.
    def tick():
        wm.pump()
        if done.is_set():
            wm.pump()
            app.quit()
    timer = QTimer()
    timer.timeout.connect(tick)
    timer.start(20)

    app.exec()
    worker.join(timeout=30)

    passed, failed = RESULT["passed"], RESULT["failed"]
    print("=" * 72)
    print(f"PASSED: {passed}   FAILED: {failed}")
    if RESULT["failures"]:
        print("Failures:")
        for name, exc in RESULT["failures"]:
            print(f"  - {name}: {exc}")
    print("=" * 72)

    if not args.keep_running:
        stop_service()
        print()
        print("Note: the unit file created by `systemctl --user link` is still "
              "present:")
        print(f"    {Path.home() / '.config' / 'systemd' / 'user' / UNIT_NAME}")
        print("To remove it completely later, run manually:")
        print("    rm ~/.config/systemd/user/kwin-api-server.service")
        print("    systemctl --user daemon-reload")
    else:
        print(f"[keep-running] {UNIT_NAME} left running; stop it with:")
        print(f"    systemctl --user stop {UNIT_NAME}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        print("interrupted")
        try:
            stop_service()
        except Exception:  # noqa: BLE001
            pass
        sys.exit(130)
