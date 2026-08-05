# Next up

**Active work only.** Rewritten constantly, kept tiny, and an item is *removed* when it
lands or is dropped — never ticked off in place. Everything else lives in
`docs/backlog/` (work for later) or `docs/reasoning/` (why things are the way they are).
If a fact wants to survive, it does not belong in this file.

Last updated 2026-08-05.

## Now

**Putting the relay in production** — live at `https://strux.vanbassum.com`, behind
Traefik and Authentik. Steps 1–8 done: a device must be approved and must present its own
token, or the upgrade is refused with a 403, and pairing is one click in the dashboard.
Step 9 — secrets out of `settings list` — is the last one before the plan calls it
production-ready.
→ [`backlog/2026-08-05-relay-in-production.md`](backlog/2026-08-05-relay-in-production.md)

**An approved relay pipe still hands out the WiFi PSK** via `settings list`. That is
step 9, and it is the reason "production-ready" is not ticked yet.
→ [`backlog/2026-08-05-secret-settings-over-the-wire.md`](backlog/2026-08-05-secret-settings-over-the-wire.md)

**Telemetry works end to end** — a manager records a point, the relay writes it to
InfluxDB, and it queries back tagged by device. No buffering yet: a point taken while
the relay is down is dropped. That and the other open ends are listed in
→ [`backlog/2026-08-05-telemetry.md`](backlog/2026-08-05-telemetry.md)
