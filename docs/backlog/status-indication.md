# Status indication

The board `Led` role has no consumer since MQTT/HA were removed
(2026-07-06). That's acceptable: the LED's job in the template is to
demonstrate the hardware wiring (BoardConfig → GpioLed → Led role →
MockLed), which it does without a consumer.

If the dark LED starts to bother: a **StatusManager** (~70 lines) — a
`Timer` polling a couple of states (network AP-fallback / connecting /
connected) and mapping them to blink patterns on
`getBoard().GetLed()`. No registries; trivially replaced later by
something richer (see the monitor/signal/alarm sketches in
`docs/ideas/manager-ideas.md`).

Explicitly not SystemManager's job (its charter forbids timers) and not
NetworkManager's (would bind the LED to network vocabulary).
