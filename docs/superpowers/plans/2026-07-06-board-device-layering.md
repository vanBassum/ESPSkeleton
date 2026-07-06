# Board/Device Layering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Application-layer `DeviceManager` with a board-provided `Board` class so each board folder owns its device instances and exposes the capability surface the application compiles against.

**Architecture:** Role interfaces (`Led`) live in `main/hardware/interfaces/` as application vocabulary; concrete drivers (`GpioLed`, `MockLed`) implement them in `main/hardware/drivers/`. Each board folder provides a concrete class named `Board` (duck-typed contract, no `IBoard` base) resolved via the existing include-path trick; `Board.cpp` is compiled via the `BOARD_SOURCES` hook. The per-board sdkconfig overlay also moves into the board folder.

**Tech Stack:** ESP-IDF v6.0, C++17, FreeRTOS. No automated tests exist in this repo — verification is a clean `idf.py build`.

**Spec:** `docs/superpowers/specs/2026-07-06-board-device-layering-design.md`

## Global Constraints

- C++17, no exceptions; `snprintf` with `sizeof` bounds; no `strcpy`/`strcat`.
- Manager pattern: `explicit Ctor(ServiceProvider&)`, copy/move deleted, `InitState`-guarded `Init()` (never init in ctor).
- `main/CMakeLists.txt` lists sources explicitly — no globbing.
- Role interfaces: 1–3 pure-virtual methods, application vocabulary only (no chip or GPIO vocabulary).
- **Build command (every verification step, from `c:\Workspace\Strux`):**
  `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
  Expected: exit code 0, output ends with `Project build complete.` (frontend build runs too if pnpm is present; its output is noise here). The profile prints an environment banner first — ignore it.
- **No TDD cycle:** the repo has no test infrastructure. Each task's cycle is: edit → build → commit.
- Commit after every task; end every commit message with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- **ODR hazard — do not reorder Tasks 1–2:** the old driver class and the new role interface are both named `Led` with overlapping inline member names (`On`, `Off`, `Toggle`). They must never be *compiled* into the binary simultaneously. Task 1 only adds headers that nothing includes yet; Task 2 atomically switches every consumer and deletes the old driver in the same commit.

---

### Task 1: Role interface and drivers (new headers, not yet included anywhere)

**Files:**
- Create: `main/hardware/interfaces/Led.h`
- Create: `main/hardware/drivers/GpioLed.h`
- Create: `main/hardware/drivers/MockLed.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `class Led` (pure virtual `void Set(bool)`, `bool IsOn() const`; non-virtual helpers `On()`, `Off()`, `Toggle()`); `class GpioLed : public Led` with ctor `GpioLed(int pin, bool activeHigh)` and `void Init()`; `class MockLed : public Led` (default-constructed). Task 2's `Board` uses exactly these names.

- [ ] **Step 1: Create `main/hardware/interfaces/Led.h`**

```cpp
#pragma once

// ──────────────────────────────────────────────────────────────
// Role interface: application vocabulary for "the LED".
// Boards bind a real driver (GpioLed) or a mock (MockLed).
// Role interfaces stay small (1-3 pure methods) and speak
// application vocabulary — never chip or GPIO vocabulary.
// ──────────────────────────────────────────────────────────────

class Led
{
public:
    virtual void Set(bool on) = 0;
    virtual bool IsOn() const = 0;
    virtual ~Led() = default;

    void On() { Set(true); }
    void Off() { Set(false); }
    void Toggle() { Set(!IsOn()); }
};
```

- [ ] **Step 2: Create `main/hardware/drivers/GpioLed.h`**

```cpp
#pragma once

#include "interfaces/Led.h"
#include "driver/gpio.h"

// ──────────────────────────────────────────────────────────────
// GPIO LED driver. Pin and polarity are injected by the board
// (from its BoardConfig constants) — this driver assumes a valid
// pin. A board without an LED binds MockLed instead.
// ──────────────────────────────────────────────────────────────

class GpioLed : public Led
{
public:
    GpioLed(int pin, bool activeHigh)
        : pin_(static_cast<gpio_num_t>(pin)), activeHigh_(activeHigh)
    {
    }

    void Init()
    {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin_;
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);

        Set(false);
    }

    void Set(bool on) override
    {
        state_ = on;
        gpio_set_level(pin_, on == activeHigh_ ? 1 : 0);
    }

    bool IsOn() const override { return state_; }

private:
    gpio_num_t pin_;
    bool activeHigh_;
    bool state_ = false;
};
```

- [ ] **Step 3: Create `main/hardware/drivers/MockLed.h`**

```cpp
#pragma once

#include "interfaces/Led.h"

// ──────────────────────────────────────────────────────────────
// Led role with no hardware behind it — remembers state only.
// For boards without an LED fitted.
// ──────────────────────────────────────────────────────────────

class MockLed : public Led
{
public:
    void Set(bool on) override { state_ = on; }
    bool IsOn() const override { return state_; }

private:
    bool state_ = false;
};
```

- [ ] **Step 4: Build**

Run (from `c:\Workspace\Strux`): `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
Expected: `Project build complete.` — nothing includes the new headers yet, so this only proves the tree still builds.

- [ ] **Step 5: Commit**

```bash
git add main/hardware/interfaces/Led.h main/hardware/drivers/GpioLed.h main/hardware/drivers/MockLed.h
git commit -m "Add Led role interface with GpioLed and MockLed drivers"
git push
```

---

### Task 2: Board class replaces DeviceManager (atomic switchover)

**Files:**
- Create: `main/hardware/boards/esp32_devkit/Board.h`
- Create: `main/hardware/boards/esp32_devkit/Board.cpp`
- Modify: `main/hardware/boards/esp32_devkit/board.cmake`
- Modify: `main/Application/ServiceProvider.h`
- Modify: `main/Application/ApplicationContext.h`
- Modify: `main/main.cpp:21`
- Modify: `main/Application/HomeAssistantManager/HomeAssistantManager.cpp`
- Modify: `main/CMakeLists.txt:19,37`
- Delete: `main/Application/DeviceManager/DeviceManager.h`
- Delete: `main/Application/DeviceManager/DeviceManager.cpp`
- Delete: `main/hardware/drivers/Led.h`

**Interfaces:**
- Consumes: `Led`, `GpioLed` from Task 1; existing `BoardConfig::LED_PIN`, `BoardConfig::LED_ACTIVE_HIGH`.
- Produces: `class Board` with `explicit Board(ServiceProvider&)`, `void Init()`, `Led& GetLed()`; `ServiceProvider::getBoard()` returning `Board&`. This is the contract every future board folder must satisfy.

**This task must land as ONE commit** — see the ODR hazard in Global Constraints. Intermediate builds are impossible mid-task; only Step 9 builds.

- [ ] **Step 1: Create `main/hardware/boards/esp32_devkit/Board.h`**

```cpp
#pragma once

#include "ServiceProvider.h"
#include "InitState.h"
#include "BoardConfig.h"
#include "drivers/GpioLed.h"

// ──────────────────────────────────────────────────────────────
// Board for the generic ESP32 DevKit. Owns every hardware driver
// instance (and bus host) and exposes the capability surface the
// application compiles against.
//
// Every board folder provides a class named Board with the same
// surface; #include "Board.h" resolves to the board selected with
// -DBOARD=<name>. There is deliberately no IBoard base class: a
// board that misses a method the application uses fails to compile
// for that board.
//
// Surface rules:
//   • role interfaces (Led&, ...) for devices the application
//     addresses by meaning — bind a Mock* driver when not fitted;
//   • concrete driver accessors are allowed as an escape hatch
//     when the application needs a driver's full API.
// ──────────────────────────────────────────────────────────────

class Board
{
    static constexpr const char *TAG = "Board";

public:
    explicit Board(ServiceProvider &serviceProvider);

    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;
    Board(Board &&) = delete;
    Board &operator=(Board &&) = delete;

    void Init();

    Led &GetLed() { return led_; }

private:
    ServiceProvider &serviceProvider_;
    InitState initState_;

    // Hardware instances — buses first, then the drivers that use them.
    GpioLed led_{ BoardConfig::LED_PIN, BoardConfig::LED_ACTIVE_HIGH };
};
```

- [ ] **Step 2: Create `main/hardware/boards/esp32_devkit/Board.cpp`**

```cpp
#include "Board.h"
#include "esp_log.h"

Board::Board(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

void Board::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    led_.Init();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}
```

- [ ] **Step 3: Append `Board.cpp` to `BOARD_SOURCES` in `main/hardware/boards/esp32_devkit/board.cmake`**

Add after the existing comment block (the file is currently comment-only):

```cmake
list(APPEND BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/Board.cpp")
```

- [ ] **Step 4: Retarget `main/Application/ServiceProvider.h`**

Replace the forward declaration `class DeviceManager;` (line 5) with `class Board;` placed alphabetically first (before `class CommandManager;`), and replace `virtual DeviceManager& getDeviceManager() = 0;` (line 20) with a `getBoard()` declared first in the method list:

```cpp
class Board;
class CommandManager;
...
    virtual Board& getBoard() = 0;
    virtual CommandManager& getCommandManager() = 0;
```

- [ ] **Step 5: Retarget `main/Application/ApplicationContext.h`**

Three edits:
1. Replace `#include "DeviceManager/DeviceManager.h"` (line 5) with `#include "Board.h"` — keep the include list otherwise; `Board.h` resolves into the selected board folder via the include path.
2. Replace the accessor (line 25): `Board& getBoard() override { return m_board; }` (place it first, before `getCommandManager`).
3. Replace the member (line 43): `Board m_board{*this};` in the same declaration slot (after `m_mqttManager`, before `m_homeAssistantManager`) — declaration order is construction order; keep the Board constructed in DeviceManager's old slot.

- [ ] **Step 6: Retarget `main/main.cpp:21`**

```cpp
    g_appContext.getBoard().Init();
```

(replaces `g_appContext.getDeviceManager().Init();` — same position in the init sequence.)

- [ ] **Step 7: Retarget `main/Application/HomeAssistantManager/HomeAssistantManager.cpp`**

Three edits:
1. Line 3: replace `#include "DeviceManager/DeviceManager.h"` with `#include "Board.h"`.
2. Line 30: `serviceProvider_.getDeviceManager().getLed().Set(on);` → `serviceProvider_.getBoard().GetLed().Set(on);`
3. Line 66: `bool on = serviceProvider_.getDeviceManager().getLed().IsOn();` → `bool on = serviceProvider_.getBoard().GetLed().IsOn();`

- [ ] **Step 8: Remove DeviceManager from `main/CMakeLists.txt` and delete dead files**

Remove line 19 (`"Application/DeviceManager/DeviceManager.cpp"`) from `SOURCE_FILES_LIST` and line 37 (`"Application/DeviceManager"`) from `INCLUDE_DIRS_LIST`. Then delete:

```bash
git rm main/Application/DeviceManager/DeviceManager.h main/Application/DeviceManager/DeviceManager.cpp main/hardware/drivers/Led.h
```

- [ ] **Step 9: Build**

Run (from `c:\Workspace\Strux`): `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
Expected: `Project build complete.` If the linker reports duplicate/odd `Led` symbols, `main/hardware/drivers/Led.h` was not deleted — go back to Step 8.

- [ ] **Step 10: Verify no live DeviceManager references remain**

Run: `git grep -l DeviceManager -- ':!docs' ':!CLAUDE.md'`
Expected: no output. (`CLAUDE.md` still matches until Task 4 updates it; docs keep historical references: the spec and `docs/backlog/mqtt-registration.md`.)

- [ ] **Step 11: Commit**

```bash
git add -A
git commit -m "Replace DeviceManager with board-provided Board class"
git push
```

---

### Task 3: Per-board sdkconfig overlay moves into the board folder

**Files:**
- Modify: `CMakeLists.txt:20-31` (repo root)

**Interfaces:**
- Consumes: `BOARD` cache variable (already defined in this block).
- Produces: convention `main/hardware/boards/<board>/sdkconfig.defaults` as the per-board overlay. No such file exists today, so behavior is unchanged until a board adds one.

- [ ] **Step 1: Replace the per-board sdkconfig block in root `CMakeLists.txt`**

Current block (lines 20–31):

```cmake
# ── Per-board sdkconfig ───────────────────────────────────────
# Common defaults plus an optional board-specific override file
# (sdkconfig.defaults.<board>), letting a board change flash size / PSRAM /
# partitions without touching the shared defaults. BOARD is the same cache
# var consumed by main/CMakeLists.txt for board selection.
if(NOT DEFINED BOARD)
    set(BOARD "esp32_devkit")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${BOARD}")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.${BOARD}")
endif()
```

Replace with:

```cmake
# ── Per-board sdkconfig ───────────────────────────────────────
# Common defaults plus an optional per-board overlay living in the board
# folder (main/hardware/boards/<board>/sdkconfig.defaults), letting a board
# change flash size / PSRAM / partitions without touching the shared
# defaults. sdkconfig fragments cannot include one another, so this list is
# the composition point: the board file overlays the common one and wins on
# conflicts. BOARD is the same cache var consumed by main/CMakeLists.txt.
if(NOT DEFINED BOARD)
    set(BOARD "esp32_devkit")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/main/hardware/boards/${BOARD}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "main/hardware/boards/${BOARD}/sdkconfig.defaults")
endif()
```

(Relative entries in `SDKCONFIG_DEFAULTS` resolve against the project root.)

- [ ] **Step 2: Build**

Run (from `c:\Workspace\Strux`): `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
Expected: `Project build complete.` — no board overlay file exists, so the sdkconfig is unchanged.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "Move per-board sdkconfig overlay into the board folder"
git push
```

---

### Task 4: Documentation and cleanup

**Files:**
- Modify: `CLAUDE.md`
- Modify: `main/hardware/boards/esp32_devkit/BoardConfig.h:11-15`
- Modify: `docs/superpowers/specs/2026-07-06-board-device-layering-design.md:4` (status line)
- Delete: `docs/backlog/board-device-layering.md`
- Delete: `ideas/board-example.h`

**Interfaces:**
- Consumes: the shipped state of Tasks 1–3.
- Produces: docs a fork can follow to add a board or a role interface.

- [ ] **Step 1: Update `BoardConfig.h` LED comment**

Replace lines 11–15:

```cpp
    // LED
    // GPIO2 is the built-in LED on most ESP32 DevKit boards.
    // Set to -1 if the board has no LED.
    static constexpr int LED_PIN = 2;
    static constexpr bool LED_ACTIVE_HIGH = true;
```

with:

```cpp
    // LED
    // GPIO2 is the built-in LED on most ESP32 DevKit boards.
    // A board without an LED drops these constants and binds MockLed
    // in its Board class instead (see hardware/interfaces/Led.h).
    static constexpr int LED_PIN = 2;
    static constexpr bool LED_ACTIVE_HIGH = true;
```

- [ ] **Step 2: Update `CLAUDE.md`**

Four edits:

1. Init-order sentence in "Manager pattern": replace `(Console → Settings → System → Network → Time → Command → Mqtt → Device → HomeAssistant → Update → WebServer)` with `(Console → Settings → System → Network → Time → Command → Mqtt → Board → HomeAssistant → Update → WebServer)`.

2. In "Layer separation", the `boards/<name>/` bullet: replace

   > `boards/<name>/` — one folder per target board: `BoardConfig.h` (pins/constants) plus a `board.cmake` fragment. Selected with `-DBOARD=<name>`; only the chosen board folder is on the include path, so `#include "BoardConfig.h"` resolves to it. An optional root-level `sdkconfig.defaults.<board>` overlays the common `sdkconfig.defaults`.

   with:

   > `boards/<name>/` — one folder per target board: `BoardConfig.h` (pins/constants), `Board.h`/`Board.cpp` (the board's `Board` class — owns every driver instance and bus host, exposes the capability surface the application compiles against; `Board.cpp` is added via `BOARD_SOURCES` in the `board.cmake` fragment), and an optional `sdkconfig.defaults` overlaying the common root one. Selected with `-DBOARD=<name>`; only the chosen board folder is on the include path, so `#include "BoardConfig.h"` and `#include "Board.h"` resolve to it. There is no `IBoard` base class — the contract is duck-typed: a board missing a method the application uses fails to compile for that board.

3. In "Layer separation", after the `drivers/` bullet, add:

   > - `interfaces/` — role interfaces in application vocabulary (`Led`), 1–3 pure-virtual methods each, never chip or GPIO vocabulary. Drivers implement them (`GpioLed : Led`); a board without the hardware binds a mock (`MockLed`). Add a role interface only when application code speaks in that role; expose a concrete driver accessor from `Board` instead when the application needs a driver's full API (escape hatch). Multi-instance roles get a semantic enum (`Sensor::Ambient`, never `Sensor_2`) mapped by the board — introduce it with the first multi-instance role.

4. The `main/Application/` bullet: replace

   > Hardware driver *instances* live in `DeviceManager`, which exposes typed accessors (`getLed()`).

   with:

   > Hardware driver *instances* live in the board's `Board` class, reached via `ServiceProvider::getBoard()`.

- [ ] **Step 3: Mark the spec implemented and delete superseded files**

In `docs/superpowers/specs/2026-07-06-board-device-layering-design.md` line 4, change `**Status:** Approved direction, pending spec review` to `**Status:** Implemented 2026-07-06`. Then:

```bash
git rm docs/backlog/board-device-layering.md ideas/board-example.h
```

- [ ] **Step 4: Build (docs only — confirms BoardConfig.h comment edit is benign)**

Run (from `c:\Workspace\Strux`): `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build`
Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "Document board/device layering; drop resolved backlog entry and sketch"
git push
```

---

## Final verification (after all tasks)

1. `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1; idf.py build` → `Project build complete.`
2. `git grep -l DeviceManager -- ':!docs'` → no output.
3. Hardware check (user, when convenient): flash a devkit, confirm the LED entity still appears in Home Assistant and on/off + state work.
