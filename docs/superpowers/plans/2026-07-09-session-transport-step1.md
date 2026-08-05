# Session Transport — Step 1 (foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry every *no-body* WS command over the new binary session-chunk format (`[session|flags|payload]`), running the new transport alongside the existing TEXT-JSON path, then migrate the frontend and delete the old command path — without changing concurrency behaviour.

**Architecture:** Inbound WS **binary** frames become session chunks (new path); **text** frames keep the existing `{id,type}` dispatch and remain the log-broadcast direction (old path). A `Session` (a `Stream`) is created per request, its reply streamed back as binary chunks. Step-1 requests are single-chunk and dispatched synchronously inside one frame handler, so there is no persistent session state, no busy-gate, and no client serialization yet (those arrive in step 2 with streamed uploads). At the end, the old TEXT command-dispatch path is removed; TEXT survives only as the outbound broadcast direction.

**Tech Stack:** ESP-IDF v6.0, C++17, FreeRTOS, esp_http_server WebSocket; React 19 + TypeScript frontend (`backend.ts`). No unit-test framework — verification is build + flash + a Python WS harness + the real UI.

## Global Constraints

- Build env: dot-source `C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1`, set `$env:PYTHONUTF8=1`, then `idf.py build`. Flash: `idf.py -p COM3 flash`.
- C++17, no exceptions; `snprintf` with `sizeof` bounds, no `strcpy`/`strcat`.
- Sources are listed explicitly in [main/CMakeLists.txt](../../../main/CMakeLists.txt) `SOURCE_FILES_LIST` (no globbing); new `.cpp` files must be added there.
- JSON via `lib/json` (`JsonWriter`/`JsonReader`/`JsonScope`); no external JSON lib.
- Commit after every task. Commit messages end with the `Co-Authored-By` trailer used in this repo.
- Test device is on **COM3**, reachable on the LAN (IP from boot log); web password default `admin`.

## Wire format (reference — implemented in Task 1)

```
chunk = [ session : u16 little-endian ][ flags : u8 ][ payload : bytes ]
        FLAG_FINAL  = 0x01   last chunk for this direction (EOF)
        FLAG_REJECT = 0x02   transport/framework refused (busy / unknown command); payload = reason text
```

- Request (client→device): one binary frame, `flags = FINAL`, payload = the command envelope JSON `{"type":"...",...args}`.
- Reply (device→client): one or more binary frames with the same `session`, payload = handler output bytes; the last carries `FLAG_FINAL`. An unknown command / framework error is a single `FLAG_REJECT` frame whose payload is the reason.
- `session` correlates request↔reply (it plays today's `id` role). Broadcasts stay TEXT `{ "log": ... }`, unchanged.

---

## Task 1: Wire-format header (`SessionProtocol.h`)

**Files:**
- Create: `main/Application/WebServerManager/SessionProtocol.h`

**Interfaces:**
- Produces: `session::FLAG_FINAL`, `session::FLAG_REJECT`, `session::HEADER_LEN`, `session::readU16(const uint8_t*)`, `session::writeHeader(uint8_t* out, uint16_t s, uint8_t flags) -> size_t`.

- [ ] **Step 1: Create the header**

```cpp
#pragma once

#include <cstdint>
#include <cstddef>

// On-wire session chunk: [ session:u16 LE ][ flags:u8 ][ payload ]. See
// docs/superpowers/specs/2026-07-09-session-mux-transport-design.md.
namespace session
{
    inline constexpr uint8_t FLAG_FINAL  = 0x01;   // last chunk this direction (EOF)
    inline constexpr uint8_t FLAG_REJECT = 0x02;   // transport/framework refused; payload = reason
    inline constexpr size_t  HEADER_LEN  = 3;

    inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

    // Writes the 3-byte header into `out`; returns HEADER_LEN.
    inline size_t writeHeader(uint8_t* out, uint16_t s, uint8_t flags)
    {
        out[0] = static_cast<uint8_t>(s & 0xFF);
        out[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
        out[2] = flags;
        return HEADER_LEN;
    }
}
```

- [ ] **Step 2: Build**

Run (PowerShell): `. "C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1"; $env:PYTHONUTF8=1; idf.py build`
Expected: build succeeds (header is unused yet; a header-only file triggers no warning).

- [ ] **Step 3: Commit**

```bash
git add main/Application/WebServerManager/SessionProtocol.h
git commit -m "Session transport: add wire-format header (SessionProtocol.h)"
```

---

## Task 2: `Session`, `WsSessionLink`, `SessionMux` classes (compile only)

Build the three transport classes. Step-1 implementations are minimal and synchronous; comments mark where step 2 grows them. Not wired into dispatch yet.

**Files:**
- Create: `main/Application/WebServerManager/WsSessionLink.h`
- Create: `main/Application/WebServerManager/SessionMux.h`
- Create: `main/Application/WebServerManager/SessionMux.cpp`
- Modify: `main/CMakeLists.txt` (add `SessionMux.cpp` to `SOURCE_FILES_LIST`)

**Interfaces:**
- Consumes: `session::*` (Task 1); `Stream` (`lib/common/Stream.h`); the `sendMutex_` pattern from `WebSocketHandler`.
- Produces:
  - `class WsSessionLink { WsSessionLink(httpd_req_t* req, Mutex& sendMutex); bool SendChunk(uint16_t session, uint8_t flags, const void* payload, size_t len); }`
  - `class Session : public Stream { Session(uint16_t id, WsSessionLink& link); void feedRequest(const uint8_t* data, size_t len); size_t read(...); size_t write(...); void finish(); void reject(const char* reason); }`
  - `class SessionMux { struct Sink { virtual void OnSessionOpened(Session&) = 0; }; SessionMux(WsSessionLink& link, Sink& sink); void OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len); }`

- [ ] **Step 1: Create `WsSessionLink.h`**

```cpp
#pragma once

#include "SessionProtocol.h"
#include "Mutex.h"
#include <esp_http_server.h>
#include <cstring>

// Transport link for the WebSocket: one session chunk == one WS binary frame
// ([session|flags|payload]). Step 1 sends synchronously on the httpd task using
// the live request; the shared send mutex serializes whole frames so a log
// broadcast can't split a chunk's bytes (chunks between frames may interleave
// harmlessly — the client routes by session id). Step 2 (async worker) will
// switch to httpd_ws_send_frame_async(server, fd).
class WsSessionLink
{
    httpd_req_t* req_;
    Mutex& sendMutex_;
    uint8_t frame_[session::HEADER_LEN + 4096];   // header + one chunk payload

public:
    WsSessionLink(httpd_req_t* req, Mutex& sendMutex) : req_(req), sendMutex_(sendMutex) {}

    static constexpr size_t MAX_PAYLOAD = 4096;

    bool SendChunk(uint16_t session, uint8_t flags, const void* payload, size_t len)
    {
        if (len > MAX_PAYLOAD) return false;
        session::writeHeader(frame_, session, flags);
        if (len) memcpy(frame_ + session::HEADER_LEN, payload, len);

        httpd_ws_frame_t f = {};
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = frame_;
        f.len = session::HEADER_LEN + len;

        LOCK(sendMutex_);
        return httpd_ws_send_frame(req_, &f) == ESP_OK;
    }
};
```

- [ ] **Step 2: Create `SessionMux.h`**

```cpp
#pragma once

#include "Stream.h"
#include "WsSessionLink.h"
#include "SessionProtocol.h"
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>

// A session's stream. read() = the request bytes fed by the mux; write() = the
// reply, buffered and flushed as binary DATA chunks, closed by finish() with
// FLAG_FINAL. Step 1: the request is a single chunk fed up front (no blocking).
// Step 2 will make read() pull further inbound chunks for large bodies.
class Session : public Stream
{
    uint16_t id_;
    WsSessionLink& link_;

    const uint8_t* req_ = nullptr;   // request bytes (borrowed; valid during dispatch)
    size_t reqLen_ = 0;
    size_t reqPos_ = 0;

    uint8_t out_[WsSessionLink::MAX_PAYLOAD];   // reply chunk buffer
    size_t outLen_ = 0;
    bool failed_ = false;

    void flush(uint8_t flags)
    {
        if (!link_.SendChunk(id_, flags, out_, outLen_)) failed_ = true;
        outLen_ = 0;
    }

public:
    Session(uint16_t id, WsSessionLink& link) : id_(id), link_(link) {}

    void feedRequest(const uint8_t* data, size_t len) { req_ = data; reqLen_ = len; reqPos_ = 0; }

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        size_t n = std::min(size, reqLen_ - reqPos_);
        if (n) { memcpy(dst, req_ + reqPos_, n); reqPos_ += n; }
        return n;
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0)
        {
            size_t n = std::min(sizeof(out_) - outLen_, remaining);
            memcpy(out_ + outLen_, p, n);
            outLen_ += n; p += n; remaining -= n;
            if (outLen_ == sizeof(out_)) flush(0);   // full buffer → non-final DATA chunk
        }
        return size;
    }

    // Emit the final chunk, closing the reply direction.
    void finish() { flush(session::FLAG_FINAL); }

    // Transport/framework refusal (unknown command, busy): one REJECT chunk.
    void reject(const char* reason) { link_.SendChunk(id_, session::FLAG_REJECT, reason, strlen(reason)); }

    bool failed() const { return failed_; }
    uint16_t id() const { return id_; }
};

// Routes inbound chunks to sessions. Step 1: every inbound chunk is a complete
// single-chunk request, dispatched synchronously; no persistent state, no
// busy-gate (a session lives only for the duration of OnChunk). Step 2 adds a
// slot table + busy-refuse + multi-chunk request bodies.
class SessionMux
{
public:
    struct Sink
    {
        virtual ~Sink() = default;
        virtual void OnSessionOpened(Session& session) = 0;
    };

    SessionMux(WsSessionLink& link, Sink& sink) : link_(link), sink_(sink) {}

    void OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len);

private:
    WsSessionLink& link_;
    Sink& sink_;
};
```

- [ ] **Step 3: Create `SessionMux.cpp`**

```cpp
#include "SessionMux.h"

void SessionMux::OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len)
{
    // Step 1: treat every inbound chunk as a complete request. (FINAL is
    // expected; multi-chunk request bodies arrive in step 2.)
    (void)flags;
    Session session(id, link_);
    session.feedRequest(payload, len);
    sink_.OnSessionOpened(session);
}
```

- [ ] **Step 4: Add `SessionMux.cpp` to the build**

Modify [main/CMakeLists.txt](../../../main/CMakeLists.txt): add to `SOURCE_FILES_LIST`, next to the other `Application/WebServerManager/*.cpp` entries:

```cmake
    "Application/WebServerManager/SessionMux.cpp"
```

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: succeeds. (Classes compile; still unused.)

- [ ] **Step 6: Commit**

```bash
git add main/Application/WebServerManager/WsSessionLink.h main/Application/WebServerManager/SessionMux.h main/Application/WebServerManager/SessionMux.cpp main/CMakeLists.txt
git commit -m "Session transport: add Session / WsSessionLink / SessionMux (compile-only)"
```

---

## Task 3: Dispatch a no-body command over binary chunks (device)

Wire inbound **binary** frames through the mux to `CommandManager::Execute`, replying as chunks. Leave the TEXT path intact.

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.h` (make it a `SessionMux::Sink`; add the binary handler)
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp` (route binary frames; implement `OnSessionOpened`)

**Interfaces:**
- Consumes: `SessionMux`, `Session`, `WsSessionLink` (Task 2); `CommandManager::Execute(const char* type, Stream& in, Stream& out) -> bool`; `MemoryStream`; `ExtractJsonString` (`JsonHelpers.h`).
- Produces: `WebSocketHandler::OnSessionOpened(Session&)`; `WebSocketHandler::HandleBinary(httpd_req_t*, const uint8_t* frame, size_t len)`.

- [ ] **Step 1: Declare the Sink + handlers in `WebSocketHandler.h`**

Add the include and base class, and the two private methods. Change the class declaration:

```cpp
#include "SessionMux.h"
```
```cpp
class WebSocketHandler : public SessionMux::Sink {
```
Add to the private section (near `DispatchMessage`):
```cpp
    // New binary session transport. A request is one binary chunk; the reply
    // streams back as chunks on the same session id. Runs alongside the TEXT
    // path until the frontend is migrated (then the TEXT request path is removed).
    void HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len);
    void OnSessionOpened(Session& session) override;
```

- [ ] **Step 2: Route binary frames in `HandleWs` (`WebSocketHandler.cpp`)**

In `HandleWs`, the frame is already received into `buf`. After the `CLOSE` check and before the `HTTPD_WS_TYPE_TEXT` handling, add:

```cpp
    if (frame.type == HTTPD_WS_TYPE_BINARY)
    {
        if (frame.len >= session::HEADER_LEN)
            self->HandleBinary(req, buf, frame.len);
        return ESP_OK;
    }
```

(The existing `if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) return ESP_OK;` line below then handles the remaining text path unchanged.)

- [ ] **Step 3: Implement `HandleBinary` and `OnSessionOpened`**

Add near `DispatchMessage`:

```cpp
void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = session::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + session::HEADER_LEN;
    size_t plen = len - session::HEADER_LEN;

    WsSessionLink link(req, sendMutex_);
    SessionMux mux(link, *this);
    mux.OnChunk(sid, flags, payload, plen);
}

void WebSocketHandler::OnSessionOpened(Session& session)
{
    // Read the request envelope (single chunk in step 1). The header line is
    // the whole envelope JSON: {"type":"...",...args}. A trailing '\n' (added
    // by the client for step-2 forward-compat) is stripped; anything after it
    // would be the body (none in step 1).
    char envelope[512];
    size_t n = session.read(envelope, sizeof(envelope) - 1);
    envelope[n] = '\0';
    if (char* nl = strchr(envelope, '\n')) *nl = '\0';

    char type[32] = {};
    ExtractJsonString(envelope, "type", type, sizeof(type));

    if (type[0] == '\0')
    {
        session.reject("missing type");
        return;
    }

    MemoryStream in(envelope, strlen(envelope));   // envelope = args (as today)
    if (!commandManager_ || !commandManager_->Execute(type, in, session))
    {
        session.reject(type);   // unknown command
        return;
    }
    session.finish();   // FINAL — end of reply
}
```

- [ ] **Step 4: Build**

Run: `idf.py build`
Expected: succeeds.

- [ ] **Step 5: Flash**

Run: `idf.py -p COM3 flash`
Expected: `Hash of data verified.` then `Hard resetting`.

- [ ] **Step 6: Verify with the binary-chunk Python harness**

Create the harness in the scratchpad (reuse handshake/masking; send/parse binary). Save as `session_probe.py`:

```python
import socket, os, base64, struct, json, urllib.request

HOST, PORT, PASSWORD = "DEVICE_IP", 80, "admin"

def login():
    req = urllib.request.Request(f"http://{HOST}/api/login",
        data=json.dumps({"password": PASSWORD}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    return json.loads(urllib.request.urlopen(req, timeout=5).read())["token"]

def handshake(token):
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws?token={token} HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(1)
    assert b"101" in buf.split(b"\r\n")[0], buf
    return s

def send_binary(s, data):
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    n = len(data)
    if n < 126: hdr = bytes([0x82, 0x80 | n])
    else:       hdr = bytes([0x82, 0x80 | 126]) + struct.pack(">H", n)
    s.sendall(hdr + mask + masked)

def recv_exact(s, n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d));  assert c;  d += c
    return d

def read_frame(s):
    b0, b1 = recv_exact(s, 2)
    ln = b1 & 0x7F
    if ln == 126: ln = struct.unpack(">H", recv_exact(s, 2))[0]
    elif ln == 127: ln = struct.unpack(">Q", recv_exact(s, 8))[0]
    return b0 & 0x0F, recv_exact(s, ln) if ln else b""   # (opcode, payload)

FLAG_FINAL, FLAG_REJECT = 0x01, 0x02

def command(s, sid, type_, **args):
    env = json.dumps({"type": type_, **args}).encode() + b"\n"
    send_binary(s, struct.pack("<H", sid) + bytes([FLAG_FINAL]) + env)
    data = b""
    while True:
        op, payload = read_frame(s)
        if op == 0x1:   # text broadcast — ignore
            continue
        assert op == 0x2, op
        rsid = struct.unpack("<H", payload[:2])[0]
        flags = payload[2]
        assert rsid == sid, (rsid, sid)
        if flags & FLAG_REJECT:
            raise RuntimeError("REJECT: " + payload[3:].decode())
        data += payload[3:]
        if flags & FLAG_FINAL:
            return data

token = login()
s = handshake(token)
info = json.loads(command(s, 1, "info"))
print("PASS info:", info.get("project"), info.get("firmware"))
```

Run: `python session_probe.py` (set `DEVICE_IP`).
Expected: `PASS info: Strux <version>` — a binary session round-trip works.

- [ ] **Step 7: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp
git commit -m "Session transport: dispatch no-body commands over binary chunks"
```

---

## Task 4: Verify reply streaming across chunks (large `getLogs`)

No new production code — this proves multi-chunk replies reassemble (the `Session::write` buffer flush + `finish`). Reuses the Task 3 harness.

**Files:**
- Test only: scratchpad `session_probe.py` (extend)

- [ ] **Step 1: Extend the harness to flood logs and call `getLogs`**

Append to `session_probe.py`:

```python
# flood logs past one chunk (4096) via connect/disconnect churn, then getLogs
for i in range(60):
    handshake(token).close()
import time; time.sleep(1.0)
s2 = handshake(token)
logs = json.loads(command(s2, 1, "getLogs"))
assert isinstance(logs.get("lines"), list), logs
print(f"PASS getLogs: {len(logs['lines'])} lines, {len(json.dumps(logs))} bytes reassembled")
```

- [ ] **Step 2: Run**

Run: `python session_probe.py`
Expected: `PASS getLogs: 200 lines, >4096 bytes reassembled` — a reply larger than one chunk reassembles from multiple binary chunks into valid JSON.

- [ ] **Step 3: Commit (harness snapshot)**

```bash
git add docs/superpowers/plans/2026-07-09-session-transport-step1.md
git commit -m "Session transport: verify multi-chunk reply reassembly (getLogs)"
```

---

## Task 5: Migrate `backend.ts` to the binary session transport

Replace the frontend `send()` wire encoding (TEXT `{id,type}` → binary chunk) and reply reassembly. **Concurrency is unchanged** — `session` plays today's `id` role, calls stay concurrent, no queue. Broadcasts (TEXT, no `id`) and the binary *download* path are untouched.

**Files:**
- Modify: `frontend/src/lib/backend.ts` (the `send()` path, the `onmessage` handler, session-id counter)

**Interfaces:**
- Consumes: the device binary transport (Task 3) — request `[sid|FINAL|json+\n]`, reply chunks `[sid|flags|bytes]`.
- Produces: unchanged public `send<T>()` / API-method surface; callers are untouched.

- [ ] **Step 1: Add session state and a reply-assembly map**

Replace the module-level `let nextId = 1` with:

```typescript
let nextSession = 1

const FLAG_FINAL = 0x01
const FLAG_REJECT = 0x02
```

In the class, replace `private pending = new Map<number, PendingRequest>()` semantics to accumulate bytes. Change `PendingRequest`:

```typescript
interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
  chunks: Uint8Array[]
}
```

- [ ] **Step 2: Rewrite `send()` to emit a binary chunk**

Replace the body of `send<T>()`:

```typescript
async send<T>(type: string, params: Record<string, unknown> = {}): Promise<T> {
  await this.ensureConnected()
  const session = nextSession++
  const envelope = JSON.stringify({ type, ...params }) + "\n"
  const body = new TextEncoder().encode(envelope)
  const frame = new Uint8Array(3 + body.length)
  frame[0] = session & 0xff
  frame[1] = (session >> 8) & 0xff
  frame[2] = FLAG_FINAL
  frame.set(body, 3)
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => {
      this.pending.delete(session)
      reject(new Error("Request timeout"))
    }, 10000)
    this.pending.set(session, { resolve: resolve as (d: unknown) => void, reject, timer, chunks: [] })
    this.ws!.send(frame)
  })
}
```

- [ ] **Step 3: Handle binary reply frames in `onmessage`**

Replace the binary branch (currently dispatching to `binaryHandlers`) so session chunks are reassembled, and only non-session binary (none, in step 1) would fall through. Replace the `if (ev.data instanceof ArrayBuffer) { ... }` block:

```typescript
if (ev.data instanceof ArrayBuffer) {
  const view = new Uint8Array(ev.data)
  if (view.length < 3) return
  const session = view[0] | (view[1] << 8)
  const flags = view[2]
  const req = this.pending.get(session)
  if (!req) {
    // not a known session reply — legacy binary subscribers (unused in step 1)
    this.binaryHandlers.forEach((fn) => fn(ev.data))
    return
  }
  if (flags & FLAG_REJECT) {
    this.pending.delete(session)
    clearTimeout(req.timer)
    req.reject(new Error(new TextDecoder().decode(view.subarray(3)) || "rejected"))
    return
  }
  req.chunks.push(view.subarray(3))
  if (flags & FLAG_FINAL) {
    this.pending.delete(session)
    clearTimeout(req.timer)
    const total = req.chunks.reduce((a, c) => a + c.length, 0)
    const buf = new Uint8Array(total)
    let off = 0
    for (const c of req.chunks) { buf.set(c, off); off += c.length }
    const text = new TextDecoder().decode(buf)
    try { req.resolve(text.length ? JSON.parse(text) : {}) }
    catch (e) { req.reject(e instanceof Error ? e : new Error("bad reply")) }
  }
  return
}
```

Leave the text branch (`JSON.parse` → broadcasts) below **only for broadcasts**: since replies are now binary, drop the `msg.id` matching from the text branch and route every text message to `broadcastHandlers`:

```typescript
try {
  const msg = JSON.parse(ev.data)
  this.broadcastHandlers.forEach((fn) => fn(msg))
} catch (e) { /* keep existing warn */ }
```

- [ ] **Step 4: Typecheck + build the frontend**

Run: `cd frontend; pnpm typecheck; pnpm build`
Expected: no type errors; build emits gzipped assets into `../www`.

- [ ] **Step 5: Flash (frontend is embedded) and drive the real UI**

Run: `idf.py -p COM3 flash`
Then open the device IP in a browser and exercise: Info page loads; Settings loads + edit/save; Console shows history *and* live log lines; Firmware page shows status + partitions; Wi-Fi scan. Confirm no red timeout toasts and the network tab shows WS binary frames.
Expected: every page works exactly as before.

- [ ] **Step 6: Commit**

```bash
git add frontend/src/lib/backend.ts
git commit -m "Session transport: migrate backend.ts send() to binary session chunks"
```

---

## Task 6: Remove the old TEXT command-dispatch path

Requests are now binary; TEXT survives only as the outbound broadcast direction. Delete the inbound TEXT command handling and the now-unused `DispatchMessage` / `WsResponseStream`.

**Files:**
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp` (remove TEXT request parsing + `DispatchMessage` + `WsResponseStream`)
- Modify: `main/Application/WebServerManager/WebSocketHandler.h` (remove `DispatchMessage` decl and `wsBuf_` if unused)

- [ ] **Step 1: Remove the inbound TEXT command path in `HandleWs`**

Delete the block from `buf[frame.len] = '\0';` through the `self->DispatchMessage(req, id, type, json);` call and the `id`/`type` extraction above it. Keep the `CLOSE` handling and the new binary branch. A TEXT frame inbound is no longer expected; drop it:

```cpp
    // Inbound TEXT frames are no longer used (requests are binary session
    // chunks; TEXT is outbound broadcasts only). Ignore any stray text.
    if (frame.type == HTTPD_WS_TYPE_TEXT)
        return ESP_OK;
```

- [ ] **Step 2: Delete `DispatchMessage` and `WsResponseStream`**

Remove `WebSocketHandler::DispatchMessage` (the whole method), its declaration in the header, the `WsResponseStream` class (its buffering now lives in `Session::write`), and `wsBuf_` if no longer referenced. Keep `WsRequestStream` (it's the step-2 inbound-body primitive) and the `httpd_ws_get_frame_type` forward-declaration.

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: succeeds; no reference to removed symbols.

- [ ] **Step 4: Flash and re-verify the full UI + harness**

Run: `idf.py -p COM3 flash`
Then re-run `python session_probe.py` (expect both PASS lines) and re-check every UI page as in Task 5 Step 5.
Expected: all green; the device now serves commands only over the binary session transport.

- [ ] **Step 5: Commit**

```bash
git add main/Application/WebServerManager/WebSocketHandler.h main/Application/WebServerManager/WebSocketHandler.cpp
git commit -m "Session transport: remove the old TEXT command-dispatch path"
```

---

## Notes / spec refinements discovered during planning

- **Frontend serialization moved to step 2.** Step-1 no-body commands dispatch synchronously within one frame handler, so a session never outlives its `OnChunk`; there is no persistent active-session state, no busy-gate, and no `REJECT` for concurrency. Concurrent `send()`s therefore keep working exactly as today (session id = old id). The single-in-flight gate and the client open-serialization queue belong to step 2 (upload), where a session holds the socket across many inbound frames. Update the spec's "Frontend impact" / migration step 1 to reflect this when convenient.
- **Per-message send-mutex hold dropped.** With chunk framing each frame is self-describing (session id), so a broadcast interleaving *between* reply chunks is harmless; `SendChunk` takes `sendMutex_` per frame instead of across the whole reply.
- **`WsRequestStream` stays dormant** until step 2 (multi-frame inbound bodies).

## Self-review

- **Spec coverage:** wire format (Task 1), `Session`/`SessionMux`/`WsSessionLink` (Task 2), `type`-in-stream + command dispatch (Task 3), reply streaming across chunks (Task 4), frontend cutover incl. broadcasts-stay-TEXT (Task 5), delete old path (Task 6). Deferred by design and noted: busy-gate, client serialization, multi-frame inbound, UART link, concurrency worker.
- **Placeholders:** none — every step has concrete code/commands.
- **Type consistency:** `SendChunk(uint16_t, uint8_t, const void*, size_t)`, `feedRequest`, `finish`, `reject`, `OnSessionOpened`, `HandleBinary`, `readU16`/`writeHeader`, `FLAG_FINAL`/`FLAG_REJECT` used identically across tasks and frontend.
