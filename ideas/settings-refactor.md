# Brainstorm: Distributed, Strongly-Typed Settings

**Status:** Brainstorm — not approved for implementation yet.
**Updated:** 2026-07-02 (supersedes the earlier version of this file; the
registration mechanism it worried about now exists — see
`ideas/registration-pattern.h` and the CommandManager registry on the
`modular-managers` branch).

## The Idea

Move setting definitions out of the central `SettingsDefs.h` into the manager
that owns them, using the registration pattern (intrusive chain, owner
provides the memory, misuse fails at first boot). Each manager declares its
settings as typed `inline static` members and registers them in `Init()`.

```cpp
class MqttManager {
    inline static IntSetting  mqttPort_   {"mqtt.port",    "MQTT Port",    1883};
    inline static BoolSetting mqttEnabled_{"mqtt.enabled", "MQTT Enabled", false};
    inline static StringSetting mqttBroker_{"mqtt.broker", "MQTT Broker",  ""};

    // Init():
    //   serviceProvider_.getSettingsManager().Register({&mqttPort_, &mqttEnabled_, &mqttBroker_});
    // usage:
    //   int port = mqttPort_.Get();
};
```

`IntSetting` / `BoolSetting` / `StringSetting` share an intrusive base
(key, label, type tag, `next`, `registered`, dtor-abort guard — the
registration pattern verbatim). The UI walk uses the base; owners use the
typed leaf. Register() takes base pointers, so heterogeneous types chain
fine (e.g. via std::initializer_list of `Setting*` — no heap).

## Decisions reached so far

### Scattered defaults: mostly a PRO

- The default is domain knowledge: `1883` means "standard MQTT port" next to
  the code that opens the socket; in a central table it's just inventory.
- Completes the rip-out property: today deleting MqttManager leaves orphaned
  `mqtt.*` rows in SettingsDefs.h. Distributed, settings die with their
  folder and the settings UI shrinks automatically.
- Forks stop touching shared files: new `climate.*` settings live entirely in
  the fork's new manager — no merge conflict with backports.
- The "overview" we lose barely exists: the authoritative overview is the
  runtime-generated settings UI (and the `getSettings` dump), not the header.

Real costs to solve:
1. UI grouping/order — chain order is registration order. Fix with a `group`
   field or sort by key prefix. Cosmetic but real.
2. Cross-cutting settings need an owner (see SystemManager below).

### Settings become PRIVATE to their owner

Key insight: managers that don't own a setting shouldn't read it by key —
they go through the owner's typed API
(`getSystemManager().GetDeviceName(buf, len)`, not `getString("device.name")`).
Consequences:

- Each key string exists in exactly ONE place: the owner's definition. The
  string becomes an NVS storage detail, like a DB column name — not an API.
- Strong typing everywhere: consumers get compiler-checked signatures;
  owners get typed `Get()`/`Set()` on the entry handle; defaults are typed
  values (`1883`, `false`), not strings parsed at boot.
- SettingsManager's by-key getString/setString remains only for: the owner's
  own accessors, and the generic settings-UI path (setSetting from frontend).

### Typed defaults kill ApplyDefaults()

With the default stored (typed) in the entry, `Get()` falls back to it when
NVS has no value. No boot-time NVS writes; "user never changed this" stays
distinguishable from "user set it to the default".

### Registration timing

Register in `Init()`, unconditionally — NOT register-on-first-use (rejected:
unused settings would be missing from the frontend). Linking + stamping the
SettingsManager pointer doesn't need NVS; NVS is only touched at first
`Get()`, and managers read their own settings during their own `Init()`,
which runs after SettingsManager::Init(). The full schema is registered
before WebServerManager (last in main.cpp) can serve the UI.

### Rejected: central list with #ifdefs per manager

Solves cross-cutting on paper, but it's the exact central coupling the
registration pattern removes — the shared file still changes for every
manager, just conditionally. Wacky. Dead end.

## Open: SystemManager for cross-cutting settings

Most keys map cleanly: `wifi.*` → NetworkManager, `mqtt.*` → MqttManager,
`ntp.*` → TimeManager. The orphans are `device.name` (read by Network for
hostname, Mqtt for discovery) and `device.pin` (read by CheckAuth).

Candidates considered:
- DeviceManager — no: that's the hardware tree, `device.name` isn't hardware.
- CredentialManager — no: a whole manager for one PIN.
- SettingsManager owns them itself — workable, but mixes mechanism with
  schema ownership.
- **SystemManager (leaning yes)** — owns device identity and lifecycle:
  - `device.name` + typed `GetDeviceName()`
  - `device.pin` + the `CheckAuth(json, resp)` helper (auth belongs with the
    credential owner)
  - possibly the generic `ping`/`info`/`reboot` commands (they are "system"
    commands) — which would make CommandManager a PURE dispatcher with zero
    manager dependencies.

  Scope guard: "SystemManager owns device identity and lifecycle." The
  moment someone wants a timer or watchdog in it, the answer is no.

  Cost: one more manager (wiring in ServiceProvider/ApplicationContext/
  main.cpp/CMakeLists).

**Not yet decided:** whether SystemManager takes CheckAuth + ping/info/reboot
from CommandManager, or only the two settings. Also undecided: UI grouping
mechanism (`group` field vs key-prefix sort).

## Decisions from the sketch review (2026-07-03)

See `ideas/settings-registry-example.h` (third revision) for the code shape.

- Explicit types: `Int32Setting`/`UInt32Setting`/`FloatSetting`/`BoolSetting`/
  `StringSetting`. No bare `int`. Float bit-casts through u32 (NVS has no
  float). Double deferred (ESP32 FPU is single-precision; no-default switches
  make adding it loud later).
- Defaults are `const`. Managed fields private/protected — encapsulation is
  the compiler's job.
- `Get()`/`Set()` before `Register()` **crashes** (with the key name): a
  forgotten registration is found on the first test run, never shipped as a
  silent default.
- Guard policy: failures that would corrupt the chain (double-register,
  destroy-registered) are unconditional log+abort; sloppiness checks (key
  length, flash residency of key/label/string-default via `esp_ptr_in_drom`)
  may be `assert`. Flash residency cannot be checked at compile time
  (pointer values don't exist until link time).
- `ResetToDefaults()` becomes: erase NVS namespace + commit. Nothing is
  written back — defaults resolve at read.
- Out of scope, recorded: validation (valid-but-wrong passes any validator —
  "bad config" is not the schema's job) and onChange hooks (only real use
  case is hot-applying UI changes without reboot; today's save→reboot UX is
  fine and nothing blocks adding hooks later).

## Next up (separate small feature)

A `FATAL(fmt, ...)` helper in `lib/` for unrecoverable situations:
unconditional (never compiled out, unlike assert under NDEBUG), logs the
message + `__FILE__:__LINE__`, then aborts into the ESP-IDF panic backtrace —
the informativeness of assert with the reliability of abort. The registration
pattern's abort() guards (settings sketch AND the shipped command registry)
switch to it once it exists.

## Relation to other work

- Mechanism: `ideas/registration-pattern.h` (bare pattern),
  `main/Application/CommandManager/CommandEntry.h` (first application).
- Broader modularity design: `docs/superpowers/specs/2026-07-02-modular-managers-design.md`
  (this settings refactor is its parked item §8).
