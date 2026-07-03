# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Strux is a template/foundation for ESP32 firmware (ESP-IDF v6.0, C++, FreeRTOS) with a React web UI. It is meant to be copied and renamed into new projects, so keep the core generic — several downstream forks (e.g. the KC1245 Thermostat) backport improvements from and to this repo.

## Build commands

Firmware (requires ESP-IDF v6.0+ environment):

```bash
idf.py set-target esp32
idf.py build                          # also builds the frontend if pnpm is installed
idf.py -p <PORT> flash monitor
idf.py -DBOARD=<name> build           # select a board from main/hardware/boards/ (default: esp32_devkit)
```

Frontend (React 19 + TypeScript + Vite + Tailwind + shadcn/ui, package manager is pnpm):

```bash
cd frontend
pnpm dev          # hot-reload dev server, proxies WebSocket to a running device
pnpm build        # tsc -b && vite build && gzip into ../www (embedded in flash as FAT image)
pnpm typecheck    # tsc --noEmit
```

There are no automated tests; verification is building and flashing a device. Design notes and deferred ideas live in `docs/backlog/`.

## Architecture

### Manager pattern (dependency injection)

Everything in firmware is a "manager" owned by `ApplicationContext` ([main/Application/ApplicationContext.h](main/Application/ApplicationContext.h)), which implements the pure-virtual `ServiceProvider` interface ([main/Application/ServiceProvider.h](main/Application/ServiceProvider.h)). Every manager:

- takes `ServiceProvider&` in its constructor and reaches other managers through it (never directly),
- has copy/move deleted,
- initializes in `Init()` guarded by an `InitState` (`lib/rtos/InitState.h`), not in the constructor.

`main.cpp` is only ordered `Init()` calls — order matters (Console → Settings → System → Network → Time → Command → Mqtt → Device → HomeAssistant → Update → WebServer). Adding a manager means: create the class, add it to `ServiceProvider`, `ApplicationContext`, `main.cpp`, and `main/CMakeLists.txt` (both `SOURCE_FILES_LIST` and `INCLUDE_DIRS_LIST` — sources are listed explicitly, no globbing).

### Layer separation

- `main/hardware/` — changes when you swap the board. Split into:
  - `boards/<name>/` — one folder per target board: `BoardConfig.h` (pins/constants) plus a `board.cmake` fragment. Selected with `-DBOARD=<name>`; only the chosen board folder is on the include path, so `#include "BoardConfig.h"` resolves to it. An optional root-level `sdkconfig.defaults.<board>` overlays the common `sdkconfig.defaults`.
  - `drivers/` — board-independent chip/peripheral drivers shared by boards (e.g. `Led.h`), parameterized via `BoardConfig` constants.
- `main/Application/` — changes when you add a feature. Managers and business logic. Hardware driver *instances* live in `DeviceManager`, which exposes typed accessors (`getLed()`).
- `main/lib/` — stable building blocks: RTOS wrappers (`Task`, `Mutex`, `Timer`), `Stream`/`MemoryStream`/`BufferStream`, `JsonWriter`/`JsonReader`, `DateTime`/`TimeSpan`. Rarely changes.

Note: `board.cmake` fragments cannot change component `REQUIRES` (ESP-IDF resolves those in an early pass without the `BOARD` cache var). IDF built-in deps go in `COMPONENT_REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt); managed components go in [main/idf_component.yml](main/idf_component.yml).

### Commands (the device's RPC surface)

`CommandManager` is a pure dispatcher — it knows no commands and no other managers. Each command lives in the manager that owns its domain:

- Handlers have the signature `void Handler(Stream& in, Stream& out)`: `in` carries the request payload, the handler writes its complete reply to `out`. Streams are the contract; JSON is a dialect the handler opts into by constructing `JsonReader`/`JsonObject` on line one. Binary payloads (e.g. firmware chunks) use the same contract.
- Owners declare an `inline static CommandEntry commands_[]` table ([CommandEntry.h](main/Application/CommandManager/CommandEntry.h)) with `InvokeCommand<&Owner::Method>` trampolines, and hand it to `CommandManager::Register()` from their `Init()`. Tables must have static storage duration — a registered entry that dies aborts with `FATAL`.
- Two transports reach `Execute()`: the WebSocket (`WebSocketHandler`, JSON envelope with `id`/`type`) and a generic `POST /api/command` HTTP route used for large transfers (firmware uploads are chunked; partition downloads stream).

Log lines broadcast to all WebSocket clients via `ConsoleManager`. The frontend side is a singleton `BackendService` ([frontend/src/lib/backend.ts](frontend/src/lib/backend.ts)) that matches replies to requests by id and auto-reconnects.

`UpdateManager`'s entire external surface is its command table: session-based updates addressed by partition label (`updateBegin`/`updateWrite`/`updateEnd`), pull OTA from URL, and partition download. App partitions go through `esp_ota_*` (image validation, running slot refused); data partitions are raw erase+write. The built frontend is gzipped into `www/` and flashed as a FAT partition, updatable independently of the app.

### Settings

Settings are typed leaf objects (`lib`-style, [TypedSettings.h](main/Application/SettingsManager/TypedSettings.h)) declared in the manager that owns them and registered at runtime:

```cpp
inline static UInt32Setting port_{ "mqtt.port", "MQTT Port", 1883 };
// in Init():  settings.Register({ &port_ });
uint32_t p = port_.Get();   // NVS value or the typed default
```

`SettingsManager` is the NVS link; the settings UI is generated dynamically from the registered definitions.

### Home Assistant integration

`MqttManager` handles the MQTT connection; `HomeAssistantManager` publishes MQTT discovery. Any manager can register HA entities: `mqtt.RegisterCommand(name, handler)` for inbound commands and `mqtt.RegisterDiscovery(...)` / `PublishEntityDiscovery(...)` for discovery configs (re-published on every MQTT connect).

## Conventions

- C++17, no exceptions/RTTI-heavy patterns; `snprintf` with `sizeof` bounds, no `strcpy`/`strcat`.
- JSON is generated with `lib/json/JsonWriter.h` and parsed with `JsonReader` (no external JSON lib); `JsonScope.h` provides RAII `JsonObject`/`JsonArray` with auto-close.
- Firmware version derives from the latest git tag (`v0.1.0` → `0.1.0`) in the root CMakeLists.
