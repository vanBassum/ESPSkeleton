---
id: 2026-08-11-11h18
date: 2026-08-11
time: "11:18"
title: A mount point must be made structurally irrelevant, not handled at each call site
builds-on:
supersedes:
---

**Before:** we knew the UI is served from two mount points the build cannot know — the
device root (`/`) and the relay's `/devices/<id>/` — and treated that as something each
piece of the frontend handles correctly on its own. `vite.config.ts` chose a relative
asset base for it, with a comment warning that "if client-side routing with real paths
is ever added, a relative base breaks on deep routes." The warning was read as a
constraint on a future feature, not as a statement about a feature already shipped.

**What changed it:** the router already asserted absolute paths. Clicking a menu item
through the relay ran `pushState(null, "", "/settings")`, which discarded
`/devices/<id>/` from the URL. The page kept working — its WebSocket was already open —
so the breakage stayed invisible until F5, which asked the relay for a path it has no
route for. The instructive part is what did *not* break: `resolveWsUrl` had been
serving both mount points correctly all along, and it never mentions a device id. It
derives the socket from the *document's own directory*. Same problem, same file tree,
opposite outcome — and the difference is that one derived its answer from where the
document actually is, while the other asserted where it thought it should be.

**Now:** "works from any mount point" is not a property each call site can be trusted
to preserve, because a call site that hardcodes a path is indistinguishable from a
correct one until the prefix differs. The durable version is to make the mount point
*unable* to matter. Routing in the hash does that: the path never changes, so the
document URL stays the mount point at every route, and every derived thing —
relative asset URLs, `resolveWsUrl`, both servers' SPA fallbacks — keeps resolving
without knowing a relay exists. Base-relative paths were the attractive alternative
(clean URLs, and the relay's existing fallback already serves them), and what broke
them is that they only hold while every route is exactly one segment deep: a
two-segment route moves the document directory and takes the assets and the socket
with it. That is the same class of latent breakage as the original bug, deferred rather
than removed.

Rests on: the hash is never transmitted to a server, so no server-side route or
fallback can be a precondition for a reload working.

**Follows:** `use-route.ts` reads and writes `location.hash`; the `vite.config.ts`
comment now names hash routing as what keeps the relative base safe, instead of warning
about a router we had already written.
