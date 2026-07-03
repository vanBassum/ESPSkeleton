# Webserver login — non-blocking follow-ups

From the final whole-branch review of the login feature (shipped 2026-07-03,
`6c14578..196d487`). Neither blocks anything; both are small.

## Untracked 5th WS client

When all `MAX_WS_CLIENTS` slots are full, `AddWsClient` logs "rejected
(max reached)" but `HandleWs` still returns `ESP_OK` — the connection
stays open, trusted, and untracked: no broadcasts reach it and
`TouchClient` never refreshes its session, so its (open!) tab gets
GC'd after 30 minutes, violating "an open tab never logs out" for that
client. Honest behavior is a ~3-line fix: `AddWsClient` returns `bool`;
`HandleWs` returns `ESP_FAIL` when full so the client falls into the
normal reconnect loop instead of a half-alive state.

## `HandleLoginGet` body[80] can truncate

A 32-char device name that needs JSON escaping can exceed the 80-byte
response buffer. `BufferStream` truncates safely and the frontend falls
back to "Strux", so purely cosmetic — bump to 128 when touching the file.
