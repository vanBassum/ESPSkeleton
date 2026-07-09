# Review homegrown lib types vs standard C++

Scan `main/lib/` for things that could be replaced by standard C++ (or
ESP-IDF-provided) equivalents instead of maintaining our own. Raised
2026-07-03: "cpp already has streams, mutex, whatever."

Candidates to evaluate, with the questions to answer per item:

- `Mutex` / `RecursiveMutex` / `LOCK()` — vs `std::mutex`,
  `std::recursive_mutex`, `std::lock_guard` (ESP-IDF implements them on
  FreeRTOS via pthread). Do we lose anything (ISR-safety notes, timeout
  locking, static init order)?
- `Stream` / `BufferStream` — vs `std::iostream`/`std::streambuf`.
  Caution: iostreams are notoriously heavy for embedded (code size,
  heap, locale machinery); most embedded projects avoid them on
  purpose. Evaluate honestly, don't assume std wins.
- `Task` / `Timer` — vs `std::thread` (pthread on IDF) / `esp_timer`.
- `DateTime` / `TimeSpan` — vs `std::chrono` (+ `time_t` interop).
- `InitState` — vs `std::once_flag`/`std::call_once`.
- Smaller wins regardless: `std::string_view`, `std::span` (C++20 —
  check toolchain flag), `std::optional`, `std::clamp` in APIs that
  currently take `char*`/len pairs.

Decision criteria: code size (flash), RAM, no hidden heap, no
exceptions/RTTI, FreeRTOS fit (priorities, stack sizes, core pinning),
and readability for people copying this template.
