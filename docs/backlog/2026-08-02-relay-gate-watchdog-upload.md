# Relay gate watchdog fires mid-upload (one-shot mode only)

**Found 2026-08-02 by inspection. Reproduced on hardware 2026-08-03. Largely
resolved the same day by addressed writes; what remains is narrow.**

## The bug

`relay-server/relay.py` serializes to one request in flight per device
(`DeviceConnection.gate`), held for a whole session, with a watchdog that releases
it after `REQUEST_TIMEOUT` (20 s) in case a device dies without sending FINAL. The
watchdog measures **session length, not idleness**, so an upload that legitimately
runs longer than 20 s has its gate taken away while it is still healthy, and a
concurrent request can then interleave into the middle of its request body.

## Reproduced

Paced a 1,070,704-byte image to WAN-like speed so the session spanned ~29 s, then
loaded a page at 24 s. The upload died at 884,736 bytes and the page load got a 504
as well — **both** requests lost.

The device reported `{"ok":false,"error":"stream failed at 884736 bytes"}` and left
the boot pointer alone, so nothing bad was written. That is thanks to the
`Stream::failed()` work backported the same day; before it, the truncated image
would have been validated and the failure reported as "image validation failed",
blaming the upload for a broken link.

## Resolved for the path that matters

`writePartition` now takes an optional `offset`, with `clearPartition` and
`activatePartition` as the explicit surrounding steps. A firmware push is therefore
many short sessions instead of one long one, and the watchdog never sees a session
long enough to trip over.

Verified on hardware over the relay: 1,067,456 bytes in 17 pieces of 64 KB with
1.5 s gaps — 37.7 s total, nearly double the watchdog — every piece accepted, image
activated, **and a browser page load succeeded in one of the gaps at t=25 s**, the
exact request that got a 504 in the failing case above.

## What remains

One-shot mode (`writePartition` with no `offset`) still holds the pipe for a whole
image, so it still trips the watchdog on a link slow enough to push it past 20 s. It
is fine on a LAN — measured 13–14 s for 1 MB over the relay — but that is only 6 s of
margin.

Two ways to close it, not exclusive:

1. **Make the watchdog measure idleness** — reset it on any chunk seen for that
   session in either direction. A long healthy upload keeps re-arming it; a genuinely
   dead device still frees the pipe. The device has its own `RECV_TIMEOUT_MS = 10000`
   in `RelaySessionLink::RecvChunk`, so the two timeouts want deciding together
   rather than separately.
2. **Have the web UI use chunked mode**, and leave one-shot for the local socket
   where the margin is comfortable. Needs no server change.

## Not the answer

Multiplexed channels — a slot table plus concurrent sessions — was previously
recorded here as "the real fix for the whole class". That was **rejected
2026-08-03**: concurrency is not worth per-channel buffers and a slot table on this
much RAM, and addressed writes achieve the same coexistence with no new machinery.
See `docs/reasoning/` and `2026-08-03-command-worker-task.md`.

Separately, exposing the relay publicly still wants a device→server credential
(today any client that guesses the MAC-derived device id **evicts** the real device
and takes its place), server-side login, and TLS.
