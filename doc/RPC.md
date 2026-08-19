# JSONRPC Methods

> **Status: TBD — placeholder.** This document is not finalized. The
> *transport* layer (framing, buffers, the daemon ↔ script `poll()`/`push()`
> interface) is frozen in [PROTOCOL.md](PROTOCOL.md) and implemented in
> phases 1–2, but the *application* layer — what the JSON payloads mean — is
> still TBD. Until it is specified, the socket carries raw JSON payloads that
> the KWin script receives via `/cli${id}` `poll()`.

## Transport

See [PROTOCOL.md](PROTOCOL.md) §2: JSONRPC 2.0 messages are carried as
length-prefixed UTF-8 JSON frames over the unix socket at
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket` (frozen, not yet
implemented).

## Planned methods (not implemented yet)

The intent is to expose KWin scripting capabilities to clients. Candidate
methods (final list TBD):

* workspace / window listing and properties (window count, captions,
  stacking order, …)
* window operations (activate, close, minimize, maximize, …)
* virtual desktop operations (switch, list, …)
* global shortcut registration / invocation
* general-purpose "run this KWin scripting snippet" (TBD)

Request/response and error shapes, plus any notifications (e.g. window added /
removed), will be specified here.
