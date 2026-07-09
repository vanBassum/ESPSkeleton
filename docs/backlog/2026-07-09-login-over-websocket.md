# Login over the WebSocket (retire /api/login, per-connection auth)

**Status: idea, 2026-07-09.** One of the endpoint-decommission issues in the
"everything over one WS" direction. Redesigns `webserver-login`
(spec `docs/superpowers/specs/2026-07-03-webserver-login-design.md`).

## Why

Today auth is HTTP: `POST /api/login` mints a RAM token into a `SessionTable`,
`GET /api/login` returns the device name for the login page, the WS carries
`?token=`, and `/api/command` carries `Bearer`. In the everything-over-one-WS
model there is no HTTP command surface to Bearer-guard and no URL to hang a
token on — login has to move onto the socket.

## How

- Open the WS **unauthenticated**. A short pre-auth window accepts only a small
  allow-list of commands.
- `login{password}` and `hello` (device name) become **commands** registered by
  the web transport, marked pre-auth-allowed.
- A successful `login` **promotes the socket** to authenticated for its
  lifetime; socket close ends the session.
- Auth becomes **per-connection state**, not a token table: drop `SessionTable`,
  tokens-in-URL, per-frame token validation, and the session-timeout sweeper.

## Trade-offs / decisions

- **Gate vs mechanism.** "Needs auth" is a property of the *transport/connection*
  (how the command arrived), not of the command — the same command is trusted on
  a local serial console and gated on a remote WS. So the *decision to gate*
  stays transport-owned; `login` is only the shared password-check *mechanism*.
  A per-command `requiresAuth` flag was considered and rejected for this reason.
- **Pre-auth surface.** Exactly which commands run before login (`login`,
  `hello`), and the window length before the device drops an un-authed socket.
- **Device name before auth.** `hello` pre-auth command vs. baking the name into
  the static bootstrap the server already serves.
- **TLS.** The password now rides in-band, so the tunnel must be `wss` (no
  regression — token-in-URL has the same exposure today; prefer a relay that
  stays a dumb pipe, end-to-end to the device, over one that terminates TLS).
- Physically-trusted transports (local serial console) mark connections
  authenticated from birth — no login there.

## Depends on

Nothing in the streaming/multiplexing track — login is small and needs neither
reply-streaming nor multiplexing, just the existing command dispatch plus the
per-connection auth model. Redesigns `webserver-login`. See also
`2026-07-03-remote-access.md` (the relay needs the server's own auth — open fork
on whether it reuses this WS model).

## Done when

`GET`+`POST /api/login` are gone, the browser authenticates purely over the WS,
and the token table / `?token=` / `Bearer` machinery is deleted.
