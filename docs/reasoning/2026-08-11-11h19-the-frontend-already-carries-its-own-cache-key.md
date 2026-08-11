---
id: 2026-08-11-11h19
date: 2026-08-11
time: "11:19"
title: The frontend already carries its own cache key, so the firmware version was never the right one
builds-on:
supersedes:
---

**Before:** the relay's file cache was planned as keyed on `(deviceId, firmware, path)`.
The firmware version was chosen deliberately, and for a good reason: the UI is served
from the device precisely so it can never mismatch the firmware it talks to, and a
version-keyed cache preserves that guarantee on the server.

**What changed it:** the assumption underneath it is false. `www` is its own partition
and is uploaded independently of the app — demonstrated by replacing a device's entire
UI over the WebSocket while it went on reporting fw `0.0.5`, no reboot involved. A
firmware-keyed cache would have served the old UI for as long as the entry lived, and
the more that key was trusted, the longer that would be.

**Now:** the build already ships the freshness information the cache needs. Vite
content-hashes every asset's *name*, so those bytes can never change meaning — they are
cacheable with no expiry and no version key, and a new build simply asks for different
names. `index.html` is the only mutable file, and it is the file that *names* the hashed
ones, so the freshness of that one 477-byte file is sufficient for the correctness of
everything else. A short TTL on it replaces a version key on everything.

This reframes what a cache key is for here: not "which version of the product is this",
but "can these bytes still mean something different". Content-addressed names answer
that question at the filename level, so the server does not have to ask the device.

A device-supplied digest was considered and set aside. It is strictly more general — it
would cover files the build does *not* content-hash, and would validate the whole cache
in one tiny reply instead of re-pulling `index.html`. What made it not worth it is that
it removes no round trip (asking for a digest and asking for a 316-byte gzipped
`index.html` cost the same one pipe transaction) and it still needs an interval to
decide when to ask. It buys generality, not freshness, so it stays available if an
unhashed file ever matters.

Rests on: vite keeps content-hashing asset names, and `index.html` keeps naming them —
both properties of the bundler's output, not of our code, so a bundler change is what
would invalidate this.

**Follows:** the cache is keyed `(deviceId, path)`, immutable entries never expire,
mutable ones carry a 10 s TTL, and a device's entries are dropped when it reconnects. The
same distinction is passed to the browser as `Cache-Control`, which is the bigger win:
an immutable asset is not requested a second time at all. The backlog's caching item is
removed.
