---
id: 2026-08-05-15h29
date: 2026-08-05
time: "15:29"
title: A document asserts the present tense, so it rots; a note asserts a date, so it cannot
builds-on:
supersedes:
---

**Before:** writing understanding down durably meant writing a document. `docs/` held
four kinds of artifact — design specs, implementation plans, a backlog, and reasoning
notes — and the specs and plans were treated as the serious ones: the place a decision
was *recorded*, with the notes as commentary alongside.

**What changed it:** a cleanup pass, and what it turned up. `CLAUDE.md` named
`SessionMux` and `CommandSink` as the shared layer above the transport seam, two days
after both stopped existing. `docs/ideas/` was documented as a folder and linked from a
backlog item; it had been deleted. A spec's implementation status described a component
extraction that a later refactor undid. And a page titled *Key facts a fresh session
must know* instructed the reader to `POST /api/login` for a token — an endpoint retired
two steps further down the same page — then to flash the wrong COM port and find the
device at an IP it hadn't had for weeks.

Every one of those was written by someone who cared, and each was true when written.

The cost is not the individual errors, it is what they do to the corpus: a reader cannot
tell a stale assertion from a live one without checking it against the code, and once
that is true of a few pages it is effectively true of all of them. The documents stop
being a shortcut and become a second thing to verify.

**Now:** the mechanism is grammatical. A document's claims are present tense and
unowned by any date — "the shared layer is `SessionMux`" — so every commit silently
invalidates a sentence somewhere, and nothing marks the moment it happened. A note's
claims are scoped to a moment: *on this date, we came to understand this, because of
that*. Code moving afterwards does not make it wrong; it makes it history.

So `docs/` becomes three things and the specs and plans are deleted outright:
`next-up.md` for what is being worked on right now, `backlog/` for later, `reasoning/`
for why — immutable, dated, one delta each. A design document's content splits along the
same seam it should always have had: what is *left* goes to the backlog, *why* goes to a
note, *how to operate the thing* goes to CLAUDE.md, and the narrative of how it was
built goes nowhere, because git already has it.

Rests on: git history being the archive. Nothing was lost by deleting seventeen files —
"deleted" and "unavailable" are different things, and conflating them is what makes
people keep stale documents.

**Follows:** `next-up.md` has to stay small on purpose, and its items have to be
*removed* rather than annotated when they land. Anything persistent that accumulates in
it is a document in disguise, and will rot the same way for the same reason.
