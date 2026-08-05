# Next up

**Active work only.** Rewritten constantly, kept tiny, and an item is *removed* when it
lands or is dropped — never ticked off in place. Everything else lives in
`docs/backlog/` (work for later) or `docs/reasoning/` (why things are the way they are).
If a fact wants to survive, it does not belong in this file.

Last updated 2026-08-05.

## Now

**Putting the relay in production** — live at `https://strux.vanbassum.com`, behind
Traefik and Authentik. Steps 1–3 done; next is step 4, pointing a real device at
`wss://strux.vanbassum.com/device` and watching the relay task's stack high-water mark
now that TLS shares its 10 K. Production-safe after step 9.
→ [`backlog/2026-08-05-relay-in-production.md`](backlog/2026-08-05-relay-in-production.md)

**`/device` is unauthenticated and publicly reachable right now.** Accepted as a test
window. Do not leave a device pointed at the public URL until step 7 lands.
