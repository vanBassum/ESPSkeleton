# WS reply streaming (single reply, no 4096-byte cap)

**Status: idea, 2026-07-09.** The minimal transport fix that lets a single WS
command reply exceed the fixed buffer. **Distinct from — and a prerequisite of
— `2026-07-03-multiplexed-channels.md`.**

## Not the same as multiplexing

Two separate capabilities, easily conflated:

- **Reply streaming (this issue).** One command's reply is written out *as it is
  produced* — fragmented WS frames (initial TEXT `fin=0` → `HTTPD_WS_TYPE_CONTINUE`
  → final `fin=1`) — instead of being buffered into `wsBuf_[4096]`. Removes the
  size cap. Still **one reply at a time** on the socket: no concurrency, no
  channel ids, no worker task.
- **Multiplexed channels (`2026-07-03-multiplexed-channels.md`).** *Many* logical
  streams interleaved concurrently over the socket, which needs channel-id
  framing **and** handler execution moved off the transport task (worker pool).
  Builds on reply streaming; adds concurrency.

You can ship reply streaming on its own. It directly fixes
`2026-07-09-ws-reply-buffer-overflow.md` (getLogs) with none of the multiplexing
machinery. That's the point of splitting it out.

## How

- Replace the fixed `BufferStream out(wsBuf_, 4096)` in
  `WebSocketHandler::DispatchMessage` with a `WsResponseStream` whose `write()`
  flushes each buffer-full as a WS fragment (first fragment TEXT `fin=0`,
  continuations `fin=0`, last `fin=1`). The handler still writes
  `{"id":N,"payload":<...>}` through `Stream& out` exactly as today — it just
  streams instead of filling a cap.
- Hold `sendMutex_` for the **whole** fragmented message so a log broadcast can't
  interleave between fragments (otherwise the browser reassembles a corrupt
  message).
- Decide mid-stream error handling: once bytes are on the wire a reply can no
  longer become `{"id":N,"error":...}`, so a failed handler mid-stream needs a
  defined abort (close the socket, or a trailing marker the client detects).

## Trade-offs / decisions

- Fragment/flush buffer size: small = more frames + overhead, large = more RAM.
- One reply still owns the socket for its duration (head-of-line blocking) —
  fine for `getLogs`-sized replies; large binary transfers are what multiplexing
  is for, not this.
- `sendMutex_`-across-message means broadcasts wait behind a streaming reply.
  Acceptable at these sizes.

## Depends on

Nothing — self-contained transport change. **Fixes**
`2026-07-09-ws-reply-buffer-overflow.md`. **Prerequisite of**
`2026-07-03-multiplexed-channels.md`.

## Done when

A command reply larger than 4 KB (e.g. a full `getLogs`) arrives intact over the
WebSocket.
