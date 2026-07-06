# Strux

*Start structured. Make it your own.*

Strux is a flexible foundation for building embedded applications on ESP32. It gives you a clean, modular starting point with WiFi, a web UI, OTA updates, MQTT with Home Assistant auto-discovery, and the infrastructure to grow your project without fighting your own codebase.

It's not a framework that forces you into rigid patterns. It's a well-organized starting point that you copy, rename, and shape into whatever you're building.

<img width="1096" height="591" alt="image" src="https://github.com/user-attachments/assets/cc282e06-f84b-497d-8e5a-3e04add95bac" />

---

## What's Included

- **WiFi** — Station mode with automatic AP fallback (`Strux-AP`) after failed connections
- **Web UI** — React + TypeScript dashboard served from flash, accessible from any browser
- **OTA Updates** — Dual-partition firmware updates and independent web UI updates, no USB after initial flash
- **MQTT** — Connects to any MQTT broker with automatic Home Assistant device discovery
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
│   ├── Application/                   # Application logic (managers)
│   │   ├── ApplicationContext.h       # Service locator — owns all managers
│   │   ├── ServiceProvider.h          # Dependency injection interface
│   │   ├── CommandManager/            # Command dispatch (WebSocket + HTTP)
│   │   ├── ConsoleManager/            # Log capture + WebSocket broadcast
│   │   ├── HomeAssistantManager/     # MQTT discovery publishing
│   │   ├── MqttManager/              # MQTT connection + entity registration
│   │   ├── NetworkManager/            # WiFi STA/AP with retry and fallback
│   │   ├── SettingsManager/           # NVS key-value store
│   │   ├── SystemManager/             # Device identity, ping/info/reboot
│   │   ├── TimeManager/              # SNTP + timezone
│   │   ├── UpdateManager/            # OTA firmware + www partition
│   │   └── WebServerManager/         # HTTP + WebSocket server
│   ├── hardware/                      # Hardware abstraction
│   │   ├── boards/                    # One folder per target board (-DBOARD=<name>)
│   │   │   └── esp32_devkit/          # Generic ESP32 DevKit (default)
│   │   │       ├── BoardConfig.h      # Pin definitions for this board
│   │   │       ├── Board.h/.cpp       # The board's Board class — owns all drivers
│   │   │       └── board.cmake        # Board build fragment (adds Board.cpp)
│   │   ├── interfaces/                # Role interfaces (application vocabulary)
│   │   │   └── Led.h                  # Led role: Set/IsOn + On/Off/Toggle helpers
│   │   └── drivers/                   # Shared drivers, usable by any board
│   │       ├── GpioLed.h              # GPIO implementation of the Led role
│   │       └── MockLed.h              # Led role without hardware (state only)
│   └── lib/                           # Reusable utilities
│       ├── common/                    # Stream, MemoryStream, BufferStream, Fatal
│       ├── json/                      # JsonWriter, JsonReader, JsonScope
│       ├── rtos/                      # Task, Mutex, Timer, InitState
│       └── system/                    # DateTime, TimeSpan
├── frontend/                          # React web UI (Vite + Tailwind + shadcn)
├── www/                               # Build output — gzipped, embedded in flash
├── CMakeLists.txt                     # Root ESP-IDF project config
├── partitions.csv                     # Flash partition layout
└── sdkconfig.defaults                 # ESP-IDF defaults
```

### The key separation

| Folder | Contains | Changes when you... |
|--------|----------|---------------------|
| `hardware/boards/<name>/` | Pin definitions, the board's `Board` class (owns all driver instances), `board.cmake`, optional `sdkconfig.defaults` overlay | Swap or add a board |
| `hardware/interfaces/` | Role interfaces the application speaks (`Led`) — small, application vocabulary | Application expects a new capability |
| `hardware/drivers/` | Board-independent chip/peripheral drivers implementing the roles | Add a peripheral |
| `Application/` | Managers, business logic, orchestration, commands | Add features or change behavior |
| `lib/` | RTOS wrappers, JSON, time utilities | Rarely — these are stable building blocks |

**Rule of thumb:** if the code changes when you swap the board, it belongs in `hardware/boards/<name>/`. If it's a chip driver several boards could use, it belongs in `hardware/drivers/`. If it changes when you add a feature, it belongs in `Application/`.

### Multiple boards

The target board is selected at configure time with `-DBOARD=<name>` (default: `esp32_devkit`). Only the selected board folder is put on the include path, so application code just includes `BoardConfig.h` or `Board.h` and gets the right one. The application never changes between boards: it compiles against the `Board` class's surface, and each board makes itself compatible — with real hardware or a mock. There is no `IBoard` base class; a board missing something the application uses simply fails to compile. To support a new board:

1. Copy `main/hardware/boards/esp32_devkit/` to `main/hardware/boards/<your_board>/` and edit `BoardConfig.h` and `Board.h`/`Board.cpp` (bind each role to a real driver or a `Mock*` one)
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

All managers follow the same pattern: they receive a `ServiceProvider&` reference at construction and initialize via `Init()`. This gives you dependency injection without a framework.

```
ApplicationContext (owns everything)
├── ConsoleManager        — Captures ESP-IDF logs, broadcasts via WebSocket
├── SettingsManager       — NVS read/write behind typed setting objects
├── SystemManager         — Device identity, ping/info/reboot commands
├── NetworkManager        — WiFi STA/AP with retry and fallback
│   └── WiFiInterface     — ESP WiFi abstraction (swappable for Ethernet)
├── TimeManager           — SNTP time sync with timezone support
├── CommandManager        — Pure dispatcher for commands registered by other managers
├── MqttManager           — MQTT client connection + entity registration
├── Board                 — The selected board's hardware (LED, sensors, buses)
├── HomeAssistantManager  — Publishes MQTT discovery for registered entities
├── UpdateManager         — Session-based updates to any partition by label
└── WebServerManager      — HTTP + WebSocket server, static file serving
    ├── StaticFileHandler
    └── WebSocketHandler
```

### Boot sequence (main.cpp)

```cpp
g_appContext.getConsoleManager().Init();
g_appContext.getSettingsManager().Init();
g_appContext.getSystemManager().Init();
g_appContext.getNetworkManager().Init();
g_appContext.getTimeManager().Init();
g_appContext.getCommandManager().Init();
g_appContext.getMqttManager().Init();
g_appContext.getBoard().Init();
g_appContext.getHomeAssistantManager().Init();
g_appContext.getUpdateManager().Init();
g_appContext.getWebServerManager().Init();
```

The `main.cpp` stays clean — just `Init()` calls. Hardware drivers live in the board's `Board` class; application managers (like `HomeAssistantManager`) reach them through `getBoard()` and wire them to MQTT for Home Assistant control.

---

## Home Assistant Integration

When MQTT is enabled and a broker is configured, Strux automatically publishes [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) messages. Your device appears in Home Assistant without manual configuration.

**Built-in entities:**

| Entity | Type | Description |
|--------|------|-------------|
| LED | Light | On/off control of the board LED |
| IP Address | Sensor | Device IP (diagnostic) |
| WiFi Signal | Sensor | RSSI in dBm (diagnostic) |
| Uptime | Sensor | Seconds since boot (diagnostic) |
| Free Heap | Sensor | Available RAM in bytes (diagnostic) |
| Reboot | Button | Restart the device remotely |

### Adding your own HA entities

The `MqttManager` is extensible. Any manager can register commands and discovery configs:

```cpp
// In your manager's Init():
auto& mqtt = serviceProvider_.getMqttManager();

// Handle commands from HA
mqtt.RegisterCommand("my_thing", [this](const char* data, int len) {
    // handle ON/OFF or JSON payload
});

// Register HA discovery (called on every MQTT connect)
mqtt.RegisterDiscovery([this]() {
    auto& mqtt = serviceProvider_.getMqttManager();
    mqtt.PublishEntityDiscovery("switch", "my_thing", [&mqtt](JsonWriter& json) {
        json.field("name", "My Thing");
        // ... entity-specific fields
    });
});
```

**MQTT Settings** (configurable via web UI):

| Setting | Default | Description |
|---------|---------|-------------|
| `mqtt.enabled` | off | Enable MQTT connection |
| `mqtt.broker` | — | Broker hostname or IP |
| `mqtt.port` | 1883 | Broker port |
| `mqtt.user` | — | Username (optional) |
| `mqtt.pass` | — | Password (optional) |
| `mqtt.prefix` | strux | Topic prefix (`{prefix}/{device_id}/...`) |

**Topic structure:**

```
{prefix}/{device_id}/status    → "online" / "offline" (LWT)
{prefix}/{device_id}/state     → JSON with sensor values
{prefix}/{device_id}/set/#     → Incoming commands (e.g. set/reboot, set/led)
```

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
inline static StringSetting broker_{ "mqtt.broker", "MQTT Broker", ""   };
inline static Int32Setting  port_  { "mqtt.port",   "MQTT Port",   1883 };

// In your manager's Init():
serviceProvider_.getSettingsManager().Register({ &broker_, &port_ });

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

### Board

Each board folder provides a [`Board`](main/hardware/boards/esp32_devkit/Board.h) class that owns every hardware driver instance (and bus host) and exposes the capability surface the application compiles against. Devices the application addresses by *meaning* go through small role interfaces in `hardware/interfaces/` — the included [`Led`](main/hardware/interfaces/Led.h) role is implemented by [`GpioLed`](main/hardware/drivers/GpioLed.h) and wired to a Home Assistant `light` entity. A board without the hardware binds a mock ([`MockLed`](main/hardware/drivers/MockLed.h)); a driver whose full API the application needs can be exposed directly as an escape hatch.

This is where you add your project-specific hardware:

```cpp
class Board {
public:
    Led& GetLed() { return led_; }
    // Add role accessors, or concrete ones when the app needs the full API:
    // TemperatureSensor& GetTemperatureSensor() { return sensor_; }
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

1. **Rename the project** in `CMakeLists.txt` (`project(YourProject)`) and `.github/workflows/release.yml`
2. **Update `BoardConfig.h`** (or add a new board folder under `hardware/boards/`) with your board's pin assignments
3. **Add hardware drivers** in `hardware/drivers/` and instantiate them in the board's `Board` class
4. **Add application logic** as new managers in `Application/`
5. **Register HA entities** via `MqttManager::RegisterCommand()` and `RegisterDiscovery()`
6. **Extend the web UI** — add pages in `frontend/src/pages/`, register routes in the sidebar
7. **Add settings** by declaring typed setting members in the owning manager and registering them in its `Init()` (see [Settings](#settings))

### Adding a New Manager

1. Create a new directory under `Application/YourManager/`
2. Implement your manager class, accepting `ServiceProvider&` in the constructor
3. Add it to `ServiceProvider.h` (forward declare + virtual getter)
4. Add it to `ApplicationContext.h` (member + getter implementation)
5. Call `Init()` from `main.cpp`
6. Register source files in `main/CMakeLists.txt`

### Adding a New Command

Commands are dispatched by `CommandManager`, but each command lives in the manager that owns its domain. Declare a static command table in your manager and register it in `Init()`:

```cpp
// In your manager's header:
void Cmd_MyThing(Stream& in, Stream& out);

inline static CommandEntry commands_[] = {
    { "myThing", &InvokeCommand<&MyManager::Cmd_MyThing> },
};

// In Init():
serviceProvider_.getCommandManager().Register(this, commands_);
```

Handlers read the request payload from `in` and write their complete reply to `out` — for JSON, construct a `JsonReader`/`JsonObject` on the streams. The frontend calls commands via the WebSocket RPC layer in `backend.ts`; large transfers (uploads/downloads) go through the same commands over `POST /api/command`.

### Adding a Hardware Driver

1. Define pins in the board's `hardware/boards/<name>/BoardConfig.h`
2. Create your driver in `hardware/drivers/` (e.g., `hardware/drivers/MyDisplay.h`), taking pins/buses as constructor parameters. If the application addresses the device by role, add or implement a small role interface in `hardware/interfaces/`
3. Instantiate it in the board's `Board` class, expose it (role interface or concrete accessor), and wire up MQTT entities if needed
4. Add component dependencies in `main/CMakeLists.txt` (IDF built-ins) or `main/idf_component.yml` (managed components); board-only source files go in the board's `board.cmake` via `BOARD_SOURCES`

See [`Led.h`](main/hardware/interfaces/Led.h), [`GpioLed.h`](main/hardware/drivers/GpioLed.h), and [`Board.h`](main/hardware/boards/esp32_devkit/Board.h) for a complete example.

---

## License

This project is unlicensed. Use it however you want.
