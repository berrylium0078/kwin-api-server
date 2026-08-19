# Communication Protocol

> **Status: frozen (phase 0).** This document is the contract for the
> *transport* layer of kwin-api-server: how messages are framed and delivered
> between the three parties — **clients** (unix socket), the **daemon**
> (`kwin-api-daemon`) and the **KWin script** (`kwinscript.js`, D-Bus).
>
> The transport semantics below are frozen and are being implemented in
> phases:
>
> * **phase 1 (implemented)** — the pure socket protocol layer in
>   `daemon/src/proto/*` (`kwin-api-proto`, unit-tested via `socketpair()`):
>   the frame-decoder state machine (module 1), the RX message queue with the
>   JSON-array splice (module 2) and the TX queue (module 3). This layer has
>   **no D-Bus and no libsystemd** dependency.
> * **phase 2 (implemented)** — client session management in the daemon
>   (`Server`, `ClientSession`, `DaemonDBusObject`, `ClientDBusObject`,
>   `PollWaiter`): the framed protocol is now the daemon's socket protocol.
>   The daemon accepts clients, assigns increasing positive integer ids,
>   serves `/daemon` poll() (log + control messages) and a `/cli${id}` object
>   per client (poll + push), all multiplexed on one sd-event loop. The
>   legacy line protocol (`SocketServer`) is superseded.
>
> The *application* layer (what the JSON payloads mean, the JSONRPC method
> list) is specified separately in [RPC.md](RPC.md) and is still TBD.

## 1. Overview and layering

There are two links and three parties:

```
 clients ───────────► (unix socket) ───────────► daemon ──────────► (D-Bus) ──────────► script
```

* **client ↔ daemon** — a unix stream socket carrying length-prefixed UTF-8
  JSON frames (§2).
* **daemon ↔ script** — the session-bus D-Bus service the daemon owns; the
  script receives and sends *message payloads* through the daemon's
  `poll()`/`push()` methods (§3). A client's message never leaves the daemon
  as a D-Bus frame — only its payload travels over D-Bus.

## 2. Client ↔ daemon (unix socket)

### 2.1 Transport

The daemon binds `service.socket` in its working directory (under systemd:
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket`) and accepts unix stream
connections. Clients are multiplexed concurrently on the daemon's single
sd-event loop.

### 2.2 Framing

Every message is a JSON value, serialized as UTF-8, prefixed with its length
as a 32-bit unsigned integer in **native byte order**:

```text
+----------------------+----------------------+
| uint32 native endian | UTF-8 JSON payload   |
| payload length       | length bytes         |
+----------------------+----------------------+
        4 bytes
```

* `length` is the number of **UTF-8 payload bytes**, **excluding** the 4-byte
  header. A complete frame is `4 + length` bytes on the wire.
* **Single-message maximum: 1 MB = 1,000,000 bytes** (payload). The frame
  itself is therefore at most `1,000,004` bytes.
* *Native byte order*: both peers of a unix socket live on the same host, so
  byte order is unambiguous and no conversion is performed.

### 2.3 Buffers and flow control

For each connected client the daemon maintains **two per-direction buffers,
each capped at 16 MB** (16,000,000 bytes — same decimal convention as the
message limit above):

* **read buffer** (client → daemon): holds the bytes received from the socket
  until complete frames can be extracted, plus the payloads of complete
  frames that are still queued for the script. `poll()` drains it.
* **write buffer** (daemon → client): holds complete frames queued by the
  script via `push()` until they are flushed to the socket. It is implemented
  as a **circular buffer** (`TxBuffer`): space freed by a partial flush is
  reused immediately, so a slow client's `push()`es only block when the whole
  16 MB is genuinely full.

**Inbound processing (client → daemon):**

1. Bytes are read into the read buffer until at least one complete frame
   (`4 + length` bytes) is buffered.
2. **Oversized frame** — `length > 1,000,000`: the daemon consumes and
   discards the **entire frame** (all `length` payload bytes, as they arrive,
   without buffering them), writes an error log line, and **keeps the
   connection alive** — the client may continue sending further frames.
3. Otherwise the payload is extracted, queued for `poll()` and the buffered
   bytes are released.
4. **Read-buffer full** — if the read buffer cannot accommodate the current
   frame (reading more would exceed the 16 MB cap), the daemon **pauses
   reading** from that client's socket. It resumes once a `poll()` call has
   drained queued messages and freed buffer space. A single message can never
   exceed the buffer (1 MB < 16 MB), so this only happens under backpressure,
   when the script has not polled for a while.

**Outbound processing (daemon → client):**

1. Messages in this direction are always created by the script's `push()`
   (see §3.3). At that point the daemon has already checked the message length
   and the write-buffer capacity, so the write path needs **no further
   validation**.
2. While the write buffer is **non-empty**, the daemon watches the socket for
   writability and flushes frames; when the buffer is empty no write watcher
   is needed. A write error (client gone) closes the connection.

## 3. Daemon ↔ script (D-Bus)

The loaded KWin script and the daemon talk over the session bus. The daemon
serves the objects below under its D-Bus name (`KWIN_API_SERVICE_NAME`,
default `org.example.KwinApiServer`). The script derives the name from the
`@DAEMON_DBUS_SERVICE@` placeholder substituted at staging time (see
[DEVELOP.md](DEVELOP.md) → "How the script finds the daemon").

### 3.1 Objects and interfaces

| object path | interface | methods |
|---|---|---|
| `/daemon` | `<service name>` (default `org.example.KwinApiServer`) | `log(level: s, msg: s) -> ()` *(existing)*, `poll(timeout: i) -> s` *(new)* |
| `/cli${id}` — one per connected client | `org.example.KwinApiClient` | `poll(timeout: i) -> s`, `push(msg: s) -> s` |

* `id` is a positive integer the daemon assigns when a client connects; it is
  unique among live clients and stable for the connection's lifetime. The
  script learns about clients (their `id`s) from daemon → script
  notifications delivered through `/daemon` `poll()`.
* **Control message payloads** (implemented, phase 2): client connect /
  disconnect events are queued on `/daemon` as JSON objects —

  ```
  {"event":"client_connected","id":N}
  {"event":"client_disconnected","id":N}
  ```

  so a `/daemon` `poll()` returns e.g. `[{"event":"client_connected","id":3}]`.
  The script uses these to start/stop polling the corresponding `/cli${id}`
  objects.
* **Implementation status**: all objects and methods in the table are
  implemented (`daemon/src/dbus_service.cpp` for the derived path,
  `daemon/src/daemon_dbus_object.cpp` for `/daemon`, `daemon/src/
  client_dbus_object.cpp` for `/cli${id}`).

### 3.2 `poll(timeout: i) -> s` — receiving messages

Returns the messages currently queued for this object.

* **Return value** — a UTF-8 **string** that is always valid JSON. In the
  normal case it is the **JSON serialization of an array** whose elements are
  the queued message payloads **themselves**, embedded as JSON values (not
  wrapped in strings) — e.g.
  `[{"jsonrpc":"2.0","id":1},{"jsonrpc":"2.0","id":2}]`. Since every JSONRPC
  message is a JSON object, the script can `JSON.parse` the returned string
  **once** and directly use it as an `object[]`. In the error case it is a
  **JSON-encoded string** — e.g. `"Error: invalid client"` — i.e. the error
  message quoted and escaped per JSON rules (quotes, backslashes, control
  characters), so `JSON.parse` yields the error message as a string. The
  script distinguishes the two cases by the parsed result: an **array** means
  messages, a **string** means an error. The daemon deliberately returns the
  array as a plain string rather than a D-Bus array/variant to avoid
  `callDBus()` compatibility problems. The daemon builds the array by
  splicing the **verbatim** payload texts (`[` + payload₁ + `,` + payload₂ +
  `]`) without interpreting them, so the transport keeps payloads opaque; the
  payloads are valid JSON values by framing contract.
* **Which messages** — `/cli${id}` `poll()` returns the payloads of the
  frames received from that client (client → script direction);
  `/daemon` `poll()` returns daemon → script control messages (e.g. client
  connect/disconnect notifications; payload format application-layer).
* **Timing** — if at least one message is already queued, the call returns
  **immediately** with **all** currently queued messages (FIFO batch).
  Otherwise the call **blocks** until the first message arrives and then
  returns the batch. If no message arrives within `timeout` milliseconds, it
  returns the **empty array** `[]`. `timeout = 0` is allowed and never
  blocks: it returns the queued messages, or `[]` if there are none.
* **Concurrency** — at most **one pending `poll()` per object**; a second
  `poll()` on the same object while one is pending is ignored and returns an
  error. Different objects are independent: the script may poll `/daemon`
  and several `/cli${id}` objects at the same time.
* **Timeout range** — `0 ≤ timeout ≤ 25_000` milliseconds. Out-of-range
  values return an error without blocking.

### 3.3 `push(msg: s) -> s` — sending to a client

Queues one message for a client's socket.

* `msg` is the message payload (a string); the daemon frames it (4-byte
  header + payload) and appends the frame to the client's write buffer.
* **Success** — the message is queued; returns the **empty string** `""`.
* **Failure** (message ignored, error string returned instead) when:
  * `msg` is longer than 1 MB (its UTF-8 length > 1,000,000 bytes), or
  * the client's write buffer cannot hold the frame (`4 + length` bytes
    would exceed the 16 MB cap).

### 3.4 Message flow

```
 client ───────frame──────► daemon ──payload──► /cli${id} poll() ──► script
 script ──push()──► /cli${id} ──frame──► daemon write buffer ──► client
 daemon ──control──► /daemon poll() ──► script
```

## 4. Frozen decisions (phase 0)

* Decimal size convention throughout: **1 MB = 1,000,000 bytes**;
  **16 MB = 16,000,000 bytes**.
* The `/cli${id}` interface is the fixed literal name
  `org.example.KwinApiClient`; it does **not** follow
  `KWIN_API_SERVICE_NAME`.
* `poll()` returns a UTF-8 string that is **always valid JSON**: a
  JSON-serialized array (normal) or a JSON-encoded string (error). The script
  parses it once — an **array** means messages (parse as `object[]`; the
  daemon splices the verbatim payload texts, `[` + payload₁ + `,` + payload₂
  + `]`), a **string** means an error message. The transport does not
  interpret payloads.
* `poll()` returns **all** queued messages in one call (FIFO batch), not just
  one.
* Oversized inbound frames are consumed and discarded **without being
  buffered**; the connection is kept.
* Outbound messages are validated **once, at `push()` time**; the socket
  write path does not re-validate.

## 5. Legacy line protocol (superseded)

The framed protocol above is now the daemon's socket protocol (phase 2). The
pre-framing line protocol (`daemon/src/socket_server.*`) is kept only for its
unit tests:

```
ping    -> pong
status  -> <status line>
quit    -> server closes the connection
<other> -> echo <other>
```

## See also

* [RPC.md](RPC.md) — application layer: JSONRPC methods and payload formats
  (TBD).
* [DEVELOP.md](DEVELOP.md) — implementation notes and the D-Bus interface
  as implemented today.
