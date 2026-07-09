# Partition download over the WebSocket (retire the HTTP download path)

**Status: idea, 2026-07-09.** One of the endpoint-decommission issues in the
"everything over one WS" direction.

## Why

Partition download currently streams over `POST /api/command?type=downloadPartition`
and the browser reads a chunked HTTP body — again on a separate socket because
the WS reply path couldn't stream large/binary data. A multiplexed WS removes
the need for the second connection.

## How

- `downloadPartition` becomes a command emitting an **outbound binary stream**
  over one WS channel, chunked and **interleaved** so a multi-MB download does
  not head-of-line-block interactive commands on the same socket.
- The browser reassembles the channel's binary frames into the file (as it
  reassembles the chunked HTTP body today).

## Trade-offs / decisions

- Outbound binary framing (channel-tagged binary frames) — shared with upload;
  per `2026-07-03-multiplexed-channels.md`.
- Fairness / yielding between chunks so a big download can't starve status polls
  — this interleaving is the whole point, and it only works if handler execution
  is off the transport task (the worker-pool prerequisite in
  `2026-07-03-multiplexed-channels.md`).
- Progress from channel byte count vs. an expected size the UI already knows.

## Depends on

`2026-07-03-multiplexed-channels.md` (binary + outbound streaming, worker-pool execution).
Blocks `2026-07-09-retire-api-command-route.md`.

## Done when

Partition download runs entirely over the WS; nothing uses `/api/command` for
`downloadPartition`.
