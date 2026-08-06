---
id: 2026-08-06-16h53
date: 2026-08-06
time: "16:53"
title: Bounded reply depth cannot express the replies we already send
builds-on:
supersedes:
---

**Before:** a flat key/value reply looked like the right shape for a format-independent
writer, or at most a root record plus one level of named list. Bounded depth was attractive
for two concrete reasons, not for tidiness: the writer's entire state collapses to a couple
of bools, and a *cap is what makes a non-JSON format renderable at all* — a flat `key=value`
console format can serialise one level of list by repeating a key, but cannot serialise
arbitrary nesting without inventing path syntax.

**What changed it:** enumerating what the handlers actually emit. `help list` nests
categories → commands: an array of objects each containing an array of strings. Five more
replies are a root object carrying a named array of flat records *plus sibling fields* —
`settings list`, `wifi scan`, `partition list`, `log list`, and `help`'s own `arguments`
followed by a trailing `truncated`. Every candidate cap either changed a wire shape or split
a command in two. Separately, the ergonomic complaint that the cap was also answering —
having to name a type to open the root — turned out to be independent of depth: it goes away
by making every scope come from a factory method, root included.

**Now:** reply depth stays open, and the price is explicit rather than forgotten — a second
writer implementation owes the whole grammar, in whatever syntax it has. That price is
unpaid today because JSON is the only reply format. The cap was not a bad idea; what killed
it is that the replies we already send are deeper than any cap that keeps its benefit.

**Follows:** `ReplyObject`/`ReplyArray` may each open either kind of child, as `JsonScope`
did.
