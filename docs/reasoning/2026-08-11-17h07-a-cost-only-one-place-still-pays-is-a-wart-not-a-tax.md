---
id: 2026-08-11-17h07
date: 2026-08-11
time: "17:07"
title: A cost only one place still pays is a wart, not a structural tax
builds-on: 2026-08-05-13h55
supersedes:
---

**Before:** running command handlers on whichever task dispatched them was understood as
a *structural* tax — every transport must size its stack for the heaviest handler, so the
price is paid again by each new transport. A command worker task with a work queue was
the fix in waiting, and the private `httpd_ws_get_frame_type` call was read as a symptom
of the same missing thing: a transport that cannot drain a streamed body on its own task
needs somewhere else for the handler to run. Both were deferred, not doubted.

**What changed it:** two facts that were each recorded separately and never put together.
The relay stopped being fed frames and started reading its own socket
(2026-08-05-13h55), so "a transport that cannot read" ceased to exist as a category —
the private symbol is httpd's problem specifically, not a general property of streaming
on a transport task. And the tax got measured instead of reasoned about: the relay task,
which carries TLS *and* the command handlers, had 6412 of 10240 bytes still free
immediately after the handshake.

**Now:** the worst case is affordable and there are two payers, not N. That reframes both
items. A worker task would trade a few KB of RAM for a queue, a hand-off, and a second
place a command can stall — worse than the thing it fixes, at this size. And the private
call is not a placeholder for a coming refactor; it is a permanent, bounded exception,
the only private symbol in the codebase, which fails as a clean link error rather than
silent breakage if IDF ever drops it.

The general shape is what carries: an abstraction that removes a cost is justified by how
many places pay the cost, not by the cost's existence. The same expense in one place is a
wart to be tolerated and named. Rests on the transport count staying at two — a third
transport restores the original argument intact, because it is the *repetition* that was
ever the case for the worker task, not the RAM.

**Follows:** the command-worker-task backlog item is closed rather than deferred. It gets
reopened by a third transport, not by a bad day with stack sizes.
