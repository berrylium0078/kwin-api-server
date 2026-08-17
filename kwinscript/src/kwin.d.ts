// SPDX-License-Identifier: MIT
//
// Ambient type declarations for the KWin scripting API (QJSEngine globals).
// This file is intentionally a *subset* — only what the bundled script (and
// the KWin scripts in this project) actually use.
//
// Reference: https://develop.kde.org/docs/plasma/kwin/api/
//            https://develop.kde.org/docs/plasma/kwin/scripting/

// Log a message to KWin's debug output (visible with `kwin_x11 --replace`
// debug output / journalctl for kwin_wayland).
declare function print(message?: unknown): void;

// Register a global shortcut; the callback fires when the user presses it.
declare function registerShortcut(
    id: string,
    description: string,
    defaultShortcut: string,
    callback: () => void,
): Shortcut;

interface Shortcut {
    active: boolean;
}

interface Window {
    readonly caption: string;
    readonly resourceClass: string;
    readonly resourceName: string;
    readonly desktop: number;
    close(): void;
    minimize(): void;
    maximize(): void;
    unmaximize(): void;
    activate(): void;
}

interface Workspace {
    windowList(): Window[];
    readonly activeWindow: Window | null;
    currentDesktop: number;
    desktops: number;
    slotToggleMaximize(): void;
    slotSwitchToNextDesktop(): void;
    slotSwitchToPreviousDesktop(): void;
}

declare const workspace: Workspace;

// Call an arbitrary D-Bus method; resolves with the reply (if any).
declare function callDBus(
    service: string,
    path: string,
    interfaceName: string,
    method: string,
    ...args: unknown[]
): Promise<unknown>;

// Persistent per-plugin config (stored by KWin).
declare function readConfig(key: string, defaultValue?: string): string;
declare function writeConfig(key: string, value: string): void;
declare const options: Record<string, string>;
