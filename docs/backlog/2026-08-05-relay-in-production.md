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

## Decisions

All settled. SQLite for state. `web.password` stays empty — Authentik guards remote
access; a password only matters on the LAN. Domain is `strux.vanbassum.com`, and
`*.vanbassum.com` already resolves to the host, so no DNS record was needed. The GHCR
package is **public** — the source is in a public repo already, so a private image would
only add a PAT to keep alive on the server.

**Who may approve a device**: whoever can load the dashboard, and the blueprint binds
that to `authentik Admins`. The pairing API carries no check of its own, because a
second one would only be able to disagree with the proxy.

**`relay.deviceId` stays MAC-derived** — the plan used to say "becomes something
random", which was right while the id was the only identity. Once the token proves the
device, the id carries no secrecy, and MAC-derived buys something random cannot: the MAC
is in efuse, so a wiped board comes back as the *same* id with a fresh token and pairing
is one re-approve. A random id in NVS would return as a stranger and orphan its old
approval. The id is technical; `device.name` is what a human reads.

## Plumbing

- [x] **1. `relay-server/Dockerfile` + `/healthz`** → the relay runs in a container.
      `python:3.13-slim`, non-root, `requirements.txt`. `/healthz` exists so the
      healthcheck has a target (slim has no curl).
- [x] **2. `.github/workflows/relay-image.yml`** → `ghcr.io/vanbassum/strux-relay:latest`
      and `:sha-<short>` on every push touching `relay-server/**`, plus manual dispatch.
      `permissions: packages: write`, `GITHUB_TOKEN`, amd64.
- [x] **3. `strato-stack/stacks/strux-relay/strux-relay-compose.yml`** + DNS + `.env`
      → live at `https://strux.vanbassum.com`, browser side behind Authentik. Verified
      from outside: dashboard 302s to the login flow, `/device` reaches the relay
      unauthenticated, `/devices/<id>/ws` still 302s (so the `Path`-not-`PathPrefix`
      rule does what it was written for), and a WS client gets a 101 over TLS.
- [x] **4. Point the device at `wss://<domain>/device`** → TLS proven on hardware. The
      relay task's stack came out at **6412 of 10240 bytes free** immediately after the
      handshake — so TLS costs it under 4 K and the 10 K is not close to tight. The
      number is no longer a one-off measurement: `CheckStackHeadroom()` logs every new
      low, and WARNs under a quarter left.

## Security — the actual gates

- [x] **5. SQLite on the `runtime` volume** → state survives a restart. stdlib
      `sqlite3`, WAL. Tables: `approved`, `pending`, and an `events` log for step 11.
      Keep per-connect queries tiny; blob-sized work (the future file cache) goes through
      `asyncio.to_thread`.
- [x] **6. Device `relay.token`** → the device can prove who it is. New `StringSetting`,
      empty by default; on `Init`, if empty, generate 32 hex from `esp_fill_random` and
      store it. Sent as an `X-Strux-Token` upgrade header, so the session protocol does
      not change.
- [x] **7. Server refuses unknown id or wrong token with HTTP 403** → strangers cannot
      register and nobody can take a device's slot. **This is the step that makes public
      exposure safe.** Refuse *before* accepting the upgrade: the device already logs
      `upgrade refused with HTTP 403`, so there is a clear reason on both ends. An
      authenticated reconnect still replaces the old pipe — it proved itself, and
      refusing it would lock a rebooted device out until the dead socket times out.
- [x] **8. Dashboard: pending list, approve, forget** → pair a new device in one click,
      and recover a board whose NVS was wiped. Pending records hold the token that was
      presented; two different tokens for one id means somebody is guessing, so show
      both rather than collapsing them. Built with 7 rather than after it: without an
      approve button, 7 locks every device out until somebody edits SQLite by hand.
- [ ] **9. Secrets out of `settings list`** → an approved pipe stops handing out the
      WiFi PSK. Write-only is the recommended shape; detail in
      [secret-settings-over-the-wire](2026-08-05-secret-settings-over-the-wire.md).

**Production-ready after 9.**

## Hardening

- [ ] **10. Rate-limit `/device`** → no brute-forcing ids. The *cap* half of this landed
      early with step 7: a public endpoint that INSERTs is a disk-filling machine, so
      `pending` stops at `MAX_PENDING` (50). Past the cap a known pair still counts its
      attempts, so the signal keeps rising while no new row is created. What is left is
      the rate limit itself.
- [x] **11. Log refusals and approvals** → the attack signal is visible when it fires.
      Came along with 5 and 8 rather than as its own step: refusals carry a reason
      (`not approved` / `token mismatch`), approvals and forgets are recorded, and the
      dashboard renders the last 20. Verified in production — the eight events from the
      device's first pairing are in there.

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
- **Authentik apps are declarative, and applying one costs no downtime.** No UI clicking:
  the proxy provider, application and group binding are entries in
  `stacks/authentik/blueprints/20-forward-auth.yaml`, and
  `docker exec authentik-worker ak apply_blueprint /blueprints/custom/20-forward-auth.yaml`
  applies them live — the outpost gets the new provider pushed to it, nothing restarts.
  The one trap is in that file already: the outpost entry's `providers:` list *replaces*
  what the outpost has, so a new proxy app must be added there as well as declared.
- **`lablr` is the stack to copy from, not `dozzle`** — it has the same shape as this one,
  an Authentik-gated UI beside an endpoint (`/agent`) that headless hardware reaches
  unauthenticated because it cannot follow a login redirect.
- **The `./runtime/data` bind mount is owned by root when Docker creates it**, and the
  image runs as uid 10001. It is `chown`ed on the server; a fresh host needs that again
  before step 5 writes SQLite there.
- **Deploying only this stack** is `git reset --hard origin/main` then
  `make up stack=strux-relay`. Plain `make deploy` would `up` every stack, including
  whatever else happens to be mid-flight on `main`.
- **The device generates its own token**, the server pins it on approval. No server→device
  message and no network-triggered NVS write; the secret only travels inside TLS.
- **Device settings involved:** `relay.enabled`, `relay.url`, `relay.deviceId`,
  and `relay.token` (new in step 6).
- **Approval needs no push.** The device is already retrying every 5 s, so the reconnect
  after an approve is the one that succeeds — that *is* the handshake. Which also means an
  unapproved device logs `upgrade refused with HTTP 403` twelve times a minute until
  somebody deals with it. Noisy on purpose.
- **`forget` drops the live pipe too.** The token is only checked when a connection is
  made, so revoking without closing the socket would leave a forgotten device connected
  until it happened to reconnect.
- **The device name is attacker-controlled before approval.** An unapproved stranger's
  `name` lands in the pending list, which renders on an admin page — so the dashboard
  escapes it. Escaping is not decoration here.
- **GHCR has a secondary push rate limit, and it presents as an auth error.** Three
  pushes in quick succession got `denied: denied` on `docker login` once and
  `permission_denied … exceeded a secondary rate limit` on push once. Neither is a
  permissions problem — the workflow's token log says `Packages: write` both times. Wait
  a few minutes and `gh run rerun <id> --failed`; nothing needs changing.
- **Two IDF installs, and the tools are not where the docs assume.** `C:\esp\v6.0\esp-idf`
  is the framework; the toolchain is an ESP-IDF Installation Manager layout under
  `C:\Espressif\tools`, activated with
  `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1` (`export.ps1`/`export.sh`
  both fail — they look for a python env that install never created there). The board is
  on **COM8**; `.vscode/settings.json` still says COM10.
