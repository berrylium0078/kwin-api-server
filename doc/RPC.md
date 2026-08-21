# JSON-RPC Application Layer

> **Status: first method implemented — window claiming (tokens).** This
> document specifies the *application* layer on top of the frozen transport
> ([PROTOCOL.md](PROTOCOL.md)): the JSON-RPC 2.0 methods, notifications and
> payload formats exchanged between clients and the KWin script through the
> daemon's unix socket. The transport itself is opaque to the daemon: client
> frames arrive verbatim at the script via `/cli${id}` `poll()`, script
> replies/notifications go back via `push()` (PROTOCOL.md §3).

## 1. Transport recap

* Every JSON-RPC message is a JSON object (or array — batches) carried as a
  length-prefixed UTF-8 frame over
  `$XDG_RUNTIME_DIR/kwin-api-server/service.socket` (PROTOCOL.md §2).
* Requests from a client are dispatched by the script's shared JSON-RPC
  server; responses and server-initiated **notifications** are pushed back
  over the same connection.
* One request may be answered by at most one response; notifications carry no
  `id`. Errors follow the JSON-RPC 2.0 error object shape:

  ```jsonc
  { "jsonrpc": "2.0", "id": 3,
    "error": { "code": -32602, "message": "invalid params: timeout: ...", "data": [...] } }
  ```

  | code  | meaning            | when |
  |-------|--------------------|------|
  | -32700| Parse error        | unparseable frame payload |
  | -32600| Invalid Request   | not a valid JSON-RPC message |
  | -32601| Method not found  | unknown `method` |
  | -32602| Invalid params    | params failed the method's zod schema |

## 2. Window tokens (window claiming)

### 2.1 Protocol intent

A client claims one of **its own** windows by proving ownership through the
window title: the server hands out an opaque **token**, the client sets its
window's title so that it starts with the token, and the server — which
continuously watches window create / destroy / caption-change events — detects
the prefix and confirms the claim.

* **The rename is the permission proof.** Only the application that owns a
  window can change its title (even a KWin script cannot — `captionNormal` is
  read-only), so a window whose caption carries the client's token is the
  client's own window. The server never assumes anything else about the
  client's identity.
* **Each token is unique and owned by exactly one client; each window is bound
  to at most one token.**
* **Low latency via aggressive matching.** Validation happens on the *first*
  caption match — no polling, no handshake round-trip beyond the title change
  itself. Each token owns a single-shot **CoarseTimer** (Qt coalesces nearby
  deadlines) that defines the validate window: while it is running, a rare
  name collision (a second, *different* window matching the same token)
  withdraws the claim with `token.invalidated` (`ambiguous`); once it fires,
  a still-pending token fails validation (`timeout`) and later caption
  matches are ignored. Clients must handle `token.invalidated` at any time
  during the validate window.
* After validation the client may freely rename the window (the token prefix
  is ugly); the server makes **no assumptions** about post-validation
  captions — the binding is by window identity (`internalId`), not by caption.

### 2.2 Token lifecycle

```
 client                                server
   │  token.request {timeout}            │
   ├──────────────────────────────────────►  token created (pending);
   │                                      │  its CoarseTimer deadline armed
   │  (client sets its window title       │
   │   to <token> + anything)             │
   │                                      │  watches window events:
   │  ◄── notification token.validated ───┤  first caption match → bound
   │                                      │  (ambiguity still possible while
   │                                      │   the deadline timer runs)
   │  (client may rename the window now;  │  (no further caption assumptions)
   │   token keeps referring to it)       │
   │  ◄── notification token.invalidated ─┤  on window close / supersession /
   │       {reason}                       │  a second window matching before
   │                                      │  the deadline (ambiguous) /
   │                                      │  deadline without validation
   │                                      │  (timeout)
```

States: `pending` (created, awaiting validation) → `active` (bound to a
window) → gone. A token leaves the registry exactly once, always accompanied
by a `token.invalidated` notification (except when its owner already
disconnected — there is nobody left to notify).

### 2.3 Method: `token.request`

Request:

```jsonc
{ "jsonrpc": "2.0", "id": 1, "method": "token.request",
  "params": { "timeout": 10000 } }
```

| param     | type   | description |
|-----------|--------|-------------|
| `timeout` | number | validate-phase timeout in milliseconds, `1 .. 3_600_000`. The token stays pending for at most this long. |

Response:

```jsonc
{ "jsonrpc": "2.0", "id": 1, "result": { "token": "a1b2C3d4E5f6G7h8I9jK" } }
```

* `token` is a **20-character string over the base64 alphabet**
  (`A–Z a–z 0–9 + /`, 120 bits of entropy), generated with `Math.random()`
  and **rejection-sampled against the tokens in use** so duplicates are
  skipped. Every token has the same length — this is what lets the server
  turn a caption change into a single hash lookup
  (`caption.substring(0, 20)`).
* Invalid params (missing / non-integer / out-of-range `timeout`) are rejected
  with `-32602`.

### 2.4 Notification: `token.validated`

Sent to the requesting client when exactly one window's caption starts with
the token (a prefix of exactly `token.length` characters, i.e. `caption ===
token` or `caption.startsWith(token)`):

```jsonc
{ "jsonrpc": "2.0", "method": "token.validated",
  "params": {
    "token": "a1b2C3d4E5f6G7h8I9jK",
    "window": {
      "caption": "a1b2C3d4E5f6G7h8I9jK-my-title"  // captionNormal at validation time
    }
  } }
```

From this moment the token refers to that window (for future methods and
event subscriptions). The `window` payload is informational only — the client
already knows which window it titled, so the caption confirms the match.

### 2.5 Notification: `token.invalidated`

Sent when the token stops being usable. `reason` is one of:

| reason          | condition |
|-----------------|-----------|
| `"timeout"`     | the token never validated within its `timeout` window (validate failed) |
| `"window_closed"` | the bound window was destroyed |
| `"ambiguous"`   | two *different* windows matched the token **before the validate deadline** (the validation is ambiguous — withdrawn) |
| `"superseded"`  | another token validated against the same window; the old token is withdrawn (the old owner may be a *different* client) |

```jsonc
{ "jsonrpc": "2.0", "method": "token.invalidated",
  "params": { "token": "a1b2C3d4E5f6G7h8I9jK", "reason": "window_closed",
              "window": { "caption": "…" } } }   // bound window, when there is one
```

Semantics worth noting:

* **`ambiguous`** — ambiguity is defined **within the validate window**: a
  token is validated aggressively on its first caption match, and if a
  *second, different* window matches the same token **before the validate
  deadline** (the token's CoarseTimer is still running), ownership is no
  longer unambiguous and the claim is withdrawn — even if validation had
  already completed moments earlier. After the deadline has passed, later
  caption matches are **ignored** (the server makes no assumptions about
  post-deadline captions). In a well-behaved session this is very unlikely,
  e.g. two windows racing to be named with the same random token, but clients
  should be prepared to handle it.
* **`superseded`** — a window can only be claimed once. When a (new) token
  validates against a window that already carries an active token, the old
  token is invalidated first — whoever owns it (possibly another client) gets
  the notification.
* The `window` field is present for `window_closed` / `superseded` (and for
  `ambiguous` when the token was already active); it is `null` for `timeout`
  and for `ambiguous` while still pending.

### 2.6 Window identity

Internally the script binds each active token to its window via KWin's
`internalId` (a UUID that is stable for the window's lifetime) — that is why
the client may freely rename the window after validation without the binding
being affected. Notifications do **not** carry the `internalId`: the client
already knows which of its windows it titled with the token, so the `window`
payload only echoes the caption at the time of the event (informational; it
may no longer start with the token after a free rename).

## 3. Implementation notes (kwinscript)

* `kwinscript/src/jsonrpc.ts` — the shared `JSONRPCServer` (json-rpc-2.0) plus
  `registerMethod(name, zodSchema, handler)`: params are validated with zod;
  failures become proper `-32602` responses. Feature modules import the server
  and register their methods.
* `kwinscript/src/clients.ts` — the daemon control loop (`/daemon` `poll()` →
  client connect / disconnect) and one poll loop per client (`/cli{id}`
  `poll()` → dispatch → `push()` the response). Only one `poll()` is pending
  per D-Bus object, as the transport requires.
* `kwinscript/src/tokens.ts` — the token registry, the window-event wiring
  (`windowAdded` / `windowRemoved` / `captionNormalChanged`), base64 token
  generation with rejection sampling, and one CoarseTimer deadline per token
  (pending → `timeout`; active → end of the ambiguity window).
* The daemon itself is **transport-only**: it forwards frames verbatim and
  never interprets JSON-RPC payloads.

## 4. Planned methods

The token protocol is the foundation; future methods will take a validated
`token` as their window reference (property get/set, event subscriptions,
window operations). Not yet specified.
