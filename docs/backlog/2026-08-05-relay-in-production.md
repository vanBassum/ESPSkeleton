# Relay in production

Getting the relay from "works on the LAN" to "safe to leave running on a public
server". Worked through step by step — tick a box when it is done and verified on the
real thing, not when the code is written.

Server: `vanbassum.com` (`c:\Workspace\strato-stack`, Traefik + Authentik, one stack per
service, `make up stack=<name>`). The relay stays in this repo under `relay-server/` for
now; nothing in it imports from the firmware, so moving it to its own repo later is a
copy, not a refactor.

Related: [what the relay still owes](2026-07-03-remote-access.md) ·
[why the transport reads its own socket](../reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md)

## Decisions still needed

- [ ] **Domain** — `relay.` / `strux.` / `devices.`vanbassum.com, and is there wildcard
      DNS or does it need a record?
- [ ] **GHCR package** — public (no creds on the server) or private (needs a
      `read:packages` PAT and a `docker login` on the host)?
- [ ] **Who may approve a device** — `authentik Admins` only, or any logged-in user?

Settled: SQLite for state. `relay.deviceId` becomes something random. `web.password`
stays empty — Authentik guards remote access; a password only matters on the LAN.

## Plumbing

- [ ] **1. `relay-server/Dockerfile` + `/healthz`** → the relay runs in a container.
      `python:3.13-slim`, non-root, `requirements.txt`. `/healthz` exists so the
      healthcheck has a target (slim has no curl).
- [ ] **2. `.github/workflows/relay-image.yml`** → `ghcr.io/vanbassum/strux-relay:latest`
      and `:sha-<short>` on every push touching `relay-server/**`, plus manual dispatch.
      `permissions: packages: write`, `GITHUB_TOKEN`, amd64.
- [ ] **3. `strato-stack/stacks/strux-relay/strux-relay-compose.yml`** + DNS + `.env`
      → live at the domain, browser side behind Authentik.
- [ ] **4. Point the device at `wss://<domain>/device`** → TLS proven on hardware.
      Check `uxTaskGetStackHighWaterMark` on the relay task while connected: the TLS
      handshake now shares its 10 K with the command handlers. **Then put the device back
      on the LAN URL until step 7.**

> Step 3 goes up with `/device` unauthenticated — Bas's call, accepted as a test
> window. While it is open, anyone can register a device, and a device the relay
> serves gets to run its own HTML on the relay's origin inside an authenticated
> session. So: test, then take the device off the public URL, and do not leave a
> device pointed at it until step 7 closes the hole.

## Security — the actual gates

- [ ] **5. SQLite on the `runtime` volume** → state survives a restart. stdlib
      `sqlite3`, WAL. Tables: approved devices, and a refusals/approvals log for step 11.
      Keep per-connect queries tiny; blob-sized work (the future file cache) goes through
      `asyncio.to_thread`.
- [ ] **6. Device `relay.token`** → the device can prove who it is. New `StringSetting`,
      empty by default; on `Init`, if empty, generate 32 hex from `esp_fill_random` and
      store it. Sent as an upgrade header, so the session protocol does not change.
- [ ] **7. Server refuses unknown id or wrong token with HTTP 403** → strangers cannot
      register and nobody can take a device's slot. **This is the step that makes public
      exposure safe.** Refuse *before* accepting the upgrade: the device already logs
      `upgrade refused with HTTP 403`, so there is a clear reason on both ends. An
      authenticated reconnect still replaces the old pipe — it proved itself, and
      refusing it would lock a rebooted device out until the dead socket times out.
- [ ] **8. Dashboard: pending list, approve, forget** → pair a new device in one click,
      and recover a board whose NVS was wiped. Pending records hold the token that was
      presented; two different tokens for one id means somebody is guessing, so show
      both rather than collapsing them.
- [ ] **9. Secrets out of `settings list`** → an approved pipe stops handing out the
      WiFi PSK. Write-only is the recommended shape; detail in
      [secret-settings-over-the-wire](2026-08-05-secret-settings-over-the-wire.md).

**Production-ready after 9.**

## Hardening

- [ ] **10. Rate-limit `/device`, cap the pending list** → no brute-forcing ids, no junk
      accumulating.
- [ ] **11. Log refusals and approvals** → the attack signal is visible when it fires.

## Later, only if this ever hosts hardware Bas did not install

- [ ] **12. Origin isolation per device** — subdomain each, or a sandboxed iframe with a
      strict CSP. Today every device's UI is served from one origin, so all devices are
      same-origin with each other and with the dashboard. Pairing reduces that to
      "devices you approved", which is enough for a single operator.

## Facts worth not re-deriving

- **Traefik routers** — three on one host, and `Path`, *not* `PathPrefix`, on the device
  one: `/devices/<id>/ws` starts with `/device`, so a prefix rule would quietly put the
  browser socket on the unauthenticated route.

  | router | rule | middleware |
  |---|---|---|
  | `strux-relay-device` | `Host(…) && Path(/device)` | none |
  | `strux-relay-outpost` | `Host(…) && PathPrefix(/outpost.goauthentik.io/)` | → `authentik@docker` |
  | `strux-relay` | `Host(…)` | `authentik@docker` |

- **`/device` can never sit behind Authentik.** Forward-auth answers an unauthenticated
  request with a redirect to the login flow; the device would log
  `upgrade refused with HTTP 302` forever. The proxy protects the human side only.
- **House stack conventions** — `stacks/<name>/<name>-compose.yml`, `expose` rather than
  `ports`, external `ingress-network`, per-stack state in `./runtime` (what
  `make clean-runtime` looks for), `homepage.*` labels, domain from `${X_DOMAIN}` in the
  tracked `.env`.
- **The device generates its own token**, the server pins it on approval. No server→device
  message and no network-triggered NVS write; the secret only travels inside TLS.
- **Device settings involved:** `relay.enabled`, `relay.url`, `relay.deviceId`,
  and `relay.token` (new in step 6).
