---
id: 2026-08-03-15h36-2
date: 2026-08-03
time: "15:36"
title: Having to skip undeclared arguments is what forces the newline terminator
builds-on: 2026-08-03-15h36
supersedes:
---

**Before:** the newline ending the argument section looked like a stylistic choice
between reasonable options — a `--` marker, or "the first token that is not a flag",
or a newline. The argument for newline was that raw binary may begin with a dash, so
a dash-based rule is ambiguous. Real, but it reads as a detail.

**What changed it:** Bas's requirement that the argument reader must leave the stream
at the start of the body *even when the request carries arguments the handler never
declared*. Skipping a declared argument is easy — the parser knows whether a value
follows, because it knows the type. Skipping an **undeclared** one is impossible: its
arity is unknown, so the parser cannot tell whether the next token is that argument's
value or the first byte of the body. Guess either way and a handler eventually reads
argument text as data.

**Now:** consuming to a newline needs no arity knowledge at all, which is why the
newline is not a preference but the only terminator that satisfies the requirement.
The dash-based rule cannot be the primary one, quite separately from the binary
ambiguity.

A same-line body is still worth having — hex typed by hand on one line — so it
survives as a secondary rule: once every declared argument is satisfied, a token not
beginning with a dash *is* the body, starting there. That is safe only for text
bodies, so two restrictions come with it and belong in the docs rather than in
someone's head: a binary body always follows a newline, and undeclared arguments do
not mix with a same-line body.

**The part worth remembering:** ignoring unknown arguments stopped being a judgement
call. It was decided earlier on the grounds that a typo is the caller's problem — a
preference, which a later reader could reasonably reverse. It is now *structural*: if
the parser refused or stopped at an unknown argument, the stream would be left in the
middle of the arguments and the body would be unfindable. Tolerance is load-bearing.

Also corrected while specifying this: reading a JSON envelope "to the closing bracket"
must count braces and respect quotes rather than stopping at the first `}`. Today's
envelope is flat so it cannot bite, but a setting value containing a brace is enough,
and the parser should not depend on the shape of current data.
