# WS inbound streaming (request body over WS frames)

**Status: idea, 2026-07-09.** The inbound sibling of
`2026-07-09-ws-reply-streaming.md`: let a command *read* a large request body
streamed over the WebSocket, instead of the whole request arriving in one frame.
Needed to move firmware upload onto the WS.

## Why

Today a WS request is a single frame (`buf[512]` in `HandleWs`) parsed for
`{id,type,...}` — fine for small JSON, useless for a multi-MB firmware image. The
HTTP `/api/command` path handles large inbound via `HttpRequestStream` (drains
`httpd_req_recv`). The WS needs the equivalent: the handler's `Stream& in` fed
frame-by-frame as the client streams the body.

## How

- A streaming request is announced by an initial control frame `{id,type,...}`;
  subsequent frames (binary, for firmware) belong to that request and are fed
  into the handler's `Stream& in` as they arrive.
- The handler blocks on `in.read()` and processes as data lands (esp_ota
  sequential writes) — the same contract as HTTP's `HttpRequestStream`.
- **No multiplexing (deferred):** the inbound stream owns the socket until it
  completes. Only one streaming request is in flight at a time, which keeps the
  framing simple (no channel ids needed yet). HOL blocking, accepted.

## Trade-offs / decisions

- End-of-stream signal (final frame / declared length) so the handler knows the
  body is complete.
- Backpressure so the client can't outrun flash writes.
- Binary framing is simple while single-in-flight; it gets channel ids only when
  `2026-07-03-multiplexed-channels.md` lands.

## Depends on

Nothing new — same transport rework as `2026-07-09-ws-reply-streaming.md`, other
direction. Independent of `2026-07-03-multiplexed-channels.md` (that adds
concurrency later). Base for `2026-07-09-firmware-upload-over-websocket.md`.

## Done when

A command can consume a multi-MB request body streamed over the WS (proven by
firmware upload).
