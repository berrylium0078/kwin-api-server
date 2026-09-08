# Firefox native messaging host (kwin-api-server/native/)

Bridges the Firefox extension's native messaging to kwin-api-server's unix
socket.

## Files

| File | Purpose |
|---|---|
| `kwin-api-host.py` | The host itself (Python 3, standard library only). Pure byte-level forwarding: Firefox ↔ socket |
| `host-manifest.json` | Manifest template; `xmake install` replaces `@HOST_PATH@` with the absolute path and installs it |
| `test_native_host.py` | End-to-end test: mocked browser + mocked daemon, no Firefox/KWin needed |

## How it works

Both frame formats are identical on Linux:

```
Firefox native messaging (stdin/stdout):
    4-byte length (uint32, native endian) + UTF-8 JSON message
kwin-api-server socket (doc/PROTOCOL.md §2.2):
    4-byte length (uint32, native endian) + UTF-8 JSON frame
```

So the host does no JSON parsing at all: frames read from stdin are written
verbatim to the socket, and frames read from the socket are written verbatim
to stdout. It exits (code 0) as soon as either side hits EOF.

Socket path: `$KWIN_API_SOCKET` (override, used by tests) or
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket`. The host is a single
self-contained script (Python standard library only) and works from any
install prefix.

## Installing via xmake

The `kwin-api-native` target in `xmake.lua` installs the host and the
manifest:

| Installed item | Path |
|---|---|
| host script | `<prefix>/libexec/kwin-api-server/kwin-api-host.py` (0755) |
| manifest (rendered) | `<prefix>/lib/firefox/native-messaging-hosts/org.plasma_tweak.kwin_api.json` |

```sh
sudo xmake install                    # installs to /usr
```

When the install prefix is `/usr`, the manifest is additionally copied into
the system directories Firefox actually scans, `/usr/lib/mozilla/native-messaging-hosts/`
and `/usr/lib64/mozilla/native-messaging-hosts/` (whichever exists), so a
single `sudo xmake install` is enough for Firefox to find the host.

Uninstall:

```sh
sudo xmake uninstall                  # or xmake uninstall --installdir=<dir>
```

### Rootless / staged install (dev)

```sh
xmake install -o ./stage              # installs into the stage/ prefix (does not touch /usr)
# Make Firefox use the staged host (paths point into stage/, keep the dir):
cp stage/lib/firefox/native-messaging-hosts/org.plasma_tweak.kwin_api.json \
   ~/.mozilla/native-messaging-hosts/
```

The manifest's `allowed_extensions` is `kwin-window-manager@plasma-tweak.local`,
which must match `browser_specific_settings.gecko.id` in
`extension/manifest.json`. If you change the id, update both places
(`host-manifest.json` and the extension manifest).

## Tests

```sh
python3 test_native_host.py
```

Covers: request → response round trip, forwarding of daemon-initiated
notifications, host exit when the daemon disconnects, and forwarding of
id-null requests (no response). The host logs to stderr; on failure the logs
are printed to help debugging.

## Manual smoke test

```sh
# Start a fake daemon (any unix-socket JSON-RPC server), then:
printf '\x00\x00\x00\x00...' | KWIN_API_SOCKET=/tmp/x.sock python3 kwin-api-host.py
# or check that an installed host can reach the socket:
python3 <prefix>/libexec/kwin-api-server/kwin-api-host.py --check
```
