# Stream Commands Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** UpdateManager goes pure — its bespoke HTTP routes are deleted and replaced by commands plus one generic `POST /api/command?type=<name>` route with true streaming both ways.

**Architecture:** Spec section "HTTP transport / UpdateManager goes pure" (`docs/superpowers/specs/2026-07-03-stream-commands-design.md`). UpdateManager gains a session model (`updateBegin`/`updateWrite`/`updateEnd`), pull-OTA (`updateFromUrl`), and `downloadPartition`. WebServerManager gains two `Stream` adapters over `httpd_req_recv`/`httpd_resp_send_chunk` and loses all knowledge of UpdateManager. The frontend firmware page switches to the command path.

**Tech Stack:** ESP-IDF v6.0 (`esp_http_server`, `esp_http_client`), C++17; React 19 + TypeScript.

## Global Constraints

- No heap in handlers/adapters (stack buffers only).
- Handler errors are in-band (`{"ok":false,"error":...}`); HTTP status is only the transport envelope (404 = unknown command).
- Build command (PowerShell):
  `$env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; Set-Location C:\Workspace\Strux; idf.py build`
- Work on branch `stream-commands-phase2` (create from `main`); commit after every task.
- Between Task 2 and Task 3 the web UI's upload/download buttons are broken (old routes gone, new frontend not yet built) — flash only after Task 3.

---

### Task 0: Branch

- [ ] `git checkout -b stream-commands-phase2`

---

### Task 1: UpdateManager command set + session model

**Files:**
- Modify: `main/Application/UpdateManager/UpdateManager.h`
- Modify: `main/Application/UpdateManager/UpdateManager.cpp`
- Modify: `main/CMakeLists.txt:46-61` (COMPONENT_REQUIRES)

**Interfaces:**
- Consumes: existing `BeginAppUpdate/WriteAppChunk/FinalizeAppUpdate`, `BeginWwwUpdate/WriteWwwChunk/FinalizeWwwUpdate`, `JsonReader` , `JsonObject`, `Stream`.
- Produces commands (Task 3 frontend relies on these payloads):
  - `updateBegin` — in `{"target":"app"|"www"}` → out `{"ok":true}` or `{"ok":false,"error":"busy"|"begin failed"|"bad target"}`
  - `updateWrite` — in: RAW image bytes → out `{"ok":true,"size":N}` or `{"ok":false,"error":"no session"|"write failed"}`
  - `updateEnd` — in ignored → out `{"ok":true}` or `{"ok":false,"error":<finalize error>}`
  - `updateFromUrl` — in `{"url":"http://...","target":"app"|"www"?}` (default app) → out `{"ok":true,"size":N}` or `{"ok":false,"error":...}`
  - `downloadPartition` — in `{"partition":"<label>"}` → out: RAW partition bytes (no JSON on success; unknown label writes `{"ok":false,"error":"unknown partition"}`)

- [ ] **Step 1: Add `esp_http_client` to COMPONENT_REQUIRES**

In `main/CMakeLists.txt`, extend the list:

```cmake
set(COMPONENT_REQUIRES
    esp_timer
    esp_netif
    esp_event
    esp_wifi
    nvs_flash
    esp_http_server
    esp_http_client
    esp_app_format
    app_update
    esp_partition
    fatfs
    wear_levelling
    esp_driver_gpio
    mdns
    espressif__mqtt
)
```

- [ ] **Step 2: UpdateManager.h — session state + new command declarations**

Add to the private section (near the OTA state):

```cpp
    // Active command-driven update session (updateBegin/Write/End).
    // Guarded by the single-client assumption: Begin*Update()'s own
    // otaActive_/wwwActive_ checks protect flash integrity regardless.
    enum class UpdateTarget { None, App, Www };
    UpdateTarget activeTarget_ = UpdateTarget::None;
```

Replace the command declarations + table:

```cpp
    // ── WebSocket commands (registered with CommandManager in Init) ──
    void Cmd_UpdateStatus(Stream& in, Stream& out);
    void Cmd_Partitions(Stream& in, Stream& out);
    void Cmd_UpdateBegin(Stream& in, Stream& out);
    void Cmd_UpdateWrite(Stream& in, Stream& out);
    void Cmd_UpdateEnd(Stream& in, Stream& out);
    void Cmd_UpdateFromUrl(Stream& in, Stream& out);
    void Cmd_DownloadPartition(Stream& in, Stream& out);

    bool WriteActiveChunk(const void* data, size_t size);

    inline static CommandEntry commands_[] = {
        { "updateStatus",      &InvokeCommand<&UpdateManager::Cmd_UpdateStatus> },
        { "partitions",        &InvokeCommand<&UpdateManager::Cmd_Partitions> },
        { "updateBegin",       &InvokeCommand<&UpdateManager::Cmd_UpdateBegin> },
        { "updateWrite",       &InvokeCommand<&UpdateManager::Cmd_UpdateWrite> },
        { "updateEnd",         &InvokeCommand<&UpdateManager::Cmd_UpdateEnd> },
        { "updateFromUrl",     &InvokeCommand<&UpdateManager::Cmd_UpdateFromUrl> },
        { "downloadPartition", &InvokeCommand<&UpdateManager::Cmd_DownloadPartition> },
    };
```

- [ ] **Step 3: UpdateManager.cpp — implement the commands**

Add includes: `#include "JsonReader.h"` and `#include "esp_http_client.h"`.
Append after the existing command section:

```cpp
bool UpdateManager::WriteActiveChunk(const void* data, size_t size)
{
    switch (activeTarget_)
    {
    case UpdateTarget::App: return WriteAppChunk(data, size);
    case UpdateTarget::Www: return WriteWwwChunk(data, size);
    case UpdateTarget::None: return false;
    }
    return false;
}

void UpdateManager::Cmd_UpdateBegin(Stream& in, Stream& out)
{
    JsonReader<256> req(in);
    JsonObject resp(out);

    char target[8] = {};
    req.GetString("target", target, sizeof(target));

    if (activeTarget_ != UpdateTarget::None)
    {
        resp.field("ok", false);
        resp.field("error", "busy");
        return;
    }

    if (strcmp(target, "app") == 0)
    {
        if (!BeginAppUpdate()) { resp.field("ok", false); resp.field("error", "begin failed"); return; }
        activeTarget_ = UpdateTarget::App;
    }
    else if (strcmp(target, "www") == 0)
    {
        if (!BeginWwwUpdate()) { resp.field("ok", false); resp.field("error", "begin failed"); return; }
        activeTarget_ = UpdateTarget::Www;
    }
    else
    {
        resp.field("ok", false);
        resp.field("error", "bad target");
        return;
    }

    resp.field("ok", true);
}

void UpdateManager::Cmd_UpdateWrite(Stream& in, Stream& out)
{
    char buf[1024];

    if (activeTarget_ == UpdateTarget::None)
    {
        while (in.read(buf, sizeof(buf)) > 0) {}   // drain so the transport isn't left mid-body
        JsonObject resp(out);
        resp.field("ok", false);
        resp.field("error", "no session");
        return;
    }

    uint32_t total = 0;
    size_t n;
    while ((n = in.read(buf, sizeof(buf))) > 0)
    {
        if (!WriteActiveChunk(buf, n))
        {
            activeTarget_ = UpdateTarget::None;   // Write*Chunk aborted the session
            while (in.read(buf, sizeof(buf)) > 0) {}
            JsonObject resp(out);
            resp.field("ok", false);
            resp.field("error", "write failed");
            return;
        }
        total += n;
    }

    JsonObject resp(out);
    resp.field("ok", true);
    resp.field("size", total);
}

void UpdateManager::Cmd_UpdateEnd(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const char* err = nullptr;
    switch (activeTarget_)
    {
    case UpdateTarget::App:  err = FinalizeAppUpdate(); break;
    case UpdateTarget::Www:  err = FinalizeWwwUpdate(); break;
    case UpdateTarget::None: err = "no session";        break;
    }
    activeTarget_ = UpdateTarget::None;

    if (err) { resp.field("ok", false); resp.field("error", err); return; }
    resp.field("ok", true);
}

void UpdateManager::Cmd_UpdateFromUrl(Stream& in, Stream& out)
{
    JsonReader<512> req(in);
    JsonObject resp(out);

    char url[256] = {};
    char target[8] = "app";
    if (!req.GetString("url", url, sizeof(url)))
    {
        resp.field("ok", false);
        resp.field("error", "missing url");
        return;
    }
    req.GetString("target", target, sizeof(target));
    bool isApp = (strcmp(target, "www") != 0);

    if (activeTarget_ != UpdateTarget::None)
    {
        resp.field("ok", false);
        resp.field("error", "busy");
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { resp.field("ok", false); resp.field("error", "client init failed"); return; }

    bool began = false;
    const char* err = nullptr;
    uint32_t total = 0;

    do
    {
        if (esp_http_client_open(client, 0) != ESP_OK) { err = "connect failed"; break; }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) { err = "http status"; break; }

        began = isApp ? BeginAppUpdate() : BeginWwwUpdate();
        if (!began) { err = "begin failed"; break; }

        char buf[1024];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0)
        {
            bool ok = isApp ? WriteAppChunk(buf, n) : WriteWwwChunk(buf, n);
            if (!ok) { err = "write failed"; began = false; break; }   // Write*Chunk aborted
            total += n;
        }
        if (err) break;
        if (n < 0) { err = "read failed"; break; }

        err = isApp ? FinalizeAppUpdate() : FinalizeWwwUpdate();
        began = false;
    } while (false);

    if (began)   // opened a session but bailed before finalize consumed it
    {
        LOCK(mutex_);
        if (isApp) AbortOta();      // AbortOta expects mutex_ held (as in WriteAppChunk)
        else       wwwActive_ = false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err) { resp.field("ok", false); resp.field("error", err); return; }
    resp.field("ok", true);
    resp.field("size", total);
    ESP_LOGI(TAG, "Pull update from %s complete (%lu bytes)", url, (unsigned long)total);
}

void UpdateManager::Cmd_DownloadPartition(Stream& in, Stream& out)
{
    JsonReader<256> req(in);

    char label[17] = {};
    req.GetString("partition", label, sizeof(label));

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p)
    {
        JsonObject resp(out);
        resp.field("ok", false);
        resp.field("error", "unknown partition");
        return;
    }

    ESP_LOGI(TAG, "Download partition '%s' (%lu bytes)", label, (unsigned long)p->size);

    uint8_t buf[4096];
    size_t offset = 0;
    while (offset < p->size)
    {
        size_t n = (p->size - offset < sizeof(buf)) ? (p->size - offset) : sizeof(buf);
        if (esp_partition_read(p, offset, buf, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return;
        }
        if (out.write(buf, n) != n)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return;
        }
        offset += n;
    }
}
```


- [ ] **Step 4: Build**

Run the build command. Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add main/Application/UpdateManager main/CMakeLists.txt
git commit -m "UpdateManager: session-based update commands, pull OTA, partition download"
```

---

### Task 2: WebServerManager — /api/command route, bespoke routes deleted

**Files:**
- Modify: `main/Application/WebServerManager/WebServerManager.h`
- Modify: `main/Application/WebServerManager/WebServerManager.cpp`

**Interfaces:**
- Consumes: `CommandManager::Execute(type, in, out)`; `Stream`.
- Produces: `POST /api/command?type=<name>` — request body = command payload (streamed in), response body = handler reply (chunked out), `Content-Type: application/octet-stream`, CORS as before, 404 on unknown command, 400 on missing `type`. `OPTIONS /api/command` preflight. Routes `/api/upload/app`, `/api/upload/www`, `/api/download` REMOVED.

- [ ] **Step 1: WebServerManager.h — swap handler declarations**

Replace:

```cpp
    static esp_err_t HandleUploadApp(httpd_req_t* req);
    static esp_err_t HandleUploadWww(httpd_req_t* req);
    static esp_err_t HandleDownloadPartition(httpd_req_t* req);
```

with:

```cpp
    static esp_err_t HandleApiCommand(httpd_req_t* req);
```

- [ ] **Step 2: WebServerManager.cpp — routes**

Remove `#include "UpdateManager.h"`; add `#include "CommandManager.h"`
(already present) and `#include "Stream.h"`.

In `RegisterRoutes()`, replace the four upload/download/preflight
registrations (`upload_app`, `upload_www`, `upload_app_opts`,
`upload_www_opts`, `download`) with:

```cpp
    // The one API route: every command, streamed both ways.
    const httpd_uri_t api_command = {
        .uri = "/api/command",
        .method = HTTP_POST,
        .handler = HandleApiCommand,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &api_command);

    const httpd_uri_t api_command_opts = {
        .uri = "/api/command",
        .method = HTTP_OPTIONS,
        .handler = HandleCorsPreflight,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &api_command_opts);
```

- [ ] **Step 3: WebServerManager.cpp — replace the three handlers with the adapter + dispatch**

Delete `HandleUploadApp`, `HandleUploadWww`, `HandleDownloadPartition`
entirely. Add in their place:

```cpp
// ──────────────────────────────────────────────────────────────
// /api/command — the generic command entrance. Request body in,
// handler reply out, streamed both directions. The HTTP layer knows
// no commands; HTTP status is only the transport envelope.
// ──────────────────────────────────────────────────────────────

namespace {

class HttpRequestStream : public Stream
{
    httpd_req_t* req_;
    int remaining_;

public:
    explicit HttpRequestStream(httpd_req_t* req)
        : req_(req), remaining_(req->content_len) {}

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;   // httpd's socket recv timeout applies
        if (remaining_ <= 0) return 0;
        int want = (size < static_cast<size_t>(remaining_)) ? static_cast<int>(size) : remaining_;
        int n = httpd_req_recv(req_, static_cast<char*>(dst), want);
        if (n <= 0) { remaining_ = 0; return 0; }
        remaining_ -= n;
        return static_cast<size_t>(n);
    }

    size_t write(const void*, size_t, TickType_t) override { return 0; }
    size_t available() const override { return remaining_ > 0 ? remaining_ : 0; }
};

class HttpResponseStream : public Stream
{
    httpd_req_t* req_;
    bool failed_ = false;

public:
    explicit HttpResponseStream(httpd_req_t* req) : req_(req) {}

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        if (failed_) return 0;
        if (httpd_resp_send_chunk(req_, static_cast<const char*>(data), size) != ESP_OK)
        {
            failed_ = true;
            return 0;
        }
        return size;
    }

    size_t read(void*, size_t, TickType_t) override { return 0; }
    bool failed() const { return failed_; }
};

} // namespace

esp_err_t WebServerManager::HandleApiCommand(httpd_req_t* req)
{
    auto* self = static_cast<WebServerManager*>(req->user_ctx);

    char query[96] = {};
    char type[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "type", type, sizeof(type)) != ESP_OK ||
        type[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?type=");
        return ESP_FAIL;
    }

    SetCorsHeaders(req);
    httpd_resp_set_type(req, "application/octet-stream");

    HttpRequestStream in(req);
    HttpResponseStream out(req);

    if (!self->serviceProvider_.getCommandManager().Execute(type, in, out))
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown command");
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, nullptr, 0);   // finish chunked response
    return out.failed() ? ESP_FAIL : ESP_OK;
}
```

- [ ] **Step 4: Build**

Run the build command. Expected: `Project build complete`. WebServerManager
now has zero UpdateManager knowledge — grep to confirm:
`grep -r "UpdateManager" main/Application/WebServerManager/` → no matches.

- [ ] **Step 5: Commit**

```bash
git add main/Application/WebServerManager
git commit -m "Replace bespoke upload/download routes with generic /api/command"
```

---

### Task 3: Frontend — firmware page over the command path

**Files:**
- Modify: `frontend/src/lib/backend.ts:240-289` (upload/download methods)
- Modify: `frontend/src/pages/FirmwarePage.tsx:13-18,109-128,171-176`

**Interfaces:**
- Consumes: `updateBegin`/`updateWrite`/`updateEnd`/`downloadPartition` (Task 1 payloads); `POST /api/command?type=` (Task 2).
- Produces: `backend.uploadPartition(target, file, onProgress)` and `backend.downloadPartitionFile(label)`; `UploadResult` unchanged.

- [ ] **Step 1: backend.ts — replace the upload/download API**

Replace `partitionDownloadUrl`, `uploadFirmware`, `uploadWww`, and the
private `upload` method with:

```ts
  private commandUrl(type: string): string {
    const host = import.meta.env.DEV ? `http://${DEV_HOST}` : ""
    return `${host}/api/command?type=${encodeURIComponent(type)}`
  }

  /** Upload a .bin into an update session: begin (WS) → write (HTTP, streamed) → end (WS). */
  async uploadPartition(
    target: "app" | "www",
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    const begin = await this.send<{ ok: boolean; error?: string }>("updateBegin", { target })
    if (!begin.ok) throw new Error(begin.error ?? "updateBegin failed")

    try {
      const write = await this.postCommand("updateWrite", file, onProgress)
      if (!write.ok) throw new Error(write.error ?? "updateWrite failed")

      const end = await this.send<{ ok: boolean; error?: string }>("updateEnd")
      if (!end.ok) throw new Error(end.error ?? "updateEnd failed")

      return { ok: true, size: write.size ?? file.size }
    } catch (e) {
      // Best effort: close a dangling session so the next attempt isn't "busy".
      this.send("updateEnd").catch(() => {})
      throw e
    }
  }

  /** Fetch a partition image through the command pipe and save it as <label>.bin. */
  async downloadPartitionFile(label: string): Promise<void> {
    const res = await fetch(this.commandUrl("downloadPartition"), {
      method: "POST",
      body: JSON.stringify({ partition: label }),
    })
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
    const blob = await res.blob()
    const text = blob.size < 256 ? await blob.slice(0, 256).text() : ""
    if (text.startsWith('{"ok":false')) throw new Error(JSON.parse(text).error ?? "download failed")

    const url = URL.createObjectURL(blob)
    const a = document.createElement("a")
    a.href = url
    a.download = `${label}.bin`
    a.click()
    URL.revokeObjectURL(url)
  }

  private postCommand(
    type: string,
    body: Blob,
    onProgress?: (percent: number) => void,
  ): Promise<{ ok: boolean; size?: number; error?: string }> {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest()
      xhr.open("POST", this.commandUrl(type))

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) {
          onProgress(Math.round((e.loaded / e.total) * 100))
        }
      }

      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(JSON.parse(xhr.responseText))
        } else {
          reject(new Error(xhr.responseText || `${xhr.status} ${xhr.statusText}`))
        }
      }

      xhr.onerror = () => reject(new Error("Upload failed"))
      xhr.ontimeout = () => reject(new Error("Upload timed out"))
      xhr.timeout = 120000

      xhr.send(body)
    })
  }
```

- [ ] **Step 2: FirmwarePage.tsx — use the new API**

Replace `chooseUploadFn` (lines 13-18):

```ts
function uploadTarget(p: Partition): "app" | "www" | null {
  if (!p.uploadable) return null
  if (p.type === "app") return "app"
  if (p.label === "www") return "www"
  return null
}
```

In `PartitionRow`, replace the `uploadFn` wiring (lines 109-111):

```ts
  const target = uploadTarget(p)
  const canUpload = !!target && !p.running
  const uploading = progress !== null
```

and in `onFileChosen` (lines 113-128) replace the guard + call:

```ts
  async function onFileChosen(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    e.target.value = ""
    if (!file || !target) return

    setError(null)
    setProgress(0)
    try {
      await backend.uploadPartition(target, file, setProgress)
      onAfterUpload()
    } catch (err) {
      setError(err instanceof Error ? err.message : "Upload failed")
    } finally {
      setProgress(null)
    }
  }
```

Update the Upload button's disabled/title logic to use `target` instead
of `uploadFn` (line 158-166: `!uploadFn` → `!target`). Replace the
Download link (lines 171-176):

```tsx
          <Button
            variant="outline"
            size="sm"
            onClick={() => backend.downloadPartitionFile(p.label).catch((e) => setError(e.message))}
          >
            <DownloadIcon className="mr-1.5 size-3.5" />
            Download
          </Button>
```

- [ ] **Step 3: Typecheck + builds**

```bash
cd frontend
pnpm typecheck
pnpm build
```

then the firmware build command. Expected: all green.

- [ ] **Step 4: Commit**

```bash
git add frontend/src
git commit -m "Firmware page uploads/downloads through /api/command"
```

---

### Task 4: Flash and verify on device

Flash: `idf.py -p COM3 flash`. Then:

- [ ] Boot log clean (all managers `Initialized`, no FATAL).
- [ ] WS regression: run the phase-1 protocol test (ping/info/settings/
      logs/updateStatus/partitions/wifiScan/unknown) — all PASS.
- [ ] `downloadPartition` via HTTP: POST `{"partition":"nvs"}` to
      `/api/command?type=downloadPartition` → 24 KB binary.
- [ ] Unknown command over HTTP → 404; missing `?type=` → 400.
- [ ] Session guards: `updateWrite` with no session → `no session`;
      double `updateBegin` → `busy`; `updateEnd` clears it.
- [ ] Full app OTA through the UI: upload the built `Strux.bin` to the
      non-running slot, progress bar moves, reboot, device comes back on
      the new slot (`updateStatus.running` flips).
- [ ] `www` upload through the UI with `build/www.bin` (or skip if no
      image at hand).
- [ ] `updateFromUrl`: serve `build/Strux.bin` locally
      (`python -m http.server 8000` in `build/`), then over WS:
      `updateFromUrl {"url":"http://<pc-ip>:8000/Strux.bin"}` →
      `{"ok":true,...}`, reboot flips slots again.
- [ ] Partition download via the UI Download button saves `<label>.bin`.

---

## Self-review notes

- Spec coverage: UpdateManager command table incl. `updateFromUrl` (T1),
  bespoke route deletion + one `/api/command` route with streaming +
  CORS + POST-only (T2), fetch-blob downloads (T3), phase-2 verification
  incl. pull OTA (T4). WebServerManager's remaining manager deps:
  CommandManager + ConsoleManager broadcast — matches spec.
- `downloadPartition` raw-reply-over-HTTP works because HTTP has no JSON
  envelope (body IS the reply); over WS it would corrupt the payload
  envelope — documented spec limitation (bulk goes over HTTP).
- `Cmd_UpdateWrite`'s error paths construct `JsonObject` only after
  draining, so the reply is one complete JSON value in all cases.
- Frontend `UploadResult`/callers: `HomePage`/`SettingsPage` don't touch
  upload APIs; only `FirmwarePage` does.
