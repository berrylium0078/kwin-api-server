# Development Guide — kwin-api-server

This document describes how the project is built internally and how to test
it. For basic usage (build/install/configuration/socket) see the
[README](../README.md).

## Architecture overview

`kwin-api-daemon` is a single-threaded daemon driven by one libsystemd
`sd-event` loop. Three subsystems are multiplexed on that loop:

1. **unix socket server** — binds `service.socket` in its working directory
   and serves clients (`sd_event_add_io` per client, concurrent read/write);
2. **own session-bus D-Bus service** — acquires `KWIN_API_SERVICE_NAME` and
   serves the daemon's own interface (`sd_bus_attach_event` on the same loop);
3. **KWin scripting client** — loads/runs/unloads the KWin script through
   `org.kde.kwin.Scripting`.

The loaded KWin script (`kwinscript.js`) talks *back* to the daemon over the
same D-Bus name (the `log()` method on `/daemon`), so the loop is the single
coordination point for everything.

## Project structure

```
xmake.lua                  build configuration (C++20, xmake + Ninja)
run.py                     convenience runner: stop -> (--build) -> start -> follow
                           logs via the real systemd user unit
run.sh                     manual foreground test run (private bus + mock KWin)
daemon/
  src/
    main.cpp               entry point + orchestration (event loop lifecycle)
    config.{hpp,cpp}       env-var configuration (pure, unit-tested)
    log.{hpp,cpp}          leveled stderr logging (-> journal under systemd)
    file_util.{hpp,cpp}    script staging + @DAEMON_DBUS_SERVICE@ substitution
    socket_server.{hpp,cpp} sd-event driven unix socket server
    dbus_service.{hpp,cpp} sd-bus session-bus service (name + own interface)
    kwin_client.{hpp,cpp}  KWin scripting D-Bus client (loadScript/run/unloadScript)
  systemd/
    kwin-api-server.service.in user unit template; xmake fills in the install
                              paths and installs the generated file
                              (-> <prefix>/lib/systemd/user)
    kwinscript-unload.sh      ExecStopPost safety net (-> <prefix>/libexec)
kwinscript/
  src/index.ts             the KWin script (bundled by esbuild)
  kwin-ts/                 KWin scripting API TS declarations (ambient package,
                           wired into the typecheck via tsconfig.json typeRoots)
  tsconfig.json / package.json / esbuild.build.mjs
  dist/kwinscript.js       generated single-file bundle
test/
  unit/                    C++ unit tests (kwin-api-test, `xmake test`)
  mock_kwin.cpp            mock org.kde.KWin (sd-bus) for integration tests
  integration.sh           automated end-to-end test (private bus + mock)
doc/
  DEVELOP.md               this document
  PROTOCOL.md              communication protocol — transport layer frozen
                           (phase 0); implementation pending
  RPC.md                   JSONRPC methods (TBD, placeholder)
build/                     xmake build directory (and build.ninja)
```

## D-Bus interface

### The daemon's own service

The daemon serves two objects on its session-bus name (`KWIN_API_SERVICE_NAME`,
default `org.example.KwinApiServer`):

* the object path **derived from the name** (`org.example.KwinApiServer` →
  `/org/example/KwinApiServer`):

  ```
  org.example.KwinApiServer.Status() -> s
  ```

* the fixed path **`/daemon`**, used by the loaded KWin script. The interface
  name is the same dotted string as the service name, so both follow
  `KWIN_API_SERVICE_NAME`:

  ```
  <service name>.log(level: s, msg: s) -> ()
  ```

  `log()` writes `msg` to the daemon's journal output with a `[script]` source
  tag, mapping `level` to the daemon's own log levels
  (`debug` / `info` / `warn` / `error`; anything else is logged as `info`).

The frozen transport protocol ([PROTOCOL.md](PROTOCOL.md) §3) extends this
script-facing interface (planned, not yet implemented):

* `/daemon` gains `poll(timeout: i) -> s` — daemon → script control messages
  (e.g. client connect/disconnect notifications), returned as a
  JSON-serialized array string;
* one object per connected client at `/cli${id}` with the fixed interface
  `org.example.KwinApiClient`, exposing:
  * `poll(timeout: i) -> s` — that client's inbound messages (client →
    script direction);
  * `push(msg: s) -> s` — queue one message for that client's socket (script
    → client direction), returning `""` on success or an error string when
    the message exceeds 1 MB or the client's 16 MB write buffer is full.

`Introspect` is provided automatically by sd-bus.

### How the script finds the daemon

The KWin script never hardcodes the daemon's service name. It ships with the
placeholder constant `@DAEMON_DBUS_SERVICE@`
(`kwinscript/src/index.ts`); while **staging** the bundle as `kwinscript.js`
in the working directory, the daemon rewrites every occurrence to its runtime
service name (`daemon/src/file_util.cpp`). The script then calls
`<name> log("<level>", "<message>")` on `/daemon` through a Promise wrapper
around KWin's callback-based `callDBus()`, guarded by a single-shot `QTimer`
that rejects the call if no reply arrives within 5 s.

### KWin scripting (client side)

The daemon drives KWin through its scripting D-Bus API
(`daemon/src/kwin_client.cpp`):

```
org.kde.kwin.Scripting.loadScript(filePath: s, pluginName: s) -> i
org.kde.kwin.Script.run()                        on /Scripting/Script<id>
org.kde.kwin.Scripting.unloadScript(pluginName: s) -> b
```

`loadScript` is retried while KWin is still starting (`KWIN_LOAD_RETRIES` /
`KWIN_LOAD_RETRY_DELAY_MS`). On shutdown `unloadScript` is sent
fire-and-forget; the unit's `ExecStopPost` (`kwinscript-unload.sh`) repeats it
as a safety net for SIGKILL/crash.

## Script startup (kwinscript)

1. **Build** — `xmake` runs `pnpm --dir kwinscript build`: `tsc --noEmit`
   (type check against the kwin-ts declarations) followed by esbuild into a
   single IIFE (`dist/kwinscript.js`). The esbuild target is **ES2016**
   because KWin's QJSEngine supports ES6 (Promise, generators) but not
   `async`/`await` (KDE bug 478617 / QTBUG-58620) — esbuild lowers those.
   `xmake` only re-bundles when a **non-git-ignored** input under
   `kwinscript/` changed (`src/`, `kwin-ts/`, `tsconfig.json`, `package.json`,
   `pnpm-lock.yaml`, `esbuild.build.mjs`); touching the git-ignored `dist/`,
   `node_modules/` or the generated `pnpm-workspace.yaml` does not trigger a
   rebuild (see the `kwinscript` target in `xmake.lua`).
2. **Stage** — the daemon copies the bundle to `<work_dir>/kwinscript.js`,
   substituting `@DAEMON_DBUS_SERVICE@` with its runtime service name.
3. **Load & run** — `loadScript(stagedPath, KWIN_PLUGIN_NAME)` → `run()`.
   The script registers a demo global shortcut, and sends `log("info",
   "kwinscript loaded")` to the daemon (which shows up as `[script] ...` in
   the journal).
4. **Unload** — graceful shutdown sends `unloadScript(pluginName)`.

## Testing

### Unit tests — `xmake test`

C++ unit tests in `test/unit/` (target `kwin-api-test`, run with
`xmake test` or directly: `build/linux/x86_64/release/kwin-api-test`). The
harness is minimal: `TEST(name)` registers a case, `CHECK(cond)` /
`CHECK_EQ(a, b)` fail it, and the binary prints `ALL TESTS PASSED (N)` on
success (xmake's test target matches on that).

Covered areas:

* `config_*` / `dbus_name_validation` / `object_path_derivation` — env-var
  parsing and validation (`daemon/src/config.*`);
* `copy_file_overwrite_*` / `stage_kwinscript_*` — file helpers, including
  placeholder substitution and in-place re-staging (`daemon/src/file_util.*`);
* `socket_server_*` — bind, line protocol, concurrent clients, EOF, max
  clients (`daemon/src/socket_server.*`).

Add new cases in the existing `test/unit/*.cpp` files; they are picked up
automatically.

### Integration test — `test/integration.sh`

End-to-end test against a **private session bus** with a **mock
`org.kde.KWin`** (`kwin-api-mock`, `test/mock_kwin.cpp`) — no real KWin
needed. Prerequisites: `dbus-daemon`, `socat`, `dbus-send` (and the built
binaries from `xmake`).

```sh
test/integration.sh
```

It verifies:

* `--check` mode (socket bound + D-Bus name registered);
* script staging: the fake bundle's `@DAEMON_DBUS_SERVICE@` placeholder is
  substituted with the runtime service name in the staged copy;
* `loadScript`/`run` call sequence and arguments on the mock;
* the daemon's `log()` method: a `dbus-send` call to `/daemon` must appear as
  `[script] ...` in the daemon log;
* the unix socket line protocol (two concurrent clients: ping/pong, status);
* graceful shutdown (SIGTERM) → `unloadScript` + socket cleanup.

### Manual test run — `run.sh`

A foreground variant for poking at things by hand. It starts a private
session bus + the mock KWin + the real daemon (with the *real* kwinscript
bundle), all in the foreground, so the daemon's output streams to your
terminal. No real KWin or systemd needed.

```sh
./run.sh        # after `xmake`
```

It prints the socket path; talk to it from another terminal with:

```sh
socat - UNIX-CONNECT:<printed-socket-path>     # type: ping / status / quit
```

Ctrl+C stops the daemon and tears down the mock and the private bus.

### systemd-based runner — `run.py`

`run.py` drives the *real* systemd user unit instead (see README):

```sh
./run.py            # stop, start, follow logs; on exit stops again
./run.py --build    # + rebuild with xmake, staged-install to ./stage,
                    #   systemctl link + daemon-reload, then start
```

## Notes / troubleshooting

* The session bus address: `sd_bus_open_user` honors
  `DBUS_SESSION_BUS_ADDRESS` and falls back to `$XDG_RUNTIME_DIR/bus`, so the
  unit does not need to set it.
* `sd_bus_set_close_on_exit(bus, 0)` keeps the connection usable after the
  event loop stops, which the shutdown-time `unloadScript` relies on.
* If the script does not show up in KWin, test KWin's scripting API directly:
  `qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript <file> <plugin>`.
