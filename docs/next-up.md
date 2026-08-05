# Next up

Rewritten as work lands — this is the ordered view, not a log. The detail lives in
`docs/backlog/`; this file only says what to do next and what is waiting on a decision.

Last updated 2026-08-05, after the buffer plan, `help`, and the relay reading its own
socket.

## Needs hardware, not thought

1. **Flash and exercise the relay rewrite.** The relay now drives its WebSocket at the
   transport layer and reads on the task that runs the command, so the queue, the
   per-frame `malloc` and the dropped-chunk failure mode are gone (and with them the
   RAM trade this list used to lead with). It builds; nothing has run on a device.
   Push a firmware update over the relay, watch a reconnect, and confirm the keepalive
   holds an idle pipe up.
   Detail: [`reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md`](reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md)

## Waiting on a decision, not on code

2. **The relay gate watchdog** — measure idleness, not session length. Decide together
   with the device's own `RECV_TIMEOUT_MS`, since the two interact. The device half now
   has an answer (ping after 30 idle seconds, reconnect when one will not send), so
   what is left is the server's gate.
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
