// SPDX-License-Identifier: MIT
//
// Sample KWin script bundled by esbuild into a single file
// (kwinscript/dist/kwinscript.js). kwin-api-daemon loads it through
// org.kde.kwin.Scripting.loadScript and runs it via /Scripting/Script<id>.
//
// Everything here runs inside KWin's QJSEngine — no Node.js, no browser.
// The ambient types come from the kwin-ts package (see tsconfig.json).

const SCRIPT_ID = "kwin-api-server-demo";

// --- daemon D-Bus contract --------------------------------------------------
//
// The daemon (kwin-api-daemon) serves a `log` method on the object at
// "/daemon" of its own session-bus service. The service name is *not*
// hardcoded here: the daemon rewrites the @DAEMON_DBUS_SERVICE@ placeholder
// to its runtime name (KWIN_API_SERVICE_NAME) while staging the script
// (see daemon/src/file_util.cpp), so this bundle works under any service name.
const DAEMON_DBUS_SERVICE = "@DAEMON_DBUS_SERVICE@";
// The daemon serves its own interface under the same dotted name as its
// service name (see daemon/src/dbus_service.cpp), so it derives from the
// same placeholder.
const DAEMON_DBUS_INTERFACE = DAEMON_DBUS_SERVICE;
const DAEMON_DBUS_PATH = "/daemon";
const DAEMON_DBUS_LOG_METHOD = "log";
// Upper bound for a log() round trip; the call is rejected after this.
const DAEMON_DBUS_TIMEOUT_MS = 5000;

// --- D-Bus helpers ----------------------------------------------------------
//
// KWin's global callDBus() is asynchronous but callback-based: the reply (if
// any) is passed to the optional trailing callback. Wrap it in a Promise and
// arm a single-shot QTimer that rejects the promise when the reply does not
// arrive in time. The timer is stopped in both outcomes.

function callDBusAsync(
    service: string,
    path: string,
    interfaceName: string,
    method: string,
    timeoutMs: number,
    ...args: unknown[]
): Promise<unknown> {
    return new Promise((resolve, reject) => {
        const timer = new QTimer();
        timer.singleShot = true;
        timer.interval = timeoutMs;
        timer.timeout.connect(() => {
            timer.stop();
            reject(new Error(
                `D-Bus call timed out after ${timeoutMs} ms: ` +
                `${service} ${path} ${interfaceName}.${method}`,
            ));
        });
        timer.start();
        callDBus(service, path, interfaceName, method, ...args, (reply: unknown) => {
            timer.stop();
            resolve(reply);
        });
    });
}

// Send a log line to the daemon's log(level, msg). Failures (daemon not
// running, timeout, placeholder not rewritten, ...) are reported to KWin's
// debug output but never thrown.
async function sendLog(level: string, message: string): Promise<void> {
    try {
        await callDBusAsync(
            DAEMON_DBUS_SERVICE,
            DAEMON_DBUS_PATH,
            DAEMON_DBUS_INTERFACE,
            DAEMON_DBUS_LOG_METHOD,
            DAEMON_DBUS_TIMEOUT_MS,
            level,
            message,
        );
    } catch (error) {
        print(`[${SCRIPT_ID}] cannot send log to daemon (${DAEMON_DBUS_SERVICE}): ${error}`);
    }
}

// --- demo -------------------------------------------------------------------

// A shortcut to prove the script is alive. Press Meta+Ctrl+Shift+K and watch
// KWin's debug output / journal:
//   journalctl --user -u kwin_wayland --since today
registerShortcut(`${SCRIPT_ID}:toggle`, "KWin API Server demo", "Meta+Ctrl+Shift+K", () => {
    const windows = workspace.windowList();
    print(`[${SCRIPT_ID}] toggle: ${windows.length} window(s) tracked`);
    workspace.slotToggleMaximize();
    sendLog("info", `shortcut toggled (${windows.length} window(s) tracked)`);
});

sendLog("info", "kwinscript loaded");
print(`[${SCRIPT_ID}] loaded by kwin-api-server`);
