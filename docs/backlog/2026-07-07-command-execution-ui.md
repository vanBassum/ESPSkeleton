# Command execution from the web UI

Idea (Bas, 2026-07-07): let the web UI execute CommandManager commands
interactively.

**Decided 2026-07-07: shape B — a dedicated workbench page.** No log
split-view (not needed); the console never grows an input. Design
still being brainstormed — no plan yet. Original options for the
record:

**A — input line on the Console page.** Terminal/REPL feel. Problem:
the console is the device's stdout stream; command replies are
per-client RPC responses, not device output. Interleaving fabricates
lines that aren't in the log (and multi-line JSON replies are noisy in
a line-oriented view).

**B — dedicated command page (workbench).** Postman-style: command
name + JSON params editor, send, and a history list showing each sent
command with its reply (pretty-printed) and errors. Matches the
architecture: commands ARE the RPC surface; the reply belongs next to
the request, not in stdout.

Enabler worth doing with either: CommandManager has no introspection —
a `listCommands` command (walk the registration chain, return names)
would give the UI autocomplete/discovery.

**That enabler shipped 2026-08-05 and went further than this note asked.** `help list`
walks the registry for categories and command names, *and* `help list -category X
-command Y` returns that command's declared arguments — name, type, required, max
length — by re-dispatching the handler with a reader that prints its declarations
instead of filling them. So the "per-command schemas don't exist" objection to the
Scalar/Swagger shape below is no longer true: they do exist, they are generated from
the handler itself, and they cannot go stale. Step 3 of the sketch is now the cheapest
part of this feature rather than the wildest.

## Direction sketch (Bas, 2026-07-07 — thoughts incomplete, parked)

Start minimal and grow:

1. **v1 — one big box** showing what happens: sent commands and their
   replies. Nothing else. **All** commands executable — including the
   binary/transfer ones (firmware download etc.), not just the JSON
   dialect. Open question that raises: do we need protection against
   huge replies flooding/crashing the page (truncate/preview
   megabyte-scale payloads)? Undecided.
2. **Then** `listCommands` introspection (walk CommandManager's chain,
   names only).
3. **Maybe later — Scalar/Swagger-like**: a card per command with
   editable fields instead of raw JSON. Requires per-command schemas,
   which don't exist (handlers parse payloads ad hoc) — "too wild for
   now".

Parked here until the thinking completes; no plan yet.
