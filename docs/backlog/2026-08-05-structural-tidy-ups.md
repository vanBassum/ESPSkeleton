# Structural tidy-ups

Small things that are each a paragraph, not a project. Collected here so they stop
living in `next-up.md`, which is supposed to be an ordered index rather than a list of
loose ends. None of them is a bug; all of them are places where a name or an edge no
longer matches what the code does.

## `UpdateManager` owns only the `partition` category

Its entire command table is `partition status|list|write|clear|activate|read`, and its
whole external surface is that table. The name is left over from when it owned
`updateBegin`/`updateWrite`/`updateEnd` and a pull-OTA-from-URL path. `PartitionManager`
would say what it is. A rename touches `ServiceProvider`, `ApplicationContext`,
`main.cpp` and `main/CMakeLists.txt` — mechanical, but it lands in every downstream fork,
so it wants doing deliberately rather than in passing.

## `RelayManager` includes `WebServerManager.h` to borrow the `Authenticator`

The last sideways edge between two transports, and the reason `main.cpp`'s init order
has a comment about `Relay` coming last. The `Authenticator` is the credential
authority for the *device*, not for the web server — it lives under
`WebServerManager/` because that is where it was extracted, not because it belongs
there. Moving it (its own small manager, or into `SystemManager`) removes the edge and
the ordering constraint together.

## `ConsoleManager` holds a single broadcast callback

Which is what forces `WebServerManager` to reach into `RelayManager`: one callback,
two transports that want log lines, so somebody has to fan out. Bas wanted to think
about what "central" should mean before changing this, and it is entangled with
[broadcast-redesign](2026-07-09-broadcast-redesign.md) — broadcasts becoming
device-initiated sessions is the version of this that solves it properly rather than
turning one callback into a list of two.

## `help` makes `ctx.readArgs()` load-bearing

Describe mode works by re-dispatching a command with a reader that prints the
declarations instead of filling them and returns a sentinel, which stops the handler
at its own `RETURN_IF_ERROR`. So a handler that never calls `readArgs` has no `help`
**and runs its body when described** — against streams that go nowhere, so nothing
reaches the client, but a flash write would still happen. It is logged as an error
when it occurs, and every handler today does the right thing.

Enforcing it at compile time would mean moving the argument declarations outside the
handler, which is exactly what makes them impossible to get out of sync today. So the
trade is deliberate; this note exists so the next person to add a command knows the
call is not optional.

## Decided *not* to do: the `ConsoleManager` log ring

Recorded here because it was the fourth site in the no-dynamic-buffers plan and the
answer was "leave it alone", which is easy to forget and re-litigate. The ring is one
allocation at `Init`, sized from constants, never freed, and it prefers PSRAM when the
board has it. It is effectively static already. Turning it into a plain array to
satisfy a literal reading of "no heap" would cost the PSRAM preference and buy
nothing.
