# JSON-RPC Application Layer

> **Status: window claiming (tokens), window property methods and workspace
> property methods (update/query/watch) implemented.** This
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
  `id`. A request without an `id` is a notification (the method runs, no
  response is sent); a request whose `id` is `null` is treated the same way —
  the method still runs, but the reply is suppressed. In batches, the
  id-`null` replies are dropped individually.
* Some methods take their params **by position** (JSON-RPC by-position:
  `params` is an array following the signature's parameter order) — the
  window-property methods in §3 and the workspace-property methods in §4 do;
  `token.request` (a single object param)
  is by-name. This is stated per method.
* Errors follow the JSON-RPC 2.0 error object shape:

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

From this moment the token refers to that window (for the property methods of
§3 and future ones). The `window` payload is informational only — the client
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

## 3. Window properties (update / query / watch)

The token protocol (§2) gives a client a stable reference to one of its
windows. The three methods below operate on the window a validated token is
bound to. All of them take their params **by position** (JSON-RPC by-position:
`params` is an array in signature order), and a request whose `id` is `null`
is treated as a notification: the method runs, but no response is sent (§1).

Property names are validated against an immutable supported-property catalog
(three `Set`s in `kwinscript/src/windows.ts`); an unsupported name makes the
whole call fail with `-32602` **before anything is applied or any listener is
changed**. The catalog covers the writable properties of JS primitive type
(boolean / number) plus `desktops` / `activities`, which are passed as **ID
sets** (`string[]`, §3.5); it is meant to be extended.

| Set | used by | contents |
|---|---|---|
| `UPDATE_PROPS` | `windows.update` | writable, JS-primitive-typed properties + `desktops` / `activities` (ID sets) |
| `QUERY_PROPS` | `windows.query` | same catalog for now (a separate Set so the two methods stay independently extensible) |
| `WATCH_PROPS` | `window.watch` | writable properties with a `<prop>Changed` signal (booleans + `desktops` / `activities`) |

Supported properties:

| property | type | update / query | watch |
|---|---|---|---|
| `opacity` | number | ✓ | – (not boolean) |
| `skipsCloseAnimation` | boolean | ✓ | ✓ |
| `fullScreen` | boolean | ✓ | ✓ |
| `onAllDesktops` | boolean | ✓ | – (no `onAllDesktopsChanged`) |
| `skipTaskbar` | boolean | ✓ | ✓ |
| `skipPager` | boolean | ✓ | ✓ |
| `skipSwitcher` | boolean | ✓ | ✓ |
| `keepAbove` | boolean | ✓ | ✓ |
| `keepBelow` | boolean | ✓ | ✓ |
| `minimized` | boolean | ✓ | ✓ |
| `demandsAttention` | boolean | ✓ | ✓ |
| `noBorder` | boolean | ✓ | – (no `noBorderChanged`) |
| `excludeFromCapture` | boolean | ✓ | ✓ |
| `desktops` | string[] of desktop IDs | ✓ | ✓ |
| `activities` | string[] of activity IDs | ✓ | ✓ |

All three methods share the same token-resolution errors: an **unknown**
token, a token **owned by another client** (the token is the ownership proof —
knowing it must not grant control) or a token that is **not yet validated**
(`pending`, no bound window) is rejected with `-32602`.

### 3.1 Method: `windows.update`

Modify window properties. Request (by-position):

```jsonc
{ "jsonrpc": "2.0", "id": 5, "method": "windows.update",
  "params": ["a1b2C3d4E5f6G7h8I9jK", { "onAllDesktops": true }] }
```

| param | type | description |
|---|---|---|
| `token` | string | a validated token (§2) the client owns |
| `updateInfo` | object | property name → new value (e.g. `{"onAllDesktops": true}`) |

Response: `{ "jsonrpc": "2.0", "id": 5, "result": null }` (void).

* Every property name in `updateInfo` must be in `UPDATE_PROPS`; otherwise the
  whole call fails with `-32602` and **no property is applied** (the names are
  validated before any assignment).
* `desktops` / `activities` values must be ID sets whose IDs exist in the
  server's workspace catalogs (§3.5); a single unknown ID fails the whole call
  with `-32602` **before** the window is touched.
* Other values are assigned to the window exactly as a KWin script would
  (`window.onAllDesktops = true`); the server does not interpret them.

### 3.2 Method: `windows.query`

Read window properties. Request (by-position):

```jsonc
{ "jsonrpc": "2.0", "id": 6, "method": "windows.query",
  "params": ["a1b2C3d4E5f6G7h8I9jK", ["onAllDesktops", "opacity", "onAllDesktops"]] }
```

| param | type | description |
|---|---|---|
| `token` | string | a validated token (§2) the client owns |
| `queryInfo` | string[] | property names to read |

Response:

```jsonc
{ "jsonrpc": "2.0", "id": 6,
  "result": { "onAllDesktops": true, "opacity": 1 } }
```

* Every name in `queryInfo` must be in `QUERY_PROPS`; otherwise → `-32602`.
* `queryInfo` is **deduplicated**: each asked property appears exactly once in
  the result (first occurrence wins).

### 3.3 Method: `window.watch`

Set up or remove property listeners. Request (by-position):

```jsonc
{ "jsonrpc": "2.0", "id": 7, "method": "window.watch",
  "params": ["a1b2C3d4E5f6G7h8I9jK", { "skipTaskbar": true, "keepAbove": false }] }
```

| param | type | description |
|---|---|---|
| `token` | string | a validated token (§2) the client owns |
| `watchInfo` | object | property name → `true` to set up a listener, `false` to remove it |

Response: an object mapping each asked property to whether a listener is
connected **after** the operation:

```jsonc
{ "jsonrpc": "2.0", "id": 7,
  "result": { "skipTaskbar": true, "keepAbove": false } }
```

* Every name in `watchInfo` must be in `WATCH_PROPS` (a boolean writable
  property with a `<prop>Changed` signal) and every value must be a boolean;
  otherwise → `-32602` (the boolean check is part of the params schema, so a
  non-boolean value never reaches the handler).
* Setting / removing a listener is **idempotent per property**: watching a
  watched property keeps exactly one listener (a second `watch(token, {p:
  true})` does not double-notify), unwatching an unwatched property is a
  no-op.
* While a listener is connected, every change of the property emits the
  `window.changed` notification (§3.4) to the client that set the listener up.
* When the token is invalidated (window closed, superseded, ambiguous, …) or
  its client disconnects, all listeners of that token are torn down: the
  client no longer owns the window, so it stops receiving `window.changed`
  for it.

### 3.4 Notification: `window.changed`

Sent to a client that watched a property whenever that property's value
changed (the KWin `<prop>Changed` signal fired):

```jsonc
{ "jsonrpc": "2.0", "method": "window.changed",
  "params": { "token": "a1b2C3d4E5f6G7h8I9jK",
              "property": "skipTaskbar", "value": true } }
```

* `value` is the property's new value, read from the window when the signal
  fired. For `desktops` / `activities` it is the new **ID set** (string[],
  §3.5) — e.g. `"value": []` when the window moved to all desktops.

### 3.5 `desktops` and `activities` (ID sets)

`desktops` and `activities` are the two non-primitive supported properties.
On the wire they are always passed and returned as **arrays of IDs**
(`string[]`), never as desktop / activity objects:

* A **desktop ID** is the desktop's KWin UUID (`VirtualDesktop::id`, e.g.
  `"d5a5076b-83d3-4d2f-b7aa-010f099d4627"`). KWin has identified desktops by
  UUID (not by number) since Plasma 5.23 and persists the IDs in `kwinrc`, so
  a desktop keeps its ID **across sessions** and a client may store it.
* An **activity ID** is the activity's UUID as managed by
  `kactivitymanagerd` — likewise stable across sessions.
* **An empty array means "all"**: `desktops: []` pins the window to all
  desktops and `activities: []` to all activities — the same convention as
  the underlying KWin properties ("if it's on all desktops/activities, the
  list is empty").

The server keeps two workspace catalogs — a `desktop ID → VirtualDesktop`
map and an activity ID set — maintained from the workspace events:
`workspace.desktopsChanged` triggers a **full rescan** of `workspace.desktops`
(desktop layout changes are rare in normal use, so rescanning is cheap and
immune to signal-payload drift), while `workspace.activityAdded` /
`workspace.activityRemoved` update the activity set with a **single
insert / delete** per event (plus an initial scan of `workspace.activities`
at script load).

`windows.update` validates the `desktops` / `activities` values against these
catalogs **before** touching the window: an ID that is not currently present
(e.g. a desktop that was removed meanwhile, or a typo) fails the whole call
with `-32602` ("unknown desktop/activity ID"), atomically — nothing is
applied. `windows.query` returns the window's current ID sets and
`window.watch` notifications carry the new ID sets.

## 4. Workspace properties (update / query / watch)

The workspace-property methods operate on the **WorkspaceWrapper** — the
global workspace state — so they take **no token**: they do not need a window
claim. Like the window-property methods (§3) they take their params **by
position** (`params` is an array), reject unsupported property names with
`-32602` before applying anything, and treat requests with `id: null` as
notifications (the method runs, no reply is sent, §1).

| Set | used by | contents |
|---|---|---|
| `WORKSPACE_UPDATE_PROPS` | `workspace.update` | `currentDesktop`, `currentActivity` |
| `WORKSPACE_QUERY_PROPS` | `workspace.query` | all four properties |
| `WORKSPACE_WATCH_PROPS` | `workspace.watch` | all four properties (each has a `<prop>Changed` signal) |

| property | type | update | query | watch |
|---|---|---|---|---|
| `currentDesktop` | string (desktop ID) | ✓ | ✓ | ✓ |
| `currentActivity` | string (activity ID) | ✓ | ✓ | ✓ |
| `desktops` | string[] (desktop IDs, all desktops) | – | ✓ | ✓ |
| `activities` | string[] (activity IDs, all activities) | – | ✓ | ✓ |

* IDs use the same wire format and UUID semantics as §3.5. `currentDesktop`
  and `currentActivity` values are validated against the same workspace
  catalogs before being applied: an unknown ID fails the whole call with
  `-32602`, atomically.
* `desktops` / `activities` are the **workspace-level lists** (all desktops /
  all activities), which is how a client discovers the IDs to use with the
  window properties of §3.5. They are read-only at the workspace level (the
  first version) — `workspace.update` rejects them with `-32602`.

### 4.1 Method: `workspace.update`

```jsonc
{ "jsonrpc": "2.0", "id": 8, "method": "workspace.update",
  "params": [{ "currentActivity": "9ef4d4ac-f299-46f4-b9b0-a712877e7d7a" }] }
```

| param | type | description |
|---|---|---|
| `updateInfo` | object | property name → new value (e.g. `{"currentActivity": "<activity UUID>"}`) |

Response: `{ "jsonrpc": "2.0", "id": 8, "result": null }` (void).

### 4.2 Method: `workspace.query`

```jsonc
{ "jsonrpc": "2.0", "id": 9, "method": "workspace.query",
  "params": [["currentDesktop", "desktops", "desktops"]] }
```

| param | type | description |
|---|---|---|
| `queryInfo` | string[] | property names to read |

Response:

```jsonc
{ "jsonrpc": "2.0", "id": 9,
  "result": { "currentDesktop": "d5a5076b-83d3-4d2f-b7aa-010f099d4627",
              "desktops": ["d5a5076b-83d3-4d2f-b7aa-010f099d4627", "…"] } }
```

* `queryInfo` is **deduplicated** like `windows.query` (§3.2).

### 4.3 Method: `workspace.watch`

```jsonc
{ "jsonrpc": "2.0", "id": 10, "method": "workspace.watch",
  "params": [{ "currentDesktop": true, "activities": false }] }
```

| param | type | description |
|---|---|---|
| `watchInfo` | object | property name → `true` to set up a listener, `false` to remove it |

Response: per-property listener state after the operation, e.g.
`{ "currentDesktop": true, "activities": false }`. Same idempotency rules as
`window.watch` (§3.3). Watches are per client and torn down when the client
disconnects.

### 4.4 Notification: `workspace.changed`

Sent to a client that watched a workspace property whenever it changed (the
`<prop>Changed` signal fired), carrying the property's **current** value:

```jsonc
{ "jsonrpc": "2.0", "method": "workspace.changed",
  "params": { "property": "currentDesktop",
              "value": "93027690-9c98-47a4-b6a4-860c46f7411a" } }
```

For `desktops` / `activities` the `value` is the new ID set (string[]).

## 5. Implementation notes (kwinscript)

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
  generation with rejection sampling, one CoarseTimer deadline per token
  (pending → `timeout`; active → end of the ambiguity window), and the shared
  `resolveTokenWindow(token, conn)` used by every token-referencing method.
* `kwinscript/src/windows.ts` — the property methods of §3: the three
  immutable supported-property Sets, the by-position zod schemas, the
  per-token watch registry (one signal slot per property, idempotent
  connect/disconnect), the `window.changed` notification, and
  `dropClientWatches()` which `tokens.ts` calls on token invalidation /
  client disconnect. It also maintains the workspace catalogs of §3.5
  (`desktopsChanged` full rescan; `activityAdded` / `activityRemoved`
  incremental insert / delete) — exported via `lookupDesktop()` /
  `hasActivity()` — and validates `desktops` / `activities`
  update values against them before applying anything. Requests with
  `id: null` are answered silently (the reply is suppressed in `jsonrpc.ts`
  `dispatchMessage`).
* `kwinscript/src/workspace.ts` — the workspace-property methods of §4:
  `workspace.update` / `workspace.query` / `workspace.watch` with their
  immutable property Sets, the per-client watch registry (global workspace
  signals), the `workspace.changed` notification, and
  `dropClientWorkspaceWatches()` which `clients.ts` calls on disconnect. ID
  values are validated against the catalogs from `windows.ts`.
* The daemon itself is **transport-only**: it forwards frames verbatim and
  never interprets JSON-RPC payloads.

## 6. Planned methods

The token protocol, the window property methods (§3, including the
`desktops` / `activities` ID sets) and the workspace property methods (§4)
are in place; future window methods will keep taking a validated `token` as
their window reference.
Candidates: window operations (activate, close, geometry move/resize),
workspace operations beyond properties (create/remove desktops), and
composite subscriptions. Not yet specified.
