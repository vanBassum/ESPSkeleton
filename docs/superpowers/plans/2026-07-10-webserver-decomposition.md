# WebServer / WebSocket Decomposition — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the two god classes (`WebSocketHandler`, `WebServerManager`) into small self-contained classes — `Authenticator`, `WsConnection`, `ConnectionRegistry`, `AuthGate` — with no runtime-model change.

**Architecture:** Pure restructure of the existing stateless-per-frame WS transport. The login-over-WS behavior is unchanged, so it is the regression test. One deliberate behavior trim: `hello` drops the device name (`{authRequired}` only).

**Design doc:** `docs/superpowers/specs/2026-07-10-webserver-decomposition-design.md` — read it first.

## Global Constraints

- **C++17**, no exceptions; `strlcpy` not `strcpy`; `snprintf`/`memcpy` with `sizeof`. Match existing style in the files touched. Managers/classes here take dependencies by reference at construction.
- **NO automated tests** (repo convention). Each task verifies by **build + flash + the `auth-matrix` WebSocket probe** (the regression test), which must pass unchanged after every task (except Task 1 adjusts the `hello` assertion).
- **Firmware build** (PowerShell, repo root): `Set-Location C:\Workspace\Strux; $env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build` → `Project build complete.` Flash: `idf.py -p COM3 flash`.
- **Frontend** (Task 1 only): `cd frontend && pnpm typecheck && pnpm build`.
- **Device notes:** reachable at `ws://192.168.50.111/ws`, EMPTY password. **Do NOT `ping`** (no ICMP; use a WS call to check reachability). **Do NOT `erase-flash`** (wipes WiFi → AP mode). If a probe leaves a password set, recover via WS: `login {password:"admin"}` → `setSetting web.password ""` → `saveSettings`. Run every command **foreground**; no background tasks.
- **Regression probe:** `node C:\Users\bas\AppData\Local\Temp\claude\c--Workspace-Strux\11a7f4d7-c4c7-4be3-99b9-bda529d42546\scratchpad\auth-matrix.mjs` (run from `C:\Workspace\Strux\frontend`; imports the shared, already-correct `auth-probe.mjs`). It exercises: empty-password authed-at-connect, password set/save, pre-auth reject, wrong/right password, `getLogs` after login, key resume + bad-key reject, and restores the empty password. It must pass after every task.
- **New `.cpp` files must be added to `SOURCE_FILES_LIST` in `main/CMakeLists.txt`** in the task that creates them (the folder is already on the include path; header-only files need no CMake change).
- **Commit trailer** (every commit): `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **No behavior change** other than Task 1's `hello` trim. If a step would change behavior, stop and report.

## File Structure

- `main/Application/WebServerManager/Authenticator.{h,cpp}` — NEW. Credential authority: `web.password` setting, `SessionTable`, epoch. One job: answer "is this password/key valid" and mint keys.
- `main/Application/WebServerManager/WsConnection.h` — NEW, header-only. One live connection's state (fd, authed, key, connectedAt, fails) + transitions. No I/O.
- `main/Application/WebServerManager/ConnectionRegistry.{h,cpp}` — NEW. The fixed 4-slot table: add/find/remove/reap/forEach. One job: own the connection set.
- `main/Application/WebServerManager/AuthGate.{h,cpp}` — NEW. The pre-auth handshake + authed/not routing decision. Depends only on `Authenticator&`.
- `main/Application/WebServerManager/WebSocketHandler.{h,cpp}` — MODIFIED. Shrinks to transport + registry + gate + mux-dispatch + broadcast.
- `main/Application/WebServerManager/WebServerManager.{h,cpp}` — MODIFIED. Shrinks to the HTTP server; owns an `Authenticator`.
- `main/CMakeLists.txt` — MODIFIED. Add the new `.cpp`s.
- `frontend/src/lib/backend.ts`, `frontend/src/pages/LoginPage.tsx`, `frontend/src/config.ts` — MODIFIED (Task 1 only).

---

### Task 1: `hello` drops the device name (do this first)

Doing this first removes `WebSocketHandler`'s only use of `GetDeviceName`, so the later extractions have no `SystemManager` dependency to thread.

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp` (the `hello` branch of `HandlePreAuth`)
- Modify: `main/Application/WebServerManager/WebServerManager.{h,cpp}` (remove now-dead `GetDeviceName`)
- Modify: `frontend/src/config.ts`, `frontend/src/lib/backend.ts`, `frontend/src/pages/LoginPage.tsx`

**Interfaces:**
- Produces: `hello` reply is now `{"authRequired":true|false}` (no `name`).

- [ ] **Step 1: Trim the `hello` reply (firmware)**

In `WebSocketHandler.cpp`, in `HandlePreAuth`'s `hello` branch, remove the device-name fetch and emit only `authRequired`. It currently builds `{"name":...,"authRequired":...}` via `JsonWriter`/`BufferStream` (or `SendReplyN`). Replace with a static reply:

```cpp
    if (strcmp(type, "hello") == 0)
    {
        SendReply(req, sid, auth_->AuthRequired()
                             ? "{\"authRequired\":true}"
                             : "{\"authRequired\":false}");
        return;
    }
```

If the `JsonWriter`/`BufferStream` includes added in the step-5 fix wave are now unused in `WebSocketHandler.cpp` (grep for `JsonWriter`/`BufferStream`), remove those two `#include`s. Keep `SendReplyN` (still used elsewhere? if not, leave it — it's a general helper the AuthGate task will reuse).

- [ ] **Step 2: Delete the now-dead `GetDeviceName` (firmware)**

`WebServerManager::GetDeviceName` had exactly one caller (the `hello` name). Remove its declaration from `WebServerManager.h` and its definition from `WebServerManager.cpp`.

- [ ] **Step 3: Add a static product brand (frontend)**

In `frontend/src/config.ts`, add:

```ts
/** Static brand shown on the login page (pre-auth). Post-auth the real device
 *  name comes from `getInfo`. Forks customize this. */
export const PRODUCT_NAME = "Strux"
```

- [ ] **Step 4: Login page uses the static brand; drop `getLoginInfo` (frontend)**

In `frontend/src/pages/LoginPage.tsx`: remove the `useEffect` that calls `backend.getLoginInfo()` and the `deviceName` state; render `PRODUCT_NAME` (import from `@/config`) in the brand slot; set `document.title = PRODUCT_NAME` once (or leave the title alone). In `frontend/src/lib/backend.ts`: delete the `getLoginInfo()` method (its only job was the name). `doHandshake` already reads only `authRequired` from `hello` — confirm it does not read `name`.

- [ ] **Step 5: Build firmware + frontend**

Firmware build (Global Constraints) → `Project build complete.` Then `cd frontend && pnpm typecheck && pnpm build` → clean (no remaining `getLoginInfo` reference).

- [ ] **Step 6: Flash + verify**

`idf.py -p COM3 flash`. Then a WS probe from `C:\Workspace\Strux\frontend`:

```bash
node --input-type=module -e "import {open,call} from './C:/Users/bas/AppData/Local/Temp/claude/c--Workspace-Strux/11a7f4d7-c4c7-4be3-99b9-bda529d42546/scratchpad/auth-probe.mjs'; const ws=await open(); const h=await call(ws,1,{type:'hello'}); console.log('hello:', h, 'has name?', 'name' in h); const g=await call(ws,2,{type:'getLogs'}); console.log('getLogs lines:', g.lines?.length); ws.close();"
```

Expected: `hello: { authRequired: false } has name? false` and a `getLogs lines: N` (empty-password path still works, name is gone).

- [ ] **Step 7: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.cpp main/Application/WebServerManager/WebServerManager.h main/Application/WebServerManager/WebServerManager.cpp frontend/src/config.ts frontend/src/lib/backend.ts frontend/src/pages/LoginPage.tsx
git commit -m "Decomposition prep: hello drops device name (authRequired only); static login brand"   # + trailer
```

---

### Task 2: Extract `Authenticator`

**Files:**
- Create: `main/Application/WebServerManager/Authenticator.h`, `Authenticator.cpp`
- Modify: `main/Application/WebServerManager/WebServerManager.{h,cpp}`, `WebSocketHandler.{h,cpp}`, `main/CMakeLists.txt`

**Interfaces:**
- Produces: `Authenticator` with `void Register(SettingsManager&)`, `bool AuthRequired()`, `bool CheckPassword(const char*)`, `void MintKey(char*)`, `bool ValidateKey(const char*)`, `void TouchKey(const char*)`.
- `WebSocketHandler::SetAuth` now takes `Authenticator&` (was `WebServerManager&`).

- [ ] **Step 1: Create `Authenticator.h`**

```cpp
#pragma once
#include "SessionTable.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>

class SettingsManager;

// The credential authority: owns web.password + the RAM session-key table +
// change detection. Transport-neutral; no connection state. Owned by
// WebServerManager (a plain class, not a ServiceProvider manager).
class Authenticator {
public:
    void Register(SettingsManager& settings);   // register web.password

    bool AuthRequired();                 // web.password non-empty
    bool CheckPassword(const char* pw);  // epoch-check, then compare
    void MintKey(char* out);             // SessionTable::Create (out >= SessionTable::TOKEN_LEN)
    bool ValidateKey(const char* key);   // epoch-check, then SessionTable::Touch
    void TouchKey(const char* key);      // SessionTable::Touch (refresh only)

private:
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};
    Mutex authMutex_;
    void CheckPasswordEpoch();           // clears sessions_ when web.password changed
};
```

- [ ] **Step 2: Create `Authenticator.cpp`**

Move the bodies verbatim from `WebServerManager.cpp` (they exist today as `CheckPasswordEpoch`, `AuthRequired`, `CheckPassword`, `MintKey`, `ValidateToken`, `TouchSession`), renaming `ValidateToken`→`ValidateKey` and `TouchSession`→`TouchKey`. `Register` takes the snapshot after registering:

```cpp
#include "Authenticator.h"
#include "SettingsManager.h"
#include <cstring>
#include <esp_log.h>

void Authenticator::Register(SettingsManager& settings)
{
    settings.Register({ &webPassword_ });
    webPassword_.Get(passwordSnapshot_, sizeof(passwordSnapshot_));
}

void Authenticator::CheckPasswordEpoch()
{
    LOCK(authMutex_);
    char current[64] = {};
    webPassword_.Get(current, sizeof(current));
    if (strcmp(current, passwordSnapshot_) != 0)
    {
        sessions_.Clear();
        strlcpy(passwordSnapshot_, current, sizeof(passwordSnapshot_));
    }
}

bool Authenticator::AuthRequired()
{
    char pw[64] = {};
    webPassword_.Get(pw, sizeof(pw));
    return pw[0] != '\0';
}

bool Authenticator::CheckPassword(const char* pw)
{
    CheckPasswordEpoch();
    char expected[64] = {};
    webPassword_.Get(expected, sizeof(expected));
    return strcmp(pw ? pw : "", expected) == 0;
}

void Authenticator::MintKey(char* out)      { sessions_.Create(out); }
bool Authenticator::ValidateKey(const char* key) { CheckPasswordEpoch(); return sessions_.Touch(key); }
void Authenticator::TouchKey(const char* key)    { sessions_.Touch(key); }
```

(Match the exact current bodies in `WebServerManager.cpp` for the epoch/get logic — the above mirrors them.)

- [ ] **Step 3: Gut the credential code out of `WebServerManager`**

In `WebServerManager.h`: remove the members `webPassword_`, `sessions_`, `passwordSnapshot_`, `authMutex_` and the methods `CheckPasswordEpoch`, `ValidateToken`, `TouchSession`, `AuthRequired`, `CheckPassword`, `MintKey`. Add `#include "Authenticator.h"` and a member `Authenticator auth_;`. Keep `CheckBearer` and `SendUnauthorized`.

In `WebServerManager.cpp`: delete the moved definitions. In `Init()`, replace the `settings.Register({ &webPassword_ }); webPassword_.Get(...)` lines with `auth_.Register(serviceProvider_.getSettingsManager());`. Change `wsHandler_.SetAuth(*this)` → `wsHandler_.SetAuth(auth_)`. Rewrite `CheckBearer` to validate via `auth_.ValidateKey(token)` instead of `ValidateToken`.

- [ ] **Step 4: Point `WebSocketHandler` at `Authenticator`**

In `WebSocketHandler.h`: change the forward decl `class WebServerManager;` → `class Authenticator;`, the member `WebServerManager* auth_` → `Authenticator* auth_`, and `SetAuth(WebServerManager&)` → `SetAuth(Authenticator&)`. In `WebSocketHandler.cpp`: include `Authenticator.h` (drop `WebServerManager.h` if now unused); rewire the calls — `auth_->CheckPassword`, `auth_->MintKey`, `auth_->AuthRequired` stay by name; `auth_->ValidateToken(...)` → `auth_->ValidateKey(...)`; `auth_->TouchSession(...)` → `auth_->TouchKey(...)`. (There is no `GetDeviceName` call anymore — removed in Task 1.)

- [ ] **Step 5: CMake**

Add `Authenticator.cpp` to `SOURCE_FILES_LIST` in `main/CMakeLists.txt` (alongside the other `WebServerManager/*.cpp`).

- [ ] **Step 6: Build + flash + regression**

Firmware build → `Project build complete.` `idf.py -p COM3 flash`. Run the `auth-matrix` probe (Global Constraints) → all expectations pass, empty password restored. (Behavior identical; this proves the extraction didn't change anything.)

- [ ] **Step 7: Commit**

```bash
git add main/Application/WebServerManager/Authenticator.h main/Application/WebServerManager/Authenticator.cpp main/Application/WebServerManager/WebServerManager.h main/Application/WebServerManager/WebServerManager.cpp main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp main/CMakeLists.txt
git commit -m "Decompose: extract Authenticator (credential authority) out of WebServerManager"   # + trailer
```

---

### Task 3: Extract `WsConnection` + `ConnectionRegistry`

**Files:**
- Create: `main/Application/WebServerManager/WsConnection.h` (header-only), `ConnectionRegistry.h`, `ConnectionRegistry.cpp`
- Modify: `main/Application/WebServerManager/WebSocketHandler.{h,cpp}`, `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `SessionTable::TOKEN_LEN`.
- Produces: `WsConnection` (fields `fd`, `authed`, `key[TOKEN_LEN]`, `connectedAt`, `consecBinFails`; methods `active()`, `authenticate(const char* key)`, `age(int64_t now)`); `ConnectionRegistry` with `WsConnection* find(int fd)`, `WsConnection* add(int fd, bool authed, int64_t now)`, `void remove(int fd)`, and `template<class F> void forEach(F&&)`.

- [ ] **Step 1: Create `WsConnection.h`**

```cpp
#pragma once
#include "SessionTable.h"
#include <cstdint>
#include <cstring>

// One live WebSocket connection's state. Value/state object — no I/O.
struct WsConnection {
    int      fd = 0;                                 // 0 = empty slot
    bool     authed = false;
    char     key[SessionTable::TOKEN_LEN] = {};      // session key once authed
    int64_t  connectedAt = 0;
    int      consecBinFails = 0;

    bool active() const { return fd != 0; }
    int64_t age(int64_t now) const { return now - connectedAt; }
    void authenticate(const char* k)
    {
        authed = true;
        strlcpy(key, k ? k : "", sizeof(key));
    }
    void reset()
    {
        fd = 0; authed = false; key[0] = 0; connectedAt = 0; consecBinFails = 0;
    }
};
```

- [ ] **Step 2: Create `ConnectionRegistry.h`**

```cpp
#pragma once
#include "WsConnection.h"
#include "Mutex.h"
#include <cstdint>

// The fixed slot table of live connections. Owns allocation, lookup, removal,
// and the pre-auth reaper. Thread-safe (its own mutex). Snapshot via forEach
// for broadcast (the caller sends outside any lock it holds).
class ConnectionRegistry {
    static constexpr const char* TAG = "ConnectionRegistry";
public:
    static constexpr int MAX = 4;
    static constexpr int64_t PRE_AUTH_TIMEOUT_US = 10LL * 1000 * 1000;

    WsConnection* find(int fd);
    // Allocates a slot for fd; when full, reaps a stale UN-authenticated slot.
    // Returns nullptr if no slot could be freed. `now` = esp_timer_get_time().
    WsConnection* add(int fd, bool authed, int64_t now);
    void remove(int fd);

    // Invoke fn(const WsConnection&) for each active slot, under the lock.
    template <class F> void forEach(F&& fn)
    {
        LOCK(mutex_);
        for (auto& c : conns_) if (c.active()) fn(c);
    }

private:
    WsConnection conns_[MAX];
    Mutex mutex_;
};
```

- [ ] **Step 3: Create `ConnectionRegistry.cpp`**

Port the current `AddWsClient`/`RemoveWsClient` logic (the reaper predicate `!authed && age > PRE_AUTH_TIMEOUT_US`) into the registry:

```cpp
#include "ConnectionRegistry.h"
#include <esp_log.h>

WsConnection* ConnectionRegistry::find(int fd)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) return &c;
    return nullptr;
}

WsConnection* ConnectionRegistry::add(int fd, bool authed, int64_t now)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) return &c;   // already present

    WsConnection* slot = nullptr;
    for (auto& c : conns_) if (!c.active()) { slot = &c; break; }
    if (!slot)   // full: reap a stale un-authenticated squatter
        for (auto& c : conns_)
            if (!c.authed && now - c.connectedAt > PRE_AUTH_TIMEOUT_US) { slot = &c; break; }
    if (!slot) { ESP_LOGW(TAG, "table full: fd=%d refused", fd); return nullptr; }

    slot->reset();
    slot->fd = fd;
    slot->authed = authed;
    slot->connectedAt = now;
    return slot;
}

void ConnectionRegistry::remove(int fd)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) { c.reset(); return; }
}
```

- [ ] **Step 4: Rewire `WebSocketHandler` to the registry**

In `WebSocketHandler.h`: remove the parallel arrays (`wsClients_`, `consecBinFails_`, `clientTokens_`, `clientAuthed_`, `clientConnectedAt_`, and the `MAX_WS_CLIENTS`/`MAX_BIN_FAILS`/`PRE_AUTH_TIMEOUT_US` constants that moved to the registry). Add `#include "ConnectionRegistry.h"` and a member `ConnectionRegistry registry_;`. Keep `AddWsClient`/`RemoveWsClient`/`IsAuthed`/`SetAuthed`/`TouchClient` as thin wrappers OR inline their uses — prefer inlining at call sites through `registry_`:
- `AddWsClient(fd)` → `registry_.add(fd, !auth_->AuthRequired(), esp_timer_get_time()) != nullptr`.
- `RemoveWsClient(fd)`/`OnClientDisconnected(fd)` → `registry_.remove(fd)`.
- `IsAuthed(fd)` → `auto* c = registry_.find(fd); return c && c->authed;`.
- `SetAuthed(fd,key)` → `if (auto* c = registry_.find(fd)) c->authenticate(key);`.
- `TouchClient(fd)` → `if (auto* c = registry_.find(fd); c && c->authed) auth_->TouchKey(c->key);`.

In `Broadcast`/`BroadcastBinary`: replace the array snapshot with `registry_.forEach([&](const WsConnection& c){ if (c.authed) targets.push_back(c.fd); });` (collect fds under the registry lock, then send outside it, preserving the existing send-mutex pattern). Keep the `consecBinFails`/`MAX_BIN_FAILS` giving-up behavior — store the counter on `WsConnection` and update via `registry_.find(fd)` after a failed send (a fixed local `MAX_BIN_FAILS` constant can live on `WebSocketHandler`).

- [ ] **Step 5: CMake**

Add `ConnectionRegistry.cpp` to `SOURCE_FILES_LIST` in `main/CMakeLists.txt`.

- [ ] **Step 6: Build + flash + regression**

Build → flash → `auth-matrix` passes unchanged. Extra check: broadcast gating still holds — the matrix's un-authed-socket broadcast check (or re-run the Task-4-of-login broadcast probe) shows 0 broadcasts to an un-authed socket.

- [ ] **Step 7: Commit**

```bash
git add main/Application/WebServerManager/WsConnection.h main/Application/WebServerManager/ConnectionRegistry.h main/Application/WebServerManager/ConnectionRegistry.cpp main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp main/CMakeLists.txt
git commit -m "Decompose: WsConnection + ConnectionRegistry replace the parallel fd arrays"   # + trailer
```

---

### Task 4: Extract `AuthGate` + slim `HandleBinary`; finish

**Files:**
- Create: `main/Application/WebServerManager/AuthGate.h`, `AuthGate.cpp`
- Modify: `main/Application/WebServerManager/WebSocketHandler.{h,cpp}`, `main/CMakeLists.txt`, `docs/session-transport-roadmap.md`

**Interfaces:**
- Consumes: `Authenticator&`, `WsConnection&`, `WsSessionLink&`, `session::*`, `ExtractJsonString`.
- Produces: `AuthGate` with `enum class Disposition { Handled, PassToMux, Rejected }` and `Disposition Handle(WsConnection&, WsSessionLink&, uint16_t sid, const uint8_t* payload, size_t len)`.

- [ ] **Step 1: Create `AuthGate.h`**

```cpp
#pragma once
#include <esp_http_server.h>
#include <cstdint>
#include <cstddef>
#include "WsSessionLink.h"

class Authenticator;
struct WsConnection;

// The pre-auth handshake + authed/not routing decision, self-contained.
// Depends only on the Authenticator. Owns the reply framing.
class AuthGate {
public:
    explicit AuthGate(Authenticator& auth) : auth_(auth) {}

    enum class Disposition { Handled, PassToMux, Rejected };

    // Parse the first chunk's `type`. hello/login/auth → handled here (reply via
    // `link`, flip conn.authed on success), returns Handled. An authed non-verb
    // → PassToMux. An unauthenticated non-verb → REJECT reply, Rejected.
    Disposition Handle(WsConnection& conn, WsSessionLink& link,
                       uint16_t sid, const uint8_t* payload, size_t len);

private:
    Authenticator& auth_;
    void SendReply(WsSessionLink& link, uint16_t sid, const char* json);
    void SendReject(WsSessionLink& link, uint16_t sid, const char* reason);
};
```

- [ ] **Step 2: Create `AuthGate.cpp`**

Move `HandlePreAuth`'s body (the type-peek + hello/login/auth) and the `SendReply`/`SendReject` framing out of `WebSocketHandler.cpp`. `SendReply`/`SendReject` frame `[sid|FINAL|payload]` / `[sid|REJECT|reason]` and `link.SendRaw(...)`. `hello` emits `{"authRequired":...}` (post-Task-1). `login`→`CheckPassword`+`MintKey`+`conn.authenticate(key)`; `auth`→`ValidateKey`+`conn.authenticate(key)`. Include `Authenticator.h`, `WsConnection.h`, `SessionProtocol.h`, `JsonHelpers.h`, `<cstring>`, `<algorithm>`, `<cstdio>`.

```cpp
#include "AuthGate.h"
#include "Authenticator.h"
#include "WsConnection.h"
#include "SessionProtocol.h"
#include "JsonHelpers.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

void AuthGate::SendReply(WsSessionLink& link, uint16_t sid, const char* json)
{
    uint8_t buf[session::HEADER_LEN + 160];
    size_t n = strlen(json);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_FINAL);
    memcpy(buf + session::HEADER_LEN, json, n);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

void AuthGate::SendReject(WsSessionLink& link, uint16_t sid, const char* reason)
{
    uint8_t buf[session::HEADER_LEN + 32];
    size_t n = strlen(reason);
    if (n > sizeof(buf) - session::HEADER_LEN) n = sizeof(buf) - session::HEADER_LEN;
    session::writeHeader(buf, sid, session::FLAG_REJECT);
    memcpy(buf + session::HEADER_LEN, reason, n);
    link.SendRaw(buf, session::HEADER_LEN + n);
}

AuthGate::Disposition AuthGate::Handle(WsConnection& conn, WsSessionLink& link,
                                       uint16_t sid, const uint8_t* payload, size_t len)
{
    char line[160];
    size_t n = std::min(len, sizeof(line) - 1);
    memcpy(line, payload, n);
    line[n] = '\0';
    if (char* nl = strchr(line, '\n')) *nl = '\0';

    char type[16] = {};
    ExtractJsonString(line, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0)
    {
        SendReply(link, sid, auth_.AuthRequired() ? "{\"authRequired\":true}"
                                                  : "{\"authRequired\":false}");
        return Disposition::Handled;
    }
    if (strcmp(type, "login") == 0)
    {
        char pw[64] = {};
        ExtractJsonString(line, "password", pw, sizeof(pw));
        if (auth_.CheckPassword(pw))
        {
            char key[SessionTable::TOKEN_LEN] = {};
            auth_.MintKey(key);
            conn.authenticate(key);
            char reply[64];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"key\":\"%s\"}", key);
            SendReply(link, sid, reply);
        }
        else SendReply(link, sid, "{\"ok\":false}");
        return Disposition::Handled;
    }
    if (strcmp(type, "auth") == 0)
    {
        char key[SessionTable::TOKEN_LEN] = {};
        ExtractJsonString(line, "key", key, sizeof(key));
        if (auth_.ValidateKey(key)) { conn.authenticate(key); SendReply(link, sid, "{\"ok\":true}"); }
        else SendReply(link, sid, "{\"ok\":false}");
        return Disposition::Handled;
    }

    if (conn.authed) return Disposition::PassToMux;
    SendReject(link, sid, "unauthorized");
    return Disposition::Rejected;
}
```

- [ ] **Step 3: Slim `HandleBinary`**

In `WebSocketHandler.h`: remove `HandlePreAuth`, `SetAuthed`, `SendReply`, `SendReplyN`, `SendReject` declarations; add `#include "AuthGate.h"` and a member `AuthGate gate_{ /* auth_ */ };` — since `auth_` is a pointer set later via `SetAuth`, construct `gate_` lazily or store an `AuthGate*`/`std::optional`. Simplest: keep `Authenticator* auth_` and build the gate inline per frame (`AuthGate gate(*auth_);` — it's cheap, holds only a reference) OR store `AuthGate` and initialize it in `SetAuth`. **Recommended:** in `SetAuth(Authenticator& a)` set `auth_ = &a;` and `gate_.emplace(a)` if using `std::optional<AuthGate>`, or just construct `AuthGate gate(*auth_)` locally inside `HandleBinary` (no member). Pick the local-construction form to avoid lifetime juggling.

In `WebSocketHandler.cpp`, rewrite `HandleBinary`:

```cpp
void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;
    int fd = httpd_req_to_sockfd(req);

    WsConnection* conn = registry_.find(fd);
    if (!conn) return;   // unknown fd (closed mid-frame)

    WsSessionLink link(req, sendMutex_);
    AuthGate gate(*auth_);
    switch (gate.Handle(*conn, link, sid, payload, plen))
    {
        case AuthGate::Disposition::PassToMux:
        {
            SessionMux mux(link, *this, sessionFrame_, SESSION_WINDOW,
                           sessionInbound_, sizeof(sessionInbound_));
            mux.OnChunk(sid, flags, payload, plen);
            break;
        }
        case AuthGate::Disposition::Handled:
        case AuthGate::Disposition::Rejected:
            break;
    }
}
```

Delete the now-moved `HandlePreAuth`/`SendReply`/`SendReplyN`/`SendReject` definitions from `WebSocketHandler.cpp`. (If `SendReplyN` is now unused anywhere, remove it; if the JsonWriter/BufferStream includes are unused, remove them.)

- [ ] **Step 4: CMake + roadmap note**

Add `AuthGate.cpp` to `SOURCE_FILES_LIST`. In `docs/session-transport-roadmap.md`, update the "Dont forget to do a code quality refactor" line to record it done (point to the design/plan docs and note WebServerManager/WebSocketHandler were decomposed into Authenticator/WsConnection/ConnectionRegistry/AuthGate).

- [ ] **Step 5: Build + flash + regression**

Build → flash → `auth-matrix` passes unchanged (empty-pw, login/reject/resume, broadcast gating). Confirm `WebSocketHandler.cpp` and `WebServerManager.cpp` are now materially smaller (the handshake/credential code is gone).

- [ ] **Step 6: Commit**

```bash
git add main/Application/WebServerManager/AuthGate.h main/Application/WebServerManager/AuthGate.cpp main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp main/CMakeLists.txt docs/session-transport-roadmap.md
git commit -m "Decompose: extract AuthGate; HandleBinary is now find-connection + gate + dispatch"   # + trailer
git push
```

---

## Self-Review

**Spec coverage:** `Authenticator` → Task 2. `WsConnection` + `ConnectionRegistry` → Task 3. `AuthGate` → Task 4. `hello` name-drop + static login brand + `getLoginInfo` removal + dead `GetDeviceName` → Task 1. WebSocketHandler slimmed → Tasks 3–4. WebServerManager slimmed (owns Authenticator, keeps CheckBearer via ValidateKey) → Task 2. CMake per new-.cpp task. Wiring by reference → Tasks 2 (SetAuth(Authenticator&)) and 4 (gate holds Authenticator&). Roadmap note → Task 4. No runtime-model change, Authenticator not a manager, no `/api/command` touch — respected throughout.

**Placeholder scan:** New-file classes have full code; existing-file edits give precise member/method move lists and the rewritten `HandleBinary`. No TBDs.

**Type consistency:** `ValidateToken`→`ValidateKey`, `TouchSession`→`TouchKey` renames applied consistently (Authenticator def + WebSocketHandler/CheckBearer call sites, Task 2). `WsConnection` field/method names (`authed`, `key`, `authenticate`, `age`, `active`, `reset`) used consistently in ConnectionRegistry (Task 3) and AuthGate/HandleBinary (Task 4). `AuthGate::Disposition` values match between header (Task 4.1) and `HandleBinary` switch (Task 4.3). `SetAuth(Authenticator&)` signature (Task 2.4) matches `WebServerManager::Init`'s `SetAuth(auth_)` (Task 2.3).
