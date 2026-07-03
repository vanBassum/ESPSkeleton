# Stream Commands Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Change the command handler contract from `(const char* json, JsonWriter& resp)` to `(Stream& in, Stream& out)` and migrate every existing handler — the device behaves exactly as before.

**Architecture:** Streams are the contract, JSON the dialect (spec: `docs/superpowers/specs/2026-07-03-stream-commands-design.md`, code sketch: `ideas/stream-commands-example.h`). New lib adapters: `MemoryStream` (memory-backed read stream), `JsonReader<N>` (buffered request parsing), `JsonObject`/`JsonArray` (RAII scope writers). WebSocketHandler moves the response envelope from flat to nested `{"id":N,"payload":{...}}`; `backend.ts` unwraps it.

**Tech Stack:** ESP-IDF v6.0, C++17, no heap/exceptions in registries; frontend React 19 + TypeScript (pnpm).

## Global Constraints

- No heap allocation in any new lib class (fixed/internal buffers only).
- Header-only lib additions (matches `Stream.h`/`BufferStream.h`/`JsonWriter.h` style) — no `main/CMakeLists.txt` changes needed.
- Registry misuse dies via `FATAL(...)` from `lib/common/Fatal.h` (survives NDEBUG).
- There are no automated tests; every task's verification is a firmware build. Build command (PowerShell):
  `$env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; Set-Location C:\Workspace\Strux; idf.py build`
- Commit after every task. Branch: `modular-managers`.
- Phase 2 (pure UpdateManager, `/api/command` route) is explicitly OUT of scope.

---

### Task 1: MemoryStream

**Files:**
- Create: `main/lib/common/MemoryStream.h`

**Interfaces:**
- Produces: `MemoryStream(const void* buf, size_t len)` — read-only `Stream` over existing bytes. Task 4 (WebSocketHandler) constructs it around a received frame.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include "Stream.h"
#include <algorithm>
#include <cstring>

// A stream backed by memory: a read-only Stream view over an existing
// byte range. NOT the same thing as BufferStream — that one is about
// buffering/chunking I/O; this one adapts bytes-already-in-RAM (a
// received WebSocket frame, a serial line buffer) to a Stream consumer.
class MemoryStream : public Stream
{
    const uint8_t* buf_;
    size_t len_;
    size_t pos_ = 0;

public:
    MemoryStream(const void* buf, size_t len)
        : buf_(static_cast<const uint8_t*>(buf)), len_(len) {}

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;   // data is already in memory; never blocks
        size_t n = std::min(size, len_ - pos_);
        memcpy(dst, buf_ + pos_, n);
        pos_ += n;
        return n;
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)data; (void)size; (void)timeout;
        return 0;   // read-only view
    }

    size_t available() const override { return len_ - pos_; }
};
```

- [ ] **Step 2: Build**

Run the build command from Global Constraints. Expected: `Project build complete`.

- [ ] **Step 3: Commit**

```bash
git add main/lib/common/MemoryStream.h
git commit -m "Add MemoryStream: read-only Stream view over memory"
```

---

### Task 2: JsonReader

**Files:**
- Create: `main/lib/json/JsonReader.h`

**Interfaces:**
- Consumes: `Stream::read`, `Stream::available` (`lib/common/Stream.h`); `ExtractJsonString`, `ExtractJsonInt`, `FindJsonField` (`lib/json/JsonHelpers.h`).
- Produces: `JsonReader<N> req(in)` with `bool GetString(key, out, maxLen)`, `int32_t GetInt(key, def=0)`, `bool GetBool(key, def=false)`. Task 4 handlers use it.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include "Stream.h"
#include "JsonHelpers.h"
#include "esp_log.h"
#include <cstddef>
#include <cstdint>

// Buffered JSON request reader. Consumes the request stream into an
// internal bounded buffer at construction, then serves typed getters
// (the JsonHelpers free functions as methods). Deliberately NOT a
// streaming parser — command payloads are a few hundred bytes; a
// streaming implementation can replace the internals later without
// touching any handler.
template <size_t N = 1024>
class JsonReader
{
    char buf_[N];
    size_t len_ = 0;

public:
    explicit JsonReader(Stream& in)
    {
        // Timeout 0: in phase 1 `in` is always a MemoryStream (data
        // already in RAM). Revisit for socket-backed streams in phase 2.
        while (len_ < N - 1)
        {
            size_t n = in.read(buf_ + len_, N - 1 - len_, 0);
            if (n == 0) break;
            len_ += n;
        }
        buf_[len_] = '\0';

        if (in.available() > 0)
            ESP_LOGE("JsonReader", "Request truncated: capacity %u exhausted, %u bytes left unread",
                     static_cast<unsigned>(N), static_cast<unsigned>(in.available()));
    }

    JsonReader(const JsonReader&) = delete;
    JsonReader& operator=(const JsonReader&) = delete;

    bool GetString(const char* key, char* out, size_t maxLen) const
    {
        return ExtractJsonString(buf_, key, out, maxLen);
    }

    int32_t GetInt(const char* key, int32_t def = 0) const
    {
        return ExtractJsonInt(buf_, key, def);
    }

    bool GetBool(const char* key, bool def = false) const
    {
        const char* v = FindJsonField(buf_, key);
        if (!v) return def;
        return *v == 't';   // JSON literals: true / false / null
    }
};
```

- [ ] **Step 2: Build**

Run the build command. Expected: `Project build complete`.

- [ ] **Step 3: Commit**

```bash
git add main/lib/json/JsonReader.h
git commit -m "Add JsonReader: buffered request parsing with typed getters"
```

---

### Task 3: JsonObject / JsonArray RAII scopes

**Files:**
- Create: `main/lib/json/JsonScope.h`

**Interfaces:**
- Consumes: `Stream::write`; `FATAL` (`lib/common/Fatal.h`).
- Produces (Task 4 handlers rely on exactly these):
  - `JsonObject root(out)` — root scope, writes `{`, writes `}` on destruction.
  - `JsonObject::field(const char* key, const char*|int32_t|uint32_t|float|bool)`
  - `JsonObject JsonObject::object(const char* key)`, `JsonArray JsonObject::array(const char* key)`
  - `JsonArray::value(const char*|int32_t|bool)`, `JsonObject JsonArray::object()`
  - Semantics: writing to a parent auto-closes its open child (cascading depth-first); opening a second child auto-closes the first; writing to a closed/invalidated scope → `FATAL`; destructor of an invalidated scope is a no-op; copy/assign deleted.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include "Stream.h"
#include "Fatal.h"
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstring>

// ──────────────────────────────────────────────────────────────
// RAII JSON scope writers. A scope writes its opener at construction
// and its closer at destruction; locals die in reverse declaration
// order, so inner braces close first and EVERY return path yields
// well-formed JSON.
//
// Rules:
//   - writing to a parent while a child is open is LEGAL and means
//     "done with the child": the parent auto-closes it (cascading)
//   - opening a second child auto-closes the first
//   - writing to a closed scope → FATAL (abandoned-scope use is a bug)
//   - destructor of a closed scope: no-op
//
// JsonWriter remains for legacy callers (MQTT discovery); new code
// uses these scopes.
// ──────────────────────────────────────────────────────────────

class JsonScope
{
protected:
    Stream* out_;                 // nullptr = closed/invalidated
    JsonScope* parent_;
    JsonScope* openChild_ = nullptr;
    bool needsComma_ = false;
    const char closer_;

    JsonScope(Stream& out, JsonScope* parent, char opener, char closer)
        : out_(&out), parent_(parent), closer_(closer)
    {
        out_->write(&opener, 1);
        if (parent_)
            parent_->openChild_ = this;
    }

    // FATAL on closed scope; auto-close open child; comma bookkeeping.
    void Prepare()
    {
        if (!out_)
            FATAL("JSON scope used after close");
        if (openChild_)
            openChild_->Close();
        if (needsComma_)
            out_->write(",", 1);
        needsComma_ = true;
    }

    void Close()
    {
        if (!out_) return;
        if (openChild_)
            openChild_->Close();          // cascades depth-first
        out_->write(&closer_, 1);
        if (parent_)
            parent_->openChild_ = nullptr;
        out_ = nullptr;
    }

    void WriteEscaped(const char* s)
    {
        out_->write("\"", 1);
        for (const char* p = s; *p; p++)
        {
            switch (*p)
            {
            case '"':  out_->write("\\\"", 2); break;
            case '\\': out_->write("\\\\", 2); break;
            case '\n': out_->write("\\n", 2); break;
            case '\r': out_->write("\\r", 2); break;
            case '\t': out_->write("\\t", 2); break;
            default:
                if (static_cast<uint8_t>(*p) >= 0x20)
                    out_->write(p, 1);
                break;
            }
        }
        out_->write("\"", 1);
    }

    void WriteRaw(const char* s) { out_->write(s, strlen(s)); }

public:
    ~JsonScope() { Close(); }

    JsonScope(const JsonScope&) = delete;
    JsonScope& operator=(const JsonScope&) = delete;
};

class JsonArray;

class JsonObject : public JsonScope
{
    friend class JsonArray;

    JsonObject(Stream& out, JsonScope* parent) : JsonScope(out, parent, '{', '}') {}

    void WriteKey(const char* key)
    {
        Prepare();
        WriteEscaped(key);
        out_->write(":", 1);
    }

public:
    explicit JsonObject(Stream& out) : JsonScope(out, nullptr, '{', '}') {}

    void field(const char* key, const char* v) { WriteKey(key); WriteEscaped(v); }

    void field(const char* key, int32_t v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        WriteRaw(buf);
    }

    void field(const char* key, uint32_t v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, v);
        WriteRaw(buf);
    }

    void field(const char* key, float v)
    {
        WriteKey(key);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        WriteRaw(buf);
    }

    void field(const char* key, bool v) { WriteKey(key); WriteRaw(v ? "true" : "false"); }

    inline JsonObject object(const char* key);
    inline JsonArray array(const char* key);
};

class JsonArray : public JsonScope
{
    friend class JsonObject;

    JsonArray(Stream& out, JsonScope* parent) : JsonScope(out, parent, '[', ']') {}

public:
    void value(const char* v) { Prepare(); WriteEscaped(v); }

    void value(int32_t v)
    {
        Prepare();
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        WriteRaw(buf);
    }

    void value(bool v) { Prepare(); WriteRaw(v ? "true" : "false"); }

    JsonObject object()
    {
        Prepare();
        return JsonObject(*out_, this);   // guaranteed copy elision
    }
};

inline JsonObject JsonObject::object(const char* key)
{
    WriteKey(key);
    return JsonObject(*out_, this);
}

inline JsonArray JsonObject::array(const char* key)
{
    WriteKey(key);
    return JsonArray(*out_, this);
}
```

Note on `object()`/`array()` returning by value with deleted copy: the
return expressions are prvalues, so C++17 guaranteed copy elision
constructs them directly in the caller's variable — no copy, no move,
one destructor. Subtlety in `Prepare()` before constructing the child:
it sets `needsComma_ = true` for the PARENT (correct — the next parent
element needs a comma after the child), then the child registers
itself via the `JsonScope` constructor.

- [ ] **Step 2: Build**

Run the build command. Expected: `Project build complete`.

- [ ] **Step 3: Commit**

```bash
git add main/lib/json/JsonScope.h
git commit -m "Add JsonObject/JsonArray RAII scopes with auto-close semantics"
```

---

### Task 4: Switch the command contract to streams and migrate all handlers

Everything in this task is one atomic change — the signature switch does
not compile piecemeal. Edit all files, then build once.

**Files:**
- Modify: `main/Application/CommandManager/CommandEntry.h`
- Modify: `main/Application/CommandManager/CommandManager.h:56-59` (Execute decl)
- Modify: `main/Application/CommandManager/CommandManager.cpp:24-35` (Execute def)
- Modify: `main/Application/WebServerManager/WebSocketHandler.cpp:195-219` (DispatchMessage)
- Modify: `main/Application/WebServerManager/WebSocketHandler.h:36`
- Modify: `main/Application/SystemManager/SystemManager.h` + `.cpp`
- Modify: `main/Application/SettingsManager/SettingsManager.h` + `.cpp`
- Modify: `main/Application/NetworkManager/NetworkManager.h` + `.cpp`
- Modify: `main/Application/ConsoleManager/ConsoleManager.h` + `.cpp`
- Modify: `main/Application/UpdateManager/UpdateManager.h` + `.cpp`

**Interfaces:**
- Consumes: `MemoryStream` (Task 1), `JsonReader` (Task 2), `JsonObject`/`JsonArray` (Task 3).
- Produces: `CommandEntry::handler` = `void (*)(void* ctx, Stream& in, Stream& out)`; `bool CommandManager::Execute(const char* type, Stream& in, Stream& out)`; WS wire format `{"id":N,"payload":<handler object>}` on success, `{"id":N,"error":"<type>"}` on unknown command. Task 5 (frontend) relies on the wire format.

- [ ] **Step 1: CommandEntry.h — new handler type + trampoline**

Replace the handler field and the trampoline section (keep the FATAL
destructor and the registration comments as they are):

```cpp
struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, Stream& in, Stream& out);

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    ~CommandEntry()
    {
        if (registered)
            FATAL("registered command '%s' destroyed — command tables must "
                  "live for the whole application", name);
    }
};

template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<void (C::*)(Stream&, Stream&)>       { using type = C; };
template <typename C> struct CommandOwner<void (C::*)(Stream&, Stream&) const> { using type = const C; };

template <auto Handler>
void InvokeCommand(void* ctx, Stream& in, Stream& out)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename CommandOwner<decltype(Handler)>::type;
        (static_cast<C*>(ctx)->*Handler)(in, out);
    }
    else
    {
        Handler(in, out);   // free/static function — ctx unused
    }
}
```

At the top of the file replace `class JsonWriter;` with `class Stream;`
and update the doc comment ("Handlers are plain function pointers"
paragraph) to mention the `(Stream& in, Stream& out)` shape.

- [ ] **Step 2: CommandManager Execute signature**

`CommandManager.h`: replace `class JsonWriter;` with `class Stream;` and:

```cpp
    /// Execute a command by type name. `in` carries the request payload;
    /// the handler writes its complete reply (e.g. one JSON object) to
    /// `out`. The caller owns any transport envelope around it.
    /// Returns true if the command was recognized.
    bool Execute(const char* type, Stream& in, Stream& out);
```

`CommandManager.cpp`: drop the `#include "JsonWriter.h"` and:

```cpp
bool CommandManager::Execute(const char* type, Stream& in, Stream& out)
{
    const CommandEntry* e = Find(type);
    if (e == nullptr)
        return false;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    e->handler(e->ctx, in, out);
    return true;
}
```

- [ ] **Step 3: WebSocketHandler — nested payload envelope**

`WebSocketHandler.h:36`: change the declaration to

```cpp
    void DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json);
```

(unchanged shape — listed for clarity). In `WebSocketHandler.cpp`
replace `DispatchMessage` and add includes `#include "MemoryStream.h"`
and `#include <cinttypes>`:

```cpp
void WebSocketHandler::DispatchMessage(httpd_req_t* req, int32_t id, const char* type, const char* json)
{
    BufferStream out(wsBuf_, sizeof(wsBuf_));

    char head[48];
    int n = snprintf(head, sizeof(head), "{\"id\":%" PRId32 ",\"payload\":", id);
    out.write(head, n);

    MemoryStream in(json, strlen(json));

    if (commandManager_ && commandManager_->Execute(type, in, out))
    {
        out.write("}", 1);
    }
    else
    {
        out.reset();
        JsonWriter err(out);   // reuse JsonWriter's escaping for the type echo
        err.beginObject();
        err.field("id", id);
        err.field("error", type);
        err.endObject();
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(out.data()));
    frame.len = out.length();
    httpd_ws_send_frame(req, &frame);
}
```

- [ ] **Step 4: SystemManager (3 handlers)**

`SystemManager.h`: `class JsonWriter;` → `class Stream;`; declarations:

```cpp
    void Cmd_Ping(Stream& in, Stream& out);
    void Cmd_Info(Stream& in, Stream& out);
    void Cmd_Reboot(Stream& in, Stream& out);
```

(The `commands_` table lines are unchanged — the trampoline adapts.)

`SystemManager.cpp`: replace `#include "JsonWriter.h"` with
`#include "JsonScope.h"`; handler bodies:

```cpp
void SystemManager::Cmd_Ping(Stream& in, Stream& out)
{
    JsonObject resp(out);
    resp.field("pong", true);
}

void SystemManager::Cmd_Info(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("project", app->project_name);
    resp.field("firmware", app->version);
    resp.field("idf", app->idf_ver);
    resp.field("date", app->date);
    resp.field("time", app->time);
    resp.field("chip", CONFIG_IDF_TARGET);
    resp.field("heapFree", static_cast<uint32_t>(esp_get_free_heap_size()));
    resp.field("heapMin", static_cast<uint32_t>(esp_get_minimum_free_heap_size()));

    char deviceTimeStr[32] = "Not synced";
    DateTime now = DateTime::Now();
    if (now.YearLocal() >= 2020)
        now.ToStringLocal(deviceTimeStr, sizeof(deviceTimeStr), "%F %T");
    resp.field("deviceTime", deviceTimeStr);
}

void SystemManager::Cmd_Reboot(Stream& in, Stream& out)
{
    {
        JsonObject resp(out);
        resp.field("ok", true);
    }   // close the scope BEFORE restarting so the reply is complete

    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
```

- [ ] **Step 5: SettingsManager (3 handlers)**

`SettingsManager.h`: `class JsonWriter;` → `class Stream;`; declarations:

```cpp
    void Cmd_GetSettings(Stream& in, Stream& out);
    void Cmd_SetSetting(Stream& in, Stream& out);
    void Cmd_SaveSettings(Stream& in, Stream& out);
```

`SettingsManager.cpp`: replace includes `#include "JsonWriter.h"` and
`#include "JsonHelpers.h"` with `#include "JsonScope.h"` and
`#include "JsonReader.h"`; handler bodies:

```cpp
void SettingsManager::Cmd_GetSettings(Stream& in, Stream& out)
{
    JsonObject root(out);
    JsonArray settings = root.array("settings");

    for (const Setting& s : *this)
    {
        JsonObject o = settings.object();
        o.field("key", s.key);
        o.field("label", s.label);
        o.field("type", SettingTypeToString(s.type));

        switch (s.type)   // NO default → new SettingType values must be handled here
        {
        case SettingType::Int32:  o.field("value", s.asInt32().Get());  break;
        case SettingType::UInt32: o.field("value", s.asUInt32().Get()); break;
        case SettingType::Float:  o.field("value", s.asFloat().Get());  break;
        case SettingType::Bool:   o.field("value", s.asBool().Get());   break;
        case SettingType::String:
        {
            char buf[128] = {};
            s.asString().Get(buf, sizeof(buf));
            o.field("value", buf);
            break;
        }
        }
    }   // each `o` closes at end of iteration; `settings` and `root` at return
}

void SettingsManager::Cmd_SetSetting(Stream& in, Stream& out)
{
    JsonReader<512> req(in);
    JsonObject resp(out);

    char key[64] = {};
    char value[128] = {};
    req.GetString("key", key, sizeof(key));
    req.GetString("value", value, sizeof(value));

    if (key[0] == '\0')
    {
        resp.field("ok", false);
        resp.field("error", "missing key");
        return;
    }

    for (Setting& s : *this)
    {
        if (strcmp(s.key, key) != 0)
            continue;

        bool ok = false;
        switch (s.type)   // NO default → new SettingType values must be handled here
        {
        case SettingType::Int32:
            ok = s.asInt32().Set(static_cast<int32_t>(strtol(value, nullptr, 10)));
            break;
        case SettingType::UInt32:
            ok = s.asUInt32().Set(static_cast<uint32_t>(strtoul(value, nullptr, 10)));
            break;
        case SettingType::Float:
            ok = s.asFloat().Set(strtof(value, nullptr));
            break;
        case SettingType::Bool:
            ok = s.asBool().Set(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            break;
        case SettingType::String:
            ok = s.asString().Set(value);
            break;
        }

        resp.field("ok", ok);
        return;
    }

    resp.field("ok", false);
    resp.field("error", "unknown key");
}

void SettingsManager::Cmd_SaveSettings(Stream& in, Stream& out)
{
    JsonObject resp(out);
    resp.field("ok", Save());
}
```

- [ ] **Step 6: NetworkManager (1 handler)**

`NetworkManager.h`: `class JsonWriter;` → `class Stream;` (if JsonWriter
is only forward-declared for the handler);
`void Cmd_WifiScan(Stream& in, Stream& out);`

`NetworkManager.cpp`: swap `#include "JsonWriter.h"` for
`#include "JsonScope.h"`; body:

```cpp
void NetworkManager::Cmd_WifiScan(Stream& in, Stream& out)
{
    WiFiInterface::ScanResult results[20] = {};
    int count = wifi().Scan(results, 20);

    JsonObject root(out);
    root.field("ok", true);
    JsonArray networks = root.array("networks");

    for (int i = 0; i < count; i++)
    {
        JsonObject n = networks.object();
        n.field("ssid", results[i].ssid);
        n.field("rssi", static_cast<int32_t>(results[i].rssi));
        n.field("channel", static_cast<int32_t>(results[i].channel));
        n.field("secure", results[i].secure);
    }
}
```

- [ ] **Step 7: ConsoleManager (1 handler + WriteHistory helper)**

`ConsoleManager.h`: forward-declare `class JsonObject;` and
`class Stream;` (replacing `class JsonWriter;` if only used here);
change:

```cpp
    void WriteHistory(JsonObject& resp) const;
    ...
    void Cmd_GetLogs(Stream& in, Stream& out);
```

`ConsoleManager.cpp`: swap `#include "JsonWriter.h"` for
`#include "JsonScope.h"`; bodies:

```cpp
void ConsoleManager::WriteHistory(JsonObject& resp) const
{
    LOCK(mutex_);

    JsonArray lines = resp.array("lines");

    int32_t start = (count_ < MAX_LINES) ? 0 : head_;
    for (int32_t i = 0; i < count_; i++)
    {
        int32_t idx = (start + i) % MAX_LINES;
        lines.value(lines_[idx]);
    }
}   // `lines` closes here; caller's `resp` stays usable (auto-detached)

void ConsoleManager::Cmd_GetLogs(Stream& in, Stream& out)
{
    JsonObject resp(out);
    WriteHistory(resp);
}
```

- [ ] **Step 8: UpdateManager (2 handlers)**

`UpdateManager.h`: `class JsonWriter;` → `class Stream;`; declarations:

```cpp
    void Cmd_UpdateStatus(Stream& in, Stream& out);
    void Cmd_Partitions(Stream& in, Stream& out);
```

`UpdateManager.cpp`: swap `#include "JsonWriter.h"` for
`#include "JsonScope.h"`; bodies:

```cpp
void UpdateManager::Cmd_UpdateStatus(Stream& in, Stream& out)
{
    JsonObject resp(out);

    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("firmware", app->version);
    resp.field("running", GetRunningPartition());
    resp.field("nextSlot", GetNextPartition());
}

void UpdateManager::Cmd_Partitions(Stream& in, Stream& out)
{
    static constexpr int MAX_PARTITIONS = 16;
    PartitionInfo parts[MAX_PARTITIONS];
    int count = GetPartitions(parts, MAX_PARTITIONS);

    JsonObject root(out);
    JsonArray arr = root.array("partitions");

    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        JsonObject o = arr.object();
        o.field("label",      p.label);
        o.field("type",       p.type);
        o.field("subtype",    p.subtype);
        o.field("offset",     p.offset);
        o.field("size",       p.size);
        o.field("running",    p.running);
        o.field("nextOta",    p.nextOta);
        o.field("uploadable", p.uploadable);
        o.field("version",    p.version);
    }
}
```

- [ ] **Step 9: Build**

Run the build command. Expected: `Project build complete`. If a handler
was missed, the compiler stops at its `commands_` table line with "no
matching `CommandOwner` specialization" — fix and rebuild.

- [ ] **Step 10: Commit**

```bash
git add main/Application
git commit -m "Switch command contract to (Stream& in, Stream& out), migrate all handlers"
```

---

### Task 5: Frontend — unwrap the payload envelope

**Files:**
- Modify: `frontend/src/lib/backend.ts:101-116` (onmessage handler)

**Interfaces:**
- Consumes: WS wire format from Task 4: `{"id":N,"payload":{...}}` success, `{"id":N,"error":"..."}` failure, broadcasts (no `id`) unchanged.
- Produces: `send<T>()` still resolves to the handler's object — pages are untouched.

- [ ] **Step 1: Change the resolve line**

In `backend.ts`, inside `ws.onmessage`, replace:

```ts
              if (msg.error) {
                req.reject(new Error(msg.error))
              } else {
                req.resolve(msg)
              }
```

with:

```ts
              if (msg.error) {
                req.reject(new Error(msg.error))
              } else {
                req.resolve(msg.payload)
              }
```

- [ ] **Step 2: Typecheck and build the frontend**

```bash
cd frontend
pnpm typecheck
pnpm build
```

Expected: no type errors; build writes gzipped assets into `../www`.

- [ ] **Step 3: Full firmware build**

Run the firmware build command (it bundles the fresh `www`).
Expected: `Project build complete`.

- [ ] **Step 4: Commit**

```bash
git add frontend/src/lib/backend.ts www
git commit -m "Unwrap nested payload envelope in backend.ts"
```

---

### Task 6: Flash and verify on device

Requires hardware + a human. Flash: `idf.py -p <PORT> flash monitor`.

- [ ] Boot log clean: all managers report `Initialized`, no FATAL.
- [ ] Web UI loads; connection indicator green (heartbeat `ping` works —
      proves the new envelope round-trips).
- [ ] Dashboard shows device info + device time (`info`).
- [ ] Console page shows log history (`getLogs`) and live lines
      (broadcasts unchanged).
- [ ] Settings page lists settings (`getSettings`), edit + save works
      (`setSetting`, `saveSettings`), values survive reboot.
- [ ] WiFi scan returns networks (`wifiScan`).
- [ ] Firmware page shows status + partition table (`updateStatus`,
      `partitions`).
- [ ] Reboot button works and the UI reconnects (`reboot`).
- [ ] Unknown command from browser console rejects:
      `backend.send("nope").catch(e => console.log(e.message))` → logs `nope`.

---

## Self-review notes

- Spec coverage: entry v2 + trampoline (T4), Execute (T4), MemoryStream
  (T1), JsonReader (T2), JsonObject/JsonArray incl. auto-close semantics
  (T3), WS nested envelope (T4), backend.ts (T5), "device behaves
  exactly as before" (T6). Phase-2 items (HTTP route, UpdateManager
  command set) intentionally absent.
- `HandleUploadApp`/`HandleUploadWww`/`HandleDownloadPartition` in
  WebServerManager still use UpdateManager's Begin/Write/Finalize
  methods directly — those methods are untouched by this plan, so the
  upload routes keep working until phase 2 replaces them.
- JsonWriter stays: used by WebSocketHandler's error path, ConsoleManager
  broadcasts (log lines), MqttManager discovery/state. Only command
  handlers migrate.
- `wsBuf_` (4 KB) and the 512-byte WS receive buffer are unchanged; the
  new envelope adds ~14 bytes of prefix — no size risk.
