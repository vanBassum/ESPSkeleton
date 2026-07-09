# Session-Multiplexed Stream Transport — Design

**Date:** 2026-07-09
**Status:** Architecture validated in discussion (this doc records it). Implementation is incremental — see *Migration*. Concurrency is deferred; framing is not.

## Goal

Every interaction with the device is **one shape**: a *session* — a byte stream
over a connection. A request is a session the initiator opens; the reply flows
back on the same session. `getLogs`, `login`, firmware upload, partition
download, and a raw transport bridge are all the same shape, differing only in
how many bytes flow. There is no HTTP command path, no per-command
begin/write/end session, and no "is this a streaming command?" flag — a small
command is just a session whose body happens to be short.

This supersedes the transport half of `2026-07-03-stream-commands-design.md`
(the WS 4 KB reply buffer, the `POST /api/command` split, and the
begin/write/end update session all fall away). The command *contract* from that
design — `void Handler(Stream& in, Stream& out)`, streams-are-the-contract,
JSON-is-a-dialect — is unchanged and is exactly what this builds on.

## Layers

Three layers, each with one job. Higher layers never see the lower ones'
concepts.

```
  CommandManager / bridge / relay     command layer — consumes a Session (a Stream)
        │  Sink / OpenSession()
  SessionMux                          generic: sessions, OPEN/DATA/CLOSE, busy-refuse   ← transport-agnostic
        │  SessionLink
  WsSessionLink / UartSessionLink     framing + integrity + send/recv                  ← transport-specific
        │
  WebSocket / UART
```

The app reads and writes a plain `Session` and never sees a chunk, a frame, or
which transport is underneath.

### `Session` — the stream the app holds (`Session : public Stream`)

A session is **full-duplex**, one direction each way:

- `read()`  = the inbound direction — the *request* on the device side, the
  *reply* on the initiator side. Returns 0 once the peer has closed its
  direction (EOF).
- `write()` = the outbound direction — the reply on the device side.

So a command handler is literally `handler(in = session, out = session)`.
Because it's just a `Stream`, *anything* that consumes a `Stream` consumes a
session — which is what makes the bridge below fall out for free.

### `SessionMux` — generic multiplexer (transport-agnostic)

Owns the sessions on one connection. Its whole surface:

- **`OpenSession()` → `Session*`** — initiator side. Sends `Open`; returns the
  session, or `nullptr` if the peer refused (`Reject` / busy).
- **`Sink::OnSessionOpened(Session&)`** — responder side. A new inbound session
  was accepted; the layer above takes it from here. On the device that's
  CommandManager: read `type` off the front, run `handler(session, session)`.
- **`OnChunk(session, kind, payload, len)`** — fed by the link once it has
  deframed one inbound chunk. Routes `Open`/`Data`/`Close`/`Reject`.
- **`TryOpen()`** (internal) — the **busy gate**. Returns `nullptr` when a
  session can't be allocated; the caller then sends `Reject`. Single active
  session today → non-null only when idle. A slot table here later is the whole
  of "concurrency", with **no change on the wire**.

The mux is **symmetric**: either end can initiate (`OpenSession`) or accept
(`Sink`). Frontend opens → device accepts (command case). Device opens a UART
session → sub-device accepts (bridge case). Same class, both roles.

### `SessionLink` — the transport seam (the only per-transport code)

```cpp
class SessionLink {
    virtual bool SendChunk(uint16_t session, ChunkKind kind,
                           const void* payload, size_t len) = 0;
};
```

Turns a chunk into wire bytes and hands inbound wire bytes back to the mux as
chunks. This is the *only* place a transport's nature shows:

- **`WsSessionLink`** (built first): one chunk = one WS binary frame with a
  3-byte header. The WS frame boundary *is* the chunk boundary; TCP gives
  integrity and backpressure. Nothing to frame beyond the header. Inbound
  frames arrive on the httpd task.
- **`UartSessionLink`** (later): a UART is a raw byte pipe with no boundaries
  and no integrity, so this link adds its own length-framing (length prefix or
  COBS), a CRC, and resync-after-garbage. All of that lives *inside* this class
  and nowhere else.

The `SessionMux` above is byte-for-byte identical across both. (We build only
the WS link now; the split is designed so extracting the generic mux when UART
arrives is mechanical, not a rewrite — YAGNI until then.)

## Wire format

One chunk:

```
[ session : u16 ][ kind : u8 ][ payload : bytes ]
```

Binary, not JSON-wrapped — so a multi-MB firmware image stays raw (no base64
+33% blowup). `kind`:

| kind     | meaning                                                        |
| -------- | -------------------------------------------------------------- |
| `Open`   | request/accept a session; its stream begins                    |
| `Data`   | bytes for the session's stream                                 |
| `Close`  | **this direction** ended → EOF for the reader                  |
| `Reject` | responder refused an `Open` (e.g. busy) → the session never starts |

`Close` is **per-direction** (the HTTP/2 / yamux END_STREAM model): the
initiator closing means "request fully sent" (EOF on the handler's `in`); the
responder closing means "reply finished" (EOF for the initiator's read).

Over WS the `session` field is redundant with nothing today (single session)
but is always present, so multiplexing later changes only the mux, never the
wire.

## Command layer (on top of the mux)

`type` lives **inside the stream**, never in the chunk — the mux is
command-blind. Convention: the first thing written into a session is a
newline-terminated header line, then the raw body:

```
{"type":"update","partition":"ota_1"}\n<firmware bytes…>
```

The device reads the header line up to `\n`, parses it (which command, which
args), then the handler streams the rest of `in` as the body. The `\n` is the
delimiter that the buffered `JsonReader` (which slurps to EOF) could not
provide — so this replaces the earlier dead-end of trying to read args and body
off one stream. A command with no body (`getLogs`) is just a header line and an
immediate `Close`.

Errors: an unknown `type`, or a handler-level failure, is written into the reply
stream as the handler's normal output (e.g. `{"ok":false,"error":...}`) followed
by `Close`. There is no separate `{id,error}` envelope — the reply stream itself
carries whatever the command wants to say.

## The bridge corollary (why we trust the abstraction)

Because a `Session` is a `Stream`, a dumb two-way pump bridges any two sessions,
across transports, with no command layer at all:

```cpp
void pump(Stream& from, Stream& to) {
    uint8_t buf[512]; size_t n;
    while ((n = from.read(buf, sizeof buf)) > 0)   // 0 = peer CLOSEd → EOF
        to.write(buf, n);
    to.close();                                    // propagate close
}
```

`OpenSession()` over WS + `OpenSession()` over UART + `pump` each way = a
transparent tunnel: bytes in on WS come out the UART and back, and the device
understands nothing about what flows through. That is precisely the
remote-access relay (`docs/backlog/2026-07-03-remote-access.md`: *"a relay only
has to forward one socket per device, understanding nothing"*), derived from the
same primitive. Backpressure composes end-to-end (a slow UART write blocks the
pump, which stops reading WS, which TCP-backpressures the far end); `Close`
propagates. The command layer is just *one* kind of session consumer; a bridge
is another. Neither is special.

## Concurrency: deferred, but the framing is not

Today: **single active session**. A second `Open` gets `Reject`. The handler
drains its session synchronously.

Later (`docs/backlog/2026-07-03-multiplexed-channels.md`): a slot table in the
mux + a worker task per (or pool for) sessions, so several run at once. This
changes only `TryOpen()` and where handlers run — **not the wire format, not
the layers, not any handler.** The session id is on the wire from day one for
exactly this reason.

### Open question: the task model (unresolved, isolated)

`Session::read()` must block until the next inbound chunk. Inbound frames arrive
on the httpd task (one per handler invocation), so *where the handler runs*
decides how `read()` blocks — and this is the **only** thing it affects:

- **httpd task** (single-in-flight, now): `read()` re-drives the socket via the
  ESP-IDF-internal `httpd_ws_get_frame_type` — a documented private-API wart
  (already used by the interim `WsRequestStream`; see the multiplexed-channels
  backlog note). Removed when the worker lands.
- **worker task** (with concurrency): the httpd task just feeds a queue and
  `read()` pops it — no private call.

Same layering either way; only `Session::read()`'s innards change.

## What already exists

The interim primitives shipped while exploring this are the guts of the WS link:

- `WsResponseStream` (commit `0c0afa7`) — streams a reply out as WS fragments.
  Becomes the outbound side of `WsSessionLink` / `Session::write`.
- `WsRequestStream` (commit `d4e0b45`) — drains a request body from WS frames
  via the private frame reader. Becomes the inbound side / `Session::read`.

Neither is wasted; they fold into the link.

## Migration

The remaining backlog items stop being separate features and become "migrate X
onto the mux, delete its old path", all converging here:

1. **`WsSessionLink` + `SessionMux` + `Session`** — build the transport,
   folding in `WsResponseStream`/`WsRequestStream`. CommandManager becomes a
   `Sink`; reads the header line and dispatches.
2. **Firmware upload** (`…firmware-upload-over-websocket.md`) — first real
   streamed consumer; also the first end-to-end verification.
3. **Partition download** (`…partition-download-over-websocket.md`) — outbound
   streamed consumer.
4. **Retire `/api/command`** (`…retire-api-command-route.md`) — delete the HTTP
   command route + CORS; HTTP serves static only.
5. **Login over the session** (`…login-over-websocket.md`) — auth becomes
   per-connection; a `login` command over a session before the socket is
   promoted. (Must precede #4 — it removes the last non-static HTTP route.)
6. **Multiplexed channels** — the concurrency half: slot table + worker task,
   removing the private-API wart.

## Testing

No automated tests (repo convention). Per step: build + flash, then drive the
real flow over the socket (as done for reply streaming: authenticate, exercise
the command, confirm the bytes arrive intact and reassemble). Firmware upload is
the first full round-trip proof of inbound streaming; the bridge/relay is proven
when remote-access is built.
```
