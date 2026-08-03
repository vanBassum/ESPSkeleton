---
name: reasoning-note
description: >
  Preserve the EVOLUTION OF UNDERSTANDING as an append-only log of understanding
  deltas in docs/reasoning/. A delta is one discrete moment where the project's
  mental model changed: a constraint discovered, an assumption falsified, two
  concepts found equivalent, a simpler or more general abstraction found, a
  hidden requirement surfaced, a solution that removed a whole class of problems,
  a once-attractive option that stopped making sense, or the problem reframed.
  Offer a note once an understanding shift has STABILIZED — whether or not any
  decision follows. Do NOT record reasoning that left the mental model unchanged,
  but DO record when a belief's epistemic status changes (verified, disproven,
  ruled out). Notes
  are immutable and atomic (one delta each); new understanding is always a new
  note. Also use to reconstruct how the project came to understand something.
---

# Understanding Delta Log

The primary artifact of this project is its **understanding of the problem**, and
what this log preserves is how that understanding *evolved*. Code, docs, and
decisions are projections of the current understanding. This log is the record of
the changes that produced it.

The unit of capture is an **understanding delta** — one discrete change to the
mental model. Not a decision. Not a transcript of reasoning. A delta. Formally:
each note is an immutable *event*, and the project's current understanding is the
result of replaying all deltas along their backlinks. You store the changes, not
the current state.

## Core principle: compress the path, never the insight

This is the rule that governs every note:

> **Preserve every change to understanding. Compress away everything that merely
> led up to it. Compress the path, never the insight.**

The value is not the conversation — it is the moments where the project's
understanding changed. A note preserves the insight, not the journey to it. Done
right, notes are short because the scaffolding is gone, never because an insight
was thinned. Do not preserve the chain of thought; preserve the deltas it
produced.

**Insights — ALWAYS preserved, at full fidelity:**

- a hidden constraint discovered
- an assumption shown to be false
- two concepts recognized as equivalent
- a simpler or more general abstraction found
- an attractive alternative shown not to work — keep BOTH why it was attractive
  AND what specifically broke it (that reversal is itself an insight)
- a realization that removes an entire class of problems
- the problem itself reframed
- a belief's **epistemic status** materially changed — a hypothesis verified, an
  assumption disproven by measurement, an option experimentally ruled out —
  even when the belief's *content* was already on the table. A confirmed
  suspicion and a negative result are both real understanding.

**Surrounding material — DELETED entirely:**

- brainstorming and "what if…" exploration
- repeated arguments and restatements
- trial-and-error, backtracking, dead ends
- rhetorical questions and thinking-out-loud
- conversation filler

The test for every sentence you write: *does this state a change in the mental
model?* If yes, keep it in full. If it only helped reach the change, delete it.

**Worked example.** The raw conversation was:

> Maybe the ring should be a separate layer. That would keep RecordLog simple.
> Hmm… but reclaim invalidates records… wait… then the ring needs record
> semantics… so maybe…

The note preserves only the delta, and nothing else:

> We realized reclaim fundamentally depends on record semantics, making a generic
> ring layer impossible. The earlier layering looked attractive because it
> promised separation of concerns, but that separation turned out to be
> artificial once reclaim entered the picture.

All the "maybe / hmm / wait" scaffolding disappears; the single understanding
shift survives at full fidelity.

## A delta answers four questions

1. **Before** — what did we believe or assume? (There is always a prior model,
   even if it was "we hadn't considered this at all.")
2. **What changed it** — the constraint, evidence, realization, or reframing that
   moved us.
3. **Now** — what do we understand as a result? Name the assumptions this new
   understanding rests on, and anything still open.
4. **Follows** *(optional)* — any consequence, including a decision. Most of the
   richest deltas have none. Never manufacture one.

If you cannot name a **Before** and a **Now** that genuinely differ, there is no
delta — do not write a note. The difference may be in *content* (we understand
something new) OR in *epistemic status* (a belief we already held is now
verified, disproven, or ruled out by evidence). "Suspected" -> "confirmed by
profiling", or "candidate" -> "experimentally rejected", is a real Before -> Now.
Require materiality: the change must affect how we would design or act, not
merely restate a belief with more confidence.

## What counts as a delta (offer a note)

- A new constraint discovered.
- An assumption surfaced or falsified.
- Two concepts turned out equivalent, or one a special case of another.
- A simpler or more general abstraction found.
- A hidden requirement appeared.
- An elegant solution removed an entire class of problems.
- A previously attractive option stopped making sense.
- The problem got reframed.
- A hypothesis was verified, or an option experimentally ruled out — profiling
  confirms the suspected bottleneck, a benchmark disproves an assumption,
  sharding was tried and did not help. Confirming and negative results both count.
- A decision — but ONLY after extracting the understanding inside it (see below);
  the decision itself goes under "Follows."

A conversation that reshaped how we think but decided nothing is a valid,
valuable delta. A conversation that ended in a decision but changed no
understanding is NOT — record nothing.

**Extract the constraint hiding inside a decision.** Many "pure decisions" are
really a constraint becoming visible. "We chose Postgres because ops already runs
Postgres" is not a note about Postgres — the delta is *existing operational
expertise is a design constraint*. Likewise "we can't afford another runtime",
"deployment simplicity outweighs technical elegance here", "the release deadline
constrains the architecture". Record the **constraint** as the understanding
(Before / What changed it / Now); the database or framework choice is just the
Follows. Only if a decision yields no such constraint or shift — a genuinely free
pick between equal, fully understood options — record nothing.

## One delta per note

Atomic means **one coherent understanding shift**, not one proposition. A
realization and the consequences *inseparable* from it belong in the SAME note —
e.g. "the ring is intrinsic to RecordLog" and "so the layering was artificial"
are one delta, not two. Split into separate notes only when a conversation
produced shifts that are *independently meaningful* — each stands on its own and
would be cited on its own. When it does, write separate files and offer them
together for approval.

## Titles are propositions, not topics

The title states the insight as a sentence you could agree or disagree with — the
delta itself, not its subject:

- NOT "CRC verification" -> "Recomputing the CRC removes the need for a status bit"
- NOT "Record layer" -> "The ring is intrinsic to RecordLog, not layered above it"
- NOT "OT device id" -> "OpenTherm cannot uniquely identify an individual gateway"

Reading the titles in chronological order must read as the story of how the
project's understanding evolved. Write every title to earn its place in that
story.

## Relationships: mostly deltas, not edges

Understanding accretes, so notes relate to earlier notes — but do NOT build a
maintained semantic graph. Every edge is an interpretation that rots, and keeping
edges correct is a bigger job than the notes. Keep only backlinks a new note can
assert about *earlier* notes at the moment of writing, which stay true forever:

- **`builds-on:`** — this delta refines, extends, clarifies, generalizes, or
  specializes an earlier note that STILL HOLDS. (All those finer shades are
  "builds-on"; their specific nature belongs in the prose, not in a field.)
- **`supersedes:`** — the earlier understanding is now WRONG. The old note stays
  untouched; it explains why code built on it exists.
- **`contradicts:`** *(rare)* — use ONLY for a delta whose whole content is "these
  two beliefs cannot both hold" and the tension is not yet resolved. It resolves
  later when a new note supersedes the losing side.

Everything else you might reach for as an edge — "two concepts are equivalent,"
"X depends on Y," "X generalizes Y" — is ITSELF an understanding delta. Write it
as a note (with `builds-on` to the notes it relates) rather than as an edge. This
keeps relationships as first-class immutable insights instead of mutable metadata
to maintain.

## Offer only once the shift has stabilized

Do not offer a note at the first flash of a realization. Discussions often
continue and overturn it minutes later. Wait until the understanding holds — the
topic is settled or the conversation has moved on. A realization reversed within
the same discussion was part of the *path*, not a delta, and must disappear
(compress the path). Capture the stabilized understanding, not each intermediate
step toward it — this keeps spurious note->supersede churn out of the immutable
log.

## Compare against the log, not only the conversation

Understanding often changes outside the live discussion — "I read the spec
yesterday", "the benchmark came back", "a customer reported…". Such statements
carry no in-conversation Before to watch shift, so check them against the
understanding already recorded in the log (loaded at session start). When a new
fact confirms, extends, or contradicts an existing note, that mismatch IS the
Before -> Now — offer a delta (with `builds-on` or `supersedes`). A stated fact
that silently contradicts a prior note is one of the most important deltas to
catch.

## The gate

Draft the note(s), show them, let the user approve or tweak in one quick exchange
(the proposition-title makes this a single glance), then write. Never write
silently.

## Immutability (do not violate)

Never edit or delete a note. Reconstructing *past* understanding is the entire
point, so an immutable record is essential. Corrections and new understanding are
always new, timestamped notes.

## Where notes live

`docs/reasoning/` in the repo root (create it if missing). One file per note —
this makes immutability visible in git (only ADDs, never modifies). Path:
`docs/reasoning/YYYY-MM-DD-HHhMM-short-slug.md`.

Date/time stamped, not numbered. Get the ACTUAL current date and time (run
`date '+%Y-%m-%d %H:%M'`) — never guess. The `id` is that timestamp:
`YYYY-MM-DD-HHhMM` (24-hour, `h` separator, e.g. `2026-07-27-14h32`), also the
filename prefix. Same-minute collisions get `-2`, `-3`. Timestamps sort
chronologically, so the folder reads as the timeline of the project's
understanding.

## Note format

A delta with NO decision — pure understanding (this is normal and complete):

```markdown
---
id: 2026-07-30-11h05
date: 2026-07-30
time: "11:05"
title: OpenTherm cannot uniquely identify an individual gateway
builds-on:
supersedes:
---

**Before:** we assumed OpenTherm probably exposed some per-unit identifier we
could gate KC extensions on precisely.

**What changed it:** reading the spec. OT identity is only ID 3 (Slave config +
MemberID) and ID 127 (product version/type) — a manufacturer + product-type
code, so every KC gateway reports the same MemberID. ID 115/73 looked
identity-ish but are OEM diagnostic codes, not identity.

**Now:** OT can say "this is a KC gateway" (all CLAUDE.md's gating needs) but
never *which* one. Per-unit identity over standard OT is impossible — the
once-attractive idea of reusing a standard OT field for it is dead. Rests on:
product-level MemberID is the identity ceiling for standard OT. Still open:
whether the OT co-processor can even carry a custom data-ID (RA2-396).
```

A delta that also produced a consequence, and invalidated an earlier note:

```markdown
---
id: 2026-10-02-09h15
date: 2026-10-02
time: "09:15"
title: Access must survive the phone being unavailable
builds-on:
supersedes: 2026-07-06-11h20
---

**Before:** phone-only entry was treated as sufficient (note 2026-07-06-11h20).

**What changed it:** a customer whose phone battery died couldn't get in. That
exposed a hidden assumption under the earlier reasoning — "the user's phone is
always available at the door" — invisible until reality removed the phone.

**Now:** "access" carries a hard availability requirement the phone can't
guarantee (dead battery, forgotten phone, guest access). This condemns the whole
*class* of phone-only solutions, not just this one. Phone-only was attractive
because it needed no extra hardware and demoed well — what broke it is
availability, not cost. Still open: which fallback (PIN / NFC / key).

**Follows:** add a PIN fallback; code built on the phone-only entry flow now
needs a refactor.
```

Field notes:

- `id`, `date`, `time` come from the real clock, never a guess.
- `title` states the *shift*, phrased as a proposition.
- `builds-on` / `supersedes` are blank unless the delta relates to an earlier
  note. Add a `contradicts:` line only in the rare unresolved-tension case.
- Omit **Follows** entirely when nothing was decided — most rich deltas have none.

## Retrieval (reconstructing the evolution)

When the user asks how the project came to understand something, what it
currently believes about X and why, what it's resting on, or what's unresolved —
and at the start of substantial work:

1. **While the whole log fits in context, read all of it.** A thousand notes is
   well under 100k tokens; reading everything is the only way to *guarantee*
   nothing relevant was missed. Glob `docs/reasoning/*.md`.
2. **If it's too large**, select the most relevant plus the most recent, and TELL
   the user what you did ("Read all N" / "Searched N, surfaced M") so a miss is
   visible.
3. **Reconstruct the movement, not just the latest state.** Follow `builds-on` /
   `supersedes` / `contradicts` chains. To tell the evolution story quickly, read
   the titles in chronological order — they are the changelog of understanding.
