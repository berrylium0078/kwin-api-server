/// <reference path="./kwin.d.ts" />
//
// SPDX-License-Identifier: MIT
//
// Sample KWin script bundled by esbuild into a single file
// (kwinscript/dist/kwinscript.js). kwin-api-daemon loads it through
// org.kde.kwin.Scripting.loadScript and runs it via /Scripting/Script<id>.
//
// Everything here runs inside KWin's QJSEngine — no Node.js, no browser.

const SCRIPT_ID = "kwin-api-server-demo";

// A shortcut to prove the script is alive. Press Meta+Ctrl+Shift+K and watch
// KWin's debug output / journal:
//   journalctl --user -u kwin_wayland --since today
registerShortcut(`${SCRIPT_ID}:toggle`, "KWin API Server demo", "Meta+Ctrl+Shift+K", () => {
    const windows = workspace.windowList();
    print(`[${SCRIPT_ID}] toggle: ${windows.length} window(s) tracked`);
    workspace.slotToggleMaximize();
});

// Example: call a D-Bus method from the script (fire-and-forget, no await
// needed — KWin keeps the promise alive):
//   void callDBus("org.kde.KWin", "/KWin", "org.kde.KWin", "nextActivity");

print(`[${SCRIPT_ID}] loaded by kwin-api-server`);
