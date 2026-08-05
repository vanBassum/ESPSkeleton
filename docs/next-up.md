# Next up

Rewritten as work lands — this is the ordered view, not a log. The detail lives in
`docs/backlog/`; this file only says what to do next and what is waiting on a decision.
Nothing here should be the only place a fact is written down.

Last updated 2026-08-05, after the relay transport rewrite and its hardware run.

## In order

The first two are a question away from being ordinary work; the rest can be picked up
as they are.

1. **Put the relay in production.** The question this list used to lead with — does the
   relay ever face a public network — is answered: yes, a Docker container on
   `vanbassum.com` behind Traefik and Authentik. So the credential, TLS and the
   browser-side gate are requirements now, being worked step by step.
   → [`backlog/2026-08-05-relay-in-production.md`](backlog/2026-08-05-relay-in-production.md)

2. **DECISION — are secret settings write-only over the wire, masked, or gated?**
   `settings list` returns `wifi.password` and `web.password` in plaintext, to anyone
   who reaches the socket while no web password is set — which is the default. The
   recommendation is write-only, and the work is small once the shape is chosen.
   → [`backlog/2026-08-05-secret-settings-over-the-wire.md`](backlog/2026-08-05-secret-settings-over-the-wire.md)

3. **Server-side file caching**, keyed on `(deviceId, firmware, path)`. The fix for
   relay page-load latency, now that one-request-in-flight is permanent.
   → [`backlog/2026-07-03-remote-access.md`](backlog/2026-07-03-remote-access.md)

4. **The command workbench page.** Shape decided (a dedicated page, not a console
   input); the introspection it was waiting for now exists, since `help` returns each
   command's declared arguments off the device.
   → [`backlog/2026-07-07-command-execution-ui.md`](backlog/2026-07-07-command-execution-ui.md)

5. **Structural tidy-ups** — small, independent, each a paragraph: `UpdateManager`'s
   name, the `RelayManager` → `WebServerManager` edge, the `help`/`readArgs`
   dependency.
   → [`backlog/2026-08-05-structural-tidy-ups.md`](backlog/2026-08-05-structural-tidy-ups.md)

## Deliberately waiting for a reason to happen

- [`broadcast-redesign`](backlog/2026-07-09-broadcast-redesign.md) — log broadcasts as
  device-initiated sessions, the last non-session path. Deferred by choice, and now
  wanted by two transports rather than one. Carries a live cosmetic bug (the
  disconnect warning storm) with cheap interim fixes listed.
- [`command-worker-task`](backlog/2026-08-03-command-worker-task.md) — pays the
  worst-case handler stack once instead of per transport, and removes the last private
  IDF symbol. Cost, not correctness; weaker now that only httpd wants it.
- [`std-cpp-review`](backlog/2026-07-03-std-cpp-review.md) — what in `lib/` should be
  standard C++ instead.
- [`flash-circular-logging`](backlog/2026-07-06-flash-circular-logging.md) — blocked on
  its own project (`FlashLoggerV2`), and on flash map space we do not have.

## Parked, with the reasoning written down

- **The console request format.** Motivation reduced to hand-typability, which the
  frontend covers; the pull contract keeps the flip cheap whenever it is wanted.
  Unknown-argument enforcement is parked with it, deliberately.
  → `docs/reasoning/2026-08-03-16h30-the-console-format-is-parked.md`
- **Multiplexed channels / concurrency on the device.** Rejected 2026-08-03, not
  deferred. Addressed writes achieve the same coexistence with no new machinery.
  → `docs/reasoning/2026-08-03-12h30-addressing-replaces-concurrency.md`
- **Dynamic buffers in the request path.** All four sites resolved 2026-08-05, three of
  them by removing the need for the buffer rather than sizing it better. The one
  deliberate non-change (`ConsoleManager`'s log ring) is recorded in structural
  tidy-ups so it is not re-litigated.
  → `docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md`
- **MQTT / Home Assistant.** Removed 2026-07-06 and out of scope for the template; see
  CLAUDE.md.

## Not for here

Structural work flows one way to the KC1245 fork (`lib/protocol`, the pull contract,
two-word routes, auth-as-commands, and now the relay transport). Nothing needs
backporting *from* it — every shared commit was walked on 2026-08-03.
