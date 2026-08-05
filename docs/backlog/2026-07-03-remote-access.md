# Remote access / relay server — what is left

Devices connect **outbound** to a server (NAT-friendly) so they are reachable
remotely. The relay is just another dumb-pipe transport into `CommandManager`, which
is why every command — including firmware update — works remotely for free. That is
the driving reason `CommandManager` is the star point of the architecture.

**Working and hardware-verified**: the device dials out, the dashboard lists it,
clicking it opens the device's *own* web UI served through the command pipe, commands
and live logs work remotely, and a firmware update over the relay has been pushed end
to end (including one pushed with no serial cable attached). Demo server:
[`relay-server/`](../../relay-server/) — Python, deliberately, to prove the path
before writing an ASP.NET one.

**Design, decision record and verified status all live in the spec:**
[`2026-08-02-remote-access-relay-design.md`](../superpowers/specs/2026-08-02-remote-access-relay-design.md).
Read that first; this file is only the open work.

## Open, in the order that will probably matter

1. **The device→server credential.** The worst gap by far: anything that guesses the
   MAC-derived device id does not impersonate the device, it **evicts** it — the
   server keys its registry on that id and the newcomer takes the slot. Until this
   exists the relay must not face a public IP. TLS, server-side login and a persistent
   registry all sit behind the same question: *does this ever face a public network,
   or is it a LAN convenience?* Answer that and the rest follows.

2. **TLS / `wss://` — untested, not unimplemented.** `RelaySocket` attaches the
   certificate bundle when the URL is `wss://`, and the build has the bundle enabled,
   but no run has ever used it. Two things to check when someone does: that the
   handshake succeeds at all, and that it fits the relay task's stack — 10 K, sized by
   reasoning rather than measurement, and now shared with the command handlers because
   one task does both. If it is tight, measure with `uxTaskGetStackHighWaterMark`
   rather than guessing again.

3. **Per-browser auth on a shared pipe.** The pipe is *one* connection, so its auth
   state is shared: a login by one remote user authenticates the pipe for every
   browser the server relays. Acceptable for a single trusted operator, wrong for
   anything else. Needs the server to carry a client identity alongside the session
   id — which is a protocol change, not a device change.

4. **Server-side file caching**, keyed on `(deviceId, firmware, path)`. An uncached
   page load is N sequential WAN round trips, because one request in flight per device
   is now permanent (multiplexing was rejected 2026-08-03 — concurrency is not worth a
   slot table and per-channel buffers on this much RAM). Caching is the fix for page
   load latency; the slow fetch-through-the-pipe then happens once per firmware
   version instead of once per view. Version-keyed so a device-served UI can never
   mismatch its firmware, which is the whole reason the UI is served from the device
   rather than copied onto the server.

5. **A legitimately silent command longer than the gate's idle window.**
   `partition clear` says nothing while it erases: 4.1–4.5 s measured for a 1.5 MB
   slot, against a 15 s window. Fine today, but the margin is the erase time of the
   largest partition rather than a number anyone chose. The fix, if a bigger partition
   ever needs it, is for the handler to report progress the way `partition write`
   already does — not a longer timeout.

## Settled, so nobody reopens them

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
