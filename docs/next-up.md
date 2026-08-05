# Next up

Rewritten as work lands — this is the ordered view, not a log. The detail lives in
`docs/backlog/`; this file only says what to do next and what is waiting on a decision.

Last updated 2026-08-05, after the buffer plan, `help`, and the relay reading its own
socket.

## Verified on hardware 2026-08-05

Nothing to do here — kept because it says what has actually been exercised, on an
esp32_devkit against the demo relay server. Everything below the horizontal rule has not.

- The relay reading its own socket: connect, reconnect after failed attempts, a full
  one-shot OTA (1.12 MB, 199 KB/s unpaced), `activate` validating the image, boot into
  the new slot, and 100 s idle without losing the pipe (which also proves the pongs the
  transport owes the server's 30 s heartbeat).
- The gate watchdog measuring silence: a 40 s paced upload with a page load cutting in,
  where both requests used to die.
- `help`, including describe-mode re-dispatch — `help list -category partition -command
  write` returns that handler's declared arguments off the device.
- The zero-copy `partition write`: an image written straight out of the transport's
  inbound buffer passes `esp_ota_set_boot_partition`'s validation and boots.

Detail: [`backlog/2026-08-02-relay-gate-watchdog-upload.md`](backlog/2026-08-02-relay-gate-watchdog-upload.md),
[`reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md`](reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md)

Not exercised: `wss://` (the TLS handshake now runs on the relay task's stack, sized at
10 K by reasoning rather than measurement), and a firmware push large enough to need a
partition whose erase outlasts the server's 15 s idle window.

---

## Waiting on a decision, not on code

1. **Device→server credential.** The worst gap: anything that guesses the MAC-derived
   device id does not impersonate the device, it **evicts** it. TLS, server-side login
   and persistence sit behind the same question — does this ever face a public network?

## Security, independent of all the above

2. **`settings list` returns passwords in plaintext.** Both `wifi.password` and
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
