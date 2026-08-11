---
id: 2026-08-11-11h45
date: 2026-08-11
time: "11:45"
title: The cache's invalidation point was already in the transport, so nothing needs to be announced
builds-on: 2026-08-11-11h19
supersedes:
---

**Before:** having established that the firmware version cannot key the cache
(2026-08-11-11h19), we treated freshness as a property to be *derived from the
content*: content-hashed asset names for the immutable files, a short TTL on
index.html, and — one step further — a digest the device announces on connect so the
server could tell a reflash from a WiFi flap. All three answer the same question, "have
these bytes stopped being true", by asking something about the bytes.

**What changed it:** the question has a structural answer that needs no content at all.
A device's frontend lives on a flash partition; it cannot start serving different bytes
without rebooting; and rebooting drops the relay pipe. So every moment a device's
content can change is a moment the server *already observes* as a connect. Nothing has
to be announced, compared, or expired: connect is the invalidation point, and it was
there the whole time — one layer below where we were looking.

**Now:** the cache's lifetime is a connection. Entries are dropped when a device
connects and kept, unexpiring, for as long as that connection lasts. That collapses
three mechanisms into one, and it removes the firmware change entirely — which is the
deeper reason to prefer it: a content digest works, but it puts one transport's caching
strategy into the framework, where no other transport has an opinion about it. The
device was being asked to describe itself for the convenience of one consumer.

It also turns the cache from reactive to eager. If connect is when the content is known
to be current, connect is also when to fetch it: index.html, then the assets index.html
names, pulled in the background before any browser asks. Fetching what index.html
*names* rather than crawling the partition inherits the previous note's insight — a new
build names new files, so the warm list is correct across a rebuild for free.

Rests on: a device cannot change what it serves without dropping the pipe. That
assumption has one known hole, and it is the same one measured earlier today — `www` can
be replaced on a *running* device, no reboot, no reconnect. Such a change stays invisible
until the device next reconnects, and the manual flush is the answer. The hole is
accepted rather than unnoticed: covering it is exactly what the digest bought, and it was
not worth a permanent seam in the framework.

**Follows:** the device-side digest is reverted; the relay drops a device's entries on
connect and warms them immediately; MUTABLE_TTL is gone. Cache-Control still separates
immutable assets from revalidated ones, because that distinction is the browser's, not
the server's.
