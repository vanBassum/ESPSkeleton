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

**Designed 2026-08-02 — see `docs/superpowers/specs/2026-08-02-remote-access-relay-design.md`.**
The relay is a second `SessionLink` under the existing `SessionMux`;
`RelayManager` only dials out, queues frames, and implements
SendRaw/RecvChunk. Nothing above the transport changes.

Open fork **resolved: `httpGet`-flavoured** — the device answers status +
content-type + content-encoding + body. Decider: frontend files are stored
gzipped (`*.gz`), so a raw-bytes `readFile` forces the server to re-derive
`Content-Encoding` and MIME in C#, where they drift from the device.
`getWebFile` is an ordinary command on `WebServerManager`, reusing an
extracted `StaticFileHandler::Resolve()` — the dogfooding below, achieved
without routing file reads through the command layer.

Guardrail held, with one refinement: `Resolve()` deliberately does *not* do
SPA fallback. That stays in each route layer (device HTTP handler, and the
proxy), so a mistyped asset path 404s instead of returning index.html/200
and tripping a browser MIME error. The device still owns where files live
and how paths map to them.

Correction to the "a relay forwards one socket, understanding nothing" line
in the session-mux spec: the server is payload-opaque, not frame-opaque. It
must read and rewrite the 3-byte session header, because it **owns the id
space** on the device pipe — otherwise its own `getWebFile` sessions collide
with browser-allocated ids (both start at 1). It must also forward session 0
(log broadcasts), so the relay is not request/response.

Prerequisites, all now on main: stream-commands rework (DONE 2026-07-03,
spec `2026-07-03-stream-commands-design.md`); the session transport this
rides on (DONE 2026-07-09, spec `2026-07-09-session-mux-transport-design.md`
— it supersedes the old "envelope request ids" point: every chunk carries a
session id); device login over the socket (DONE, `AuthGate` — so a relayed
frontend gets device login for free, handlers oblivious). Remaining auth
work is the *device credential to the server*; until it exists, milestone 1
stays off public IPs.

Not a prerequisite but the thing that makes it pleasant:
`2026-07-03-multiplexed-channels.md` (the concurrency half — worker task +
slot table). Without it the server must keep one request in flight per
device, so an uncached page load is N sequential WAN round trips.
