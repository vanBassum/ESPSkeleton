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

Open: which shape (or B now, A as sugar later); whether the workbench
also shows the live log alongside (split view) since command effects
often appear in the log.
