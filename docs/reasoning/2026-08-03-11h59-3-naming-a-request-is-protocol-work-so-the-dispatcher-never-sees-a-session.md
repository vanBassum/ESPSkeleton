---
id: 2026-08-03-11h59-3
date: 2026-08-03
time: "11:59"
title: Naming a request is protocol work, so the dispatcher must never see a session
builds-on: 2026-08-03-11h59-2
supersedes:
---

**Before:** the work of naming a request — find where the envelope ends, pull the
command name out of it — was treated as belonging to the router, on the reasoning
that extracting a routing key *is* routing. The dispatcher was given the session
object to do it with.

**What changed it:** seeing what that actually put inside the dispatcher. Only one
line of it dispatched anything; the rest was reaching into a raw buffer, scanning
for a delimiter, and closing or refusing a reply. A maximum envelope size had been
invented in the router, of all places. The dispatcher had been the single piece of
the request path that could be reasoned about without a transport, and handing it a
session destroyed exactly that property.

**Now:** "extracting the routing key is routing" is wrong — extracting it is
*parsing*, and parsing the wire format belongs to the layer that owns the wire
format. The dispatcher's contract stays a command name plus two streams, which is
what keeps it independently understandable. The envelope, and the short sequence
that names a session, dispatches it and closes it, live in the protocol layer.

That sequence is a plain function templated on the dispatcher, not a class or an
interface: the protocol layer must not depend upward, and a virtual method would
reintroduce the forwarding layer removed in note 2026-08-03-11h59-2. Duck-typing
matches how this project already handles the equivalent problem for boards.

Rests on: there being exactly one command-envelope convention. A second wire
dialect would need a second such function, not a parameter on this one.
