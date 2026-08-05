# Broadcasts — redesign as device-initiated sessions (and fix the warning storm)

**Status: idea, 2026-07-09.** Deferred by choice — broadcasts are to be worked out
and designed properly at a later stage, not bolted onto the session-mux migration
piecemeal. This note captures both the concrete bug to fix and the direction.

## Context

Log lines reach the browser via `ConsoleManager`, which hooks
`esp_log_set_vprintf` ([ConsoleManager.cpp](../../main/Application/ConsoleManager/ConsoleManager.cpp)),
accumulates whole lines, and hands each to a broadcast callback on its own task.
The callback is `WebSocketHandler::Broadcast`
([WebSocketHandler.cpp](../../main/Application/WebServerManager/WebSocketHandler.cpp)),
which since session-transport step 1 sends each line as a **binary session chunk on
the reserved session 0** (`session::BROADCAST_SESSION`) to every tracked client.

This is the one path that is *not* yet a real session: session 0 is a magic id, the
device never "opens" it, the client never replies, and it never goes through the
name-dispatch-close sequence every request does (`protocol::RunCommandSession` — what
`SessionMux` became on 2026-08-03). The session-mux design already
flags this as a later unification (design doc, *Concurrency → Frontend impact*:
"Folding broadcasts into device-opened sessions is a later unification; when it
happens they're initiator=device sessions, still exempt from the client command
gate").

## The concrete bug: broadcast warning storm

When a WS client's socket dies without a clean close (abrupt disconnect, a killed
client, a reboot), the `fd` lingers in the connection registry until a broadcast send fails and
evicts it. Each queued log line tries to send to the dead socket and fails with
`ENOTCONN` (errno 128), and httpd logs its own warnings:

```
W httpd_ws: httpd_ws_send_frame_async: Failed to send WS header
W httpd_txrx: httpd_sock_err: error in send : 128
```

**Those warning lines are themselves log output** → `ConsoleManager` queues them for
broadcast → the broadcast fails again on the same dead socket → two more warnings →
… a self-amplifying storm for ~200 ms until the client is finally evicted
(`WS client removed`). It self-heals and is cosmetic, but it is noisy and wasteful,
and it is trivially triggered (any unclean disconnect). Observed 2026-07-09 during
step-2 testing with raw socket clients.

Contributing details:
- `Broadcast` (log lines) evicts a client on the **first** send failure, but the
  eviction races the already-queued backlog of lines, so several still attempt the
  dead fd before the slot is zeroed.
- `BroadcastBinary` uses a consecutive-failure counter (`MAX_BIN_FAILS = 10`) — a
  different policy for the same "is this client dead?" question. The two should agree.
- The amplifier is the log→broadcast→log feedback: httpd's transport warnings must
  not be able to generate more broadcasts that fail the same way.

## What changed since this was written (2026-08-05)

There is now a **second** consumer of log lines: the relay pipe broadcasts them as
session-0 chunks too, so a relayed frontend gets live logs with no protocol
difference. `ConsoleManager` still holds a *single* broadcast callback, so the fan-out
to two transports is done by `WebServerManager` reaching into `RelayManager` — which
is the sideways edge noted in
[structural-tidy-ups](2026-08-05-structural-tidy-ups.md), and the clearest argument
yet that this redesign is the right shape: with broadcasts as device-initiated
sessions, each transport subscribes and nobody reaches into anybody.

The bug below is unchanged and still live: `Broadcast` evicts on first failure,
`BroadcastBinary` counts to `MAX_BIN_FAILS = 10`, and the log→broadcast→log
amplification is still there.

## Direction (to be designed later)

Fold broadcasts into the session model as **device-initiated sessions** (initiator =
device): the device `OpenSession()`s a push channel and writes chunks to it, the
same `Session`/`SessionLink` path as everything else, so session 0's special-casing
disappears from both the wire and the code. Broadcast push stays exempt from the
client command busy-gate (it's device-initiated, not a client-opened command
session). This is the north-star "everything is a session" applied to the last
non-session path.

Open questions for that design pass:
- Fan-out to N clients vs. a per-client session — where does the client list live
  once broadcasts are sessions?
- Dead-client detection unified in one place (one policy, not `Broadcast` vs.
  `BroadcastBinary`), and decoupled from the log feedback loop (e.g. suppress
  re-broadcasting httpd's own transport-error warnings, or mark the client dead
  before the retry that would log them).
- Backpressure / drop policy for a slow client (logs must not block the device).
- Relationship to the worker task from
  [command-worker-task](2026-08-03-command-worker-task.md).

## Cheap interim (optional, if the storm is annoying before the redesign)

Not the real fix, but each would break the amplification without the full redesign:
- Evict the client from the connection registry *before* the send that would fail-and-log, or on
  the first `ENOTCONN`, so the queued backlog finds no target.
- Don't feed httpd's own `httpd_ws`/`httpd_txrx` `W` lines back into the broadcast
  (filter by tag in `ConsoleManager`), removing the feedback entirely.
- Unify the dead-client policy between `Broadcast` and `BroadcastBinary`.

## Related

- Design: [session-mux transport](../superpowers/specs/2026-07-09-session-mux-transport-design.md)
- Roadmap: [session-transport-roadmap](../session-transport-roadmap.md)
