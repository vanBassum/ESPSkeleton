---
id: 2026-08-06-16h55
date: 2026-08-06
time: "16:55"
title: A fork's main.cpp was a copy of the framework's init order
builds-on:
supersedes:
---

**Before:** keeping the template and its forks in sync was understood as a file-level
problem. A fork edits the same files the template does — `ApplicationContext.h`, `main.cpp`,
`CMakeLists.txt` — so improvements collide, and separating framework files from application
files would fix it.

**What changed it:** looking at what `main.cpp` actually contained. It was the framework's
init *order*, and that order carries constraints which are invisible from the call site:
Relay after WebServer because it shares the WebServer's `Authenticator`, Telemetry after
Relay because it leaves the device down the relay's pipe. Every fork owned a copy of that
sequence. Adding a framework manager therefore meant hand-placing it in each fork, with
nothing able to detect a wrong placement — it would simply misbehave at boot.

**Now:** what forks were re-deriving was *ordering knowledge*, not file contents. Ordering
belongs to whoever owns the managers, so each layer's context carries its own `Init()` and a
pull brings a new manager's position along with it. Folder separation alone would have left
this exactly where it was: the same `main.cpp`, still holding the same sequence.

Also visible once looked at: the layering the split was meant to introduce was already true
in the code. Nothing outside `main.cpp` called `getBoard()`, and `Board` held a
`ServiceProvider&` it never used. Adopting the layering meant deleting a member, not adding a
mechanism — which is an argument for doing this while the application is still empty rather
than after managers accumulate on both sides of the seam.

**Follows:** three layers, each a context owning its instances and its own ordered `Init()`;
`main.cpp` becomes four calls.
