---
id: 2026-08-03-11h59
date: 2026-08-03
time: "11:59"
title: Filing code under a transport asserts that the transport owns it
builds-on:
supersedes:
---

**Before:** the session framing lived in the webserver's folder because the browser
WebSocket was its only user. That felt like a neutral filing decision.

**What changed it:** adding a second transport (the relay). The relay's diff looked
far wider than "one new manager" — a dispatch adapter had to be filed in a third
folder to avoid implying the relay depended on the webserver, the relay had to
include the webserver's header to borrow a credential store, and startup ordering
grew a comment explaining that borrowing. All three were the *same* misfiling
surfacing in different shapes, not three separate problems.

**Now:** a folder location is a dependency assertion, and a false one generates
work indefinitely. The test that settles it without further debate: does the code
depend on a manager? If not, it is a building block and belongs in the shared
library, not under whichever feature happened to need it first. The session framing
depends on nothing but the stream abstraction and moved out; the credential store
declares a setting and therefore stays application code.

Rests on: "depends on a manager" is a proxy for "is application policy". That has
held for every file examined so far, but it is a heuristic, not a proof.
