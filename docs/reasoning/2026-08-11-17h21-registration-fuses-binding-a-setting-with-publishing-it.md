---
id: 2026-08-11-17h21
date: 2026-08-11
time: "17:21"
title: Registration fuses binding a setting to storage with publishing it, which is why secrecy has nowhere clean to live
builds-on:
supersedes:
---

**Before:** hiding a secret setting from `settings list` was framed as a question about
the *setting*: give `Setting` a `secret` flag (or a flags word) and branch in the
serializer. The objection to it was aesthetic — a flag in the settings vocabulary
obscures a small, sharp lib, and a bare `"set": true` on the wire says nothing about what
it means.

**What changed it:** looking at what `Register()` actually does. It does two unrelated
jobs in one call. It **binds** the leaf — it is what sets `mgr`, and an unregistered
setting FATALs on its first `Get()` — and it **publishes** it into the chain that
`settings list` walks and the UI renders. There is no way to store a value through this
lib without also showing it.

**Now:** the flag was never the real question. A flag is only needed *because* membership
of the chain means two things at once; split binding from publishing and the whole
question dissolves — a credential is bound but not published, `Get()` works in firmware,
and `settings list` cannot leak it rather than declining to. That is also why the flag
felt like it obscured the lib: it adds vocabulary to work around a conflation instead of
naming it.

What the choice actually costs is one thing, and it is not about secrecy at all:
**generated UI or structural safety.** A browser can only render an editor for a
credential the device admits exists, and anything that admits it exists is a flag under
another name. Keeping the chain's invariant — everything in it is public — means the UI
must hand-roll those editors. The failure modes differ in the way this project usually
cares about: a fork that adds `mqtt.password` and forgets to mark it secret leaks
silently, while a fork that binds it gets no UI until it writes one, which is a visible
gap.

Rests on the credential population staying tiny (three today), and on `relay.token` being
generated state rather than a user-editable setting — it needs storage and nothing else,
so bind-without-publish fits it exactly.

Also settled in passing: if a secret is ever announced on the wire, the field to send is
not `"set": true`. Omit `value` entirely and say `"secret": true` — absence is the
message and the reason is named. A key called `set` sitting beside one called `value`
invites misreading.
