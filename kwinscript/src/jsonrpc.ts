// SPDX-License-Identifier: MIT
//
// The shared JSON-RPC layer of the KWin script.
//
// One JSONRPCServer instance is created here and exported; feature modules
// (e.g. src/tokens.ts) import it and register their methods through
// registerMethod(), which validates the params with a zod schema before
// dispatching to the handler. Requests from every client are dispatched
// through this single server (the JSON-RPC 2.0 `receive` call carries the
// originating connection as `serverParams`).
//
// Server -> client communication (notifications such as token.validated /
// token.invalidated) is sent with notifyClient() through the same connection
// object.

import {
    JSONRPCServer,
    JSONRPCErrorCode,
    JSONRPCErrorException,
    createJSONRPCNotification,
} from "json-rpc-2.0";
import type { JSONRPCRequest } from "json-rpc-2.0";
import type { ZodType } from "zod";
import { sendLog } from "./dbus";

/**
 * The per-connection context passed to every registered method as
 * `serverParams` (see clients.ts — ClientConnection implements this).
 * Deliberately an interface here so feature modules never need to import the
 * client registry (no import cycles).
 */
export interface RpcConnection {
    /** The daemon-assigned client id (stable for the connection's lifetime). */
    readonly id: number;
    /** False once the client disconnected; notifications are dropped then. */
    readonly alive: boolean;
    /**
     * Queue one JSON-RPC message (request/response/notification object) for
     * the client's socket.
     */
    send(message: unknown): void;
}

/** The single JSON-RPC server shared by all clients and modules. */
export const rpcServer = new JSONRPCServer<RpcConnection>({
    // json-rpc-2.0's default error listener is console.warn, which is not
    // reliable inside QJSEngine; route unexpected method errors to the daemon
    // log instead (sendLog is async and never throws).
    errorListener: (error: unknown) => {
        void sendLog("warn", `jsonrpc: unexpected error: ${String(error)}`);
    },
});

/**
 * Register a JSON-RPC method whose `params` are validated with a zod schema.
 * On schema failure the client receives a proper -32602 Invalid params error
 * (JSON-RPCErrorException is mapped by json-rpc-2.0 into the error response).
 */
export function registerMethod<Params, Result>(
    name: string,
    schema: ZodType<Params>,
    handler: (params: Params, conn: RpcConnection) => Result | Promise<Result>,
): void {
    rpcServer.addMethod(name, (rawParams: unknown, conn: RpcConnection) => {
        const parsed = schema.safeParse(rawParams);
        if (!parsed.success) {
            const detail = parsed.error.issues
                .map((issue) => {
                    const path = issue.path.length > 0 ? issue.path.join(".") : "(root)";
                    return `${path}: ${issue.message}`;
                })
                .join("; ");
            throw new JSONRPCErrorException(
                `invalid params: ${detail}`,
                JSONRPCErrorCode.InvalidParams,
                parsed.error.issues,
            );
        }
        return handler(parsed.data, conn);
    });
}

/**
 * Send a server -> client JSON-RPC notification (e.g. "token.validated").
 * Dropped silently once the client disconnected.
 */
export function notifyClient(conn: RpcConnection, method: string, params: unknown): void {
    if (!conn.alive) {
        return;
    }
    conn.send(createJSONRPCNotification(method, params));
}

/**
 * Dispatch one inbound message from a client through the shared server and
 * push the response (if any) back to that client. Notifications from the
 * client (requests without an id) yield no response.
 */
export async function dispatchMessage(conn: RpcConnection, message: unknown): Promise<void> {
    const response = await rpcServer.receive(message as JSONRPCRequest | JSONRPCRequest[], conn);
    if (response !== null && response !== undefined) {
        conn.send(response);
    }
}
