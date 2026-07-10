# WebServer / WebSocket Decomposition — Design

**Date:** 2026-07-10
**Status:** Approved in discussion (this doc records it)
**Context:** The code-quality refactor the session-transport roadmap flagged
("separate some things out of the webmanager") after login-over-WS (step 5)
landed. `WebSocketHandler` and `WebServerManager` have each accreted ~5
responsibilities; this breaks them into self-contained classes that each do one
job. Pure restructure — **no behavior change** except one deliberate, discussed
trim to the `hello` contract (below).

## Goal

De-god-class the two files by extracting focused, self-contained classes. No
runtime-model change: the transport stays **stateless-per-frame** on the httpd
task (a fresh `SessionMux` per frame; a streamed body drained synchronously
within one `HandleBinary` via `RecvChunk`). The persistent per-connection
*object graph* (a `Websocket→Auth→Mux→Stream` chain) is explicitly **not** built
here — that is step-6 territory, which may never happen; these classes are
justified on today's readability alone.

## Principle

Each new class is small, self-contained, and does one specific job with a
narrow interface — no framework-y mechanisms. Dependencies are threaded by
reference at construction.

## Class map

### `Authenticator` — the credential authority (new)

Everything password/token, lifted whole out of `WebServerManager` (the "foreign
body" the roadmap named). A plain class **owned by `WebServerManager`** — *not*
a `ServiceProvider` manager (one consumer today: the WS transport; promote later
only if a second transport ever needs it).

Owns: the `web.password` `StringSetting`, the `SessionTable`, the
`passwordSnapshot_` + epoch check.

```cpp
class Authenticator {
public:
    void Register(SettingsManager& settings);   // register web.password (called from WebServerManager::Init)

    bool AuthRequired();                 // web.password non-empty
    bool CheckPassword(const char* pw);  // epoch-check, then compare
    void MintKey(char* out);             // SessionTable::Create (out >= TOKEN_LEN)
    bool ValidateKey(const char* key);   // epoch-check, then SessionTable::Touch  (was ValidateToken)
    void TouchKey(const char* key);      // SessionTable::Touch, no epoch          (was TouchSession)
private:
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};
    Mutex authMutex_;
    void CheckPasswordEpoch();           // clears sessions_ on a changed password
};
```

After this, `WebServerManager` has **zero** password/token/SessionTable code.

### `WsConnection` — one live connection's state (new, header-only)

Replaces the five fd-indexed parallel arrays in `WebSocketHandler` with one
object per slot. A value/state object — **no I/O**.

```cpp
struct WsConnection {
    int      fd = 0;                     // 0 = empty slot
    bool     authed = false;
    char     key[SessionTable::TOKEN_LEN] = {};
    int64_t  connectedAt = 0;
    int      consecBinFails = 0;

    bool active() const { return fd != 0; }
    void authenticate(const char* k);    // authed = true; strlcpy key
    int64_t age(int64_t now) const { return now - connectedAt; }
    // send-failure bookkeeping stays here; the actual send is the transport's
};
```

### `ConnectionRegistry` — the fixed slot table (new)

The "4-slot table" logic, self-contained: allocation, lookup, removal, and the
pre-auth reaper. Guarded by its own mutex.

```cpp
class ConnectionRegistry {
public:
    WsConnection* find(int fd);                 // nullptr if absent
    WsConnection* add(int fd, bool authed, int64_t now);  // reaps a stale un-authed slot when full; nullptr if none free
    void remove(int fd);
    template<class F> void forEach(F&& fn);     // for broadcast (snapshot under lock)
private:
    static constexpr int MAX = 4;
    static constexpr int64_t PRE_AUTH_TIMEOUT_US = 10LL * 1000 * 1000;
    WsConnection conns_[MAX];
    Mutex mutex_;
};
```

### `AuthGate` — the handshake + gate decision (new)

The pre-auth handshake and the authed/not routing, lifted out of
`WebSocketHandler`. Depends on **only** `Authenticator&` (the name-drop below is
what lets it avoid `SystemManager`/`ServiceProvider`).

```cpp
class AuthGate {
public:
    explicit AuthGate(Authenticator& auth);
    enum class Disposition { Handled, PassToMux, Rejected };
    // Parses the first chunk's type. hello/login/auth → handled here (reply via
    // `link`, flip `conn.authed` on success); authed non-verb → PassToMux;
    // unauthed non-verb → reply REJECT, Rejected.
    Disposition Handle(WsConnection& conn, WsSessionLink& link,
                       uint16_t sid, const uint8_t* payload, size_t len);
private:
    Authenticator& auth_;
    // owns the reply framing (was SendReply/SendReplyN/SendReject on the handler)
};
```

`hello` reply is now `{"authRequired":true|false}` (see contract change);
`login` → `{"ok":true,"key":"…"}`/`{"ok":false}`; `auth` → `{"ok":true}`/`{"ok":false}`.

### `WebSocketHandler` — slimmed to transport + orchestration

Keeps: the httpd `/ws` route, `HandleWs` recv, the send mutex + the
`sessionFrame_`/`sessionInbound_` window buffers, broadcast, and mux dispatch.
Now *owns* a `ConnectionRegistry` and an `AuthGate`, and per inbound frame:
`registry.find(fd)` → `gate.Handle(...)` → on `PassToMux` build the `SessionMux`
and run `OnSessionOpened`→`CommandManager` (unchanged). Broadcast iterates the
registry and sends only to `authed` connections. Per-frame key liveness:
`auth.TouchKey(conn.key)` when authed (was `TouchClient`).

### `WebServerManager` — slimmed to the HTTP server

Keeps: httpd lifecycle, the route table (`/api/command`, CORS, static, `/ws`
registration), FAT mount, the ConsoleManager broadcast bridge. Now *owns* an
`Authenticator` and hands it to the transport by reference. `CheckBearer`/
`SendUnauthorized` stay (they guard `/api/command` until step 4) but call
through the `Authenticator` for the key check. `GetDeviceName` is **removed** —
its only caller was `hello`, which no longer returns the name.

## Contract change: `hello` drops the device name

`hello` returns **`{"authRequired":<bool>}`** — the device name is removed.
Rationale (both wins): it was a pre-auth information leak (a passer-by who opens
the socket learned the device name without authenticating), and it's what forces
`AuthGate` to reach `SystemManager`; dropping it makes `AuthGate` depend only on
`Authenticator`. Device *identity* belongs to the future connection-server
device list, not an unauthenticated login page.

**Frontend follow-through** (the only frontend change in this refactor):
- `backend.ts`: `doHandshake` reads only `authRequired` from `hello`; **remove
  `getLoginInfo()`** (its sole job was the name).
- `LoginPage.tsx`: brand slot becomes **static** — the fork's product name from
  `frontend/src/config.ts` (add a `PRODUCT_NAME` constant; forks already
  customize `config.ts`), or the icon + "Sign in". No pre-auth fetch.
- Post-auth is unchanged: the sidebar/tab title still gets the real device name
  from `getInfo`.

## Wiring (references at construction)

```
WebServerManager
 ├─ Authenticator     auth_          // owns; Register(settings) in Init
 ├─ WebSocketHandler  ws_            // owns; ws_.SetAuth(auth_) in Init
 └─ StaticFileHandler staticFiles_
WebSocketHandler
 ├─ ConnectionRegistry registry_     // owns
 └─ AuthGate           gate_{auth_}  // owns; constructed with Authenticator&
```

Inbound frame path:
`HandleWs → HandleBinary → registry_.find(fd) → gate_.Handle(conn, link, …)`
→ `{ Handled | PassToMux → SessionMux dispatch | Rejected }`.

## Files

- **New** (under `main/Application/WebServerManager/`): `Authenticator.{h,cpp}`,
  `WsConnection.h` (header-only), `ConnectionRegistry.{h,cpp}`, `AuthGate.{h,cpp}`.
- **Modified:** `WebSocketHandler.{h,cpp}` (shed registry/auth-gate/handshake),
  `WebServerManager.{h,cpp}` (shed credential authority).
- **CMake:** add the new `.cpp`s to `SOURCE_FILES_LIST` in
  `main/CMakeLists.txt` (sources are listed explicitly; the folder is already on
  the include path).
- **Frontend:** `backend.ts`, `LoginPage.tsx`, `config.ts` (add `PRODUCT_NAME`).

## Verification

Pure restructure ⇒ the login-over-WS behavior is the regression test. Build +
flash, then re-run the `auth-matrix` probe (empty-password authed-at-connect,
password-set login/reject/resume, broadcast gated to authed) — it must pass
**unchanged except** `hello` now returns `{authRequired}` with no `name` (adjust
that one assertion; the quoted-name `hello` test from the step-5 fix wave is
retired). Frontend: `pnpm typecheck` + `pnpm build`; a browser sanity check that
the login page renders its static brand and login still works.

## Out of scope (deliberately)

- **No runtime-model change** — still stateless-per-frame on the httpd task; no
  worker task, no persistent per-connection object chain (step 6, may never come).
- **`Authenticator` stays a plain class**, not a `ServiceProvider` manager.
- **No renames** beyond what extraction requires (`WebSocketHandler` keeps its
  name).
- `/api/command` + CORS removal is **step 4**, untouched here.
