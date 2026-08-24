// SPDX-License-Identifier: MIT
//
// Window claiming protocol — token validation and invalidation (see
// doc/RPC.md §"Window tokens").
//
// A client claims one of *its own* windows by proving ownership through the
// window title: the server hands out an opaque token (a fixed-length base64
// string), the client sets its window's title so that it starts with the
// token, and the server — which continuously watches window create / destroy
// / title change events — detects the prefix and answers with a
// "token.validated" notification. From then on the client may refer to the
// window by the token (and may freely rename it: after validation the server
// makes no assumptions about the caption, design note in doc/RPC.md).
//
// Matching is deliberately aggressive (validate on the first match, no
// round-trip polling). Each token carries its own single-shot CoarseTimer
// armed at request time:
//
//   * while the deadline timer is active, the validate window is open — a
//     *second, different* window matching the same token makes the
//     validation ambiguous and the token is withdrawn ("ambiguous");
//   * when the timer fires, a still-pending token fails validation
//     ("timeout"); for an active token the timer's inactivity simply marks
//     the end of the validate window — later caption matches are ignored;
//   * "window_closed" and "superseded" behave as before.
//
// Because every token has the same length, a caption change can be checked
// against the token table with a single substring(0, L) lookup — no
// windowList scan is needed.

import { z } from "zod";
import { JSONRPCErrorCode, JSONRPCErrorException } from "json-rpc-2.0";
import { registerMethod, notifyClient, type RpcConnection } from "./jsonrpc";
import { sendLog } from "./dbus";
import { dropClientWatches } from "./windows";

/** Tokens are 20 chars from the base64 alphabet (120 bits of entropy). */
const TOKEN_LENGTH = 20;
const TOKEN_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/** Upper bound for the validate-phase timeout (per token.request). */
const MAX_VALIDATE_TIMEOUT_MS = 3_600_000;
/**
 * Qt::CoarseTimer (5% accuracy) — the event loop may coalesce the deadline
 * timers of nearby tokens into one wakeup. The Qt namespace is not exposed
 * to KWin scripts, so use the enum value directly (kwin-ts:
 * TimerType.CoarseTimer === 1; KWin's ScriptTimer already defaults to it).
 */
const COARSE_TIMER = 1 as TimerType;

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
    /** The matched (and once validated, bound) window; null until the first
     * caption match. This is what the ambiguity check compares against. */
    window: KWin.Window | null;
    /** Single-shot CoarseTimer armed at request time. While it is active the
     * validate deadline has not passed yet; after it fires, `active ===
     * false` is the "deadline reached" marker used by the ambiguity check. */
    deadlineTimer: QTimer;
}

const byToken = new Map<string, TokenEntry>();
/** Active tokens keyed by windowKey(window). */
const byWindow = new Map<string, TokenEntry>();

/** Stable per-window identifier (KWin's internalId, normalized). */
function windowKey(window: KWin.Window): string {
    // internalId is a QUuid; String() yields "{<uuid>}" — strip the braces.
    return String(window.internalId).replace(/[{}]/g, "");
}

/** Serialize a window for notifications (caption only). */
function windowInfo(window: KWin.Window): { caption: string } {
    return {
        caption: window.captionNormal,
    };
}

/**
 * A random token of exactly TOKEN_LENGTH base64 characters. Duplicate tokens
 * are skipped by rejection sampling (120 bits make collisions practically
 * impossible, but uniqueness is guaranteed by the map lookup).
 */
function generateToken(): string {
    for (;;) {
        let token = "";
        for (let i = 0; i < TOKEN_LENGTH; i++) {
            token += TOKEN_ALPHABET[Math.floor(Math.random() * TOKEN_ALPHABET.length)];
        }
        if (!byToken.has(token)) {
            return token;
        }
        // Duplicate — resample.
    }
}

/**
 * Called on every caption change and on window creation. A caption whose
 * first TOKEN_LENGTH characters name a known token drives the per-entry
 * state machine (aggressive validation + pre-deadline ambiguity detection).
 */
function checkCaption(window: KWin.Window): void {
    const caption = window.captionNormal;
    if (caption.length < TOKEN_LENGTH) {
        return;
    }
    // All tokens have the same length: the prefix is the map key.
    const prefix = caption.substring(0, TOKEN_LENGTH);
    const entry = byToken.get(prefix);
    if (!entry) {
        return;
    }
    // Ambiguity is only defined *before the validate deadline*: once the
    // deadline timer fired (active === false), a later caption match is
    // ignored — the server makes no assumptions about captions after the
    // validate window.
    if (!entry.deadlineTimer.active) {
        return;
    }
    if (entry.window === null) {
        // First match: validate immediately (aggressive strategy — the
        // client claims its own window by title).
        validate(entry, window);
    } else if (entry.window !== window) {
        // A second, different window carries the same prefix before the
        // deadline: the validation is ambiguous — withdraw the token.
        invalidate(entry, "ambiguous");
    }
    // The same window matched again: nothing to do.
}

/** Bind a token to its window and notify the owner. */
function validate(entry: TokenEntry, window: KWin.Window): void {
    const key = windowKey(window);
    const existing = byWindow.get(key);
    if (existing && existing !== entry) {
        // Another token already owns this window — it is superseded (it may
        // well belong to a different client).
        invalidate(existing, "superseded");
    }
    entry.state = "active";
    entry.window = window;
    byWindow.set(key, entry);
    notifyClient(entry.conn, "token.validated", {
        token: entry.token,
        window: windowInfo(window),
    });
    sendLog("info", `token ${entry.token} validated for client ${entry.conn.id} on ${key}`);
}

/** Remove a token from the registry and notify its owner with the reason. */
function invalidate(entry: TokenEntry, reason: TokenReason): void {
    entry.deadlineTimer.stop(); // the validate window no longer matters
    if (entry.state === "active" && entry.window) {
        byWindow.delete(windowKey(entry.window));
    }
    byToken.delete(entry.token);
    // Any property listeners the owner set up through this token (window.watch)
    // are torn down: the client no longer owns the window, so it must not keep
    // receiving window.changed notifications for it.
    dropClientWatches([entry.token]);
    notifyClient(entry.conn, "token.invalidated", {
        token: entry.token,
        reason,
        window: entry.window ? windowInfo(entry.window) : null,
    });
    sendLog("info", `token ${entry.token} invalidated (${reason}) for client ${entry.conn.id}`);
}

/** The per-entry deadline fired. */
function onTokenDeadline(entry: TokenEntry): void {
    if (entry.state === "pending") {
        // Never validated within the validate window.
        invalidate(entry, "timeout");
    }
    // Active: the validate window is over. The timer going inactive is the
    // "deadline reached" marker for the ambiguity check; nothing else to do.
}

function onWindowAdded(window: KWin.Window): void {
    // Track title changes of every managed window.
    window.captionNormalChanged.connect(() => checkCaption(window));
    // A freshly created window may already carry a token prefix.
    checkCaption(window);
}

function onWindowRemoved(window: KWin.Window): void {
    const entry = byWindow.get(windowKey(window));
    if (entry) {
        invalidate(entry, "window_closed");
    }
}

/** Drop every token owned by a disconnected client (no notification). */
export function dropClientTokens(clientId: number): void {
    const removed: TokenEntry[] = [];
    byToken.forEach((entry) => {
        if (entry.conn.id === clientId) {
            removed.push(entry);
        }
    });
    for (const entry of removed) {
        entry.deadlineTimer.stop();
        if (entry.state === "active" && entry.window) {
            byWindow.delete(windowKey(entry.window));
        }
        byToken.delete(entry.token);
    }
    // Same for the property listeners of those tokens.
    dropClientWatches(removed.map((entry) => entry.token));
    if (removed.length > 0) {
        sendLog("info", `client ${clientId} disconnected: dropped ${removed.length} token(s)`);
    }
}

/**
 * Resolve a client's token to the window it is bound to, or throw -32602.
 *
 * This is the shared entry point for every token-referencing method
 * (windows.update / windows.query / window.watch, and future ones). A token
 * must exist, belong to the calling client (the token is the ownership
 * proof — another client knowing the token must not control the window) and
 * be in the `active` state (pending tokens have no window yet).
 */
export function resolveTokenWindow(token: string, conn: RpcConnection): KWin.Window {
    const entry = byToken.get(token);
    if (!entry) {
        throw new JSONRPCErrorException(
            `invalid params: unknown token "${token}"`,
            JSONRPCErrorCode.InvalidParams,
        );
    }
    if (entry.conn !== conn) {
        throw new JSONRPCErrorException(
            `invalid params: token "${token}" belongs to another client`,
            JSONRPCErrorCode.InvalidParams,
        );
    }
    if (entry.state !== "active" || entry.window === null) {
        throw new JSONRPCErrorException(
            `invalid params: token "${token}" is not bound to a window`,
            JSONRPCErrorCode.InvalidParams,
        );
    }
    return entry.window;
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
    // Each token owns its deadline timer (CoarseTimer, so nearby deadlines
    // are coalesced by Qt). It keeps running after validation: its firing is
    // what closes the ambiguity window.
    const deadlineTimer = new QTimer();
    deadlineTimer.singleShot = true;
    deadlineTimer.timerType = COARSE_TIMER;
    deadlineTimer.interval = params.timeout;
    const entry: TokenEntry = {
        token,
        conn,
        state: "pending",
        window: null,
        deadlineTimer,
    };
    deadlineTimer.timeout.connect(() => onTokenDeadline(entry));
    deadlineTimer.start();
    byToken.set(token, entry);
    sendLog("info", `token.request from client ${conn.id}: token=${token} timeout=${params.timeout}ms`);
    return { token };
});

try {
    wireWindowEvents();
    sendLog("info", "tokens: window events wired");
} catch (error) {
    sendLog("error", `tokens: cannot wire window events: ${error}`);
}
