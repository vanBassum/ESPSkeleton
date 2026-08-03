---
id: 2026-08-03-14h57
date: 2026-08-03
time: "14:57"
title: Form versus meaning is the one line that organises errors, validation and help
builds-on: 2026-08-03-12h30-4
supersedes:
---

**Before:** "an error is an error". A command returning a failure was one concept, so
a framework error type would naturally grow cases as command authors needed them —
out of bounds, flash write failed, image invalid — and validation and documentation
were similarly undifferentiated.

**What changed it:** Bas, on the framework returning errors: "I don't want the
commands to influence the error returns, the errors are part of the framework not the
user of the framework" — with the example that reading past the end of flash is
information to relay, not a framework error, and that returning *data alongside* an
error is normal.

**Now:** there are two different failures and the boundary between them is whether
the request was *usable* or whether it *worked*. The framework validates form: is the
command known, are the required arguments present, is that number a number. The
handler validates meaning: is that address inside this partition, did the flash
accept the write. The framework cannot cross the line — it has no idea how big a
partition is.

That one distinction settles three things that looked separate:

- **Which channel.** Form failures are a REJECT chunk: the request was not usable, so
  there is no reply. Meaning goes in the reply, which is why it can carry data
  alongside — partial results, how far it got, what went wrong. Conflating them costs
  a client the ability to tell "I sent garbage" from "the device says no", which are
  different things to act on.
- **What the error type may contain.** A closed enum owned by the framework, and
  crucially *named* for request validity rather than "command error" — the name is
  what stops the first author with an out-of-bounds case from extending it.
- **What generated help can describe.** Only form. Which is why prose help is still
  wanted for meaning, and why prose that restates the argument list is the only part
  that can go stale.

Rests on: the framework never needing domain knowledge to decide whether a request is
usable. Holds for everything examined so far, because form questions are all answerable
from the request text alone.
