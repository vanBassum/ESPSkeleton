# Retire the HTTP /api/command route (HTTP serves static only)

**Status: idea, 2026-07-09.** The cleanup that closes the endpoint-decommission
set — do it last.

**Update 2026-07-09 — reframed by the session-mux transport**
(`docs/superpowers/specs/2026-07-09-session-mux-transport-design.md`, step 4).
Commands already left `/api/command` in step 1 (they run over binary sessions).
What still uses the route: firmware upload (step 2) and partition download
(step 3); auth still uses `/api/login` (step 5). So this depends on steps 2, 3
and 5 — once they've migrated, delete `POST /api/command`, `/api/login`, and CORS
so HTTP serves static only. Login must land before this (it removes the last
non-static HTTP route); see the dependency note below.

## Why

Once login, firmware upload, and partition download have moved onto the WS,
nothing uses `POST /api/command` anymore. Removing it (and its `OPTIONS`/CORS
preflight) leaves HTTP doing one job — serving the static app that bootstraps
the page. That is the endgame: **bytes that describe the app → HTTP; bytes that
talk to the device → the one WS.** Two payoffs:

- **Fewer sockets on the tiny server.** Today every `fetch` to `/api/command`,
  every static asset, and the WS each burn a slot in the single-threaded httpd
  pool (~7); under load `lru_purge` evicts the quietest socket — often our own
  WS (`httpd_sock_err: recv 104` / ECONNRESET). One long-lived socket per client
  makes that whole class of bug disappear.
- **Trivial remote bridging.** A relay only has to forward one socket per device
  (see `2026-07-03-remote-access.md`), understanding nothing.

## How

- Delete the `POST /api/command` route, its `OPTIONS` preflight, and the CORS
  header helper (CORS existed only for cross-origin `fetch`; the WS handshake
  does not need it the same way).
- Remove `HttpRequestStream` / `HttpResponseStream` and the Bearer guard on
  `/api/command` (auth is per-connection on the WS now — see
  `2026-07-09-login-over-websocket.md`).
- Keep only the static-file wildcard route (plus whatever the login page needs
  pre-auth).

## Trade-offs / decisions

- **Dev workflow.** The Vite dev server currently proxies `fetch` to
  `/api/command`; confirm everything it needs now rides the WS before deleting.
- **Bench/tooling.** Anything still hitting HTTP commands (scripts, ad-hoc
  tooling) must move to the WS or a dedicated debug transport.

## Depends on

`2026-07-09-login-over-websocket.md`, `2026-07-09-firmware-upload-over-websocket.md`,
`2026-07-09-partition-download-over-websocket.md` (all must migrate off first).
Rides `2026-07-03-multiplexed-channels.md`.

## Done when

The only HTTP routes left are static-file serving; every device interaction is
over the WebSocket.
