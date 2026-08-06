---
id: 2026-08-06-16h56
date: 2026-08-06
time: "16:56"
title: A board provider is affordable once it declares roles only
builds-on: 2026-08-06-16h55
supersedes:
---

**Before:** the board layer deliberately had no base class. The contract was duck-typed so a
board could omit whatever the application never called, and a pure-virtual board interface was
rejected as re-introducing `IBoard` — which would oblige every board to satisfy every
capability any board has.

**What changed it:** separating *roles* from *concrete driver accessors*. The cost is entirely
about concrete accessors: one fixed interface listing them becomes the union of every board's
peripherals, so the day one board grows a display, every other board owes a `MockDisplay`.
Roles are not like that — they are few, they are application vocabulary, and the existing
discipline already required a board without the hardware to bind a `Mock*`, which is what
`MockLed` exists for. So for roles, "every board owes every one" was already the rule, and the
duck-typed freedom it supposedly protected had never been exercised by any board.

A second thing surfaced while working this out: a provider does two jobs — letting peers
*inside* a layer find each other, and stating the contract for the layer *above*. The
pure-virtual form is forced by the first, not the second: a context passes `*this` to its
managers while it is still being constructed, so the reference they hold must be abstract.
Drivers take their pins and buses as constructor arguments and never look for a peer, which is
why the board never needed a vtable for job one — only, optionally, for job two.

**Now:** a role-only board provider is affordable, and what it buys is *where a missing role is
reported*. A board that forgets one now fails in the board, leaving a pure virtual
unimplemented, instead of failing later at a call site in application code that has no idea
which board it is compiling for. What is genuinely given up is real and small: a board must
bind every role even if this product never uses it.

**Follows:** `BoardProvider` in `hardware/interfaces/` declaring roles only;
`BoardContext : BoardProvider`; concrete driver accessors stay off the interface, and
`AppProvider::getBoard()` returns `BoardContext&` rather than `BoardProvider&` so that escape
hatch stays reachable.
