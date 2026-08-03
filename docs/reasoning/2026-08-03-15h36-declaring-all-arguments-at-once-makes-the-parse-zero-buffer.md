---
id: 2026-08-03-15h36
date: 2026-08-03
time: "15:36"
title: Declaring every argument in one call makes the parse zero-buffer, so pull-by-pull was the wrong shape
builds-on: 2026-08-03-14h57-2
supersedes: 2026-08-03-14h57-3
---

**Before:** arguments were pulled one statement at a time, and note
2026-08-03-14h57-3 concluded that holding the argument text was *inherent* to
pull-by-name in arbitrary order — pull number three asks for a name that may be
anywhere, so something has to have kept it. Re-scanning a raw buffer was the cheapest
way to hold it, and holding it was accepted as unavoidable.

**What changed it:** Bas replaced the sequence of pulls with a single call that
declares every argument at once, destinations included. That removes the reason to
hold anything: the parser reads a token, finds which declared destination matches,
writes into it, and keeps nothing. Arbitrary order falls out for free, in one pass,
with no storage whatsoever.

**Now:** the buffer was never a consequence of pulling, or of arbitrary order, or of
JSON. It was a consequence of asking for arguments *one at a time*, which forces the
parser to answer questions it has not been asked yet. Declare them together and the
question disappears.

This applies to JSON as much as to a token format. JSON needed random access only
because a field's position was unknown when a pull asked for it; with all
destinations known up front, a JSON parser can assign key by key as it streams and
retain nothing. So the request-sized buffer stops being *necessary* rather than
waiting to be replaced by a different encoding.

Two further things fell out, neither of them the goal:

- **The end-of-arguments marker disappears.** It existed because separate pulls gave
  the framework no way to know when the argument block ended; one call *is* the end.
  In help mode that call returns a sentinel and the error macro propagates it, so the
  handler's body is never reached — and forgetting it is no longer possible, because a
  handler that does not call it has no arguments at all and breaks the first time it
  is used rather than the first time it is described. That is the structural safety
  that previously required splitting every command into two functions.
- **Capacity stops being passed by hand.** A `char (&)[N]` parameter deduces N, so
  `sizeof` leaves the call site, along with the chance of getting it wrong.

Rests on: no command needing to read one argument, branch on it, and then read
different ones. That is the single thing the pull shape could express and this cannot;
nothing here wants it, and it would be questionable design if it did.

Watch: a variadic template instantiates per call signature, which matters on this
target. The variadic layer has to stay paper-thin — build a small stack array of
type-erased descriptors and hand it to one ordinary non-template function — or every
command grows its own copy of the parser.
