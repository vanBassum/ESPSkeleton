# HomeAssistantManager entity registry — design

Part 2 of the MQTT / Home Assistant separation (part 1:
`2026-07-06-mqtt-slim-design.md`, prerequisite). Also resolves the
"DeviceManager / HomeAssistant entity inversion" parked in the old
`mqtt-registration.md`: entities invert — owners declare them and
register with HomeAssistantManager, instead of HA code reaching into
owners.

## Goal

HomeAssistantManager owns everything Home-Assistant-shaped and is the
single consumer of MqttManager in the template. It is a pure layer on
top: deleting its folder plus its two wiring lines removes HA from a
fork without touching MQTT.

## Setting

`ha.enabled` (bool, default **true**) — registered unconditionally, like
`mqtt.enabled`. HA is active only when both `ha.enabled` and MQTT are
up; when inactive it registers nothing with MqttManager and is inert.
Like `mqtt.enabled`, a change takes effect on the next reboot (the
check happens once in `Init()`).

## Entity registry — intrusive chain, Setting idiom

Entities are caller-owned static typed leaf objects (the TypedSettings
"non-removable node" pattern): a base struct carrying identity plus the
intrusive link (`next`/`registered` private, manager is a friend,
`FATAL` on destroy-while-registered and on use-before-registration),
and typed leaves that owners declare and register:

```cpp
// HaEntity.h — base: identity + chain link
struct HaEntity
{
    const char* const component;  // HA component: "light", "sensor", "button", …
    const char* const objectId;   // unique per device, e.g. "led"
    const char* const name;       // display name in HA

    // Writes the entity-specific discovery fields (cmd_t, stat_t,
    // pl_on, dev_cla, …). uniq_id, avty_t and the device block are
    // written by the manager around this call.
    virtual void WriteDiscoveryFields(JsonWriter& json) = 0;

    // Inbound command plumbing: leaves that accept commands return
    // their command subtopic (e.g. "set/led") and handle the payload;
    // pure-state leaves return nullptr.
    virtual const char* CommandSubtopic() const { return nullptr; }
    virtual void OnCommand(const char* data, int len) {}

protected:
    // PublishState helper for leaves: publishes to {objectId}/state
    // via the manager (no-op while disconnected). Reaches the manager
    // through the same registered-backpointer trick as Setting::Manager().
    void PublishState(const char* payload, bool retain = true);
};
```

```cpp
// in HomeAssistantManager:
void Register(std::initializer_list<HaEntity*> entities);
```

Manager semantics:

- One `MqttConnectHook`: on every (re)connect, walk the chain and
  publish each entity's discovery config
  (`homeassistant/{component}/{deviceId}/{objectId}/config`, retained),
  wrapping `WriteDiscoveryFields` with `uniq_id`, `avty_t` and the
  device block exactly as `PublishEntityDiscovery` does today.
- One `MqttSubscription` per commanding entity (`CommandSubtopic() !=
  nullptr`), registered when the entity is registered, dispatching to
  `OnCommand`.
- Registering an entity while connected publishes its discovery
  immediately (preserves late-registration behavior).
- Topic layout, `uniq_id` scheme (`strux_{deviceId}_{objectId}`) and the
  device block are unchanged — existing HA setups see the same entities.

## Typed leaves shipped by the template

Grown on demand, like TypedSettings — start with exactly what the
template uses:

- `HaLight` — ON/OFF light: `cmd_t`/`stat_t`/`pl_on`/`pl_off`
  discovery, `OnCommand` parses ON/OFF and calls an owner hook
  (CommandEntry-style trampoline: function pointer + deduced-owner ctx,
  no std::function), `SetState(bool)` publishes retained state.
- `HaButton` — command-only: `cmd_t` + `dev_cla`/`ent_cat`, owner hook
  on press.
- `HaDiagnosticSensor` — `stat_t` pointing at the shared state topic,
  `val_tpl`/`dev_cla`/`unit`/`icon` fields, `ent_cat: diagnostic`.

A leaf the template lacks is a subclass away; forks add their own
(thermostat: `HaClimate`) without touching the manager.

## Built-ins — dogfooding the registry

The manager's own features become its own registered static entities,
not special cases:

- Four `HaDiagnosticSensor` instances (ip, rssi, uptime, heap).
- One `HaButton` "Reboot" whose hook calls the same reboot path as
  SystemManager's command (delay so any acks flush, then restart).
- The periodic state publisher moves here: the 30 s timer and
  `PublishState()` JSON (ip/rssi/uptime/heap to `{base}/state`) — it
  exists solely to feed the diagnostic sensors' `val_tpl`.

The LED demo stays in HomeAssistantManager as a declared `HaLight`
wired to `getBoard().GetLed()` — it doubles as the reference example
for downstream forks ("add more HA entities here" comment moves onto
it).

## Wiring

`main.cpp` order unchanged (`… Mqtt → Board → HomeAssistant …`).
`ServiceProvider::getHomeAssistantManager()` unchanged. New files
(`HaEntity.h`, leaf headers) live in
`main/Application/HomeAssistantManager/` and are added to
`main/CMakeLists.txt` lists as usual.

## Out of scope

- New entity types beyond the three leaves above.
- Availability per entity (all entities share the LWT status topic).
- Any MQTT-transport concern — that is part 1's spec.

## Verification

Build + flash with a broker and HA instance: device rediscovered with
identical entity ids; LED toggles from HA; diagnostic sensors update;
reboot button works; disabling `ha.enabled` leaves MQTT connected but
publishes no discovery.
