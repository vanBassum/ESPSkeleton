# Flash-based circular logging (influx-like entries)

Idea (Bas, 2026-07-06): a flash ring logger whose unit is an **entry
(Record): a set of key/value pairs (Fields)** — InfluxDB-style points,
not console text lines. ConsoleManager is unrelated.

**Prior art: `C:\Workspace\FlashLoggerV2` — continue there, don't
restart.** It is a live strict-TDD project (CMake + GoogleTest, its
own CLAUDE.md workflow: failing test first, user owns design,
LogBook.md is Bas-only) whose LogBook already settled much of the
design:

- Record = multiple Fields; repeated keys continue one value (e.g.
  8-byte timestamp over two 4-byte fields).
- Variable-length records with **Option B** overhead (one header field
  per record, CRC, header written last for crash safety) — chosen via
  flash-efficiency analysis over static layouts.
- Record start: reserved keys (`0xFF` empty, `0x00` erased), NOT
  timestamp-as-marker (considered and rejected — leaks housekeeping
  into application keys).
- `format(key_size, value_size)` / `init()` with magic+CRC header:
  implemented. Field layer (fixed-size indexed fields, append,
  validation): implemented. **Record layer: not started** — that's the
  continuation point, then circular/sector behavior.
- Thread safety: field layer has none by decision (caller's
  responsibility) — in Strux that lands in the wrapping manager.

Strux side, whenever the library is usable: consume it as a component
(esp_partition adapter implementing its `IFlash`), one thin manager as
the thread-safe wrapper. Facts to remember: the 4 MB flash map is
currently exactly full — a log partition means shrinking something
(www uses ~140 KB of 896 KB), and partition layout changes cost
existing devices one factory reflash.

Priority: behind the remote-access server.
