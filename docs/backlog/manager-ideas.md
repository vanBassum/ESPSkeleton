# Manager ideas (vague, deliberately)

Sketches of managers that might exist someday (Bas, 2026-07-06). None
are designed and none should be built until a real project needs one —
the rule from the role-interface layer applies to managers too: add
the abstraction when application code actually speaks it, prove it in
a fork, backport what generalizes.

- **StatusManager** — timer + poll, maps a few device states to blink
  patterns on the board LED. The smallest of the bunch; ladder and
  ownership reasoning in `status-indication.md`.
- **MonitorManager** — holds multiple *monitors*, each checking the
  state of something (a value in range, a peer reachable, a task
  alive) and yielding an ok/not-ok signal.
- **SignalManager** — routes a named signal to an IO (LED, buzzer,
  relay): monitors raise signals, boards decide what a signal looks
  like physically.
- **AlarmManager** — keeps a list of alarms with state
  (active/acked/cleared), history, and surfacing to the UI. The
  annunciator on top of monitors + signals.

Together the last three form an industrial-style annunciator stack —
plausible for a thermostat-class product (over-temp → buzzer + log +
notification), oversized for the template today.
