# Status indication / monitors / alarms

The board `Led` role has no consumer since MQTT/HA were removed
(2026-07-06). Ideas discussed, in increasing order of machinery:

1. **StatusManager** (~70 lines) — a `Timer` polling a couple of states
   (network AP-fallback / connecting / connected) and mapping them to
   blink patterns on `getBoard().GetLed()`. Do this when the dark LED
   starts to bother; it needs no registries and is trivially replaced.
2. **Monitor / signal / alarm stack** (Bas, 2026-07-06) — monitors that
   check the state of something, a signal manager routing outcomes to
   IO, an alarm manager keeping an alarm list. A real annunciator
   pattern, but framework-before-consumer for the template. Decision:
   **not in Strux until a fork proves it** — grow it in the product
   that needs real alarms (thermostat-style: over-temp → buzzer + log),
   then backport what generalizes. Same rule as role interfaces: add
   the abstraction when application code actually speaks it.

Explicitly not SystemManager's job (its charter forbids timers) and not
NetworkManager's (would bind the LED to network vocabulary).
