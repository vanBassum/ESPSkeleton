# Next up

Rewritten as work lands — this is the ordered view, not a log. The detail lives in
`docs/backlog/`; this file only says what to do next and what is waiting on a decision.

Last updated 2026-08-05, after the cheap half of the buffer plan and `help`.

## Waiting on a decision, not on code

1. **The relay's per-frame `malloc`** — now the only dynamic buffer left in the request
   path. One allocate/free per inbound WebSocket frame, hundreds per upload, which is
   the shape that fragments a heap. The obvious fixed-size
   replacement is queue depth 16 × 4 KB ≈ 65 KB against 109 KB free heap, so it trades
   against upload throughput, chunk size, or PSRAM. Changing it without deciding that
   only moves the problem.
   Detail: [`backlog/2026-08-03-no-dynamic-buffers.md`](backlog/2026-08-03-no-dynamic-buffers.md)

2. **The relay gate watchdog** — measure idleness, not session length. Decide together
   with the device's own `RECV_TIMEOUT_MS`, since the two interact.
   Detail: [`backlog/2026-08-02-relay-gate-watchdog-upload.md`](backlog/2026-08-02-relay-gate-watchdog-upload.md)

3. **Device→server credential.** The worst gap: anything that guesses the MAC-derived
   device id does not impersonate the device, it **evicts** it. TLS, server-side login
   and persistence sit behind the same question — does this ever face a public network?

## Security, independent of all the above

4. **`settings list` returns passwords in plaintext.** Both `wifi.password` and
   `web.password`, to anyone who reaches the socket while no web password is set — which
   is the default. The relay widens that past the LAN. Needs a decision on whether
   secret-ish settings are write-only over the wire, masked, or gated.

## Open smaller items

- `ConsoleManager` holds a single broadcast callback, which is what forces the
  `WebServerManager` → `RelayManager` edge. Bas wanted to think about what "central"
  should mean before changing it.
- `RelayManager` includes `WebServerManager.h` to borrow the `Authenticator` — the last
  sideways edge between two transports.
- `UpdateManager` now owns only the `partition` category; the name has drifted from the
  contents.
- `help` makes `ctx.readArgs()` load-bearing: it is what stops a described handler
  before its body. A handler that skips it runs for real under `help` — against streams
  that go nowhere, so nothing reaches the client, but a flash write would still happen.
  Logged as an error when it occurs; enforcing it at compile time would need the
  argument declarations to sit outside the handler, which is exactly what makes them
  impossible to get out of sync today.
- [`backlog/2026-08-03-command-worker-task.md`](backlog/2026-08-03-command-worker-task.md)
  — the stack-tax and private-API halves that survived the multiplexing rejection. Cost,
  not correctness.

## Parked

The console request format. Motivation reduced to hand-typability, which the frontend
covers; the pull contract keeps the flip cheap whenever it is wanted. Unknown-argument
enforcement is parked with it, deliberately.
See `docs/reasoning/2026-08-03-16h30-the-console-format-is-parked.md`.

## Not for here

Today's structural work flows one way to the KC1245 fork (`lib/protocol`, the pull
contract, two-word routes, auth-as-commands). Nothing needs backporting *from* it —
every shared commit was walked on 2026-08-03.
