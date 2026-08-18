#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# run.py — (re)build/install (optional), start and follow kwin-api-server logs.
#
#   ./run.py            stop the service -> start it -> follow its logs
#   ./run.py --build    additionally: xmake build -> xmake install -o stage
#                       (staged install) -> systemctl link + daemon-reload so
#                       systemctl picks up the freshly built .service under
#                       stage/
#
# Note: this project builds with xmake (there is no CMakeLists.txt in the
# repo), so "build + staged install" here maps to `xmake` and
# `xmake install -o stage` (see the README "Build" / "Install" sections),
# not to cmake.
#
# On exit (Ctrl+C / SIGTERM, turned into KeyboardInterrupt by the signal
# handlers) the service is stopped and the user is reminded to manually remove
# the unit file created by `systemctl link`:
# ~/.config/systemd/user/kwin-api-server.service.

import argparse
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
UNIT_NAME = "kwin-api-server.service"
# Where `xmake install -o stage` puts the generated unit (see xmake.lua on_install).
STAGED_UNIT = ROOT / "stage" / "lib" / "systemd" / "user" / UNIT_NAME
# The link `systemctl --user link` creates in the user config directory.
LINKED_UNIT = Path.home() / ".config" / "systemd" / "user" / UNIT_NAME


def run(cmd, *, check=True, cwd=None):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=check, cwd=cwd)


def systemctl(*args, check=True):
    return run(["systemctl", "--user", *args], check=check)


def stop_service():
    # Stopping a unit that is not running (or not loaded yet) is fine.
    systemctl("stop", UNIT_NAME, check=False)


def build_and_install():
    run(["xmake", "f", "-m", "release"], cwd=ROOT)
    run(["xmake"], cwd=ROOT)
    run(["xmake", "install", "-o", "stage"], cwd=ROOT)


def link_unit():
    if not STAGED_UNIT.is_file():
        print(f"error: staged unit not found: {STAGED_UNIT}", file=sys.stderr)
        print("       run with --build first, or check whether the install step "
              "succeeded", file=sys.stderr)
        sys.exit(1)
    # `systemctl link` is idempotent for the same unit file (re-linking succeeds
    # and leaves the existing symlink untouched), so there is no need to remove
    # an old link first. If the unit was previously linked to a different path,
    # systemctl reports a clear error; remove ~/.config/systemd/user/... and
    # re-run.
    systemctl("link", str(STAGED_UNIT))
    # link must come before daemon-reload for systemctl to see the unit.
    systemctl("daemon-reload")


def start_service():
    systemctl("start", UNIT_NAME)


def follow_logs():
    run(["journalctl", "--user", "-f", "-u", "kwin-api-server"])


def print_removal_hint():
    print()
    print("Service stopped.")
    print("Note: the unit file created by `systemctl link` is still present:")
    print(f"    {LINKED_UNIT}")
    print("To remove it completely later, run manually:")
    print(f"    rm {LINKED_UNIT}")


def _raise_interrupt(_signum, _frame):
    # Turn SIGINT/SIGTERM into KeyboardInterrupt so the finally block below
    # always performs the cleanup.
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(
        description="(Re)build/install, start and follow the kwin-api-server "
                    "service.")
    parser.add_argument("--build", action="store_true",
                        help="build with xmake, staged-install to ./stage via "
                             "`xmake install -o stage`, then link + "
                             "daemon-reload so systemctl picks up the new unit")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, _raise_interrupt)
    signal.signal(signal.SIGTERM, _raise_interrupt)

    try:
        print("==> stopping the currently running service")
        stop_service()

        if args.build:
            print("==> building and staged-installing (xmake install -o stage)")
            build_and_install()
            print("==> linking the generated unit and reloading systemd")
            link_unit()

        print(f"==> starting {UNIT_NAME}")
        start_service()

        print("==> following logs (Ctrl+C to stop)")
        follow_logs()
    finally:
        # Stop the service and print the hint on every exit path: normal,
        # Ctrl+C, or an error.
        print("==> stopping the service")
        stop_service()
        print_removal_hint()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        # Cleanup already ran in main()'s finally block.
        print()
        sys.exit(130)
    except subprocess.CalledProcessError as e:
        print(f"error: command failed: {' '.join(e.cmd)} (exit {e.returncode})",
              file=sys.stderr)
        sys.exit(1)
