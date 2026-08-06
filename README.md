# Strux

*Start structured. Make it your own.*

Strux is a flexible foundation for building embedded applications on ESP32. It gives you a clean, modular starting point with WiFi, a web UI, OTA updates, and the infrastructure to grow your project without fighting your own codebase.

It's not a framework that forces you into rigid patterns. It's a well-organized starting point that you copy, rename, and shape into whatever you're building.

<img width="1096" height="591" alt="image" src="https://github.com/user-attachments/assets/cc282e06-f84b-497d-8e5a-3e04add95bac" />

---

## What's Included

- **WiFi** — Station mode with automatic AP fallback (`Strux-AP`) after failed connections
- **Web UI** — React + TypeScript dashboard served from flash, accessible from any browser
- **OTA Updates** — Dual-partition firmware updates and independent web UI updates, no USB after initial flash
- **Live Console** — Stream device logs to the browser in real time over WebSocket
- **Settings** — Key/value store backed by NVS with a dynamic settings UI
- **Time Sync** — SNTP client with timezone support
- **Modular Architecture** — Service provider pattern with isolated managers, easy to extend

## Tech Stack

| Layer | Stack |
|-------|-------|
| Firmware | C++, ESP-IDF v6.0, FreeRTOS |
| Frontend | React 19, TypeScript, Vite, Tailwind CSS, shadcn/ui |
| Target | ESP32 (4 MB flash) |
| CI/CD | GitHub Actions — builds firmware + frontend, publishes releases |

---

## Project Structure

```
Strux/
├── main/                              # ESP-IDF firmware
│   ├── main.cpp                       # Boot sequence — just Init() calls
│   ├── strux/                         # THE FRAMEWORK — pull improvements into a fork
│   │   ├── StruxContext.h             # Owns every framework manager + the init order
│   │   ├── StruxProvider.h            # What one framework manager may reach for
│   │   ├── CommandManager/            # Command dispatch (WebSocket + relay)
│   │   ├── ConsoleManager/            # Log capture + WebSocket broadcast
│   │   ├── NetworkManager/            # WiFi STA/AP with retry and fallback
│   │   ├── RelayManager/              # Outbound pipe for off-LAN access
│   │   ├── SettingsManager/           # NVS key-value store
│   │   ├── SystemManager/             # Device identity, ping/info/reboot
│   │   ├── TelemetryManager/          # Measurements out via the relay
│   │   ├── TimeManager/               # SNTP + timezone
│   │   ├── UpdateManager/             # OTA firmware + www partition
│   │   ├── WebServerManager/          # HTTP + WebSocket server
│   │   └── lib/                       # Reusable utilities
│   │       ├── common/                # Stream, MemoryStream, BufferStream, Fatal
│   │       ├── json/                  # JsonWriter, JsonReader
│   │       ├── protocol/              # CommandContext, ArgReader, ReplyWriter, Session
│   │       ├── rtos/                  # Task, Mutex, Timer, InitState
│   │       └── system/                # DateTime, TimeSpan
│   ├── app/                           # THE APPLICATION — this is what a fork writes
│   │   ├── AppContext.h               # Owns this product's managers
│   │   ├── AppProvider.h              # Peers + getStrux() + getBoard()
│   │   └── LedManager/                # Worked example — delete when you have real ones
│   ├── hardware/                      # THE BOARD — depends on nothing above it
│   │   ├── boards/                    # One folder per target board (-DBOARD=<name>)
│   │   │   └── esp32_devkit/          # Generic ESP32 DevKit (default)
│   │   │       ├── BoardConfig.h      # Pin definitions for this board
│   │   │       ├── BoardContext.h/.cpp # Owns all drivers, answers BoardProvider
│   │   │       └── board.cmake        # Board build fragment (adds BoardContext.cpp)
│   │   ├── interfaces/                # Role interfaces (application vocabulary)
│   │   │   ├── BoardProvider.h        # The roles every board owes
│   │   │   └── Led.h                  # Led role: Set/IsOn + On/Off/Toggle helpers
│   │   └── drivers/                   # Shared drivers, usable by any board
│   │       ├── GpioLed.h              # GPIO implementation of the Led role
│   │       └── MockLed.h              # Led role without hardware (state only)
├── frontend/                          # React web UI (Vite + Tailwind + shadcn)
├── www/                               # Build output — gzipped, embedded in flash
├── CMakeLists.txt                     # Root ESP-IDF project config
├── partitions.csv                     # Flash partition layout
└── sdkconfig.defaults                 # ESP-IDF defaults
```

### The key separation

| Folder | Contains | Changes when you... |
|--------|----------|---------------------|
| `hardware/boards/<name>/` | Pin definitions, the board's `BoardContext` class (owns all driver instances), `board.cmake`, optional `sdkconfig.defaults` overlay | Swap or add a board |
| `hardware/interfaces/` | Role interfaces the application speaks (`Led`) — small, application vocabulary | Application expects a new capability |
| `hardware/drivers/` | Board-independent chip/peripheral drivers implementing the roles | Add a peripheral |
| `strux/` | The framework: managers, dispatch, transports, settings, OTA | The template improves — pull it into a fork |
| `app/` | Your product: managers, business logic, commands | Add features or change behavior |
| `lib/` | RTOS wrappers, JSON, protocol, time utilities — the substrate all three layers use, part of none of them | Rarely — these are stable building blocks |

**Rule of thumb:** if the code changes when you swap the board, it belongs in `hardware/boards/<name>/`. If it's a chip driver several boards could use, it belongs in `hardware/drivers/`. If it changes when you add a feature to this product, it belongs in `app/`. If every fork would want it, it belongs in `strux/`.

Dependencies run one way: the board depends on nothing, the framework depends on the board's *nothing* (it never touches hardware), and the application depends on both. Anything the framework needs from the application arrives by registration — a command, a setting, a telemetry point — never by reaching upward. `app/LedManager/` is the worked example of all three edges at once.

### Multiple boards

The target board is selected at configure time with `-DBOARD=<name>` (default: `esp32_devkit`). Only the selected board folder is put on the include path, so application code just includes `BoardConfig.h` or `BoardContext.h` and gets the right one. The application never changes between boards: it compiles against the `BoardContext` class's surface, and each board makes itself compatible — with real hardware or a mock. Every board implements [`BoardProvider`](main/hardware/interfaces/BoardProvider.h), the list of roles a board owes, so a board that forgets one fails in the board rather than at some call site. To support a new board:

1. Copy `main/hardware/boards/esp32_devkit/` to `main/hardware/boards/<your_board>/` and edit `BoardConfig.h` and `BoardContext.h`/`BoardContext.cpp` (bind each role to a real driver or a `Mock*` one)
2. Add extra board-only source files to `BOARD_SOURCES` in its `board.cmake` (optional)
3. Add `sdkconfig.defaults` in the board folder if the board needs different flash size, PSRAM, or partitions (optional)
4. Build with `idf.py -DBOARD=<your_board> build`

Shared chip drivers (sensors, displays, expanders) go in `main/hardware/drivers/`, parameterized through `BoardConfig` constants so every board can reuse them.

---

## Getting Started

### Prerequisites

- [ESP-IDF v6.0+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/)
- [Node.js 22+](https://nodejs.org/) and [pnpm](https://pnpm.io/)

Or use the included dev container (requires Docker + VS Code with the Dev Containers extension).

### Build & Flash

```bash
idf.py set-target esp32
idf.py build                          # default board: esp32_devkit
idf.py -p /dev/ttyUSB0 flash monitor
```

To build for a different board (see `main/hardware/boards/`):

```bash
idf.py -DBOARD=<name> build
```

If [pnpm](https://pnpm.io/) is installed, the frontend is built automatically as part of `idf.py build`. The React app is compiled, gzipped, and embedded into a FAT partition on flash. No SD card or external storage needed.

If pnpm is not available, the firmware still builds — you just won't have a web UI until you build the frontend manually (`cd frontend && pnpm install && pnpm build`) and reflash.

### Flash from Browser (no toolchain needed)

If you just want to flash a pre-built release without installing ESP-IDF, you can use the **ESP Web Flasher** directly from your browser:

1. Download the latest `Strux-factory.bin` from [GitHub Releases](https://github.com/vanBassum/Strux/releases)
2. Open [ESP Web Flasher](https://espressif.github.io/esptool-js/)
3. Connect your ESP32 via USB
4. Select the serial port, set flash offset to `0x0`, and upload the factory binary
5. Click **Program** — done

This works in Chrome and Edge. No drivers or build tools required.

### Development

For frontend development with hot reload against a running device:

```bash
cd frontend
pnpm dev
```

Vite's dev server will proxy WebSocket connections to the device. Edit React components and see changes instantly.

---

## Architecture

All managers follow the same pattern: they receive a `StruxProvider&` (or `AppProvider&` in `app/`) reference at construction and initialize via `Init()`. This gives you dependency injection without a framework.

```
Board (bottom — depends on nothing above it)
└── the selected board's hardware: LED, sensors, buses

StruxContext (the framework — answers StruxProvider)
├── ConsoleManager        — Captures ESP-IDF logs, broadcasts via WebSocket
├── SettingsManager       — NVS read/write behind typed setting objects
├── SystemManager         — Device identity, ping/info/reboot commands
├── NetworkManager        — WiFi STA/AP with retry and fallback
│   └── WiFiInterface     — ESP WiFi abstraction (swappable for Ethernet)
├── TimeManager           — SNTP time sync with timezone support
├── CommandManager        — Pure dispatcher for commands registered by other managers
├── UpdateManager         — Session-based updates to any partition by label
├── WebServerManager      — HTTP + WebSocket server, static file serving
│   ├── StaticFileHandler
│   └── WebSocketHandler
├── RelayManager          — Outbound pipe so the device is reachable off-LAN
└── TelemetryManager      — Measurements out through the relay

AppContext (the top — answers AppProvider, holds Board& and StruxProvider&)
└── LedManager            — Worked example; replace with your product's managers
```

### Boot sequence (main.cpp)

```cpp
Board board;
StruxContext strux;
AppContext application{ board, strux };

board.Init();         // hardware first: it depends on nothing
strux.Init();         // then the framework the application registers into
application.Init();   // then the product
```

That is the whole file. The order *within* each layer lives in that layer's context — `StruxContext::Init()` owns Strux's ordering, so a fork that pulls a new framework manager gets its position too. Hardware drivers live in the board's `BoardContext` class; application managers reach them through `AppProvider::getBoard()`, and the framework never touches hardware at all.

> **Looking for Home Assistant / MQTT?** That's deliberately not what Strux is for — [ESPHome](https://esphome.io/) does HA-native devices far better. Strux targets product firmware with its own web UI and logic. (An earlier version shipped MQTT + HA discovery; it lives on in git history if a fork wants it.)

---

## OTA Updates

After initial USB flash, the device can be updated entirely over the web UI:

- **Firmware > Application Firmware** — Writes to the inactive OTA slot, then reboots into it
- **Firmware > WWW Partition** — Updates the web UI independently of firmware

The CI pipeline produces three artifacts per release:

| File | Purpose |
|------|---------|
| `Strux-factory.bin` | Full image (bootloader + partitions + app + www) for initial flash |
| `Strux-app.bin` | Firmware only, for OTA update via web UI |
| `Strux-www.bin` | Web UI only, for updating the frontend independently |

---

## WiFi Behavior

1. On boot, attempts to connect to the configured WiFi network (stored in NVS)
2. Retries up to 3 times on failure
3. Falls back to an open access point (`Strux-AP`) if all retries fail
4. Connect to the AP and access the web UI to configure WiFi credentials

---

## Settings

All settings are stored in NVS (non-volatile storage) and configurable through the web UI's Settings page. Each setting is a typed object declared in the manager that owns it and registered in that manager's `Init()`:

```cpp
// In your manager's header:
inline static StringSetting host_{ "myfeature.host", "My Feature Host", ""   };
inline static Int32Setting  port_{ "myfeature.port", "My Feature Port", 1883 };

// In your manager's Init():
strux_.getSettingsManager().Register({ &host_, &port_ });

// Anywhere in the owner — typed, no string keys:
int32_t port = port_.Get();   // NVS value, or the default if unset
```

The web UI auto-generates form fields for each registered setting, grouped by key prefix. Adding a new setting is a declaration plus a `Register()` entry — no central table to edit.

---

## Hardware Layer

The `hardware/` directory contains everything that changes when you swap the board or add a peripheral. It is split into `boards/<name>/` (one folder per target board, selected with `-DBOARD=<name>`) and `drivers/` (shared, board-independent drivers).

### BoardConfig.h

Each board has its own [`BoardConfig.h`](main/hardware/boards/esp32_devkit/BoardConfig.h) with its pin assignments:

```cpp
namespace BoardConfig
{
    static constexpr int LED_PIN = 2;             // GPIO2 on most ESP32 DevKits
    static constexpr bool LED_ACTIVE_HIGH = true;

    // Add your pins:
    // static constexpr int MODBUS_TX_PIN = 17;
    // static constexpr int SPI_MOSI_PIN  = 13;
}
```

### BoardContext

Each board folder provides a [`BoardContext`](main/hardware/boards/esp32_devkit/BoardContext.h) that owns every hardware driver instance (and bus host) and answers [`BoardProvider`](main/hardware/interfaces/BoardProvider.h) — the roles every board owes. Devices the application addresses by *meaning* go through small role interfaces in `hardware/interfaces/` — the included [`Led`](main/hardware/interfaces/Led.h) role is implemented by [`GpioLed`](main/hardware/drivers/GpioLed.h), and a board without the hardware binds a mock ([`MockLed`](main/hardware/drivers/MockLed.h)).

`BoardProvider` lists roles only. A driver whose *full* API the application needs is exposed straight off `BoardContext` as an escape hatch, off the interface — which is what stops the role list turning into the union of every board's peripherals.

This is where you add your project-specific hardware:

```cpp
class BoardContext : public BoardProvider {
public:
    Led& GetLed() override { return led_; }        // a role — on BoardProvider
    // TemperatureSensor& GetSensor() override { return sensor_; }

    // Concrete accessors stay OFF BoardProvider — only boards that have it need it:
    // Dps5020& GetDps5020() { return dps5020_; }

private:
    GpioLed led_{ BoardConfig::LED_PIN, BoardConfig::LED_ACTIVE_HIGH };
    // Ds18b20 sensor_{ oneWire_ };
    // Dps5020 dps5020_{ uart_ };
};
```

---

## Making It Yours

This is a template — copy it, rename it, and build on top of it:

1. **Rename the project** in `CMakeLists.txt` (`project(YourProject)`), `.github/workflows/release.yml`, and `frontend/src/config.ts` (dev-server host + GitHub repo for the release check) — the UI itself needs no renaming: it shows the device name and project name reported by the firmware
2. **Update `BoardConfig.h`** (or add a new board folder under `hardware/boards/`) with your board's pin assignments
3. **Add hardware drivers** in `hardware/drivers/` and instantiate them in the board's `BoardContext` class
4. **Add application logic** as new managers in `app/` — and delete `app/LedManager/`, which is only there as a worked example. Nothing in `strux/` needs editing to add a feature.
5. **Extend the web UI** — add pages in `frontend/src/pages/`, register routes in the sidebar
6. **Add settings** by declaring typed setting members in the owning manager and registering them in its `Init()` (see [Settings](#settings))

### Adding a New Manager

Almost always an *application* manager — see [`app/LedManager/`](main/app/LedManager/) for the worked example:

1. Create a new directory under `app/YourManager/`
2. Implement your manager class, accepting `AppProvider&` in the constructor. It reaches the framework through `services.getStrux()` and the hardware through `services.getBoard()`
3. Add it to `app/AppProvider.h` (forward declare + virtual getter)
4. Add it to `app/AppContext.h` (member, getter, and a call in `Init()`)
5. Register source files in `APP_SOURCES` and the folder in `INCLUDE_DIRS_LIST`, both in `main/CMakeLists.txt`

Nothing in `strux/` is touched — the manager announces itself by registering its commands and settings into the framework from its own `Init()`.

A *framework* manager (one every fork would want) is the same five steps against `strux/StruxProvider.h`, `strux/StruxContext.h` — member **and** a position in its ordered `Init()` — and `STRUX_SOURCES`.

### Adding a New Command

Commands are dispatched by `CommandManager`, but each command lives in the manager that owns its domain. Declare a static command table in your manager and register it in `Init()`:

```cpp
// In your manager's header:
RequestError Cmd_MyThing(CommandContext& ctx);

inline static CommandEntry commands_[] = {
    { "myCategory", "myThing", &InvokeCommand<&MyManager::Cmd_MyThing> },
};

// In Init():
services_.getStrux().getCommandManager().Register(this, commands_);

// The handler: declare every argument in one call, then write the reply.
RequestError MyManager::Cmd_MyThing(CommandContext& ctx)
{
    uint32_t times = 1;
    RETURN_IF_ERROR(ctx.readArgs(Optional("times", times)));

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("times", times);
    return RequestError::Ok;
}
```

Routes are two words (`category command`). `ctx.readArgs(...)` is not optional even with no arguments — it is what positions the stream at the request body and what lets `help list -category X -command Y` describe the command by re-dispatching it. The reply goes through `ctx.reply`, never by naming a wire format. The frontend calls commands via the WebSocket RPC layer in `backend.ts`, and the same commands are reachable through the relay; there is no HTTP command route.

### Adding a Hardware Driver

1. Define pins in the board's `hardware/boards/<name>/BoardConfig.h`
2. Create your driver in `hardware/drivers/` (e.g., `hardware/drivers/MyDisplay.h`), taking pins/buses as constructor parameters. If the application addresses the device by role, add or implement a small role interface in `hardware/interfaces/`
3. Instantiate it in the board's `BoardContext` class and expose it (role interface or concrete accessor)
4. Add component dependencies in `main/CMakeLists.txt` (IDF built-ins) or `main/idf_component.yml` (managed components); board-only source files go in the board's `board.cmake` via `BOARD_SOURCES`

See [`Led.h`](main/hardware/interfaces/Led.h), [`GpioLed.h`](main/hardware/drivers/GpioLed.h), and [`BoardContext.h`](main/hardware/boards/esp32_devkit/BoardContext.h) for a complete example.

---

## License

This project is unlicensed. Use it however you want.
