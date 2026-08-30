#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_native_host.py — end-to-end test of the Firefox native messaging host.

Plays both peers of kwin-api-host.py:

  * the *browser* side: the host as a subprocess with stdin/stdout pipes
    speaking Firefox's native messaging framing, and
  * the *daemon* side: a mock unix socket speaking kwin-api-server's framing
    (doc/PROTOCOL.md §2.2) that answers JSON-RPC requests.

Both framings are identical (4-byte native-endian length + UTF-8 JSON), which
is exactly what the host relies on — so this test proves the whole bridge
without needing Firefox or KWin.

Usage: python3 test_native_host.py
"""

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HOST = Path(__file__).resolve().parent / "kwin-api-host.py"

RESULTS = {"passed": 0, "failed": 0, "failures": []}


def check(cond, what):
    if cond:
        RESULTS["passed"] += 1
    else:
        RESULTS["failed"] += 1
        RESULTS["failures"].append(what)
        print(f"  FAIL: {what}")


# ---------------------------------------------------------------------------
# framing helpers (shared by both protocols under test)
# ---------------------------------------------------------------------------

def frame(payload: bytes) -> bytes:
    return struct.pack("=I", len(payload)) + payload


def _read_some(f, n):
    """Read up to n bytes from a file or socket; b"" on EOF."""
    if hasattr(f, "recv"):
        return f.recv(n)
    return f.read(n)


def read_exact(f, n):
    buf = b""
    while len(buf) < n:
        chunk = _read_some(f, n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(f):
    """Read one length-prefixed frame from a file/socket; None on EOF."""
    hdr = read_exact(f, 4)
    if hdr is None:
        return None
    (n,) = struct.unpack("=I", hdr)
    body = read_exact(f, n)
    if body is None:
        return None
    return json.loads(body.decode("utf-8"))


def write_frame(f, obj):
    payload = json.dumps(obj).encode("utf-8")
    if hasattr(f, "sendall"):
        f.sendall(frame(payload))
    else:
        f.write(frame(payload))
        f.flush()


# ---------------------------------------------------------------------------
# mock daemon: a unix socket that speaks the kwin-api-server framing
# ---------------------------------------------------------------------------

class MockDaemon:
    """Serves one connection. On each request frame pops the next canned
    response (if any), then sends the optional notification once."""

    def __init__(self, sock_path, responses, notification=None, on_request=None,
                 close_after=None):
        self.sock_path = sock_path
        self.responses = list(responses)
        self.notification = notification
        self.on_request = on_request
        self.close_after = close_after  # close the connection after N requests
        self.requests = []
        self.ready = threading.Event()  # set once listen() succeeded
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        # The host must not connect before the mock bound the socket.
        self.ready.wait(timeout=5)
        if self.error:
            raise self.error
        return self

    def _run(self):
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            srv.bind(self.sock_path)
            srv.listen(1)
            self.ready.set()
            conn, _ = srv.accept()
            with conn:
                while True:
                    req = read_frame(conn)
                    if req is None:
                        break
                    self.requests.append(req)
                    if self.on_request:
                        self.on_request(req)
                    if self.responses:
                        write_frame(conn, self.responses.pop(0))
                    if self.notification is not None:
                        write_frame(conn, self.notification)
                        self.notification = None
                    if self.close_after is not None and \
                            len(self.requests) >= self.close_after:
                        break  # simulate the daemon going away
        except Exception as exc:  # surface mock-side failures in the main thread
            self.error = exc
            self.ready.set()
            raise
        finally:
            srv.close()
            try:
                os.unlink(self.sock_path)
            except FileNotFoundError:
                pass


# ---------------------------------------------------------------------------
# host process helpers
# ---------------------------------------------------------------------------

def start_host(sock_path):
    env = dict(os.environ)
    env["KWIN_API_SOCKET"] = sock_path
    return subprocess.Popen(
        [sys.executable, str(HOST)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=env,
    )


def send(proc, obj):
    proc.stdin.write(frame(json.dumps(obj).encode("utf-8")))
    proc.stdin.flush()


def recv(proc, timeout=10.0):
    """Read one frame from the host's stdout (blocking with a timeout).

    Reads via the raw pipe fd: mixing select() with proc.stdout's internal
    buffer would deadlock once the buffer holds data the kernel pipe no
    longer has.
    """
    import os
    import select

    fd = proc.stdout.fileno()

    def _read(n):
        buf = b""
        while len(buf) < n:
            ready, _, _ = select.select([fd], [], [], timeout)
            if not ready:
                raise TimeoutError("timed out waiting for host output")
            chunk = os.read(fd, n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    try:
        hdr = _read(4)
        if hdr is None:
            return None
        (n,) = struct.unpack("=I", hdr)
        body = _read(n)
        if body is None:
            return None
        return json.loads(body.decode("utf-8"))
    except TimeoutError:
        # Help debugging: show what the host said on stderr before giving up.
        print(f"  [host stderr while waiting: {proc.stderr.read(4096)!r}]")
        raise


# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------

def test_round_trip(tmp):
    print("test: request -> daemon -> response -> stdout")
    sock = str(tmp / "roundtrip.sock")
    req = {"jsonrpc": "2.0", "id": 7, "method": "workspace.query",
           "params": [["currentDesktop", "desktops"]]}
    resp = {"jsonrpc": "2.0", "id": 7,
            "result": {"currentDesktop": "d1", "desktops": ["d1", "d2"]}}
    daemon = MockDaemon(sock, [resp]).start()
    proc = start_host(sock)
    send(proc, req)
    out = recv(proc)
    check(out == resp, f"round trip reply mismatch: {out!r}")
    check(daemon.requests == [req], f"daemon saw wrong request: {daemon.requests!r}")
    proc.stdin.close()
    proc.wait(timeout=5)
    check(proc.returncode == 0, f"host exit code {proc.returncode} after stdin EOF")


def test_notification_forwarding(tmp):
    print("test: daemon-initiated notification is forwarded to Firefox")
    sock = str(tmp / "notify.sock")
    req = {"jsonrpc": "2.0", "id": 1, "method": "workspace.watch",
           "params": [{"currentDesktop": True}]}
    resp = {"jsonrpc": "2.0", "id": 1, "result": {"currentDesktop": True}}
    notif = {"jsonrpc": "2.0", "method": "workspace.changed",
             "params": {"property": "currentDesktop", "value": "d2"}}
    daemon = MockDaemon(sock, [resp], notification=notif).start()
    proc = start_host(sock)
    send(proc, req)
    out1 = recv(proc)
    out2 = recv(proc)
    check(out1 == resp, f"first frame mismatch: {out1!r}")
    check(out2 == notif, f"notification not forwarded: {out2!r}")
    proc.stdin.close()
    proc.wait(timeout=5)


def test_daemon_eof_exits_host(tmp):
    print("test: daemon disconnect ends the host (exit 0)")
    sock = str(tmp / "eof.sock")
    req = {"jsonrpc": "2.0", "id": 2, "method": "workspace.query",
           "params": [["desktops"]]}
    resp = {"jsonrpc": "2.0", "id": 2, "result": {"desktops": ["d1"]}}
    daemon = MockDaemon(sock, [resp], close_after=1).start()
    proc = start_host(sock)
    send(proc, req)
    out = recv(proc)
    check(out == resp, f"reply mismatch: {out!r}")
    daemon.thread.join(timeout=5)  # mock closes the connection after the reply
    proc.wait(timeout=5)
    check(proc.returncode == 0, f"host exit code {proc.returncode} after daemon EOF")


def test_no_reply_request(tmp):
    print("test: id-null request is forwarded, no reply is expected")
    sock = str(tmp / "notify2.sock")
    req = {"jsonrpc": "2.0", "id": None, "method": "workspace.update",
           "params": [{"currentDesktop": "d2"}]}
    daemon = MockDaemon(sock, []).start()
    proc = start_host(sock)
    send(proc, req)
    daemon.thread.join(timeout=5)
    check(daemon.requests == [req], f"daemon saw wrong request: {daemon.requests!r}")
    proc.stdin.close()
    proc.wait(timeout=5)
    check(proc.returncode == 0, f"host exit code {proc.returncode} after stdin EOF")


def main():
    print(f"host under test: {HOST}")
    with tempfile.TemporaryDirectory(prefix="kwin-api-host-test-") as td:
        tmp = Path(td)
        test_round_trip(tmp)
        test_notification_forwarding(tmp)
        test_daemon_eof_exits_host(tmp)
        test_no_reply_request(tmp)

    print()
    if RESULTS["failed"]:
        print(f"FAILED: {RESULTS['failed']}/{RESULTS['failed'] + RESULTS['passed']} "
              f"checks failed")
        for f in RESULTS["failures"]:
            print(f"  - {f}")
        return 1
    print(f"ALL TESTS PASSED ({RESULTS['passed']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
