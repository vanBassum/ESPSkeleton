---
id: 2026-08-11-17h08
date: 2026-08-11
time: "17:08"
title: The lib wrappers are a capability surface, not reimplementations of std types
builds-on:
supersedes:
---

**Before:** `main/lib/` looked like a pile of homegrown types that duplicate things the
standard library already has — raised as "cpp already has streams, mutex, whatever" —
and a review was owed to find out how many could be deleted in favour of `std`. The
open question was how much of it was invented-here.

**What changed it:** reading each one against the standard type it supposedly duplicates.
Every one of them has grown at least one capability the `std` type cannot express, and in
each case that capability is load-bearing rather than decorative:

- `Mutex` has `TakeFromISR`/`GiveFromISR`. `std::mutex` has no ISR-safe variant at all —
  not a size argument, a correctness one. `LOCK()`/`ContextLock` goes further and is the
  clearest case: it acquires with a timeout and, on failure, reboots with a diagnostic
  naming the mutex, the holding task, and the file and line that blocked. `lock_guard`
  waits forever and says nothing.
- `Task` sets stack depth, priority and core affinity as arguments of the thing being
  created; all three are used. `std::thread` can reach them on IDF only through
  `esp_pthread_set_cfg()` — out-of-band global state set before construction. That does
  not escape the IDF API, it only hides it from the call site.
- `Stream` carries the zero-copy lending pair (`lendInput`/`lendOutput`/`commitOutput`)
  that removed the 4 KB upload buffer, per-call timeouts, and a `failed()` that separates
  a broken transport from a finished one. Iostreams model none of the three, and bring
  locale and allocation machinery for the privilege.
- `InitState` is "another task waits until this one is ready", with a bounded wait.
  `std::call_once` answers "has this run", which is a different question.
- `DateTime` is wall-clock calendar work — local components, `strftime`-style formatting,
  parsing. C++17 `chrono` has no calendar; that arrives in C++20.

**Now:** these are not reimplementations that happen to share a name with `std` types —
they are the FreeRTOS capability surface, and the standard type is a strict subset in
every case examined. There is nothing to delete, and the review's premise was the thing
that was wrong.

The transferable test is sharper than "does std cover this topic": a wrapper is redundant
when the standard type expresses everything its callers actually *use*. Judged by topic,
all six look redundant; judged by use, none of them are. Rests on C++17 (no `chrono`
calendar, no `span`) and on the FreeRTOS specifics — ISR context, per-task stacks, core
pinning — being genuinely exercised, which they are.

Noticed in passing and not part of this delta: `Task` and `Timer` each hold a
`std::function`, and `Timer::Init` takes a `std::string` name — heap, in types otherwise
careful about it. That is a wart in our own code, not an argument about `std`.

**Follows:** the std-C++ review is closed with nothing replaced.
`std::string_view`/`std::optional`/`std::clamp` stay worth reaching for at call sites that
pass `char*` plus a length, but as a habit when touching such a call site, not as a
project.
