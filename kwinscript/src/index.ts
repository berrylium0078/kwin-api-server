// SPDX-License-Identifier: MIT
//
// KWin script bundled by esbuild into a single file
// (kwinscript/dist/kwinscript.js). kwin-api-daemon loads it through
// org.kde.kwin.Scripting.loadScript and runs it via /Scripting/Script<id>.
//
// Everything here runs inside KWin's QJSEngine — no Node.js, no browser.
// The ambient types come from the kwin-ts package (see tsconfig.json).
//
// The script implements the JSON-RPC application layer of the protocol
// (doc/RPC.md): it wraps the daemon's D-Bus poll()/push() interface into a
// control loop plus per-client message loops (src/clients.ts) and dispatches
// client requests through the shared JSON-RPC server (src/jsonrpc.ts), where
// feature modules register their methods with zod-validated params
// (src/tokens.ts — the window claiming protocol).
//
// Importing a module for its side effects registers methods / wires signals;
// the control loop is started below.

import { sendLog } from "./dbus";
import { startControlLoop } from "./clients";
// Side effects: registers "token.request" and wires the window events.
import "./tokens";

void (async () => {
    sendLog("info", "kwinscript loaded");
    print("[kwinscript] loaded by kwin-api-server");
    startControlLoop();
})();
