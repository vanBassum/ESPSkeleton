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

There are no automated tests; verification is building and flashing a device.

## Architecture

### Manager pattern (dependency injection)

Everything in firmware is a "manager" owned by `ApplicationContext` ([main/Application/ApplicationContext.h](main/Application/ApplicationContext.h)), which implements the pure-virtual `ServiceProvider` interface ([main/Application/ServiceProvider.h](main/Application/ServiceProvider.h)). Every manager:

- takes `ServiceProvider&` in its constructor and reaches other managers through it (never directly),
- has copy/move deleted,
- initializes in `Init()` guarded by an `InitState` (`lib/rtos/InitState.h`), not in the constructor.

`main.cpp` is only ordered `Init()` calls — order matters (Console → Settings → Network → Time → Command → Mqtt → Device → HomeAssistant → Update → WebServer). Adding a manager means: create the class, add it to `ServiceProvider`, `ApplicationContext`, `main.cpp`, and `main/CMakeLists.txt` (both `SOURCE_FILES_LIST` and `INCLUDE_DIRS_LIST` — sources are listed explicitly, no globbing).

### Layer separation

- `main/hardware/` — changes when you swap the board. Split into:
  - `boards/<name>/` — one folder per target board: `BoardConfig.h` (pins/constants) plus a `board.cmake` fragment. Selected with `-DBOARD=<name>`; only the chosen board folder is on the include path, so `#include "BoardConfig.h"` resolves to it. An optional root-level `sdkconfig.defaults.<board>` overlays the common `sdkconfig.defaults`.
  - `drivers/` — board-independent chip/peripheral drivers shared by boards (e.g. `Led.h`), parameterized via `BoardConfig` constants.
- `main/Application/` — changes when you add a feature. Managers and business logic. Hardware driver *instances* live in `DeviceManager`, which exposes typed accessors (`getLed()`).
- `main/lib/` — stable building blocks: RTOS wrappers (`Task`, `Mutex`, `Timer`), `JsonWriter`, `Stream`, `DateTime`/`TimeSpan`. Rarely changes.

Note: `board.cmake` fragments cannot change component `REQUIRES` (ESP-IDF resolves those in an early pass without the `BOARD` cache var). IDF built-in deps go in `COMPONENT_REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt); managed components go in [main/idf_component.yml](main/idf_component.yml).

### Frontend ↔ firmware protocol

The React app talks to the device over a WebSocket (`WebSocketHandler`) using JSON commands dispatched by `CommandManager`. Log lines stream over the same socket via `ConsoleManager`. The built frontend is gzipped into `www/` and flashed as a FAT partition; OTA can update the app partition and the www partition independently (`UpdateManager`).

### Home Assistant integration

`MqttManager` handles the MQTT connection; `HomeAssistantManager` publishes MQTT discovery. Any manager can register HA entities: `mqtt.RegisterCommand(name, handler)` for inbound commands and `mqtt.RegisterDiscovery(...)` / `PublishEntityDiscovery(...)` for discovery configs (re-published on every MQTT connect).

## Conventions

- C++17, no exceptions/RTTI-heavy patterns; `snprintf` with `sizeof` bounds, no `strcpy`/`strcat`.
- JSON is generated with `lib/json/JsonWriter.h` (no external JSON lib).
- Settings are typed keys in `SettingsManager` backed by NVS; the settings UI is generated dynamically from definitions.
- Firmware version derives from the latest git tag (`v0.1.0` → `0.1.0`) in the root CMakeLists.
