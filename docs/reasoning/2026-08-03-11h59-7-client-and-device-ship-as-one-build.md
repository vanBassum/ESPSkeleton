---
id: 2026-08-03-11h59-7
date: 2026-08-03
time: "11:59"
title: Client and device always ship as one build, so wire-format compatibility is not a constraint
builds-on:
supersedes:
---

**Before:** an unexamined assumption that the request/reply wire format carried the
usual cost of change — versioning, negotiation, staying compatible with clients
already deployed.

**What changed it:** the frontend is built into the firmware image and served from
the device's own flash. Even remote access serves that same image: the relay server
fetches the frontend from the device rather than hosting one of its own. So there is
no client that can be older or newer than the device it talks to.

**Now:** the wire format between frontend and firmware has no compatibility surface
and can be changed freely — no negotiation, no version field, no deprecation
window. This is what makes otherwise-unaffordable format choices affordable here,
and it is a property of this system rather than a general truth.

Rests on: the frontend is only ever served from the device it talks to. Still open,
and a real hole in that: the frontend partition can be flashed independently of the
application, so a mismatched pair is constructible. It is not a supported
configuration and nothing currently detects it.
