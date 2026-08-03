---
id: 2026-08-03-14h57-3
date: 2026-08-03
time: "14:57"
title: Re-scanning the raw argument text beats parsing it into slots up front
builds-on: 2026-08-03-14h57-2
supersedes:
---

**Before:** to let a handler pull arguments by name in any order, I assumed the
framework had to parse the argument line up front into a table of name/value slots
plus an arena holding the values.

**What changed it:** Bas's version kept the raw argument text in one buffer and
re-scanned it from the beginning on each pull. It is cheaper (one buffer instead of
slots plus arena), simpler (no fill step, no slot bookkeeping), and the scanning cost
is irrelevant at this scale — a handful of pulls over a hundred-odd bytes.

It also buys something I had claimed needed a declaration: a single pull can accept a
long *and* a short name, because the pull knows both names it will match and looks for
either. The framework never needs the set of valid names, so short aliases cost no
metadata.

**Now:** pull-by-name in arbitrary order does inherently require the arguments to be
*held* until the handler runs — that part is not avoidable, and it is worth being
honest that "pull-based" hides the storage rather than removing it. What is avoidable
is *structuring* that storage. Keeping the raw text is the cheapest form of holding it.

The storage is bounded by the schema, never by the request: a fixed buffer for the
argument line, and destinations sized by what they hold (seventeen bytes for a
partition label because a label is seventeen bytes). An over-long argument line is
refused rather than growing anything. That is the line that matters — a compile-time
known cost, not one that scales with input.

**Follows:** unknown-argument detection was dropped. Catching a misspelled *optional*
argument would need either a call in every handler or a check reported after the work
already happened, and Bas's call was that a typo is the caller's problem — you get
binary back instead of hex, notice, and try again. Misspelling a *required* argument is
still caught, because its pull fails and the error names what was expected. The
end-of-arguments call survives for an unrelated reason (see
[[2026-08-03-14h57-2]]), not for this.
