# Firmware upload over the WebSocket (retire the HTTP upload path)

**Status: idea, 2026-07-09.** One of the endpoint-decommission issues in the
"everything over one WS" direction.

## Why

Firmware upload currently streams chunks over `POST /api/command?type=updateWrite`
(XHR), on its own HTTP connection — precisely because the single-threaded HTTP
server plus the fixed WS reply buffer couldn't carry a multi-MB inbound stream
without starving everything else. With a multiplexed WS (worker-pool execution,
per-channel streaming) that reason is gone.

## How

- `updateWrite` becomes a command fed an **inbound binary stream** over one WS
  channel; the handler drains `Stream& in` frame-by-frame (esp_ota sequential
  writes, lazy sector erase — the flash-write path is unchanged, only its feed
  changes).
- The `updateBegin`/`updateWrite`/`updateEnd` **session collapses into one
  streamed command** (per `multiplexed-channels.md`): ordering within one stream
  is free, finalize runs at end-of-stream. The cross-request session state only
  ever existed because HTTP forced the image into many requests.
- Progress reported over the WS (channel byte count) instead of XHR upload
  progress.

## Trade-offs / decisions

- Inbound binary framing scheme (how a channel's binary chunks are tagged) —
  shared with download; defined in `multiplexed-channels.md`.
- Chunk size vs. throughput; backpressure so the transport can't outrun the
  flash write.
- Keep esp_ota image validation / running-slot refusal exactly as today.

## Depends on

`multiplexed-channels.md` (binary + inbound streaming, worker-pool execution).
Blocks `2026-07-09-retire-api-command-route.md`.

## Done when

Firmware upload runs entirely over the WS; nothing uses `/api/command` for
`updateWrite`, and the update session is a single streamed command.
