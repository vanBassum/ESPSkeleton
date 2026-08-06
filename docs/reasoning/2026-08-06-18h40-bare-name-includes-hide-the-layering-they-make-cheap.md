---
id: 2026-08-06-18h40
date: 2026-08-06
time: "18:40"
title: Bare-name includes hide the layering they make cheap to change
builds-on: 2026-08-06-16h55
supersedes:
---

**Before:** the layer split put `lib/` under `strux/`, which read as obvious — the framework
is far and away its biggest consumer, and `lib` had lived beside the framework's managers
since before there were layers at all.

**What changed it:** Bas said lib should be root level, because the app and the drivers can
use it too. Checking who actually includes what settles it harder than the argument does:

- `hardware/boards/esp32_devkit/BoardContext.h` includes `InitState.h`, which was
  `strux/lib/rtos/InitState.h`. That file's own header comment says the board layer "depends
  on nothing above it — not the framework, not the application."
- `app/LedManager` includes `InitState.h` and `Timer.h`, same origin.

So the layer below and the layer above were both reaching inside the framework's folder, on
the very day the split was made to stop exactly that.

**Why nobody saw it:** every folder is on the include path, so headers are included by name
alone. That convention was chosen deliberately — it is what made the whole reorg cost zero
source edits, because moving a file between layers does not touch its callers. The cost is
the mirror image: **an include that names no path cannot show a path violation.** In a
codebase where `#include "InitState.h"` is written identically from all three layers, the
only artifact that records which layer a header belongs to is the folder it sits in, and the
only reader who checks is a human looking at the tree.

The property is worth stating plainly because it will recur: this project's include style
makes the *directory* the sole carrier of layering intent. Grep cannot audit it. The
compiler cannot audit it. Reviewing the tree is the audit.

**Now:** `main/lib/` sits beside `hardware/`, `strux/` and `app/` rather than inside any of
them, with its own `LIB_SOURCES` list. It is not a fourth layer — it is the substrate the
three layers stand on, and the test for whether something belongs in it is that it **names no
layer**. `Task`, `Mutex`, `Stream`, `JsonWriter`, `DateTime` and the request/reply seam all
pass; a manager never would.

**What this does NOT license:** hoisting `hardware/drivers/` to the root by the same
argument. The test is who includes it, and the answers differ — `lib` is included from all
three layers today, `drivers/` only from boards. Drivers are to the board layer what `lib`
used to be mistaken for: its private building blocks, correctly placed inside it.
