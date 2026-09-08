#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# test/daemon/test_daemon.py — end-to-end test of kwin-api-daemon under the
# real systemd user manager.
#
# It drives the real systemd user unit via `systemctl --user` (link ->
# daemon-reload -> start -> stop, the same flow as run.py --build) and talks
# to the daemon over BOTH of its live interfaces at the same time:
#
#   * the unix socket (service.socket) as several concurrent JSONRPC clients,
#   * the session-bus D-Bus objects /daemon and /cli{id} as the KWin script
#     (poll / push / log).
#
# The daemon is started through a *mock service* (kwin-api-server-test.service,
# rendered from kwin-api-server-test.service.in): it runs the real daemon but
# points KWIN_SCRIPT_PATH at the empty test/daemon/empty-kwinscript.js, so the
# loaded script does nothing inside KWin. The mock unit also sets small
# KWIN_RX_BUFFER_CAP / KWIN_TX_BUFFER_CAP / KWIN_MAX_CLIENTS, so the
# backpressure and client-limit behaviour of PROTOCOL.md §2.3 / the session
# table becomes reachable with a few hundred KB and a handful of sockets.
#
# The focus is the D-Bus call semantics of PROTOCOL.md §3 that the C++ unit
# tests cannot cover: poll() batching/FIFO, blocking-timeout behaviour, the
# JSON-encoded poll() error strings vs. the plain-text push() error strings,
# concurrent-poll rejection, object lifetime after disconnect (including a
# pending poll being dropped), write-buffer-full + recovery, and the log()
# surface.
#
# Requirements: a running systemd user manager, a session bus, and the real
# KWin of a Plasma session (org.kde.KWin must be owned on the session bus, so
# the daemon can loadScript the empty script). The daemon must be built
# (`xmake`) with the buffer-cap env vars; the test verifies that via --help.
#
# On exit the service is stopped and the user is reminded to remove the
# `systemctl --user link` (~/.config/systemd/user/kwin-api-server-test.service).
#
# Usage:
#   python3 test/daemon/test_daemon.py [--keep-running]
# Env:
#   KAS_DAEMON_BIN   path of the daemon binary (default: autodetected)

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import dbus
import dbus.bus

# dbus-python logs expected introspection errors (unknown objects etc.) at
# ERROR level; those are exercised deliberately by the tests, so silence them.
import logging
logging.getLogger("dbus.proxies").setLevel(logging.CRITICAL)
logging.getLogger("dbus.connection").setLevel(logging.CRITICAL)

ROOT = Path(__file__).resolve().parents[2]   # repo root (test/daemon -> ..)
TESTDIR = Path(__file__).resolve().parent

UNIT_NAME = "kwin-api-server-test.service"
SERVICE_NAME = "org.example.KwinApiTest"
CLIENT_IFACE = "org.example.KwinApiClient"
STATUS_PATH = "/org/example/KwinApiTest"  # derived from SERVICE_NAME
PLUGIN_NAME = "kwin-api-server-test"
RX_CAP = 65536          # bytes; small on purpose (see module docstring)
TX_CAP = 65536
MAX_CLIENTS = 4

XDG_RUNTIME = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}")
SOCKET_PATH = XDG_RUNTIME / "kwin-api-server-test" / "service.socket"
LINKED_UNIT = Path.home() / ".config" / "systemd" / "user" / UNIT_NAME

BUS = dbus.SessionBus()

# Shared state (filled in by main()):
DAEMON_BIN = None
CURSOR0 = None            # journal cursor taken right before `systemctl start`

RESULT = {"passed": 0, "failed": 0, "failures": []}


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, *, check=True, timeout=30, capture=True):
    """Run a command; returns CompletedProcess (text)."""
    return subprocess.run([str(c) for c in cmd], capture_output=capture,
                          text=capture, check=check, timeout=timeout)


def systemctl(*args, check=True, timeout=30):
    return run(["systemctl", "--user", *args], check=check, timeout=timeout)


def find_binary(name):
    candidates = [
        ROOT / "build" / "linux" / "x86_64" / "release" / name,
        ROOT / "build" / "linux" / "x86_64" / "debug" / name,
        ROOT / "stage" / "bin" / name,
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def fresh_connection():
    """An independent D-Bus connection (dbus.SessionBus() is a shared
    singleton, which is not safe for concurrent calls from worker threads)."""
    addr = os.environ.get("DBUS_SESSION_BUS_ADDRESS")
    if not addr:
        addr = f"unix:path={XDG_RUNTIME}/bus"
    return dbus.bus.BusConnection(addr)


# --- framing (PROTOCOL.md §2.2: uint32 native-endian length + UTF-8 JSON) ---

def frame(payload: bytes) -> bytes:
    return struct.pack("=I", len(payload)) + payload


def connect_client(timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(SOCKET_PATH))
    return s


def send_frame(s, payload: bytes):
    s.sendall(frame(payload))


def recv_frame(s, timeout=5.0):
    """Read one [len][payload] frame; returns payload bytes, or None on EOF."""
    s.settimeout(timeout)
    header = b""
    while len(header) < 4:
        chunk = s.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    (n,) = struct.unpack("=I", header)
    payload = b""
    while len(payload) < n:
        chunk = s.recv(n - len(payload))
        if not chunk:
            return None
        payload += chunk
    return payload


class FrameReader:
    """Frame-aware socket reader: every byte read goes through this parser, so
    nothing is ever lost (useful when draining a socket and later asserting on
    the complete frame stream)."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.frames = []  # completed frames (payload bytes), FIFO

    def _extract(self):
        while len(self.buf) >= 4:
            (n,) = struct.unpack("=I", self.buf[:4])
            if len(self.buf) < 4 + n:
                break
            self.frames.append(self.buf[4:4 + n])
            self.buf = self.buf[4 + n:]

    def read_available(self):
        """Non-blocking read; returns the number of newly completed frames."""
        self.sock.setblocking(False)
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                self.buf += d
        except BlockingIOError:
            pass
        finally:
            self.sock.setblocking(True)
        before = len(self.frames)
        self._extract()
        return len(self.frames) - before

    def read_frame(self, timeout_s=5.0):
        """Block until one complete frame is available; returns its payload."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self.frames or self.read_available() > 0:
                return self.frames.pop(0)
            time.sleep(0.01)
        raise socket.timeout(f"no frame within {timeout_s}s")


# --- D-Bus proxies ----------------------------------------------------------

def daemon_obj():
    return BUS.get_object(SERVICE_NAME, "/daemon", introspect=False)


def cli_obj(cid):
    return BUS.get_object(SERVICE_NAME, f"/cli{cid}", introspect=False)


def status_obj():
    return BUS.get_object(SERVICE_NAME, STATUS_PATH, introspect=False)


def daemon_poll(timeout_ms, call_timeout_s=15):
    return daemon_obj().poll(timeout_ms, dbus_interface=SERVICE_NAME,
                             timeout=call_timeout_s)


def cli_poll(cid, timeout_ms, call_timeout_s=15):
    return cli_obj(cid).poll(timeout_ms, dbus_interface=CLIENT_IFACE,
                             timeout=call_timeout_s)


def cli_push(cid, msg, call_timeout_s=15):
    return cli_obj(cid).push(msg, dbus_interface=CLIENT_IFACE,
                             timeout=call_timeout_s)


def daemon_log(level, msg):
    daemon_obj().log(level, msg, dbus_interface=SERVICE_NAME)


def parse_poll(raw):
    """poll() reply -> list of queued payloads. poll() errors are JSON-encoded
    strings; raise when the reply parses to a string."""
    val = json.loads(str(raw))
    if isinstance(val, str):
        raise AssertionError(f"poll() returned error: {val}")
    return val


def wait_poll_messages(poll_fn, want, timeout_s=5.0):
    """Repeatedly poll (timeout 0) until `want` messages were collected."""
    got = []
    deadline = time.monotonic() + timeout_s
    while len(got) < want and time.monotonic() < deadline:
        got.extend(parse_poll(poll_fn(0)))
        if len(got) < want:
            time.sleep(0.01)
    if len(got) < want:
        raise AssertionError(f"collected {len(got)} messages, wanted {want}")
    return got


def wait_cli_raw(cid, want, timeout_s=5.0):
    """Poll /cli{cid} until the raw reply parses to >= want messages; returns
    (raw_string, parsed_list)."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = str(cli_poll(cid, 0))
        val = json.loads(raw)
        if isinstance(val, list) and len(val) >= want:
            return raw, val
        time.sleep(0.01)
    raise AssertionError(f"/cli{cid} did not yield {want} message(s)")


def drain_daemon():
    """Discard whatever control messages are currently queued on /daemon."""
    parse_poll(daemon_poll(0))


def wait_daemon_events(expected):
    """Wait until every (event_type, cid) pair in `expected` has been seen
    (in any order, possibly across batches). Other events are discarded."""
    want = set(expected)
    seen = set()
    deadline = time.monotonic() + 5.0
    while not want.issubset(seen) and time.monotonic() < deadline:
        for ev in parse_poll(daemon_poll(0)):
            key = (ev.get("event"), ev.get("id"))
            if key in want:
                seen.add(key)
        time.sleep(0.01)
    missing = want - seen
    if missing:
        raise AssertionError(f"missing daemon events: {sorted(missing)}")


def wait_daemon_event(event_type, cid=None, timeout_s=5.0):
    """Wait for a single control event (see wait_daemon_events)."""
    want = {(event_type, cid)} if cid is not None else None
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for ev in parse_poll(daemon_poll(0)):
            if ev.get("event") == event_type and (cid is None or ev.get("id") == cid):
                return ev
        time.sleep(0.01)
    raise AssertionError(f"no {event_type} event (cid={cid}) within {timeout_s}s")


def new_client():
    """Connect one socket client; returns (socket, id). Drains stale /daemon
    control messages first, then waits for our client_connected event."""
    drain_daemon()
    s = connect_client()
    evs = wait_poll_messages(daemon_poll, 1)
    ev = evs[0]
    if ev.get("event") != "client_connected":
        raise AssertionError(f"expected client_connected, got {ev}")
    return s, ev["id"]


def close_client(s, cid):
    """Close a client and consume its client_disconnected event."""
    try:
        s.close()
    except OSError:
        pass
    wait_daemon_event("client_disconnected", cid)


# --- journal helpers (log assertions) ---------------------------------------

def journal_cursor():
    r = run(["journalctl", "--user", "-u", UNIT_NAME, "-n", "1", "--show-cursor",
             "-o", "cat"], check=False)
    for line in reversed(r.stdout.splitlines()):
        if line.startswith("-- cursor: "):
            return "--after-cursor=" + line[len("-- cursor: "):].strip()
    # no entries yet: fall back to "since now"
    return "--since=" + time.strftime("%Y-%m-%d %H:%M:%S")


def journal_lines(filters):
    r = run(["journalctl", "--user", "-u", UNIT_NAME, "-o", "cat", filters],
            check=False)
    return r.stdout


def wait_journal_contains(text, filters, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if text in journal_lines(filters):
            return True
        time.sleep(0.05)
    return False


# ---------------------------------------------------------------------------
# the tests
# ---------------------------------------------------------------------------

def test(name):
    def deco(fn):
        def wrapper():
            try:
                fn()
                RESULT["passed"] += 1
                print(f"  PASS  {name}")
            except Exception as e:  # noqa: BLE001 - test harness
                RESULT["failed"] += 1
                RESULT["failures"].append((name, e))
                print(f"  FAIL  {name}: {type(e).__name__}: {e}")
        return wrapper
    return deco


@test("service_ready: socket, D-Bus name, env caps, script loaded")
def t_service_ready():
    # the unit environment carries the small caps we rely on below
    show = run(["systemctl", "--user", "show", UNIT_NAME, "-p", "Environment"],
               check=False).stdout
    assert "KWIN_RX_BUFFER_CAP=65536" in show, "RX cap not in unit env"
    assert "KWIN_TX_BUFFER_CAP=65536" in show, "TX cap not in unit env"
    assert "KWIN_MAX_CLIENTS=4" in show, "max clients not in unit env"
    # the daemon must have loaded and run the (empty) script in KWin
    assert wait_journal_contains("KWin script loaded", CURSOR0), \
        "daemon did not log loadScript success"
    assert wait_journal_contains("KWin script running", CURSOR0), \
        "daemon did not log Script.run success"
    assert SOCKET_PATH.is_socket(), f"service.socket missing at {SOCKET_PATH}"
    assert BUS.name_has_owner(SERVICE_NAME), "daemon D-Bus name not owned"


@test("status: Status() on the derived object path")
def t_status_method():
    reply = str(status_obj().Status(dbus_interface=SERVICE_NAME, timeout=5))
    assert reply == f"service={SERVICE_NAME}", f"unexpected Status reply: {reply}"


@test("introspect: /daemon and /cli{id} expose the documented methods")
def t_introspect():
    xml = str(daemon_obj().Introspect(
        dbus_interface="org.freedesktop.DBus.Introspectable", timeout=5))
    assert 'name="poll"' in xml and 'name="log"' in xml
    assert SERVICE_NAME in xml
    s, cid = new_client()
    xml_cli = str(cli_obj(cid).Introspect(
        dbus_interface="org.freedesktop.DBus.Introspectable", timeout=5))
    assert 'name="poll"' in xml_cli and 'name="push"' in xml_cli
    assert CLIENT_IFACE in xml_cli
    close_client(s, cid)


@test("daemon poll: timeout 0 with nothing queued returns []")
def t_daemon_poll_empty():
    drain_daemon()
    assert str(daemon_poll(0)) == "[]"


@test("daemon poll: batch of 3 connects arrives as one FIFO batch")
def t_daemon_poll_batch_connects():
    drain_daemon()
    socks = [connect_client() for _ in range(3)]
    time.sleep(0.5)  # let the daemon accept and queue all three
    evs = parse_poll(daemon_poll(0))
    assert len(evs) == 3, f"a single poll should return all 3, got {evs}"
    assert [e["event"] for e in evs] == ["client_connected"] * 3
    ids = [e["id"] for e in evs]
    assert len(set(ids)) == 3, f"ids not unique: {ids}"
    for s in socks:
        s.close()
    time.sleep(0.5)  # and queue the three disconnects
    dis = parse_poll(daemon_poll(0))
    assert len(dis) == 3 and \
        [e["event"] for e in dis] == ["client_disconnected"] * 3, \
        f"disconnects not batched: {dis}"
    assert sorted(e["id"] for e in dis) == sorted(ids)


@test("daemon poll: blocking poll is answered by a connect")
def t_daemon_poll_blocks_until_connect():
    drain_daemon()
    result = {}

    def worker():
        conn = fresh_connection()
        obj = conn.get_object(SERVICE_NAME, "/daemon", introspect=False)
        result["raw"] = str(obj.poll(5000, dbus_interface=SERVICE_NAME,
                                     timeout=10))

    t = threading.Thread(target=worker)
    t.start()
    time.sleep(0.3)  # let the poll arm before the client connects
    s = connect_client()
    t.join(timeout=8)
    assert not t.is_alive(), "blocking poll did not return after connect"
    evs = parse_poll(result["raw"])
    assert len(evs) == 1 and evs[0]["event"] == "client_connected"
    close_client(s, evs[0]["id"])


@test("daemon poll: timeout returns [] after ~timeout, never blocks forever")
def t_daemon_poll_timeout_returns_empty():
    drain_daemon()
    t0 = time.monotonic()
    raw = str(daemon_poll(500))
    elapsed = time.monotonic() - t0
    assert raw == "[]", f"expected empty array, got {raw}"
    assert 0.25 <= elapsed <= 2.0, f"poll(500) took {elapsed:.3f}s"


@test("daemon poll: out-of-range timeout -> JSON-encoded error string")
def t_daemon_poll_out_of_range():
    expected = '"error: timeout out of range (0..25000)"'
    assert str(daemon_poll(-1)) == expected
    assert str(daemon_poll(25001)) == expected
    # the JSON-encoded string parses to the plain error message
    assert json.loads(str(daemon_poll(-1))) == "error: timeout out of range (0..25000)"


@test("daemon poll: second concurrent poll is rejected; pending one recovers")
def t_daemon_poll_concurrent_rejected():
    drain_daemon()
    result = {}

    def worker():
        conn = fresh_connection()
        obj = conn.get_object(SERVICE_NAME, "/daemon", introspect=False)
        result["raw"] = str(obj.poll(3000, dbus_interface=SERVICE_NAME,
                                     timeout=10))

    t = threading.Thread(target=worker)
    t.start()
    time.sleep(0.3)
    second = str(daemon_poll(0))
    assert second == '"error: a poll is already pending"', f"got {second}"
    t.join(timeout=10)
    assert not t.is_alive()
    assert parse_poll(result["raw"]) == []  # the armed poll timed out empty
    # the object is usable again afterwards
    assert str(daemon_poll(0)) == "[]"


@test("daemon poll: connect then disconnect arrive in FIFO order")
def t_daemon_connect_disconnect_fifo():
    drain_daemon()
    s = connect_client()
    time.sleep(0.3)  # let the daemon queue the connect event
    s.close()
    time.sleep(0.3)  # and the disconnect event
    evs = parse_poll(daemon_poll(0))
    assert len(evs) == 2, \
        f"expected [connected, disconnected] in one batch, got {evs}"
    assert evs[0]["event"] == "client_connected" and \
        evs[1]["event"] == "client_disconnected", f"bad FIFO order: {evs}"
    assert evs[0]["id"] == evs[1]["id"]


@test("cli poll: timeout 0 with no messages returns [] (and range check)")
def t_cli_poll_empty():
    s, cid = new_client()
    assert str(cli_poll(cid, 0)) == "[]"
    # the shared handler also rejects out-of-range timeouts on /cli{id}
    assert str(cli_poll(cid, -1)) == '"error: timeout out of range (0..25000)"'
    close_client(s, cid)


@test("cli poll: batch is FIFO and payloads are spliced verbatim")
def t_cli_poll_batch_fifo_verbatim():
    s, cid = new_client()
    payloads = ['{ "m" : 1 }', '{ "m" : 2 }', '{ "m" : 3 }']
    for p in payloads:
        send_frame(s, p.encode())
    raw, msgs = wait_cli_raw(cid, 3)
    # raw == '[' + payload1 + ',' + payload2 + ',' + payload3 + ']' — the
    # daemon splices the verbatim payload texts without re-serializing them
    assert raw == "[" + ",".join(payloads) + "]", f"not spliced verbatim: {raw}"
    assert [m["m"] for m in msgs] == [1, 2, 3]
    close_client(s, cid)


@test("cli poll: blocking poll is answered by an arriving frame")
def t_cli_poll_blocks_until_frame():
    s, cid = new_client()
    result = {}

    def worker():
        conn = fresh_connection()
        obj = conn.get_object(SERVICE_NAME, f"/cli{cid}", introspect=False)
        result["raw"] = str(obj.poll(5000, dbus_interface=CLIENT_IFACE,
                                     timeout=10))

    t = threading.Thread(target=worker)
    t.start()
    time.sleep(0.3)
    send_frame(s, b'{"blocked":true}')
    t.join(timeout=8)
    assert not t.is_alive(), "blocking poll did not return after frame"
    assert parse_poll(result["raw"]) == [{"blocked": True}]
    # the pending state was released: a fresh poll works again
    assert str(cli_poll(cid, 0)) == "[]"
    close_client(s, cid)


@test("cli poll: a frame split into three writes is still delivered")
def t_cli_poll_fragmented_send():
    s, cid = new_client()
    wire = frame(b'{"fragmented":true}')
    s.sendall(wire[:4])
    time.sleep(0.1)
    s.sendall(wire[4:10])
    time.sleep(0.1)
    s.sendall(wire[10:])
    _, msgs = wait_cli_raw(cid, 1)
    assert msgs == [{"fragmented": True}]
    close_client(s, cid)


@test("cli poll: multi-byte UTF-8 payload is delivered byte-exact")
def t_cli_poll_utf8():
    s, cid = new_client()
    payload = '{"msg":"café — naïve"}'
    send_frame(s, payload.encode())
    raw, msgs = wait_cli_raw(cid, 1)
    assert raw == "[" + payload + "]", f"UTF-8 not verbatim: {raw}"
    assert msgs == [{"msg": "café — naïve"}]
    close_client(s, cid)


@test("cli poll: the transport is opaque — a non-JSON payload is spliced raw")
def t_cli_poll_opaque_splice():
    s, cid = new_client()
    send_frame(s, b"garbage")
    deadline = time.monotonic() + 5
    raw = ""
    while time.monotonic() < deadline:
        raw = str(cli_poll(cid, 0))
        if raw != "[]":
            break
        time.sleep(0.01)
    # the daemon does not validate payloads: verbatim splice yields invalid
    # JSON here (by design — payloads are valid JSON per the framing contract)
    assert raw == "[garbage]", f"unexpected splice: {raw}"
    close_client(s, cid)


@test("cli poll: unknown object -> D-Bus error")
def t_cli_poll_unknown_object():
    try:
        cli_poll(99999, 0)
        raise AssertionError("expected a D-Bus error for an unknown object")
    except dbus.DBusException:
        pass


@test("cli poll: second concurrent poll is rejected")
def t_cli_poll_concurrent_rejected():
    s, cid = new_client()
    result = {}

    def worker():
        conn = fresh_connection()
        obj = conn.get_object(SERVICE_NAME, f"/cli{cid}", introspect=False)
        result["raw"] = str(obj.poll(3000, dbus_interface=CLIENT_IFACE,
                                     timeout=10))

    t = threading.Thread(target=worker)
    t.start()
    time.sleep(0.3)
    second = str(cli_poll(cid, 0))
    assert second == '"error: a poll is already pending"', f"got {second}"
    t.join(timeout=10)
    assert not t.is_alive()
    assert parse_poll(result["raw"]) == []
    close_client(s, cid)


@test("cli poll: a pending poll is dropped when the client disconnects")
def t_cli_pending_poll_cancelled_on_disconnect():
    s, cid = new_client()
    outcome = {}

    def worker():
        conn = fresh_connection()
        obj = conn.get_object(SERVICE_NAME, f"/cli{cid}", introspect=False)
        try:
            obj.poll(5000, dbus_interface=CLIENT_IFACE, timeout=2)
            outcome["exc"] = None
        except dbus.DBusException as e:
            outcome["exc"] = e

    t = threading.Thread(target=worker)
    t.start()
    time.sleep(0.3)
    s.close()  # disconnect while the poll is pending
    t.join(timeout=6)
    assert not t.is_alive()
    # PROTOCOL.md/DEVELOP.md: the pending poll is cancelled without a reply,
    # so the caller's D-Bus call times out
    assert outcome["exc"] is not None, "pending poll should have been dropped"
    # and the object is gone afterwards
    try:
        cli_poll(cid, 0)
        raise AssertionError("expected UnknownObject after disconnect")
    except dbus.DBusException:
        pass
    wait_daemon_event("client_disconnected", cid)


@test("push: success returns the empty string and the frame reaches the client")
def t_push_roundtrip():
    s, cid = new_client()
    assert str(cli_push(cid, "hello")) == ""
    assert recv_frame(s) == b"hello"
    close_client(s, cid)


@test("push: an empty message is framed as-is (no write-path validation)")
def t_push_empty_string():
    s, cid = new_client()
    assert str(cli_push(cid, "")) == ""
    s.settimeout(5)
    header = s.recv(4)
    assert header == b"\x00\x00\x00\x00", f"expected zero-length frame, got {header!r}"
    close_client(s, cid)


@test("push: over-1-MB message is ignored with a plain-text error")
def t_push_oversize_rejected():
    s, cid = new_client()
    big = "x" * (1_000_001)
    reply = str(cli_push(cid, big))
    assert reply == "message exceeds 1 MB", f"got {reply!r}"
    # nothing was queued: the client receives no frame
    s.settimeout(1.0)
    try:
        data = s.recv(4)
        assert data == b"", f"unexpected data after rejected push: {data!r}"
    except socket.timeout:
        pass
    close_client(s, cid)


@test("push: multiple pushes are delivered to the client in order")
def t_push_multiple_ordered():
    s, cid = new_client()
    for m in ("A", "B", "C"):
        assert str(cli_push(cid, m)) == ""
    got = [recv_frame(s), recv_frame(s), recv_frame(s)]
    assert got == [b"A", b"B", b"C"], f"got {got}"
    close_client(s, cid)


@test("push: UTF-8 payload reaches the client byte-exact")
def t_push_utf8():
    s, cid = new_client()
    assert str(cli_push(cid, "café")) == ""
    assert recv_frame(s) == "café".encode()
    close_client(s, cid)


@test("push: TX buffer full -> plain error; recovers once the client drains")
def t_push_tx_full_then_recovers():
    s, cid = new_client()
    reader = FrameReader(s)   # every byte read goes through this parser
    big = "x" * 8000
    results = []
    for _ in range(100):
        r = str(cli_push(cid, big))
        results.append(r)
        if r == "write buffer full":
            break
    assert "write buffer full" in results, \
        "push never hit the TX cap — KWIN_TX_BUFFER_CAP not in effect? " \
        f"first results: {results[:5]}"
    n_ok = results.index("write buffer full")
    assert all(r == "" for r in results[:n_ok]), f"unexpected early errors: {results[:n_ok]}"

    # once the client drains the socket, the daemon's TX ring drains too and
    # push() must succeed again (the ring reuses the freed space)
    ok = False
    for _ in range(100):
        reader.read_available()
        r = str(cli_push(cid, "y" * 100))
        if r == "":
            ok = True
            break
        assert r == "write buffer full", f"unexpected push error: {r!r}"
        time.sleep(0.05)
    assert ok, "push never recovered after draining the socket"

    # the client eventually receives the n_ok big frames + the small one,
    # in push order (big frames queued before the recovery push)
    frames = []
    while len(frames) < n_ok + 1:
        frames.append(reader.read_frame(timeout_s=3.0))
    assert len(frames) == n_ok + 1, f"got {len(frames)} frames, want {n_ok + 1}"
    assert all(len(f) == 8000 for f in frames[:n_ok]), \
        f"bad frame lengths (n_ok={n_ok}): {[len(f) for f in frames]}"
    assert frames[n_ok] == b"y" * 100, \
        f"last frame wrong (n_ok={n_ok}, total={len(frames)}): " \
        f"lens={[len(f) for f in frames]}, last={frames[n_ok][:20]!r}"
    close_client(s, cid)


@test("push: to a disconnected client -> object gone (D-Bus error)")
def t_push_to_disconnected_client():
    s, cid = new_client()
    s.close()
    wait_daemon_event("client_disconnected", cid)
    try:
        cli_push(cid, "x")
        raise AssertionError("expected a D-Bus error after disconnect")
    except dbus.DBusException:
        pass


@test("socket: oversized frame is discarded, connection stays up")
def t_oversized_frame_discarded():
    s, cid = new_client()
    cursor = journal_cursor()
    send_frame(s, b"x" * (1_000_001))   # 1 MB + 1 payload bytes
    send_frame(s, b'{"ok":1}')
    _, msgs = wait_cli_raw(cid, 1)
    assert msgs == [{"ok": 1}], "normal frame after oversized one was lost"
    assert wait_journal_contains("discarded oversized frame", cursor), \
        "daemon did not log the oversized-frame discard"
    close_client(s, cid)


@test("socket: a zero-length frame is discarded too (not a JSON value)")
def t_zero_length_frame_discarded():
    s, cid = new_client()
    cursor = journal_cursor()
    s.sendall(b"\x00\x00\x00\x00")       # length == 0 -> invalid payload
    send_frame(s, b'{"z":1}')
    _, msgs = wait_cli_raw(cid, 1)
    assert msgs == [{"z": 1}]
    assert wait_journal_contains("discarded oversized frame", cursor)
    close_client(s, cid)


@test("socket: two concurrent clients are fully isolated")
def t_multiple_clients_isolated():
    drain_daemon()
    sa, sb = connect_client(), connect_client()
    evs = wait_poll_messages(daemon_poll, 2)
    ida, idb = evs[0]["id"], evs[1]["id"]
    # a frame sent to A is only visible on /cli{ida}
    send_frame(sa, b'{"to":"A"}')
    _, msgs = wait_cli_raw(ida, 1)
    assert msgs == [{"to": "A"}]
    assert str(cli_poll(idb, 0)) == "[]"
    # push() routes only to the addressed client
    assert str(cli_push(ida, "for-A")) == ""
    assert str(cli_push(idb, "for-B")) == ""
    assert recv_frame(sa) == b"for-A"
    assert recv_frame(sb) == b"for-B"
    sa.close()
    sb.close()
    wait_daemon_events({("client_disconnected", ida), ("client_disconnected", idb)})


@test("backpressure: RX buffer full pauses reads; poll() resumes them")
def t_rx_backpressure_pause_resume():
    # With the 64 KB RX cap the daemon must pause reading after ~a few hundred
    # KB of un-polled frames; with the 16 MB default this never happens at this
    # size, so hitting EAGAIN proves the cap is in effect.
    class FrameSender:
        def __init__(self, sock):
            self.sock = sock
            self.sock.setblocking(False)
            self.buf = b""

        def push(self, payload):
            self.buf += frame(payload)

        def pump(self):
            while self.buf:
                try:
                    n = self.sock.send(self.buf)
                except BlockingIOError:
                    return False
                self.buf = self.buf[n:]
            return True

    s, cid = new_client()
    n_frames = 600
    sender = FrameSender(s)
    received = []
    blocked_hits = 0

    def drain_queued():
        received.extend(parse_poll(cli_poll(cid, 0)))

    for i in range(n_frames):
        payload = json.dumps({"i": i, "pad": "x" * 980}).encode()
        sender.push(payload)
        if not sender.pump():
            blocked_hits += 1
            # backpressure: drain the daemon's RX queue until it resumes reads
            while not sender.pump():
                drain_queued()
                time.sleep(0.002)

    deadline = time.monotonic() + 15
    while len(received) < n_frames and time.monotonic() < deadline:
        drain_queued()
        if len(received) < n_frames:
            time.sleep(0.01)

    assert len(received) == n_frames, f"got {len(received)} of {n_frames} frames"
    assert blocked_hits > 0, \
        "sender never blocked — RX backpressure did not engage " \
        "(KWIN_RX_BUFFER_CAP not in effect?)"
    assert [m["i"] for m in received] == list(range(n_frames)), \
        "frames arrived out of order"
    close_client(s, cid)


@test("clients: the connection limit rejects one more client")
def t_max_clients_rejected():
    drain_daemon()
    socks = [connect_client() for _ in range(MAX_CLIENTS)]
    evs = wait_poll_messages(daemon_poll, MAX_CLIENTS)
    assert all(e["event"] == "client_connected" for e in evs)
    ids = [e["id"] for e in evs]

    cursor = journal_cursor()
    extra = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    extra.settimeout(5)
    extra.connect(str(SOCKET_PATH))
    # the daemon accepts the connection and immediately closes it
    assert extra.recv(1) == b"", "rejected client should see EOF"
    extra.close()
    assert wait_journal_contains("client rejected", cursor), \
        "daemon did not log the rejected client"

    for s in socks:
        s.close()
    wait_daemon_events({("client_disconnected", cid) for cid in ids})


@test("log: info message appears in the daemon journal with [script] tag")
def t_log_info_visible():
    cursor = journal_cursor()
    marker = f"hello-log-test-{time.time_ns()}"
    daemon_log("info", marker)
    assert wait_journal_contains(f"[script] {marker}", cursor), \
        "info log line not in journal"


@test("log: an unknown level is logged as info, still tagged [script]")
def t_log_unknown_level_visible():
    cursor = journal_cursor()
    marker = f"bogus-level-marker-{time.time_ns()}"
    daemon_log("totally-bogus", marker)
    assert wait_journal_contains(f"[script] {marker}", cursor), \
        "unknown-level log line not in journal"


@test("log: wrong argument signature is rejected by D-Bus")
def t_log_bad_signature():
    bad_calls = [
        lambda: daemon_obj().log("one-arg", dbus_interface=SERVICE_NAME),
        lambda: daemon_obj().log("info", 123, dbus_interface=SERVICE_NAME),
    ]
    for call in bad_calls:
        try:
            call()
            raise AssertionError("expected a D-Bus error for a bad log() call")
        except (dbus.DBusException, TypeError):
            pass


# ---------------------------------------------------------------------------
# setup / teardown
# ---------------------------------------------------------------------------

def kwin_owned():
    r = run(["busctl", "--user", "--no-pager", "status", "org.kde.KWin"],
            check=False)
    return r.returncode == 0


def ensure_kwin():
    """The daemon loads the (empty) script into org.kde.KWin, so a real KWin
    must own the name on the session bus."""
    if not kwin_owned():
        die("org.kde.KWin is not owned on the session bus — this test needs "
            "a Plasma session with KWin running")
    print("[kwin] org.kde.KWin is owned on the session bus")


def render_unit(daemon_bin):
    template = (TESTDIR / "kwin-api-server-test.service.in").read_text()
    content = (template
               .replace("@DAEMON_BIN@", str(daemon_bin))
               .replace("@SCRIPT_PATH@", str(TESTDIR / "empty-kwinscript.js"))
               .replace("@UNLOAD_HELPER@", str(ROOT / "daemon" / "systemd"
                                               / "kwinscript-unload.sh"))
               .replace("@SERVICE_NAME@", SERVICE_NAME)
               .replace("@RX_CAP@", str(RX_CAP))
               .replace("@TX_CAP@", str(TX_CAP))
               .replace("@MAX_CLIENTS@", str(MAX_CLIENTS)))
    unit = TESTDIR / UNIT_NAME
    unit.write_text(content)
    return unit


def start_service():
    global CURSOR0
    systemctl("stop", UNIT_NAME, check=False)   # idempotent
    unit = render_unit(DAEMON_BIN)
    print(f"[unit] rendering {unit}")
    systemctl("link", str(unit))
    systemctl("daemon-reload")
    CURSOR0 = journal_cursor()                  # everything after this is new
    print(f"[unit] starting {UNIT_NAME}")
    systemctl("start", UNIT_NAME)

    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if SOCKET_PATH.is_socket() and BUS.name_has_owner(SERVICE_NAME):
            time.sleep(0.3)
            if service_active():
                return
        if not service_active():
            time.sleep(0.05)
        time.sleep(0.1)
    dump_journal_and_status()
    die(f"{UNIT_NAME} did not become ready")


def service_active():
    r = systemctl("is-active", UNIT_NAME, check=False)
    return r.stdout.strip() == "active"


def dump_journal_and_status():
    print("--- systemctl status ---", file=sys.stderr)
    print(systemctl("status", UNIT_NAME, check=False).stdout, file=sys.stderr)
    print("--- journal (last 80 lines) ---", file=sys.stderr)
    print(run(["journalctl", "--user", "-u", UNIT_NAME, "-n", "80", "-o", "cat"],
              check=False).stdout, file=sys.stderr)


def stop_service():
    print("[unit] stopping the service")
    systemctl("stop", UNIT_NAME, check=False)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and service_active():
        time.sleep(0.1)
    print(f"[unit] stopped ({'active' if service_active() else 'inactive'})")
    if not SOCKET_PATH.exists():
        print("[unit] service.socket removed (RuntimeDirectory cleaned up)")
    if wait_journal_contains("KWin script unloaded", CURSOR0, timeout_s=5):
        print("[unit] daemon unloaded the KWin script on shutdown")


def print_removal_hint():
    print()
    print("Service stopped.")
    print("Note: the unit file created by `systemctl --user link` is still "
          "present:")
    print(f"    {LINKED_UNIT}")
    print("To remove it completely later, run manually:")
    print(f"    rm {LINKED_UNIT}")
    print("    systemctl --user daemon-reload")


def main():
    parser = argparse.ArgumentParser(
        description="End-to-end test of kwin-api-daemon under the real "
                    "systemd user manager (mock service + empty script).")
    parser.add_argument("--keep-running", action="store_true",
                        help="leave the service running on exit (debugging)")
    parser.add_argument("--only", metavar="SUBSTR", default=None,
                        help="run only tests whose internal name contains SUBSTR")
    args = parser.parse_args()

    global DAEMON_BIN
    DAEMON_BIN = Path(os.environ.get("KAS_DAEMON_BIN")) if os.environ.get(
        "KAS_DAEMON_BIN") else find_binary("kwin-api-daemon")
    if DAEMON_BIN is None or not DAEMON_BIN.is_file():
        die("kwin-api-daemon not found (run 'xmake' first, or set KAS_DAEMON_BIN)")
    help_out = run([DAEMON_BIN, "--help"], check=False).stdout
    if "KWIN_RX_BUFFER_CAP" not in help_out or "KWIN_TX_BUFFER_CAP" not in help_out:
        die(f"{DAEMON_BIN} does not support KWIN_RX_BUFFER_CAP/KWIN_TX_BUFFER_CAP "
            "(rebuild with 'xmake')")
    print(f"[daemon] using {DAEMON_BIN}")

    print("=" * 72)
    print(f"test/daemon: kwin-api-daemon end-to-end "
          f"(systemd user unit {UNIT_NAME})")
    print("=" * 72)

    try:
        ensure_kwin()
        start_service()
    except Exception as e:  # noqa: BLE001
        die(f"startup failed: {e}")

    # run the tests in definition order
    for _, fn in sorted(((k, v) for k, v in globals().items()
                         if k.startswith("t_") and (not args.only or args.only in k)),
                        key=lambda kv: kv[0]):
        fn()

    print("=" * 72)
    passed, failed = RESULT["passed"], RESULT["failed"]
    print(f"PASSED: {passed}   FAILED: {failed}")
    if RESULT["failures"]:
        print("Failures:")
        for name, exc in RESULT["failures"]:
            print(f"  - {name}: {exc}")
    print("=" * 72)

    if not args.keep_running:
        stop_service()
        print_removal_hint()
    else:
        print(f"[keep-running] {UNIT_NAME} left running; stop it with:")
        print(f"    systemctl --user stop {UNIT_NAME}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        print("interrupted — stopping the service")
        try:
            stop_service()
        except Exception:  # noqa: BLE001
            pass
        print_removal_hint()
        sys.exit(130)
