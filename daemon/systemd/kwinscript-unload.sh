#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Unload the KWin script after the daemon has exited. This is the safety net
# referenced by the unit's ExecStopPost=: the daemon itself already unloads on
# graceful shutdown, this covers SIGKILL / crashes. Never fails the unit when
# KWin is already gone or the session bus is not available.
#
# Inherits KWIN_PLUGIN_NAME from the service environment (see the unit file).

set -u

if [ -z "${KWIN_PLUGIN_NAME:-}" ]; then
    echo "Plugin name not configured, nothing to do"
    exit 0
fi
if !(command -v dbus-send >/dev/null 2>&1); then
    echo "dbus-send not found, skipping"
    exit 1
fi

echo "SESSION_BUS: ${DBUS_SESSION_BUS_ADDRESS:-}"

# The daemon (sd_bus_open_user) falls back to $XDG_RUNTIME_DIR/bus; do the
# same here so ExecStopPost works even without DBUS_SESSION_BUS_ADDRESS.
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
        export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
    else
        export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"
    fi
fi

dbus-send --session --print-reply \
    --dest=org.kde.KWin /Scripting \
    org.kde.kwin.Scripting.unloadScript "string:${KWIN_PLUGIN_NAME}" || true