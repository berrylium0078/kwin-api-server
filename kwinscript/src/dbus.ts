// SPDX-License-Identifier: MIT
//
// D-Bus helpers for the KWin script: Promise wrappers around KWin's
// callback-based callDBus(), plus typed wrappers for the daemon's D-Bus
// interface (log / poll / push), per doc/PROTOCOL.md §3.
//
// The daemon's service name is *not* hardcoded: the daemon rewrites the
// @DAEMON_DBUS_SERVICE@ placeholder to its runtime name (KWIN_API_SERVICE_NAME)
// while staging the script (see daemon/src/file_util.cpp), so this bundle
// works under any service name.

// The daemon serves its own interface under the same dotted name as its
// service name (see daemon/src/dbus_service.cpp); /daemon uses it, while the
// per-client /cli{id} objects use the fixed literal "org.example.KwinApiClient"
// (PROTOCOL.md §4).
const DAEMON_DBUS_SERVICE = "@DAEMON_DBUS_SERVICE@";
const DAEMON_DBUS_INTERFACE = DAEMON_DBUS_SERVICE;
const DAEMON_DBUS_PATH = "/daemon";
const DAEMON_CLIENT_INTERFACE = "org.example.KwinApiClient";

// Upper bound for a log() round trip; the call is rejected after this.
const DAEMON_DBUS_TIMEOUT_MS = 5000;
// poll() blocks up to 25000 ms (PROTOCOL.md §3.2); the wrapper timer must
// exceed that with enough margin for D-Bus transport + event-loop scheduling
// latency (this environment has measured multi-second delivery delays), so a
// legitimate 25 s poll answer is never mistaken for a dead daemon.
const POLL_TIMEOUT_MS = 20000;
const POLL_CALL_TIMEOUT_MS = POLL_TIMEOUT_MS + 5000; // 5s
// How long to wait before retrying a failed D-Bus call (e.g. the daemon was
// restarted and the object is temporarily gone).
const DBUS_RETRY_DELAY_MS = 500;

export const DAEMON_DBUS_SERVICE_NAME = DAEMON_DBUS_SERVICE;
export const DAEMON_DBUS_PATH_NAME = DAEMON_DBUS_PATH;

// KWin's global callDBus() is asynchronous but callback-based: the reply (if
// any) is passed to the optional trailing callback. Wrap it in a Promise and
// arm a single-shot QTimer that rejects the promise when the reply does not
// arrive in time. The timer is stopped in both outcomes.
export function callDBusAsync(
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

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => {
        const timer = new QTimer();
        timer.singleShot = true;
        timer.interval = ms;
        timer.timeout.connect(() => {
            timer.stop();
            resolve();
        });
        timer.start();
    });
}

/** Send a log line to the daemon's log(level, msg). Never throws. */
export async function sendLog(level: string, message: string): Promise<void> {
    try {
        await callDBusAsync(
            DAEMON_DBUS_SERVICE,
            DAEMON_DBUS_PATH,
            DAEMON_DBUS_INTERFACE,
            "log",
            DAEMON_DBUS_TIMEOUT_MS,
            level,
            message,
        );
    } catch (error) {
        print(`[kwinscript] cannot send log to daemon (${DAEMON_DBUS_SERVICE}): ${error}`);
    }
}

/**
 * poll() on /daemon: returns the daemon -> script control messages (client
 * connect / disconnect notifications) as parsed JSON (an array of payloads,
 * or a string error message — PROTOCOL.md §3.2).
 */
export async function pollDaemon(): Promise<unknown> {
    const raw = await callDBusAsync(
        DAEMON_DBUS_SERVICE,
        DAEMON_DBUS_PATH,
        DAEMON_DBUS_INTERFACE,
        "poll",
        POLL_CALL_TIMEOUT_MS,
        POLL_TIMEOUT_MS,
    );
    return JSON.parse(raw as string);
}

/** poll() on /cli{id}: the client's inbound messages (see pollDaemon). */
export async function pollClient(clientId: number): Promise<unknown> {
    const raw = await callDBusAsync(
        DAEMON_DBUS_SERVICE,
        `/cli${clientId}`,
        DAEMON_CLIENT_INTERFACE,
        "poll",
        POLL_CALL_TIMEOUT_MS,
        POLL_TIMEOUT_MS,
    );
    return JSON.parse(raw as string);
}

/**
 * push() on /cli{id}: queue one message (a JSON-RPC message, already
 * serialized) for the client's socket. Returns true on success. Errors
 * (message too large / write buffer full / client gone) are logged and
 * reported to the daemon but never thrown.
 */
export async function pushToClient(clientId: number, payload: string): Promise<boolean> {
    try {
        const reply = await callDBusAsync(
            DAEMON_DBUS_SERVICE,
            `/cli${clientId}`,
            DAEMON_CLIENT_INTERFACE,
            "push",
            DAEMON_DBUS_TIMEOUT_MS,
            payload,
        );
        if (reply === "") {
            return true;
        }
        sendLog("warn", `push to client ${clientId} rejected by daemon: ${reply}`);
        return false;
    } catch (error) {
        // The client may have disconnected (object gone).
        print(`[kwinscript] push to client ${clientId} failed: ${error}`);
        return false;
    }
}

/** Wait a bit and retry-able D-Bus wrapper: used by the daemon control loop. */
export const DBUS_RETRY_DELAY = DBUS_RETRY_DELAY_MS;
export { sleep };
