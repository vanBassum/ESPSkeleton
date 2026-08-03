---
id: 2026-08-03-11h59-8
date: 2026-08-03
time: "11:59"
title: The request parser needs buffers because its delimiter must be searched for, not because JSON is verbose
builds-on: 2026-08-03-11h59-7
supersedes:
---

**Before:** the peeking, the fixed-size envelope buffer and the double parse of the
same line were treated as awkwardness inherent to carrying JSON, and JSON was
treated as the format.

**What changed it:** two realisations. The format stopped being JSON the moment
firmware bytes were added — a request is a binary chunk, then a newline-terminated
JSON line, then raw body — so JSON is the dialect of one field, not the protocol;
it is already a custom format. And the specific reason the router must look without
touching is that JSON has *no consumable prefix*: fields have no guaranteed order,
so the command name cannot be taken off the front leaving a well-formed remainder.

**Now:** buffers exist because a delimiter has to be *found* before anything can act
on the envelope, and two readers of one line force the first to tiptoe. A positional
format — verb then arguments, each layer consuming its own front — has the prefix
property by construction: no search, no lookahead, the envelope stops being a
concept, and request-side storage drops from the whole envelope to one token.

The usual objection to positional formats is schema evolution: arguments cannot be
reordered or inserted and names do not survive version skew. Per note
2026-08-03-11h59-7 that cost is close to zero here, which is exactly what makes this
a good idea in this system and a bad one in general.

Asymmetric, though: the argument is about requests. Replies stay structured because
the frontend consumes them, so command-line-in and document-out is the shape to
consider — roughly what HTTP does. Nothing is being changed now; the current format
stays.
