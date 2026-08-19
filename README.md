# kwin-api-server

A systemd user service that acts as a **proxy between clients and KWin's
scripting API**: it exposes KWin scripting to clients as JSONRPC over a unix
socket, so external programs can drive KWin (windows, virtual desktops,
global shortcuts, …).

```
                    ┌─────────────────────────────── kwin-api-daemon ───────────────┐
 clients ────────►  │  unix socket (service.socket)      sd-event loop               │
 (JSONRPC, TBD)     │     │  (sd_event_add_io per client, concurrent read/write)     │
                    │     ▼                                                          │
 other D-Bus apps   │  sd-bus: session bus (KWIN_API_SERVICE_NAME)                   │
 ────────────────►  │     │  (sd_bus_attach_event → same loop)                       │
                    │     ▼                                                          │
                    │  org.kde.KWin /Scripting: loadScript / run / unloadScript      │
                    └────────────────────────────────────────────────────────────────┘
                                      │ loads
                                      ▼
                              kwinscript.js (esbuild bundle of
                              kwinscript/src/*.ts, loaded by KWin)
```

**Status**: the unix socket / D-Bus / KWin-scripting bridge is in place. The
transport protocol is frozen (phase 0) and implemented in phases 1–2 — see
[doc/PROTOCOL.md](doc/PROTOCOL.md) for the length-prefixed JSON framing, the
per-client buffers, the daemon ↔ script `poll()`/`push()` interface and the
client session management (Server / ClientSession). The JSONRPC method layer
on top of the transport is still being specified ([doc/RPC.md](doc/RPC.md));
the socket currently carries raw JSON payloads that the KWin script receives
via `/cli${id}` `poll()`.

## Requirements

* Linux with systemd (user manager) — `libsystemd` (sd-bus/sd-event), headers
  e.g. `libsystemd-dev` / `systemd-libs`
* C++20 compiler (gcc ≥ 10 or clang ≥ 10)
* [xmake](https://xmake.io) ≥ 2.9, [Ninja](https://ninja-build.org)
* Node.js ≥ 18, [pnpm](https://pnpm.io) ≥ 10 (for the TypeScript subproject)
* D-Bus session bus (`dbus-daemon`); a KWin (Plasma) session for the real
  KWin interaction
* `dbus-send` for the `ExecStopPost` unload helper

## Build

```sh
# xmake flow (default): configures with Ninja as the underlying build engine
xmake f -m release
xmake                        # builds the kwinscript bundle (tsc + esbuild) and all C++ targets
xmake test                   # unit tests
```

`xmake` builds the kwinscript bundle first (the daemon target depends on it).
The bundle can also be built and type-checked standalone:

```sh
pnpm --dir kwinscript typecheck     # tsc --noEmit against the kwin-ts declarations
pnpm --dir kwinscript build         # tsc --noEmit, then esbuild -> dist/kwinscript.js
```

esbuild targets **ES2016**: KWin evaluates scripts in QJSEngine, which
supports ES6 (Promise, generators, …) but not the ES2017 `async`/`await`
keywords (KDE bug 478617 / QTBUG-58620), so esbuild lowers those while keeping
the rest native. `xmake` only re-bundles when a non-git-ignored input under
`kwinscript/` changed (see [doc/DEVELOP.md](doc/DEVELOP.md) → Script
startup).

## Install

```sh
sudo xmake install           # installs to $KAS_INSTALLDIR (default /usr)
xmake install -o <dir>       # staged install to a different prefix, e.g. ./stage
```

The systemd user unit is **generated at install time** from
`daemon/systemd/kwin-api-server.service.in`; the paths inside it
(`ExecStart=`, `ExecStopPost=`, `KWIN_SCRIPT_PATH=`) always follow the
*effective* install prefix. Installed files (default `/usr` prefix):

| path | content |
|---|---|
| `$KAS_INSTALLDIR/bin/kwin-api-daemon` | the daemon |
| `$KAS_INSTALLDIR/share/kwin-api-server/kwinscript.js` | esbuild bundle of the KWin script |
| `$KAS_INSTALLDIR/lib/systemd/user/kwin-api-server.service` | systemd user unit (generated) |
| `$KAS_INSTALLDIR/libexec/kwin-api-server/kwinscript-unload.sh` | ExecStopPost unload helper |

### Quick dev loop with run.py

`run.py` manages the systemd unit for you: stop → (optionally rebuild and
staged-install to `./stage`, then `systemctl link` + `daemon-reload`) → start →
follow the logs; on exit it stops the service again.

```sh
./run.py            # stop, start, follow logs (Ctrl+C stops again)
./run.py --build    # also: xmake build + staged install + (re)link the unit
```

## Configuration

| variable | default | description |
|---|---|---|
| `KWIN_API_SERVICE_NAME` | `org.example.KwinApiServer` | D-Bus name to register on the session bus |
| `KWIN_SCRIPT_PATH` | – (required) | path of the bundled `kwinscript.js` |
| `KWIN_PLUGIN_NAME` | – (required) | `pluginName` for KWin scripting |
| `KWIN_WORK_DIR` | current directory | where `service.socket` + the staged `kwinscript.js` live |
| `KWIN_LOAD_RETRIES` | `30` | `loadScript` retries while KWin is unreachable |
| `KWIN_LOAD_RETRY_DELAY_MS` | `1000` | delay between retries |
| `KWIN_MAX_CLIENTS` | `64` | max concurrent unix-socket clients |
| `KWIN_DEBUG` | – | `1` enables verbose logging |

Admin overrides can be dropped in `/etc/kwin-api-server/env`
(`EnvironmentFile=-` in the unit). CLI: `--help`, `--version`, `--check`
(verify socket + D-Bus name without touching KWin).

## Running as a systemd user service

```sh
# after a real install (or: link the staged unit — run.py --build does this)
systemctl --user daemon-reload
systemctl --user enable --now kwin-api-server.service
systemctl --user status kwin-api-server.service
journalctl --user -u kwin-api-server.service -f
```

The unit is tied to `graphical-session.target` (KWin is up before the script
is loaded; the daemon additionally retries `loadScript` while KWin is
starting). `RuntimeDirectory=kwin-api-server` gives the daemon a fresh,
systemd-managed working directory, so `service.socket` and the staged
`kwinscript.js` are cleaned up automatically.

## Talking to the service

The socket lives in the daemon's working directory. Under systemd that is the
runtime directory:

```
$XDG_RUNTIME_DIR/kwin-api-server/service.socket
# e.g. /run/user/1000/kwin-api-server/service.socket
```

(When run manually, it is `<KWIN_WORK_DIR>/service.socket`.)

```sh
socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/kwin-api-server/service.socket"
```

The socket speaks the length-prefixed JSON framing of
[doc/PROTOCOL.md](doc/PROTOCOL.md): each message is a 4-byte native-endian
length followed by a UTF-8 JSON payload (1 MB limit). Incoming frames are
delivered to the KWin script via `/cli${id}` `poll()`, and script replies
arrive back via `push()`. The old `ping`/`status`/`echo` line protocol is
gone.

## Documentation

* [doc/DEVELOP.md](doc/DEVELOP.md) — development guide: project structure,
  D-Bus interface, how the KWin script is started, and how to test the
  project (unit tests, integration test, `run.sh` manual run, `run.py`).
* [doc/PROTOCOL.md](doc/PROTOCOL.md) — communication protocol: client ↔
  daemon framing (length-prefixed UTF-8 JSON, 1 MB message limit, 16 MB
  per-direction buffers) and daemon ↔ script D-Bus interface
  (`poll` / `push`). **Transport frozen (phase 0); implemented in
  phases 1–2.**
* [doc/RPC.md](doc/RPC.md) — supported JSONRPC methods. **TBD, placeholder.**
