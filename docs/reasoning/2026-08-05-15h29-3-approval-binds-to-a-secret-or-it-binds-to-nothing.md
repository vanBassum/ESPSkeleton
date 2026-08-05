---
id: 2026-08-05-15h29-3
date: 2026-08-05
time: "15:29"
title: An approval binds to a secret or it binds to nothing — and the device can generate the secret
builds-on: 2026-08-05-15h29-2
supersedes:
---

**Before:** the device→server credential looked like a provisioning chore — mint a
secret per device somewhere, configure it on both sides, keep a list. Tedious enough to
keep deferring, which is why it stayed open for as long as the relay was LAN-only, and
why "does this ever face a public network" sat at the top of the next-up list as a
question rather than a task.

**What changed it:** Bas proposed something that does not sound like a credential at
all: the device connects, is blocked, and he approves it after logging in.

Following it through twice.

First, it *is* the credential problem. Approval is a one-time act; connecting is not.
The device reconnects on every reboot, every WiFi blip, every server restart. So unless
approval mints something the device keeps, the next connection claiming that id proves
nothing — either you approve every reconnect (unusable) or approval binds to the id,
which is id-as-password with a nicer first run. Pairing is not an alternative to a
secret; it is a way of issuing one.

Second, and this is the part that made it cheap: the secret does not have to come from
the server. If the device generates a random token at first boot, stores it, and presents
it on every connect, the server only has to *pin* the pair on approval — trust on first
use, the same shape as an SSH host key. That removes the entire return path: no
server→device control message, no network-triggered write to NVS, and no change to the
session protocol at all, because the token rides as a header on the WebSocket upgrade.
Compare the server-mints version, which needs a new verb, a delivery guarantee, and a
device that writes flash because a stranger told it to.

**Now:** the general form is worth keeping. **An approval flow needs a secret to bind
to, and the cheapest place to generate one is whichever side can store it without being
told.** Asking who should *issue* the credential is the wrong first question; asking who
can *hold* it decides the protocol.

A smaller companion insight from the same pass: refuse before accepting the upgrade
(HTTP 403), not by accepting and closing. The device's transport reports the upgrade
status, so a 403 reaches its log as a reason; a close reason is swallowed by the
transport layer that handles control frames on our behalf. Refusing earlier is both
cheaper and more legible.

**Follows:** eviction becomes safe again once identity is proven. Replacing an existing
pipe on reconnect looks reckless while an id is all anyone presents, but it is *required*
behaviour — after a device reboots, the server still holds a socket that will not notice
for another thirty seconds, and refusing the newcomer locks the device out for that long.
With a token, the newcomer has proved it is the same device, so replace is correct and
the dangerous version is refusing.
