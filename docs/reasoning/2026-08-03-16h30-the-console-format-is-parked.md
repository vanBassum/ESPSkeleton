---
id: 2026-08-03-16h30
date: 2026-08-03
time: "16:30"
title: Parking the console format turns everything deferred to it into an open decision
builds-on: 2026-08-03-15h36
supersedes:
---

**Before:** the console request format was the near-term direction. Several things were
deferred to it explicitly — the zero-buffer parse, unknown-argument enforcement, the
deletion of the JSON reader whose own comments called it "on its way out". Handlers were
converted to `readArgs` precisely so the flip would be one line.

**What changed it:** counting the remaining motivations honestly, once `readArgs` had
banked the architectural half.

- The zero-copy body read does **not** depend on the format. I had claimed it did. The
  JSON envelope is newline-terminated and the reader already leaves the stream at the
  body, so what zero-copy needs is `Session` exposing its current chunk — available now.
- The buffer saving is roughly 700 bytes of dispatch stack. Real, but not the RAM
  problem; the relay's per-frame allocation and the 4 KB upload buffer are, and neither is
  format-related.
- Which left hand-typability as the only motivation standing, and Bas: "we have a frontend
  for that anyway its more convineint and less error prone."

**Now:** JSON is the request format for the foreseeable future. The console form is
parked, not abandoned — the pull contract keeps the flip cheap whenever it is wanted, and
that decoupling is the part worth having regardless of encoding.

**The generalisation worth keeping.** "Defer it until X" is a bet on X arriving soon. When
X is parked, every deferral silently converts into an absence, and the recorded reasons
stop being reasons — they were about X's timing, not about the thing itself. So parking a
direction means walking back through what was parked *behind* it, rather than inheriting
"not done" by default.

Concretely, and this is where the walk-back landed somewhere I did not expect:
unknown-argument enforcement stays parked *with the format*, rather than being
retro-fitted. Bas: "the specific behaviour is tied to that implementation we may or may
not build in the future." He is right — the decision that an extra argument should fail
was made while discussing a format where an undeclared `-name` is unambiguous. In JSON it
needs a quote-and-depth-aware key scan, which is exactly the code I got wrong on a first
attempt, and the benefit is thinner when the only client is a generated frontend that
cannot typo.

So the deferral was revisited rather than inherited, and the answer was still "not now" —
but for the format's reason instead of the parked-format's reason. The point of walking it
back is to *re-derive* the answer, not to change it.

`RequestError::UnknownArgument` is therefore removed from the enum: nothing can produce
it, and a value no code path reaches is an interface making a promise it does not keep. It
comes back with the reader that needs it.

Also cleaned up: the token reader, the framework usage sketch and a line-reading helper
that existed only to serve the old envelope are deleted rather than left in the tree. Code
describing a direction the project is not currently taking reads as intent, which is the
same reason a folder's location is an assertion (note 2026-08-03-11h59). Git and these
notes hold the design; an unused header pretending to be next does not.
