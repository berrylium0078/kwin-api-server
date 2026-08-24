// SPDX-License-Identifier: MIT
//
// Window property methods — windows.update / windows.query / window.watch
// (doc/RPC.md §"Window properties").
//
// All three methods take their params **by position** (JSON-RPC by-position:
// `params` is an array, `[token, ...]`) and use a claimed token from the
// window-claim protocol (src/tokens.ts) as the window reference. Params are
// zod-validated like every other method; unsupported property names are
// rejected with -32602 before anything is applied. Requests whose `id` is
// `null` are treated as notifications: the method runs, but no response is
// sent (see src/jsonrpc.ts dispatchMessage).
//
// Supported properties: the writable KWin.Window properties of JS primitive
// type (boolean / number), plus `desktops` and `activities` which are passed
// as **ID sets** (`string[]` of KDE UUIDs — see §3.5 in doc/RPC.md). The
// catalog is three immutable Sets below. To support more properties, extend
// the Sets (and add the property to the declarations in kwin-ts when
// missing). window.watch requires a property that has a `<prop>Changed`
// signal on KWin.Window — that signal is what the listener connects to.

import { z } from "zod";
import { JSONRPCErrorCode, JSONRPCErrorException } from "json-rpc-2.0";
import { registerMethod, notifyClient, type RpcConnection } from "./jsonrpc";
import { resolveTokenWindow } from "./tokens";
import { sendLog } from "./dbus";

// ---------------------------------------------------------------------------
// Workspace catalogs: desktop IDs and activity IDs
// ---------------------------------------------------------------------------
//
// windows.update validates the `desktops` / `activities` ID values against
// these catalogs before touching the window. Both are maintained from the
// workspace events:
//
//   * `workspace.desktopsChanged`  -> full rescan of workspace.desktops
//     (desktop layout changes are rare in normal use, so rescanning is cheap
//     and immune to signal-payload drift);
//   * `workspace.activityAdded` / `workspace.activityRemoved` -> single
//     insert / delete of the id carried by the signal (plus an initial scan
//     of workspace.activities at load).
//
// Desktop and activity IDs are KDE UUIDs and stay stable across sessions
// (desktops: persisted in kwinrc; activities: managed by kactivitymanagerd),
// so a client may store them (doc/RPC.md §3.5).

/** Desktop ID -> VirtualDesktop, kept in sync with workspace.desktops. */
const desktopsById = new Map<string, KWin.VirtualDesktop>();
/** Activity IDs present on the workspace. */
const activitiesById = new Set<string>();

function rescanDesktops(): void {
    desktopsById.clear();
    for (const desktop of workspace.desktops) {
        desktopsById.set(String(desktop.id), desktop);
    }
}

function rescanActivities(): void {
    activitiesById.clear();
    for (const id of workspace.activities) {
        activitiesById.add(id);
    }
}

function wireWorkspaceEvents(): void {
    rescanDesktops();
    rescanActivities();
    workspace.desktopsChanged.connect(rescanDesktops);
    workspace.activityAdded.connect((id: string) => activitiesById.add(id));
    workspace.activityRemoved.connect((id: string) => activitiesById.delete(id));
}

try {
    wireWorkspaceEvents();
    sendLog("info", `windows: workspace catalogs: ${desktopsById.size} desktop(s), ${activitiesById.size} activity(ies)`);
} catch (error) {
    sendLog("error", `windows: cannot wire workspace events: ${error}`);
}

/**
 * Look up a desktop by its ID in the workspace catalog. Shared with the
 * workspace-property methods (src/workspace.ts) so that both validate IDs
 * against the same maintained set.
 */
export function lookupDesktop(id: string): KWin.VirtualDesktop | undefined {
    return desktopsById.get(id);
}

/** Whether an activity ID is present in the workspace catalog (shared, see
 * lookupDesktop). */
export function hasActivity(id: string): boolean {
    return activitiesById.has(id);
}

// ---------------------------------------------------------------------------
// Supported-property catalog (immutable by convention — never mutate these
// Sets; extend them as more window properties become supported)
// ---------------------------------------------------------------------------

/**
 * Properties `windows.update` accepts: writable KWin.Window properties of JS
 * primitive type (boolean / number), plus `desktops` and `activities` which
 * are passed as ID sets (string[]). Assigning to them from the script is
 * exactly what a KWin script does (`window.onAllDesktops = true`), so these
 * are the properties a client may change through its token.
 */
export const UPDATE_PROPS = new Set<string>([
    "opacity", // number
    "skipsCloseAnimation", // boolean
    "fullScreen", // boolean
    "onAllDesktops", // boolean
    "skipTaskbar", // boolean
    "skipPager", // boolean
    "skipSwitcher", // boolean
    "keepAbove", // boolean
    "keepBelow", // boolean
    "minimized", // boolean
    "demandsAttention", // boolean
    "noBorder", // boolean
    "excludeFromCapture", // boolean
    "desktops", // string[] of desktop IDs (empty = all desktops)
    "activities", // string[] of activity IDs (empty = all activities)
]);

/**
 * Properties `windows.query` may read. Same catalog as UPDATE_PROPS for now
 * (only modifiable properties are supported); a dedicated Set keeps the two
 * methods independently extensible.
 */
export const QUERY_PROPS = new Set<string>([...UPDATE_PROPS]);

/**
 * Properties `window.watch` may subscribe to: writable properties that have a
 * `<prop>Changed` signal on KWin.Window (what the listener connects to).
 * Mostly booleans; `desktops` / `activities` are the two non-boolean members
 * (their `desktopsChanged` / `activitiesChanged` signals exist). `opacity` is
 * a number and `onAllDesktops` / `noBorder` have no `<prop>Changed` signal
 * (only `desktopsChanged` / `decorationChanged` exist), so they are not
 * watchable.
 */
export const WATCH_PROPS = new Set<string>([
    "skipsCloseAnimation",
    "fullScreen",
    "skipTaskbar",
    "skipPager",
    "skipSwitcher",
    "keepAbove",
    "keepBelow",
    "minimized",
    "demandsAttention",
    "excludeFromCapture",
    "desktops",
    "activities",
]);

// ---------------------------------------------------------------------------
// params schemas (by-position)
// ---------------------------------------------------------------------------

const UpdateParamsSchema = z.tuple([
    z.string(), // token
    z.record(z.string(), z.unknown()), // updateInfo: property name -> new value
]);

const QueryParamsSchema = z.tuple([
    z.string(), // token
    z.array(z.string()), // queryInfo: property names to read
]);

const WatchParamsSchema = z.tuple([
    z.string(), // token
    z.record(z.string(), z.boolean()), // watchInfo: property name -> watch (true) / unwatch (false)
]);

/** Throw a -32602 Invalid params error with a consistent message prefix. */
function invalidParams(message: string): never {
    throw new JSONRPCErrorException(
        `invalid params: ${message}`,
        JSONRPCErrorCode.InvalidParams,
    );
}

// ---------------------------------------------------------------------------
// watch state
// ---------------------------------------------------------------------------

interface WatchState {
    /** The window the token was bound to when the watch was set up. */
    window: KWin.Window;
    /** property name -> the connected signal slot (needed to disconnect). */
    handlers: Map<string, () => void>;
}

/** Watches by token: every token owns at most one slot per property. */
const watchesByToken = new Map<string, WatchState>();

/** The signal of `prop` on `window` (KWin naming: `<prop>Changed`). */
function propSignal(window: KWin.Window, prop: string): Signal<() => void> {
    return (window as unknown as Record<string, unknown>)[prop + "Changed"] as Signal<() => void>;
}

/**
 * Current value of `prop` on `window`, JSON-serializable by construction:
 * `desktops` / `activities` are reported as ID sets (string[] of UUIDs —
 * empty means "all"), everything else is the raw primitive value.
 */
function propValue(window: KWin.Window, prop: string): unknown {
    if (prop === "desktops") {
        return window.desktops.map((desktop) => String(desktop.id));
    }
    if (prop === "activities") {
        return window.activities.slice();
    }
    return (window as unknown as Record<string, unknown>)[prop];
}

/**
 * Validate an update value for `desktops` / `activities`: it must be an array
 * of IDs and every ID must exist in the workspace catalog maintained from the
 * workspace events. Throws -32602 otherwise. Returns the validated IDs.
 */
function validateIdList(prop: string, value: unknown): string[] {
    if (!Array.isArray(value) || value.some((id) => typeof id !== "string")) {
        invalidParams(`property "${prop}" expects an array of IDs (string[])`);
    }
    const ids = value as string[];
    for (const id of ids) {
        if (prop === "desktops" && !desktopsById.has(id)) {
            invalidParams(`unknown desktop ID "${id}"`);
        }
        if (prop === "activities" && !activitiesById.has(id)) {
            invalidParams(`unknown activity ID "${id}"`);
        }
    }
    return ids;
}

/** Assign one update value to the window (IDs were validated beforehand). */
function applyUpdate(window: KWin.Window, prop: string, value: unknown): void {
    if (prop === "desktops") {
        // Empty list = all desktops (the underlying KWin convention).
        window.desktops = (value as string[]).map(
            (id) => desktopsById.get(id) as KWin.VirtualDesktop,
        );
    } else if (prop === "activities") {
        // Empty list = all activities.
        window.activities = (value as string[]).slice();
    } else {
        (window as unknown as Record<string, unknown>)[prop] = value;
    }
}

/**
 * Set (enable === true) or remove (enable === false) the listener for one
 * property of the token's window. Idempotent: enabling twice keeps a single
 * slot, disabling when nothing is connected is a no-op. Returns whether a
 * listener is connected after the operation.
 */
function setWatch(token: string, conn: RpcConnection, window: KWin.Window, prop: string, enable: boolean): boolean {
    let state = watchesByToken.get(token);
    if (enable) {
        if (!state) {
            state = { window, handlers: new Map() };
            watchesByToken.set(token, state);
        }
        if (!state.handlers.has(prop)) {
            const handler = (): void => {
                notifyClient(conn, "window.changed", {
                    token,
                    property: prop,
                    value: propValue(window, prop),
                });
            };
            propSignal(window, prop).connect(handler);
            state.handlers.set(prop, handler);
            sendLog("info", `window.watch: client ${conn.id} watches ${prop} on token ${token}`);
        }
        return true;
    }
    if (state) {
        const handler = state.handlers.get(prop);
        if (handler !== undefined) {
            try {
                propSignal(state.window, prop).disconnect(handler);
            } catch (error) {
                sendLog("warn", `window.watch: cannot disconnect ${prop} for token ${token}: ${error}`);
            }
            state.handlers.delete(prop);
            if (state.handlers.size === 0) {
                watchesByToken.delete(token);
            }
            sendLog("info", `window.watch: client ${conn.id} no longer watches ${prop} on token ${token}`);
        }
    }
    return false;
}

/**
 * Tear down every watch of the given tokens (called by src/tokens.ts when a
 * token is invalidated or its client disconnects): disconnect the signal
 * slots and forget the state, so no further window.changed notifications are
 * sent for those tokens (e.g. after `superseded` the window is owned by
 * another client and the old owner must not keep listening).
 */
export function dropClientWatches(tokens: Iterable<string>): void {
    for (const token of tokens) {
        const state = watchesByToken.get(token);
        if (!state) {
            continue;
        }
        for (const [prop, handler] of state.handlers) {
            try {
                propSignal(state.window, prop).disconnect(handler);
            } catch (error) {
                // The window may already be destroyed (window_closed).
                sendLog("warn", `window.watch: cannot disconnect ${prop} for token ${token}: ${error}`);
            }
        }
        watchesByToken.delete(token);
    }
}

// ---------------------------------------------------------------------------
// methods
// ---------------------------------------------------------------------------

registerMethod("windows.update", UpdateParamsSchema, (params, conn) => {
    const [token, updateInfo] = params;
    const window = resolveTokenWindow(token, conn);
    // Validate everything first — property names, and the desktops/activities
    // ID values against the workspace catalogs: a single invalid entry makes
    // the whole call an Invalid params error and nothing is applied.
    for (const [prop, value] of Object.entries(updateInfo)) {
        if (!UPDATE_PROPS.has(prop)) {
            invalidParams(`unsupported property "${prop}" in windows.update`);
        }
        if (prop === "desktops" || prop === "activities") {
            validateIdList(prop, value);
        }
    }
    for (const [prop, value] of Object.entries(updateInfo)) {
        applyUpdate(window, prop, value);
    }
    if (Object.keys(updateInfo).length > 0) {
        sendLog("info", `windows.update from client ${conn.id}: token=${token} props=${Object.keys(updateInfo).join(",")}`);
    }
    return null; // void
});

registerMethod("windows.query", QueryParamsSchema, (params, conn) => {
    const [token, queryInfo] = params;
    const window = resolveTokenWindow(token, conn);
    const result: Record<string, unknown> = {};
    const seen = new Set<string>();
    for (const prop of queryInfo) {
        if (!QUERY_PROPS.has(prop)) {
            invalidParams(`unsupported property "${prop}" in windows.query`);
        }
        if (seen.has(prop)) {
            continue; // deduplicate: each asked property appears once
        }
        seen.add(prop);
        result[prop] = propValue(window, prop);
    }
    return result;
});

registerMethod("window.watch", WatchParamsSchema, (params, conn) => {
    const [token, watchInfo] = params;
    const window = resolveTokenWindow(token, conn);
    // The zod schema already rejected non-boolean values; validate the
    // property names (and their watchability) before changing any listener.
    for (const prop of Object.keys(watchInfo)) {
        if (!WATCH_PROPS.has(prop)) {
            invalidParams(
                `unsupported property "${prop}" in window.watch ` +
                `(not a boolean property or no ${prop}Changed signal)`,
            );
        }
    }
    const result: Record<string, boolean> = {};
    for (const [prop, enable] of Object.entries(watchInfo)) {
        result[prop] = setWatch(token, conn, window, prop, enable);
    }
    return result;
});
