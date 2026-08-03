---
id: 2026-08-03-12h30
date: 2026-08-03
time: "12:30"
title: Addressing the write replaces the need for concurrency
builds-on:
supersedes:
---

**Before:** a long upload monopolising the transport was understood as a
*concurrency* problem, so the fix was multiplexed channels — a slot table, several
sessions in flight, per-channel buffers, and handler execution moved onto a worker
task. That had been the recorded plan since 2026-07-03 and was described as "the
real fix for the whole class".

**What changed it:** two things landing together. The gate-watchdog failure was
reproduced (a paced 29-second upload lost its gate at 20 s and died at 884 KB, and
the competing request got a 504 too), which made the cost of long sessions concrete.
And Bas reframed the goal: the problem is not that one request blocks others, it is
that *one request lasts too long*. A write that takes a start address can be cut
short and resumed later, so the sender chooses the session length — and gaps between
pieces are where everything else runs.

**Now:** concurrency and coexistence are different requirements, and only
coexistence was ever needed. Addressing the write buys it with no new machinery at
all: no slot table, no per-channel buffers, no worker task, no session lifetimes to
manage on a device with a few hundred KB of RAM. The sender already knows how it
wants to divide the work, so the device does not need a scheduler to guess.

Verified rather than argued: 1,067,456 bytes in 17 pieces over the relay, 37.7 s
total — nearly double the timeout that killed the single-session attempt — with every
piece accepted and a browser page load succeeding in one of the gaps.

Two consequences worth keeping. Serialisation on the relay is now *permanent* rather
than a stage, so the fix for an uncached page load being N sequential round trips is
server-side caching, not device concurrency. And the worker task that multiplexing
depended on had two independent justifications of its own — paying the
heaviest-handler stack cost once instead of once per transport, and retiring the
private ESP-IDF call used to drain frames on the httpd task. Those survive the
rejection and are recorded separately; they are cost, not correctness.
