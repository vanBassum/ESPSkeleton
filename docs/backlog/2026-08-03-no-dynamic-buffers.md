# Get the dynamic buffers out (plan, not today's work)

Bas, 2026-08-03: heap buffers are not wanted in this firmware. Fragmentation on a
device that runs for months is not something you can test your way out of, and a
failed allocation in the middle of a firmware write is a failure mode that a fixed
buffer simply does not have.

Nothing here is broken today. This is a direction, with the sites ranked by how much
they actually matter.

## The pattern to replace them with

It already exists in the transports: `sessionFrame_` and `sessionInbound_` are
fixed-size **members of the transport**, lent to each session. Extend that — a
handler that needs a scratch buffer borrows one from the transport that dispatched
it, rather than allocating or putting kilobytes on a task stack.

Why a *per-transport* buffer and not one global static: the local socket and the
relay dispatch on their own tasks and can run handlers simultaneously, so a single
shared scratch buffer would race. Per transport it cannot, because each transport
runs one command at a time — which the rejection of concurrency
(`docs/reasoning/2026-08-03-12h30-addressing-replaces-concurrency.md`) makes a
permanent property rather than a current accident.

Mechanically that means the scratch buffer wants to arrive with the session, so a
handler can reach it without knowing which transport it is serving.

## Sites, worst first

**1. `RelayManager::OnData` — a malloc per inbound WebSocket frame.**
The queue carries `{pointer, length}` and each frame is copied into a fresh
allocation, freed after dispatch. This is the hottest path in the system: a firmware
upload is hundreds of allocate/free cycles of ~4 KB, which is exactly the shape that
fragments a heap.

The catch, and why it was written this way: a static ring at `QUEUE_DEPTH` 16 ×
`INBOUND_WINDOW` 4096 is ~65 KB of internal RAM, permanently. Free heap on the
devkit measured **109 KB**, so that is not affordable as-is. Real options, needing a
decision rather than a patch:

- shrink the depth and accept backpressure (the depth exists because a firmware push
  streams while the consumer pauses ~32 ms per sector to write flash — so this trades
  directly against upload throughput),
- shrink the inbound window and send smaller chunks,
- put the ring in PSRAM where a board has it (this devkit does not),
- a small fixed pool of slots plus explicit refusal when it is exhausted, which is at
  least a *bounded* failure instead of a heap-dependent one.

**2. `WiFiInterface::ScanNetworks` — `new wifi_ap_record_t[count]` per scan.**
Runtime-sized, allocated and freed on every scan. Easy fix: a fixed maximum number of
results, which the caller already bounds anyway (`maxResults`) — so the allocation is
sized by something we have already decided to cap.

**3. `UpdateManager`'s `HeapBuf` — 4 KB per write/download command.**
Added 2026-08-03 to get the buffer off a transport task stack, where 4 KB of an 8 KB
stack was too much. The heap was the quick way to fix a real overrun; the borrowed
per-transport scratch buffer above is the intended answer, and this is the site that
motivates building it.

**4. `ConsoleManager`'s log ring — one allocation at Init, never freed.**
Effectively static already: allocated once at startup, sized from constants, PSRAM
when available. Nothing to fix unless the "no heap at all" rule is meant literally,
in which case it becomes a plain array and the PSRAM preference is lost.

## Suggested order

Do **2** first — it is small and self-contained. Then **3** together with building the
borrowed-scratch mechanism, since 3 is the reason to build it. Leave **1** until the
RAM trade above has actually been decided; changing it without that decision just
moves the problem. Leave **4** alone unless the rule is literal.
