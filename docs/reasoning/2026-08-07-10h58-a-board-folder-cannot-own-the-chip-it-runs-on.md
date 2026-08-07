---
id: 2026-08-07-10h58
date: 2026-08-07
time: "10:58"
title: A board folder cannot own the chip it runs on
builds-on: 2026-08-06-16h56
supersedes:
---

**Before:** a board folder was believed to own everything that changes when you swap boards.
`BoardConfig.h` for pins, `BoardContext` for drivers, `board.cmake` for extra sources, and an
optional `sdkconfig.defaults` overlay for "flash size, PSRAM, or partitions" — the composition
point for that last one already exists in the root `CMakeLists.txt`, which appends the board's
file after the common one so the board wins on conflicts. The one known hole was component
`REQUIRES`, documented as an ESP-IDF quirk: those resolve in an early expansion pass that runs
`main/CMakeLists.txt` in script mode *without* the `BOARD` cache var, so a board fragment
setting them would always be describing the default board.

**What changed it:** adding a second board that is a different *chip* — an ESP32-C3 SuperMini
next to the Xtensa DevKit. The obvious move was `CONFIG_IDF_TARGET="esp32c3"` in the board's
overlay, so that `-DBOARD=esp32c3_supermini` alone would be enough. It configures, it builds,
and it builds **wrong**: the configure log says `IDF_TARGET is not set, guessed 'esp32' from
sdkconfig`, picks `toolchain-esp32.cmake`, and produces an Xtensa binary for a RISC-V board.
`IDF_TARGET` is resolved in the *same* early pass as `REQUIRES`, before `SDKCONFIG_DEFAULTS`
has been composed and before `BOARD` exists.

So the `REQUIRES` quirk was never a quirk about `REQUIRES`. It is one boundary — everything
ESP-IDF resolves in the expansion pass is out of a board folder's reach — and `REQUIRES` was
merely the first instance of it we hit.

**Now:** the chip is not board-folder data. It is selected with `idf.py set-target`, and a
board's `sdkconfig.defaults` may only carry settings resolved in the normal configure pass
(console routing, flash mode, partitions). Selecting a board is therefore *two* facts, not one:
`set-target` picks the chip, `-DBOARD` picks the pinout, and nothing in the tree can derive the
first from the second.

What makes this worth a note is not the rule but its failure mode. A misplaced `REQUIRES`
silently describes the wrong board; a misplaced `CONFIG_IDF_TARGET` silently builds for the
wrong *architecture*, and the only symptom is a line in the configure log that reads like
routine chatter. The kind of setting that belongs in a board overlay is precisely the kind
whose absence is loud — so the quiet ones deserve naming in `CLAUDE.md` rather than a comment
in the file where they don't work.

**Follows:** `main/hardware/boards/esp32c3_supermini/` with no `CONFIG_IDF_TARGET` in its
overlay and a comment saying why; the `REQUIRES` note in `CLAUDE.md` rewritten as an
early-expansion-pass boundary with both instances under it; `README.md` documenting board
selection as a chip *and* a board.
