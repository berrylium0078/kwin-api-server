// SPDX-License-Identifier: MIT
//
// Workspace property methods — workspace.update / workspace.query /
// workspace.watch (doc/RPC.md §"Workspace properties").
//
// These operate on the WorkspaceWrapper (the global workspace state), not on
// a claimed window: no token is involved. Like the window-property methods
// (src/windows.ts) they take their params **by position** (params is an
// array), are zod-validated, reject unsupported property names with -32602
// before applying anything, and answer requests with `id: null` silently
// (src/jsonrpc.ts dispatchMessage).
//
// First version of the property catalog:
//
//   * `currentDesktop` / `currentActivity` — updatable, queryable, watchable;
//     the wire value is the desktop / activity ID (KDE UUID), validated
//     against the workspace catalogs maintained in src/windows.ts;
//   * `desktops` / `activities` — the full workspace lists, queryable and
//     watchable only (read-only at the workspace level): wire value is the
//     ID set (string[]), empty never occurs here (they are the current lists,
//     not a window assignment).
//
// A watch connects to the corresponding `<prop>Changed` signal of the
// WorkspaceWrapper; on change the client receives a "workspace.changed"
// notification with the property's current value. Watches are per client and
// torn down on disconnect (dropClientWorkspaceWatches, called from
// src/clients.ts).

import { z } from "zod";
import { JSONRPCErrorCode, JSONRPCErrorException } from "json-rpc-2.0";
import { registerMethod, notifyClient, type RpcConnection } from "./jsonrpc";
import { lookupDesktop, hasActivity } from "./windows";
import { sendLog } from "./dbus";

// ---------------------------------------------------------------------------
// Supported-property catalog (immutable by convention — never mutate these
// Sets; extend them as more workspace properties become supported)
// ---------------------------------------------------------------------------

/** Properties `workspace.update` accepts (writable WorkspaceWrapper
 * properties; the wire value is the ID string, validated against the
 * catalogs). */
export const WORKSPACE_UPDATE_PROPS = new Set<string>([
    "currentDesktop", // string (desktop ID)
    "currentActivity", // string (activity ID)
]);

/** Properties `workspace.query` may read. */
export const WORKSPACE_QUERY_PROPS = new Set<string>([
    "currentDesktop", // string (desktop ID)
    "currentActivity", // string (activity ID)
    "desktops", // string[] of desktop IDs (all desktops)
    "activities", // string[] of activity IDs (all activities)
]);

/** Properties `workspace.watch` may subscribe to: every WorkspaceWrapper
 * property with a `<prop>Changed` signal. */
export const WORKSPACE_WATCH_PROPS = new Set<string>([
    "currentDesktop",
    "currentActivity",
    "desktops",
    "activities",
]);

// ---------------------------------------------------------------------------
// params schemas (by-position)
// ---------------------------------------------------------------------------

const UpdateParamsSchema = z.tuple([
    z.record(z.string(), z.unknown()), // updateInfo: property name -> new value
]);

const QueryParamsSchema = z.tuple([
    z.array(z.string()), // queryInfo: property names to read
]);

const WatchParamsSchema = z.tuple([
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
// values
// ---------------------------------------------------------------------------

/**
 * Current wire value of a workspace property: desktop / activity IDs are
 * reported as their UUID strings, `desktops` / `activities` as ID sets.
 */
function workspaceValue(prop: string): unknown {
    if (prop === "currentDesktop") {
        return String(workspace.currentDesktop.id);
    }
    if (prop === "currentActivity") {
        return workspace.currentActivity;
    }
    if (prop === "desktops") {
        return workspace.desktops.map((desktop) => String(desktop.id));
    }
    return workspace.activities.slice(); // "activities"
}

/** Validate an update value for a workspace property: it must be an ID that
 * exists in the workspace catalog. Throws -32602 otherwise. */
function validateId(prop: string, value: unknown): string {
    if (typeof value !== "string") {
        invalidParams(`property "${prop}" expects an ID (string)`);
    }
    if (prop === "currentDesktop" && !lookupDesktop(value)) {
        invalidParams(`unknown desktop ID "${value}"`);
    }
    if (prop === "currentActivity" && !hasActivity(value)) {
        invalidParams(`unknown activity ID "${value}"`);
    }
    return value;
}

// ---------------------------------------------------------------------------
// watch state (per client — the workspace signals are global)
// ---------------------------------------------------------------------------

/** Watches by client id: property name -> the connected signal slot. */
const watchesByClient = new Map<number, Map<string, () => void>>();

/** The signal of `prop` on the WorkspaceWrapper (KWin naming:
 * `<prop>Changed`). */
function workspaceSignal(prop: string): Signal<() => void> {
    return (workspace as unknown as Record<string, unknown>)[prop + "Changed"] as Signal<() => void>;
}

/**
 * Set (enable === true) or remove (enable === false) the listener for one
 * workspace property. Idempotent per property and client. Returns whether a
 * listener is connected after the operation.
 */
function setWatch(conn: RpcConnection, prop: string, enable: boolean): boolean {
    let handlers = watchesByClient.get(conn.id);
    if (enable) {
        if (!handlers) {
            handlers = new Map();
            watchesByClient.set(conn.id, handlers);
        }
        if (!handlers.has(prop)) {
            const handler = (): void => {
                notifyClient(conn, "workspace.changed", {
                    property: prop,
                    value: workspaceValue(prop),
                });
            };
            workspaceSignal(prop).connect(handler);
            handlers.set(prop, handler);
            sendLog("info", `workspace.watch: client ${conn.id} watches ${prop}`);
        }
        return true;
    }
    if (handlers) {
        const handler = handlers.get(prop);
        if (handler !== undefined) {
            workspaceSignal(prop).disconnect(handler);
            handlers.delete(prop);
            if (handlers.size === 0) {
                watchesByClient.delete(conn.id);
            }
            sendLog("info", `workspace.watch: client ${conn.id} no longer watches ${prop}`);
        }
    }
    return false;
}

/** Tear down every watch of a disconnected client (called from
 * src/clients.ts). */
export function dropClientWorkspaceWatches(clientId: number): void {
    const handlers = watchesByClient.get(clientId);
    if (!handlers) {
        return;
    }
    for (const [prop, handler] of handlers) {
        try {
            workspaceSignal(prop).disconnect(handler);
        } catch (error) {
            sendLog("warn", `workspace.watch: cannot disconnect ${prop} for client ${clientId}: ${error}`);
        }
    }
    watchesByClient.delete(clientId);
}

// ---------------------------------------------------------------------------
// methods
// ---------------------------------------------------------------------------

registerMethod("workspace.update", UpdateParamsSchema, (params, conn) => {
    const [updateInfo] = params;
    // Validate everything first — property names and ID values against the
    // workspace catalogs: a single invalid entry makes the whole call an
    // Invalid params error and nothing is applied.
    for (const [prop, value] of Object.entries(updateInfo)) {
        if (!WORKSPACE_UPDATE_PROPS.has(prop)) {
            invalidParams(`unsupported property "${prop}" in workspace.update`);
        }
        validateId(prop, value);
    }
    for (const [prop, value] of Object.entries(updateInfo)) {
        if (prop === "currentDesktop") {
            // validateId ran for every entry above, so the cast is safe.
            workspace.currentDesktop = lookupDesktop(value as string) as KWin.VirtualDesktop;
        } else {
            workspace.currentActivity = value as string;
        }
    }
    if (Object.keys(updateInfo).length > 0) {
        sendLog("info", `workspace.update from client ${conn.id}: props=${Object.keys(updateInfo).join(",")}`);
    }
    return null; // void
});

registerMethod("workspace.query", QueryParamsSchema, (params, conn) => {
    const [queryInfo] = params;
    const result: Record<string, unknown> = {};
    const seen = new Set<string>();
    for (const prop of queryInfo) {
        if (!WORKSPACE_QUERY_PROPS.has(prop)) {
            invalidParams(`unsupported property "${prop}" in workspace.query`);
        }
        if (seen.has(prop)) {
            continue; // deduplicate: each asked property appears once
        }
        seen.add(prop);
        result[prop] = workspaceValue(prop);
    }
    return result;
});

registerMethod("workspace.watch", WatchParamsSchema, (params, conn) => {
    const [watchInfo] = params;
    // The zod schema already rejected non-boolean values; validate the
    // property names before changing any listener.
    for (const prop of Object.keys(watchInfo)) {
        if (!WORKSPACE_WATCH_PROPS.has(prop)) {
            invalidParams(`unsupported property "${prop}" in workspace.watch (no ${prop}Changed signal)`);
        }
    }
    const result: Record<string, boolean> = {};
    for (const [prop, enable] of Object.entries(watchInfo)) {
        result[prop] = setWatch(conn, prop, enable);
    }
    return result;
});
