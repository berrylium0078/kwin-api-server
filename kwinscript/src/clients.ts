// SPDX-License-Identifier: MIT
//
// Client registry and message loops.
//
// The script multiplexes the daemon's D-Bus interface on one control loop
// (/daemon poll() -> client connect / disconnect events) plus one poll loop
// per connected client (/cli{id} poll() -> the client's inbound JSON-RPC
// messages). Every inbound message is dispatched through the shared
// JSON-RPC server (src/jsonrpc.ts); responses and server-initiated
// notifications are pushed back over the same /cli{id} object.
//
// A client's poll loop and the daemon control loop each hold the *only*
// pending poll() on their D-Bus object, as required by PROTOCOL.md §3.2
// (at most one pending poll per object).

import { pollClient, pollDaemon, pushToClient, sendLog, sleep, DBUS_RETRY_DELAY } from "./dbus";
import { dispatchMessage, type RpcConnection } from "./jsonrpc";
import { dropClientTokens } from "./tokens";
import { dropClientWorkspaceWatches } from "./workspace";

/** One connected client: inbound poll loop + outbound push. */
class ClientConnection implements RpcConnection {
    readonly id: number;
    alive = true;

    constructor(id: number) {
        this.id = id;
    }

    /** Queue one JSON-RPC message (response / notification) for the socket. */
    send(message: unknown): void {
        void pushToClient(this.id, JSON.stringify(message));
    }

    /**
     * The client's inbound poll loop. Runs until the client disconnects or
     * the /cli{id} object disappears. Never throws.
     */
    async run(): Promise<void> {
        while (this.alive) {
            let value: unknown;
            try {
                value = await pollClient(this.id);
            } catch (error) {
                if (!this.alive) {
                    break;
                }
                sendLog("warn", `rpc: client ${this.id} poll failed (${error}); retrying`);
                await sleep(DBUS_RETRY_DELAY);
                continue;
            }
            if (!this.alive) {
                break;
            }
            if (Array.isArray(value)) {
                for (const message of value) {
                    try {
                        await dispatchMessage(this, message);
                    } catch (error) {
                        sendLog("warn", `rpc: client ${this.id} dispatch error: ${error}`);
                    }
                }
                // Yield to the event loop between batches so D-Bus replies,
                // window signals and timers always get a turn even when a
                // client floods the socket.
                await sleep(0);
            } else if (typeof value === "string") {
                // poll() returned a JSON-encoded error message (PROTOCOL.md §3.2),
                // e.g. "a poll is already pending" right after a wrapper timeout.
                // Back off so a persistent error cannot hot-loop the D-Bus calls.
                sendLog("warn", `rpc: client ${this.id} poll() error: ${value}`);
                await sleep(DBUS_RETRY_DELAY);
            } else {
                sendLog("warn", `rpc: client ${this.id}: unexpected poll() payload`);
                await sleep(DBUS_RETRY_DELAY);
            }
        }
    }

    stop(): void {
        this.alive = false;
    }
}

const connections = new Map<number, ClientConnection>();

function onClientConnected(id: number): void {
    if (connections.has(id)) {
        return;
    }
    const conn = new ClientConnection(id);
    connections.set(id, conn);
    sendLog("info", `rpc: client ${id} connected`);
    void conn.run();
}

function onClientDisconnected(id: number): void {
    const conn = connections.get(id);
    if (!conn) {
        return;
    }
    conn.stop();
    connections.delete(id);
    // All tokens owned by the disconnected client are dropped (its window
    // bindings become free again); no notification can be delivered anymore.
    dropClientTokens(id);
    // Same for its workspace-property listeners (workspace.changed).
    dropClientWorkspaceWatches(id);
    sendLog("info", `rpc: client ${id} disconnected`);
}

/**
 * The daemon control loop: poll /daemon forever and react to client connect /
 * disconnect events. Started once at script load; never resolves.
 */
export async function startControlLoop(): Promise<void> {
    sendLog("info", "rpc: control loop started");
    while (true) {
        let value: unknown;
        try {
            value = await pollDaemon();
        } catch (error) {
            sendLog("warn", `rpc: daemon control poll failed (${error}); retrying`);
            await sleep(DBUS_RETRY_DELAY);
            continue;
        }
        if (!Array.isArray(value)) {
            if (typeof value === "string") {
                // e.g. "a poll is already pending" right after a wrapper
                // timeout; back off instead of hot-looping.
                sendLog("warn", `rpc: daemon poll() error: ${value}`);
                await sleep(DBUS_RETRY_DELAY);
            }
            continue;
        }
        for (const event of value) {
            if (typeof event !== "object" || event === null) {
                continue;
            }
            const ev = event as { event?: unknown; id?: unknown };
            if (ev.event === "client_connected" && typeof ev.id === "number") {
                onClientConnected(ev.id);
            } else if (ev.event === "client_disconnected" && typeof ev.id === "number") {
                onClientDisconnected(ev.id);
            }
        }
        // Yield to the event loop (see the client loop above).
        await sleep(0);
    }
}
