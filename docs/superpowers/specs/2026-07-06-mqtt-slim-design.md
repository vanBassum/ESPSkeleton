# MqttManager slim-down — design

Part 1 of the MQTT / Home Assistant separation (part 2:
`2026-07-06-homeassistant-entities-design.md`, which depends on this).
Supersedes the parked `docs/backlog/mqtt-registration.md`.

## Goal

MqttManager becomes a generic broker link with **zero Home Assistant
vocabulary** and zero opinion about what rides on top. A fork can use it
without HA; a fork can delete it (together with HomeAssistantManager)
without touching anything else.

## What stays

- Settings (`mqtt.enabled/broker/port/user/pass/prefix`) — registered
  before the enabled check, as today.
- Device id (MAC-derived) and `{prefix}/{deviceId}` base topic.
- Connection lifecycle (esp-mqtt client, event handler).
- LWT: retained `{base}/status` = `online`/`offline`. This is broker
  session mechanics, not HA — HA merely points `avty_t` at it.
- `Publish(subtopic, payload, retain)` — publish under the base topic.
- `IsConnected()`, `GetBaseTopic()`, `GetDeviceId()`.

## What goes (moves to HomeAssistantManager, part 2)

- `RegisterDiscovery`, `PublishEntityDiscovery`, `PublishDiscovery`,
  `WriteDeviceBlock` — all discovery machinery.
- The built-in diagnostic sensors and reboot button.
- `PublishState()` and the 30 s publish timer (its JSON shape exists to
  feed HA `val_tpl` sensor templates).
- `RegisterCommand` and the `{base}/set/#` command convention, including
  the built-in `reboot` handler. The `set/<name>` convention is HA
  vocabulary (somewhere for `cmd_t` to point); generic MQTT only needs
  topic subscription.
- Both capped `std::function` arrays (`MAX_COMMAND_HANDLERS`,
  `MAX_DISCOVERY_CALLBACKS`) — replaced by intrusive chains below.

## New extension surface — intrusive chains, CommandEntry idiom

Registrations are caller-owned static nodes (the settings/commands
"non-removable node" pattern): plain function-pointer handlers with a
`void* ctx` trampoline, `next`/`registered` managed by the manager,
`FATAL` in the destructor if a registered node dies. No heap, no caps.

```cpp
// MqttSubscription.h
struct MqttSubscription
{
    const char* subtopic;   // under baseTopic, e.g. "set/led"
    void (*handler)(void* ctx, const char* data, int len);

    // Managed by MqttManager::Register() — owners never touch these.
    void* ctx = nullptr;
    MqttSubscription* next = nullptr;
    bool registered = false;

    ~MqttSubscription();    // FATAL if registered — same as CommandEntry
};

template <auto Handler>
void InvokeMqtt(void* ctx, const char* data, int len);  // trampoline,
                                                        // same shape as InvokeCommand

// MqttConnectHook.h — "run on every (re)connect"
struct MqttConnectHook
{
    void (*handler)(void* ctx);
    // ctx/next/registered as above; FATAL-on-destroy as above.
};
```

MqttManager:

```cpp
void Register(std::initializer_list<MqttSubscription*> subs);
void Register(std::initializer_list<MqttConnectHook*> hooks);
```

Semantics:

- On every `MQTT_EVENT_CONNECTED`: subscribe each registered
  subscription's full topic (`{base}/{subtopic}`, QoS 1, one subscribe
  per node — the `set/#` wildcard goes away), then invoke connect hooks
  in registration order.
- Registration while already connected subscribes / invokes immediately
  (preserves today's late-registration behavior).
- Inbound `MQTT_EVENT_DATA`: exact match of topic against the
  subscription chain; first match wins; unmatched topics log a warning.
  No wildcard support until a consumer needs it.
- Registering when MQTT is disabled still links the nodes; they are
  simply never subscribed/invoked because the client never starts.
  Consumers must not need to care whether MQTT is enabled.

## Out of scope

- Routing MQTT into CommandManager (stays parked: payloads are raw
  `ON`/`OFF` strings pointed at by discovery configs, not
  request/response envelopes).
- TLS, reconnect tuning, QoS options on Publish — unchanged.

## Verification

Firmware builds; flash a device with MQTT enabled and confirm broker
connect, LWT online/offline, and (after part 2) HA rediscovery. Interim
state after part 1 alone must still build with HomeAssistantManager
ported to the new surface in the same commit series — the two specs are
separate documents but land as one migration; there is no intermediate
commit where HA calls removed APIs.
