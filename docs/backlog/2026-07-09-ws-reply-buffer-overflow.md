# WS command reply overflows the 4096-byte buffer (getLogs breaks)

**Status: bug, reproduced 2026-07-09** (found while adding web pages to the
KC Thermostat, a Strux consumer).

## Symptom

Opening the Console page throws a red "timeout" toast once the device has been
running long enough to accumulate logs. `getLogs` never resolves.

## Root cause

- `ConsoleManager::WriteHistory` dumps the whole log ring (`MAX_LINES=200 ×
  MAX_LINE_LEN=200`, up to ~40 KB) into the reply.
- `WebSocketHandler::DispatchMessage` serializes every WS reply into a fixed
  `char wsBuf_[4096]` via a bounded `BufferStream`.
- Once the serialized reply exceeds 4096 bytes it is truncated to invalid JSON
  (or the frame send fails); the browser's `JSON.parse` throws, the reply is
  never matched to its request id, and `backend.send("getLogs")` hits its 10 s
  timeout → red toast.
- The HTTP `/api/command` path is unaffected — `HttpResponseStream` streams
  unbounded via `httpd_resp_send_chunk`. **Only the WS reply path has the fixed
  buffer.**

## Reproduction

Flood the log past ~4 KB (e.g. ~45 WS connect/disconnect cycles, each logs
~120 B of "client added/removed"), then call `getLogs` over the WS: the reply
never arrives. Under ~4 KB it returns valid JSON (~62 lines ≈ 3.5 KB was the
last size that still worked).

## Fixes

- **Proper fix:** make the WS reply path stream a single reply — see
  `2026-07-09-ws-reply-streaming.md`. That is a small, self-contained transport
  change (no cap, no multiplexing) and is all this bug needs.
- **Stopgap (frontend-only, no firmware):** point `backend.getLogs()` at the
  existing HTTP `/api/command?type=getLogs` route (which already streams)
  instead of the WS `send()`. Console keeps the WS only for its live
  log-broadcast subscription. Reverts for free once streaming lands.

## Relates to

`2026-07-09-ws-reply-streaming.md` (the fix). `2026-07-03-multiplexed-channels.md`
builds on that but is *not* required for this bug. Not related to
`2026-07-06-flash-circular-logging.md` (that is record logging, not console text).
