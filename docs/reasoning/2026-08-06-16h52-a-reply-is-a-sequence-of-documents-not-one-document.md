---
id: 2026-08-06-16h52
date: 2026-08-06
time: "16:52"
title: A command's reply is a sequence of documents, not one document
builds-on:
supersedes:
---

**Before:** we assumed a handler's reply was a single structured document. That made an
attractive shape for the reply abstraction: one object handed to the handler, owning the
reply stream for the handler's lifetime — "the reply", or a builder for it.

**What changed it:** looking at what the handlers already do. `getWebFile` writes a JSON
header record and then streams the raw file bytes onto the same reply stream. `updateWrite`
emits a `{"p":<written>}` record every 32 KB *during* a long write, flushing each one, and
then a final result record. Neither showed up when surveying `JsonScope` users, because
both hand-rolled their JSON with `snprintf` — the survey was looking for the wrong thing.

**Now:** a reply is a sequence of documents, with raw bytes legal between or after them. So
the thing on the context cannot *be* the reply; it has to be something a handler asks for a
scope from, repeatedly, with the raw stream still reachable alongside it. Opening a second
root is not an error — it is what a progress report is. What must stay an error is writing
to a scope that has already closed, which also means a scope has to close before anything
else touches the stream: the newline dividing `getWebFile`'s header from its body, and the
flush after each progress record, are both load-bearing.

**Follows:** `ctx.reply` is a `ReplyWriter` that hands out scopes; `ctx.out` stays beside it
rather than being replaced by it.
