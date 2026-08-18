# Communication Protocol

> **Status: TBD — placeholder.** This document is not finalized; the protocol
> below is a sketch of the planned design. Nothing here is implemented yet
> unless explicitly marked otherwise.

## 1. Client ↔ daemon (unix socket)

The daemon binds `service.socket` in its working directory (under systemd:
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket`). The planned protocol is
**JSONRPC over the unix socket** (newline-delimited JSON messages). Clients
connect, send requests, and receive responses/notifications; the socket
server multiplexes concurrent clients on the sd-event loop.

* transport: unix stream socket
* framing: newline-delimited JSON (TBD)
* protocol: JSONRPC 2.0 (TBD — see [RPC.md](RPC.md) for the method list)
* error/notification semantics: TBD

**Interim line protocol (currently implemented, `daemon/src/socket_server.*`)**:

```
ping    -> pong
status  -> <status line>
quit    -> server closes the connection
<other> -> echo <other>
```

## 2. Daemon ↔ script (D-Bus)

The loaded KWin script and the daemon talk over the session bus. The script
must know the daemon's service name at runtime — it ships with the
`@DAEMON_DBUS_SERVICE@` placeholder, which the daemon substitutes while
staging the bundle (see [DEVELOP.md](DEVELOP.md) → "How the script finds the
daemon").

### Implemented

The script calls the daemon's `log()` method on `/daemon`:

```
service:   <KWIN_API_SERVICE_NAME>           (e.g. org.example.KwinApiServer)
path:      /daemon
interface: <same dotted string as the service name>
method:    log(level: s, msg: s) -> ()
```

The daemon writes the message to its journal output with a `[script]` source
tag. The call goes through a Promise wrapper around KWin's callback-based
`callDBus()`, with a single-shot `QTimer` timeout (5 s).

### Planned (TBD)

Future daemon → script / script → daemon calls, e.g. invoking arbitrary KWin
scripting operations from the daemon on behalf of socket clients (window
listing, activation, shortcuts, …). The exact interface will be defined here
once [RPC.md](RPC.md) is specified.
