---
id: 2026-08-03-15h36-3
date: 2026-08-03
time: "15:36"
title: Refusing an unknown argument became free once every argument is declared in one call
builds-on: 2026-08-03-15h36
supersedes: 2026-08-03-15h36-2
---

**Before:** unknown arguments were silently ignored, and note 2026-08-03-15h36-2
argued that tolerance had become *structural* — the reader had to skip past them to
find the body, so refusing one would leave the stream mid-arguments and the body
unfindable.

**What changed it:** Bas: "in case someone added an extra argument, we should just
fail and return error". Which dissolves the constraint rather than fighting it. If an
unknown argument is a refusal, the reader never has to skip it — it stops there and
the request is rejected, so the unknowable-arity problem it was reasoning about
simply never arises.

**Now:** the earlier note has it backwards. Tolerance was not load-bearing; it was a
leftover from the *pull-by-pull* shape, where the framework never saw the full set of
declared arguments and therefore could not know whether a name was unknown or merely
not asked for yet. Declaring them all in one call gives the reader the complete set,
so detecting an extra one costs nothing — and the reason for tolerating typos
evaporates with it. A misspelled optional argument is now caught instead of silently
changing behaviour, which was the failure that bothered us in the first place.

The terminator is still a newline, but for the original reason only: raw binary may
begin with a dash, so a dash-based rule is ambiguous. The arity argument is void.

The pattern, third time today: a decision that looked forced was really a consequence
of the surrounding shape. Change the shape and the trade-off disappears instead of
being balanced. Worth suspecting whenever something is described as unavoidable.

One implementation consequence, easy to miss: the JSON envelope's `type` field is the
*command name*, not an argument, so a reader that refuses unknown keys has to know it
is not one — otherwise every request fails on its own routing key.
