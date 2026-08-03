---
id: 2026-08-03-11h59-5
date: 2026-08-03
time: "11:59"
title: Authentication belongs to the transport, so a shared gate is only for transports that need one
builds-on:
supersedes:
---

**Before:** the login handshake was widened to work over any transport, on the
assumption that authentication is a shared layer every transport passes through.

**What changed it:** the KC1245 fork reached the opposite conclusion and wrote it
down. It briefly made the same widening, then reverted it within the hour: its
second transport is Bluetooth, which proves its peer at the link layer through
pairing and bonding, so a login handshake on top would re-ask a question the radio
already answered and would imply a credential nothing provisions. Its gate is
therefore deliberately narrowed to the WiFi socket type.

**Now:** authentication is a property of the physical link, not of the session
layer. Each transport authenticates its own peer by whatever suits it, and the
session layer only ever receives traffic already vouched for. The shared gate is
not "the auth layer" — it is *the password-over-a-session-pipe gate*, used by
transports that need a password and skipped entirely by transports that
authenticate lower down.

Under that framing both repositories are correct and neither needs to reverse:
here the relay carries the actual browser, so it needs the actual password
handshake and the wide type is right; there Bluetooth never routes through the gate
at all, so the narrow type is right and costs nothing. The divergence is only
dangerous while unwritten, because a future reader will "fix" one side into the
other.

Still open: the gate's name still overclaims — it reads as the auth layer rather
than as one credential scheme. Renaming it is the cheap way to stop that
misreading.
