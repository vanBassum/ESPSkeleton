# Session Transport — Migration Roadmap

Living index for resuming the WebSocket → session-mux migration across sessions.
Update the status column as steps land.

- **Design (read first):** [specs/2026-07-09-session-mux-transport-design.md](superpowers/specs/2026-07-09-session-mux-transport-design.md)
- **Step-1 plan:** [plans/2026-07-09-session-transport-step1.md](superpowers/plans/2026-07-09-session-transport-step1.md)

## North star

Every device interaction is a **session** — a full-duplex `Stream` over one
binary transport. Three layers: command layer / `SessionMux` (generic) /
`SessionLink` (`WsSessionLink` now, UART later). Unify, don't grow parallel
paths. The relay/remote-access goal is real and is what the session framing +
byte-pump bridge are for.

## Steps

| # | Step | Status | Backlog |
|---|------|--------|---------|
| 1 | Foundation: mux + `Session`/`WsSessionLink`, no-body commands over binary, frontend cutover, old TEXT path removed | **DONE** (`main`, `c3c2096`) | — |
| 2 | Firmware upload over WS | **NEXT** | [firmware-upload](backlog/2026-07-09-firmware-upload-over-websocket.md) |
| 3 | Partition download over WS | pending | [partition-download](backlog/2026-07-09-partition-download-over-websocket.md) |
| 4 | Retire `/api/command` + `/api/login` + CORS (HTTP static-only) | pending (needs 2, 3, 5) | [retire-api-command](backlog/2026-07-09-retire-api-command-route.md) |
| 5 | Login over WS (per-connection auth; `login` command) | pending (must precede 4) | [login](backlog/2026-07-09-login-over-websocket.md) |
| 6 | Concurrency: worker task + slot table (removes the private-API wart) | pending | [multiplexed-channels](backlog/2026-07-03-multiplexed-channels.md) |

Also [ws-inbound-streaming](backlog/2026-07-09-ws-inbound-streaming.md): partially
done — `WsRequestStream` shipped dormant in step 1; its real use is step 2.

## Shipped in step 1

- **Wire:** `[session:u16 LE | flags:u8 | payload]`. `FLAG_FINAL=0x01` (per-direction EOF), `FLAG_REJECT=0x02` (transport refusal, payload=reason). **Session 0 = device broadcasts** (log lines); clients allocate ids from 1.
- **Command envelope in-stream:** first line `{"type":...,args}\n`, then body (no body for step-1 commands). No `stream:true` flag — a small command is just a short session.
- **Files:** `main/Application/WebServerManager/`: `SessionProtocol.h`, `SessionMux.{h,cpp}`, `WsSessionLink.h`, `WebSocketHandler.{h,cpp}`; frontend `frontend/src/lib/backend.ts`.
- **Result:** commands, replies, and broadcasts all binary chunks — **zero TEXT on the socket**; commands no longer touch `/api/command`.

## Key facts a fresh session must know

- **Single-in-flight is synchronous today:** a step-1 session lives only within one `OnChunk` call on the httpd task, so there is **no busy-gate, no `REJECT` for concurrency, and no client serialization** yet. Both the gate and the frontend open-queue arrive in **step 2**, when a streamed upload holds the socket across frames.
- **Inbound-drain primitive:** `WsRequestStream` + the forward-declared **private `httpd_ws_get_frame_type`** (in `WebSocketHandler.cpp`) drain multi-frame bodies on the httpd task. Dormant now; first exercised in step 2; the private call is removed in step 6 when the worker task lands (see the note in the multiplexed-channels backlog).
- **Verification (no unit tests):** build + flash, then drive over the socket. Build env per [memory] (dot-source `C:\Espressif` v6.0 profile + `PYTHONUTF8=1`, `idf.py build`), flash `idf.py -p COM3 flash`. Probe pattern: `POST /api/login {"password":"admin"}` → token → WS `?token=` → send binary `[sid|FLAG_FINAL|{"type":...}\n]`, read binary chunks (skip session 0 broadcasts) until `FLAG_FINAL`, reassemble → JSON. Device typically at `192.168.50.111`.

## Immediate next action (step 2)

Settle one open question first (brainstorm before coding): **collapse
`updateBegin`/`updateWrite`/`updateEnd` into a single streamed `update` command,
or keep the three and stream `updateWrite`.** Then write a plan, then execute
against the device. Step 2 is where the private-API drain, the busy-gate/`REJECT`,
and the frontend serialization queue all first get exercised end-to-end.
