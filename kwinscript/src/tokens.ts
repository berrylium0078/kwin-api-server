// SPDX-License-Identifier: MIT
//
// Window claiming protocol — token validation and invalidation (see
// doc/RPC.md §"Window tokens").
//
// A client claims one of *its own* windows by proving ownership through the
// window title: the server hands out an opaque token (a fixed-length UUID),
// the client sets its window's title so that it starts with the token, and
// the server — which continuously watches window create / destroy / title
// change events — detects the prefix and answers with a "token.validated"
// notification. From then on the client may refer to the window by the token
// (and may freely rename it: after validation the server makes no assumptions
// about the caption, design note in doc/RPC.md).
//
// Matching is deliberately aggressive (validate on the first unambiguous
// match, no round-trip polling) and may be withdrawn when a rare name
// collision is observed:
//
//   * token.validated    — exactly one window's caption starts with the token
//   * token.invalidated  — reason "timeout"       (never validated in time)
//                         reason "window_closed"  (the bound window closed)
//                         reason "ambiguous"      (>= 2 windows share the prefix)
//                         reason "superseded"     (another token claimed the
//                                                  same window)
//
// Because every token has the same length, a caption change can be checked
// against the token table with a single substring(0, L) lookup.

import { z } from "zod";
import { registerMethod, notifyClient, type RpcConnection } from "./jsonrpc";
import { sendLog } from "./dbus";

/** UUID v4: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx". */
const TOKEN_LENGTH = 36;
/** Upper bound for the validate-phase timeout (per token.request). */
const MAX_VALIDATE_TIMEOUT_MS = 3_600_000;

const TokenRequestSchema = z.object({
    // Validate-phase timeout in milliseconds: the server keeps the token
    // pending for at most this long before failing the validation.
    timeout: z.number().int().min(1).max(MAX_VALIDATE_TIMEOUT_MS),
});

type TokenReason = "timeout" | "window_closed" | "ambiguous" | "superseded";

interface TokenEntry {
    token: string;
    /** The client that requested this token (used to route notifications). */
    conn: RpcConnection;
    state: "pending" | "active";
    /** The bound window; non-null once validated (active). */
    window: KWin.Window | null;
    /** Epoch ms by which the token must have been validated (pending only). */
    deadline: number;
}

/** Stable per-window identifier (KWin's internalId, normalized). */
function windowKey(window: KWin.Window): string {
    // internalId is a QUuid; String() yields "{<uuid>}" — strip the braces.
    return String(window.internalId).replace(/[{}]/g, "");
}

/** Serialize a window for notifications. */
function windowInfo(window: KWin.Window): {
    internalId: string;
    pid: number;
    caption: string;
} {
    return {
        internalId: windowKey(window),
        pid: window.pid,
        caption: window.captionNormal,
    };
}

/** A UUID-v4-shaped token (all tokens have the same length). */
function generateToken(): string {
    const hex = "0123456789abcdef";
    let token = "";
    for (let i = 0; i < TOKEN_LENGTH; i++) {
        switch (i) {
            case 8:
            case 13:
            case 18:
            case 23:
                token += "-";
                break;
            case 14:
                token += "4"; // version nibble
                break;
            default: {
                const r = Math.floor(Math.random() * 16);
                token += hex[i === 19 ? (r & 0x3) | 0x8 : r]; // RFC 4122 variant
            }
        }
    }
    return token;
}

class TokenRegistry {
    readonly byToken = new Map<string, TokenEntry>();
    /** Active tokens keyed by windowKey(window). */
    readonly byWindow = new Map<string, TokenEntry>();
    private readonly timeoutTimer = new QTimer();

    constructor() {
        this.timeoutTimer.singleShot = true;
        this.timeoutTimer.timeout.connect(() => this.onTimeout());
    }

    scheduleTimeout(): void {
        let earliest = Infinity;
        this.byToken.forEach((entry) => {
            if (entry.state === "pending" && entry.deadline < earliest) {
                earliest = entry.deadline;
            }
        });
        if (earliest === Infinity) {
            this.timeoutTimer.stop();
            return;
        }
        this.timeoutTimer.interval = Math.max(1, earliest - Date.now());
        this.timeoutTimer.start();
    }

    onTimeout(): void {
        const now = Date.now();
        const expired: TokenEntry[] = [];
        this.byToken.forEach((entry) => {
            if (entry.state === "pending" && entry.deadline <= now) {
                expired.push(entry);
            }
        });
        for (const entry of expired) {
            invalidate(entry, "timeout");
        }
        this.scheduleTimeout();
    }
}

const registry = new TokenRegistry();

/** Called on every caption change and on window creation. */
function checkCaption(window: KWin.Window): void {
    const caption = window.captionNormal;
    if (caption.length < TOKEN_LENGTH) {
        return;
    }
    // All tokens have the same length: the prefix is the map key.
    const prefix = caption.substring(0, TOKEN_LENGTH);
    const entry = registry.byToken.get(prefix);
    if (!entry) {
        return;
    }
    recount(entry);
}

/**
 * Re-evaluate one token against the current set of windows. Aggressive
 * strategy: a single unambiguous match validates (or confirms) the token; two
 * or more matching windows make the ownership ambiguous and withdraw it.
 */
function recount(entry: TokenEntry): void {
    const matches = workspace.windowList().filter(
        (w) => w.captionNormal.length >= TOKEN_LENGTH && w.captionNormal.startsWith(entry.token),
    );
    if (matches.length > 1) {
        invalidate(entry, "ambiguous");
        return;
    }
    if (matches.length === 0) {
        return;
    }
    const window = matches[0];
    if (entry.state === "pending") {
        validate(entry, window);
    }
    // active + single match: the bound window (or, after a free rename, some
    // other window) carries the prefix — ownership is unambiguous, nothing to
    // do (the server makes no assumptions about post-validation captions).
}

/** Bind a pending token to its window and notify the owner. */
function validate(entry: TokenEntry, window: KWin.Window): void {
    const key = windowKey(window);
    const existing = registry.byWindow.get(key);
    if (existing && existing !== entry) {
        // Another token already owns this window — it is superseded (it may
        // well belong to a different client).
        invalidate(existing, "superseded");
    }
    entry.state = "active";
    entry.window = window;
    registry.byWindow.set(key, entry);
    notifyClient(entry.conn, "token.validated", {
        token: entry.token,
        window: windowInfo(window),
    });
    sendLog("info", `token ${entry.token} validated for client ${entry.conn.id} on ${key}`);
}

/** Remove a token from the registry and notify its owner with the reason. */
function invalidate(entry: TokenEntry, reason: TokenReason): void {
    if (entry.state === "active" && entry.window) {
        registry.byWindow.delete(windowKey(entry.window));
    }
    registry.byToken.delete(entry.token);
    registry.scheduleTimeout();
    notifyClient(entry.conn, "token.invalidated", {
        token: entry.token,
        reason,
        window: entry.window ? windowInfo(entry.window) : null,
    });
    sendLog("info", `token ${entry.token} invalidated (${reason}) for client ${entry.conn.id}`);
}

function onWindowAdded(window: KWin.Window): void {
    // Track title changes of every managed window.
    window.captionNormalChanged.connect(() => checkCaption(window));
    window.captionNormalChanged.connect(() => sendLog("warning",
        `window ${window.internalId} changed caption ${window.captionNormal}`));
    // A freshly created window may already carry a token prefix.
    checkCaption(window);
}

function onWindowRemoved(window: KWin.Window): void {
    const entry = registry.byWindow.get(windowKey(window));
    if (entry) {
        invalidate(entry, "window_closed");
    }
}

/** Drop every token owned by a disconnected client (no notification). */
export function dropClientTokens(clientId: number): void {
    const removed: TokenEntry[] = [];
    registry.byToken.forEach((entry) => {
        if (entry.conn.id === clientId) {
            removed.push(entry);
        }
    });
    for (const entry of removed) {
        if (entry.state === "active" && entry.window) {
            registry.byWindow.delete(windowKey(entry.window));
        }
        registry.byToken.delete(entry.token);
    }
    registry.scheduleTimeout();
    if (removed.length > 0) {
        sendLog("info", `client ${clientId} disconnected: dropped ${removed.length} token(s)`);
    }
}

function wireWindowEvents(): void {
    workspace.windowAdded.connect(onWindowAdded);
    workspace.windowRemoved.connect(onWindowRemoved);
    // A script (re)load must keep tracking captions of already-managed windows.
    for (const window of workspace.windowList()) {
        window.captionNormalChanged.connect(() => checkCaption(window));
    }
}

// --- method registration (runs once at script load) --------------------------

registerMethod("token.request", TokenRequestSchema, (params, conn) => {
    const token = generateToken();
    registry.byToken.set(token, {
        token,
        conn,
        state: "pending",
        window: null,
        deadline: Date.now() + params.timeout,
    });
    registry.scheduleTimeout();
    sendLog("info", `token.request from client ${conn.id}: token=${token} timeout=${params.timeout}ms`);
    return { token };
});

try {
    wireWindowEvents();
    sendLog("info", "tokens: window events wired");
} catch (error) {
    sendLog("error", `tokens: cannot wire window events: ${error}`);
}
