---
id: 2026-08-03-11h59-6
date: 2026-08-03
time: "11:59"
title: Shared lineage means shared defects, so "we are ahead" is not a reason to skip a backport
builds-on:
supersedes:
---

**Before:** with the template's structure cleaner than the fork's, the fork was
treated as the side that owed a merge. Backporting looked like a cosmetic exercise
in making files diffable again.

**What changed it:** actually reading the fork's fix commits against this tree.
Every applicable one described a bug that existed here too — a broken transport
indistinguishable from a completed request, so a truncated firmware image got
validated and blamed on the upload; leftover body chunks read as a fresh request
header, so one failed upload poisoned the next; multi-kilobyte buffers on
transport task stacks; an inbound queue too shallow for a continuous upload,
dropping chunks invisibly. Worse for us than for them: the fork hit these over
Bluetooth, while this tree's second transport is a wide-area link, which breaks
mid-upload far more readily.

**Now:** structural leadership and defect parity are independent. A fork that
diverges structurally still exercises the *same* shared code on different hardware
and different failure modes, so it functions as a second test rig whose findings
apply upstream. "We are ahead" is a statement about design, never about bugs.

Two second-order facts fell out. A survey based on remembered commits missed a real
truncation defect; only walking every commit that touched shared subsystems found
it. And the fork found its string-truncation bugs because it compiles at a higher
optimisation level, where the relevant warning has enough information to fire —
this tree cannot catch the next one of those without the same setting.
