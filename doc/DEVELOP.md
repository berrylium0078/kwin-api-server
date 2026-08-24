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
    socket_server.{hpp,cpp} legacy line-protocol socket server (superseded by
                            Server, kept for its unit tests)
    dbus_service.{hpp,cpp} sd-bus session-bus service (name + Status object)
    kwin_client.{hpp,cpp}  KWin scripting D-Bus client (loadScript/run/unloadScript)
    server.{hpp,cpp}       Server: listen/accept, client ids, session map,
                           /daemon object wiring (connect/disconnect events)
    client_session.{hpp,cpp} ClientSession: per-client proto::Client + /cli{id}
                            D-Bus object + sd-event IO multiplexing
    daemon_dbus_object.{hpp,cpp} /daemon object: log() + poll() (control queue)
    client_dbus_object.{hpp,cpp} /cli{id} object: poll() + push()
    poll_waiter.{hpp,cpp}  deferred D-Bus reply helper (async poll timeouts)
    dbus_util.{hpp,cpp}    shared poll() handler + JSON error encoding
    proto/                 pure socket protocol layer (phase 1, no libsystemd):
      protocol.hpp           limits + RxBuffer / TxBuffer / Client classes
      rx_buffer.cpp          RX message queue (JSON array splice, poll/drain)
      tx_buffer.cpp          TX queue (circular buffer: complete frames,
                             push/flush; flushed space is reused immediately)
      client.cpp             Client session + explicit RX state machine
  systemd/
    kwin-api-server.service.in user unit template; xmake fills in the install
                              paths and installs the generated file
                              (-> <prefix>/lib/systemd/user)
    kwinscript-unload.sh      ExecStopPost safety net (-> <prefix>/libexec)
kwinscript/
  src/
    index.ts             the KWin script entry (bundled by esbuild)
    dbus.ts              Promise wrappers around KWin's callDBus(): the
                         daemon's log()/poll()/push() interface
    jsonrpc.ts           the shared JSON-RPC server (json-rpc-2.0) +
                         registerMethod(name, zodSchema, handler): params
                         validated with zod, failures become -32602 errors
    clients.ts           daemon control loop (/daemon poll) + one poll loop
                         per client (/cli{id} poll -> dispatch -> push)
    tokens.ts            the window-claim token protocol (doc/RPC.md §2):
                         token registry, window-event wiring, validation
                         state machine, timeout timers; also the shared
                         resolveTokenWindow() used by token-referencing
                         methods
    windows.ts           window property methods (doc/RPC.md §3):
                         windows.update / windows.query / window.watch with
                         by-position params, the three immutable
                         supported-property Sets, the per-token watch
                         registry and the window.changed notification;
                         also the workspace desktop/activity ID catalogs
                         (desktopsChanged rescan, activityAdded/Removed
                         incremental updates) used to validate
                         desktops/activities update values
    workspace.ts         workspace property methods (doc/RPC.md §4):
                         workspace.update / workspace.query / workspace.watch
                         for currentDesktop / currentActivity / desktops /
                         activities, the per-client watch registry and the
                         workspace.changed notification; ID values validated
                         against the catalogs from windows.ts
  kwin-ts/               KWin scripting API TS declarations (ambient package,
                         wired into the typecheck via tsconfig.json typeRoots)
  tsconfig.json / package.json / esbuild.build.mjs
  dist/kwinscript.js     generated single-file bundle
test/
  unit/                    C++ unit tests (kwin-api-test, `xmake test`)
  daemon/                  Python end-to-end test against the real systemd
                           user service (mock service unit + test_daemon.py)
  rpc/                     Python end-to-end test of the JSON-RPC application
                           layer (test_rpc.py): the token protocol and the
                           window + workspace property methods — real service
                           via run.py + real PyQt6 windows in a Plasma session
doc/
  DEVELOP.md               this document
  PROTOCOL.md              communication protocol — transport layer frozen
                           (phase 0), implemented
  RPC.md                   JSON-RPC application layer — tokens, window
                           properties and workspace properties implemented,
                           more planned
build/                     xmake build directory (and build.ninja)
```

## D-Bus interface

### The daemon's own service

The daemon serves several objects on its session-bus name
(`KWIN_API_SERVICE_NAME`, default `org.example.KwinApiServer`):

* the object path **derived from the name** (`org.example.KwinApiServer` →
  `/org/example/KwinApiServer`), served by `DbusService`:

  ```
  org.example.KwinApiServer.Status() -> s
  ```

* the fixed path **`/daemon`**, used by the loaded KWin script
  (`DaemonDBusObject`). The interface name is the same dotted string as the
  service name, so both follow `KWIN_API_SERVICE_NAME`:

  ```
  <service name>.log(level: s, msg: s) -> ()
  <service name>.poll(timeout: i) -> s
  ```

  `log()` writes `msg` to the daemon's journal output with a `[script]` source
  tag, mapping `level` to the daemon's own log levels
  (`debug` / `info` / `warn` / `error`; anything else is logged as `info`).
  `poll()` returns daemon → script control messages — client connect /
  disconnect notifications queued by the `Server`:
  `{"event":"client_connected","id":N}` /
  `{"event":"client_disconnected","id":N}` — as a JSON-serialized array
  string, with the poll() semantics of [PROTOCOL.md](PROTOCOL.md) §3.2.

* one object per connected client at `/cli${id}` with the fixed interface
  `org.example.KwinApiClient` (`ClientDBusObject`, owned by `ClientSession`):

  ```
  org.example.KwinApiClient.poll(timeout: i) -> s
  org.example.KwinApiClient.push(msg: s) -> s
  ```

  `poll()` returns that client's inbound messages (client → script direction);
  `push()` queues one message for that client's socket (script → client
  direction), returning `""` on success or an error string when the message
  exceeds 1 MB or the client's 16 MB write buffer is full. Both objects share
  the poll() semantics (one pending poll per object, timeout 0..25000 ms,
  JSON-encoded error strings) via `dbus_util::handle_poll_request` and
  `PollWaiter` (deferred sd-bus replies + timeout timers).

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
  clients (`daemon/src/socket_server.*`);
* `rx_buffer_*` / `tx_buffer_*` / `proto_*` — the pure socket protocol layer
  (`daemon/src/proto/*`), driven over `socketpair()`: header splits (1+1+1+1,
  2+2, full), payload splits, multiple frames in one write, empty JSON values,
  the 1 MB boundary (near / exactly / over), oversized-frame discard followed
  by a normal frame, RX-buffer-full pause + resume, TX-buffer-full, peer
  close, EAGAIN, a TX flush round-trip, and TX ring wrap-around (partial
  flushes interleaved with pushes must reuse flushed space).

Add new cases in the existing `test/unit/*.cpp` files; they are picked up
automatically.

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

### systemd-based integration test — `test/daemon/test_daemon.py`

A Python end-to-end test that runs the **real daemon under the real systemd
user manager**. It links/starts/stops a mock service unit (`test/daemon/
kwin-api-server-test.service`, generated from `...service.in`) that makes the
daemon load an **empty script** (`test/daemon/empty-kwinscript.js`) instead of
the built bundle, then plays the roles of *several socket clients* and of
*the KWin script* at the same time, talking to the daemon over both the unix
socket and its session-bus D-Bus objects (`/daemon`, `/cli{id}`).

```sh
python3 test/daemon/test_daemon.py      # after `xmake`
```

It needs a running systemd user manager, a session bus and the **real KWin of
a Plasma session** (the daemon loads the empty script into `org.kde.KWin`, so
the name must be owned). The mock unit sets small
`KWIN_RX_BUFFER_CAP`/`KWIN_TX_BUFFER_CAP` so backpressure tests reachable with
a few hundred KB instead of megabytes. Focus is the D-Bus call semantics that
the C++ unit tests cannot cover: poll() batching/FIFO, blocking-timeout
behaviour, concurrent-poll rejection, JSON-encoded poll errors vs. plain-text
push errors, push() write-buffer-full, object lifetime after disconnect, and
the log() surface. On exit the service is stopped and the user is reminded to
remove the `systemctl --user link` (`~/.config/systemd/user/
kwin-api-server-test.service`).

### JSON-RPC e2e test — `test/rpc/test_rpc.py`

A Python end-to-end test of the **application layer** (doc/RPC.md §2–§4)
against the **real service** in a real Plasma session. It starts
`kwin-api-server.service`
through run.py (`./run.py --no-follow`, optionally `--build`), then plays
several JSON-RPC clients over the unix socket and creates / renames / closes
**real windows with PyQt6** to drive the token protocol through its actual
KWin window events. Requires PyQt6.

```sh
./run.py --build              # once: build + staged install
python3 test/rpc/test_rpc.py  # afterwards (or: python3 test/rpc/test_rpc.py --build)
```

Covers every boundary case of the token protocol: basic validation (window
info in `token.validated`), the validate-phase timeout, `window_closed`,
`ambiguous` (a second window sharing the prefix *before* the validate
deadline — and the boundary case that a second window *after* the deadline is
**not** ambiguous), `superseded` (another — possibly different — client
claims the same window), multi-client isolation, free renames after
validation, disconnect cleanup, and the JSON-RPC error surface (`-32602`
invalid params via zod, `-32601` unknown method). On top of that it exercises
the window-property methods of §3: update/query round-trips (including
deduplication and the atomic "no partial apply" behavior on unsupported
properties), watch enable/disable with `window.changed` notifications
(including idempotency — a double watch does not double-notify), watch
cleanup when a token is superseded by another client, by-position params,
id-`null` requests (the method runs, no reply is sent), the token
resolution errors (unknown / foreign / not-yet-validated token) and the
`desktops` / `activities` ID-set wire format. And the workspace-property
methods of §4: querying the current desktop / activity and the full desktop /
activity ID lists, updating `currentDesktop` / `currentActivity` (with
unknown-ID rejection and restore), and `workspace.changed` notifications for
watched workspace properties.

The daemon is transport-only: **all** application logic lives in the KWin
script (`kwinscript/src/jsonrpc.ts` + `clients.ts` + `tokens.ts` +
`windows.ts` + `workspace.ts`), so this
test runs against the real daemon with the real bundle.

## Notes / troubleshooting

* The session bus address: `sd_bus_open_user` honors
  `DBUS_SESSION_BUS_ADDRESS` and falls back to `$XDG_RUNTIME_DIR/bus`, so the
  unit does not need to set it.
* `sd_bus_set_close_on_exit(bus, 0)` keeps the connection usable after the
  event loop stops, which the shutdown-time `unloadScript` relies on.
* If the script does not show up in KWin, test KWin's scripting API directly:
  `qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript <file> <plugin>`.
