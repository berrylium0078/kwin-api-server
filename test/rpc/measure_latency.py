#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# test/rpc/measure_latency.py — measure the end-to-end latency of the window
# claiming protocol (doc/RPC.md §2): from the client's `token.request` to the
# `token.validated` notification, in a real systemd + KWin + PyQt6 session.
#
# Reuses the harness of test_rpc.py (RpcClient / WindowManager / service
# lifecycle) so the measurement runs against the exact same stack.
#
# Reported metrics (per iteration, wall-clock on the client):
#   * roundtrip   token.request -> response   (socket + daemon + script
#                 dispatch + push; no window involved)
#   * validate    response -> token.validated with the window ALREADY mapped
#                 (the pure protocol: title-change propagation into KWin,
#                 caption match, validation, notification push)
#   * total       token.request -> token.validated (pre-mapped window)
#   * fresh       total when the window is CREATED with the token title
#                 (includes KWin's window-mapping time; real-world usage)
#
# Usage:
#   python3 test/rpc/measure_latency.py [--iterations N] [--fresh M] [--keep-running]

import argparse
import os
import sys
import threading
import time
from pathlib import Path
from statistics import mean, median

from PyQt6.QtCore import QTimer
from PyQt6.QtWidgets import QApplication

ROOT = Path(__file__).resolve().parents[2]
TESTDIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTDIR))

import test_rpc as tr  # noqa: E402  (RpcClient, WindowManager, service helpers)

NEUTRAL_TITLE = "kwin-api-latency-probe-window"


def fmt(v):
    """Format one metric column (seconds -> milliseconds)."""
    return f"{v * 1000:8.1f}"


def report(name, values):
    if not values:
        return
    values = sorted(values)
    n = len(values)
    p50 = median(values)
    p95 = values[min(n - 1, int(n * 0.95))]
    print(f"  {name:24s} n={n:3d}  "
          f"min={fmt(values[0])} ms  p50={fmt(p50)}  mean={fmt(mean(values))}  "
          f"p95={fmt(p95)}  max={fmt(values[-1])}", flush=True)


def run_measurements(wm, iters, fresh, done):
    """Worker thread: performs the measured claims and fills `results`."""
    results = {"roundtrip": [], "validate": [], "total": [], "fresh": []}
    try:
        # --- warmup: a real window, mapped, claimed once ---------------------
        handle = wm.create(NEUTRAL_TITLE)
        c = tr.RpcClient()
        try:
            token = c.request_token(60000)
            wm.rename(handle, token)
            c.wait_token_validated(token)
        finally:
            c.close()
        print(f"[measure] warmup done — window mapped, protocol warm", flush=True)

        # --- pre-mapped window: pure protocol latency ------------------------
        for i in range(iters):
            c = tr.RpcClient()
            try:
                t0 = time.monotonic()
                result = c.request_ok("token.request", {"timeout": 60000})
                t1 = time.monotonic()
                token = result["token"]
                wm.rename(handle, token)
                c.wait_token_validated(token)
                t3 = time.monotonic()
                results["roundtrip"].append(t1 - t0)
                results["validate"].append(t3 - t1)
                results["total"].append(t3 - t0)
                print(f"[measure] pre-mapped #{i}: roundtrip={fmt(t1 - t0)} ms  "
                      f"validate={fmt(t3 - t1)}  total={fmt(t3 - t0)}", flush=True)
            finally:
                c.close()
            time.sleep(0.5)  # let the script process the disconnect

        # --- fresh window: includes KWin mapping time -------------------------
        for i in range(fresh):
            c = tr.RpcClient()
            try:
                t0 = time.monotonic()
                result = c.request_ok("token.request", {"timeout": 60000})
                t1 = time.monotonic()
                token = result["token"]
                h = wm.create(token)
                c.wait_token_validated(token)
                t3 = time.monotonic()
                results["fresh"].append(t3 - t0)
                print(f"[measure] fresh #{i}: total={fmt(t3 - t0)} ms "
                      f"(request rt={fmt(t1 - t0)} ms + rest={fmt(t3 - t1)})",
                      flush=True)
                wm.close(h)
            finally:
                c.close()
            time.sleep(0.5)
    finally:
        done["results"] = results
        done["event"].set()


def main():
    parser = argparse.ArgumentParser(
        description="Measure the window-claim protocol latency (request -> "
                    "token.validated).")
    parser.add_argument("--iterations", type=int, default=10,
                        help="pre-mapped window iterations (default 10)")
    parser.add_argument("--fresh", type=int, default=5,
                        help="fresh-window iterations (default 5)")
    parser.add_argument("--keep-running", action="store_true",
                        help="leave the service running on exit")
    args = parser.parse_args()

    if not tr.kwin_owned():
        tr.die("org.kde.KWin is not owned on the session bus — this test "
               "needs a Plasma session with KWin running")
    print("[kwin] org.kde.KWin is owned on the session bus")

    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    wm = tr.WindowManager()

    try:
        tr.start_service(False)
    except Exception as e:  # noqa: BLE001
        tr.die(f"startup failed: {e}")

    done = {"event": threading.Event(), "results": None}
    worker = threading.Thread(target=run_measurements,
                              args=(wm, args.iterations, args.fresh, done),
                              daemon=True)
    worker.start()

    def tick():
        wm.pump()
        if done["event"].is_set():
            wm.pump()
            app.quit()
    timer = QTimer()
    timer.timeout.connect(tick)
    timer.start(20)
    app.exec()
    worker.join(timeout=30)

    results = done["results"] or {}
    print("=" * 72)
    print("Window-claim latency (client wall-clock)")
    print("=" * 72)
    report("token.request roundtrip", results.get("roundtrip", []))
    report("response -> validated (mapped)", results.get("validate", []))
    report("total request -> validated", results.get("total", []))
    report("fresh window (incl. mapping)", results.get("fresh", []))
    print("=" * 72)

    if not args.keep_running:
        tr.stop_service()
    else:
        print(f"[keep-running] {tr.UNIT_NAME} left running; stop it with:")
        print(f"    systemctl --user stop {tr.UNIT_NAME}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted")
        try:
            tr.stop_service()
        except Exception:  # noqa: BLE001
            pass
        sys.exit(130)
