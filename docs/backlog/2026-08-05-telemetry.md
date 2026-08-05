# Telemetry

A manager records a measurement, the relay puts it in InfluxDB. The chain works end to
end as of 2026-08-05 — an ESP32 point queries back out of Influx tagged by device — and
this is what it still owes.

Related: [the relay in production](2026-08-05-relay-in-production.md) ·
[why the relay never parses a payload](../reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md)

## How it works today

```
manager ──► TelemetryManager::Point ──► session 0xFFFF ──► relay ──► InfluxDB
             (line protocol)            (fire & forget)   (batches)
```

- The **device formats Influx line protocol**. The relay batches lines and POSTs them
  without reading one — same property as everywhere else: it moves bytes.
- **Session `0xFFFF`** is a second reserved device-initiated id alongside session 0.
  Logs are fanned out to browsers; telemetry goes to a database. Different destination,
  so a different id rather than a discriminator inside the payload. The relay's own
  allocation range stops one short so it can never collide.
- `TelemetryManager` samples `device` (heapFree, heapMin, uptime, rssi) on its own task
  every `telem.interval` seconds. Off unless `telem.enabled`.
- Keys are `telem.*`, not `telemetry.*` — **NVS keys are max 15 chars** and `Register()`
  asserts at runtime, so a long key boot-loops the board.

## Open

- [ ] **Buffering.** No buffer at all: a point taken while the relay is down is counted
      and dropped. Deferred deliberately. Before writing one, decide: RAM or flash, what
      depth, and what to throw away when it fills — oldest or newest. A device logging
      every 30 s through a one-hour outage is 120 points, which is small; the question is
      really what happens over a day.
- [ ] **A device tags its own points and the relay does not verify it.** An approved
      device could write points attributed to another device. Injecting `device=<id>`
      server-side means finding the first unescaped space in every line — parsing, and
      the fiddly end of it. Fine while every device is one we installed.
- [ ] **`telem.enabled` is read once at `Init`**, so changing it needs a reboot to take
      effect. Fine, but surprising; either start the task always and gate inside it, or
      say so in the settings label.
- [ ] **Nothing reads the data yet.** Grafana is not deployed. The Influx UI's Data
      Explorer works for ad-hoc looks.
- [ ] **No command surface.** `telemetry stats` returning sent/dropped would make an
      empty graph diagnosable from the device end; the counters exist already
      (`Sent()`, `Dropped()`).

## Facts worth not re-deriving

- **The first field takes no leading comma.** The tag buffer legitimately starts with
  one (it is appended to a tag); the field buffer must not, because it is written
  straight after the space that ends the tags. Getting this wrong produces
  `invalid field format` from Influx and nothing else — every point refused, silently,
  unless you read the relay's log.
- **An integer field needs the `i` suffix** or Influx stores it as a float, and a later
  integer write to the same field is then rejected as a type conflict.
- **A timestamp is only sent when the clock is set** (`YearLocal() >= 2020`). Unsynced,
  the field is omitted and Influx stamps arrival — which, with no buffering, is within a
  second of the truth and beats stamping everything 1970.
- **The relay's write token is write-only on one bucket.** A compromised relay can
  append telemetry and nothing else — it cannot read data back or touch other buckets.
- **Testing the relay with hand-written line protocol proves nothing about the device.**
  It faithfully forwarded a malformed line for 17 points before anyone noticed. The
  formatter needs a real parser at the other end.
