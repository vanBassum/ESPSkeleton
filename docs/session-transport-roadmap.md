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
| 2 | Firmware upload over WS (now `writePartition`) | **DONE** (`main`, `9585366`) — verified on device: app + data paths, reboot into OTA'd slot, device-driven progress | — (shipped) |
| 3 | Partition download over WS | **DONE** (`main`) — verified on live device: nvs/www exact-size, unknown-partition error, over the same reply-streaming path | — (shipped) |
| 4 | Retire `/api/command` + `/api/login` + CORS (HTTP static-only) | **DONE** (`main`) — verified on device: HTTP serves the WS upgrade + static app only (`GET /` → 200, `POST /api/command` → 405 route gone); WS auth-matrix intact. `/api/login` went in step 5. | — (shipped) |
| 5 | Login over WS (per-connection auth; session key) | **DONE** (`main`, `a97b9f7`..`e89942e`) — verified on device: empty-password and password-set matrix over a fresh WS (`hello`/`login`/`auth` resume all pass), empty password restored, `getLogs` works with no login — [design](superpowers/specs/2026-07-10-login-over-websocket-design.md) | — (shipped) |
| 6 | Concurrency: worker task + slot table | **REJECTED 2026-08-03** — not worth a slot table and per-channel buffers on this much RAM. Long uploads instead become many short sessions via an addressed `writePartition`. The stack-tax and private-API halves survive on their own. | [command-worker-task](backlog/2026-08-03-command-worker-task.md) |

Code quality refactor: **DONE** — `WebServerManager`/`WebSocketHandler` decomposed
into `Authenticator` (credential authority), `WsConnection`/`ConnectionRegistry`
(per-connection state + slot table), and `AuthGate` (pre-auth handshake +
authed/not routing decision), leaving `WebSocketHandler::HandleBinary` as
find-connection → gate → dispatch. See
[design](superpowers/specs/2026-07-10-webserver-decomposition-design.md) and
[plan](superpowers/plans/2026-07-10-webserver-decomposition.md).

Also `ws-inbound-streaming` shipped: `WsRequestStream` (dormant in step 1) became
`WsSessionLink::RecvChunk` in step 2.

Deferred by choice: [broadcast-redesign](backlog/2026-07-09-broadcast-redesign.md) —
log broadcasts (session 0) are the last non-session path; to be folded into
device-initiated sessions and have their dead-client / warning-storm handling
designed properly at a later stage.

## Shipped in step 1

- **Wire:** `[session:u16 LE | flags:u8 | payload]`. `FLAG_FINAL=0x01` (per-direction EOF), `FLAG_REJECT=0x02` (transport refusal, payload=reason). **Session 0 = device broadcasts** (log lines); clients allocate ids from 1.
- **Command envelope in-stream:** first line `{"type":...,args}\n`, then body (no body for step-1 commands). No `stream:true` flag — a small command is just a short session.
- **Files:** `main/Application/WebServerManager/`: `SessionProtocol.h`, `SessionMux.{h,cpp}`, `WsSessionLink.h`, `WebSocketHandler.{h,cpp}`; frontend `frontend/src/lib/backend.ts`.
- **Result:** commands, replies, and broadcasts all binary chunks — **zero TEXT on the socket**; commands no longer touch `/api/command`.

## Key facts a fresh session must know

- **Single-in-flight is synchronous today:** a step-1 session lives only within one `OnChunk` call on the httpd task, so there is **no busy-gate, no `REJECT` for concurrency, and no client serialization** yet. Both the gate and the frontend open-queue arrive in **step 2**, when a streamed upload holds the socket across frames.
- **Inbound-drain primitive:** `WsRequestStream` + the forward-declared **private `httpd_ws_get_frame_type`** (in `WebSocketHandler.cpp`) drain multi-frame bodies on the httpd task. Dormant now; first exercised in step 2; the private call is removed only if the worker task ever lands (see [command-worker-task](backlog/2026-08-03-command-worker-task.md)) — step 6 as a concurrency step is rejected.
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

## Step 3 — as built

**Frontend-only change.** The device side needed nothing: `Cmd_DownloadPartition`
already writes raw partition bytes to `out`, and `Session::write` → the reply
window → `OnSessionOpened`'s `finish()` already stream any outbound reply over WS
(shipped in step 1, exercised by upload progress in step 2). Download is just a
larger reply over the same session chunks — no new framing.

- **`backend.ts`:** `downloadPartitionFile` moved off `POST /api/command` onto a WS
  session opened through the same `enqueue` FIFO as upload (owns the socket until
  done — the device would `REJECT` an interleaved id). Added a **binary reply mode**
  to the reassembler: accumulate raw chunks, resolve with a `Uint8Array`, report
  cumulative bytes for progress. The reply timeout is now **idle-based** (`bumpTimer`
  on each chunk) so a large-but-steady transfer isn't killed by a total-duration cap;
  this also covers the upload progress path. A **truncation guard** throws when the
  received byte count ≠ the expected partition size (the old HTTP path saved truncated
  files silently). The dead `authHeaders()` helper was removed. `FirmwarePage` needed
  no change — the method signature is unchanged.
- **Still on `/api/command`:** only the pre-WS `ping` token-check (retired in step 4/5).
- **Verified against the live device** (`192.168.50.111`, firmware unchanged) by
  reproducing the exact wire path: `nvs` (24576 B) and `www` (917504 B, many outbound
  chunks) round-trip byte-exact; an unknown label returns `{"ok":false,"error":...}`
  in a FINAL chunk and trips the frontend guard.
- **Known non-issue (efficiency, deferred):** the outbound `SESSION_WINDOW` is 512 B,
  so a large download emits many small WS frames (~307 KB/s for www). Correct, just
  not optimal; a larger window costs permanent RAM and belongs with the step-6 tuning,
  per the backlog's accepted HOL-blocking / efficiency deferral.

## Step 5 — as built

Auth moved fully in-band: a **connection-level `authed` bit** (per-fd, in the WS
transport's client table) replaces `?token=`/`Bearer`. The WS upgrade is accepted
**unauthenticated**; a gate in front of `SessionMux` handles the pre-auth vocabulary
`hello`/`login`/`auth` itself and never lets an un-authed chunk reach the mux or
`CommandManager` — so the command layer stays 100% auth-blind. `SessionTable` is
**kept**, repurposed as the RAM key table for `auth{key}` resume across reconnects
within one boot (reboot clears it — re-login required). Empty `web.password` is the
new default: auth is disabled out of the box (`authed = !AuthRequired()` at connect),
matching the "open by default" template goal. `POST`/`GET /api/login`, the WS
`?token=` query param, and the pre-WS HTTP `ping` token-check are all removed —
nothing HTTP-side does auth anymore. `/api/command` and CORS are untouched, deferred
to step 4 as planned.

- **Fixed during build-out:** the gate originally only intercepted chunks on
  *un-authed* connections; an already-authed connection sending `hello`/`login`/`auth`
  (e.g. a stray re-login) fell through to the mux and got treated as an unknown
  command. The gate now recognizes the handshake verbs on any connection, authed or
  not, and only forwards everything else past itself once authed.
- **Verified on a freshly flashed device** (`192.168.50.111`, this task): WS smoke
  matrix — empty-password `hello` (`authRequired:false`), password-set `hello`
  (`authRequired:true`), pre-auth `getLogs` rejected, wrong password rejected, correct
  password mints a key and unlocks `getLogs`, `auth{key}` resume on a new connection
  succeeds, a bogus key is rejected, then the empty password was restored and confirmed
  with a fresh connection (`hello` → `authRequired:false`, `getLogs` succeeds with no
  login). Browser-matrix verification (login page, cross-tab, reboot-drops-session,
  `pnpm dev` cross-origin) is left to manual check per the design's done-when criteria.
