# Webserver Login Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Password-protect the device UI with a login page; all auth lives at the webserver transport edge — commands and streams never see it.

**Architecture:** `POST /api/login` (password → 128-bit RAM token) feeds a 4-slot `SessionTable` in `WebServerManager`. HTTP requests to `/api/command` carry `Authorization: Bearer`; the WebSocket carries `?token=` validated once at upgrade. The React app gates on an `authenticated` flag in `backend.ts` and shows a stripped shadcn login-03 page. Spec: `docs/superpowers/specs/2026-07-03-webserver-login-design.md`.

**Tech Stack:** ESP-IDF v6.0 (`esp_http_server`, `esp_random`, `esp_timer`), C++17, React 19 + TypeScript + shadcn/ui, pnpm.

## Global Constraints

- No automated tests in this repo — every firmware task gates on `idf.py build` (exit 0), every frontend task on `pnpm typecheck` (exit 0). Final task is manual on-device verification.
- Firmware builds require an ESP-IDF v6.0 environment (`idf.py` on PATH). Frontend commands run in `frontend/`.
- C++17, no exceptions; `snprintf`/`strlcpy` with `sizeof` bounds, never `strcpy`/`strcat`.
- JSON via `lib/json` (`JsonWriter`, `JsonReader`) — no external JSON lib.
- New sources must be added to `SOURCE_FILES_LIST` in `main/CMakeLists.txt` (explicit list, no globbing).
- Auth checking must never appear in any command handler or in `CommandManager` — transport edge only.
- Commit after every task (repo convention: commit and push).

---

### Task 1: SessionTable

**Files:**
- Create: `main/Application/WebServerManager/SessionTable.h`
- Create: `main/Application/WebServerManager/SessionTable.cpp`
- Modify: `main/CMakeLists.txt` (add `SessionTable.cpp` to `SOURCE_FILES_LIST`)

**Interfaces:**
- Consumes: `lib/rtos` `Mutex` + `LOCK` (ContextLock.h), `esp_random()`, `esp_timer_get_time()`.
- Produces (used by Task 2/3): `SessionTable::Create(char* tokenOut)`, `bool SessionTable::Touch(const char* token)`, `SessionTable::Clear()`, constants `SessionTable::TOKEN_LEN` (33) and `SessionTable::MAX_SESSIONS` (4).

- [ ] **Step 1: Write SessionTable.h**

```cpp
#pragma once

#include "Mutex.h"
#include <cstdint>
#include <cstddef>

// ──────────────────────────────────────────────────────────────
// Fixed-slot bearer-token session table — pure token bookkeeping.
// Password checking and password-change detection live in
// WebServerManager (the owner of the setting); this class only
// mints, refreshes, and expires opaque tokens.
//
// The idle timeout is a garbage collector, not a UX rule: any WS
// frame or authenticated HTTP request refreshes a session, so an
// open browser tab (with its heartbeat) never expires. Sessions die
// ~30 minutes after their tab closes.
// ──────────────────────────────────────────────────────────────
class SessionTable {
public:
    static constexpr int     MAX_SESSIONS    = 4;    // matches MAX_WS_CLIENTS
    static constexpr size_t  TOKEN_LEN       = 33;   // 32 hex chars + NUL
    static constexpr int64_t IDLE_TIMEOUT_US = 30LL * 60 * 1000 * 1000;

    /// Mint a new session (128 bits of esp_random, hex-encoded) and
    /// write its token into `tokenOut` (must hold TOKEN_LEN bytes).
    /// Prefers an empty/expired slot; evicts the stalest live one if full.
    void Create(char* tokenOut);

    /// True if `token` names a live session; refreshes its activity.
    /// Expired entries are reclaimed lazily here — no timer task.
    bool Touch(const char* token);

    /// Drop all sessions (reboot does this implicitly — RAM only).
    void Clear();

private:
    struct Session {
        char    token[TOKEN_LEN] = {};   // token[0] == 0 → empty slot
        int64_t lastActivity = 0;
    };
    Session sessions_[MAX_SESSIONS];
    Mutex mutex_;
};
```

- [ ] **Step 2: Write SessionTable.cpp**

```cpp
#include "SessionTable.h"
#include "ContextLock.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>

void SessionTable::Create(char* tokenOut)
{
    uint32_t r[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
    snprintf(tokenOut, TOKEN_LEN, "%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(r[0]), static_cast<unsigned long>(r[1]),
             static_cast<unsigned long>(r[2]), static_cast<unsigned long>(r[3]));

    int64_t now = esp_timer_get_time();

    LOCK(mutex_);
    Session* slot = nullptr;
    for (auto& s : sessions_)   // prefer an empty or expired slot
    {
        if (s.token[0] == 0 || now - s.lastActivity > IDLE_TIMEOUT_US)
        {
            slot = &s;
            break;
        }
    }
    if (!slot)                  // all live: evict the stalest
    {
        slot = &sessions_[0];
        for (auto& s : sessions_)
            if (s.lastActivity < slot->lastActivity) slot = &s;
    }
    strlcpy(slot->token, tokenOut, TOKEN_LEN);
    slot->lastActivity = now;
}

bool SessionTable::Touch(const char* token)
{
    if (!token || token[0] == 0) return false;
    int64_t now = esp_timer_get_time();

    LOCK(mutex_);
    for (auto& s : sessions_)
    {
        if (s.token[0] == 0 || strcmp(s.token, token) != 0) continue;
        if (now - s.lastActivity > IDLE_TIMEOUT_US)   // lazy expiry
        {
            s.token[0] = 0;
            return false;
        }
        s.lastActivity = now;
        return true;
    }
    return false;
}

void SessionTable::Clear()
{
    LOCK(mutex_);
    for (auto& s : sessions_)
    {
        s.token[0] = 0;
        s.lastActivity = 0;
    }
}
```

- [ ] **Step 3: Register the source file**

In `main/CMakeLists.txt`, inside `set(SOURCE_FILES_LIST ...)`, after the line `"Application/WebServerManager/WebSocketHandler.cpp"`, add:

```cmake
    "Application/WebServerManager/SessionTable.cpp"
```

(`Application/WebServerManager` is already in `INCLUDE_DIRS_LIST` — no include-path change.)

- [ ] **Step 4: Build**

Run: `idf.py build`
Expected: exit 0, `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add main/Application/WebServerManager/SessionTable.h main/Application/WebServerManager/SessionTable.cpp main/CMakeLists.txt
git commit -m "Add SessionTable: fixed-slot bearer-token sessions with lazy expiry"
git push
```

---

### Task 2: WebServerManager auth edge — setting, login routes, /api/command guard

**Files:**
- Modify: `main/Application/WebServerManager/WebServerManager.h`
- Modify: `main/Application/WebServerManager/WebServerManager.cpp`

**Interfaces:**
- Consumes: Task 1's `SessionTable`; `StringSetting` (`TypedSettings.h`); `SettingsManager::Register`; `SystemManager::GetDeviceName(char*, size_t)`; `JsonReader<N>(Stream&)` + `GetString`; `JsonWriter` (`beginObject/field/endObject`); `BufferStream(char*, size_t)`; the existing `HttpRequestStream` in WebServerManager.cpp's anonymous namespace.
- Produces (used by Task 3): `bool WebServerManager::ValidateToken(const char* token)` (password-change check + touch; for upgrade-time checks) and `void WebServerManager::TouchSession(const char* token)` (touch only, no NVS read; for per-frame refresh).

- [ ] **Step 1: Extend WebServerManager.h**

Add includes at the top (after the existing ones):

```cpp
#include "SessionTable.h"
#include "TypedSettings.h"
#include "Mutex.h"
```

In the `public:` section, after `void BroadcastBinary(...)`:

```cpp
    // ── Transport-edge auth (also used by WebSocketHandler) ──
    /// Full check: detects a changed web.password (clears all sessions),
    /// then validates + refreshes the token. Reads NVS — use at request/
    /// upgrade granularity, not per WS frame.
    bool ValidateToken(const char* token);
    /// Refresh only — no settings read. Safe per WS frame. A token that
    /// no longer exists is a silent no-op (live connections stay trusted
    /// for their lifetime; see spec).
    void TouchSession(const char* token);
```

In the `private:` section, after `WebSocketHandler wsHandler_;`:

```cpp
    // ── Auth state ────────────────────────────────────────────
    inline static StringSetting webPassword_{ "web.password", "Web Password", "admin" };
    SessionTable sessions_;
    char passwordSnapshot_[64] = {};   // last-seen password; mismatch → sessions cleared
    Mutex authMutex_;                  // guards passwordSnapshot_

    /// Compare web.password against the snapshot; on change, clear all
    /// sessions and take a new snapshot. The "hook" for password edits —
    /// SettingsManager has no change notification, so we detect lazily
    /// in every HTTP auth path (login, /api/command, WS upgrade).
    void CheckPasswordEpoch();
    bool CheckBearer(httpd_req_t* req);

    static esp_err_t HandleLoginGet(httpd_req_t* req);
    static esp_err_t HandleLoginPost(httpd_req_t* req);
    static void SendUnauthorized(httpd_req_t* req);
```

- [ ] **Step 2: Implement in WebServerManager.cpp**

Add includes after the existing ones:

```cpp
#include "SystemManager.h"
#include "SettingsManager.h"
#include "JsonReader.h"
#include "JsonWriter.h"
#include "BufferStream.h"
#include "ContextLock.h"
```

In `Init()`, add after `wsHandler_.SetCommandManager(...)`:

```cpp
    serviceProvider_.getSettingsManager().Register({ &webPassword_ });
    webPassword_.Get(passwordSnapshot_, sizeof(passwordSnapshot_));
    wsHandler_.SetAuth(*this);
```

In `RegisterRoutes()`, before the `wsHandler_.RegisterRoute(server_);` line, add:

```cpp
    // Login — the only open API surface (see spec: everything else that
    // knows anything sits behind auth).
    const httpd_uri_t login_get = {
        .uri = "/api/login",
        .method = HTTP_GET,
        .handler = HandleLoginGet,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &login_get);

    const httpd_uri_t login_post = {
        .uri = "/api/login",
        .method = HTTP_POST,
        .handler = HandleLoginPost,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &login_post);

    const httpd_uri_t login_opts = {
        .uri = "/api/login",
        .method = HTTP_OPTIONS,
        .handler = HandleCorsPreflight,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &login_opts);
```

Update `SetCorsHeaders` so cross-origin dev can send the Bearer header and call GET:

```cpp
void WebServerManager::SetCorsHeaders(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}
```

At the top of `HandleApiCommand`, immediately after `auto* self = ...;`, add the guard:

```cpp
    if (!self->CheckBearer(req))
    {
        SendUnauthorized(req);
        return ESP_FAIL;
    }
```

Append the new functions at the end of the file:

```cpp
// ──────────────────────────────────────────────────────────────
// Auth — sessions at the transport edge. Nothing below this layer
// (commands, streams) ever sees a token or password.
// ──────────────────────────────────────────────────────────────

void WebServerManager::CheckPasswordEpoch()
{
    LOCK(authMutex_);
    char current[64] = {};
    webPassword_.Get(current, sizeof(current));
    if (strcmp(current, passwordSnapshot_) != 0)
    {
        ESP_LOGI(TAG, "web.password changed — clearing all sessions");
        sessions_.Clear();
        strlcpy(passwordSnapshot_, current, sizeof(passwordSnapshot_));
    }
}

bool WebServerManager::ValidateToken(const char* token)
{
    CheckPasswordEpoch();
    return sessions_.Touch(token);
}

void WebServerManager::TouchSession(const char* token)
{
    sessions_.Touch(token);
}

bool WebServerManager::CheckBearer(httpd_req_t* req)
{
    char hdr[48] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Bearer ", 7) != 0)
        return false;
    return ValidateToken(hdr + 7);
}

void WebServerManager::SendUnauthorized(httpd_req_t* req)
{
    // Manual 401 (esp_http_server's httpd_err_code_t has no 401) with
    // CORS headers so cross-origin JS can read the status.
    SetCorsHeaders(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}

esp_err_t WebServerManager::HandleLoginGet(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);

    char name[33] = {};
    self->serviceProvider_.getSystemManager().GetDeviceName(name, sizeof(name));

    char body[80];
    BufferStream out(body, sizeof(body));
    JsonWriter json(out);
    json.beginObject();
    json.field("name", name);
    json.endObject();

    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out.data(), out.length());
    return ESP_OK;
}

esp_err_t WebServerManager::HandleLoginPost(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);
    self->CheckPasswordEpoch();

    HttpRequestStream in(req);
    JsonReader<256> json(in);
    char password[64] = {};
    json.GetString("password", password, sizeof(password));

    char expected[64] = {};
    webPassword_.Get(expected, sizeof(expected));

    if (strcmp(password, expected) != 0)
    {
        // No delay, no lockout — deliberately (spec): this layer keeps
        // out the pleps, it is not a security boundary.
        SendUnauthorized(req);
        return ESP_OK;
    }

    char token[SessionTable::TOKEN_LEN] = {};
    self->sessions_.Create(token);

    char body[64];
    int n = snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);

    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}
```

Note: `webPassword_` is `inline static`, so the static handlers may read it directly; `sessions_`/`CheckPasswordEpoch` go through `self`.

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: FAILS with `'class WebSocketHandler' has no member named 'SetAuth'` — that member arrives in Task 3. If you are executing tasks strictly in order, comment nothing out; instead verify the error is exactly the missing `SetAuth` and proceed to Task 3 (the two tasks share one build gate). If the build fails for any OTHER reason, fix it now.

- [ ] **Step 4: Commit (deferred)**

Do not commit yet — Task 3 completes the firmware edge; commit there.

---

### Task 3: WebSocketHandler — token at upgrade, refresh per frame

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.h`
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp`

**Interfaces:**
- Consumes: Task 2's `WebServerManager::ValidateToken(const char*)` / `TouchSession(const char*)`; `SessionTable::TOKEN_LEN`.
- Produces: `void WebSocketHandler::SetAuth(WebServerManager&)` (called from `WebServerManager::Init()` — already wired in Task 2).

- [ ] **Step 1: Extend WebSocketHandler.h**

Add after `class CommandManager;`:

```cpp
class WebServerManager;
```

Add include (for `SessionTable::TOKEN_LEN`):

```cpp
#include "SessionTable.h"
```

In `public:`, after `SetCommandManager`:

```cpp
    void SetAuth(WebServerManager& auth);
```

In `private:`, after `CommandManager* commandManager_ = nullptr;`:

```cpp
    WebServerManager* auth_ = nullptr;
```

After `int consecBinFails_[MAX_WS_CLIENTS] = {};` add the per-connection token copies and a helper:

```cpp
    // Session token bound to each connection at upgrade time. Frames
    // refresh the session by token; if the session was meanwhile
    // evicted/cleared, the refresh is a no-op — the live connection
    // stays trusted for its lifetime (spec), it just can't reconnect.
    char clientTokens_[MAX_WS_CLIENTS][SessionTable::TOKEN_LEN] = {};

    void TouchClient(int fd);
```

Change the `AddWsClient` declaration to carry the token:

```cpp
    void AddWsClient(int fd, const char* token);
```

- [ ] **Step 2: Implement in WebSocketHandler.cpp**

Add include:

```cpp
#include "WebServerManager.h"
```

Add the setter next to `SetCommandManager`:

```cpp
void WebSocketHandler::SetAuth(WebServerManager& auth)
{
    auth_ = &auth;
}
```

Replace `AddWsClient` with the token-carrying version (same logic, plus token copy):

```cpp
void WebSocketHandler::AddWsClient(int fd, const char* token)
{
    LOCK(wsMutex_);

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (wsClients_[i] == fd) return;
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (wsClients_[i] == 0)
        {
            wsClients_[i] = fd;
            strlcpy(clientTokens_[i], token, sizeof(clientTokens_[i]));
            ESP_LOGI(TAG, "WS client added: fd=%d slot=%d", fd, i);
            return;
        }
    }
    ESP_LOGW(TAG, "WS client rejected (max reached): fd=%d", fd);
}
```

In `RemoveWsClient`, inside the `if (wsClients_[i] == fd)` block, add alongside the existing resets:

```cpp
            clientTokens_[i][0] = 0;
```

Add `TouchClient` after `RemoveWsClient`:

```cpp
void WebSocketHandler::TouchClient(int fd)
{
    char token[SessionTable::TOKEN_LEN] = {};
    {
        LOCK(wsMutex_);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
        {
            if (wsClients_[i] == fd)
            {
                strlcpy(token, clientTokens_[i], sizeof(token));
                break;
            }
        }
    }
    if (token[0] != 0 && auth_)
        auth_->TouchSession(token);   // outside wsMutex_ — TouchSession locks its own table
}
```

In `HandleWs`, replace the upgrade branch:

```cpp
    if (req->method == HTTP_GET)
    {
        // Auth happens HERE, once. esp_http_server has already sent the
        // 101 handshake before invoking us; returning ESP_FAIL makes
        // httpd close the socket immediately, which is how an upgrade
        // is "refused". The frontend can't read a close reason — it
        // discriminates bad-token from network failure via an HTTP
        // ping before connecting (see backend.ts).
        char query[96] = {};
        char token[SessionTable::TOKEN_LEN] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK ||
            !self->auth_ || !self->auth_->ValidateToken(token))
        {
            ESP_LOGW(TAG, "WS upgrade refused: missing/invalid token");
            return ESP_FAIL;
        }
        self->AddWsClient(httpd_req_to_sockfd(req), token);
        return ESP_OK;
    }
```

In the frame path, after the successful `httpd_ws_recv_frame` call (right after the `if (ret != ESP_OK) {...}` block), add:

```cpp
    // Any inbound frame (heartbeat included) keeps the session alive —
    // an open tab never logs out; see spec.
    self->TouchClient(httpd_req_to_sockfd(req));
```

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: exit 0, `Project build complete` (this also clears Task 2's expected failure).

- [ ] **Step 4: Commit**

```bash
git add main/Application/WebServerManager
git commit -m "Webserver login: session auth at the transport edge

web.password setting (default admin), POST /api/login mints RAM tokens,
Bearer guard on /api/command, token check at WS upgrade, frame-refresh
keeps open tabs alive. Commands and streams never see auth."
git push
```

---

### Task 4: backend.ts auth plumbing + useAuth hook

**Files:**
- Modify: `frontend/src/lib/backend.ts`
- Create: `frontend/src/hooks/use-auth.ts`

**Interfaces:**
- Consumes: Task 2/3's HTTP contract — `GET /api/login` → `{name}`, `POST /api/login` `{password}` → `{token}` or 401, Bearer on `/api/command`, `?token=` on `/ws`.
- Produces (used by Task 5): `backend.login(password): Promise<boolean>`, `backend.getLoginInfo(): Promise<{name: string}>`, `backend.authenticated: boolean`, `backend.hasToken: boolean`, `backend.onAuthChange(fn): () => void`; hook `useAuth(): { authenticated: boolean; checking: boolean }`.

- [ ] **Step 1: Add auth state to BackendService**

In `frontend/src/lib/backend.ts`, next to the other type aliases add:

```ts
type AuthHandler = (authenticated: boolean) => void
```

Inside `class BackendService`, alongside the existing private fields:

```ts
  private token: string | null = sessionStorage.getItem("strux.token")
  private authHandlers = new Set<AuthHandler>()
  private _authenticated = false
```

And the accessors/helpers (next to the `status` getter):

```ts
  get authenticated(): boolean {
    return this._authenticated
  }

  get hasToken(): boolean {
    return this.token !== null
  }

  onAuthChange(fn: AuthHandler): () => void {
    this.authHandlers.add(fn)
    return () => {
      this.authHandlers.delete(fn)
    }
  }

  private setAuthenticated(auth: boolean) {
    if (auth !== this._authenticated) {
      this._authenticated = auth
      this.authHandlers.forEach((fn) => fn(auth))
    }
  }

  private clearAuth() {
    this.token = null
    sessionStorage.removeItem("strux.token")
    this.setAuthenticated(false)
  }

  private authHeaders(): Record<string, string> {
    return this.token ? { Authorization: `Bearer ${this.token}` } : {}
  }

  private apiUrl(path: string): string {
    const host = import.meta.env.DEV ? `http://${DEV_HOST}` : ""
    return `${host}${path}`
  }
```

Simplify `commandUrl` to reuse `apiUrl`:

```ts
  private commandUrl(type: string): string {
    return this.apiUrl(`/api/command?type=${encodeURIComponent(type)}`)
  }
```

- [ ] **Step 2: Add login() and getLoginInfo()**

In the `// ── API methods ──` section:

```ts
  /** Open endpoint: device name for the login page's brand slot. */
  async getLoginInfo(): Promise<{ name: string }> {
    const res = await fetch(this.apiUrl("/api/login"))
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
    return res.json()
  }

  /** Returns false on wrong password; throws on network failure. */
  async login(password: string): Promise<boolean> {
    const res = await fetch(this.apiUrl("/api/login"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password }),
    })
    if (res.status === 401) return false
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
    const { token } = (await res.json()) as { token: string }
    this.token = token
    sessionStorage.setItem("strux.token", token)
    this.setAuthenticated(true)
    this.connect()
    return true
  }
```

- [ ] **Step 3: Rewrite doConnect with token gating and pre-validation**

Replace the whole `doConnect()` method with:

```ts
  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }

    if (!this.token) {
      this.setStatus("disconnected")
      return Promise.reject(new Error("Not authenticated"))
    }

    this.setStatus("connecting")

    const p = (async () => {
      // Validate the token over HTTP first: the browser WS API cannot
      // distinguish a refused upgrade (bad token) from a network
      // failure, and we must not clear a good token on a flaky link.
      try {
        const res = await fetch(this.commandUrl("ping"), {
          method: "POST",
          headers: this.authHeaders(),
          body: "{}",
        })
        if (res.status === 401) {
          this.clearAuth()
          this.setStatus("disconnected")
          throw new Error("Not authenticated")
        }
      } catch (e) {
        if (e instanceof Error && e.message === "Not authenticated") throw e
        // Network error: fall through — the WS attempt below owns retries.
      }

      await new Promise<void>((resolve, reject) => {
        const host = import.meta.env.DEV ? DEV_HOST : location.host
        const proto = location.protocol === "https:" ? "wss:" : "ws:"
        const url = `${proto}//${host}/ws?token=${this.token}`
        console.log(`[BackendService] connecting to ${url} (DEV=${import.meta.env.DEV})`)
        const ws = new WebSocket(url)
        ws.binaryType = "arraybuffer"
        let opened = false

        ws.onopen = () => {
          opened = true
          this.ws = ws
          this.setStatus("connected")
          this.setAuthenticated(true)
          this.startHeartbeat()
          resolve()
        }

        ws.onmessage = (ev) => {
          // Binary frames are dispatched to binary subscribers.
          if (ev.data instanceof ArrayBuffer) {
            this.binaryHandlers.forEach((fn) => fn(ev.data))
            return
          }
          // Defensive: some browsers may deliver the first frame as a Blob if
          // binaryType wasn't applied in time. Convert and dispatch.
          if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
            ev.data.arrayBuffer().then((buf) => {
              this.binaryHandlers.forEach((fn) => fn(buf))
            })
            return
          }
          try {
            const msg = JSON.parse(ev.data)
            if (typeof msg.id === "number") {
              const req = this.pending.get(msg.id)
              if (req) {
                this.pending.delete(msg.id)
                clearTimeout(req.timer)
                if (msg.error) {
                  req.reject(new Error(msg.error))
                } else {
                  req.resolve(msg.payload)
                }
              }
            } else {
              this.broadcastHandlers.forEach((fn) => fn(msg))
            }
          } catch (e) {
            const sample = typeof ev.data === "string" ? ev.data.slice(-80) : "(non-string)"
            console.warn(
              `[BackendService] failed to parse WS frame (${typeof ev.data === "string" ? ev.data.length : "?"} bytes); tail: ${sample}`,
              e,
            )
          }
        }

        ws.onclose = () => {
          this.ws = null
          this.stopHeartbeat()
          this.setStatus("disconnected")
          for (const [, req] of this.pending) {
            clearTimeout(req.timer)
            req.reject(new Error("WebSocket closed"))
          }
          this.pending.clear()
          if (!opened) reject(new Error("Connection failed"))
          if (this.token) {
            this.reconnectTimer = setTimeout(() => {
              this.doConnect().catch(() => {})
            }, 2000)
          }
        }

        ws.onerror = () => ws.close()
      })
    })()

    this.connecting = p
    p.catch(() => {}).then(() => {
      if (this.connecting === p) this.connecting = null
    })
    return p
  }
```

(The `onmessage` body is unchanged from today — it moves inside the new structure verbatim.)

- [ ] **Step 4: Bearer + 401 handling on the HTTP transfer paths**

In `postCommand`, after `xhr.open("POST", this.commandUrl(type))`:

```ts
      if (this.token) xhr.setRequestHeader("Authorization", `Bearer ${this.token}`)
```

and change the start of `xhr.onload` to catch 401:

```ts
      xhr.onload = () => {
        if (xhr.status === 401) {
          this.clearAuth()
          reject(new Error("Not authenticated"))
        } else if (xhr.status >= 200 && xhr.status < 300) {
```

(closing the `if/else` chain as before).

In `downloadPartitionFile`, add the header to the fetch and a 401 check:

```ts
      const res = await fetch(this.commandUrl("downloadPartition"), {
        method: "POST",
        headers: this.authHeaders(),
        body: JSON.stringify({ partition: label }),
      })
      if (res.status === 401) {
        this.clearAuth()
        throw new Error("Not authenticated")
      }
      if (!res.ok || !res.body) throw new Error(`${res.status} ${res.statusText}`)
```

- [ ] **Step 5: Create the useAuth hook**

Create `frontend/src/hooks/use-auth.ts`:

```ts
import { useState, useEffect } from "react"
import { backend } from "@/lib/backend"

export function useAuth() {
  const [authenticated, setAuthenticated] = useState(backend.authenticated)
  // A stored token might still be valid — hold the login page back until
  // the auto-connect's validation resolves (or 3 s pass, e.g. device off).
  const [checking, setChecking] = useState(backend.hasToken && !backend.authenticated)

  useEffect(() => {
    const unsub = backend.onAuthChange((auth) => {
      setAuthenticated(auth)
      setChecking(false)
    })

    if (backend.hasToken && !backend.authenticated) {
      const timer = setTimeout(() => setChecking(false), 3000)
      return () => {
        unsub()
        clearTimeout(timer)
      }
    }

    return unsub
  }, [])

  return { authenticated, checking }
}
```

- [ ] **Step 6: Typecheck**

Run: `cd frontend && pnpm typecheck`
Expected: exit 0, no output.

- [ ] **Step 7: Commit**

```bash
git add frontend/src/lib/backend.ts frontend/src/hooks/use-auth.ts
git commit -m "Frontend auth plumbing: token storage, login API, gated WS connect"
git push
```

---

### Task 5: LoginPage (stripped shadcn login-03) + App gate

**Files:**
- Create: `frontend/src/pages/LoginPage.tsx`
- Create (via scaffold): `frontend/src/components/ui/card.tsx`, `frontend/src/components/ui/label.tsx` (plus whatever else the block pulls in)
- Delete (after scaffold): the scaffolded `login-form` component and any demo block page
- Modify: `frontend/src/App.tsx`

**Interfaces:**
- Consumes: Task 4's `backend.login`, `backend.getLoginInfo`, `useAuth`.
- Produces: `<LoginPage />` default export; the app gate in `App.tsx`.

- [ ] **Step 1: Scaffold the login-03 block**

Run: `cd frontend && pnpm dlx shadcn@latest add login-03`
Expected: adds `src/components/ui/card.tsx` and `src/components/ui/label.tsx` (button/input already exist) plus a `login-form` component (path printed by the CLI, typically `src/components/login-form.tsx`).

Verify: `ls src/components/ui/card.tsx src/components/ui/label.tsx` — both exist.

- [ ] **Step 2: Delete the scaffolded demo pieces**

Delete the generated `login-form` component file and any generated demo/block page (keep only the `components/ui/*` primitives). We write our own page next — the demo has email/social/signup content we stripped by design.

- [ ] **Step 3: Create LoginPage.tsx**

Create `frontend/src/pages/LoginPage.tsx`:

```tsx
import { useState, useEffect } from "react"
import { CpuIcon, Loader2Icon } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { backend } from "@/lib/backend"

// shadcn login-03, stripped: brand slot + password + submit. No email,
// no social logins, no signup/forgot links, no terms footer (spec).
export default function LoginPage() {
  const [password, setPassword] = useState("")
  const [error, setError] = useState("")
  const [loading, setLoading] = useState(false)
  const [deviceName, setDeviceName] = useState("Strux")

  useEffect(() => {
    backend.getLoginInfo().then((res) => {
      setDeviceName(res.name)
      document.title = res.name
    }).catch(() => { /* open endpoint unreachable — keep fallback */ })
  }, [])

  async function handleSubmit(e: React.FormEvent) {
    e.preventDefault()
    setError("")
    setLoading(true)
    try {
      const ok = await backend.login(password)
      if (!ok) setError("Invalid password")
    } catch {
      setError("Connection failed")
    } finally {
      setLoading(false)
    }
  }

  return (
    <div className="bg-muted flex min-h-svh flex-col items-center justify-center gap-6 p-6 md:p-10">
      <div className="flex w-full max-w-sm flex-col gap-6">
        <div className="flex items-center gap-2 self-center font-medium">
          <div className="bg-primary text-primary-foreground flex size-6 items-center justify-center rounded-md">
            <CpuIcon className="size-4" />
          </div>
          {deviceName}
        </div>
        <Card>
          <CardHeader className="text-center">
            <CardTitle className="text-xl">Welcome back</CardTitle>
            <CardDescription>Sign in to access the device</CardDescription>
          </CardHeader>
          <CardContent>
            <form onSubmit={handleSubmit} className="grid gap-6">
              <div className="grid gap-2">
                <Label htmlFor="password">Password</Label>
                <Input
                  id="password"
                  type="password"
                  autoComplete="current-password"
                  autoFocus
                  required
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                />
              </div>
              {error && <p className="text-destructive text-center text-sm">{error}</p>}
              <Button type="submit" className="w-full" disabled={loading}>
                {loading
                  ? <><Loader2Icon className="mr-1.5 size-4 animate-spin" />Signing in...</>
                  : "Sign in"}
              </Button>
            </form>
          </CardContent>
        </Card>
      </div>
    </div>
  )
}
```

- [ ] **Step 4: Gate App.tsx**

Replace `frontend/src/App.tsx`'s `App` function (imports added, `PageContent` unchanged):

```tsx
import { SidebarProvider, SidebarTrigger } from "@/components/ui/sidebar"
import { AppSidebar, type Page } from "@/components/AppSidebar"
import { useRoute } from "@/hooks/use-route"
import { useAuth } from "@/hooks/use-auth"
import HomePage from "@/pages/HomePage"
import ConsolePage from "@/pages/ConsolePage"
import SettingsPage from "@/pages/SettingsPage"
import FirmwarePage from "@/pages/FirmwarePage"
import LoginPage from "@/pages/LoginPage"

function PageContent({ page }: { page: Page }) {
  switch (page) {
    case "home":
      return <HomePage />
    case "console":
      return <ConsolePage />
    case "settings":
      return <SettingsPage />
    case "firmware":
      return <FirmwarePage />
  }
}

export default function App() {
  const { authenticated, checking } = useAuth()
  const { page, navigate } = useRoute()

  if (checking) return null   // stored token being validated — avoid login-page flash
  if (!authenticated) return <LoginPage />

  return (
    <SidebarProvider>
      <AppSidebar currentPage={page} onNavigate={navigate} />
      <main className="flex h-screen w-full min-w-0 flex-col overflow-hidden p-6">
        <SidebarTrigger className="shrink-0 md:hidden" />
        <div className="min-h-0 w-full flex-1 overflow-y-auto">
          <PageContent page={page} />
        </div>
      </main>
    </SidebarProvider>
  )
}
```

- [ ] **Step 5: Typecheck and build**

Run: `cd frontend && pnpm typecheck`
Expected: exit 0.
Run: `cd frontend && pnpm build`
Expected: exit 0, gzipped output written into `../www`.

- [ ] **Step 6: Commit**

```bash
git add frontend/src
git commit -m "Login page (stripped shadcn login-03) and auth gate in App"
git push
```

---

### Task 6: Full build + on-device verification

**Files:** none (verification only)

- [ ] **Step 1: Full firmware build**

Run: `idf.py build`
Expected: exit 0 (also re-embeds the freshly built frontend).

- [ ] **Step 2: Flash and verify on hardware**

Run: `idf.py -p <PORT> flash monitor`

Manual checklist (from the spec's Verification section):

1. Open the device UI → login page appears with the device name in the brand slot.
2. Wrong password → immediate "Invalid password"; retry works right away.
3. Password `admin` → app loads; console page streams logs (WS authenticated).
4. Reload the tab → still logged in (sessionStorage token survives).
5. Second browser (or private window) → independent login works alongside.
6. Log in 5 times in a row (private windows) → oldest session evicted; its tab's next HTTP action lands on the login page.
7. Change `web.password` on the settings page + save → next HTTP action in every tab returns to the login page; new password works.
8. `cd frontend && pnpm dev` against the device → login and app work cross-origin.
9. Leave a console tab open beyond 30 minutes → never logs out (heartbeat refresh).
10. Firmware page: upload a partition image and download one → both work with the Bearer header (and fail to the login page if the token is cleared server-side, e.g. after a password change).

- [ ] **Step 3: Update backlog and commit**

`docs/backlog/webserver-login.md` is now implemented — delete it (the spec records the design; the backlog tracked the intent).

```bash
git rm docs/backlog/webserver-login.md
git commit -m "Webserver login shipped: drop backlog entry"
git push
```
