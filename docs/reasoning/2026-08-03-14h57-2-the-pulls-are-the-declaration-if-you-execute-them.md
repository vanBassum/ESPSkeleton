---
id: 2026-08-03-14h57-2
date: 2026-08-03
time: "14:57"
title: The argument pulls are the declaration, if you execute them instead of reading them
builds-on: 2026-08-03-14h57
supersedes:
---

**Before:** I argued, more than once, that a help command listing a command's
arguments requires the arguments to be *declared* — a metadata table alongside the
handler — and that pull-based access therefore rules help out. The apparent
consequence was a choice between hand-written help that can drift and a metadata
table that has to be maintained.

**What changed it:** Bas's observation that `Args` is an interface, so a different
implementation can be substituted — one that prints each pull instead of fetching it.
The pull already carries the argument's name, its type (from which method was called)
and whether it is required (from which macro wrapped it). Running the argument block
against that implementation *emits the declaration*. There is no second place for an
argument's name to exist, so it cannot drift.

**Now:** enumeration needs the pulls to be **executed**, not read. And it costs
nothing extra, because the interface already had to exist for a different reason — a
JSON-backed implementation serving the old envelope while the console parser serves
the new one, which is also what lets every handler be converted exactly once.

This also makes help global rather than per-category: because help re-dispatches
through the registry, one framework command describes every command in the system.
Nothing is hand-written, so nothing can drift. Prose help survives only for what a
pull cannot express — that an address defaults to zero, that you clear a partition
first when resuming, that a length is in bytes — and it should deliberately not
restate the argument list, since that half is generated.

**The residual problem, and why it has no clean answer.** Something must stop the
handler's body running when the pulls are only describing themselves. Two shapes:

- An end-of-arguments call in every handler. Compact, but forgettable, and forgetting
  it means help *runs* the command.
- Splitting each command into an arguments function and a handler, so help never has
  the body to call. Structurally impossible to get wrong, but it costs a parameter
  struct and a second function per command. Bas rejected it as too much code, which is
  fair — the same objection that killed the metadata table.

Compactness won, with the omission made loud rather than silent: the framework can see
that the end-of-arguments call never happened and name the offending handler.

Worth recording why this cannot be verified at startup instead, because it is a
tempting idea: probing whether a handler stops requires *running* it, and a handler
that forgot the call would then execute during the test. `partition write` executing at
boot is exactly the outcome that cannot be risked. Any check of this property has to
risk the thing it is checking — which is precisely what the split version avoided by
never having the body available.
