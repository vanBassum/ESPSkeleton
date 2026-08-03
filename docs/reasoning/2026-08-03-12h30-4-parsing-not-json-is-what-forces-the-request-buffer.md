---
id: 2026-08-03-12h30-4
date: 2026-08-03
time: "12:30"
title: Named-field parsing is what forces a request buffer, and generating output never did
builds-on: 2026-08-03-11h59-8
supersedes:
---

**Before:** JSON was treated as one choice applying to the whole protocol, so
dropping it looked like an all-or-nothing move — and the request buffer looked like
the price of using it.

**What changed it:** separating the two directions. Generating structured output is
inherently single-pass: fields are emitted in the order chosen, so a writer never
holds the document. Parsing into *named* fields is not, because a field may be
anywhere, so the whole request must be in memory before the first field can be read.
The buffer was never about JSON's verbosity; it is the cost of random access.

**Now:** the format can be asymmetric without being inconsistent — a single-pass
console form for requests, JSON for replies. And the win is not "smaller buffers", it
is that storage stops being *request*-sized and becomes *schema*-sized: a partition
label needs seventeen bytes because a label is seventeen bytes, and the request's
length stops entering into it. A compile-time known cost instead of one that grows
with input.

Two refinements from Bas that the first design got wrong. Order-independent named
arguments do **not** require buffering, as long as each flag carries its own value —
each token goes straight into the caller's variable, and the only thing a handler
cannot do is act before it has read all its flags, which is just having its
parameters. But the *body* must be last, and an explicit `--` has to separate it,
because raw bytes may legitimately begin with a dash so "the first token that is not
a flag" is ambiguous.

The larger consequence: this is the unlock, not just a cleanup. A body cannot begin
until the parser has found the end of the JSON, which is why the body is copied into
a buffer of its own. Arguments that end exactly where they end mean the body starts
at the next byte, so a handler can consume the transport's chunk in place. That is
what retires the 4 KB heap buffer.

What it does not fix, and should not be claimed to: the relay's per-frame
allocation. That is queue design and still needs its RAM decision.

Verified on hardware for the two commands with no frontend callers, chosen exactly so
the format could be proven without touching the web UI. Both request forms dispatch
side by side, told apart by the first byte, and the JSON branch is explicitly
temporary — it disappears with the last unconverted handler.
