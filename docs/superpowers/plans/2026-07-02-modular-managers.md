# Command Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Scope note:** This plan implements ONLY the CommandManager registry concept from the design spec (§3–§5): the intrusive command registry and moving each command into the manager that owns its domain. Everything else in the spec — DeviceManager/HA entity inversion (§6), documentation of the rip-out rule (§2), rip-out smoke tests — is **deliberately parked**; Bas will work those out later.

**Goal:** Invert CommandManager into a heap-free intrusive command registry; each manager owns and registers its own commands.

**Architecture:** Commands become `CommandEntry` chains: each manager owns an `inline static CommandEntry commands_[]` table and registers it in `Init()` via `CommandManager::Register(this, commands_)`. CommandManager is a pure dispatcher guarded by a `RecursiveMutex`; handlers run outside the lock.

**Tech Stack:** ESP-IDF v6.0, C++17, FreeRTOS. No heap, no `std::function` in the registry. Spec: `docs/superpowers/specs/2026-07-02-modular-managers-design.md`. Normative registry sketch: `ideas/command-registry-example.h`.

## Global Constraints

- Branch: `modular-managers` (already created and pushed).
- **There is no test framework.** The test cycle for every task is: the project builds. Build command (PowerShell, from anywhere):
  `$env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; Set-Location C:\Workspace\Strux; idf.py build`
  Expected on success: `Project build complete.` (The `PYTHONIOENCODING` matters: without it idf.py crashes with `UnicodeEncodeError` relaying Vite's output.)
- Command **names and JSON payloads must not change** — the frontend is untouched. `CommandManager::Execute(type, json, resp)` keeps its exact signature (WebSocketHandler.cpp:203 calls it).
- No dynamic memory in anything this plan adds. No `std::function`, no `new`, no containers.
- C++17. `snprintf` with `sizeof` bounds. Match surrounding code style (4 spaces, `TAG` logging).
- Commit after every task (messages given per task). Push the branch after each commit.
- `CommandEntry` tables must have static storage duration — `inline static` class members. Never stack/heap (the dtor aborts the device if a registered entry dies; this is by design).
- Init-order note: the registry is usable **from construction** (mutex and head are member-initialized), so managers whose `Init()` runs before CommandManager's (Console, Settings, Network, Time — see main.cpp order) may still register safely. `CommandManager::Init()` only registers its own built-ins.

---

### Task 1: CommandEntry + CommandManager registry core

**Files:**
- Create: `main/Application/CommandManager/CommandEntry.h`
- Modify: `main/Application/CommandManager/CommandManager.h` (full rewrite, shown below)
- Modify: `main/Application/CommandManager/CommandManager.cpp` (full rewrite, shown below)

**Interfaces:**
- Consumes: `RecursiveMutex` (`lib/rtos/RecursiveMutex.h`, exists), `LOCK()` (`lib/rtos/ContextLock.h`), `InitState`, `JsonWriter`, `ExtractJsonString` (`lib/json/JsonHelpers.h`).
- Produces (used by Tasks 2–5):
  - `struct CommandEntry { const char* name; void (*handler)(void* ctx, const char* json, JsonWriter& resp); /* + managed fields */ };`
  - `template <size_t N> void CommandManager::Register(void* ctx, CommandEntry (&commands)[N])`
  - `bool CommandManager::CheckAuth(const char* json, JsonWriter& resp)` — **now public**; returns true if authorized, else writes `ok:false, error:"auth"` into resp.
  - `bool CommandManager::Execute(const char* type, const char* json, JsonWriter& resp)` — unchanged signature.

**Note:** after this task, `wifiScan`, `getSettings`, `setSetting`, `saveSettings`, `getLogs`, `updateStatus`, `partitions` are temporarily gone (they return unknown-command). Tasks 2–5 restore them. Each intermediate state still builds.

- [ ] **Step 1: Create `main/Application/CommandManager/CommandEntry.h`**

```cpp
#pragma once

#include <cstdlib>
#include "esp_log.h"

class JsonWriter;

// ──────────────────────────────────────────────────────────────
// One command in the CommandManager registry.
//
// Entries are the links of an intrusive chain. Owners declare them
// as an `inline static CommandEntry commands_[]` class member
// (static storage duration) and hand the array to
// CommandManager::Register(), which stamps ctx and links them.
// Owners never touch ctx/next/registered.
//
// Handlers are plain function pointers — no heap, no std::function.
// The usual shape is a static member "trampoline" that casts ctx
// back to the owning manager and calls a private method.
// ──────────────────────────────────────────────────────────────
struct CommandEntry
{
    const char* name;
    void (*handler)(void* ctx, const char* json, JsonWriter& resp);

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    // A registered entry is a live link in the dispatch chain; letting it
    // die would leave a dangling pointer in the chain. There is no
    // compile-time way to forbid this (a deleted dtor would propagate up
    // through the owning manager to the global ApplicationContext), so:
    // abort. The device resets with a clear message on the very first run
    // of the offending code.
    ~CommandEntry()
    {
        if (registered)
        {
            ESP_LOGE("CommandEntry", "registered command '%s' destroyed — "
                     "command tables must live for the whole application", name);
            abort();
        }
    }
};
```

- [ ] **Step 2: Rewrite `main/Application/CommandManager/CommandManager.h`**

Replace the entire file with:

```cpp
#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"
#include <cstring>
#include <cassert>
#include <cstddef>

class JsonWriter;

class CommandManager {
    static constexpr const char* TAG = "CommandManager";

public:
    explicit CommandManager(ServiceProvider& serviceProvider);

    CommandManager(const CommandManager&) = delete;
    CommandManager& operator=(const CommandManager&) = delete;
    CommandManager(CommandManager&&) = delete;
    CommandManager& operator=(CommandManager&&) = delete;

    void Init();

    /// Register a table of commands. `commands` MUST have static storage
    /// duration (see ~CommandEntry). `ctx` (usually the owner's `this`) is
    /// stamped into every entry and handed back to its handler at dispatch.
    ///
    /// Thread-safe, and usable from construction — managers whose Init()
    /// runs before CommandManager's may register safely.
    template <size_t N>
    void Register(void* ctx, CommandEntry (&commands)[N])
    {
        LOCK(mutex_);
        for (size_t i = 0; i < N; ++i)
        {
            // Re-registering would re-link an entry already in the chain
            // and cycle it → Execute() would hang. Fail loudly on first
            // boot instead.
            assert(!commands[i].registered && "command registered twice");
            assert(Find(commands[i].name) == nullptr && "duplicate command name");

            commands[i].ctx = ctx;
            commands[i].registered = true;
            commands[i].next = head_;
            head_ = &commands[i];
        }
    }

    /// Execute a command by type name. Writes response fields into the JsonWriter.
    /// The caller is responsible for the outer object and transport-specific fields (e.g. "id").
    /// Returns true if the command was recognized.
    bool Execute(const char* type, const char* json, JsonWriter& resp);

    /// PIN check helper for handlers that guard state-changing commands.
    /// Call as the FIRST line of such handlers. Returns true if authorized;
    /// otherwise writes {ok:false, error:"auth"} into resp.
    /// (Auth is deliberately NOT part of the registry — see the design spec.)
    bool CheckAuth(const char* json, JsonWriter& resp);

private:
    ServiceProvider& serviceProvider_;
    InitState initState_;

    RecursiveMutex mutex_;
    CommandEntry* head_ = nullptr;

    // Locks internally (recursive, so Register may call it under its own
    // lock). Handing the pointer out after unlock is safe because entries
    // are immortal and name/handler/ctx are written before linking.
    const CommandEntry* Find(const char* name);

    // Built-in generic commands (everything domain-specific lives in the
    // manager that owns the domain).
    static void Cmd_Ping(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_Info(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_Reboot(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "ping",   &CommandManager::Cmd_Ping },
        { "info",   &CommandManager::Cmd_Info },
        { "reboot", &CommandManager::Cmd_Reboot },
    };
};
```

- [ ] **Step 3: Rewrite `main/Application/CommandManager/CommandManager.cpp`**

Replace the entire file with:

```cpp
#include "CommandManager.h"
#include "SettingsManager.h"
#include "JsonWriter.h"
#include "JsonHelpers.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

CommandManager::CommandManager(ServiceProvider& serviceProvider)
    : serviceProvider_(serviceProvider)
{
}

void CommandManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    Register(this, commands_);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

bool CommandManager::Execute(const char* type, const char* json, JsonWriter& resp)
{
    const CommandEntry* e = Find(type);
    if (e == nullptr)
        return false;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    e->handler(e->ctx, json, resp);
    return true;
}

const CommandEntry* CommandManager::Find(const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0)
            return e;
    return nullptr;
}

bool CommandManager::CheckAuth(const char* json, JsonWriter& resp)
{
    char storedPin[64] = {};
    serviceProvider_.getSettingsManager().getString("device.pin", storedPin, sizeof(storedPin));

    // No PIN configured — auth disabled
    if (storedPin[0] == '\0')
        return true;

    char pin[64] = {};
    ExtractJsonString(json, "pin", pin, sizeof(pin));

    if (strcmp(pin, storedPin) == 0)
        return true;

    ESP_LOGW(TAG, "Auth failed for command");
    resp.field("ok", false);
    resp.field("error", "auth");
    return false;
}

// ──────────────────────────────────────────────────────────────
// Built-in commands
// ──────────────────────────────────────────────────────────────

void CommandManager::Cmd_Ping(void* ctx, const char* json, JsonWriter& resp)
{
    resp.field("pong", true);
}

void CommandManager::Cmd_Info(void* ctx, const char* json, JsonWriter& resp)
{
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

void CommandManager::Cmd_Reboot(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<CommandManager*>(ctx);
    if (!self->CheckAuth(json, resp))
        return;

    resp.field("ok", true);
    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
```

- [ ] **Step 4: Build**

Run: `$env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; Set-Location C:\Workspace\Strux; idf.py build`
Expected: `Project build complete.` (No references to the removed `Cmd_WifiScan` etc. remain — they were all private to CommandManager.)

- [ ] **Step 5: Commit**

```bash
git add main/Application/CommandManager
git commit -m "Invert CommandManager into an intrusive command registry

Entries are chain links owned by registering managers (inline static
tables). No heap, no std::function. RecursiveMutex-guarded Find;
handlers dispatch outside the lock. Domain commands temporarily
removed; they return in the owning managers next."
git push
```

---

### Task 2: SettingsManager owns the settings commands

**Files:**
- Modify: `main/Application/SettingsManager/SettingsManager.h`
- Modify: `main/Application/SettingsManager/SettingsManager.cpp`

**Interfaces:**
- Consumes: `CommandEntry` (Task 1), `CommandManager::Register(this, commands_)`, `CommandManager::CheckAuth(json, resp)` via `serviceProvider_.getCommandManager()`.
- Produces: WebSocket commands `getSettings`, `setSetting` (auth), `saveSettings` (auth) — identical names/payloads/responses as before.

- [ ] **Step 1: Add command table to `SettingsManager.h`**

Add `#include "CommandEntry.h"` after `#include "InitState.h"`. Then add to the **private** section of the class (after the existing private members):

```cpp
    // ── WebSocket commands (registered with CommandManager in Init) ──
    static void Cmd_GetSettings(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_SetSetting(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_SaveSettings(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "getSettings",  &SettingsManager::Cmd_GetSettings },
        { "setSetting",   &SettingsManager::Cmd_SetSetting },
        { "saveSettings", &SettingsManager::Cmd_SaveSettings },
    };
```

- [ ] **Step 2: Register + implement in `SettingsManager.cpp`**

Add includes at the top (with the existing includes):

```cpp
#include "CommandManager.h"
#include "JsonHelpers.h"
```

In `SettingsManager::Init()`, immediately **before** the `SetReady()` call, add:

```cpp
    serviceProvider_.getCommandManager().Register(this, commands_);
```

At the end of the file, add the handlers (bodies moved verbatim from the old CommandManager, adapted to `self`):

```cpp
// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void SettingsManager::Cmd_GetSettings(void* ctx, const char* json, JsonWriter& resp)
{
    static_cast<SettingsManager*>(ctx)->WriteAllSettings(resp);
}

void SettingsManager::Cmd_SetSetting(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<SettingsManager*>(ctx);
    if (!self->serviceProvider_.getCommandManager().CheckAuth(json, resp))
        return;

    char key[64] = {};
    char value[128] = {};
    ExtractJsonString(json, "key", key, sizeof(key));
    ExtractJsonString(json, "value", value, sizeof(value));

    if (key[0] == '\0')
    {
        resp.field("ok", false);
        resp.field("error", "missing key");
        return;
    }

    const auto* defs = self->GetDefinitions();
    int count = self->GetDefinitionCount();

    for (int i = 0; i < count; i++)
    {
        if (strcmp(defs[i].key, key) == 0)
        {
            switch (defs[i].type)
            {
            case SettingType::String:
                self->setString(defs[i].key, value);
                break;
            case SettingType::Int:
                self->setInt(defs[i].key, atoi(value));
                break;
            case SettingType::Bool:
                self->setBool(defs[i].key, strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                break;
            }

            resp.field("ok", true);
            return;
        }
    }

    resp.field("ok", false);
    resp.field("error", "unknown key");
}

void SettingsManager::Cmd_SaveSettings(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<SettingsManager*>(ctx);
    if (!self->serviceProvider_.getCommandManager().CheckAuth(json, resp))
        return;

    resp.field("ok", self->Save());
}
```

(If `strcmp`/`atoi` are not already available in this .cpp, add `#include <cstring>` and `#include <cstdlib>`.)

- [ ] **Step 3: Build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/Application/SettingsManager
git commit -m "Move settings commands into SettingsManager"
git push
```

---

### Task 3: NetworkManager owns wifiScan

**Files:**
- Modify: `main/Application/NetworkManager/NetworkManager.h`
- Modify: `main/Application/NetworkManager/NetworkManager.cpp`

**Interfaces:**
- Consumes: `CommandEntry`, `CommandManager::Register`. Uses its own `wifi_interface_` member directly.
- Produces: WebSocket command `wifiScan` — identical response shape.

- [ ] **Step 1: Add command table to `NetworkManager.h`**

Add includes after `#include "Timer.h"`:

```cpp
#include "CommandEntry.h"
```

Add a forward declaration `class JsonWriter;` above the class if not present. Add to the **private** section:

```cpp
    // ── WebSocket commands (registered with CommandManager in Init) ──
    static void Cmd_WifiScan(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "wifiScan", &NetworkManager::Cmd_WifiScan },
    };
```

- [ ] **Step 2: Register + implement in `NetworkManager.cpp`**

Add includes at the top:

```cpp
#include "CommandManager.h"
#include "JsonWriter.h"
```

In `NetworkManager::Init()`, immediately before its `SetReady()` call, add:

```cpp
    serviceProvider_.getCommandManager().Register(this, commands_);
```

At the end of the file, add:

```cpp
// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void NetworkManager::Cmd_WifiScan(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<NetworkManager*>(ctx);

    WiFiInterface::ScanResult results[20] = {};
    int count = self->wifi().Scan(results, 20);

    resp.field("ok", true);
    resp.fieldArray("networks");

    for (int i = 0; i < count; i++)
    {
        resp.beginObject();
        resp.field("ssid", results[i].ssid);
        resp.field("rssi", static_cast<int32_t>(results[i].rssi));
        resp.field("channel", static_cast<int32_t>(results[i].channel));
        resp.field("secure", results[i].secure);
        resp.endObject();
    }

    resp.endArray();
}
```

- [ ] **Step 3: Build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/Application/NetworkManager
git commit -m "Move wifiScan command into NetworkManager"
git push
```

---

### Task 4: ConsoleManager owns getLogs

**Files:**
- Modify: `main/Application/ConsoleManager/ConsoleManager.h`
- Modify: `main/Application/ConsoleManager/ConsoleManager.cpp`

**Interfaces:**
- Consumes: `CommandEntry`, `CommandManager::Register`, its own `WriteHistory(JsonWriter&)`.
- Produces: WebSocket command `getLogs` — identical response shape.

- [ ] **Step 1: Add command table to `ConsoleManager.h`**

Add `#include "CommandEntry.h"` after `#include "Task.h"`. Add to the **private** section:

```cpp
    // ── WebSocket commands (registered with CommandManager in Init) ──
    static void Cmd_GetLogs(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "getLogs", &ConsoleManager::Cmd_GetLogs },
    };
```

- [ ] **Step 2: Register + implement in `ConsoleManager.cpp`**

Add `#include "CommandManager.h"` at the top. In `ConsoleManager::Init()`, immediately before its `SetReady()` call, add:

```cpp
    serviceProvider_.getCommandManager().Register(this, commands_);
```

At the end of the file, add:

```cpp
// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void ConsoleManager::Cmd_GetLogs(void* ctx, const char* json, JsonWriter& resp)
{
    static_cast<ConsoleManager*>(ctx)->WriteHistory(resp);
}
```

Note: ConsoleManager initializes FIRST in main.cpp, before CommandManager::Init() — this is fine by design (the registry is usable from construction; see Global Constraints).

- [ ] **Step 3: Build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/Application/ConsoleManager
git commit -m "Move getLogs command into ConsoleManager"
git push
```

---

### Task 5: UpdateManager owns updateStatus + partitions

**Files:**
- Modify: `main/Application/UpdateManager/UpdateManager.h`
- Modify: `main/Application/UpdateManager/UpdateManager.cpp`

**Interfaces:**
- Consumes: `CommandEntry`, `CommandManager::Register`, its own `GetRunningPartition()`, `GetNextPartition()`, `GetPartitions(out, maxCount)`.
- Produces: WebSocket commands `updateStatus`, `partitions` — identical response shapes.

- [ ] **Step 1: Add command table to `UpdateManager.h`**

Add `#include "CommandEntry.h"` after `#include "InitState.h"`. Add a forward declaration `class JsonWriter;` above the class if not present. Add to the **private** section:

```cpp
    // ── WebSocket commands (registered with CommandManager in Init) ──
    static void Cmd_UpdateStatus(void* ctx, const char* json, JsonWriter& resp);
    static void Cmd_Partitions(void* ctx, const char* json, JsonWriter& resp);

    inline static CommandEntry commands_[] = {
        { "updateStatus", &UpdateManager::Cmd_UpdateStatus },
        { "partitions",   &UpdateManager::Cmd_Partitions },
    };
```

- [ ] **Step 2: Register + implement in `UpdateManager.cpp`**

Add includes at the top:

```cpp
#include "CommandManager.h"
#include "JsonWriter.h"
#include "esp_app_desc.h"
```

In `UpdateManager::Init()`, immediately before its `SetReady()` call, add:

```cpp
    serviceProvider_.getCommandManager().Register(this, commands_);
```

At the end of the file, add:

```cpp
// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

void UpdateManager::Cmd_UpdateStatus(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<UpdateManager*>(ctx);
    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("firmware", app->version);
    resp.field("running", self->GetRunningPartition());
    resp.field("nextSlot", self->GetNextPartition());
}

void UpdateManager::Cmd_Partitions(void* ctx, const char* json, JsonWriter& resp)
{
    auto* self = static_cast<UpdateManager*>(ctx);

    static constexpr int MAX_PARTITIONS = 16;
    UpdateManager::PartitionInfo parts[MAX_PARTITIONS];
    int count = self->GetPartitions(parts, MAX_PARTITIONS);

    resp.fieldArray("partitions");
    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        resp.beginObject();
        resp.field("label",      p.label);
        resp.field("type",       p.type);
        resp.field("subtype",    p.subtype);
        resp.field("offset",     p.offset);
        resp.field("size",       p.size);
        resp.field("running",    p.running);
        resp.field("nextOta",    p.nextOta);
        resp.field("uploadable", p.uploadable);
        resp.field("version",    p.version);
        resp.endObject();
    }
    resp.endArray();
}
```

- [ ] **Step 3: Build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/Application/UpdateManager
git commit -m "Move updateStatus and partitions commands into UpdateManager"
git push
```

---

### Task 6: Verification

- [ ] **Step 1: Full build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 2: Confirm CommandManager's dependencies dropped**

Run: `grep -n "getNetworkManager\|getConsoleManager\|getUpdateManager" main/Application/CommandManager/CommandManager.cpp`
Expected: no output. (Only `getSettingsManager` remains, inside `CheckAuth` — core→core, allowed.)

- [ ] **Step 3: On-device checks (needs hardware — coordinate with Bas)**

Flash a devkit (`idf.py -p <PORT> flash monitor`) and verify from the web UI:
- Dashboard loads; `info`/`ping` work (device time visible).
- Settings page lists settings; changing + saving works; with a `device.pin` set, wrong PIN is rejected (`error:"auth"`), correct PIN accepted.
- WiFi scan returns networks.
- Console page shows log history.
- Firmware page shows running partition, next slot, and the partition table.
- Reboot button works (asks PIN if set).

- [ ] **Step 4: Push**

```bash
git push
```

---

## Parked for later (per Bas — do NOT implement now)

- DeviceManager/HomeAssistant entity inversion (spec §6)
- Rip-out rule documentation in CLAUDE.md/README (spec §2) and rip-out smoke tests
- MqttManager `std::function` hooks cleanup (spec §8). Revisited 2026-07-03:
  sketched an intrusive-registry version in `ideas/mqtt-registry-example.h`,
  then parked again — the command registry is request/response, while MQTT
  mostly *emits* (discovery, periodic state), so the pattern doesn't map
  cleanly. Also considered folding the three registries into one shared
  `Registry<Args...>` lib template; undecided. Leave MQTT alone for now.
- DeviceManager ↔ board layering (added 2026-07-03): the board defines what
  devices exist, yet DeviceManager (Application layer) hard-codes them.
  Different boards have different devices but should expose the same
  capabilities. Maybe the DeviceManager definition belongs in the board
  folders (`hardware/boards/<name>/`) — undecided, brainstorm first.
- Webserver login (added 2026-07-03): PIN auth was ripped out entirely.
  It returns as a login in the webserver layer; authentication checking
  stays at that edge and never leaks into command handlers.
