# Webserver Login — Design

**Date:** 2026-07-03
**Status:** Approved in discussion (this doc records it)

## Goal

Password-protect the device UI with a login page, keeping every trace of
authentication at the webserver transport edge. Command handlers,
`CommandManager`, and the stream contract never learn auth exists —
this is the return of auth promised in `docs/backlog/webserver-login.md`
after PIN auth was ripped out (`8dd4d27`).

## Core decision: sessions at the edge, tokens outside the streams

Authentication is a property of the *transport*, not the payload:

- **HTTP** requests carry `Authorization: Bearer <token>`; checked
  before the request body is ever read.
- **WebSocket** connections carry `?token=<token>` in the upgrade URL;
  checked once at upgrade time. A connection that upgrades is trusted
  for its lifetime — zero auth bytes in the message stream.

Commands assume the caller is already authenticated. No per-command
checks, ever (see backlog note for why the last attempt died).

## Firmware — everything lives in WebServerManager

### Setting

```cpp
inline static StringSetting webPassword_{ "web.password", "Web Password", "admin" };
```

Declared in `WebServerManager`, registered in its `Init()`. Password-only
— no username. Default `admin` means login is always on; a template
consumer changes it on the settings page like any other setting.

### Session table

Fixed RAM table, 4 slots (matches `MAX_WS_CLIENTS`):

```cpp
struct Session {
    char     token[33];      // 32 hex chars + NUL; empty slot = token[0] == 0
    int64_t  lastActivity;   // esp_timer_get_time()
};
```

- Token: 128 bits from `esp_random()`, hex-encoded.
- Idle timeout: 30 minutes since `lastActivity`. Expired slots are
  reclaimed lazily on lookup/login — no timer task.
- A 5th login evicts the stalest slot.
- Reboot clears everything (RAM only).
- Changing `web.password` clears the table (all sessions re-login).

The timeout is a garbage collector, not a UX rule: any authenticated
HTTP request or inbound WS frame refreshes `lastActivity`, and the
frontend's existing heartbeat means **an open tab never logs out** —
required for long-lived dashboards (e.g. a 4-hour graph). Sessions die
~30 minutes after their tab closes.

### Routes

| Route | Auth | Behavior |
|---|---|---|
| `GET /api/login` | open | Returns `{"name":"<device name>"}` for the login page's brand slot — everything else that knows the name is behind auth. |
| `POST /api/login` | open | Body `{"password":"..."}`. Match → `{"token":"..."}`. Mismatch → ~1 s delay, then 401 (brute-force damping). |
| `POST /api/command` | Bearer header | Missing/unknown/expired token → 401 before touching the body. |
| `/ws` | `?token=` query param | Checked during the upgrade GET; bad token → upgrade refused. Accepted connections are bound to their session; inbound frames refresh it. |
| static files | open | The login page ships inside the React bundle, so the SPA itself must be served unauthenticated. |

CORS preflight stays open (it carries no credentials by definition).

## Frontend

### LoginPage (`frontend/src/pages/LoginPage.tsx`)

Based on the shadcn **login-03** block (`npx shadcn@latest add login-03`):
centered card on a muted background with a brand slot above the form.
Scaffold it, then strip it down:

- brand slot shows the device name (fetched like KC1080 does, falls
  back to "Strux"),
- **keep**: one password field (`autocomplete="current-password"`),
  submit button, inline error line,
- **strip**: email/username field, social-login buttons ("Login with
  Google" etc.), sign-up and forgot-password links, terms footer.

No device-discovery panel (KC1080 feature, out of scope).

### backend.ts

- `login(password)` → `POST /api/login`; on success stores the token in
  `sessionStorage` (survives reload, dies with the tab) and flips an
  `authenticated` flag with an `onAuthChange` subscription for the UI.
- WS connect appends `?token=`; `/api/command` fetches add the Bearer
  header.
- A browser cannot see *why* a WS connect failed (refused upgrade and
  network failure look identical), so before each WS connect the token
  is validated with an authenticated `ping` over HTTP; a 401 there —
  or on any other HTTP call — clears the token and flips
  `authenticated` to false. **The password is never stored** — no
  credential-based auto-re-login (deliberate difference from KC1080).
  On reconnect the token either still works or the user sees the login
  page.

### App.tsx

Renders `LoginPage` whenever `authenticated` is false; the normal app
otherwise. A `useAuth`-style hook subscribes to the backend flag.

## Deliberately out of scope

- **Logout button** — close the tab; the session reaps itself. One
  command away if ever wanted.
- **Absolute session lifetime** — open tabs stay logged in by design.
- **"Change default password" nag** — `admin` default is a conscious
  template choice.
- **Device discovery on the login page** — KC1080 feature, not wanted.
- **HTTPS / secure cookies** — bearer token + query param works
  identically for `pnpm dev` (cross-origin) and on-device serving;
  cookies would not without HTTPS.

## Edge cases

- Session expires mid-use (only possible after a 30 min disconnect):
  next request 401s → login page. Nothing on-device to lose.
- Brief WS drop (roaming, reboot of AP): token still valid → silent
  reconnect, no login page flash.
- Device reboot: table is gone → everyone logs in again.
- Log streaming starts only once an authenticated WS is up — unchanged
  from today, where it starts when any WS is up.

## Verification

Build + flash. Manually: wrong password (delay + error), right password
(app loads), reload tab (still in via sessionStorage), second browser
(independent session), 5 concurrent logins (oldest evicted), password
change (both browsers kicked to login), `pnpm dev` against the device
(login works cross-origin), leave a console/graph tab open well past 30
minutes (never logs out).
