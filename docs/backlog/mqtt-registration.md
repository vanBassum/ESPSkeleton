# MqttManager registration cleanup

MqttManager's extensibility hooks are `std::function` arrays (capped at
8): `RegisterCommand(name, handler)`, `RegisterDiscovery(callback)`,
plus `PublishEntityDiscovery(..., writeFields)`.

Revisited 2026-07-03: sketched an intrusive-registry version
([`ideas/mqtt-registry-example.h`](../../ideas/mqtt-registry-example.h)),
then parked again — the command registry is request/response, while
MQTT mostly *emits* (discovery, periodic state), so the pattern does
not map cleanly. Also considered folding the registries into one shared
`Registry<Args...>` lib template; undecided. Leave MQTT alone for now.

Also parked from the modular-managers spec (§6): DeviceManager /
HomeAssistant entity inversion.
