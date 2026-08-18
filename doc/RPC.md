# JSONRPC Methods

> **Status: TBD — placeholder.** This document is not finalized. The JSONRPC
> layer on the unix socket is planned but not implemented; until it lands the
> socket speaks the interim line protocol described in
> [PROTOCOL.md](PROTOCOL.md).

## Transport

See [PROTOCOL.md](PROTOCOL.md) §1: JSONRPC 2.0 over the unix socket at
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket` (TBD).

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
