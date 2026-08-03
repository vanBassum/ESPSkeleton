# Command worker task (the part of multiplexed-channels that survived)

Multiplexed channels were **rejected 2026-08-03** — see
`docs/reasoning/` for why, and `2026-08-02-relay-gate-watchdog-upload.md` for what
replaced them (addressed writes, so a long upload is many short sessions). That
deleted `2026-07-03-multiplexed-channels.md`, which had two things bundled into it
that are **not** multiplexing and are still worth doing. They are recorded here so
they did not die with it.

## 1. Handler execution off the transport task

Today every transport task must be sized for the heaviest command handler, because
handlers run on whichever task dispatched them. That is a per-transport tax: the
httpd task and the relay task each carry stack for the worst case, and a third
transport would carry it again.

A worker task plus a work queue in `CommandManager` — entrances hand over
`(entry, in, out)` and return — pays for the worst case **once** instead of N
times. On a device with this much RAM that is a real saving, and it is independent
of concurrency: one worker still runs one command at a time.

Note this was also listed as the prerequisite that made or broke multiplexing.
Multiplexing is gone, but the stack argument stands on its own.

## 2. The private-API wart

`WsSessionLink::RecvChunk` forward-declares and calls `httpd_ws_get_frame_type`,
which lives in ESP-IDF's private `esp_httpd_priv.h`. It is needed to read frames
beyond the first within one handler invocation — the public WS API gives one frame
per invocation, which is not enough to drain a streamed request body on the httpd
task.

The worker task removes the need for it: the httpd task goes back to one frame per
invocation, feeding a queue that the worker drains. So this wart's fix rides on
item 1, not on multiplexing — rejecting multiplexing did not doom it, it only
removed the reason anyone was going to do item 1 soon.

The wart is safe in the meantime: a future IDF dropping the symbol fails as a clean
link error, never as silent breakage.

## Why this is not urgent

Nothing is broken. Both items are cost, not correctness: some wasted stack, and one
private symbol. Do them when a third transport makes the stack tax concrete, or
when the private call actually breaks.
