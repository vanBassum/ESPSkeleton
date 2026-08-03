---
id: 2026-08-03-12h30-3
date: 2026-08-03
time: "12:30"
title: A borrowed scratch buffer is safe only because each transport runs one command at a time
builds-on: 2026-08-03-12h30
supersedes:
---

**Before:** a command handler needing a few kilobytes of scratch had two options,
both bad. A local array put it on whichever transport task dispatched the command,
where 4 KB of an 8 KB stack is most of the stack (and in the KC1245 fork, 4 KB on a
4 KB stack overran it and surfaced as an unrelated crash inside the network stack).
The heap avoided that, which is why it was used, at the price of allocate/free
churn on a device expected to run for months.

**What changed it:** Bas ruling out dynamic buffers, and noticing that the codebase
already solves this problem elsewhere. The reply framing buffers are fixed-size
members of the transport, lent to each session — no heap, no stack, sized once. The
same lending works for handler scratch.

**Now:** the third option is a buffer *owned by the transport and borrowed by the
handler*. What makes it correct is a property we only just made permanent: each
transport dispatches one command at a time, so its scratch buffer cannot be in two
handlers at once. Rejecting concurrency (note 2026-08-03-12h30) is what turns that
from a current accident into something a design may rely on.

Note the buffer must be **per transport**, not one global static: the local socket
dispatches on the web server's task and the relay on its own, so two handlers really
can run simultaneously across transports. One shared static would race; one per
transport cannot.

Rests on: no transport ever dispatching two commands concurrently. If one ever does,
this breaks silently — the second handler would scribble on the first's buffer — so
that invariant needs stating wherever the buffer is lent, not just remembered here.

Still open, and the reason this is a direction rather than a change: the relay's
inbound frame queue is the worst heap offender (one allocation per WebSocket frame),
and the obvious fixed-size replacement is ~65 KB of a 109 KB free heap. That trade —
queue depth against upload throughput against RAM — has not been decided, and
changing it without deciding would only move the problem.
