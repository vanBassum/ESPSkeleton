# Modular Managers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every feature manager removable by deleting its folder + wiring lines, by inverting CommandManager into a heap-free intrusive command registry and moving each command into the manager that owns its domain.

**Architecture:** Commands become `CommandEntry` chains: each manager owns an `inline static CommandEntry commands_[]` table and registers it in `Init()` via `CommandManager::Register(this, commands_)`. CommandManager is a pure dispatcher guarded by a `RecursiveMutex`. The LED's Home Assistant entity moves from HomeAssistantManager into DeviceManager using MqttManager's existing hooks, removing the last upward reference.

**Tech Stack:** ESP-IDF v6.0, C++17, FreeRTOS. No heap, no `std::function` in the registry. Spec: `docs/superpowers/specs/2026-07-02-modular-managers-design.md`. Normative registry sketch: `ideas/command-registry-example.h`.

## Global Constraints

- Branch: `modular-managers` (already created and pushed).
- **There is no test framework.** The test cycle for every task is: the project builds. Build command (PowerShell, from anywhere):
  `$env:PYTHONIOENCODING='utf-8'; . C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; Set-Location C:\Workspace\Strux; idf.py build`
  Expected on success: `Project build complete.` (The `PYTHONIOENCODING` matters: without it idf.py crashes with `UnicodeEncodeError` relaying Vite's output.)
- Command **names and JSON payloads must not change** — the frontend is untouched. `CommandManager::Execute(type, json, resp)` keeps its exact signature (WebSocketHandler.cpp:203 calls it).
- No dynamic memory in anything this plan adds. No `std::function`, no `new`, no containers.
- C++17. `snprintf` with `sizeof` bounds. Match surrounding code style (4 spaces, `TAG` logging).
- Commit after every task (messages given per task). Do not push force; push the branch after each commit.
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
- Consumes: `CommandEntry`, `CommandManager::Register`. Uses its own `wifi_interface_` member directly (no more `getNetworkManager().wifi()` from outside).
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

### Task 6: DeviceManager self-registers its LED HA entity; HomeAssistantManager drops DeviceManager

**Files:**
- Modify: `main/Application/DeviceManager/DeviceManager.h`
- Modify: `main/Application/DeviceManager/DeviceManager.cpp`
- Modify: `main/Application/HomeAssistantManager/HomeAssistantManager.h`
- Modify: `main/Application/HomeAssistantManager/HomeAssistantManager.cpp`

**Interfaces:**
- Consumes: `MqttManager::RegisterCommand(name, handler)`, `MqttManager::RegisterDiscovery(cb)`, `MqttManager::PublishEntityDiscovery(component, objectId, writeFields)`, `MqttManager::Publish(subtopic, payload, retain)`, `MqttManager::GetBaseTopic()` — all existing.
- Produces: identical MQTT/HA behavior (LED light entity, same topics `{base}/set/led`, `{base}/led/state`). HomeAssistantManager no longer references DeviceManager.

Note: `MqttManager::Init()` runs before `DeviceManager::Init()` (main.cpp order), and its Register* hooks safely store callbacks whether or not MQTT is enabled/connected — registration is unconditional, publishing degrades gracefully.

- [ ] **Step 1: Add LED entity to `DeviceManager.h`**

Add a private method declaration (in the private section, after `Led led_;`):

```cpp
    void RegisterLedEntity();
    void PublishLedState();
```

- [ ] **Step 2: Implement in `DeviceManager.cpp`**

Replace the whole file with:

```cpp
#include "DeviceManager.h"
#include "MqttManager/MqttManager.h"
#include "JsonWriter.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>

DeviceManager::DeviceManager(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

void DeviceManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    led_.Init();
    RegisterLedEntity();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ──────────────────────────────────────────────────────────────
// Home Assistant entities for this project's hardware.
//
// Devices register their own entities via MqttManager's hooks —
// HomeAssistantManager only publishes generic device-level entities
// and knows nothing about project hardware. If your project removes
// MqttManager, delete this section (the compiler will point here).
// ──────────────────────────────────────────────────────────────

void DeviceManager::RegisterLedEntity()
{
    auto &mqtt = serviceProvider_.getMqttManager();

    mqtt.RegisterCommand("led", [this](const char *data, int len)
    {
        bool on = (len >= 2 && strncmp(data, "ON", 2) == 0);
        led_.Set(on);
        PublishLedState();
    });

    mqtt.RegisterDiscovery([this]()
    {
        auto &mqtt = serviceProvider_.getMqttManager();

        mqtt.PublishEntityDiscovery("light", "led", [&mqtt](JsonWriter &json)
        {
            json.field("name", "LED");

            char topic[128];
            snprintf(topic, sizeof(topic), "%s/set/led", mqtt.GetBaseTopic());
            json.field("cmd_t", topic);

            snprintf(topic, sizeof(topic), "%s/led/state", mqtt.GetBaseTopic());
            json.field("stat_t", topic);

            json.field("pl_on", "ON");
            json.field("pl_off", "OFF");
        });

        PublishLedState();
    });
}

void DeviceManager::PublishLedState()
{
    serviceProvider_.getMqttManager().Publish("led/state", led_.IsOn() ? "ON" : "OFF", true);
}
```

- [ ] **Step 3: Strip HomeAssistantManager down to an extension-point shell**

Replace `main/Application/HomeAssistantManager/HomeAssistantManager.cpp` with:

```cpp
#include "HomeAssistantManager.h"
#include "esp_log.h"

HomeAssistantManager::HomeAssistantManager(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

void HomeAssistantManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // ── Add project-wide HA entities here ────────────────────
    // Generic device-level entities (IP, RSSI, uptime, heap, reboot)
    // are published by MqttManager itself. Hardware-specific entities
    // are registered by the manager that owns the hardware (see
    // DeviceManager::RegisterLedEntity for the pattern):
    //
    //   auto &mqtt = serviceProvider_.getMqttManager();
    //   mqtt.RegisterCommand("my_thing", [this](const char *data, int len) { ... });
    //   mqtt.RegisterDiscovery([this]() { ... PublishEntityDiscovery(...); });

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}
```

In `HomeAssistantManager.h`, remove the line `void PublishLedState();` from the private section. Nothing else changes.

- [ ] **Step 4: Verify no stray references**

Run: `grep -rn "getDeviceManager" main/Application --include=*.cpp --include=*.h | grep -v ApplicationContext | grep -v ServiceProvider`
Expected: no output (only the wiring files may name DeviceManager).

- [ ] **Step 5: Build**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 6: Commit**

```bash
git add main/Application/DeviceManager main/Application/HomeAssistantManager
git commit -m "DeviceManager self-registers its LED HA entity

HomeAssistantManager no longer references DeviceManager; it is now a
pure extension point. Feature->feature edges are down to the two
documented MQTT stacks."
git push
```

---

### Task 7: Document the architecture rules (CLAUDE.md + README)

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`

**Interfaces:** none (documentation).

- [ ] **Step 1: Add rules to `CLAUDE.md`**

In `CLAUDE.md`, inside the `## Architecture` section, after the "Manager pattern (dependency injection)" subsection, add a new subsection:

```markdown
### Core vs. feature managers (the rip-out rule)

Core managers — **Settings, Console, Command** — are the skeleton; any manager may reference them. Feature managers — **Network, Time, Mqtt, HomeAssistant, Update, WebServer, Device** — may only be referenced from the wiring files (`ServiceProvider.h`, `ApplicationContext.h`, `main.cpp`, `main/CMakeLists.txt`). Exceptions: HomeAssistant→Mqtt and Device→Mqtt (entity registration) are explicit stacks — removing Mqtt means removing/trimming those too.

To remove a feature manager: delete its folder, remove its lines from the four wiring files, build — the compiler finds anything missed. No other manager needs editing.

Commands are not implemented centrally: each manager owns an `inline static CommandEntry commands_[]` table ([main/Application/CommandManager/CommandEntry.h](main/Application/CommandManager/CommandEntry.h)) and registers it in `Init()` via `getCommandManager().Register(this, commands_)`. Tables must have static storage duration (the entry dtor aborts otherwise — deliberately). Auth is not in the registry; handlers guarding state changes call `CommandManager::CheckAuth()` first.

### Thread-safety contract

Every manager must survive use from any task: `InitState` guards initialization, a `Mutex` (or `RecursiveMutex` when re-entry is by design) guards mutable state, and misuse asserts/aborts loudly at first occurrence instead of corrupting. Convention: public methods lock; private `*Locked` methods document that the caller holds the lock. Never hold a lock while calling out to handlers/callbacks.
```

- [ ] **Step 2: Update README's "Adding a New Command" section**

In `README.md`, replace the current "### Adding a New Command" paragraph (which describes the central command table) with:

```markdown
### Adding a New Command

Commands live with the manager that owns the domain — there is no central command table. In your manager:

1. Declare a static trampoline and an `inline static CommandEntry commands_[]` table (private members):

```cpp
static void Cmd_MyThing(void* ctx, const char* json, JsonWriter& resp);

inline static CommandEntry commands_[] = {
    { "myThing", &MyManager::Cmd_MyThing },
};
```

2. Register it in `Init()`: `serviceProvider_.getCommandManager().Register(this, commands_);`
3. The trampoline casts `ctx` back: `static_cast<MyManager*>(ctx)->...`. If the command changes state, call `getCommandManager().CheckAuth(json, resp)` as its first line.

Tables must be `inline static` class members (static storage duration) — a registered `CommandEntry` that gets destroyed aborts the device on purpose. The frontend calls commands via the WebSocket RPC layer in `backend.ts`, unchanged.
```

- [ ] **Step 3: Build (docs don't affect it, but keep the habit)**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "Document rip-out rule, distributed commands, thread-safety contract"
git push
```

---

### Task 8: Verification — rip-out smoke test + on-device checks

**Files:** none permanently (scratch branch only).

- [ ] **Step 1: Full clean-ish build on the branch**

Run the build command from Global Constraints.
Expected: `Project build complete.`

- [ ] **Step 2: Rip-out smoke test — HomeAssistantManager**

This proves the whole point of the design. On a scratch branch:

```bash
git checkout -b ripout-smoke
git rm -r main/Application/HomeAssistantManager
```

Then remove HomeAssistant from the wiring (guided by compile errors):
- `main/Application/ServiceProvider.h`: delete the `class HomeAssistantManager;` forward declaration and the `getHomeAssistantManager()` pure virtual.
- `main/Application/ApplicationContext.h`: delete its include, getter, and member.
- `main/main.cpp`: delete the `getHomeAssistantManager().Init();` line.
- `main/CMakeLists.txt`: delete the `HomeAssistantManager.cpp` source line and the `Application/HomeAssistantManager` include dir line.

Run the build command. Expected: `Project build complete.` — **with zero edits inside any other manager**. If any non-wiring file needed editing, that is a design violation: stop and report it.

- [ ] **Step 3: Rip-out smoke test — MqttManager (the stack case)**

Still on `ripout-smoke`:

```bash
git rm -r main/Application/MqttManager
```

Wiring removals as above (ServiceProvider.h, ApplicationContext.h, main.cpp, main/CMakeLists.txt — also remove `espressif__mqtt` from `COMPONENT_REQUIRES` and the mqtt dependency from `main/idf_component.yml`). Per the documented stack rule, also delete DeviceManager's entity block: remove `RegisterLedEntity()`/`PublishLedState()` declarations and definitions and the `RegisterLedEntity();` call — the section is marked with a comment saying exactly this.

Run the build command. Expected: `Project build complete.`

- [ ] **Step 4: Discard the scratch branch**

```bash
git checkout modular-managers
git branch -D ripout-smoke
```

- [ ] **Step 5: On-device checks (needs hardware — coordinate with Bas)**

Flash a devkit (`idf.py -p <PORT> flash monitor`) and verify from the web UI:
- Dashboard loads; `info`/`ping` work (device time visible).
- Settings page lists settings; changing + saving works; with a `device.pin` set, wrong PIN is rejected (`error:"auth"`), correct PIN accepted.
- WiFi scan returns networks.
- Console page shows log history.
- Firmware page shows running partition, next slot, and the partition table.
- Reboot button works (asks PIN if set).
- With MQTT configured: device appears in Home Assistant with the LED light + diagnostic sensors; toggling the LED from HA works and state updates.

- [ ] **Step 6: Final commit if any fixes were needed, then push**

```bash
git push
```

---

## Self-review notes (done at plan time)

- Spec §1/§2 (layering + rule) → Task 7. §3 (registry) → Task 1. §4 (relocations) → Tasks 2–5. §5 (auth) → Tasks 1–2 (CheckAuth public, handlers call it). §6 (Device/HA) → Task 6. §7 (thread-safety doc) → Task 7. §8 verification → Task 8. `RecursiveMutex` prerequisite from the spec already exists in `lib/rtos/RecursiveMutex.h` — no task needed.
- Init-order hazard (Console/Settings/Network register before CommandManager::Init) addressed by making the registry constructor-ready; noted in Global Constraints and CommandManager.h docs.
- No CMakeLists changes needed in Tasks 1–6: `CommandEntry.h` is header-only and no .cpp files are added or removed.
