#!/usr/bin/env python3
"""Minimal relay server for Strux devices.

Proves the remote-access path over a LAN: devices dial *out* to this server, and a
browser reaches a device's own web UI through it.

    device  ──ws──►  /device?id=<id>&fw=<ver>…    (outbound, NAT-friendly)
    browser ──http─►  /devices/<id>/{path}        → `web read` command on the device
    browser ──ws──►  /devices/<id>/ws             → relayed to the device pipe

What this server understands and does not:

  * It parses and REWRITES the 3-byte session header — it must, because it owns
    the session-id space on the device pipe (see below).
  * It never parses a session payload. Commands, auth, uploads and log lines are
    opaque bytes it moves between two sockets.

Who may connect: a device must be approved, and must present the token it was
approved with in an `X-Strux-Token` header, or the upgrade is refused with a 403.
Approvals live in SQLite; pairing a new device is one click in the dashboard. The
device id is technical (MAC-derived, public, in every URL) while `name` is what a
human reads — nothing is keyed on the name.

The human side of this server is expected to sit behind a reverse proxy that
authenticates users; there are no accounts here. `/device` cannot be, because a
device cannot follow a login redirect — hence the token.

Frontend files are cached here, for as long as the device stays connected — a device's
content cannot change without a reboot, and a reboot drops the pipe, so connecting is
the only invalidation point this server needs.

Deliberately out of scope for now: per-device origin isolation (every device's UI is
served from this one origin).

Open work: docs/backlog/2026-07-03-remote-access.md
Deployment: docs/backlog/2026-08-05-relay-in-production.md
"""

from __future__ import annotations

import argparse
import asyncio
import gzip
import hashlib
import hmac
import json
import logging
import os
import re
import sqlite3
import struct
import time
from collections import OrderedDict

import aiohttp
from aiohttp import WSMsgType, web

log = logging.getLogger("relay")

# ── Wire format ───────────────────────────────────────────────────────────────
# [ session : u16 LE ][ flags : u8 ][ opaque payload ]
HEADER = struct.Struct("<HB")
FLAG_FINAL = 0x01
FLAG_REJECT = 0x02

# Reserved by the device for its own broadcasts (log lines), so the pipe is not
# request/response and session 0 must be forwarded to every attached browser.
BROADCAST_SESSION = 0

# Also device-initiated, but consumed here instead of forwarded: Influx line protocol,
# one or more lines per chunk. A separate id from 0 because the two channels have
# different destinations — logs go to every browser, measurements go to a database —
# and the header is the right place to say which, rather than sniffing the payload.
TELEMETRY_SESSION = 0xFFFF

# Session-id ownership. Both this server and a browser would otherwise allocate
# from 1 on the same device socket and collide — which presents as "the device
# replied to the wrong request". So the server owns the space: browser ids are
# rewritten into the low half, our own web-read sessions come from the high half.
# SERVER_ID_LIMIT stops one short of 0x10000 to keep TELEMETRY_SESSION unallocatable.
BROWSER_ID_BASE, BROWSER_ID_LIMIT = 1, 0x8000
SERVER_ID_BASE, SERVER_ID_LIMIT = 0x8000, TELEMETRY_SESSION

# How long a session may go SILENT before this server gives up on it — never how
# long it may run. A 1 MB upload legitimately takes tens of seconds and used to have
# the pipe taken away mid-image, letting a concurrent request interleave into its
# request body; both requests then died. Measuring silence instead means a healthy
# upload re-arms the timer with every chunk, while a device that died still frees the
# pipe within this window.
#
# It is deliberately LONGER than the device's own RECV_TIMEOUT_MS (10 s, in
# RelaySessionLink): the two have to be decided together, because whichever fires
# first decides how the session ends. Device first is what we want — it EOFs its own
# request, its handler writes a reply, and that reply releases the gate in the normal
# way. Server first would mean releasing the pipe while the device still believes the
# session is open, which is precisely the interleaving the gate exists to prevent.
SESSION_IDLE_TIMEOUT = 15.0


class RelayError(Exception):
    """The device answered, but not usefully (reject, malformed, disconnected)."""


# ── Telemetry forwarding ──────────────────────────────────────────────────────
# Devices emit Influx LINE PROTOCOL, already formatted, so this server stays what it
# is everywhere else: something that moves bytes without interpreting them. It batches
# lines and POSTs them; it does not parse a measurement, a tag or a field.
#
# The one thing that costs: a device tags its own points with `device=<id>`, and this
# server does not verify that. An approved device could therefore write points
# attributed to another device. Injecting the tag here instead would mean finding the
# first unescaped space in every line — parsing, and the fiddly end of it. Left as the
# device's job while every device is one Bas installed; the note in the backlog says
# what to do if that stops being true.

class InfluxWriter:
    """Batches line-protocol lines and POSTs them to InfluxDB.

    Inert unless configured. A device that sends telemetry to a relay with no Influx
    behind it is not an error — the points are dropped and said so once.
    """

    # A device sends a point every few seconds and there may be many devices, so
    # batching keeps this from being one HTTP request per measurement. Either bound
    # trips a flush.
    MAX_BATCH = 500
    MAX_AGE = 5.0

    # Long enough to ride out a slow write, short enough that a wedged Influx does not
    # hold the flush task forever.
    TIMEOUT = 10.0

    def __init__(self, url: str, token: str, org: str, bucket: str):
        self.enabled = bool(url and token and bucket)
        self.url = url.rstrip("/") if url else ""
        self.token = token
        self.org = org
        self.bucket = bucket
        self.lines: list[bytes] = []
        self.dropped = 0
        self.written = 0
        self._oldest = 0.0
        self._session: aiohttp.ClientSession | None = None
        self._task: asyncio.Task | None = None
        self._warned = False

    async def start(self) -> None:
        if not self.enabled:
            log.info("telemetry: no Influx configured — device points will be dropped")
            return
        self._session = aiohttp.ClientSession()
        self._task = asyncio.create_task(self._flusher())
        log.info("telemetry: writing to %s bucket %r as org %r",
                 self.url, self.bucket, self.org)

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
        if self.lines:
            await self._flush()
        if self._session:
            await self._session.close()

    def submit(self, payload: bytes) -> None:
        """One telemetry chunk: one or more line-protocol lines, newline separated."""
        if not self.enabled:
            if not self._warned:
                self._warned = True
                log.warning("telemetry: dropping points, no Influx configured")
            self.dropped += 1
            return

        fresh = [ln.strip() for ln in payload.split(b"\n") if ln.strip()]
        if not fresh:
            return
        if not self.lines:
            self._oldest = time.monotonic()
        self.lines.extend(fresh)
        # Size bound is checked here so a burst does not sit waiting for the timer.
        if len(self.lines) >= self.MAX_BATCH:
            asyncio.create_task(self._flush())

    async def _flusher(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            if self.lines and time.monotonic() - self._oldest >= self.MAX_AGE:
                await self._flush()

    async def _flush(self) -> None:
        if not self.lines or not self._session:
            return
        batch, self.lines = self.lines, []

        # ns precision: the device sends nanoseconds when its clock is synced, and
        # omits the timestamp entirely when it is not, letting Influx stamp arrival.
        url = f"{self.url}/api/v2/write"
        params = {"org": self.org, "bucket": self.bucket, "precision": "ns"}
        headers = {"Authorization": f"Token {self.token}",
                   "Content-Type": "text/plain; charset=utf-8"}
        try:
            async with self._session.post(
                url, params=params, headers=headers, data=b"\n".join(batch),
                timeout=aiohttp.ClientTimeout(total=self.TIMEOUT),
            ) as r:
                if r.status >= 300:
                    body = (await r.text())[:300]
                    self.dropped += len(batch)
                    log.warning("telemetry: Influx refused %d lines (HTTP %d): %s",
                                len(batch), r.status, body)
                else:
                    self.written += len(batch)
                    log.debug("telemetry: wrote %d lines", len(batch))
        except Exception as exc:
            # Dropped, not retried: there is no buffer here yet and holding a growing
            # list while Influx is down trades a gap in a graph for the relay's RAM.
            self.dropped += len(batch)
            log.warning("telemetry: write of %d lines failed (%s)", len(batch), exc)


influx: InfluxWriter | None = None


# ── Frontend file cache ───────────────────────────────────────────────────────
# Why it exists: one request in flight per device is permanent (multiplexing was
# rejected 2026-08-03), so an uncached page load is N *sequential* WAN round trips,
# each one an ESP32 reading flash 512 bytes at a time. Caching makes that happen once
# instead of once per view.
#
# What it is keyed on is the part that changed. The backlog proposed
# (deviceId, firmware, path) — but the www partition is uploaded INDEPENDENTLY of the
# app, which is not a hypothetical: on 2026-08-11 a device's entire UI was replaced
# while it kept reporting fw 0.0.5. A firmware-keyed cache would have served the old UI
# forever.
#
# So the key is (deviceId, path) and the freshness rule comes from the build instead.
# Vite content-hashes every asset's NAME (assets/index-D3-EqElo.js), so those bytes can
# never change meaning — cached with no expiry, and a new build simply asks for
# different names. index.html is the one mutable file and it is the file that *names*
# the hashed ones, so its freshness is sufficient for the correctness of everything
# else. It gets a short TTL; nothing needs a version number.
#
# The same distinction is handed to the browser as Cache-Control, which matters more
# than the server-side hit: an immutable asset is not requested again at all, so the
# second page load costs zero pipe traffic rather than a cheap hit.

class FileCache:
    """(deviceId, path) → one frontend file, bounded, LRU, with single-flight.

    **A connection is the cache's lifetime.** Entries are dropped when a device
    connects and then kept for as long as that connection lasts — no TTL and no
    revalidation, because the only moment a device's content can change under us is
    one we already see: it has to reboot, and rebooting drops the pipe.

    That is what keeps this entirely server-side. The alternative — the device
    announcing a content digest so the server could tell a flap from a reflash — works,
    but it puts a relay's caching strategy into the firmware, and no other transport has
    an opinion about it. What it costs is the one case connect cannot see: `www`
    replaced on a running device without a reboot. That stays cached until the device
    next reconnects, and the Flush button is the answer.
    """

    # Bytes, not entries: entries here are one 477-byte index.html and one 130 KB
    # bundle, and it is the bundle that decides whether this fits in a container.
    MAX_BYTES = 32 * 1024 * 1024

    # A vite content-hashed name: assets/<name>-<hash>.<ext>. Only used to decide what
    # the BROWSER is told (immutable vs revalidate) — server-side, every entry lives
    # for the connection either way.
    IMMUTABLE = re.compile(r"/assets/[^/]+-[A-Za-z0-9_-]{8,}\.[A-Za-z0-9]+$")

    # Files named by index.html, so a freshly connected device can be warmed in one go
    # instead of one round trip per asset on somebody's first page load.
    ASSET_REF = re.compile(r"""["']\.?(/assets/[^"']+)["']""")

    def __init__(self) -> None:
        # Keyed (deviceId, path). OrderedDict as an LRU: move_to_end on read,
        # popitem(last=False) to evict.
        self.entries: "OrderedDict[tuple[str, str], dict]" = OrderedDict()
        self.bytes = 0
        self.hits = 0
        self.misses = 0
        # One in-flight fetch per key. Two browsers asking for the same uncached file
        # would otherwise take the pipe twice in a row for identical bytes.
        self._inflight: dict[tuple[str, str], asyncio.Future] = {}

    @classmethod
    def immutable(cls, path: str) -> bool:
        return bool(cls.IMMUTABLE.search(path))

    def get(self, key: tuple[str, str]) -> dict | None:
        entry = self.entries.get(key)
        if entry is None:
            return None
        self.entries.move_to_end(key)
        self.hits += 1
        return entry

    def put(self, key: tuple[str, str], header: dict, body: bytes) -> dict:
        # A single file larger than the whole budget would evict everything and then
        # itself; refuse to store it rather than thrash.
        entry = {
            "header": header,
            "body": body,
            # Weak ETag would be wrong here: these are exact bytes, and the browser
            # gets a 304 off this comparison.
            "etag": '"' + hashlib.sha256(body).hexdigest()[:16] + '"',
        }
        if len(body) > self.MAX_BYTES:
            return entry
        self._drop(key)
        self.entries[key] = entry
        self.bytes += len(body)
        while self.bytes > self.MAX_BYTES and self.entries:
            self._drop(next(iter(self.entries)))
        return entry

    def _drop(self, key: tuple[str, str]) -> None:
        entry = self.entries.pop(key, None)
        if entry is not None:
            self.bytes -= len(entry["body"])

    async def get_or_fetch(self, conn: "DeviceConnection", path: str) -> dict:
        """Cached entry for `path`, fetching it off the device at most once."""
        key = (conn.device_id, path)
        entry = self.get(key)
        if entry is not None:
            return entry

        # Someone else is already pulling these exact bytes — wait for theirs.
        inflight = self._inflight.get(key)
        if inflight is not None:
            return await asyncio.shield(inflight)

        loop = asyncio.get_running_loop()
        future: asyncio.Future = loop.create_future()
        self._inflight[key] = future
        try:
            self.misses += 1
            header, body = await conn.get_web_file(path)
            # Only 200s are cached. A 404 is cheap, and caching one would pin a
            # mistake for as long as the entry lives.
            entry = (self.put(key, header, body) if header.get("status") == 200
                     else {"header": header, "body": body, "etag": None})
            if not future.done():
                future.set_result(entry)
            return entry
        except BaseException as exc:
            if not future.done():
                future.set_exception(exc)
            raise
        finally:
            self._inflight.pop(key, None)
            # Nobody may be awaiting an exception we already re-raised.
            if future.done() and future.exception() is not None:
                future.exception()

    def drop_device(self, device_id: str) -> int:
        n = 0
        for key in [k for k in self.entries if k[0] == device_id]:
            self._drop(key)
            n += 1
        return n

    def flush(self) -> int:
        n = len(self.entries)
        self.entries.clear()
        self.bytes = 0
        return n

    def stats(self) -> dict:
        return {"entries": len(self.entries), "bytes": self.bytes,
                "hits": self.hits, "misses": self.misses}


cache = FileCache()


async def warm_cache(conn: "DeviceConnection") -> None:
    """Pull a freshly connected device's frontend into the cache, once.

    Runs as its own task: it takes the pipe several times over, and the device's read
    loop must not wait for that. A browser arriving mid-warm is not a problem — it
    shares the in-flight fetch rather than starting a second one.

    index.html first, then everything it names. Fetching the assets it *names* rather
    than crawling the partition is what keeps this to the files that are actually
    served, and it stays correct across a rebuild for free: a new build names new files.
    """
    try:
        entry = await cache.get_or_fetch(conn, "/index.html")
        if entry["header"].get("status") != 200:
            log.info("warm %s: no index.html", conn.device_id)
            return

        body = entry["body"]
        if entry["header"].get("contentEncoding") == "gzip":
            # Stored gzipped on the device and served through untouched — so to read
            # the references out of it, decompress a copy here and throw it away.
            body = gzip.decompress(body)

        paths = sorted(set(FileCache.ASSET_REF.findall(body.decode("utf-8", "replace"))))
        for path in paths:
            if conn.ws.closed:
                return
            await cache.get_or_fetch(conn, path)

        log.info("warmed %s: index.html + %d asset%s, %d kB",
                 conn.device_id, len(paths), "" if len(paths) == 1 else "s",
                 cache.bytes // 1024)
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        # Never fatal: an unwarmed cache is a slow first page load, not an error, and
        # the device is still perfectly usable.
        log.info("warm %s stopped: %s", conn.device_id, exc)


class DeviceConnection:
    """One ESP32's outbound socket: id space, session maps, and the in-flight gate."""

    # Per-pipe serial number, only for logs. One device id can have two pipes alive at
    # once for a moment during a reconnect, and a line that does not say which one it
    # is about is worse than no line.
    _seq = 0

    def __init__(self, device_id: str, firmware: str, ws: web.WebSocketResponse,
                 name: str = "", project: str = ""):
        DeviceConnection._seq += 1
        self.pipe = DeviceConnection._seq
        self.device_id = device_id
        self.firmware = firmware
        # Display only. device_id is the technical identity and the thing the token
        # proves; these two are what a human reads in the device list, so nothing is
        # ever keyed on them and a rename costs a device nothing.
        self.name = name or device_id
        self.project = project
        self.ws = ws
        self.connected_at = time.time()

        # One request in flight per device. Today's device mux dispatches a chunk
        # synchronously with no slot table, so overlapping sessions would let a
        # file fetch's chunk land in the middle of a streamed request body. Held
        # for a whole session, not just one chunk.
        self.gate = asyncio.Lock()
        self._gate_holder: int | None = None
        self._gate_watchdog: asyncio.Task | None = None
        self._gate_touched = 0.0     # monotonic time of the holder's last chunk

        # aiohttp writes a frame per call, but two tasks interleaving awaits on
        # the same socket is not worth risking.
        self.send_lock = asyncio.Lock()

        self.server_sessions: dict[int, asyncio.Queue] = {}      # our web-read calls
        self.browser_sessions: dict[int, tuple] = {}             # device sid -> (browser, browser sid)
        self.browser_map: dict[tuple, int] = {}                  # (browser, browser sid) -> device sid
        self.browsers: set[web.WebSocketResponse] = set()

        self._next_server_id = SERVER_ID_BASE
        self._next_browser_id = BROWSER_ID_BASE

    # ── id allocation ─────────────────────────────────────────────────────────

    def _alloc(self, taken, cursor_attr: str, base: int, limit: int) -> int:
        for _ in range(limit - base):
            sid = getattr(self, cursor_attr)
            nxt = sid + 1
            setattr(self, cursor_attr, base if nxt >= limit else nxt)
            if sid not in taken:
                return sid
        raise RelayError("no free session ids")

    def alloc_server_id(self) -> int:
        return self._alloc(self.server_sessions, "_next_server_id", SERVER_ID_BASE, SERVER_ID_LIMIT)

    def alloc_browser_id(self) -> int:
        return self._alloc(self.browser_sessions, "_next_browser_id", BROWSER_ID_BASE, BROWSER_ID_LIMIT)

    # ── the in-flight gate ────────────────────────────────────────────────────

    async def acquire_gate(self, holder: int) -> None:
        await self.gate.acquire()
        self._gate_holder = holder
        self._gate_touched = time.monotonic()
        # A device that never FINALs must not wedge the pipe for good.
        self._gate_watchdog = asyncio.create_task(self._gate_timeout(holder))

    def touch_gate(self, sid: int) -> None:
        """A chunk moved for this session, in either direction — it is alive.

        Only the holder's own traffic counts. Session 0 carries the device's log
        broadcasts continuously, so re-arming on any chunk at all would mean the
        watchdog never fires for anything.
        """
        if sid == self._gate_holder:
            self._gate_touched = time.monotonic()

    async def _gate_timeout(self, holder: int) -> None:
        """Release the pipe after SESSION_IDLE_TIMEOUT of silence for this session.

        Sleeps to the deadline as it stands, then re-checks: every chunk pushes
        _gate_touched forward, so a long healthy transfer keeps extending the sleep
        and never trips, without waking this task per chunk.
        """
        try:
            while True:
                remaining = SESSION_IDLE_TIMEOUT - (time.monotonic() - self._gate_touched)
                if remaining <= 0:
                    break
                await asyncio.sleep(remaining)
        except asyncio.CancelledError:
            return

        if self._gate_holder == holder:
            log.warning("device %s: session %d silent for %.0fs, releasing the pipe",
                        self.device_id, holder, SESSION_IDLE_TIMEOUT)
            self.release_gate(holder)

    def release_gate(self, holder: int) -> None:
        if self._gate_holder != holder:
            return
        self._gate_holder = None
        if self._gate_watchdog:
            self._gate_watchdog.cancel()
            self._gate_watchdog = None
        if self.gate.locked():
            self.gate.release()

    # ── sending ───────────────────────────────────────────────────────────────

    async def send_chunk(self, sid: int, flags: int, payload: bytes = b"") -> None:
        async with self.send_lock:
            await self.ws.send_bytes(HEADER.pack(sid, flags) + payload)

    # ── device → server ───────────────────────────────────────────────────────

    async def on_device_chunk(self, data: bytes) -> None:
        if len(data) < HEADER.size:
            return
        sid, flags = HEADER.unpack_from(data, 0)
        payload = data[HEADER.size:]

        if sid == BROADCAST_SESSION:
            await self.fanout(data)        # verbatim — the browser expects this shape
            return

        # Consumed here, not forwarded. Deliberately NOT fanned out to browsers: a
        # measurement is not a log line, and the frontend would have to learn a second
        # payload shape on a channel it treats as text.
        if sid == TELEMETRY_SESSION:
            if influx is not None:
                influx.submit(payload)
            return

        # Device → us counts as progress: an upload's progress reports arrive this way,
        # and so does every chunk of a long file being read out of the device.
        self.touch_gate(sid)

        queue = self.server_sessions.get(sid)
        if queue is not None:
            queue.put_nowait((flags, payload))
            return

        entry = self.browser_sessions.get(sid)
        if entry is not None:
            browser, browser_sid = entry
            await self._to_browser(browser, browser_sid, flags, payload)
            if flags & (FLAG_FINAL | FLAG_REJECT):
                self.browser_sessions.pop(sid, None)
                self.browser_map.pop((id(browser), browser_sid), None)
                self.release_gate(sid)
            return

        log.warning("device %s: chunk for unknown session %d (dropped)", self.device_id, sid)

    async def fanout(self, raw: bytes) -> None:
        for browser in list(self.browsers):
            try:
                await browser.send_bytes(raw)
            except Exception:
                self.browsers.discard(browser)

    async def _to_browser(self, browser, browser_sid: int, flags: int, payload: bytes) -> None:
        try:
            await browser.send_bytes(HEADER.pack(browser_sid, flags) + payload)
        except Exception:
            self.browsers.discard(browser)

    # ── browser → device ──────────────────────────────────────────────────────

    async def relay_from_browser(self, browser, data: bytes) -> None:
        if len(data) < HEADER.size:
            return
        browser_sid, flags = HEADER.unpack_from(data, 0)
        payload = data[HEADER.size:]

        key = (id(browser), browser_sid)
        device_sid = self.browser_map.get(key)
        if device_sid is None:
            # First chunk of a new session: take the pipe and mint our own id.
            device_sid = self.alloc_browser_id()
            await self.acquire_gate(device_sid)
            self.browser_map[key] = device_sid
            self.browser_sessions[device_sid] = (browser, browser_sid)

        await self.send_chunk(device_sid, flags, payload)

        # Browser → device counts too, and this is the direction that matters most: a
        # one-shot firmware upload is minutes of body chunks with the device saying
        # almost nothing back.
        self.touch_gate(device_sid)

    def drop_browser(self, browser) -> None:
        self.browsers.discard(browser)
        for key in [k for k in self.browser_map if k[0] == id(browser)]:
            device_sid = self.browser_map.pop(key)
            self.browser_sessions.pop(device_sid, None)
            self.release_gate(device_sid)

    # ── the one command this server issues ────────────────────────────────────

    async def get_web_file(self, path: str) -> tuple[dict, bytes]:
        """Ask the device for one frontend file. Reply is a header line then bytes."""
        sid = self.alloc_server_id()
        queue: asyncio.Queue = asyncio.Queue()
        self.server_sessions[sid] = queue

        await self.acquire_gate(sid)
        try:
            request = json.dumps({"type": "web read", "path": path}) + "\n"
            await self.send_chunk(sid, FLAG_FINAL, request.encode())

            body = bytearray()
            while True:
                # Per chunk, not per reply: a large file arrives as many chunks, and
                # the same idleness rule applies to each wait.
                flags, payload = await asyncio.wait_for(queue.get(), SESSION_IDLE_TIMEOUT)
                if flags & FLAG_REJECT:
                    raise RelayError(f"device rejected web read: "
                                     f"{payload.decode('utf-8', 'replace')}")
                body += payload
                if flags & FLAG_FINAL:
                    break
        finally:
            self.server_sessions.pop(sid, None)
            self.release_gate(sid)

        newline = body.find(b"\n")
        if newline < 0:
            raise RelayError("malformed web read reply (no header line)")
        try:
            header = json.loads(bytes(body[:newline]))
        except ValueError as exc:
            raise RelayError(f"unparseable web read header: {exc}") from exc
        return header, bytes(body[newline + 1:])

    async def close(self) -> None:
        for queue in list(self.server_sessions.values()):
            queue.put_nowait((FLAG_REJECT, b"device disconnected"))
        self.server_sessions.clear()
        if self._gate_holder is not None:
            self.release_gate(self._gate_holder)
        for browser in list(self.browsers):
            try:
                await browser.close()
            except Exception:
                pass
        self.browsers.clear()

        # And the device socket itself. Without this a pipe that has already been
        # REPLACED stays open until aiohttp's heartbeat notices, ~30 s later — so its
        # teardown gets logged long after the new pipe is serving, which reads exactly
        # like the live device dropping. It cost an afternoon of chasing a phantom
        # reconnect loop; the device was fine the whole time.
        try:
            await self.ws.close()
        except Exception:
            pass


# ── Pairing store ─────────────────────────────────────────────────────────────
# What survives a restart: which device ids are approved and what token each one
# proved itself with, plus a log of refusals and approvals.
#
# A device id is public — it is in every /devices/<id>/ URL, and it is derived from
# the board's MAC. The token is what is secret, and it is the *device* that generates
# it: this server only ever pins the value a device presented, so there is no
# server→device message that could set one and no network-triggered NVS write.
#
# Queries here are per-connect and tiny, so they run inline on the event loop.
# Anything blob-sized (the file cache this will grow) belongs in asyncio.to_thread.

SCHEMA = """
CREATE TABLE IF NOT EXISTS approved (
    device_id  TEXT PRIMARY KEY,
    token      TEXT NOT NULL,
    name       TEXT,
    project    TEXT,
    firmware   TEXT,
    approved_at REAL NOT NULL,
    last_seen  REAL
);

-- Keyed on the PAIR, not the id: two different tokens claiming one id is what
-- somebody guessing looks like, and collapsing them would hide exactly that.
CREATE TABLE IF NOT EXISTS pending (
    device_id  TEXT NOT NULL,
    token      TEXT NOT NULL,
    name       TEXT,
    project    TEXT,
    firmware   TEXT,
    first_seen REAL NOT NULL,
    last_seen  REAL NOT NULL,
    attempts   INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (device_id, token)
);

CREATE TABLE IF NOT EXISTS events (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    at        REAL NOT NULL,
    kind      TEXT NOT NULL,
    device_id TEXT,
    detail    TEXT
);
CREATE INDEX IF NOT EXISTS events_at ON events (at DESC);
"""

# A public endpoint that INSERTs is a disk-filling machine, so the pending list is
# bounded. Past the cap, a *known* pair still counts its attempts (so the signal
# keeps rising) but no new row is created.
MAX_PENDING = 50

# The event log is a record of state changes — approvals, first sightings, removals —
# so it grows with operator actions and new devices, not with traffic. A few hundred
# is far more than the dashboard shows and still bounded.
MAX_EVENTS = 500


class Store:
    def __init__(self, path: str):
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        # WAL so a reader never blocks the connect path.
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.executescript(SCHEMA)
        self.db.commit()
        log.info("pairing store at %s (%d approved)", path, self.approved_count())

    def approved_count(self) -> int:
        return self.db.execute("SELECT count(*) FROM approved").fetchone()[0]

    def log_event(self, kind: str, device_id: str | None, detail: str = "") -> None:
        self.db.execute(
            "INSERT INTO events (at, kind, device_id, detail) VALUES (?,?,?,?)",
            (time.time(), kind, device_id, detail))
        # Trimmed on insert, because the dashboard only ever reads the newest handful
        # and an unbounded table on a public endpoint's path is the disk-filling
        # machine MAX_PENDING was written to avoid — reached through another table.
        # Cheap now that events are state changes rather than one per connect attempt.
        self.db.execute(
            "DELETE FROM events WHERE id <= (SELECT max(id) - ? FROM events)",
            (MAX_EVENTS,))
        self.db.commit()

    # ── the connect decision ──────────────────────────────────────────────────

    def authenticate(self, device_id: str, token: str, name: str,
                     project: str, firmware: str) -> tuple[bool, str]:
        """May this device have the pipe? Returns (ok, reason-if-not).

        A refusal records the pair as pending, which is the only way a new device
        ever gets paired: it is refused once, shows up in the dashboard, and the
        reconnect after approval succeeds.
        """
        now = time.time()
        row = self.db.execute(
            "SELECT token FROM approved WHERE device_id = ?", (device_id,)).fetchone()

        if row is not None:
            # compare_digest rather than ==: token comparison is the one place here
            # where how long a mismatch takes tells an attacker something.
            if token and hmac.compare_digest(row["token"], token):
                self.db.execute(
                    "UPDATE approved SET last_seen = ?, name = ?, project = ?, "
                    "firmware = ? WHERE device_id = ?",
                    (now, name, project, firmware, device_id))
                self.db.commit()
                return True, ""

            # Approved id, wrong token. Somebody is either guessing, or this is the
            # same board after an NVS wipe — the MAC-derived id survives that, the
            # token does not. Both look identical from here, so record and refuse:
            # the human decides which it was, in the dashboard.
            if self._remember_pending(device_id, token, name, project, firmware, now):
                self.log_event("refused", device_id, "token mismatch")
            return False, "token mismatch"

        if self._remember_pending(device_id, token, name, project, firmware, now):
            self.log_event("refused", device_id, "not approved")
        return False, "device not approved"

    def _remember_pending(self, device_id: str, token: str, name: str,
                          project: str, firmware: str, now: float) -> bool:
        """Record this attempt. True only the FIRST time this pair is seen.

        The return value is what keeps the event log readable. A device retries until
        somebody approves it, so logging an event per refusal filled the dashboard with
        one line repeated forty times and pushed the approval that mattered off the
        top. The repeat signal already has a better home: `attempts` on the pending
        row, which is one row that counts rather than N rows that each say the same.
        """
        cur = self.db.execute(
            "UPDATE pending SET last_seen = ?, attempts = attempts + 1, name = ?, "
            "project = ?, firmware = ? WHERE device_id = ? AND token = ?",
            (now, name, project, firmware, device_id, token))
        if cur.rowcount == 0:
            n = self.db.execute("SELECT count(*) FROM pending").fetchone()[0]
            if n >= MAX_PENDING:
                self.db.commit()
                log.warning("pending list is full (%d) — not recording %s",
                            MAX_PENDING, device_id)
                return False
            self.db.execute(
                "INSERT INTO pending (device_id, token, name, project, firmware, "
                "first_seen, last_seen) VALUES (?,?,?,?,?,?,?)",
                (device_id, token, name, project, firmware, now, now))
            self.db.commit()
            return True
        self.db.commit()
        return False

    # ── what the dashboard drives ─────────────────────────────────────────────

    def pending(self) -> list[dict]:
        rows = self.db.execute(
            "SELECT * FROM pending ORDER BY first_seen").fetchall()
        return [dict(r) for r in rows]

    def approved(self) -> list[dict]:
        rows = self.db.execute(
            "SELECT * FROM approved ORDER BY device_id").fetchall()
        return [dict(r) for r in rows]

    def approve(self, device_id: str, token: str) -> bool:
        """Pin the token this pair presented. Replaces any existing approval for
        the id, which is what re-pairing a wiped board is."""
        row = self.db.execute(
            "SELECT * FROM pending WHERE device_id = ? AND token = ?",
            (device_id, token)).fetchone()
        if row is None:
            return False
        self.db.execute(
            "INSERT INTO approved (device_id, token, name, project, firmware, "
            "approved_at) VALUES (?,?,?,?,?,?) "
            "ON CONFLICT(device_id) DO UPDATE SET token=excluded.token, "
            "name=excluded.name, project=excluded.project, "
            "firmware=excluded.firmware, approved_at=excluded.approved_at",
            (device_id, token, row["name"], row["project"], row["firmware"],
             time.time()))
        # Every pending row for this id goes, not just the approved pair: the other
        # rows were guesses at an id that is now settled.
        self.db.execute("DELETE FROM pending WHERE device_id = ?", (device_id,))
        self.db.commit()
        self.log_event("approved", device_id)
        return True

    def forget(self, device_id: str) -> bool:
        cur = self.db.execute("DELETE FROM approved WHERE device_id = ?", (device_id,))
        self.db.execute("DELETE FROM pending WHERE device_id = ?", (device_id,))
        self.db.commit()
        if cur.rowcount:
            self.log_event("forgotten", device_id)
        return bool(cur.rowcount)

    def recent_events(self, limit: int = 50) -> list[dict]:
        rows = self.db.execute(
            "SELECT * FROM events ORDER BY at DESC LIMIT ?", (limit,)).fetchall()
        return [dict(r) for r in rows]


store: Store | None = None


# ── Registry ──────────────────────────────────────────────────────────────────

devices: dict[str, DeviceConnection] = {}


# ── Endpoints ─────────────────────────────────────────────────────────────────

async def device_ws(request: web.Request) -> web.StreamResponse:
    """The device's outbound pipe. Registration is the query string — no protocol verb."""
    device_id = request.query.get("id")
    firmware = request.query.get("fw", "unknown")
    name = request.query.get("name", "")
    project = request.query.get("project", "")
    token = request.headers.get("X-Strux-Token", "")
    if not device_id:
        return web.Response(status=400, text="missing ?id=")

    # Refused BEFORE the upgrade, so the device gets an HTTP status it already logs
    # ("upgrade refused with HTTP 403") instead of a socket that opens and dies. This
    # is the check that makes the endpoint safe to leave on the public internet: no
    # stranger can register, and nobody can take an approved device's slot.
    assert store is not None
    ok, reason = store.authenticate(device_id, token, name, project, firmware)
    if not ok:
        log.warning("refused device %s from %s: %s", device_id, request.remote, reason)
        return web.Response(status=403, text=reason)

    ws = web.WebSocketResponse(heartbeat=30.0)
    await ws.prepare(request)

    conn = DeviceConnection(device_id, firmware, ws, name, project)
    previous = devices.get(device_id)
    if previous is not None:
        log.info("device %s reconnected on pipe #%d, dropping pipe #%d",
                 device_id, conn.pipe, previous.pipe)
        await previous.close()
    devices[device_id] = conn
    # The device says what it is serving, so a reconnect no longer has to mean throwing
    # its files away. Same digest — a flap, a redeploy of this server's peer, a WiFi
    # blip — and the cache stays warm; different digest and everything under the old one
    # goes, because that device was reflashed. Entries are keyed by digest too, so this
    # is memory hygiene rather than the thing that makes it correct.
    # Connect is the invalidation point, and the only one this server needs: a device's
    # content cannot change without a reboot, and a reboot lands here. Everything cached
    # for the old connection goes, and the new one is warmed in the background — so the
    # files are pulled once, now, instead of during somebody's first page load.
    dropped = cache.drop_device(device_id)
    asyncio.create_task(warm_cache(conn))
    log.info("device %s connected on pipe #%d (%s fw %s) from %s%s",
             device_id, conn.pipe, name or "unnamed", firmware, request.remote,
             f" — dropped {dropped} cached files" if dropped else "")

    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                await conn.on_device_chunk(msg.data)
            elif msg.type == WSMsgType.ERROR:
                log.warning("device %s pipe #%d socket error: %s",
                            device_id, conn.pipe, ws.exception())
                break
    finally:
        # Whether this pipe is still THE pipe decides what this teardown means: the
        # device going away, or a pipe that was already replaced finally closing.
        current = devices.get(device_id) is conn
        if current:
            devices.pop(device_id, None)
        await conn.close()
        log.info("device %s pipe #%d closed%s", device_id, conn.pipe,
                 "" if current else " (already replaced — device is still connected)")

    return ws


async def browser_ws(request: web.Request) -> web.StreamResponse:
    """A browser's socket, relayed onto the device pipe with ids rewritten."""
    device_id = request.match_info["deviceId"]
    conn = devices.get(device_id)
    if conn is None:
        return web.Response(status=503, text=f"device '{device_id}' is not connected")

    ws = web.WebSocketResponse(heartbeat=30.0)
    await ws.prepare(request)
    conn.browsers.add(ws)
    log.info("browser attached to %s (%d total)", device_id, len(conn.browsers))

    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                await conn.relay_from_browser(ws, msg.data)
            elif msg.type == WSMsgType.ERROR:
                break
    finally:
        conn.drop_browser(ws)
        log.info("browser detached from %s", device_id)

    return ws


async def device_frontend(request: web.Request) -> web.StreamResponse:
    """Serve the device's own frontend, from the cache when it can be."""
    device_id = request.match_info["deviceId"]
    tail = request.match_info.get("path", "")

    conn = devices.get(device_id)
    if conn is None:
        return web.Response(status=503, text=f"device '{device_id}' is not connected")

    path = "/" + tail if tail else "/index.html"

    try:
        entry = await cache.get_or_fetch(conn, path)

        if entry["header"].get("status") != 200:
            # SPA fallback is OUR decision — the device answers a real 404. Only
            # fall back for things that look like routes: a mistyped asset must
            # stay a 404 rather than becoming HTML with status 200, which the
            # browser would reject as a MIME error.
            leaf = tail.rsplit("/", 1)[-1]
            if "." in leaf:
                return web.Response(status=404, text=f"{path} not found on {device_id}")
            # Served under index.html's own key, so a fallback and a real index.html
            # request share one cache entry rather than each holding a copy.
            path = "/index.html"
            entry = await cache.get_or_fetch(conn, path)
            if entry["header"].get("status") != 200:
                return web.Response(status=404, text="no index.html on the device")

    except asyncio.TimeoutError:
        return web.Response(status=504, text=f"device '{device_id}' timed out")
    except RelayError as exc:
        return web.Response(status=502, text=str(exc))

    header, body, etag = entry["header"], entry["body"], entry["etag"]

    # Told to the browser, so the *second* load costs no pipe traffic at all rather
    # than a cheap server-side hit. Only content-hashed names may be immutable; for
    # everything else no-cache means "ask, but a 304 is likely", which is one small
    # conditional GET instead of a bundle.
    headers = {}
    if etag:
        headers["ETag"] = etag
        headers["Cache-Control"] = ("public, max-age=31536000, immutable"
                                    if cache.immutable(path) else "no-cache")
        if request.headers.get("If-None-Match") == etag:
            return web.Response(status=304, headers=headers)

    encoding = header.get("contentEncoding")
    if encoding:
        # The device stores its frontend gzipped; without this the browser gets
        # gzip bytes labelled as JavaScript.
        headers["Content-Encoding"] = encoding

    return web.Response(
        body=body,
        headers=headers,
        content_type=header.get("contentType", "application/octet-stream"),
    )


async def device_redirect(request: web.Request) -> web.StreamResponse:
    """/devices/<id> → /devices/<id>/ so the page's relative asset URLs resolve."""
    return web.HTTPFound(f"/devices/{request.match_info['deviceId']}/")


async def api_devices(request: web.Request) -> web.StreamResponse:
    now = time.time()
    return web.json_response([
        {
            "deviceId": conn.device_id,
            "name": conn.name,
            "project": conn.project,
            "firmware": conn.firmware,
            "online": not conn.ws.closed,
            "uptimeSeconds": int(now - conn.connected_at),
            "browsers": len(conn.browsers),
        }
        # Sorted by the human-readable name, since that is what the list shows first.
        for conn in sorted(devices.values(), key=lambda c: (c.name.lower(), c.device_id))
    ])


# ── Pairing API ───────────────────────────────────────────────────────────────
# Everything under here is on the browser side of the proxy, so Authentik has
# already decided who is asking. There is no additional check: whoever can load
# the dashboard is an admin, because that is what the proxy provider grants.

async def api_pairing(request: web.Request) -> web.StreamResponse:
    assert store is not None
    return web.json_response({
        "pending": store.pending(),
        "approved": store.approved(),
        "events": store.recent_events(20),
        "connected": sorted(devices.keys()),
        "cache": cache.stats(),
        "telemetry": {
            "enabled": bool(influx and influx.enabled),
            "written": influx.written if influx else 0,
            "dropped": influx.dropped if influx else 0,
            "queued": len(influx.lines) if influx else 0,
        },
    })


async def api_approve(request: web.Request) -> web.StreamResponse:
    assert store is not None
    body = await request.json()
    device_id, token = body.get("deviceId"), body.get("token")
    if not device_id or token is None:
        return web.json_response({"error": "deviceId and token are required"}, status=400)

    if not store.approve(device_id, token):
        return web.json_response({"error": "no such pending device"}, status=404)

    # Nothing is pushed to the device: it is already retrying every few seconds, so
    # the next attempt is the one that succeeds. That is the whole handshake.
    log.info("approved device %s", device_id)
    return web.json_response({"ok": True})


async def api_forget(request: web.Request) -> web.StreamResponse:
    assert store is not None
    body = await request.json()
    device_id = body.get("deviceId")
    if not device_id:
        return web.json_response({"error": "deviceId is required"}, status=400)

    forgotten = store.forget(device_id)
    cache.drop_device(device_id)

    # Drop the live pipe too, or a device stays connected after being revoked —
    # the token is only checked when a connection is made.
    conn = devices.get(device_id)
    if conn is not None:
        await conn.close()
        try:
            await conn.ws.close()
        except Exception:
            pass
        devices.pop(device_id, None)

    log.info("forgot device %s", device_id)
    return web.json_response({"ok": True, "wasApproved": forgotten})


async def api_cache_flush(request: web.Request) -> web.StreamResponse:
    """Drop every cached file. The escape hatch for a UI replaced without a reboot —
    the MUTABLE_TTL gets there on its own within seconds, this is for not waiting."""
    n = cache.flush()
    log.info("cache flushed (%d files)", n)
    return web.json_response({"ok": True, "dropped": n})


DASHBOARD = """<!doctype html>
<html><head><meta charset="utf-8"><title>Strux relay</title>
<style>
 :root { color-scheme: light dark; }
 body { font: 15px/1.5 system-ui, sans-serif; margin: 3rem auto; max-width: 52rem; padding: 0 1rem; }
 h1 { font-size: 1.3rem; margin-bottom: .25rem; }
 h2 { font-size: .8rem; text-transform: uppercase; letter-spacing: .04em; color: #888;
      margin: 2.5rem 0 0; }
 p.sub { color: #888; margin-top: 0; }
 table { border-collapse: collapse; width: 100%; margin-top: .75rem; }
 th, td { text-align: left; padding: .5rem .6rem; border-bottom: 1px solid #8883;
          vertical-align: top; }
 th { font-weight: 600; font-size: .8rem; text-transform: uppercase; letter-spacing: .04em; color: #888; }
 .dot { display: inline-block; width: .55rem; height: .55rem; border-radius: 50%; background: #22c55e; }
 .dot.off { background: #71717a; }
 code { font-size: .85em; color: #888; }
 .id { font-size: .8em; color: #888; font-family: ui-monospace, monospace; }
 .empty { color: #888; padding: 2rem 0; }
 button { font: inherit; font-size: .85em; padding: .2rem .7rem; border-radius: .3rem;
          border: 1px solid #8886; background: #8881; cursor: pointer; }
 button:hover { background: #8883; }
 button.primary { border-color: #22c55e88; background: #22c55e22; }
 .warn { border: 1px solid #f59e0b66; background: #f59e0b14; border-radius: .4rem;
         padding: .1rem 1rem 1rem; margin-top: 1.5rem; }
 .warn h2 { color: #f59e0b; }
 .ev { font-size: .8em; color: #888; }
</style></head>
<body>
 <h1>Strux relay</h1>
 <p class="sub">Devices dial out to this server. Click one to open its own web UI.</p>
 <div id="out"><p class="empty">Loading…</p></div>
<script>
// The device chooses its own name, and an UNAPPROVED device can put one in the
// pending list — so these strings are attacker-controlled and reach an admin page.
// Escaped, always.
function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function ago(t) {
  if (!t) return '—';
  const s = Math.max(0, Math.round(Date.now() / 1000 - t));
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.round(s / 60) + 'm ago';
  if (s < 86400) return Math.round(s / 3600) + 'h ago';
  return Math.round(s / 86400) + 'd ago';
}

async function post(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!r.ok) alert((await r.json().catch(() => ({}))).error || ('HTTP ' + r.status));
  tick();
}

// Delegated once, on a node that outlives every re-render — rather than inline onclick
// attributes built by concatenation. The first version of this page did the latter with
// JSON.stringify(), whose DOUBLE quotes closed the attribute early and left every button
// inert. Values ride in data-* and are read back as properties, so quoting never enters
// into it.
document.addEventListener('click', ev => {
  const b = ev.target.closest('button[data-act]');
  if (!b) return;
  const id = b.dataset.id;
  if (b.dataset.act === 'approve') {
    post('/api/approve', { deviceId: id, token: b.dataset.token });
  } else if (b.dataset.act === 'flushcache') {
    post('/api/cache/flush', {});
  } else if (b.dataset.act === 'forget') {
    if (confirm('Forget ' + id + '? It will be refused until you approve it again.'))
      post('/api/forget', { deviceId: id });
  }
});

// Name first, id second: the name is what a human recognises, the id is the
// technical handle the URL and the token are about.
function nameCell(d) {
  return '<div>' + esc(d.name || d.deviceId) + '</div>'
       + '<div class="id">' + esc(d.deviceId) + '</div>';
}

function pendingBlock(pending) {
  if (!pending.length) return '';
  const byId = {};
  pending.forEach(p => { byId[p.device_id] = (byId[p.device_id] || 0) + 1; });
  return '<div class="warn"><h2>Waiting for approval</h2>'
    + '<table><thead><tr><th>Device</th><th>Firmware</th><th>Token</th>'
    + '<th>Tries</th><th>Last seen</th><th></th></tr></thead><tbody>'
    + pending.map(p => '<tr>'
        + '<td>' + nameCell({ name: p.name, deviceId: p.device_id })
        + (byId[p.device_id] > 1
            ? '<div class="id" style="color:#f59e0b">two tokens claim this id</div>' : '')
        + '</td>'
        + '<td><code>' + esc(p.project || '?') + ' ' + esc(p.firmware || '?') + '</code></td>'
        + '<td><code>' + esc((p.token || '').slice(0, 8) || 'none') + '…</code></td>'
        + '<td>' + p.attempts + '</td>'
        + '<td>' + ago(p.last_seen) + '</td>'
        + '<td><button class="primary" data-act="approve" data-id="' + esc(p.device_id)
        + '" data-token="' + esc(p.token) + '">Approve</button></td>'
        + '</tr>').join('')
    + '</tbody></table></div>';
}

function deviceBlock(live, approved) {
  const online = {};
  live.forEach(d => { online[d.deviceId] = d; });
  const rows = approved.map(a => {
    const d = online[a.device_id];
    const shown = d || { deviceId: a.device_id, name: a.name, project: a.project,
                         firmware: a.firmware };
    return '<tr>'
      + '<td><span class="dot' + (d ? '' : ' off') + '"></span></td>'
      + '<td>' + (d ? '<a href="/devices/' + encodeURIComponent(a.device_id) + '/">'
                      + nameCell(shown) + '</a>'
                    : nameCell(shown)) + '</td>'
      + '<td><code>' + esc(shown.project || '?') + ' ' + esc(shown.firmware || '?')
      + '</code></td>'
      + '<td>' + (d ? d.uptimeSeconds + 's' : 'last seen ' + ago(a.last_seen)) + '</td>'
      + '<td>' + (d ? d.browsers : '—') + '</td>'
      + '<td><button data-act="forget" data-id="' + esc(a.device_id)
      + '">Forget</button></td>'
      + '</tr>';
  }).join('');

  if (!rows) {
    return '<p class="empty">No devices paired yet. Set <code>relay.enabled</code> and '
         + '<code>relay.url</code> on a device; it will appear above for approval.</p>';
  }
  return '<h2>Devices</h2><table><thead><tr><th></th><th>Device</th>'
    + '<th>Firmware</th><th>Connected</th><th>Viewers</th><th></th>'
    + '</tr></thead><tbody>' + rows + '</tbody></table>';
}

function eventBlock(events) {
  if (!events.length) return '';
  return '<h2>Recent</h2><div class="ev">'
    + events.map(e => ago(e.at) + ' — ' + esc(e.kind) + ' ' + esc(e.device_id || '')
        + (e.detail ? ' (' + esc(e.detail) + ')' : '')).join('<br>')
    + '</div>';
}

function cacheBlock(c) {
  if (!c) return '';
  const total = c.hits + c.misses;
  const rate = total ? Math.round((c.hits / total) * 100) + '%' : '—';
  return '<h2>File cache</h2><div class="ev">' + c.entries + ' files, '
       + Math.round(c.bytes / 1024) + ' kB — ' + c.hits + ' hits / ' + c.misses
       + ' fetched from devices (' + rate + ') '
       + '<button data-act="flushcache">Flush</button></div>';
}

function telemetryBlock(t) {
  if (!t) return '';
  if (!t.enabled) return '<h2>Telemetry</h2><div class="ev">No Influx configured — '
                        + 'device points are dropped.</div>';
  return '<h2>Telemetry</h2><div class="ev">' + t.written + ' points written'
       + (t.queued ? ', ' + t.queued + ' queued' : '')
       + (t.dropped ? ', <b>' + t.dropped + ' dropped</b>' : '')
       + '</div>';
}

async function tick() {
  let live = [], pair = { pending: [], approved: [], events: [] };
  try {
    [live, pair] = await Promise.all([
      (await fetch('/api/devices')).json(),
      (await fetch('/api/pairing')).json(),
    ]);
  } catch (e) { return; }
  document.getElementById('out').innerHTML =
      pendingBlock(pair.pending) + deviceBlock(live, pair.approved)
    + cacheBlock(pair.cache) + telemetryBlock(pair.telemetry) + eventBlock(pair.events);
}
tick(); setInterval(tick, 2000);
</script>
</body></html>
"""


async def dashboard(request: web.Request) -> web.StreamResponse:
    return web.Response(text=DASHBOARD, content_type="text/html")


async def healthz(request: web.Request) -> web.StreamResponse:
    """Liveness only — a connected device is not a health condition.

    The container healthcheck talks to this with Python's stdlib, because
    python:*-slim ships neither curl nor wget.
    """
    return web.Response(text="ok\n", content_type="text/plain")


def build_app() -> web.Application:
    app = web.Application()
    # Order matters: the device WS route and /devices/<id>/ws must both be
    # matched before the frontend catch-all.
    app.router.add_get("/", dashboard)
    app.router.add_get("/healthz", healthz)
    app.router.add_get("/api/devices", api_devices)
    app.router.add_get("/api/pairing", api_pairing)
    app.router.add_post("/api/approve", api_approve)
    app.router.add_post("/api/forget", api_forget)
    app.router.add_post("/api/cache/flush", api_cache_flush)
    app.router.add_get("/device", device_ws)
    app.router.add_get("/devices/{deviceId}/ws", browser_ws)
    app.router.add_get("/devices/{deviceId}", device_redirect)
    app.router.add_get("/devices/{deviceId}/", device_frontend)
    app.router.add_get("/devices/{deviceId}/{path:.*}", device_frontend)

    # The Influx client needs a running loop, so it starts with the app rather than
    # in main().
    async def _start(_app):
        assert influx is not None
        await influx.start()

    async def _stop(_app):
        assert influx is not None
        await influx.stop()

    app.on_startup.append(_start)
    app.on_cleanup.append(_stop)
    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="Minimal Strux relay server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    # In the container this is the bind-mounted runtime volume, so approvals survive
    # a restart or an image update. Defaults to the cwd for running it off a checkout.
    parser.add_argument("--db", default="relay.db")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    global store, influx
    store = Store(args.db)

    # Env rather than flags: these are deployment facts and one of them is a secret,
    # so they come from the compose file alongside every other secret on this host.
    influx = InfluxWriter(
        url=os.environ.get("INFLUX_URL", ""),
        token=os.environ.get("INFLUX_TOKEN", ""),
        org=os.environ.get("INFLUX_ORG", ""),
        bucket=os.environ.get("INFLUX_BUCKET", ""),
    )

    log.info("relay listening on http://%s:%d  (device endpoint: /device)",
             args.host, args.port)
    web.run_app(build_app(), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
