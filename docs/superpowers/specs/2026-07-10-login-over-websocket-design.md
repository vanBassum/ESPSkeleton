# Login over the WebSocket — Design

**Date:** 2026-07-10
**Status:** Approved in discussion (this doc records it)
**Roadmap:** session-transport step 5 (`docs/session-transport-roadmap.md`).
Supersedes the transport half of `2026-07-03-webserver-login-design.md` (the
command *contract* and the "auth lives at the transport edge" principle carry
over unchanged); redesigns the backlog note
`docs/backlog/2026-07-09-login-over-websocket.md`.

## Goal

Move authentication onto the one WebSocket so HTTP can eventually serve static
files only (step 4). Today auth is HTTP: `POST /api/login` mints a token into a
RAM `SessionTable`, the token rides `?token=` on the WS upgrade and `Bearer` on
`/api/command`, and the browser must pre-validate the token with an HTTP `ping`
because a refused upgrade and a dead network look identical. In the
everything-over-one-socket model there is no HTTP command surface to guard and
no URL to hang a token on, so login has to happen in-band.

The auth **model** barely changes — a password mints a session key, the key is
presented once per connection, the connection is trusted for its lifetime, and
the key table is RAM (reboot ⇒ re-login). Only the **transport** of login and
the key moves from HTTP/URL onto the socket. This is a transport migration, not
an auth redesign.

## Core decisions

1. **Auth is connection-level state, established once.** A per-connection
   `authed` bit lives in the WS transport's per-fd client table (it replaces
   `?token=`/`clientTokens_`). It is set once and consulted per inbound chunk;
   later frames carry no credential. Auth is a property of *how the connection
   arrived*, never of a command — so it is owned by the transport, and the
   command layer never learns auth exists.

2. **The gate sits in front of the mux.** An unauthenticated connection's chunks
   are handled by the auth gate itself (the login handshake) and never reach the
   `SessionMux` or `CommandManager`. Once authed, chunks pass straight through.
   This keeps the mux and the whole command layer 100% auth-blind — not even a
   hook.

3. **Session key, presented once per connection — not per frame.** `login`
   mints a key (the existing `SessionTable` token); the client stores it; on
   reconnect it presents the key (not the password) to re-establish the bit.
   No per-frame token, so no per-frame overhead — just one table lookup when a
   connection authenticates.

4. **Empty password = auth disabled, and it is the default.** `web.password`
   defaults to `""`. Empty means every connection is `authed` from birth: no
   login page, open out of the box — the right default for a template you flash
   and poke at. A downstream product sets a password to switch auth on.

5. **`SessionTable` stays.** The backlog floated deleting it; we consciously
   keep it (~40 lines, already working). It is exactly what gives
   resume-on-reconnect, reboot-invalidation, and "password never stored
   client-side" — the professional behaviors we want. The reusable
   credential/key mechanism stays in `WebServerManager`; a dedicated
   `AuthManager` is deferred until a second transport (UART/relay) needs
   securing.

## Layers

Auth is a new connection-level gate between the transport seam and the mux.
Everything else is unchanged.

```
┌─ [Command layer] ── CommandManager dispatch (+ future bridge/relay)
│                     consumes a Session (a Stream); auth-blind
│
├─ [Session / Stream] full-duplex byte stream the handler holds (in == out); auth-blind
│
├─ [SessionMux] ────── chunk ⇄ Session; routes by id; open / FINAL / REJECT; auth-blind
│
├─ [Auth gate] ─────── connection-level gatekeeper (per-fd `authed` bit)   ← NEW
│                        authed     → pass chunk through to the mux
│                        not authed → handle the pre-auth handshake itself
│                        uses web.password + SessionTable (WebServerManager)
│
├─ [SessionLink] ───── WsSessionLink: WS frame ⇄ chunk (3-byte header); unchanged
│
└─ [WebSocket] ─────── esp_http_server binary frames; raw transport
```

## Flow

A virgin connection logs in and runs two commands. The login handshake is
handled entirely by the gate; the second command skips auth — a one-bool check.

```mermaid
sequenceDiagram
    autonumber
    participant FE as Frontend (backend.ts)
    participant WS as WebSocket
    participant Link as SessionLink
    participant Gate as Auth gate
    participant Mux as SessionMux
    participant Cmd as CommandManager

    rect rgb(235, 244, 255)
    Note over FE,Cmd: 1 — Connect (virgin, no stored key)
    FE->>WS: open /ws  (no ?token=)
    WS->>Gate: connection opened
    Note over Gate: web.password set → authed = false
    WS-->>FE: socket open
    end

    rect rgb(255, 244, 235)
    Note over FE,Cmd: 2 — Login handshake (gate only; mux & commands never see it)
    FE->>WS: [sid=1 | FINAL | {"type":"login","password":"…"}]
    WS->>Link: binary frame
    Link->>Gate: chunk(sid=1, FINAL)
    Note over Gate: not authed → intercept<br/>check web.password ✓<br/>mint key (SessionTable)<br/>authed = true
    Gate-->>Link: chunk(sid=1, FINAL, {"ok":true,"key":"…"})
    Link-->>WS: binary frame
    WS-->>FE: reply
    Note over FE: store key, authenticated = true
    end

    rect rgb(235, 255, 240)
    Note over FE,Cmd: 3 — First command: getLogs (authed → flows to the mux)
    FE->>WS: [sid=2 | FINAL | {"type":"getLogs"}]
    WS->>Link: binary frame
    Link->>Gate: chunk(sid=2, FINAL)
    Note over Gate: authed = true → pass through
    Gate->>Mux: chunk(sid=2)
    Mux->>Cmd: OnSessionOpened → Execute("getLogs")
    Cmd-->>Mux: write reply, FINAL
    Mux-->>Gate: chunk(sid=2, FINAL, {…logs…})
    Gate-->>Link: (verbatim)
    Link-->>WS: binary frame(s)
    WS-->>FE: reply → resolve
    end

    rect rgb(235, 255, 240)
    Note over FE,Cmd: 4 — Second command: reboot (already logged in — NO re-auth)
    FE->>WS: [sid=3 | FINAL | {"type":"reboot"}]
    WS->>Link: binary frame
    Link->>Gate: chunk(sid=3, FINAL)
    Note over Gate: authed = true → pass through (one bool check)
    Gate->>Mux: chunk(sid=3)
    Mux->>Cmd: OnSessionOpened → Execute("reboot")
    Cmd-->>Mux: write reply, FINAL
    Mux-->>Gate: chunk(sid=3, FINAL, {"ok":true})
    Gate-->>FE: reply → resolve
    end
```

**Reconnect** differs only in phase 2: the client already has a key, so it sends
`{"type":"auth","key":"…"}` instead of `login`; the gate validates the key and
flips the bit — same shape, no password, silent. After a **reboot** the RAM
table is empty, so `auth` fails and the client falls back to `login` (the login
page). **Empty password** collapses phases 1–2: the gate sets `authed` at
connect, and phase 3 works with no login at all.

## Pre-auth vocabulary

Handled by the gate on an unauthenticated connection; each is a normal session
(header line, single `FINAL` chunk, small). Anything else on an unauthenticated
connection is rejected.

| type | request | reply | effect |
|---|---|---|---|
| `hello` | `{}` | `{"name":"<device>","authRequired":<bool>}` | none. Feeds the login page's brand slot; `authRequired:false` (empty password) tells the client to skip the login page. Replaces `GET /api/login`. |
| `login` | `{"password":"…"}` | `{"ok":true,"key":"…"}` / `{"ok":false}` | on match: mint a key, set the connection `authed`. Replaces `POST /api/login`. |
| `auth` | `{"key":"…"}` | `{"ok":true}` / `{"ok":false}` | on a live key: set the connection `authed` (silent resume). |

The key is the existing `SessionTable` token (128 bits `esp_random`, 32 hex
chars). `login` calls `SessionTable::Create`; `auth` calls `SessionTable::Touch`.
Rejections are ordinary command replies, not transport `REJECT` (that stays
reserved for "couldn't start a session at all").

## Gate behavior (device)

`WebSocketHandler` owns the per-fd state and the gate:

- **Per-fd fields** (replacing `clientTokens_`): `bool authed`, `int64_t
  connectedAt`.
- **At connect** (`AddWsClient`): `authed = (web.password is empty)`;
  `connectedAt = now`. The WS upgrade no longer reads or requires `?token=` —
  it only checks there is room in the client table (see reaper).
- **On an inbound chunk** (`HandleBinary`): look up the fd's slot.
  - `authed` → hand the chunk to the mux exactly as today (`SessionMux::OnChunk`
    → `OnSessionOpened` → `CommandManager::Execute`).
  - not `authed` → peek the header line for `type` and run the pre-auth
    vocabulary above; emit the reply as a single `FINAL` chunk on the request's
    session id (built directly via the link, the way `Session::finish/reject`
    do). The chunk never touches the mux or `CommandManager`.
- **Credential calls** delegate to `WebServerManager`: `CheckPassword(pw)` (does
  the password-epoch check, then compares) and `MintKey(out)` /
  `ValidateKey(key)` wrapping `SessionTable`. `hello` calls the existing
  `GetDeviceName` + reports whether `web.password` is empty.

## Broadcasts gated to authenticated connections

Log lines broadcast on session 0. Today they reach every WS client; now that a
connection can be unauthenticated (sitting on the login page), `Broadcast` /
`BroadcastBinary` must send **only to fds whose `authed` bit is set**. An
unauthenticated socket receives nothing but its own handshake replies.

## Pre-auth reaper

An unauthenticated socket can only send `login`/`auth`/`hello`, but it still
occupies one of the 4 client slots — a new exposure (today a bad upgrade is
refused outright). Guard it without a timer task: when `AddWsClient` finds the
table full, sweep out any **unauthenticated** client older than a short deadline
(`PRE_AUTH_TIMEOUT`, 10 s) to make room; if none can be freed, refuse the
upgrade. Authenticated connections are never reaped by this path (their liveness
is the socket + heartbeat, as today).

## Password change

Keep today's semantics. `CheckPassword` still detects a changed `web.password`
and clears the `SessionTable` (so stored keys stop resuming). Live
authenticated connections keep their `authed` bit for their lifetime — they are
not force-dropped (matches the current spec's "live connections stay trusted for
their lifetime"); they must re-login only when they next reconnect. Hard,
immediate revocation is out of scope.

## Frontend (`backend.ts`)

The layered client refactor is **deferred** (see below); for step 5 adapt the
existing service:

- **Connect unauthenticated.** `doConnect` no longer needs a token and drops the
  HTTP `ping` pre-check — the WS opens for anyone, and a wrong password now comes
  back as a clean `login` reply instead of an ambiguous socket failure.
- **On every (re)connect**, once the socket is open: send `hello`. If
  `authRequired` is false → `setAuthenticated(true)`. Else if a key is stored →
  send `auth{key}`; on `ok` authenticate, on failure clear the key. Else stay
  unauthenticated → login page.
- **`login(password)`** sends the `login` session; on `ok` store the returned key
  in `sessionStorage` (same slot as today's token) and `setAuthenticated(true)`.
- **`getLoginInfo()`** becomes the `hello` session (was `GET /api/login`).
- `login`/`auth`/`hello` reuse the existing `send()` path (enqueue → allocSession
  → awaitReply); they are ordinary sessions the *device gate* intercepts, so the
  client needs no special framing.
- **Remove:** the `Authorization`/`Bearer` header, `?token=` on the WS URL, the
  `commandUrl("ping")` call, and (with them) the last frontend HTTP `fetch`.
  `LoginPage` and `useAuth` are unchanged in shape.

## Scope / interaction with step 4

Step 5 deletes `GET`+`POST`+`OPTIONS /api/login` and removes the frontend's last
HTTP calls, so after it **nothing** the frontend uses touches `/api/command`.
Deleting the `/api/command` route itself, its `Bearer` guard, and the CORS
helper is **step 4** — left in place here (unused) to keep this change focused.

## Deleted vs. kept

**Deleted (this step):** `GET/POST/OPTIONS /api/login`; `?token=` on the WS
upgrade + upgrade-time `ValidateToken`; the HTTP `ping` pre-check; per-fd
`clientTokens_`.
**Kept:** `SessionTable` (mint/validate/expire keys + idle GC), `web.password`
and its epoch/change detection, the command contract and mux/link unchanged.

## Deferred (noted, not built here)

- **Access + refresh token rotation.** A later evolution: short-lived access
  token (~5 min) + longer rotating refresh token (~1 h), `auth` exchanging a
  refresh token for a fresh access token. The single-key model here is the
  foundation; adding the split changes only the token semantics, not the layers.
- **Layered frontend client.** Refactor `backend.ts` into transport / auth / mux
  / stream layers mirroring the backend, replacing today's flatter service.
- **`AuthManager`.** Extract the credential/key mechanism out of
  `WebServerManager` when a second secured transport makes the seam real.
- **Hard session revocation.** Force-dropping live authenticated connections on a
  password change.

## Edge cases

- **Reload (F5):** key survives in `sessionStorage` → reconnect sends `auth{key}`
  → silent. Tab close clears it.
- **Reconnect (roam / AP reboot):** key still in the table → `auth` resumes; no
  login-page flash.
- **Device reboot:** RAM table gone → `auth` fails → login page.
- **Password changed while a tab is open:** the live connection stays trusted;
  the stored key stops resuming, so the next reconnect lands on the login page.
- **Empty → non-empty password (product enables auth):** existing open
  connections were `authed` from birth and stay so for their lifetime; new
  connections must log in. Acceptable; a reboot makes it uniform.
- **All 4 slots held by pre-auth squatters:** the reaper frees stale
  unauthenticated slots on the next upgrade attempt; a legitimate login evicts a
  squatter.

## Verification

No automated tests (repo convention). Build + flash, then drive the socket
(reusing the step-2/3 probe pattern, minus the HTTP login — open the WS, send
`login`, read the key):

- Wrong password → `{"ok":false}`, no key, commands still rejected.
- Right password → key returned; a following `getLogs`/`reboot` runs with no
  re-auth.
- Reconnect with the stored key (`auth`) → resumes without the password.
- Reboot → `auth` fails → client shows the login page.
- Empty `web.password` → no login required; commands work immediately; login
  page never shows.
- Log broadcasts arrive only on an authenticated connection.
- Leave a console tab open past the idle window → never logged out (heartbeat).
- `pnpm dev` against the device → login + commands work cross-origin over the WS.
