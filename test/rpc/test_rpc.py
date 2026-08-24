#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# test/rpc/test_rpc.py — end-to-end test of the JSON-RPC application layer
# (doc/RPC.md): the window-claim token protocol (§2) and the window property
# methods windows.update / windows.query / window.watch (§3) in a REAL systemd
# + KDE (KWin) desktop session.
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
#   1. token.request -> token (20-char base64 string)
#   2. window caption gets the token prefix -> token.validated (window info)
#   3. never validated within timeout -> token.invalidated reason "timeout"
#   4. bound window closed            -> token.invalidated reason "window_closed"
#   5. a second window shares the prefix BEFORE the validate deadline
#      -> token.invalidated reason "ambiguous"
#   5b. a second window shares the prefix AFTER the validate deadline
#      -> NOT ambiguous (the token stays valid)
#   6. another token claims the same window -> old token invalidated
#      reason "superseded" (possibly owned by a different client)
#   7. free rename after validation does NOT invalidate the token
#   8. multi-client isolation (each client only hears about its own tokens)
#   9. disconnect cleanup: a disconnected client's tokens are dropped, so a
#      later claim of the same window is clean
#  10. invalid params -> -32602; unknown method -> -32601
#
# And the window-property methods (doc/RPC.md §3), which use by-position
# params ([token, ...]) and treat requests with id null as notifications:
#  11. windows.update / windows.query round-trip on real properties
#      (opacity / onAllDesktops / skipTaskbar / skipPager / keepAbove),
#      query deduplication, void update result (null)
#  12. unsupported property -> -32602, atomically (nothing applied)
#  13. window.watch: enable -> window.changed on change; disable -> silence;
#      per-property listener state in the result
#  14. watch idempotency: a double watch keeps exactly one listener (a single
#      change emits exactly one window.changed)
#  15. watch teardown when the token is superseded: the old owner stops
#      receiving window.changed
#  16. id null: the method runs but no reply is sent
#  17. token resolution errors: unknown / foreign / not-yet-validated token
#      -> -32602
#  18. by-position params of the wrong shape (object instead of array,
#      missing elements, non-boolean watch values) -> -32602
#  19. desktops / activities as ID sets (string[]): query wire format,
#      round-trip update, unknown-ID and non-array values -> -32602,
#      atomically
#  20. watching desktops: a desktop change emits window.changed carrying the
#      new ID set (including the [] <-> [desktop] flip)
#
# And the workspace property methods (doc/RPC.md §4), which take no token:
#  21. workspace.query: current desktop/activity and the full desktop/activity
#      ID lists, deduplication
#  22. workspace.update: switching currentActivity / currentDesktop (verified
#      and restored), unknown-ID and non-string values -> -32602, desktops /
#      activities read-only at the workspace level
#  23. workspace.watch: currentActivity / currentDesktop changes emit
#      workspace.changed with the new ID; idempotent, unwatch silences
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

# Tokens are 20-char base64 strings (doc/RPC.md §2.3).
TOKEN_LENGTH = 20
BASE64_TOKEN_RE = re.compile(r"^[A-Za-z0-9+/]{20}$")
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
        assert BASE64_TOKEN_RE.match(token), f"token not base64({TOKEN_LENGTH}): {token!r}"
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

    def wait_window_changed(self, token, prop, value=None, timeout=WAIT_TIMEOUT):
        """Wait for a window.changed notification (doc/RPC.md §3.4). The value
        is compared with == so that list values (desktops/activities ID sets)
        match too; boolean values are JSON booleans, never 0/1."""
        return self.wait_notification(
            lambda n: n.get("method") == "window.changed"
            and n.get("params", {}).get("token") == token
            and n.get("params", {}).get("property") == prop
            and (value is None or n.get("params", {}).get("value") == value),
            f"window.changed({token}, {prop})", timeout)

    def wait_workspace_changed(self, prop, value=None, timeout=WAIT_TIMEOUT):
        """Wait for a workspace.changed notification (doc/RPC.md §4.4)."""
        return self.wait_notification(
            lambda n: n.get("method") == "workspace.changed"
            and n.get("params", {}).get("property") == prop
            and (value is None or n.get("params", {}).get("value") == value),
            f"workspace.changed({prop})", timeout)

    def send_expect_no_reply(self, method, params, wait=4.0):
        """Send a request with id null (notification-style, doc/RPC.md §1):
        the method must run, but the server must not send a reply. A reply to
        an id-null request would be collected under the None response key."""
        body = {"jsonrpc": "2.0", "id": None, "method": method, "params": params}
        with self._send_lock:
            self.sock.sendall(frame(json.dumps(body).encode("utf-8")))
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            with self._cond:
                if None in self._responses:
                    raise AssertionError(
                        f"server replied to the id-null request {method}: "
                        f"{self._responses[None]}")
            time.sleep(0.05)

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
        # the payload carries only the caption; the client knows its own window
        assert set(win.keys()) == {"caption"}, f"unexpected window fields: {win}"
        assert win["caption"].startswith(token), f"caption mismatch: {win}"
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


@test("deadline passed: a second window matching afterwards is NOT ambiguous")
def t_no_ambiguity_after_deadline(wm):
    c = RpcClient()
    try:
        token = c.request_token(3000)                  # short validate window
        handle_a = wm.create(token)
        c.wait_token_validated(token)
        time.sleep(4)                                  # let the deadline pass
        handle_b = wm.create(token)                    # second window, same prefix
        time.sleep(4)                                  # give the server a chance to react
        leftovers = c.drain_notifications()
        assert not leftovers, \
            f"token was withdrawn after the validate deadline: {leftovers}"
        # the token is still valid on window A
        wm.close(handle_a)
        c.wait_token_invalidated(token, "window_closed")
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
        assert "caption" in inv["params"]["window"], "window missing in superseded"
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


@test("tokens: every request yields the same-length base64 token")
def t_token_lengths(wm):
    c = RpcClient()
    try:
        tokens = [c.request_token(60000) for _ in range(5)]
        assert len({len(t) for t in tokens}) == 1 and len(tokens[0]) == TOKEN_LENGTH
        assert len(set(tokens)) == 5, "tokens must be unique"
    finally:
        c.close()


# ---------------------------------------------------------------------------
# window property methods (doc/RPC.md §3)
# ---------------------------------------------------------------------------

def _claim_window(wm, c):
    """Create a window titled with a fresh token and wait for validation."""
    token = c.request_token(60000)
    handle = wm.create(token)
    c.wait_token_validated(token)
    return token, handle


@test("update/query: round-trip on real properties, dedupe, void update")
def t_update_query_roundtrip(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        # by-position params: [token, updateInfo]
        assert c.request_ok("windows.update", [token, {
            "onAllDesktops": True, "skipTaskbar": True, "opacity": 0.5,
        }]) is None, "windows.update must return null (void)"
        # duplicate names are deduplicated; the result carries each once
        result = c.request_ok("windows.query", [token, [
            "onAllDesktops", "opacity", "onAllDesktops", "skipTaskbar",
            "skipTaskbar", "opacity",
        ]])
        assert set(result.keys()) == {"onAllDesktops", "opacity", "skipTaskbar"}, result
        assert result["onAllDesktops"] is True, result
        assert result["skipTaskbar"] is True, result
        assert abs(result["opacity"] - 0.5) < 1e-9, result
        # empty update / empty query are both valid
        assert c.request_ok("windows.update", [token, {}]) is None
        assert c.request_ok("windows.query", [token, []]) == {}
        # a second update is observable: the values really changed
        c.request_ok("windows.update", [token, {
            "onAllDesktops": False, "opacity": 1.0,
        }])
        result = c.request_ok("windows.query", [token, ["onAllDesktops", "opacity"]])
        assert result["onAllDesktops"] is False, result
        assert abs(result["opacity"] - 1.0) < 1e-9, result
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("update: unsupported property -> -32602, nothing applied (atomic)")
def t_update_unsupported(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        err = c.request_error("windows.update", [token, {"bogus": 1}], -32602)
        assert "unsupported property" in err["message"], err
        # a mixed call must fail as a whole: the supported part is not applied
        c.request_ok("windows.update", [token, {"onAllDesktops": True}])
        err = c.request_error("windows.update", [token, {"onAllDesktops": False, "bogus": 1}], -32602)
        assert "unsupported property" in err["message"], err
        result = c.request_ok("windows.query", [token, ["onAllDesktops"]])
        assert result == {"onAllDesktops": True}, \
            f"failed update leaked a partial apply: {result}"
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("query: unsupported property -> -32602")
def t_query_unsupported(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        err = c.request_error("windows.query", [token, ["opacity", "bogus"]], -32602)
        assert "unsupported property" in err["message"], err
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: enable -> window.changed on change; disable -> silence")
def t_watch_enable_disable(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        # skipTaskbar starts false; watch it
        assert c.request_ok("window.watch", [token, {"skipTaskbar": True}]) == \
            {"skipTaskbar": True}
        # a real change (via windows.update) fires the watched signal
        c.request_ok("windows.update", [token, {"skipTaskbar": True}])
        n = c.wait_window_changed(token, "skipTaskbar", value=True)
        assert n["params"]["token"] == token, n
        # unwatch: the result says there is no listener any more
        assert c.request_ok("window.watch", [token, {"skipTaskbar": False}]) == \
            {"skipTaskbar": False}
        # another change is now silent
        c.request_ok("windows.update", [token, {"skipTaskbar": False}])
        time.sleep(4)
        leftovers = c.drain_notifications()
        assert not leftovers, f"notifications after unwatch: {leftovers}"
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: idempotent — a double watch keeps a single listener")
def t_watch_idempotent(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        assert c.request_ok("window.watch", [token, {"keepAbove": True}]) == \
            {"keepAbove": True}
        assert c.request_ok("window.watch", [token, {"keepAbove": True}]) == \
            {"keepAbove": True}
        # one change must emit exactly one window.changed (no double connect)
        c.request_ok("windows.update", [token, {"keepAbove": True}])
        c.wait_window_changed(token, "keepAbove", value=True)
        time.sleep(4)
        leftovers = c.drain_notifications()
        assert not leftovers, \
            f"double watch produced duplicate notifications: {leftovers}"
        # unwatching an unwatched property is a no-op (still false)
        assert c.request_ok("window.watch", [token, {"keepAbove": False}]) == \
            {"keepAbove": False}
        assert c.request_ok("window.watch", [token, {"keepAbove": False}]) == \
            {"keepAbove": False}
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: multiple properties, per-property listener state in the result")
def t_watch_multi(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        result = c.request_ok("window.watch", [token, {
            "skipTaskbar": True, "skipPager": True, "keepAbove": False,
        }])
        assert result == {"skipTaskbar": True, "skipPager": True, "keepAbove": False}, result
        # each watched property fires its own notification
        c.request_ok("windows.update", [token, {"skipTaskbar": True, "skipPager": True}])
        c.wait_window_changed(token, "skipTaskbar", value=True)
        c.wait_window_changed(token, "skipPager", value=True)
        # unwatch everything; keepAbove was never watched
        result = c.request_ok("window.watch", [token, {
            "skipTaskbar": False, "skipPager": False, "keepAbove": False,
        }])
        assert result == {"skipTaskbar": False, "skipPager": False, "keepAbove": False}, result
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: unsupported properties / non-boolean values -> -32602")
def t_watch_unsupported(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        # opacity is a number, not a boolean property
        err = c.request_error("window.watch", [token, {"opacity": True}], -32602)
        assert "unsupported property" in err["message"], err
        # onAllDesktops / noBorder have no <prop>Changed signal
        err = c.request_error("window.watch", [token, {"onAllDesktops": True}], -32602)
        assert "unsupported property" in err["message"], err
        err = c.request_error("window.watch", [token, {"noBorder": True}], -32602)
        assert "unsupported property" in err["message"], err
        # unknown property
        err = c.request_error("window.watch", [token, {"bogus": True}], -32602)
        assert "unsupported property" in err["message"], err
        # non-boolean value (rejected by the params schema)
        err = c.request_error("window.watch", [token, {"skipTaskbar": "yes"}], -32602)
        assert "invalid params" in err["message"], err
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: listeners are torn down when the token is superseded")
def t_watch_superseded_cleanup(wm):
    a = RpcClient()
    b = RpcClient()
    try:
        token_a = a.request_token(60000)
        handle = wm.create(token_a)
        a.wait_token_validated(token_a)
        # A watches skipTaskbar (starts false)
        assert a.request_ok("window.watch", [token_a, {"skipTaskbar": True}]) == \
            {"skipTaskbar": True}
        # B claims the same window: A's token — and its watch — is torn down
        token_b = b.request_token(60000)
        wm.rename(handle, token_b)
        b.wait_token_validated(token_b)
        a.wait_token_invalidated(token_a, "superseded")
        # B watches the same property and changes it: only B hears about it
        assert b.request_ok("window.watch", [token_b, {"skipTaskbar": True}]) == \
            {"skipTaskbar": True}
        b.request_ok("windows.update", [token_b, {"skipTaskbar": True}])
        b.wait_window_changed(token_b, "skipTaskbar", value=True)
        time.sleep(4)
        leftovers_a = a.drain_notifications()
        assert not leftovers_a, \
            f"A kept receiving window.changed after supersession: {leftovers_a}"
        wm.close(handle)
        b.wait_token_invalidated(token_b, "window_closed")
    finally:
        a.close()
        b.close()


@test("id null: the method runs but no reply is sent")
def t_id_null_no_reply(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        # notification-style update: no reply, but the update still applies
        c.send_expect_no_reply("windows.update", [token, {"skipTaskbar": True}])
        result = c.request_ok("windows.query", [token, ["skipTaskbar"]])
        assert result == {"skipTaskbar": True}, result
        # the same holds for window.watch (id null, no reply, listener set)
        c.send_expect_no_reply("window.watch", [token, {"skipPager": True}])
        c.request_ok("windows.update", [token, {"skipPager": True}])
        c.wait_window_changed(token, "skipPager", value=True)
        # and an ordinary request still gets a reply (sanity)
        result = c.request_ok("windows.query", [token, ["skipTaskbar"]])
        assert result == {"skipTaskbar": True}, result
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("token resolution: unknown / foreign / pending token -> -32602")
def t_token_resolution_errors(wm):
    a = RpcClient()
    b = RpcClient()
    try:
        # unknown token
        err = a.request_error("windows.update", ["no-such-token", {}], -32602)
        assert "unknown token" in err["message"], err
        # a token that was requested but never validated has no window yet
        token_pending = a.request_token(60000)
        err = a.request_error("windows.query", [token_pending, ["opacity"]], -32602)
        assert "not bound" in err["message"], err
        # a token owned by another client must not grant control
        token_a = a.request_token(60000)
        handle = wm.create(token_a)
        a.wait_token_validated(token_a)
        err = b.request_error("windows.query", [token_a, ["opacity"]], -32602)
        assert "another client" in err["message"], err
        wm.close(handle)
        a.wait_token_invalidated(token_a, "window_closed")
    finally:
        a.close()
        b.close()


@test("by-position params: wrong shape -> -32602")
def t_by_position_params(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        # params must be an array [token, updateInfo], not an object
        err = c.request_error("windows.update",
                              {"token": token, "updateInfo": {}}, -32602)
        assert "invalid params" in err["message"], err
        # missing the updateInfo element
        err = c.request_error("windows.update", [token], -32602)
        assert "invalid params" in err["message"], err
        # token must be a string
        err = c.request_error("windows.update", [123, {}], -32602)
        assert "invalid params" in err["message"], err
        # queryInfo must be an array of strings
        err = c.request_error("windows.query", [token, "opacity"], -32602)
        assert "invalid params" in err["message"], err
        # watch values must be booleans
        err = c.request_error("window.watch", [token, {"skipTaskbar": 1}], -32602)
        assert "invalid params" in err["message"], err
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("desktops/activities: ID wire format, round-trip, unknown-ID rejection")
def t_desktops_activities_ids(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        result = c.request_ok("windows.query", [token, ["desktops", "activities"]])
        # wire format: string[] of IDs (KDE UUIDs); empty means "all"
        for prop in ("desktops", "activities"):
            assert isinstance(result[prop], list), result
            assert all(isinstance(x, str) and x for x in result[prop]), result
        # round-trip: the queried IDs are in the workspace catalogs by
        # construction, so setting them back must succeed
        c.request_ok("windows.update", [token, {
            "desktops": result["desktops"], "activities": result["activities"],
        }])
        again = c.request_ok("windows.query", [token, ["desktops", "activities"]])
        assert again == result, f"desktops/activities round-trip mismatch: {result} -> {again}"
        # unknown IDs are rejected with -32602, atomically (nothing changes)
        err = c.request_error("windows.update", [token, {
            "desktops": ["00000000-0000-0000-0000-000000000000"],
        }], -32602)
        assert "unknown desktop" in err["message"], err
        err = c.request_error("windows.update", [token, {
            "activities": ["00000000-0000-0000-0000-000000000000"],
        }], -32602)
        assert "unknown activity" in err["message"], err
        # non-array values are rejected too
        err = c.request_error("windows.update", [token, {
            "desktops": "00000000-0000-0000-0000-000000000000",
        }], -32602)
        assert "invalid params" in err["message"], err
        still = c.request_ok("windows.query", [token, ["desktops", "activities"]])
        assert still == result, f"rejected update changed the window: {result} -> {still}"
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


@test("watch: desktops changes emit window.changed with the new ID set")
def t_watch_desktops(wm):
    c = RpcClient()
    try:
        token, handle = _claim_window(wm, c)
        initial = c.request_ok("windows.query", [token, ["desktops"]])["desktops"]
        assert isinstance(initial, list) and all(isinstance(x, str) for x in initial)
        assert c.request_ok("window.watch", [token, {"desktops": True}]) == \
            {"desktops": True}
        if initial:
            # flip to all desktops: the notification carries the new ID set ([])
            c.request_ok("windows.update", [token, {"desktops": []}])
            n = c.wait_window_changed(token, "desktops", value=[])
            assert n["params"]["value"] == [], n
            # and back to the original desktop(s)
            c.request_ok("windows.update", [token, {"desktops": initial}])
            n = c.wait_window_changed(token, "desktops", value=initial)
            assert n["params"]["value"] == initial, n
        else:
            # the window was already on all desktops; without a known desktop
            # ID there is nothing to flip to — just make sure watching itself
            # stays quiet until the window is closed
            time.sleep(3)
            leftovers = c.drain_notifications()
            assert not leftovers, leftovers
        assert c.request_ok("window.watch", [token, {"desktops": False}]) == \
            {"desktops": False}
        wm.close(handle)
        c.wait_token_invalidated(token, "window_closed")
    finally:
        c.close()


# ---------------------------------------------------------------------------
# workspace property methods (doc/RPC.md §4)
# ---------------------------------------------------------------------------

@test("workspace: query current desktop/activity and the ID lists")
def t_workspace_query(wm):
    c = RpcClient()
    try:
        # by-position params: [queryInfo]; duplicates are deduplicated
        result = c.request_ok("workspace.query", [[
            "currentDesktop", "currentActivity", "desktops", "activities",
            "currentDesktop",
        ]])
        assert set(result.keys()) == \
            {"currentDesktop", "currentActivity", "desktops", "activities"}, result
        # current values are IDs present in the respective lists
        assert result["currentDesktop"] in result["desktops"], result
        assert result["currentActivity"] in result["activities"], result
        for prop in ("desktops", "activities"):
            assert isinstance(result[prop], list), result
            assert all(isinstance(x, str) and x for x in result[prop]), result
        # empty query -> {}
        assert c.request_ok("workspace.query", [[]]) == {}
        # unsupported property -> -32602
        err = c.request_error("workspace.query", [["bogus"]], -32602)
        assert "unsupported property" in err["message"], err
    finally:
        c.close()


@test("workspace: update currentActivity / currentDesktop, unknown-ID rejection")
def t_workspace_update(wm):
    c = RpcClient()
    try:
        cur = c.request_ok("workspace.query", [["currentActivity", "currentDesktop"]])
        lists = c.request_ok("workspace.query", [["activities", "desktops"]])
        other_activity = next(
            (a for a in lists["activities"] if a != cur["currentActivity"]), None)
        other_desktop = next(
            (d for d in lists["desktops"] if d != cur["currentDesktop"]), None)

        # switch to another known activity/desktop, verify, restore
        if other_activity:
            assert c.request_ok("workspace.update", [{"currentActivity": other_activity}]) is None
            assert c.request_ok("workspace.query", [["currentActivity"]]) == \
                {"currentActivity": other_activity}
            c.request_ok("workspace.update", [{"currentActivity": cur["currentActivity"]}])
        if other_desktop:
            assert c.request_ok("workspace.update", [{"currentDesktop": other_desktop}]) is None
            assert c.request_ok("workspace.query", [["currentDesktop"]]) == \
                {"currentDesktop": other_desktop}
            c.request_ok("workspace.update", [{"currentDesktop": cur["currentDesktop"]}])

        # unknown IDs -> -32602
        err = c.request_error("workspace.update", [{
            "currentDesktop": "00000000-0000-0000-0000-000000000000",
        }], -32602)
        assert "unknown desktop" in err["message"], err
        err = c.request_error("workspace.update", [{
            "currentActivity": "00000000-0000-0000-0000-000000000000",
        }], -32602)
        assert "unknown activity" in err["message"], err
        # non-string value -> -32602
        err = c.request_error("workspace.update", [{"currentDesktop": 123}], -32602)
        assert "invalid params" in err["message"], err
        # desktops/activities are read-only at the workspace level
        for prop in ("desktops", "activities"):
            err = c.request_error("workspace.update", [{prop: []}], -32602)
            assert "unsupported property" in err["message"], err
    finally:
        c.close()


@test("workspace: watch currentActivity/currentDesktop -> workspace.changed")
def t_workspace_watch(wm):
    c = RpcClient()
    try:
        cur = c.request_ok("workspace.query", [["currentActivity", "currentDesktop"]])
        lists = c.request_ok("workspace.query", [["activities", "desktops"]])
        other_activity = next(
            (a for a in lists["activities"] if a != cur["currentActivity"]), None)
        other_desktop = next(
            (d for d in lists["desktops"] if d != cur["currentDesktop"]), None)

        # currentActivity: watch, switch, hear it, unwatch, restore silently
        assert c.request_ok("workspace.watch", [{"currentActivity": True}]) == \
            {"currentActivity": True}
        assert c.request_ok("workspace.watch", [{"currentActivity": True}]) == \
            {"currentActivity": True}   # idempotent: still one listener
        if other_activity:
            c.request_ok("workspace.update", [{"currentActivity": other_activity}])
            n = c.wait_workspace_changed("currentActivity", value=other_activity)
            assert n["params"]["value"] == other_activity, n
        assert c.request_ok("workspace.watch", [{"currentActivity": False}]) == \
            {"currentActivity": False}
        c.request_ok("workspace.update", [{"currentActivity": cur["currentActivity"]}])
        time.sleep(3)
        leftovers = c.drain_notifications()
        assert not leftovers, f"workspace.changed after unwatch: {leftovers}"

        # currentDesktop: same dance
        assert c.request_ok("workspace.watch", [{"currentDesktop": True}]) == \
            {"currentDesktop": True}
        if other_desktop:
            c.request_ok("workspace.update", [{"currentDesktop": other_desktop}])
            n = c.wait_workspace_changed("currentDesktop", value=other_desktop)
            assert n["params"]["value"] == other_desktop, n
        assert c.request_ok("workspace.watch", [{"currentDesktop": False}]) == \
            {"currentDesktop": False}
        c.request_ok("workspace.update", [{"currentDesktop": cur["currentDesktop"]}])
        time.sleep(3)
        leftovers = c.drain_notifications()
        assert not leftovers, f"workspace.changed after unwatch: {leftovers}"
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
        t_no_ambiguity_after_deadline(wm)
        t_superseded_cross_client(wm)
        t_multi_client_isolation(wm)
        t_disconnect_cleanup(wm)
        t_rpc_errors(wm)
        t_token_lengths(wm)
        # window property methods (doc/RPC.md §3)
        t_update_query_roundtrip(wm)
        t_update_unsupported(wm)
        t_query_unsupported(wm)
        t_watch_enable_disable(wm)
        t_watch_idempotent(wm)
        t_watch_multi(wm)
        t_watch_unsupported(wm)
        t_watch_superseded_cleanup(wm)
        t_id_null_no_reply(wm)
        t_token_resolution_errors(wm)
        t_by_position_params(wm)
        t_desktops_activities_ids(wm)
        t_watch_desktops(wm)
        # workspace property methods (doc/RPC.md §4)
        t_workspace_query(wm)
        t_workspace_update(wm)
        t_workspace_watch(wm)
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
    print(f"test/rpc: JSON-RPC application layer (unit {UNIT_NAME})")
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
