---
id: 2026-08-05-13h55
date: 2026-08-05
time: "13:55"
title: The relay's buffer was slack across a task boundary we did not have to have
builds-on: 2026-08-02-relay-design
supersedes:
---

**Before:** the relay's per-frame `malloc` was a RAM question. A queue 16 slots deep,
4 KB each, one allocate/free per inbound frame — and the open decision was how much
fixed RAM to spend replacing it: 65 KB for the whole ring, or a smaller window, or
PSRAM, or a small pool that refuses when exhausted. The design doc said to leave it
alone until that trade was decided, because changing it without deciding only moves
the problem. Which was right, and still the wrong question.

**What changed it:** Bas asked why a streaming transport needs a big buffer at all.
It doesn't. Nothing in the session layer holds a payload — a firmware image streams
chunk by chunk, and one chunk is 4 KB because that is what a chunk is. The sixteen of
them were not about payload at all. They were slack between two tasks running at
different speeds: a network that delivers faster than flash accepts.

And a queue cannot fix a rate difference, only a burst. 4 KB per 32 ms sector write is
about 128 KB/s of consumer; anything sustained above that fills any depth and drops a
chunk, which the session layer could not see, so it wrote a hole into somebody's
firmware image. Depth 16 was not a size, it was "raise it until the drops stopped."

Then the second question: why are there two tasks? Because the relay borrowed a
WebSocket client library that owns its own task and delivers frames through a
callback. A callback cannot be the bottom of a streaming handler — the handler asks
for its next chunk, that chunk is only read when the library's loop runs again, and
the loop only runs once the callback returns, which it cannot, because the handler is
inside it. The library also holds its own lock while calling us, the same lock our
replies need, so the callback cannot even wait for the other task to catch up. Every
piece of the machinery — second task, copy, queue, depth, drop path — follows from not
owning the read.

**Now:** the transport reads. One floor below that client library, the same handshake
and framing are an ordinary blocking read, so the relay task connects, reads a frame,
and runs the command it just read. `RecvChunk` is a read on the calling task, exactly
as it always was on the browser socket. What that deleted: the queue, the per-frame
allocation, the sixteen slots, the reassembly buffer, the stale-chunk drain, and the
disconnect sentinel that existed to unblock a reader waiting on a queue. One 4 KB
inbound buffer remains, because a chunk must be whole before it can be routed.
Backpressure is now the TCP window: while flash is busy we are not reading.

The general shape, which is the part worth carrying: **a queue between two tasks is
often the cost of not owning one of them.** When the buffering question looks like
"how much slack do we need", it is worth asking first who is driving, because an
inverted interface (they call us) forces buffering that a direct one (we call them)
does not. The RAM trade did not need deciding — it needed removing.

Rests on: the relay server holding one request in flight per device, so nothing else
is waiting behind a slow command; and control frames being handled inside the read by
the transport, so pings and closes never reach the session layer.

**Follows:** the liveness question moved with it. The library was doing reconnect and
keepalive for us, so the pipe now pings after 30 idle seconds and reconnects when a
ping cannot be sent — which is where the relay-gate watchdog item will have to be
decided, on the device side at least. And residue got better rather than worse: a
handler that returns before the request's FINAL chunk used to mean discarding whatever
sat in the queue, and now means skipping the rest of that session *by id*, which
cannot swallow an unrelated request.
