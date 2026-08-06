---
id: 2026-08-06-16h54
date: 2026-08-06
time: "16:54"
title: The reply seam pays in discoverability, not in RAM
builds-on: 2026-08-03-15h36
supersedes:
---

**Before:** the reply writer was conceived as the mirror of `ArgReader`, so we expected a
mirrored payoff. On the request side the abstraction earned itself in memory: declaring every
argument at once made the parse zero-buffer (note 2026-08-03-15h36), and the seam is what
would let a different reader delete `JsonArgReader`'s ~700-byte envelope buffer without
touching a handler.

**What changed it:** there is no equivalent prize on the reply side. `JsonScope` already
wrote straight through to the reply stream and held no buffer at all, so the seam has nothing
to delete. Two other candidate payoffs also failed. A describe-the-reply mirror of
`DescribeArgReader` is impossible in principle: arguments are *declared* in one call, which
is what lets `help` re-dispatch a handler and stop it before its body, whereas a reply is
*emitted* as the body computes it — running a handler to learn its reply shape means running
the body. And there is no second reply format wanting to exist today.

**Now:** the seam's value is at the call site. Every scope comes from a factory — the root
from the writer, children from their parent — so a handler never spells a type, `auto`
suffices, and the methods on offer are exactly the enclosing scope's. That is why it survives
being, in effect, `JsonScope` with a vtable underneath: what it buys is that the set of legal
next moves is discoverable, and that no handler names a wire format. Format independence is
real but latent, and resting on JSON staying the only reply format for now.

The general shape: two mirrored seams need not pay in the same currency. The request seam
paid in RAM, the reply seam pays in ergonomics, and looking for the RAM win on the reply side
was looking for the wrong thing.
