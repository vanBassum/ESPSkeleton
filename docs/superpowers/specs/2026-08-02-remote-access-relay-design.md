# Remote Access Relay — Design

**Date:** 2026-08-02
**Status:** **Milestone 1 shipped and verified on hardware 2026-08-02** — see
*Implementation status* at the end. First milestone is deliberately
functional-only; see *Out of scope for milestone 1*.

## Goal

Reach a device's **own** web UI from outside its LAN, without a VPN and without
the server holding a copy of the frontend. A user logs in to a relay server,
sees connected devices, clicks one, and the device's UI opens — served through
the device's own command pipe, so the UI can never mismatch the firmware it is
talking to.

This is the concrete design for `docs/backlog/2026-07-03-remote-access.md`, and
it is the payoff the session-mux transport
(`2026-07-09-session-mux-transport-design.md`) was built for: the relay is a
**second `SessionLink`**, not a second protocol.

## Shape

The whole firmware change is one new transport under the existing mux:

```
  Browser (local)  ──  WsSessionLink    ┐
                                        ├─ SessionMux → AuthGate → CommandManager
  Relay server     ──  RelaySessionLink ┘
```

`SessionMux`, `Session`, `AuthGate`, `CommandManager` and **every command
handler** are untouched. Two connections, two links, one command layer.

`RelayManager`'s entire job:

- maintain the outbound WebSocket to the server (it dials out, so NAT/firewall
  traversal is free — the reason the relay is outbound in the first place),
- push received frames into a queue,
- implement `SessionLink::SendRaw()` and a **blocking** `RecvChunk()` that pops
  that queue,
- reconnect with backoff, and register the device on (re)connect.

It knows nothing about commands, files, or the frontend.

## Extracting `SessionLink`

The seam already exists: `WsSessionLink` (`SendRaw` + `RecvChunk`) is what
`Session`/`SessionMux` are written against. Making it an interface is finishing
what the transport spec designed, not new architecture.

**Keep the shipped signatures, not the ones sketched in the 2026-07-09 spec.**
That spec proposed `SendChunk(session, flags, payload, len)`; the shipped code
has `SendRaw(frame, len)` because `Session` assembles the 3-byte header and the
payload directly into an *external* buffer, so a flush is one send with **no
extra copy**. A `SendChunk` signature would reintroduce that copy on every
chunk, including every firmware-image chunk. So:

```cpp
class SessionLink {
public:
    virtual ~SessionLink() = default;
    // `frame` is [session|flags|payload]; `len` is header + payload.
    virtual bool SendRaw(const uint8_t* frame, size_t len) = 0;
    // Next inbound chunk into `buf`; returns payload length, or -1 = end of stream.
    virtual int  RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) = 0;
};
```

`WsSessionLink` gains `: public SessionLink` and `override`. Behaviour change:
none. This step is verifiable on its own — flash it, use the UI locally,
nothing should differ.

### Why `RecvChunk()` must be queue-backed on the relay

> **Superseded 2026-08-05 — it must not be.** Everything below is a correct
> description of what `esp_websocket_client` forces, and the conclusion drawn from it
> was that the relay is simply the transport that cannot read. It isn't: one layer
> below that client, the same handshake and framing are an ordinary blocking read, so
> the relay task now reads the socket *and* runs the command, like the browser socket
> does. `RelaySocket` is that read; `RelaySessionLink::RecvChunk` is four lines on top
> of it. The queue, the per-frame `malloc`, the reassembly buffer, and the disconnect
> sentinel are all gone — with them went the dropped-chunk failure mode that made the
> queue depth a correctness knob. See
> `docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md`.
>
> What survives from this section: a command handler still must not run on a task it
> depends on for input — that is exactly why the reading moved to *our* task rather
> than the handler moving to theirs.

This is the one place the two transports genuinely differ, and it is easy to
discover the hard way at the end of the project instead of the start.

`Session::read()` **pulls**: when the current chunk is drained it calls
`link.RecvChunk()` and expects to block until the next chunk arrives. Over
httpd that works because frames can be read synchronously off the socket (via
the private `httpd_ws_get_frame_type` wart). `esp_websocket_client` is
**event-callback driven** — it hands frames to a callback on its own task and
there is nothing to synchronously read.

So `RelaySessionLink::RecvChunk()` blocks on a queue that the WS event callback
fills. Consequences to design for, not discover:

- **Frame reassembly belongs in the callback.** `esp_websocket_client` can
  deliver a large frame as several `WEBSOCKET_EVENT_DATA` callbacks
  (`payload_offset` / `payload_len` / `data_len`); a chunk must be whole before
  it is queued, or `RecvChunk` hands the mux a truncated header.
- **A command handler must not run on the WS client task.** It would deadlock:
  the handler blocks in `read()` waiting for the queue that only that same task
  can fill. `RelayManager` needs its own task that owns the mux and runs
  handlers, sized for the heaviest handler (the "stack for the worst command"
  tax — see `docs/backlog/2026-08-03-command-worker-task.md`).
- **Disconnect must unblock a waiting reader.** On `WEBSOCKET_EVENT_DISCONNECTED`
  queue a sentinel so `RecvChunk` returns -1 and in-flight sessions EOF, rather
  than hanging until `portMAX_DELAY`.

## `getWebFile` — an ordinary command

Not a new mechanism. `WebServerManager` gains its first `commands_[]` table and
registers it in `Init()`. The server then asks for a file exactly the way the
browser asks for anything: open a session, write a header line, read the reply.

Request is the usual header line; reply is a status/header line, then the raw
bytes as the session body:

```
→ {"type":"getWebFile","path":"/assets/index-abc123.js"}\n
← {"ok":true,"status":200,"contentType":"application/javascript","contentEncoding":"gzip"}\n<bytes…>
```

`contentEncoding` is **not optional cosmetics**: files are stored gzipped on the
FAT partition (`*.gz`), so the reply must say so or the browser receives gzip
bytes labelled as JavaScript. This is why the `httpGet`-flavoured reply won the
backlog's `readFile`-vs-`httpGet` fork — the alternative is reimplementing
gzip/MIME decisions in C# and letting them drift from the device.

### Reuse, don't duplicate — and where the SPA fallback lives

`StaticFileHandler::Handle()` already does path→file resolution: `.gz`
preference, MIME by extension, `..` rejection, `/` → `/index.html`. Extract that
into a reusable resolver on `StaticFileHandler`:

```cpp
struct Resolved { char path[600]; const char* contentType; bool gzipped; };
static bool Resolve(const char* basePath, const char* uri, Resolved& out);
```

Both the local HTTP route and `getWebFile` call it. `BASE_PATH` ("/www") is
already a `WebServerManager.cpp` constant; share it rather than passing it twice.

**Deliberate split:** `Resolve()` does *not* do SPA fallback — it returns
not-found for a missing path. Fallback (serve `index.html` for a non-file route)
stays in each route layer: `StaticFileHandler::Handle` keeps it for local HTTP,
and ASP.NET does its own. This preserves the backlog guardrail ("HTTP decisions
stay in the route layer; the command stays *give me bytes*") and, more
practically, avoids the failure mode where a mistyped `.js` path returns
`index.html` with status 200 and the browser reports a confusing MIME error. The
device still owns *where files live and how paths map to them* — which is the
whole architectural point; it just doesn't own the proxy's 404 policy.

## The relay is payload-opaque, not frame-opaque

An amendment to the 2026-07-09 spec's line that "a relay only has to forward one
socket per device, understanding nothing". Not quite: the server must read and
rewrite the 3-byte session header. It must **not** parse the payload.

```
[ session : u16 LE ][ flags : u8 ][ ── opaque to the server ── ]
```

### Session-id ownership (the correctness requirement)

Both the browser and the server's own file fetches would allocate session ids
from 1 on the *same* device socket. They collide, and it presents as "the device
replied to the wrong request".

**The server owns the id space on the device pipe.** It allocates its own ids for
`getWebFile`, and *rewrites* browser-side ids into ids of its own, keeping a
per-connection map. Two things fall out for free: multiple simultaneous browser
sessions stop being a special case, and demuxing device→server traffic is
well-defined.

### Session 0 must be forwarded

Session 0 is reserved for device-initiated broadcasts (log lines). The relay is
therefore **not** request/response. Routing of device→server chunks:

| session id                  | goes to                        |
| --------------------------- | ------------------------------ |
| 0                           | broadcast to attached browsers |
| server-allocated (file)     | `DeviceFileProxy`              |
| mapped from a browser id    | that browser, id rewritten back |
| unknown                     | drop + log                     |

## Request serialization (also a correctness requirement)

Today's `SessionMux` is step 1: one chunk = one complete request, dispatched
**synchronously**, single-in-flight, no slot table. A browser page load fires
5–20 asset requests at once plus command traffic. Over one relay socket those
cannot overlap.

So the server must keep **one request in flight per device** — a FIFO queue,
with generous timeouts. This mirrors the client-side open-serialization the
transport spec already requires of `backend.ts`. Skipping it does not produce a
clean error; it produces `REJECT`s and phantom device bugs.

Known cost, accepted: an uncached page load is N *sequential* round trips over
the WAN. Mitigations, in order of when they should happen:

1. Nothing (milestone 1). Measure it.
2. A per-device `(deviceId, firmwareVersion, path)` in-memory cache — small, and
   it takes every load after the first to zero device round trips. Have devices
   report firmware version at registration so the key exists from day one even
   though the cache does not.
3. ~~The real fix: the worker task + slot table.~~ **Rejected 2026-08-03** — see
   `docs/reasoning/2026-08-03-12h30-addressing-replaces-concurrency.md`. One request
   in flight per device is permanent, so caching (point 2) *is* the fix for page-load
   latency, not a stopgap before concurrency.

## Frontend changes (smaller than expected)

The frontend has **no router** — it is a single page with local state. So:

- **Assets:** `base: "./"` in `frontend/vite.config.ts`. Relative asset URLs
  resolve correctly under `/devices/{id}/` and are harmless locally. No
  `<base>` injection, no build-time prefix, no per-device builds. (If a router
  with real paths is ever added, revisit — relative bases break on deep routes.)
- **WebSocket URL:** one place, `backend.ts` (`const ws = new WebSocket(...)`).
  Replace the hardcoded `/ws` with a `resolveWsUrl()` covering three cases: dev
  proxy (`import.meta.env.DEV` → `DEV_HOST`), local device (`/ws`), relayed
  (`ws` resolved against `location.pathname`). Deriving from `location` rather
  than parsing a device id keeps the frontend ignorant that a relay exists.

This must land **before** the file proxy: the moment ASP.NET serves `index.html`
under `/devices/{id}/`, absolute `/assets/...` URLs 404 and the proxy is
untestable.

## ASP.NET Core server

Components (from the original plan, unchanged — they were right):

```
DeviceConnectionManager        connected-device registry (in-memory)
DeviceConnection              one device socket: id space, session map, FIFO queue
DeviceFileProxy               getWebFile requests
DeviceFrontendEndpoint        /devices/{deviceId}/{**path}
DeviceBrowserWebSocketEndpoint /devices/{deviceId}/ws
DeviceDashboardApi            connected-device list
```

Route precedence: `/devices/{id}/ws` must match before the `{**path}` catch-all.

Dashboard: a separate React app is the right target, but milestone 1 does not
need a second build pipeline — one static page fetching `/api/devices` is enough.

## Auth: a freebie and a gap

**Freebie:** the device already has an in-band handshake (`hello`/`login`/`auth`
in `AuthGate`, gated on the `web.password` setting). Relay the bytes and device
login works remotely with zero new code — handlers stay oblivious, exactly as
the backlog predicted.

**Gap:** device→server registration has no credential, so any device can claim
any `deviceId`, and anyone who reaches the server reaches any device whose
`web.password` is unset. Acceptable on a dev box or LAN. **Do not put milestone 1
on a public IP.** The device-credential-to-server half is the one deferred item
that is a security boundary rather than a nicety.

## RAM budget (check before writing `RelayManager`)

`WebSocketHandler` holds its framing buffers as *members*: `sessionFrame_`
(515 B) + `sessionInbound_` (4099 B). The relay needs its own pair, plus the
inbound frame queue, plus a task stack sized for the heaviest command handler,
plus mbedTLS once WSS lands (~40 KB+). On 320 KB of RAM this is worth a number
up front, not a surprise at link time.

## Implementation order

1. **Extract `SessionLink`** — interface + `WsSessionLink : SessionLink`. No
   behaviour change; flash and confirm the local UI is unaffected.
2. **`getWebFile` command** — extract `StaticFileHandler::Resolve()`, add
   `WebServerManager`'s first `commands_[]`. Testable from the existing browser
   WS before any server exists.
3. **ASP.NET registry + minimal dashboard** — accept device sockets, list them.
4. **`RelayManager`** — WS client, queue-backed `RecvChunk`, own task, reconnect,
   register. Device appears in the dashboard.
5. **Frontend** — `base: "./"` + `resolveWsUrl()`.
6. **File proxy** — `/devices/{id}/{**path}`, server-owned session ids,
   one request in flight per device.
7. **Browser relay** — `/devices/{id}/ws`, id rewriting, session-0 forwarding.
8. **Test** (below).

## Testing

No automated tests (repo convention): build, flash, drive the real flow. Beyond
the happy path, milestone 1 is not done until these behave:

- Device reconnects after the server restarts, and after Wi-Fi drops.
- Device disconnects **mid-page-load** — in-flight `getWebFile` sessions fail
  cleanly, the browser gets an error, nothing hangs on `portMAX_DELAY`.
- Clicking an **offline** device 503s promptly instead of hanging.
- Normal command traffic while the page is loading (the serialization path).
- Log lines still stream to a relayed browser (session 0).
- **Direct local access still works**, unchanged, including the login handshake.

## Out of scope for milestone 1

HTTPS/WSS, server-side user accounts, database persistence, file caching, file
hashes/versions beyond the registration-time firmware version, session
resumption, production deployment, whole-FAT-partition download, and any server
knowledge of the device's filesystem layout.

Caching is explicitly deferred **as an optimization**: session-id ownership and
request serialization are correctness requirements and ship in milestone 1;
caching ships immediately after if the page load is unpleasant.

## Implementation status

**Milestone 1 shipped 2026-08-02**, branch `remote-access-step1`, verified against
real hardware (ESP32 DevKit, fw 0.0.5, relay on a LAN host). All eight steps of
the order above landed. Six refinements emerged while building it:

- **The relay server is Python, not ASP.NET Core** (Bas's call: prove the path
  first). `relay-server/relay.py` — aiohttp, ~450 lines, in-memory registry, an
  inline HTML dashboard instead of a second React app. The component split from
  the plan survived as classes/handlers in the one file. Nothing about the device
  side assumes the server's language.
- **Registration is the connect URL's query string**, not a protocol message:
  `/device?id=<id>&fw=<version>`. The server knows who connected before the first
  chunk arrives and the session protocol gains no relay-specific verb. Device id
  defaults to `esp32-<mac>`, so a fresh device registers without being told who it
  is; `relay.deviceId` overrides.
- **`CommandSink` was extracted** (`Application/CommandManager/CommandSink.{h,cpp}`).
  Both transports need "read the type off the header line, run the handler, close
  the reply", and a second copy in `RelayManager` would have been the exact
  duplication this design exists to avoid. `WebSocketHandler` is no longer a
  `SessionMux::Sink`; it owns one.
- **`AuthGate` moved to `SessionLink&`**, so `hello`/`login`/`auth` works over the
  relay unchanged — the predicted freebie, now real. Consequence worth knowing:
  the pipe is *one* connection, so its `WsConnection` auth state is shared by
  every browser the server relays. A login by one remote user authenticates the
  pipe for all of them. Fine for a single trusted operator; per-browser auth needs
  the server to carry a client identity alongside the session id.
- **The in-flight gate is held per session, not per chunk**, with a watchdog. A
  device that dies mid-session (`reboot` never sends FINAL — the handler restarts
  before `CommandManager` closes the reply) would otherwise wedge the pipe forever.
- **`ReadHeaderLine` became `StringReader` in `lib/common`** — lifted out of
  `UpdateManager.cpp`'s anonymous namespace once `getWebFile` was a second caller,
  then generalised: reading a line off a Stream is not a command concept.

### Verified on hardware

20/20 end-to-end checks: dashboard registry; `/devices/<id>` → trailing-slash
redirect; `index.html` and both assets through `getWebFile` (413 KB bundle in
~0.85 s, byte-identical to the device-served copy, `Content-Encoding: gzip` passed
through); a missing asset is a real 404 while an unknown route falls back to
`index.html`; `info`/`ping`/`getSettings`/`partitions`/`getLogs` over the relay
(including a multi-chunk reply); session-0 log broadcasts reaching a relayed
browser; three concurrent asset fetches serialized correctly; offline device 503s
promptly on both the HTTP and WS routes.

Disconnect behaviour: a request against a pipe the device has just abandoned
returns 502 immediately (the ESP sends a clean FIN on restart) rather than hanging;
the device reconnects by itself in ~3 s via `esp_websocket_client`'s auto-reconnect,
after which the relay serves and dispatches again. The local path is unaffected
throughout — the same commands work directly against the device.

> **Names, as of 2026-08-05.** Two components named above no longer exist:
> `CommandSink` and `SessionMux` were folded into `protocol::RunCommandSession`
> (`lib/protocol/CommandEnvelope.h`) when the dispatcher stopped knowing about
> sessions. The design they describe is intact; only the filing changed. See
> `docs/reasoning/2026-08-03-11h59-*`.

## Milestone 2 — 2026-08-05: the transport, and firmware update proven

Two changes, both hardware-verified the same day, which together close the "do not
rely on remote firmware update" warning this section used to carry.

**The device reads its own socket.** `RelayManager` no longer uses
`esp_websocket_client`; it drives a WebSocket at the `esp_transport` layer
(`RelaySocket`), so the relay task connects, reads a frame, and runs the command it
read — the same shape as the browser socket. The queue this design specified, the
per-frame `malloc` that fed it, the reassembly buffer, the stale-chunk drain and the
disconnect sentinel are all gone, and with them the dropped-chunk failure mode: a
full queue used to write a hole into a firmware image, silently. Backpressure is now
the TCP window. Reasoning:
`docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md`. Reconnect,
keepalive and URL parsing moved to us with it; control frames did not (the transport
answers a ping and completes a close inside the read).

**The gate watchdog measures silence, not session length.** `SESSION_IDLE_TIMEOUT`
(15 s), re-armed by any chunk for the holder's session in either direction. There is
no cap on how long a healthy session may run; a dead one still frees the pipe.

> **Invariant, if either timeout is ever touched:** the server's idle timeout must
> stay LONGER than the device's `RECV_TIMEOUT_MS` (10 s, `RelaySessionLink`).
> Whichever fires first decides how a stalled session ends, and it has to be the
> device: it EOFs its own request, the handler writes a reply, and that reply
> releases the gate normally. Server first releases the pipe while the device still
> believes the session is open — the interleaving the gate exists to prevent.

### Verified on hardware 2026-08-05

esp32_devkit, fw 0.0.5, `relay.py` on a LAN host, driven by a script speaking the
session protocol in place of a browser.

- **One-shot firmware update, the case that used to lose two requests at once.**
  1,120,064 bytes to `ota_1`, paced to 27 KB/s so the session spanned **40.1 s**
  (twice the old fixed budget), with a page load starting at t=24.5 s. Write returned
  `{"ok":true,...,"size":1120064}`; the page load returned 200 after waiting 20.1 s
  for its turn; `activate` accepted the image — so all 1.12 MB landed byte-correct
  through the new transport and the zero-copy write — and the device booted the new
  slot (`Loaded app from partition at offset 0x1a0000`). No watchdog warning.
- **Unpaced: 5.5 s, 199 KB/s.** Run in both directions (`ota_0` ↔ `ota_1`), and a
  later build was pushed over the relay with no serial cable attached — the path
  updating itself.
- **Liveness:** three failed connects before the server existed, then self-recovery;
  100 s fully idle without losing the pipe, which also proves the pongs the transport
  owes aiohttp's 30 s heartbeat.
- **`help` over the relay**, including describe-mode re-dispatch:
  `help list -category partition -command write` returns that handler's declared
  arguments off the device.

### Still owed

- **Caching**, keyed on `(deviceId, firmware, path)` — the next thing to do if page
  loads annoy. Concurrency is not the fix; it was rejected 2026-08-03.
- **TLS / `wss://`** — never exercised. `RelaySocket` attaches the certificate
  bundle for `wss://`, and the build has it enabled, but no run has used it. The TLS
  handshake now shares the relay task's stack (10 K, sized by reasoning rather than
  measurement), so this is the one change most likely to want a headroom check.
- **The device→server credential**, and **per-browser auth on a shared pipe**.
- **A silent command longer than the idle window.** `partition clear` says nothing
  while it erases — 4.1–4.5 s for a 1.5 MB slot against 15 s. Fine today, but that
  margin is the erase time of the partition rather than a number anyone chose.

Backlog: `docs/backlog/2026-07-03-remote-access.md`.
