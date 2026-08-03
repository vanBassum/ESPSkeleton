---
id: 2026-08-03-11h59-2
date: 2026-08-03
time: "11:59"
title: An inversion of control has to be paid for by something
builds-on: 2026-08-03-11h59
supersedes:
---

**Before:** a routing layer sat between the transports and the dispatcher, handing
opened sessions onward through a callback. It was accepted as the shape the design
needed, and was originally intended to grow into a real multiplexer — several
sessions in flight, a slot table, refusing work while busy.

**What changed it:** looking at what it actually contained: three lines, wrapped in
six member pointers, a nested interface and its own translation unit, reconstructed
on every inbound frame. The callback existed for one mechanical reason — the
session object lives on the stack for a single dispatch, so it could not be
returned to the caller and had to be pushed forward instead. And the multiplexer
future had been designed away: the system dispatches one request at a time and the
relay server serialises per device, so the layer never grew into its name.

**Now:** an inversion of control is a real cost — it turns a readable top-down flow
into a bounce — and it must buy something: policy, lifetime ownership, or multiple
implementations. A stack-scoped object is not enough of a reason. Where the only
justification is "this thing cannot be returned", the caller should construct it
and call downward instead.

The generalisation worth keeping: a layer whose whole body is *forwarding* is a
symptom, not a layer. Two have now been removed on that basis — a stateless
dispatch adapter and this router — and both were introduced to give displaced work
a home rather than to express a boundary.
