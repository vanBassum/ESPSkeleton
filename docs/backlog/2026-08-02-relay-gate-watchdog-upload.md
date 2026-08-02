# Relay gate watchdog fires mid-upload; firmware update over the relay untested

**Found 2026-08-02 by inspection, not by failure** — while reviewing whether the
relay demo is fit to expose. Nothing has actually corrupted yet, because nobody
has run a firmware update over the relay. That is also the point: this path is
the one that can brick a device you cannot physically reach, so it should be
fixed and tested before anyone relies on it remotely.

## The bug

`relay-server/relay.py` serializes to one request in flight per device
(`DeviceConnection.gate`), held for a whole session. A device that dies without
sending FINAL would wedge the pipe forever, so `acquire_gate` arms a watchdog:

```python
async def _gate_timeout(self, holder):
    await asyncio.sleep(REQUEST_TIMEOUT)      # 20 s
    if self._gate_holder == holder:
        self.release_gate(holder)
```

The watchdog is **per session**, but `REQUEST_TIMEOUT` was chosen for a single
small round trip. A firmware upload session lasts far longer than 20 s — hundreds
of 4 KB body chunks over a WAN, plus a ~12 s flash write inside the handler. So
the watchdog fires *while the session is still healthy*, releases the gate, and a
concurrent `getWebFile` (a browser loading any asset) can then interleave its
chunk into the middle of the upload's request body.

That is precisely the corruption the gate exists to prevent, and it lands on the
one payload where corruption means a bad image written to flash. On the device
side `Session::read()` would pull the interleaved chunk, see a session id that
isn't its own, and EOF the body early (`SessionMux.h`) — so the likely symptom is
a truncated image rather than silent garbage, but that is luck, not design.

## Fix direction

The watchdog should measure **idleness, not session length**: reset it on every
chunk seen for that session in either direction, and only release the gate when
nothing has moved for the timeout. A long healthy upload keeps re-arming it; a
genuinely dead device still frees the pipe. Note the device also has its own
`RECV_TIMEOUT_MS = 10000` in `RelaySessionLink::RecvChunk` — a slow WAN upload
could trip that too, so the two timeouts want deciding together rather than
separately.

Second, `updateBegin`/`updateWrite`/`updateEnd` and `writePartition` need an
actual end-to-end run over the relay. The transport path they exercise is the one
nothing else does: multi-chunk **request bodies** pulled through the queue-backed
`RecvChunk`, at the full 4096-byte inbound window, with a handler that holds the
pipe for tens of seconds.

## Related

The real fix for the whole class is the concurrency half —
`2026-07-03-multiplexed-channels.md`. A slot table plus a worker task removes the
need for a server-side in-flight gate at all, and with it this watchdog. Until
then the gate stays, and it needs to be right.

See `docs/superpowers/specs/2026-08-02-remote-access-relay-design.md` for the
transport it belongs to. Separate from this: exposing the relay publicly also
wants a device→server credential (today any client that guesses the MAC-derived
device id **evicts** the real device and takes its place), server-side login, and
TLS — all listed as out of scope for milestone 1 and still owed.
