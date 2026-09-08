# Relay in production

Getting the relay from "works on the LAN" to "safe to leave running on a public
server". Worked through step by step — tick a box when it is done and verified on the
real thing, not when the code is written.

Server: `vanbassum.com` (`c:\Workspace\strato-stack`, Traefik + Authentik, one stack per
service, `make up stack=<name>`). The relay left this repo on 2026-09-08 and now lives
in [`vanBassum/strux-relay`](https://github.com/vanBassum/strux-relay), with the history of everything it was here — so
the remaining steps below are worked in that repo, not this one.

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

Steps 1–8 and 11 are done and deleted; the relay is containerised, live behind Traefik
and Authentik, keeps its state in SQLite, and refuses any device that is not approved
and presenting its own token. What they left behind that is worth keeping is in *Facts*
below.

## Security — the actual gates

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
  before the relay can write SQLite there.
- **Deploying only this stack** is `git reset --hard origin/main` then
  `make up stack=strux-relay`. Plain `make deploy` would `up` every stack, including
  whatever else happens to be mid-flight on `main`.
- **The device generates its own token**, the server pins it on approval. No server→device
  message and no network-triggered NVS write; the secret only travels inside TLS. It rides
  as an `X-Strux-Token` upgrade header, so the session protocol did not change.
- **Device settings involved:** `relay.enabled`, `relay.url`, `relay.deviceId`,
  and `relay.token`.
- **TLS costs the relay task under 4 K.** Measured right after the handshake: 6412 of
  10240 bytes still free, so the 10 K is not close to tight. It is not a one-off number
  either — `CheckStackHeadroom()` logs every new low and WARNs under a quarter left.
- **An authenticated reconnect replaces the live pipe.** It proved itself, and refusing it
  would lock a rebooted device out until the dead socket timed out.
- **Two different tokens for one pending id means somebody is guessing**, so the dashboard
  shows both rather than collapsing them.
- **The refusal happens before the upgrade is accepted**, which is what gives both ends a
  reason: the device logs `upgrade refused with HTTP 403`, the server logs `not approved`
  or `token mismatch` into the `events` table the dashboard renders.
- **Blob-sized server work goes through `asyncio.to_thread`** — SQLite is stdlib
  `sqlite3` in WAL mode on the `runtime` volume, and per-connect queries stay tiny.
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
- **A GHCR push denial is two different faults wearing one error.** Both say
  `denied` and neither is explained by the workflow's token log, which reads
  `Packages: write` in every case. Tell them apart by how many runs fail:

  - **A secondary rate limit** — three pushes in quick succession got `denied: denied`
    on `docker login` once and `permission_denied … exceeded a secondary rate limit` on
    push once. Transient. Wait a few minutes and `gh run rerun <id> --failed`; nothing
    needs changing.
  - **A package linked to another repository** — every run fails, forever, with
    `denied: permission_denied: write_package`. GHCR binds a package to the repo that
    first pushed it, and `ghcr.io/vanbassum/strux-relay` was first pushed by CI in
    **Strux** (tag `sha-434b0a9`), so the extracted repo's `GITHUB_TOKEN` had no write
    grant on it. Twelve consecutive runs failed across three hours before this was
    spotted, precisely because the note here said to wait it out.

    The fix is manual and there is no REST endpoint for it: the package's settings page
    → *Manage Actions access* → add the repository with **Write**. After the first
    successful push, `docker/metadata-action` stamps `org.opencontainers.image.source`
    at the new repo and GHCR re-links it, so the old grant can go.

    Do **not** delete the package to force a relink — `latest` is what production pulls.
- **Two IDF installs, and the tools are not where the docs assume.** `C:\esp\v6.0\esp-idf`
  is the framework; the toolchain is an ESP-IDF Installation Manager layout under
  `C:\Espressif\tools`, activated with
  `. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1` (`export.ps1`/`export.sh`
  both fail — they look for a python env that install never created there). The board is
  on **COM8**; `.vscode/settings.json` still says COM10.
