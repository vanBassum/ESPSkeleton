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

There are no automated tests; verification is building, flashing, and driving the device over its own wire:

1. `idf.py build`, `idf.py -p <PORT> flash` — find the port, don't trust a number in a doc.
2. Open a WebSocket to `ws://<device>/ws`, or `ws://<relay>/devices/<deviceId>/ws` through the relay. **No login step** — auth is three ordinary commands (`auth hello|login|resume`) and is off entirely while `web.password` is empty, which is the default.
3. Send a chunk: `[sid u16 LE][flags u8][payload]`, payload starting with the envelope line `{"type":"<category> <command>", …args}\n`, body after it — same chunk or further chunks sharing the sid. `FLAG_FINAL` (0x01) on the last.
4. Read chunks with your sid until one carries `FLAG_FINAL` (or `FLAG_REJECT` 0x02, payload = reason). **Skip session 0** — those are log broadcasts, not your reply. Non-final chunks are reply data, or progress on a long write.
5. Keep each payload inside the transport's inbound window (4096 on both) — a larger frame is refused, not split.

`help list` enumerates every category and command off the device, and `help list -category X -command Y` returns that command's declared arguments, so a probe script needs no source to know what to send.

### Where docs go — three places, nothing else

- **[docs/next-up.md](docs/next-up.md) — what is being worked on *right now*.** Read it first. It is rewritten constantly and deliberately kept tiny: an item is **removed** the moment it lands or is dropped, never annotated, never ticked off in place. Only active work belongs here. If something wants to persist, it does not go in this file — it goes to the backlog (if it is work) or to a note (if it is understanding).
- **`docs/backlog/` — work for later.** One file per topic. A resolved item is **deleted**, not left with a DONE banner; what mattered about it lives in a note by then.
- **`docs/reasoning/` — why things are the way they are.** Append-only, immutable once written, one understanding-delta per note, dated. Never edited: a new understanding is a new note, related to the old one via `builds-on` or `supersedes`. This is the durable record — prefer it over prose documentation anywhere.

Design documents, implementation plans and an ideas folder were all removed on 2026-08-05: they asserted the present tense, so they rotted faster than they were read (see `docs/reasoning/2026-08-05-15h29-a-document-asserts-the-present-tense-so-it-rots.md`). A plan goes in the backlog; the reasoning behind it goes in a note; how to operate something goes in this file. Deleting a doc is not losing it — git has it.

## Architecture

### Manager pattern (dependency injection)

Everything in firmware is a "manager" owned by `ApplicationContext` ([main/Application/ApplicationContext.h](main/Application/ApplicationContext.h)), which implements the pure-virtual `ServiceProvider` interface ([main/Application/ServiceProvider.h](main/Application/ServiceProvider.h)). Every manager:

- takes `ServiceProvider&` in its constructor and reaches other managers through it (never directly),
- has copy/move deleted,
- initializes in `Init()` guarded by an `InitState` (`lib/rtos/InitState.h`), not in the constructor.

`main.cpp` is only ordered `Init()` calls — order matters (Console → Settings → System → Network → Time → Command → Board → Update → WebServer → Relay; Relay is last because it shares WebServer's `Authenticator`). Adding a manager means: create the class, add it to `ServiceProvider`, `ApplicationContext`, `main.cpp`, and `main/CMakeLists.txt` (both `SOURCE_FILES_LIST` and `INCLUDE_DIRS_LIST` — sources are listed explicitly, no globbing).

### Layer separation

- `main/hardware/` — changes when you swap the board. Split into:
  - `boards/<name>/` — one folder per target board: `BoardConfig.h` (pins/constants), `Board.h`/`Board.cpp` (the board's `Board` class — owns every driver instance and bus host, exposes the capability surface the application compiles against; `Board.cpp` is added via `BOARD_SOURCES` in the `board.cmake` fragment), and an optional `sdkconfig.defaults` overlaying the common root one. Selected with `-DBOARD=<name>`; only the chosen board folder is on the include path, so `#include "BoardConfig.h"` and `#include "Board.h"` resolve to it. There is no `IBoard` base class — the contract is duck-typed: a board missing a method the application uses fails to compile for that board.
  - `interfaces/` — role interfaces in application vocabulary (`Led`), 1–3 pure-virtual methods each, never chip or GPIO vocabulary. Drivers implement them (`GpioLed : Led`); a board without the hardware binds a mock (`MockLed`). Add a role interface only when application code speaks in that role; expose a concrete driver accessor from `Board` instead when the application needs a driver's full API (escape hatch). Multi-instance roles get a semantic enum (`Sensor::Ambient`, never `Sensor_2`) mapped by the board — introduce it with the first multi-instance role.
  - `drivers/` — board-independent chip/peripheral drivers shared by boards (e.g. `GpioLed.h`), taking pins/buses as constructor parameters (passed by the board from its `BoardConfig` constants).
- `main/Application/` — changes when you add a feature. Managers and business logic. Hardware driver *instances* live in the board's `Board` class, reached via `ServiceProvider::getBoard()`.
- `main/lib/` — stable building blocks: RTOS wrappers (`Task`, `Mutex`, `Timer`), `Stream`/`MemoryStream`/`BufferStream`, `JsonWriter`/`JsonReader`, `DateTime`/`TimeSpan`. Rarely changes.

Note: `board.cmake` fragments cannot change component `REQUIRES` (ESP-IDF resolves those in an early pass without the `BOARD` cache var). IDF built-in deps go in `COMPONENT_REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt); managed components go in [main/idf_component.yml](main/idf_component.yml).

### Commands (the device's RPC surface)

`CommandManager` is a pure dispatcher — it knows no commands and no other managers. Each command lives in the manager that owns its domain:

- Handlers have the signature `void Handler(Stream& in, Stream& out)`: `in` carries the request payload, the handler writes its complete reply to `out`. Streams are the contract; JSON is a dialect the handler opts into by constructing `JsonReader`/`JsonObject` on line one. Binary payloads (e.g. firmware chunks) use the same contract.
- Owners declare an `inline static CommandEntry commands_[]` table ([CommandEntry.h](main/Application/CommandManager/CommandEntry.h)) with `InvokeCommand<&Owner::Method>` trampolines, and hand it to `CommandManager::Register()` from their `Init()`. Tables must have static storage duration — a registered entry that dies aborts with `FATAL`.
- `help list` is the registry describing itself and the one command `CommandManager` owns: categories and names come off the chain, and a command's *arguments* come from the command itself, by re-dispatching it with a `DescribeArgReader` that prints the declarations instead of filling them and stops the handler at its own `RETURN_IF_ERROR`. So calling `ctx.readArgs(...)` is not optional — a handler that skips it has no `help` and, worse, runs its body when described (logged as an error).
- Two transports reach `Execute()`, and they differ *only* below `SessionLink` ([SessionLink.h](main/lib/protocol/SessionLink.h) — the protocol layer lives in `lib/protocol/`, not under a transport, because the transports depend on it and not the reverse): the local browser WebSocket (`WsSessionLink`, frames read on the httpd task) and the outbound relay pipe (`RelaySessionLink`, frames read on the relay's own task via [RelaySocket](main/Application/RelayManager/RelaySocket.h), a WebSocket driven at the transport layer rather than through `esp_websocket_client` — a callback-delivered frame cannot be the bottom of a streaming handler, and going one layer down is what removed the queue, the per-frame `malloc` and the dropped chunks). Both transports therefore *read* on the task that runs the command. Above that seam everything is shared — `Session` (the stream), `protocol::RunCommandSession` in [CommandEnvelope.h](main/lib/protocol/CommandEnvelope.h) (names the request, dispatches it, closes or refuses the reply), and `AuthGate` — so no handler knows or cares which transport it is serving. There is no HTTP command route; HTTP serves static files only. Wire format is binary session chunks `[session:u16 LE][flags:u8][payload]`, not a JSON envelope.
- Remote access works: `RelayManager` dials out to a server so the device is reachable off-LAN, and the server pulls the device's own frontend with the ordinary `getWebFile` command. Demo server in [relay-server/](relay-server/); what is proven and what is left in [docs/backlog/2026-07-03-remote-access.md](docs/backlog/2026-07-03-remote-access.md). Off by default (`relay.enabled`).

Log lines broadcast to all WebSocket clients via `ConsoleManager`. The frontend side is a singleton `BackendService` ([frontend/src/lib/backend.ts](frontend/src/lib/backend.ts)) that matches replies to requests by id and auto-reconnects.

`UpdateManager`'s entire external surface is its command table: session-based updates addressed by partition label (`updateBegin`/`updateWrite`/`updateEnd`), pull OTA from URL, and partition download. App partitions go through `esp_ota_*` (image validation, running slot refused); data partitions are raw erase+write. The built frontend is gzipped into `www/` and flashed as a FAT partition, updatable independently of the app.

### Settings

Settings are typed leaf objects (`lib`-style, [TypedSettings.h](main/Application/SettingsManager/TypedSettings.h)) declared in the manager that owns them and registered at runtime:

```cpp
inline static UInt32Setting port_{ "myfeature.port", "My Feature Port", 1883 };
// in Init():  settings.Register({ &port_ });
uint32_t p = port_.Get();   // NVS value or the typed default
```

`SettingsManager` is the NVS link; the settings UI is generated dynamically from the registered definitions.

### Deliberately out of scope

MQTT and Home Assistant integration were removed 2026-07-06 (last present at tag-time commit `4a41d74`): devices that exist to live in Home Assistant are better served by ESPHome; Strux is for product firmware with its own UI and (planned) relay-based remote access (`docs/backlog/remote-access.md`). Do not reintroduce an MQTT/HA layer in the template — a fork that truly needs it can resurrect the old managers from git history.

## Conventions

- C++17, no exceptions/RTTI-heavy patterns; `snprintf` with `sizeof` bounds, no `strcpy`/`strcat`.
- JSON is generated with `lib/json/JsonWriter.h` and parsed with `JsonReader` (no external JSON lib); `JsonScope.h` provides RAII `JsonObject`/`JsonArray` with auto-close.
- Firmware version derives from the latest git tag (`v0.1.0` → `0.1.0`) in the root CMakeLists.
