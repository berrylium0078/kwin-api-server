#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""kwin-api-host.py — native messaging host for Firefox (kwin-api-server bridge).

Firefox's native messaging protocol (https://mzl.la/3nP1tG9) and
kwin-api-server's unix socket protocol (doc/PROTOCOL.md §2.2) use the *same*
framing on Linux:

    4-byte length (uint32, native byte order) + UTF-8 JSON payload

so this host is a pure byte-level bridge: every native message Firefox writes
to our stdin is forwarded verbatim (reframed) to the kwin-api-server unix
socket, and every frame the daemon pushes back is forwarded verbatim to
stdout. No JSON parsing, no envelope — the extension talks JSON-RPC 2.0 to the
KWin script directly (doc/RPC.md).

Socket path (in priority order):
    $KWIN_API_SOCKET                       (override, handy for tests)
    $XDG_RUNTIME_DIR/kwin-api-server/service.socket

Exit codes: 0 on clean shutdown (peer EOF), 1 on connection/setup errors.

Run with --check to verify the socket path and connectivity without a browser.
"""

import json
import os
import selectors
import socket
import struct
import sys

# kwin-api-server frame limit (PROTOCOL.md §2.2, 1 MB = 1,000,000 payload
# bytes). Firefox's native messaging limit is the same order of magnitude, so
# this guard protects both directions.
MAX_MESSAGE = 1_000_000

DEFAULT_SOCKET_REL = os.path.join("kwin-api-server", "service.socket")


def default_socket_path() -> str:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        runtime = f"/run/user/{os.getuid()}"
    return os.path.join(runtime, DEFAULT_SOCKET_REL)


def pack_frame(payload: bytes) -> bytes:
    """Prepend the 4-byte native-endian length header (both protocols)."""
    return struct.pack("=I", len(payload)) + payload


def extract_frame(buf: bytes):
    """If `buf` holds at least one complete frame, return (payload, rest);
    otherwise (None, buf). Raises ValueError on an oversized frame."""
    if len(buf) < 4:
        return None, buf
    (n,) = struct.unpack("=I", buf[:4])
    if n > MAX_MESSAGE:
        raise ValueError(f"frame too large: {n} bytes (limit {MAX_MESSAGE})")
    if len(buf) < 4 + n:
        return None, buf
    return buf[4:4 + n], buf[4 + n:]


def log(msg: str) -> None:
    # Firefox only reads stdout; stderr goes to the terminal / journal.
    print(f"kwin-api-host: {msg}", file=sys.stderr, flush=True)


def check(sock_path: str) -> int:
    """--check: report the socket path and whether a connection succeeds."""
    log(f"check: socket path = {sock_path}")
    if not os.path.exists(sock_path):
        log(f"check: FAIL — {sock_path} does not exist (is kwin-api-server running?)")
        return 1
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(sock_path)
        s.close()
        log(f"check: OK — connected to {sock_path}")
        return 0
    except OSError as exc:
        log(f"check: FAIL — cannot connect to {sock_path}: {exc}")
        return 1


def bridge(sock_path: str) -> int:
    """The main loop: forward stdin <-> socket. Returns the exit code."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(sock_path)
    except OSError as exc:
        log(f"cannot connect to {sock_path}: {exc}")
        log("is kwin-api-server running? (systemctl --user status kwin-api-server.service)")
        return 1
    log(f"connected to {sock_path}")

    stdin_fd = sys.stdin.fileno()
    stdout = sys.stdout.buffer  # binary pipe to Firefox
    sel = selectors.DefaultSelector()
    sel.register(stdin_fd, selectors.EVENT_READ)
    sel.register(sock, selectors.EVENT_READ)

    from_firefox = b""  # native-messaging bytes from Firefox, awaiting framing
    from_daemon = b""   # socket bytes from the daemon, awaiting framing

    try:
        while True:
            for key, _ in sel.select():
                if key.fileobj is stdin_fd:
                    data = os.read(stdin_fd, 65536)
                    if not data:
                        log("stdin EOF — Firefox closed the port; exiting")
                        return 0
                    from_firefox += data
                    while True:
                        try:
                            payload, from_firefox = extract_frame(from_firefox)
                        except ValueError as exc:
                            log(f"bad frame from Firefox: {exc}; exiting")
                            return 1
                        if payload is None:
                            break
                        sock.sendall(pack_frame(payload))
                else:
                    data = sock.recv(65536)
                    if not data:
                        log("socket EOF — kwin-api-server closed; exiting")
                        return 0
                    from_daemon += data
                    while True:
                        try:
                            payload, from_daemon = extract_frame(from_daemon)
                        except ValueError as exc:
                            log(f"bad frame from daemon: {exc}; exiting")
                            return 1
                        if payload is None:
                            break
                        stdout.write(pack_frame(payload))
                        stdout.flush()
    except (BrokenPipeError, ConnectionResetError, OSError):
        # Firefox died or the daemon went away mid-write; nothing to do.
        log("connection lost; exiting")
        return 0
    finally:
        sel.close()
        sock.close()


def main(argv: list[str]) -> int:
    sock_path = os.environ.get("KWIN_API_SOCKET") or default_socket_path()
    if argv and argv[0] == "--check":
        return check(sock_path)
    return bridge(sock_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
