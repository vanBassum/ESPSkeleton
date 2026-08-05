---
id: 2026-08-05-22h28
date: 2026-08-05
time: "22:28"
title: A refusal is readable on the path that reports failure, not the one that reports success
builds-on: 2026-08-05-15h29-3
supersedes:
---

**Before:** the device asked `esp_transport_ws_get_upgrade_request_status()` only after
`esp_transport_connect()` returned success, and logged `connect to <host>:<port> failed`
otherwise. That reads as the obvious arrangement — a status belongs to a response, and a
failed connect has no response — and it is what put `upgrade refused with HTTP %d` in the
code at all. The relay server was written believing it worked: a comment in `device_ws`
says the device "already logs 'upgrade refused with HTTP 403'", which is why refusing
before the upgrade was chosen over accepting a socket and dropping it.

It never fired. Every refusal came out as a failed connect.

**What changed it:** Bas pasted a console and asked whether it was excessive. It was
three lines every five seconds, and one of them was wrong: TLS had plainly succeeded on
the line above (`Certificate validated`), so "connect failed" could not be the whole
story. In `transport_ws.c`, `ws_connect` parses the response's status line into
`ws->http_status_code` and *only then* looks for `Sec-WebSocket-Accept`. A 403 carries no
such header, so the function returns -1 — having already stored the number that explains
exactly why. The status is not lost on the failure path. It is *produced* on the failure
path, because on this transport a refusal IS a handshake failure.

**Now:** the status is read unconditionally after connect, and the caller is told which
of three things happened — `Ok`, `Unreachable`, `Refused` — rather than true/false. The
distinction is not cosmetic: it is the difference between "your WiFi is flaky" and "go
click approve", and the device now names the id to approve.

**What this generalises to:** a layered protocol collapses two outcomes into one return
code whenever the lower layer's contract is narrower than the upper layer's question.
`esp_transport_connect` answers "is there a websocket here", and both "no host" and "the
host said no" are honestly no. The information that separates them survives, but only in
state, not in the return value — so the question to ask of a failing layer is not "what
did it return" but "what did it learn before it gave up".

The corollary, learned the hard way here: a comment on one side of a wire asserting what
the other side logs is not evidence. `relay.py` claimed the message for months and
nothing checked. See [[2026-08-05-22h33-a-retry-loop-turns-a-log-line-into-a-log-stream]]
for the noise that hid it — the wrong message was printing 12 times a minute and was
still not read, because nobody reads a line they have already scrolled past a hundred
times.
