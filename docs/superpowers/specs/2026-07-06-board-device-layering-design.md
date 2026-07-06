# Board/device layering — design

**Date:** 2026-07-06
**Status:** Implemented 2026-07-06
**Replaces:** `docs/backlog/board-device-layering.md`
**Sketch:** `ideas/board-example.h` (delete after implementation)

## Problem

The board defines what devices exist, yet `DeviceManager` (Application
layer) hard-codes them. The application should not change between
boards: it declares what it expects, and each board makes itself
compatible — with real hardware or a mock.

## Core idea

Each board folder provides a concrete class named `Board` that owns
all driver instances (and bus hosts) and exposes the capability
surface the application compiles against. `Board` **replaces**
`DeviceManager`. Resolution uses the existing include-path trick:
only the selected board folder is on the include path, so
`#include "Board.h"` picks the `-DBOARD=` board at compile time.

There is deliberately **no `IBoard` base class**. The contract is
duck-typed: if a board misses a method the application uses, that
board fails to compile. Compiling *for* a board is the point — wiring
errors surface at build time, not runtime.

Devices the application interacts with are exposed through small
**role interfaces** written in application vocabulary (`Led`,
`TemperatureSensor`), never chip or plumbing vocabulary (no `IGpio`,
no device tree). A board without the hardware binds a mock
implementation of the role. Concrete driver accessors remain allowed
as a documented escape hatch when the application genuinely needs a
driver's full API.

## New layout

```
main/hardware/
  interfaces/            # role interfaces — application vocabulary, board-independent
    Led.h                #   pure-virtual: Set(bool), IsOn(); non-virtual On/Off/Toggle helpers
  drivers/               # concrete implementations, board-independent
    GpioLed.h            #   GpioLed(pin, activeHigh) : Led — replaces current Led.h
    MockLed.h            #   MockLed : Led — remembers state, drives nothing
  boards/esp32_devkit/
    BoardConfig.h        # unchanged role: pins/constants (LED_PIN = -1 convention removed)
    Board.h              # class Board — owns GpioLed, exposes Led& GetLed()
    Board.cpp            # added via BOARD_SOURCES in board.cmake
    board.cmake
    sdkconfig.defaults   # optional per-board overlay (moved from root, see below)
```

`main/Application/DeviceManager/` is deleted.

## Components

### Role interface `Led` (`hardware/interfaces/Led.h`)

```cpp
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

Role interfaces stay small (1–3 pure methods) and are added only when
the application speaks in that role.

### Driver `GpioLed` (`hardware/drivers/GpioLed.h`)

Current `Led` driver, renamed and re-parameterized: pin and polarity
arrive via constructor (from `BoardConfig` constants, passed by the
board), not via a `BoardConfig.h` include inside the driver. The
`LED_PIN = -1` compile-out (`if constexpr`) is removed — a board
without an LED binds `MockLed` instead, so `GpioLed` may assume a
valid pin.

### Driver `MockLed` (`hardware/drivers/MockLed.h`)

Implements `Led`; stores the state, touches no hardware. Serves both
"board has no LED" and template documentation of the mock idiom.

### `Board` (`hardware/boards/esp32_devkit/Board.h/.cpp`)

Follows the manager pattern: `explicit Board(ServiceProvider&)`,
copy/move deleted, `InitState`-guarded `Init()` that initializes
buses first, then drivers. For the devkit:

```cpp
class Board
{
public:
    explicit Board(ServiceProvider &ctx);
    void Init();

    Led &GetLed() { return led_; }

private:
    ServiceProvider &serviceProvider_;
    InitState initState_;
    GpioLed led_{ BoardConfig::LED_PIN, BoardConfig::LED_ACTIVE_HIGH };
};
```

`Board.cpp` is appended to `BOARD_SOURCES` by the board's
`board.cmake` — the first real use of that hook.

### Application-side changes

- `ServiceProvider.h`: `class Board;` forward declaration;
  `virtual Board& getBoard() = 0;` replaces `getDeviceManager()`.
- `ApplicationContext.h`: `#include "Board.h"`; by-value member
  `Board board_{ *this };` (static allocation, no factory — the
  binary is board-specific by design); `getBoard()` override.
- `main.cpp`: `getBoard().Init()` in the slot `DeviceManager` holds
  today (init order otherwise unchanged).
- `HomeAssistantManager.cpp`: include `Board.h`; calls become
  `getBoard().GetLed().Set(on)` / `.IsOn()`. Entity wiring itself is
  unchanged.
- `main/CMakeLists.txt`: remove the `DeviceManager` source and
  include-dir entries. `hardware` is already on the include path, so
  `interfaces/Led.h` and `drivers/GpioLed.h` resolve as-is.

### Per-board sdkconfig moves into the board folder

With `Board` in the board folder, the sdkconfig overlay is the last
board-specific file left outside it. It moves in: the root
`CMakeLists.txt` per-board block appends
`main/hardware/boards/${BOARD}/sdkconfig.defaults` to
`SDKCONFIG_DEFAULTS` when the file exists, replacing the root-level
`sdkconfig.defaults.<board>` pattern. sdkconfig fragments cannot
include one another, so CMake's `SDKCONFIG_DEFAULTS` list remains the
composition point: root `sdkconfig.defaults` holds shared settings,
the board file overlays and wins on conflicts. A board folder is then
fully self-contained. (No `sdkconfig.defaults.<board>` file exists in
the repo today, so nothing needs migrating.)

## Deferred (documented, not built)

- **Semantic role enums** (`Sensor::Ambient` → instance mapping via
  `GetTemperatureSensor(Sensor)`): the idiom for multi-instance
  roles, introduced when a project first has one. The template has a
  single LED; adding the enum now would be scaffolding without a
  consumer. The pattern stays documented in the CLAUDE.md section.
- **DeviceManager/HomeAssistant entity inversion** (parked in
  `docs/backlog/mqtt-registration.md`): untouched by this change.

## Documentation updates

- CLAUDE.md: manager list and init order (`Device` → `Board`),
  layer-separation section (board folder now provides `Board.h` +
  `Board.cpp`; `hardware/interfaces/` role-interface rules; mock
  idiom; escape-hatch rule), "adding a manager" note unaffected.
  The sdkconfig sentence changes from "root-level
  `sdkconfig.defaults.<board>`" to the in-folder overlay.
- `BoardConfig.h` comment: drop the "set LED_PIN to -1" convention;
  boards without an LED omit the `GpioLed` member and bind `MockLed`.
- Delete `docs/backlog/board-device-layering.md` (this spec resolves
  it) and `ideas/board-example.h` (superseded by this spec).

## Error handling

- Missing/wrong board contract: compile error for that board — by
  design, no runtime path.
- Role addressed but not fitted: boards must bind a mock; there is no
  null return. (Single LED today, so this is convention until the
  first multi-instance role arrives.)

## Verification

No automated tests exist; verification is:

1. `idf.py build` (esp32_devkit) — clean build.
2. `grep` for `DeviceManager` returns nothing (sources, CMake, docs
   except git history/backlog references that intentionally remain).
3. Flash a devkit: LED entity still works from Home Assistant
   (discovery, on/off, state).
