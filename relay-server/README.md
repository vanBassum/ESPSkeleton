# Relay server (demo)

A ~450-line Python server that makes a Strux device reachable from outside its
LAN. Devices dial **out** to it, so there is no port forward and no VPN.

```
device  ──ws──►  /device?id=<id>&fw=<ver>     outbound, NAT-friendly
browser ──http─►  /devices/<id>/{path}        → getWebFile command on the device
browser ──ws──►  /devices/<id>/ws             → relayed onto the device pipe
```

This is the proof-of-concept transport for
[`docs/backlog/2026-07-03-remote-access.md`](../docs/backlog/2026-07-03-remote-access.md), and the
deployment plan in [`docs/backlog/2026-08-05-relay-in-production.md`](../docs/backlog/2026-08-05-relay-in-production.md).
It is not production software — see *Not implemented* below.

## Run it

```bash
cd relay-server
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements.txt   # or bin/python on POSIX
.venv/Scripts/python relay.py --port 8080
```

Open <http://localhost:8080> for the dashboard.

On Windows, inbound connections to a fresh port are blocked by default, so the
device's connect attempt will time out with
`transport_ws: Error connecting to host`. Allow the port once (elevated):

```powershell
New-NetFirewallRule -DisplayName "Strux relay" -Direction Inbound `
  -Protocol TCP -LocalPort 8080 -Action Allow -Profile Private
```

## Point a device at it

Set two settings on the device (settings UI, or `setSetting` + `saveSettings`
over its WebSocket) and reboot:

| setting          | value                              |
| ---------------- | ---------------------------------- |
| `relay.url`      | `ws://<server-ip>:8080/device`     |
| `relay.enabled`  | `true`                             |
| `relay.deviceId` | optional — defaults to `esp32-<mac>` |

The device then appears on the dashboard; click it to open its own UI.

## How it stays dumb

The server **never parses a session payload**. Commands, the auth handshake,
uploads and log lines are opaque bytes moved between two sockets. It does parse
and rewrite the 3-byte session header, because it must own the session-id space:

* Browser ids and the server's own `getWebFile` ids both start at 1 on the same
  device socket and would collide, presenting as "the device replied to the wrong
  request". So browser ids are **rewritten** into the low half (1–0x7FFF) and the
  server's own sessions come from the high half (0x8000+).
* Session 0 is the device's log broadcast, so the pipe is **not**
  request/response — session-0 chunks are forwarded to every attached browser.
* One request is in flight per device (`DeviceConnection.gate`), held for a whole
  session. The device dispatches a chunk synchronously with no slot table, so
  overlapping sessions would let a file fetch's chunk land inside a streamed
  request body. A watchdog releases the gate after `SESSION_IDLE_TIMEOUT` of
  **silence** for that session — not after a fixed session length, which used to take
  the pipe away from a healthy multi-second upload and let a page load interleave into
  its body, killing both. Every chunk in either direction re-arms it, so a transfer of
  any duration is fine while a dead device still frees the pipe. It is longer than the
  device's own receive timeout on purpose; see the note next to the constant.

The device owns its frontend storage: the server asks for `/index.html` and never
learns it lives gzipped on a FAT partition called `www`. `Content-Encoding` comes
back from the device, so gzip passes straight through.

SPA fallback is the **server's** decision, not the device's — the device returns a
real 404, and the server only falls back to `index.html` for paths that don't look
like files. That keeps a mistyped asset a 404 instead of HTML with status 200,
which a browser rejects as a MIME error.

## Not implemented

No TLS/WSS, no device→server credential (any device may claim any id), no user
accounts, no persistence, and **no file caching** — every HTTP request is one
`getWebFile` round trip. Since requests are serialized per device, an uncached
page load is N sequential round trips; on a LAN the 413 KB bundle takes ~0.85 s.
Caching keyed on `(deviceId, firmware, path)` is the next step if that hurts.

**Do not expose this to the internet.** Anyone who reaches it reaches every
connected device, limited only by the device's own `web.password`.
