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
| 2 | Firmware upload over WS (now `writePartition`) | **DONE** (`main`, `9585366`) — verified on device: app + data paths, reboot into OTA'd slot, device-driven progress | [firmware-upload](backlog/2026-07-09-firmware-upload-over-websocket.md) |
| 3 | Partition download over WS | pending | [partition-download](backlog/2026-07-09-partition-download-over-websocket.md) |
| 4 | Retire `/api/command` + `/api/login` + CORS (HTTP static-only) | pending (needs 2, 3, 5) | [retire-api-command](backlog/2026-07-09-retire-api-command-route.md) |
| 5 | Login over WS (per-connection auth; `login` command) | pending (must precede 4) | [login](backlog/2026-07-09-login-over-websocket.md) |
| 6 | Concurrency: worker task + slot table (removes the private-API wart) | NOT PART OF THIS ROADMAP | [multiplexed-channels](backlog/2026-07-03-multiplexed-channels.md) |

Dont forget to do a code quality refactor after this is working. I think we should seperate some things out of the webmanager. but thats for later to figure out

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

## Step 2 — as built

Resolved the open question by collapsing `updateBegin`/`updateWrite`/`updateEnd`
into **one streamed `writePartition` command** (generic partition write; app-vs-data
is an internal branch), plus a unified `PartitionWriter` that both the WS upload and
the device-pull `updateFromUrl` feed. Key decisions:

- **`PartitionWriter`** (`main/Application/UpdateManager/`) owns the app/data branch:
  app → `esp_ota_*` (validate + set-boot, running slot refused); data → raw write with
  **lazy per-sector erase** (no up-front full-partition erase). RAII: dtor aborts a
  half-written app image. `UpdateManager` loses `target_`/`otaHandle_`/`writeOffset_`/`mutex_`.
- **Inbound streaming is at the session-chunk level, not WS fragments.** A browser
  `ws.send()` is always one FIN=1 message, so the body arrives as multiple session
  chunks sharing one id; `Session::read()` pulls continuation chunks via
  `WsSessionLink::RecvChunk` (the private `httpd_ws_get_frame_type`, folded out of the
  dormant `WsRequestStream`) until a chunk carries `FLAG_FINAL`. `in == out ==` the Session.
- **Envelope split:** first chunk = `{"type":"writePartition","partition":"<label>"}\n`,
  body follows. `OnSessionOpened` peeks the header line to route; the handler reads the
  same line for args, then drains the body.
- **Frontend:** `send()` and `uploadPartition()` now run through an **open-serialization
  FIFO queue** (`enqueue`) — one session on the wire at a time, required so nothing
  interleaves a session id mid-upload. XHR `/api/command` upload path removed.

**Verified on device** (`192.168.50.111`): app upload (`Strux.bin` → `ota_1`, validated,
set-boot, rebooted into the OTA'd slot), data upload (`www.bin` → `www`, lazy per-sector
erase), and device-authoritative progress (`{"p":…}` reports streamed on the reply,
mapped to the bar). Also confirmed through the real browser UI.

- **Progress is device-driven.** Client "bytes sent" can't see the flash-write position
  (the OS buffers the socket), so the handler streams `{"p":<bytesWritten>}` on the reply
  as it writes (via `Session::flush()` → non-final chunk); the frontend maps that to the
  bar. Residual: a short tail during `esp_ota_end` validation for app images (data
  partitions have no finalize).
- The explicit mux busy-gate/`REJECT` was not needed for single-httpd-task draining (a
  mismatched sid mid-body fails the read defensively); it returns with step 6's worker task.
