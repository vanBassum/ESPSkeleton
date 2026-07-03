# Webserver login

PIN auth was ripped out entirely on 2026-07-03 (commit `8dd4d27`) — the
frontend never sent a pin, so the per-command checks were dead weight.

When auth returns, it is a **login in the webserver layer** (session at
the transport edge). Authentication checking never leaks into command
handlers again: commands assume the caller is already authenticated.

Related: the relay server (see `remote-access.md`) authenticates the
same way — device credential toward the server, user login at the
server.
