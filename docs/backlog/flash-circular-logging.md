# Flash-based circular logging (influx-like entries)

Idea (Bas, 2026-07-06, deliberately not worked out yet): a flash ring
logger whose unit is an **entry: a set of key/value pairs** — think
InfluxDB points, not console text lines. This is NOT about persisting
the console log; ConsoleManager has nothing to do with it.

Shape agreed so far, nothing more:

- Build it as a reusable **component** (something ESP-IDF doesn't
  have), developed with **TDD** — an IDF-free core against an abstract
  flash interface, host-compiled tests (MSVC is available on the dev
  machine), a thin esp_partition adapter. The manager in Strux is then
  just a wrapper around the component.

Facts discovered, useful whenever this gets designed:

- The 4 MB flash map is currently exactly full; a log partition means
  shrinking something (www uses ~140 KB of its 896 KB FAT partition,
  so it has room to give). Partition layout changes cost existing
  devices one factory reflash.

Priority: behind the remote-access server. Work out the entry model
(schema? tags vs fields? query/read-back story?) before any code.
