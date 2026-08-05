#!/usr/bin/env python3
"""Minimal relay server for Strux devices.

Proves the remote-access path over a LAN: devices dial *out* to this server, and a
browser reaches a device's own web UI through it.

    device  ──ws──►  /device?id=<id>&fw=<ver>     (outbound, NAT-friendly)
    browser ──http─►  /devices/<id>/{path}        → `web read` command on the device
    browser ──ws──►  /devices/<id>/ws             → relayed to the device pipe

What this server understands and does not:

  * It parses and REWRITES the 3-byte session header — it must, because it owns
    the session-id space on the device pipe (see below).
  * It never parses a session payload. Commands, auth, uploads and log lines are
    opaque bytes it moves between two sockets.

Deliberately out of scope (see the design doc): TLS, authentication of devices to
this server, user accounts, persistence, and file caching.

Design: docs/superpowers/specs/2026-08-02-remote-access-relay-design.md
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
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

    def __init__(self, device_id: str, firmware: str, ws: web.WebSocketResponse):
        self.device_id = device_id
        self.firmware = firmware
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


# ── Registry ──────────────────────────────────────────────────────────────────

devices: dict[str, DeviceConnection] = {}


# ── Endpoints ─────────────────────────────────────────────────────────────────

async def device_ws(request: web.Request) -> web.StreamResponse:
    """The device's outbound pipe. Registration is the query string — no protocol verb."""
    device_id = request.query.get("id")
    firmware = request.query.get("fw", "unknown")
    if not device_id:
        return web.Response(status=400, text="missing ?id=")

    ws = web.WebSocketResponse(heartbeat=30.0)
    await ws.prepare(request)

    conn = DeviceConnection(device_id, firmware, ws)
    previous = devices.get(device_id)
    if previous is not None:
        log.info("device %s reconnected, dropping the previous pipe", device_id)
        await previous.close()
    devices[device_id] = conn
    log.info("device %s connected (fw %s) from %s", device_id, firmware,
             request.remote)

    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                await conn.on_device_chunk(msg.data)
            elif msg.type == WSMsgType.ERROR:
                log.warning("device %s socket error: %s", device_id, ws.exception())
                break
    finally:
        if devices.get(device_id) is conn:
            devices.pop(device_id, None)
        await conn.close()
        log.info("device %s disconnected", device_id)

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
            "firmware": conn.firmware,
            "online": not conn.ws.closed,
            "uptimeSeconds": int(now - conn.connected_at),
            "browsers": len(conn.browsers),
        }
        for conn in sorted(devices.values(), key=lambda c: c.device_id)
    ])


DASHBOARD = """<!doctype html>
<html><head><meta charset="utf-8"><title>Strux relay</title>
<style>
 :root { color-scheme: light dark; }
 body { font: 15px/1.5 system-ui, sans-serif; margin: 3rem auto; max-width: 46rem; padding: 0 1rem; }
 h1 { font-size: 1.3rem; margin-bottom: .25rem; }
 p.sub { color: #888; margin-top: 0; }
 table { border-collapse: collapse; width: 100%; margin-top: 1.5rem; }
 th, td { text-align: left; padding: .5rem .6rem; border-bottom: 1px solid #8883; }
 th { font-weight: 600; font-size: .8rem; text-transform: uppercase; letter-spacing: .04em; color: #888; }
 .dot { display: inline-block; width: .55rem; height: .55rem; border-radius: 50%; background: #22c55e; }
 code { font-size: .85em; color: #888; }
 .empty { color: #888; padding: 2rem 0; }
</style></head>
<body>
 <h1>Strux relay</h1>
 <p class="sub">Devices dial out to this server. Click one to open its own web UI.</p>
 <div id="out"><p class="empty">Loading…</p></div>
<script>
async function tick() {
  let list = [];
  try { list = await (await fetch('/api/devices')).json(); } catch (e) {}
  const out = document.getElementById('out');
  if (!list.length) {
    out.innerHTML = '<p class="empty">No devices connected. Set <code>relay.enabled</code> and '
                  + '<code>relay.url</code> in a device\\'s settings.</p>';
    return;
  }
  out.innerHTML = '<table><thead><tr><th></th><th>Device</th><th>Firmware</th>'
    + '<th>Connected</th><th>Viewers</th></tr></thead><tbody>'
    + list.map(d => '<tr><td><span class="dot"></span></td>'
        + '<td><a href="/devices/' + encodeURIComponent(d.deviceId) + '/">' + d.deviceId + '</a></td>'
        + '<td><code>' + d.firmware + '</code></td>'
        + '<td>' + d.uptimeSeconds + 's</td>'
        + '<td>' + d.browsers + '</td></tr>').join('')
    + '</tbody></table>';
}
tick(); setInterval(tick, 2000);
</script>
</body></html>
"""


async def dashboard(request: web.Request) -> web.StreamResponse:
    return web.Response(text=DASHBOARD, content_type="text/html")


def build_app() -> web.Application:
    app = web.Application()
    # Order matters: the device WS route and /devices/<id>/ws must both be
    # matched before the frontend catch-all.
    app.router.add_get("/", dashboard)
    app.router.add_get("/api/devices", api_devices)
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
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    log.info("relay listening on http://%s:%d  (device endpoint: /device)",
             args.host, args.port)
    web.run_app(build_app(), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
