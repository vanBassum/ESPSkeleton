---
id: 2026-08-03-11h59-4
date: 2026-08-03
time: "11:59"
title: A command belongs to whoever owns its data, not whoever calls it
builds-on:
supersedes:
---

**Before:** the command that serves frontend files exists solely because the relay
server needs them, and nothing else calls it. That made it look like it belonged to
the relay, and its presence in the webserver looked like the relay leaking into a
neighbour.

**What changed it:** tracing what the handler needs. The frontend lives on a flash
partition that the webserver mounts and whose path it owns, and the handler shares
the path-resolution logic with the local HTTP route so the two cannot disagree
about compression or content type. Moving the command to the relay would have
required the relay to take on the mount path and the resolver — creating a brand
new dependency on the webserver, which is precisely the edge being removed
elsewhere.

**Now:** caller and owner are different questions, and ownership follows the data.
A command whose only caller is one transport is still not that transport's command
if the transport would have to import the data layer to host it. The
delete-the-folder test agrees: deleting the relay should leave "give me a frontend
file" working, because that request is not relay-specific — the local frontend
could issue it too.

The inverse case is worth stating: had the handler needed nothing from the
webserver, "only the relay calls it" would have been a good enough reason to move
it.
