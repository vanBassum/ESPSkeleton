---
id: 2026-08-05-22h33
date: 2026-08-05
time: "22:33"
title: A retry loop turns a log line into a log stream, on both sides of the wire
builds-on: 2026-08-05-22h28
supersedes:
---

**Before:** logging every failed connect attempt looked like diligence. One `ESP_LOGW`
per failure in `RelayManager`, one `events` row per refusal in `relay.py`, and a fixed
5 s retry — each decision defensible where it was written, none of them written with the
others in view.

**What changed it:** a device left un-approved. The retry loop runs for the lifetime of
the device, so "one line per failure" is really "one line per five seconds, forever": the
console showed three lines per attempt (ours plus the TLS and websocket layers) and the
dashboard showed `refused` forty times, with the *approval* that mattered pushed off the
top of a list capped at twenty. The same design mistake, made independently in C++ and in
Python, in the same feature.

The realisation is that a retry loop reclassifies its own logging. Outside a loop, an
attempt and a state change are the same event, so logging the attempt is free. Inside one,
they diverge without bound: attempts scale with time, state changes don't. The log had
been written as though it were reporting to somebody watching, when it is really reporting
to somebody arriving late.

**Now:** both sides log the *reason*, once, and count the repeats.

- The device keeps the last reason and its status and only speaks when either changes; the
  connect that finally succeeds reports how many attempts went unmentioned.
- The server records a `refused` event only on the first sighting of a device/token pair.
  The repeat signal already had a better home — `attempts` on the `pending` row, one row
  that counts rather than N rows that each say the same thing.
- Backoff, because pacing is the other half: unreachable doubles 5 s → 60 s, while refused
  is a fixed 30 s, since it waits on a human and not on a network.

**Two things this exposed that were not about logging:**

`events` was unbounded. `MAX_PENDING = 50` exists with a comment calling a public endpoint
that INSERTs "a disk-filling machine" — and then the same endpoint's refusal path INSERTed
into `events` with no cap at all, ~17k rows/day per un-approved device. A bound placed on
one table is not a bound on a policy; the machine simply found the other door. The
dashboard reading only the newest 20 is what kept it invisible.

A retry loop should also know which failures it cannot retry out of. `relay.url` being
unparseable was being retried every 5 s, reprinting the parser's complaint forever,
although `uri_` is built once in `Init` and cannot change without a reboot. That one now
stops the task. "Retry forever" is a reasonable default only for failures that time can
fix.
