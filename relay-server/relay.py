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

Deliberately out of scope for now: file caching, and per-device origin isolation
(every device's UI is served from this one origin).

Open work: docs/backlog/2026-07-03-remote-access.md
Deployment: docs/backlog/2026-08-05-relay-in-production.md
"""

from __future__ import annotations

import argparse
import asyncio
import hmac
import json
import logging
import sqlite3
import struct
import time

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

# Session-id ownership. Both this server and a browser would otherwise allocate
# from 1 on the same device socket and collide — which presents as "the device
# replied to the wrong request". So the server owns the space: browser ids are
# rewritten into the low half, our own web-read sessions come from the high half.
BROWSER_ID_BASE, BROWSER_ID_LIMIT = 1, 0x8000
SERVER_ID_BASE, SERVER_ID_LIMIT = 0x8000, 0x10000

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
            self._remember_pending(device_id, token, name, project, firmware, now)
            self.log_event("refused", device_id, "token mismatch")
            return False, "token mismatch"

        self._remember_pending(device_id, token, name, project, firmware, now)
        self.log_event("refused", device_id, "not approved")
        return False, "device not approved"

    def _remember_pending(self, device_id: str, token: str, name: str,
                          project: str, firmware: str, now: float) -> None:
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
                return
            self.db.execute(
                "INSERT INTO pending (device_id, token, name, project, firmware, "
                "first_seen, last_seen) VALUES (?,?,?,?,?,?,?)",
                (device_id, token, name, project, firmware, now, now))
        self.db.commit()

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
    log.info("device %s connected on pipe #%d (%s fw %s) from %s",
             device_id, conn.pipe, name or "unnamed", firmware, request.remote)

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
    """Serve the device's own frontend, one web read per request (no cache yet)."""
    device_id = request.match_info["deviceId"]
    tail = request.match_info.get("path", "")

    conn = devices.get(device_id)
    if conn is None:
        return web.Response(status=503, text=f"device '{device_id}' is not connected")

    path = "/" + tail if tail else "/index.html"

    try:
        header, body = await conn.get_web_file(path)

        if header.get("status") != 200:
            # SPA fallback is OUR decision — the device answers a real 404. Only
            # fall back for things that look like routes: a mistyped asset must
            # stay a 404 rather than becoming HTML with status 200, which the
            # browser would reject as a MIME error.
            leaf = tail.rsplit("/", 1)[-1]
            if "." in leaf:
                return web.Response(status=404, text=f"{path} not found on {device_id}")
            header, body = await conn.get_web_file("/index.html")
            if header.get("status") != 200:
                return web.Response(status=404, text="no index.html on the device")

    except asyncio.TimeoutError:
        return web.Response(status=504, text=f"device '{device_id}' timed out")
    except RelayError as exc:
        return web.Response(status=502, text=str(exc))

    headers = {}
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
    + eventBlock(pair.events);
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
    app.router.add_get("/device", device_ws)
    app.router.add_get("/devices/{deviceId}/ws", browser_ws)
    app.router.add_get("/devices/{deviceId}", device_redirect)
    app.router.add_get("/devices/{deviceId}/", device_frontend)
    app.router.add_get("/devices/{deviceId}/{path:.*}", device_frontend)
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

    global store
    store = Store(args.db)

    log.info("relay listening on http://%s:%d  (device endpoint: /device)",
             args.host, args.port)
    web.run_app(build_app(), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
