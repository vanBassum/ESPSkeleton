# Remote access / relay server

Devices connect **outbound** to a server (NAT-friendly) so they are
reachable remotely. The relay is just another dumb-pipe transport into
CommandManager: every command — including firmware update — works
remotely for free. This is the driving reason CommandManager is the
star point of the architecture.

**Vision (2026-07-03):** log in to a server page, see connected devices,
click one → the device's *own* web UI opens, served **through the
command pipe**. Chosen over WireGuard (network-level fix for an
app-level problem, heavy key/routing ops) and over server-hosted
frontend copies (version skew: device-served UI can never mismatch its
firmware). The server caches device files keyed on firmware version, so
the slow fetch-through-the-pipe happens once per version, not per view.

Open fork: `readFile` command (raw FAT bytes; server owns content-type /
gzip / SPA-fallback) vs `httpGet` command (device answers status +
headers + body; server is a pure proxy). `httpGet` keeps HTTP behavior
in one place — effectively HTTP-over-commands, an app-level VPN.

Possible dogfooding: the device's own StaticFileHandler fetches bytes
via the same command, keeping one file-access path. Guardrail: HTTP
decisions stay in the route layer; the command stays "give me bytes".

Prerequisites: stream-commands rework (see `stream-commands.md`);
envelope request ids (WS protocol already has them); auth = device
credential to server + user login at server (see `webserver-login.md`)
— handlers stay oblivious.
