# Remote access / relay server — what is left

Devices connect **outbound** to a server (NAT-friendly) so they are reachable
remotely. The relay is just another dumb-pipe transport into `CommandManager`, which
is why every command — including firmware update — works remotely for free. That is
the driving reason `CommandManager` is the star point of the architecture.

Server: [`vanBassum/strux-relay`](https://github.com/vanBassum/strux-relay) — its own repository since 2026-09-08, and
Python deliberately, to prove the path before writing an ASP.NET one. The device side
assumes nothing about the server's language.

**Deployed step by step in
[`2026-08-05-relay-in-production.md`](2026-08-05-relay-in-production.md)** — that plan
took the device→server credential, TLS and the human side (Authentik) off this list, so
work from it rather than from this one.

## What is already proven on hardware

esp32_devkit against `relay.py` on a LAN host, 2026-08-02 and 2026-08-05:

- The device dials out, the dashboard lists it, clicking it opens the device's *own* web
  UI through the command pipe, and commands plus live log broadcasts work remotely.
- A 413 KB frontend bundle arrives byte-identical to the device-served copy, gzip
  `Content-Encoding` passed straight through. A missing asset 404s; an unknown route
  falls back to `index.html`.
- **Firmware update end to end**: 1,120,064 bytes to the idle OTA slot, `activate`
  validating the image, and boot into the new slot. 5.5 s unpaced (199 KB/s); a run
  paced to 40 s — twice the old watchdog budget — survived a page load cutting in at
  t=24.5 s, which used to kill both requests.
- One later build was pushed over the relay with no serial cable attached.
- Liveness: recovery from failed connects, and 100 s fully idle without losing the pipe.

`wss://` was proven on hardware later, against the production server. Still not proven:
anything with more than one browser or one operator.

## Open, in the order that will probably matter

1. **Per-browser auth on a shared pipe.** The pipe is *one* connection, so its auth
   state is shared: a login by one remote user authenticates the pipe for every
   browser the server relays. Acceptable for a single trusted operator, wrong for
   anything else. Needs the server to carry a client identity alongside the session
   id — which is a protocol change, not a device change.

## Settled, so nobody reopens them

- **A silent command is bounded by the gate's idle window, and today it fits.**
  `partition clear` says nothing while it erases: 4.1–4.5 s measured for a 1.5 MB slot,
  against a 15 s window. Nothing to do — but the margin is the erase time of the largest
  partition rather than a number anyone chose, so if a bigger partition ever overruns it
  the fix is for the handler to report progress the way `partition write` already does,
  not a longer timeout.

- **`getWebFile` is `httpGet`-flavoured** (status + content-type + content-encoding +
  body), not raw bytes: frontend files are stored gzipped, so raw bytes would force
  the server to re-derive `Content-Encoding` and MIME, where they drift from the
  device.
- **SPA fallback belongs to each route layer**, not to the device's `Resolve()`, so a
  mistyped asset path 404s instead of returning `index.html` with a 200 and tripping a
  browser MIME error.
- **The server is payload-opaque, not frame-opaque.** It must read and rewrite the
  3-byte session header because it owns the id space on the device pipe, and it must
  forward session 0, so the relay is not request/response.
- **WireGuard was considered and rejected** — a network-level fix for an app-level
  problem, with heavy key and routing operations.
