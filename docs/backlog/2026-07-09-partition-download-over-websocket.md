# Partition download over the WebSocket (retire the HTTP download path)

**Status: DONE 2026-07-10** (roadmap step 3, `main`). Frontend-only change — the
device already streamed `downloadPartition` to the reply `Stream`. `backend.ts`
now runs it as an outbound WS session (binary reassembly + idle timeout +
truncation guard); nothing uses `/api/command` for `downloadPartition`. Verified
byte-exact on the live device for `nvs`/`www` plus the unknown-partition error.
See the "Step 3 — as built" note in `docs/session-transport-roadmap.md`.

**Status: idea, 2026-07-09.** One of the endpoint-decommission issues in the
"everything over one WS" direction.

**Update 2026-07-09 — reframed by the session-mux transport**
(`docs/superpowers/specs/2026-07-09-session-mux-transport-design.md`, step 3).
Download is an outbound streamed session: the handler writes partition bytes to
`Stream& out`, chunked over the socket and ended by `FINAL`. It reuses the
reply-streaming path already shipped in step 1 — no new framing, just a larger
payload over the same session chunks.

## Why

Partition download currently streams over `POST /api/command?type=downloadPartition`
and the browser reads a chunked HTTP body — again on a separate socket because
the WS reply path couldn't stream large/binary data. A multiplexed WS removes
the need for the second connection.

## How

- `downloadPartition` becomes a command emitting an **outbound binary stream**
  over one WS channel, chunked. (Interleaving so it doesn't head-of-line-block
  other commands is the job of `2026-07-03-multiplexed-channels.md`, **deferred**
  — for now a download owns the socket until it finishes.)
- The browser reassembles the channel's binary frames into the file (as it
  reassembles the chunked HTTP body today).

## Trade-offs / decisions

- Outbound binary framing (channel-tagged binary frames) — shared with upload;
  per `2026-07-03-multiplexed-channels.md`.
- Interleaving/fairness (so a download doesn't starve other commands) is
  **deferred** to `2026-07-03-multiplexed-channels.md`; for now the socket is
  dedicated to the transfer (HOL blocking, accepted).
- Progress from channel byte count vs. an expected size the UI already knows.

## Depends on

`2026-07-09-ws-reply-streaming.md` (outbound streaming — the base). Interleaving
so it doesn't block other commands is deferred to
`2026-07-03-multiplexed-channels.md`. Blocks `2026-07-09-retire-api-command-route.md`.

## Done when

Partition download runs entirely over the WS; nothing uses `/api/command` for
`downloadPartition`.
