# Login over the WebSocket — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move authentication onto the one WebSocket — a per-connection `authed` bit set by an in-band `login`/`auth` handshake — so `/api/login` and the token-in-URL go away and HTTP can later serve static only.

**Architecture:** A connection-level auth gate sits in the WS transport, *in front of* the `SessionMux`. A connection opens unauthenticated; while unauthenticated its chunks are handled by the gate (the `hello`/`login`/`auth` handshake), never the mux or `CommandManager`; once its `authed` bit is set, chunks pass straight through. `login` mints a session key (the existing `SessionTable` token) presented once per connection; the client stores it and replays it as `auth{key}` on reconnect. Empty `web.password` disables auth and is the new default.

**Tech Stack:** ESP-IDF v6.0, C++17 (firmware); React 19 + TypeScript + Vite (frontend); the session-mux transport shipped in steps 1–3.

**Design doc:** `docs/superpowers/specs/2026-07-10-login-over-websocket-design.md` — read it first.

## Global Constraints

- **C++17**, no exceptions/RTTI-heavy patterns. `snprintf` with `sizeof` bounds; no `strcpy`/`strcat` (use `strlcpy`). JSON parsed with `ExtractJsonString` (`main/lib/json/JsonHelpers.h`, signature `bool ExtractJsonString(const char* json, const char* field, char* out, size_t outSize)`) or built with `snprintf`.
- **No automated tests** (repo convention). Each task is verified by **build** plus, where behavior is observable, a **WebSocket probe** (Node 20 global `fetch`/`WebSocket`, run from `frontend/`) or `pnpm` build. This replaces the usual TDD test cycle.
- **Firmware build** (PowerShell, from repo root): `Set-Location C:\Workspace\Strux; $env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build` — success marker `Project build complete.` Flash: `idf.py -p COM3 flash`. Device at `192.168.50.111`.
- **Frontend**: `cd frontend && pnpm typecheck && pnpm build` (build gzips into `../www`).
- **Session key** = the existing `SessionTable` token (128 bits `esp_random`, 32 hex chars + NUL; `SessionTable::TOKEN_LEN == 33`).
- **Pre-auth vocabulary** (only these run on an unauthenticated connection): `hello` → `{"name":"<device>","authRequired":<bool>}`; `login{password}` → `{"ok":true,"key":"…"}` / `{"ok":false}`; `auth{key}` → `{"ok":true}` / `{"ok":false}`.
- **Scope boundary:** `/api/command` + its `Bearer` guard + CORS stay (they are **step 4**). This step removes only `/api/login` and the `?token=` upgrade param, and stops the frontend using HTTP.
- **Commit trailer** on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## File Structure

- `main/Application/SystemManager/SystemManager.h` — `GetDeviceName` already exists; unchanged (referenced).
- `main/Application/WebServerManager/WebServerManager.{h,cpp}` — default password → `""`; add the reusable credential surface (`AuthRequired`/`CheckPassword`/`MintKey`/`GetDeviceName`); delete the HTTP login routes. Keeps `SessionTable`, `ValidateToken`, `CheckBearer`, `CheckPasswordEpoch`, `/api/command`, CORS.
- `main/Application/WebServerManager/WebSocketHandler.{h,cpp}` — the new auth gate: per-fd `authed`/`connectedAt` state, unauthenticated upgrade + pre-auth reaper, the `hello`/`login`/`auth` handshake, and broadcast gating.
- `main/Application/WebServerManager/SessionTable.{h,cpp}` — unchanged (reused).
- `frontend/src/lib/backend.ts` — connect unauthenticated; in-band `hello`/`auth`/`login`; store the key; heartbeat only once authed; drop all HTTP calls.
- `frontend/src/hooks/use-auth.ts` — hold the login page back until the first handshake resolves (covers empty-password with no stored token).
- `frontend/src/pages/LoginPage.tsx` — unchanged (`getLoginInfo()` keeps its `{name}` shape).

---

### Task 1: WebServerManager — credential surface + empty-password default

**Files:**
- Modify: `main/Application/WebServerManager/WebServerManager.h`
- Modify: `main/Application/WebServerManager/WebServerManager.cpp`

**Interfaces:**
- Consumes: `SystemManager::GetDeviceName(char*, size_t)`; `SessionTable::Create(char*)`; existing `WebServerManager::CheckPasswordEpoch()`, `webPassword_`.
- Produces (used by Task 2/3): `bool WebServerManager::AuthRequired()`, `bool WebServerManager::CheckPassword(const char* pw)`, `void WebServerManager::MintKey(char* out)` (out ≥ `SessionTable::TOKEN_LEN`), `void WebServerManager::GetDeviceName(char* out, size_t maxLen)`. Existing `bool ValidateToken(const char*)` is the key-resume check.

- [ ] **Step 1: Change the default password to empty**

In `WebServerManager.h`, change the setting default:

```cpp
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };
```

- [ ] **Step 2: Declare the credential surface**

In `WebServerManager.h`, in the `public:` section under the existing auth comment (near `ValidateToken`/`TouchSession`), add:

```cpp
    /// True when a password is set (auth on). Empty web.password ⇒ open.
    bool AuthRequired();
    /// Password-epoch check, then compare `pw` to web.password.
    bool CheckPassword(const char* pw);
    /// Mint a session key into `out` (must hold SessionTable::TOKEN_LEN bytes).
    void MintKey(char* out);
    /// Device name for the pre-auth `hello`.
    void GetDeviceName(char* out, size_t maxLen);
```

- [ ] **Step 3: Implement them**

In `WebServerManager.cpp`, add after `ValidateToken` (SystemManager.h and SessionTable are already included transitively; `strcmp` needs `<cstring>`, already used in the file):

```cpp
bool WebServerManager::AuthRequired()
{
    char pw[64] = {};
    webPassword_.Get(pw, sizeof(pw));
    return pw[0] != '\0';
}

bool WebServerManager::CheckPassword(const char* pw)
{
    CheckPasswordEpoch();
    char expected[64] = {};
    webPassword_.Get(expected, sizeof(expected));
    return strcmp(pw ? pw : "", expected) == 0;
}

void WebServerManager::MintKey(char* out)
{
    sessions_.Create(out);
}

void WebServerManager::GetDeviceName(char* out, size_t maxLen)
{
    serviceProvider_.getSystemManager().GetDeviceName(out, maxLen);
}
```

- [ ] **Step 4: Build**

Run: `Set-Location C:\Workspace\Strux; $env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
Expected: `Project build complete.` (No behavior change yet — the gate consumes these next. Note the running device now defaults to an empty password only after a fresh NVS; an existing device keeps its stored `web.password`.)

- [ ] **Step 5: Commit**

```bash
git add main/Application/WebServerManager/WebServerManager.h main/Application/WebServerManager/WebServerManager.cpp
git commit -m "Login-over-WS: WebServerManager credential surface + empty-password default

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: WebSocketHandler — open unauthenticated + per-connection authed bit + reaper

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.h`
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp`
- Test (probe): `C:\Users\bas\AppData\Local\Temp\claude\c--Workspace-Strux\11a7f4d7-c4c7-4be3-99b9-bda529d42546\scratchpad\auth-probe.mjs`

**Interfaces:**
- Consumes: `WebServerManager::AuthRequired()` (Task 1).
- Produces (used by Task 3/4): per-fd arrays `clientAuthed_`, `clientConnectedAt_`; `bool IsAuthed(int fd)`; `AddWsClient(int fd)` (token param dropped).

- [ ] **Step 1: Replace per-fd state and the AddWsClient signature in the header**

In `WebSocketHandler.h`: keep `clientTokens_` (now populated in-band on login/auth, and still refreshed by `TouchClient`). Add the authed bit + connect timestamp + reaper deadline, and change `AddWsClient`. Replace the `clientTokens_` declaration block and the `AddWsClient` declaration with:

```cpp
    // Per-connection auth state (replaces the ?token= upgrade check). authed is
    // set by the in-band login/auth handshake (see HandlePreAuth), or at connect
    // when web.password is empty. clientTokens_ holds the session key once authed,
    // so TouchClient can keep it alive in the SessionTable for reconnect-resume.
    char    clientTokens_[MAX_WS_CLIENTS][SessionTable::TOKEN_LEN] = {};
    bool    clientAuthed_[MAX_WS_CLIENTS] = {};
    int64_t clientConnectedAt_[MAX_WS_CLIENTS] = {};
    static constexpr int64_t PRE_AUTH_TIMEOUT_US = 10LL * 1000 * 1000;   // reap idle un-authed sockets

    void TouchClient(int fd);

    /// True if the connection on `fd` is authenticated.
    bool IsAuthed(int fd);

    /// False when the client table is full (after reaping stale un-authed slots).
    bool AddWsClient(int fd);
    void RemoveWsClient(int fd);
```

(Delete the old `char clientTokens_[...]` line, the old `void TouchClient(int fd);`, and the old `bool AddWsClient(int fd, const char* token);` — they are replaced above.)

- [ ] **Step 2: Add the esp_timer include**

In `WebSocketHandler.cpp`, add near the other includes:

```cpp
#include "esp_timer.h"
```

- [ ] **Step 3: Rewrite AddWsClient (unauthenticated + reaper)**

In `WebSocketHandler.cpp`, replace the whole `AddWsClient` body with:

```cpp
bool WebSocketHandler::AddWsClient(int fd)
{
    LOCK(wsMutex_);

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd) return true;

    int64_t now = esp_timer_get_time();

    int slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == 0) { slot = i; break; }

    if (slot < 0)   // full: reap a stale UN-authenticated squatter to make room
    {
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (!clientAuthed_[i] && now - clientConnectedAt_[i] > PRE_AUTH_TIMEOUT_US)
            { slot = i; break; }
    }
    if (slot < 0) { ESP_LOGW(TAG, "WS client rejected (table full): fd=%d", fd); return false; }

    wsClients_[slot]        = fd;
    clientAuthed_[slot]     = !auth_->AuthRequired();   // empty password ⇒ authed at connect
    clientConnectedAt_[slot] = now;
    clientTokens_[slot][0]  = 0;
    consecBinFails_[slot]   = 0;
    ESP_LOGI(TAG, "WS client added: fd=%d slot=%d authed=%d", fd, slot, (int)clientAuthed_[slot]);
    return true;
}
```

- [ ] **Step 4: Clear the new state in RemoveWsClient**

In `WebSocketHandler.cpp`, inside `RemoveWsClient`'s matching-slot block, add alongside the existing resets:

```cpp
            wsClients_[i] = 0;
            consecBinFails_[i] = 0;
            clientTokens_[i][0] = 0;
            clientAuthed_[i] = false;
            clientConnectedAt_[i] = 0;
```

- [ ] **Step 5: Add IsAuthed**

In `WebSocketHandler.cpp`, add near `TouchClient`:

```cpp
bool WebSocketHandler::IsAuthed(int fd)
{
    LOCK(wsMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd) return clientAuthed_[i];
    return false;
}
```

- [ ] **Step 6: Open the upgrade unauthenticated**

In `WebSocketHandler.cpp`, replace the `if (req->method == HTTP_GET) { ... }` block in `HandleWs` with:

```cpp
    if (req->method == HTTP_GET)
    {
        // The WS now opens UNAUTHENTICATED — auth is an in-band handshake
        // (see HandlePreAuth). esp_http_server has already sent the 101; we
        // only need a client slot. A full table (after reaping stale un-authed
        // sockets) refuses the upgrade so the client hits its reconnect loop.
        if (!self->AddWsClient(httpd_req_to_sockfd(req)))
            return ESP_FAIL;
        return ESP_OK;
    }
```

- [ ] **Step 7: Gate inbound chunks on the authed bit**

In `WebSocketHandler.cpp`, replace the body of `HandleBinary` with (Task 3 fills in the not-authed branch — for now it silently drops, which only affects password-set devices, not the empty-password default):

```cpp
void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;

    int fd = httpd_req_to_sockfd(req);
    if (IsAuthed(fd))
    {
        WsSessionLink link(req, sendMutex_);
        SessionMux mux(link, *this, sessionFrame_, SESSION_WINDOW,
                       sessionInbound_, sizeof(sessionInbound_));
        mux.OnChunk(sid, flags, payload, plen);
        return;
    }
    // Unauthenticated: handled by the pre-auth gate (Task 3). Until then, drop.
    (void)sid; (void)flags;
}
```

- [ ] **Step 8: Build**

Run the firmware build (Global Constraints). Expected: `Project build complete.`

- [ ] **Step 9: Flash**

Run: `idf.py -p COM3 flash`
Expected: flash completes; device boots (empty password on a fresh NVS, or its stored password otherwise — for this task test on an empty-password device; if the device has a stored password, clear it first or expect the authed path to be exercised in Task 3).

- [ ] **Step 10: Probe — an empty-password connection is authed at connect**

Create `…/scratchpad/auth-probe.mjs`:

```javascript
const HOST = process.env.HOST ?? "192.168.50.111"
const enc = (o) => new TextEncoder().encode(JSON.stringify(o) + "\n")
function chunk(sid, flags, bytes) {
  const f = new Uint8Array(3 + bytes.length)
  f[0] = sid & 0xff; f[1] = (sid >> 8) & 0xff; f[2] = flags; f.set(bytes, 3)
  return f
}
export function open() {
  const ws = new WebSocket(`ws://${HOST}/ws`)
  ws.binaryType = "arraybuffer"
  return new Promise((res, rej) => { ws.onopen = () => res(ws); ws.onerror = () => rej(new Error("ws error")) })
}
export function call(ws, sid, obj) {                     // one FINAL request → JSON reply (or REJECT)
  return new Promise((resolve, reject) => {
    const on = (ev) => {
      const v = new Uint8Array(ev.data); const s = v[0] | (v[1] << 8); const fl = v[2]
      if (s !== sid) return
      ws.removeEventListener("message", on)
      const text = new TextDecoder().decode(v.subarray(3))
      if (fl & 0x02) return reject(new Error("REJECT: " + text))
      resolve(text ? JSON.parse(text) : {})
    }
    ws.addEventListener("message", on)
    ws.send(chunk(sid, 0x01, enc(obj)))
  })
}

if (import.meta.url === `file://${process.argv[1].replace(/\\/g, "/")}`) {
  const ws = await open()
  console.log("hello:", await call(ws, 1, { type: "hello" }))
  console.log("getLogs (should succeed on empty-pw):", (await call(ws, 2, { type: "getLogs" })).lines?.length, "lines")
  ws.close()
}
```

Run: `cd /c/Workspace/Strux/frontend && node "C:/Users/bas/AppData/Local/Temp/claude/c--Workspace-Strux/11a7f4d7-c4c7-4be3-99b9-bda529d42546/scratchpad/auth-probe.mjs"`
Expected: `hello: { name: '…', authRequired: false }` and `getLogs … N lines` — i.e. no `?token=`, no login, commands work immediately.

- [ ] **Step 11: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp
git commit -m "Login-over-WS: open WS unauthenticated + per-connection authed bit + pre-auth reaper

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: WebSocketHandler — the hello/login/auth handshake (and delete /api/login)

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.h`
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp`
- Modify: `main/Application/WebServerManager/WebServerManager.cpp`
- Modify: `main/Application/WebServerManager/WebServerManager.h`
- Test (probe): reuse `…/scratchpad/auth-probe.mjs`

**Interfaces:**
- Consumes: `WebServerManager::{CheckPassword,MintKey,ValidateToken,GetDeviceName,AuthRequired}`; `IsAuthed`, `clientAuthed_`, `clientTokens_` (Tasks 1–2); `session::{writeHeader,HEADER_LEN,FLAG_FINAL,FLAG_REJECT}`; `ExtractJsonString`.
- Produces: `void HandlePreAuth(httpd_req_t*, int fd, uint16_t sid, const uint8_t* payload, size_t len)`; `void SendReply(httpd_req_t*, uint16_t sid, const char* json)`; `void SetAuthed(int fd, const char* key)`.

- [ ] **Step 1: Declare the handshake helpers**

In `WebSocketHandler.h`, below `OnSessionOpened`, add:

```cpp
    // Pre-auth handshake for an unauthenticated connection: hello / login / auth,
    // else a REJECT. Runs entirely in the transport — the mux and CommandManager
    // never see it.
    void HandlePreAuth(httpd_req_t* req, int fd, uint16_t sid,
                       const uint8_t* payload, size_t len);
    void SetAuthed(int fd, const char* key);
    void SendReply(httpd_req_t* req, uint16_t sid, const char* json);
```

- [ ] **Step 2: Add cstdio for snprintf**

In `WebSocketHandler.cpp`, add to the includes if not present:

```cpp
#include <cstdio>
```

- [ ] **Step 3: Route unauthenticated chunks to the gate**

In `WebSocketHandler.cpp`, replace the `// Unauthenticated: … drop.` tail of `HandleBinary` with:

```cpp
    // Unauthenticated: the pre-auth handshake owns this connection until it
    // authenticates. The chunk never reaches the mux or CommandManager.
    HandlePreAuth(req, fd, sid, payload, plen);
```

- [ ] **Step 4: Implement SetAuthed, SendReply, HandlePreAuth**

In `WebSocketHandler.cpp`, add:

```cpp
void WebSocketHandler::SetAuthed(int fd, const char* key)
{
    LOCK(wsMutex_);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsClients_[i] == fd)
        {
            clientAuthed_[i] = true;
            strlcpy(clientTokens_[i], key ? key : "", sizeof(clientTokens_[i]));
            return;
        }
}

void WebSocketHandler::SendReply(httpd_req_t* req, uint16_t sid, const char* json)
{
    uint8_t buf[session::HEADER_LEN + 160];
    size_t n = strlen(json);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, n);
    WsSessionLink link(req, sendMutex_);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

void WebSocketHandler::HandlePreAuth(httpd_req_t* req, int fd, uint16_t sid,
                                     const uint8_t* payload, size_t len)
{
    // The request is a single small chunk: a header line {"type":...}\n, no body.
    char line[160];
    size_t n = std::min(len, sizeof(line) - 1);
    memcpy(line, payload, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[16] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0)
    {
        char name[33] = {};
        auth_->GetDeviceName(name, sizeof(name));
        char reply[96];
        snprintf(reply, sizeof(reply), "{\"name\":\"%s\",\"authRequired\":%s}",
                 name, auth_->AuthRequired() ? "true" : "false");
        SendReply(req, sid, reply);
        return;
    }
    if (strcmp(type, "login") == 0)
    {
        char pw[64] = {};
        ExtractJsonString(line, "password", pw, sizeof(pw));
        if (auth_->CheckPassword(pw))
        {
            char key[SessionTable::TOKEN_LEN] = {};
            auth_->MintKey(key);
            SetAuthed(fd, key);
            char reply[64];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"key\":\"%s\"}", key);
            SendReply(req, sid, reply);
        }
        else SendReply(req, sid, "{\"ok\":false}");
        return;
    }
    if (strcmp(type, "auth") == 0)
    {
        char key[SessionTable::TOKEN_LEN] = {};
        ExtractJsonString(line, "key", key, sizeof(key));
        if (auth_->ValidateToken(key))
        {
            SetAuthed(fd, key);
            SendReply(req, sid, "{\"ok\":true}");
        }
        else SendReply(req, sid, "{\"ok\":false}");
        return;
    }

    // Not a handshake message on an unauthenticated connection → transport refusal.
    uint8_t buf[session::HEADER_LEN + 16];
    const char* reason = "unauthorized";
    size_t rn = strlen(reason);
    session::writeHeader(buf, sid, session::FLAG_REJECT);
    memcpy(buf + session::HEADER_LEN, reason, rn);
    WsSessionLink link(req, sendMutex_);
    link.SendRaw(buf, session::HEADER_LEN + rn);
}
```

- [ ] **Step 5: Delete the HTTP login routes**

In `WebServerManager.cpp`: remove the three `login_get` / `login_post` / `login_opts` `httpd_uri_t` blocks and their `httpd_register_uri_handler` calls from `RegisterRoutes`, and delete the `HandleLoginGet` and `HandleLoginPost` function definitions. Keep `HandleApiCommand`, `HandleCorsPreflight`, `SetCorsHeaders`, `SendUnauthorized`, `CheckBearer`, `ValidateToken`, `TouchSession`, `CheckPasswordEpoch`. In `WebServerManager.h`, delete the `HandleLoginGet` and `HandleLoginPost` declarations.

- [ ] **Step 6: Build**

Run the firmware build. Expected: `Project build complete.` (No unused-function warnings for the removed login handlers.)

- [ ] **Step 7: Flash**

Run: `idf.py -p COM3 flash`

- [ ] **Step 8: Probe — full auth matrix**

Append a second entry-point script `…/scratchpad/auth-matrix.mjs`:

```javascript
import { open, call } from "./auth-probe.mjs"

// Starts empty-password (authed at connect). Set a password, then exercise login/auth.
let ws = await open()
console.log("1 hello:", await call(ws, 1, { type: "hello" }))                 // authRequired:false
await call(ws, 2, { type: "setSetting", key: "web.password", value: "admin" })
await call(ws, 3, { type: "saveSettings" })
ws.close()

ws = await open()
console.log("2 hello (pw set):", await call(ws, 1, { type: "hello" }))         // authRequired:true
try { await call(ws, 2, { type: "getLogs" }); console.log("BUG: getLogs allowed pre-auth") }
catch (e) { console.log("2 getLogs pre-auth:", e.message) }                    // REJECT: unauthorized
console.log("3 wrong pw:", await call(ws, 3, { type: "login", password: "nope" }))   // {ok:false}
const good = await call(ws, 4, { type: "login", password: "admin" })
console.log("4 right pw:", good)                                               // {ok:true, key}
console.log("5 getLogs after login:", (await call(ws, 5, { type: "getLogs" })).lines?.length, "lines")
ws.close()

ws = await open()
console.log("6 resume:", await call(ws, 1, { type: "auth", key: good.key }))   // {ok:true}
console.log("7 bad resume:", await call(ws, 2, { type: "auth", key: "deadbeef" })) // {ok:false}
ws.close()

// Restore empty password for later tasks.
ws = await open()
await call(ws, 1, { type: "auth", key: good.key })
await call(ws, 2, { type: "setSetting", key: "web.password", value: "" })
await call(ws, 3, { type: "saveSettings" })
ws.close()
console.log("restored empty password")
```

Run: `cd /c/Workspace/Strux/frontend && node "C:/Users/bas/AppData/Local/Temp/claude/c--Workspace-Strux/11a7f4d7-c4c7-4be3-99b9-bda529d42546/scratchpad/auth-matrix.mjs"`
Expected: hello flips to `authRequired:true` after the password is set; `getLogs` pre-auth is `REJECT: unauthorized`; wrong password `{ok:false}`; right password returns a `key`; `getLogs` then works; `auth` with that key resumes `{ok:true}`; a bad key `{ok:false}`; empty password restored.

- [ ] **Step 9: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp main/Application/WebServerManager/WebServerManager.h main/Application/WebServerManager/WebServerManager.cpp
git commit -m "Login-over-WS: in-band hello/login/auth handshake; delete /api/login routes

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: WebSocketHandler — gate log broadcasts to authenticated connections

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp`
- Test (probe): inline node one-liner

**Interfaces:**
- Consumes: `clientAuthed_`, `wsClients_` (Task 2).

- [ ] **Step 1: Filter Broadcast to authed clients**

In `WebSocketHandler.cpp`, in `Broadcast`, extend the snapshot and the send guard:

```cpp
    int clients[MAX_WS_CLIENTS];
    bool authed[MAX_WS_CLIENTS];

    {
        LOCK(wsMutex_);
        memcpy(clients, wsClients_, sizeof(clients));
        memcpy(authed, clientAuthed_, sizeof(authed));
    }
```

and change the send loop condition from `if (clients[i] != 0)` to `if (clients[i] != 0 && authed[i])`.

- [ ] **Step 2: Filter BroadcastBinary to authed clients**

In `WebSocketHandler.cpp`, in `BroadcastBinary`, add the same `authed[]` snapshot and change `if (clients[i] == 0) continue;` to `if (clients[i] == 0 || !authed[i]) continue;`.

- [ ] **Step 3: Build + flash**

Run the firmware build (`Project build complete.`), then `idf.py -p COM3 flash`.

- [ ] **Step 4: Probe — an unauthenticated connection receives no broadcasts**

With a password set (so an unauthenticated connection is possible), open a socket, do NOT log in, and confirm no session-0 frames arrive over ~3 s while the device logs:

Run: `cd /c/Workspace/Strux/frontend && node --input-type=module -e "import {open,call} from './C:/Users/bas/AppData/Local/Temp/claude/c--Workspace-Strux/11a7f4d7-c4c7-4be3-99b9-bda529d42546/scratchpad/auth-probe.mjs'; const ws=await open(); await call(ws,1,{type:'setSetting',key:'web.password',value:'admin'}); await call(ws,2,{type:'saveSettings'}); ws.close(); const u=await open(); let got=0; u.addEventListener('message',(e)=>{const v=new Uint8Array(e.data); if((v[0]|(v[1]<<8))===0) got++;}); await new Promise(r=>setTimeout(r,3000)); console.log('broadcasts on un-authed socket:',got,'(expect 0)'); u.close(); const a=await open(); await call(a,1,{type:'login',password:'admin'}).then(r=>call(a,2,{type:'setSetting',key:'web.password',value:''})); await call(a,3,{type:'saveSettings'}); a.close();"`
Expected: `broadcasts on un-authed socket: 0 (expect 0)`. (Log lines still reach an authed connection — verified end-to-end in Task 6 via the live console.)

- [ ] **Step 5: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.cpp
git commit -m "Login-over-WS: broadcast log lines only to authenticated connections

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Frontend — connect unauthenticated, in-band handshake, drop HTTP

**Files:**
- Modify: `frontend/src/lib/backend.ts`
- Modify: `frontend/src/hooks/use-auth.ts`

**Interfaces:**
- Consumes: the device gate (`hello`/`login`/`auth`) from Tasks 2–3; the existing `send()`/`enqueue`/`awaitReply` path.
- Produces: `backend.login(password): Promise<boolean>`, `backend.getLoginInfo(): Promise<{name}>`, `backend.authenticated`, `backend.onAuthChange` — same shapes `LoginPage`/`App`/`useAuth` already consume.

- [ ] **Step 1: Make setAuthenticated always notify**

In `backend.ts`, replace `setAuthenticated` so the hook is told even when the value is unchanged (needed so the login page resolves on an already-`false` determination):

```ts
  private setAuthenticated(auth: boolean) {
    this._authenticated = auth
    this.authHandlers.forEach((fn) => fn(auth))
  }
```

- [ ] **Step 2: Rewrite doConnect (open unauthenticated, no token/ping)**

In `backend.ts`, replace the entire `doConnect` method with:

```ts
  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    if (this.connecting) return this.connecting

    this.setStatus("connecting")

    const p = new Promise<void>((resolve, reject) => {
      const host = import.meta.env.DEV ? DEV_HOST : location.host
      const proto = location.protocol === "https:" ? "wss:" : "ws:"
      const ws = new WebSocket(`${proto}//${host}/ws`)
      ws.binaryType = "arraybuffer"
      let opened = false

      ws.onopen = () => {
        opened = true
        this.ws = ws
        this.setStatus("connected")
        void this.doHandshake()   // establishes auth in-band; sets authenticated
        resolve()
      }

      ws.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          this.onBinaryChunk(ev.data)
          return
        }
        if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
          ev.data.arrayBuffer().then((buf) => this.onBinaryChunk(buf))
        }
      }

      ws.onclose = () => {
        this.ws = null
        this.stopHeartbeat()
        this.setStatus("disconnected")
        // Keep `authenticated` as-is across a brief drop — the reconnect's
        // auth{key} either resumes silently or fails (then doHandshake shows login).
        for (const [, req] of this.pending) {
          clearTimeout(req.timer)
          req.reject(new Error("WebSocket closed"))
        }
        this.pending.clear()
        if (!opened) reject(new Error("Connection failed"))
        this.reconnectTimer = setTimeout(() => {
          this.doConnect().catch(() => {})
        }, 2000)
      }

      ws.onerror = () => ws.close()
    })

    this.connecting = p
    p.catch(() => {}).then(() => {
      if (this.connecting === p) this.connecting = null
    })
    return p
  }
```

- [ ] **Step 3: Add the handshake**

In `backend.ts`, add a method (next to `doConnect`):

```ts
  // Establish auth in-band right after the socket opens: hello tells us whether
  // auth is required; if so, resume with the stored key or fall back to the
  // login page. Runs on every (re)connect.
  private async doHandshake() {
    try {
      const info = await this.send<{ name: string; authRequired: boolean }>("hello")
      if (!info.authRequired) {
        this.setAuthenticated(true)
        this.startHeartbeat()
        return
      }
      if (this.token) {
        const res = await this.send<{ ok: boolean }>("auth", { key: this.token })
        if (res.ok) {
          this.setAuthenticated(true)
          this.startHeartbeat()
          return
        }
        this.token = null
        sessionStorage.removeItem(TOKEN_KEY)
      }
      this.setAuthenticated(false)   // needs login
    } catch {
      /* socket died mid-handshake — onclose handles the reconnect */
    }
  }
```

- [ ] **Step 4: Rewrite login and getLoginInfo (in-band)**

In `backend.ts`, replace `login` and `getLoginInfo` with:

```ts
  /** Open endpoint: device name for the login page's brand slot (pre-auth `hello`). */
  async getLoginInfo(): Promise<{ name: string }> {
    const info = await this.send<{ name: string; authRequired: boolean }>("hello")
    return { name: info.name }
  }

  /** Returns false on wrong password; throws on connection failure. On success
   *  stores the session key and marks the connection authenticated. */
  async login(password: string): Promise<boolean> {
    await this.ensureConnected()
    const res = await this.send<{ ok: boolean; key?: string }>("login", { password })
    if (!res.ok) return false
    this.token = res.key ?? null
    if (this.token) sessionStorage.setItem(TOKEN_KEY, this.token)
    this.setAuthenticated(true)
    this.startHeartbeat()
    return true
  }
```

- [ ] **Step 5: Remove the dead HTTP plumbing**

In `backend.ts`, delete the now-unused `clearAuth`, `apiUrl`, and `commandUrl` methods. `getInfo`/`getLogs`/etc. are unchanged. (`authHeaders` was already removed in step 3 of the roadmap.) Confirm `DEV_HOST` is still imported (used in `doConnect`).

- [ ] **Step 6: Hold the login page back until the handshake resolves**

Replace `frontend/src/hooks/use-auth.ts` with:

```ts
import { useState, useEffect } from "react"
import { backend } from "@/lib/backend"

export function useAuth() {
  const [authenticated, setAuthenticated] = useState(backend.authenticated)
  // Hold the login page back until the first handshake resolves — covers the
  // empty-password case (we become authed with no stored token) so there's no
  // login-page flash. Bounded so a dead device still falls through to login.
  const [checking, setChecking] = useState(!backend.authenticated)

  useEffect(() => {
    const unsub = backend.onAuthChange((auth) => {
      setAuthenticated(auth)
      setChecking(false)
    })

    setAuthenticated(backend.authenticated)
    if (backend.authenticated) setChecking(false)

    const timer = setTimeout(() => setChecking(false), 3000)
    return () => {
      unsub()
      clearTimeout(timer)
    }
  }, [])

  return { authenticated, checking }
}
```

- [ ] **Step 7: Typecheck + build**

Run: `cd /c/Workspace/Strux/frontend && pnpm typecheck && pnpm build`
Expected: typecheck clean; `vite build` succeeds and gzips into `../www` (no references to the removed `apiUrl`/`commandUrl`/`clearAuth`).

- [ ] **Step 8: Commit**

```bash
git add frontend/src/lib/backend.ts frontend/src/hooks/use-auth.ts
git commit -m "Login-over-WS: frontend connects unauthenticated, in-band handshake, no HTTP

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: End-to-end on device + docs

**Files:**
- Modify: `docs/session-transport-roadmap.md`
- Modify: `docs/backlog/2026-07-09-login-over-websocket.md`
- Modify: `docs/superpowers/specs/2026-07-10-login-over-websocket-design.md` (status line)

**Interfaces:** none (verification + docs).

- [ ] **Step 1: Full build + flash (firmware embeds the rebuilt frontend)**

Run the firmware build (it rebuilds the frontend into `www`), then flash:
`Set-Location C:\Workspace\Strux; $env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build; idf.py -p COM3 flash`
Expected: `Project build complete.`, flash OK.

- [ ] **Step 2: Browser matrix (empty password — the default)**

Open `http://192.168.50.111/` in a browser. Expected: no login page; the app loads directly; the Console page streams live log lines; commands (Firmware page partitions, Settings) work.

- [ ] **Step 3: Browser matrix (password set)**

On the Settings page set `web.password` to `admin`, save, then reload. Expected: login page shows the device name; wrong password → "Invalid password"; `admin` → app loads. Reload the tab → still in (key in `sessionStorage`, silent `auth`). Open a second browser → independent login. Leave the Console open > the heartbeat interval → never bounced. Reboot the device (Firmware → reboot, or power-cycle) → the tab drops to the login page (RAM key table gone). `pnpm dev` (`cd frontend && pnpm dev`) against the device → login + commands work cross-origin.

- [ ] **Step 4: Restore the default for a clean template**

On the Settings page clear `web.password` (empty) and save, so the template ships open by default. Reload → no login page.

- [ ] **Step 5: Update the roadmap**

In `docs/session-transport-roadmap.md`, mark step 5 DONE with a one-line verification note, and update step 4's dependency line (login no longer blocks it — only `/api/command` removal remains). Add a "Step 5 — as built" section noting: connection-level authed bit + in-band `hello`/`login`/`auth`; `SessionTable` kept for resume; empty-password default; `/api/login`, `?token=`, and the HTTP `ping` removed; `/api/command`+CORS deferred to step 4.

- [ ] **Step 6: Update the backlog + design status**

In `docs/backlog/2026-07-09-login-over-websocket.md`, flip the status to DONE with the commit/branch. In the design doc, change `Status:` to record it as implemented.

- [ ] **Step 7: Commit + push**

```bash
git add docs/
git commit -m "Login-over-WS (step 5): verified on device; roadmap/backlog/design updated

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push
```

---

## Self-Review

**Spec coverage:**
- Connection-level authed bit → Task 2. Gate in front of mux (mux/commands auth-blind) → Task 2 (`HandleBinary` fork) + Task 3 (`HandlePreAuth` never calls the mux). Session key once per connection → Task 3 (`MintKey`/`SetAuthed`) + Task 5 (`auth{key}` resume). Empty-password default + auth-disabled → Task 1 (default) + Task 2 (`authed = !AuthRequired()`). `SessionTable` kept → reused throughout. Pre-auth vocabulary `hello`/`login`/`auth` → Task 3. Broadcasts gated to authed → Task 4. Pre-auth reaper → Task 2. `/api/login` removed → Task 3; `?token=` + HTTP `ping` removed → Task 2 (upgrade) + Task 5 (frontend). `/api/command`/CORS deferred → untouched (Global Constraints). Reconnect/reload/reboot/password-change behaviors → Task 5 (`doHandshake`, keep-auth-across-drop) + device `ValidateToken`/`CheckPasswordEpoch`. Deferred items (refresh tokens, layered client, AuthManager) → not built, per design.
- Password-change-doesn't-kick-live-connections: preserved implicitly — nothing force-clears `clientAuthed_`; `CheckPasswordEpoch` only clears the `SessionTable`, so live connections keep their bit and only fail to *resume* after a reconnect. No task needed.

**Placeholder scan:** No TBD/TODO; every code step shows complete code; probe scripts are runnable.

**Type consistency:** `AuthRequired`/`CheckPassword`/`MintKey`/`GetDeviceName`/`ValidateToken` are declared in Task 1 and called with matching signatures in Tasks 2–3. `IsAuthed`/`AddWsClient(int)`/`clientAuthed_`/`clientConnectedAt_`/`clientTokens_` declared in Task 2, used in Tasks 3–4. `SetAuthed`/`SendReply`/`HandlePreAuth` declared and defined in Task 3. Frontend `login`/`getLoginInfo`/`onAuthChange`/`authenticated` keep the shapes `LoginPage`/`App`/`useAuth` already consume. Wire reply shapes (`{name,authRequired}`, `{ok,key}`, `{ok}`) match between device (Task 3) and client (Task 5).
