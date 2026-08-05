# Modular Managers — Design

**Date:** 2026-07-02
**Status:** Approved (brainstormed with Bas)
**Branch:** `modular-managers`

## Goal

Any *feature* manager can be removed from a Strux-based project by deleting its
folder and its wiring lines — with **zero edits inside surviving managers**.
The compiler, not convention alone, catches anything missed.

Constraints that shaped every decision:

- No dynamic memory. Everything statically allocated; "it compiles and links"
  should mean "it fits and works."
- Failures are boot-deterministic: a misuse fails on the first run of the
  offending code, on the desk — never sporadically in the field.
- Compile-time references (typed getters), never lookup-by-string.
- Thread-safe by contract: managers survive "idiotic" use from any task.
- Keep it simple. Strux is a template you copy and hack freely; ceremony that
  fights that loses.

## Decisions

### 1. Layering: two layers, no `services/` folder

`hardware/` (boards + shared drivers, already restructured) and `Application/`
(managers). A third `services/`-vs-`application/` split was considered and
rejected: its main benefit (confining fork diffs for backports) doesn't pay
rent because backporting between Strux and its forks is occasional manual
work, not a workflow to optimize. Clarity comes from the rule below instead.

### 2. Core vs. feature managers

| Kind | Managers | May be referenced by |
|---|---|---|
| **Core** | Settings, Console, Command | anyone |
| **Feature** | Network, Time, Mqtt, HomeAssistant, Update, WebServer, Device | wiring files only |

Wiring files: `ServiceProvider.h`, `ApplicationContext.h`, `main.cpp`,
`main/CMakeLists.txt`. These are the composition root — the one place allowed
to know the full roster. They always change when adding/removing a manager;
that is the pattern working, not a smell.

**Documented exceptions (MQTT "stacks"):** HomeAssistantManager → MqttManager
and DeviceManager → MqttManager (entity self-registration, see §6). Both are
explicit stacks on top of MQTT: removing Mqtt implies removing HA and removing
DeviceManager's entity-registration lines. No other feature→feature references
are allowed.

**Removal procedure** (goes in README/CLAUDE.md): delete the manager's folder,
remove its lines from the four wiring files, build. Typed getters guarantee
every stale reference is a compile error at the offending line.

### 3. Command registry (CommandManager inversion)

Today CommandManager implements commands *about* four other managers' domains
(wifiScan, settings, logs, update status), which is why those managers can't
be ripped out. It becomes a pure dispatcher; each manager registers its own
commands in `Init()`.

The normative sketch was `ideas/command-registry-example.h`, deleted since; the
shipped shape is `CommandEntry.h` plus the `commands_[]` tables in each manager.
Summary of the mechanism:

- `CommandEntry { name, handler, ctx, next, registered }` — the entries
  themselves are the links of an intrusive chain. Owners hold them as
  `inline static CommandEntry commands[]` members (static storage, immortal).
- `template <size_t N> void Register(void* ctx, CommandEntry (&commands)[N])`
  — array reference, count deduced, can never be wrong. Register stamps `ctx`
  (the owner's `this`) into each entry and links it. Owners never touch
  pointers.
- Handlers are plain function pointers `void(*)(void* ctx, const char* json,
  JsonWriter& resp)` — a static trampoline in the owner casts `ctx` back and
  calls a private member. No heap, no `std::function`, no max-commands
  constant. RAM cost ≈ 24 bytes/command.
- **Lifetime enforcement:** `~CommandEntry()` aborts (ESP_LOGE + `abort()`,
  device resets) if the entry is registered. A compile-time forbid (deleted
  dtor) is impossible — it would propagate through the owning manager to the
  global `ApplicationContext`. Destroying a stack-registered table therefore
  fails on the *first run, every run*.
- **Misuse asserts (boot-deterministic):** re-registration (would cycle the
  chain and hang dispatch) and duplicate names.
- **Thread safety:** a `RecursiveMutex` guards the chain. `Register` holds it
  across its whole check-and-insert loop; the private `Find` re-takes it
  (recursion). `Dispatch` looks up under the lock but runs the handler
  *outside* it — safe because entries are immortal — so a handler may register
  commands or dispatch nested commands without deadlock.

**New prerequisite:** `lib/rtos/RecursiveMutex.h` — `RecursiveMutex : public
IMutex` wrapping `xSemaphoreCreateRecursiveMutex` /
`xSemaphoreTakeRecursive` / `xSemaphoreGiveRecursive` (~20 lines). Works with
the existing `LOCK()` / `ContextLock` via `IMutex`. No ISR variants
(recursive mutexes have none; commands never run in ISRs).

### 4. Command relocations

| Command | Moves to |
|---|---|
| `wifiScan` | NetworkManager |
| `getSettings`, `setSetting`, `saveSettings` | SettingsManager |
| `getLogs` | ConsoleManager |
| `updateStatus`, `partitions` | UpdateManager |
| `ping`, `info`, `reboot` | stay in CommandManager (fully generic) |

After relocation CommandManager references no feature managers. The command
*names and payloads do not change* — the frontend is untouched.

### 5. Authentication: out of the registry

A per-command auth flag mixes security policy into a routing table
(cross-contamination of responsibilities). The registry knows nothing about
auth. Handlers that guard state-changing operations (`setSetting`,
`saveSettings`, `reboot`) call the existing `CheckAuth` helper (exposed by
CommandManager, reads `device.pin` from Settings — core→core, allowed) as
their first line. Today's PIN-in-JSON protocol and frontend behavior are
preserved exactly. If a richer model is ever wanted, it becomes flags/enum on
the *session or transport* — never a bool per command entry.

### 6. DeviceManager stays the compile-time device tree; HA entity inversion

DeviceManager keeps typed accessors (`getLed()`) and static driver instances —
no string lookup, no registry. It is the embedded analog of a device tree with
compile-time resolution: delete a device, and every stale use is a compile
error.

The LED's Home Assistant entity (discovery + `led` command + state publish)
moves from HomeAssistantManager into DeviceManager, registered through
MqttManager's existing `RegisterCommand` / `RegisterDiscovery` /
`PublishEntityDiscovery` hooks — the same hooks any fork uses for its own
entities. HomeAssistantManager keeps only generic device-level entities (IP,
WiFi RSSI, uptime, free heap, reboot button) and drops its DeviceManager
reference entirely.

Note: DeviceManager thereby gains references to Mqtt (feature→feature). This
is the same "stack" shape as HA→Mqtt and must degrade gracefully: it
registers its entities but everything works with MQTT disabled. If a project
removes MqttManager entirely, DeviceManager's entity-registration lines are
deleted along with it (compiler-guided, wiring-level change in *application*
code the fork owns anyway).

### 7. Thread-safety contract (manager convention, goes in CLAUDE.md)

Every manager must be safe under arbitrary cross-task use:

- `InitState` guards initialization (double/concurrent `Init()`).
- A `Mutex` (or `RecursiveMutex` where re-entry is by design) guards mutable
  state.
- Locking convention: public methods lock; private `*Locked` methods document
  "caller holds the lock." Never hold a lock while calling out to arbitrary
  user code (handlers, callbacks).
- Misuse that cannot be made safe is made *loud*: assert/abort at first
  occurrence rather than silent corruption.

### 8. Out of scope (parked, deliberately)

- MqttManager's `RegisterCommand`/`RegisterDiscovery` still use
  `std::function` (heap) — candidate for the same intrusive-entry treatment
  later.
- Settings-definition refactor (`ideas/settings-refactor.md`) — same
  distributed-registration philosophy, separate effort.
- Relay/tunnel server for reaching devices from outside — future feature
  manager; nothing in this design constrains it.

## Verification

No automated tests exist; verification is build + on-device:

1. `idf.py build` for `esp32_devkit` (and `pnpm typecheck` — frontend must be
   untouched).
2. Flash a devkit; from the web UI exercise every relocated command: settings
   page load/save (PIN prompt), wifi scan, console history, update status,
   partitions, reboot.
3. With MQTT configured: device appears in Home Assistant with LED light plus
   the generic diagnostic entities; LED toggles from HA.
4. Rip-out smoke test (the point of it all): on a scratch branch, delete
   `Application/HomeAssistantManager/` + wiring lines → must build; then also
   delete `Application/MqttManager/` + wiring + DeviceManager's entity
   registration → must build.
