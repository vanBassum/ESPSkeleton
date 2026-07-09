// Singleton backend service — all communication over a single WebSocket.

import { DEV_HOST } from "@/config"

const TOKEN_KEY = "device.token"

// ── Types ────────────────────────────────────────────────────────

type BroadcastHandler = (msg: Record<string, unknown>) => void
type BinaryHandler = (data: ArrayBuffer) => void

interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
  chunks: Uint8Array[]
  // When set, each non-final reply chunk is parsed as its own JSON message and
  // passed here (e.g. upload progress); the final chunk resolves the request.
  // Absent → default behaviour: accumulate all chunks and parse once at FINAL.
  onMessage?: (msg: Record<string, unknown>) => void
}

export type ConnectionStatus = "connected" | "connecting" | "disconnected"
type StatusHandler = (status: ConnectionStatus) => void
type AuthHandler = (authenticated: boolean) => void

// ── Service ──────────────────────────────────────────────────────

// Session ids correlate a reply with its request. The device is single-in-flight
// (one active session at a time), so opens are SERIALIZED through a FIFO queue
// (see `enqueue`): callers still fire concurrently, but only one session is open
// on the wire at once, and the next starts on the previous reply's FINAL. Ids
// stay within 16 bits to match the wire.
let nextSession = 1

const FLAG_FINAL = 0x01
const FLAG_REJECT = 0x02

class BackendService {
  private ws: WebSocket | null = null
  private pending = new Map<number, PendingRequest>()
  private broadcastHandlers = new Set<BroadcastHandler>()
  private binaryHandlers = new Set<BinaryHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  private connecting: Promise<void> | null = null
  // Tail of the open-serialization queue. Each enqueued task runs only after the
  // previous one has fully settled (its reply's FINAL received, or it failed).
  private queue: Promise<unknown> = Promise.resolve()
  private _status: ConnectionStatus = "disconnected"
  private token: string | null = sessionStorage.getItem(TOKEN_KEY)
  private authHandlers = new Set<AuthHandler>()
  private _authenticated = false

  get status(): ConnectionStatus {
    return this._status
  }

  get authenticated(): boolean {
    return this._authenticated
  }

  get hasToken(): boolean {
    return this.token !== null
  }

  onAuthChange(fn: AuthHandler): () => void {
    this.authHandlers.add(fn)
    return () => {
      this.authHandlers.delete(fn)
    }
  }

  private setAuthenticated(auth: boolean) {
    if (auth !== this._authenticated) {
      this._authenticated = auth
      this.authHandlers.forEach((fn) => fn(auth))
    }
  }

  private clearAuth() {
    this.token = null
    sessionStorage.removeItem(TOKEN_KEY)
    this.setAuthenticated(false)
    this.ws?.close()
  }

  private authHeaders(): Record<string, string> {
    return this.token ? { Authorization: `Bearer ${this.token}` } : {}
  }

  private apiUrl(path: string): string {
    const host = import.meta.env.DEV ? `http://${DEV_HOST}` : ""
    return `${host}${path}`
  }

  private setStatus(s: ConnectionStatus) {
    if (s !== this._status) {
      this._status = s
      this.statusHandlers.forEach((fn) => fn(s))
    }
  }

  onStatusChange(fn: StatusHandler): () => void {
    this.statusHandlers.add(fn)
    return () => {
      this.statusHandlers.delete(fn)
    }
  }

  connect() {
    this.ensureConnected().catch(() => {})
  }

  private ensureConnected(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return Promise.resolve()
    if (this.connecting) return this.connecting
    return this.doConnect()
  }

  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }

    if (!this.token) {
      this.setStatus("disconnected")
      return Promise.reject(new Error("Not authenticated"))
    }

    // Pin the token this attempt is validating. A newer login can replace
    // `this.token` while this attempt is still in flight; this attempt must
    // keep judging its own (possibly stale) token and never act on the
    // current one, so it can't clobber a fresher, already-successful login.
    const attemptToken = this.token

    this.setStatus("connecting")

    const p = (async () => {
      // Validate the token over HTTP first: the browser WS API cannot
      // distinguish a refused upgrade (bad token) from a network
      // failure, and we must not clear a good token on a flaky link.
      try {
        const res = await fetch(this.commandUrl("ping"), {
          method: "POST",
          headers: { Authorization: `Bearer ${attemptToken}` },
          body: "{}",
        })
        if (res.status === 401) {
          if (this.token === attemptToken) {
            this.clearAuth()
          } else if (this.token) {
            // Token replaced mid-attempt (fresh login) — hand off to it.
            this.reconnectTimer = setTimeout(() => {
              this.doConnect().catch(() => {})
            }, 2000)
          }
          this.setStatus("disconnected")
          throw new Error("Not authenticated")
        }
      } catch (e) {
        if (e instanceof Error && e.message === "Not authenticated") throw e
        // Network error: fall through — the WS attempt below owns retries.
      }

      await new Promise<void>((resolve, reject) => {
        const host = import.meta.env.DEV ? DEV_HOST : location.host
        const proto = location.protocol === "https:" ? "wss:" : "ws:"
        const url = `${proto}//${host}/ws?token=${attemptToken}`
        console.log(`[BackendService] connecting to ${proto}//${host}/ws (DEV=${import.meta.env.DEV})`)
        const ws = new WebSocket(url)
        ws.binaryType = "arraybuffer"
        let opened = false

        ws.onopen = () => {
          opened = true
          this.ws = ws
          this.setStatus("connected")
          this.setAuthenticated(true)
          this.startHeartbeat()
          resolve()
        }

        ws.onmessage = (ev) => {
          // Binary frames are session chunks (command replies).
          if (ev.data instanceof ArrayBuffer) {
            this.onBinaryChunk(ev.data)
            return
          }
          // Defensive: some browsers may deliver a frame as a Blob if
          // binaryType wasn't applied in time. Convert and route the same way.
          if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
            ev.data.arrayBuffer().then((buf) => this.onBinaryChunk(buf))
            return
          }
          // Text frames are broadcasts only (replies are binary now).
          try {
            this.broadcastHandlers.forEach((fn) => fn(JSON.parse(ev.data)))
          } catch (e) {
            const sample = typeof ev.data === "string" ? ev.data.slice(-80) : "(non-string)"
            console.warn(
              `[BackendService] failed to parse WS text frame (${typeof ev.data === "string" ? ev.data.length : "?"} bytes); tail: ${sample}`,
              e,
            )
          }
        }

        ws.onclose = () => {
          this.ws = null
          this.stopHeartbeat()
          this.setStatus("disconnected")
          for (const [, req] of this.pending) {
            clearTimeout(req.timer)
            req.reject(new Error("WebSocket closed"))
          }
          this.pending.clear()
          if (!opened) reject(new Error("Connection failed"))
          if (this.token) {
            this.reconnectTimer = setTimeout(() => {
              this.doConnect().catch(() => {})
            }, 2000)
          }
        }

        ws.onerror = () => ws.close()
      })
    })()

    this.connecting = p
    p.catch(() => {}).then(() => {
      if (this.connecting === p) this.connecting = null
    })
    return p
  }

  private startHeartbeat() {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => {
      if (this.ws?.readyState !== WebSocket.OPEN) return
      this.send("ping").catch(() => {
        this.setStatus("disconnected")
        this.ws?.close()
      })
    }, 15000)
  }

  private stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
  }

  // Run `task` after every previously-enqueued task has settled — the
  // open-serialization queue. A task's failure never stalls the queue.
  private enqueue<T>(task: () => Promise<T>): Promise<T> {
    const run = this.queue.then(task, task)
    this.queue = run.then(
      () => {},
      () => {},
    )
    return run
  }

  private allocSession(): number {
    const session = nextSession
    nextSession = nextSession >= 0xffff ? 1 : nextSession + 1
    return session
  }

  // Send one session chunk: [session:u16 LE | flags | payload].
  private sendChunk(session: number, flags: number, payload: Uint8Array) {
    const frame = new Uint8Array(3 + payload.length)
    frame[0] = session & 0xff
    frame[1] = (session >> 8) & 0xff
    frame[2] = flags
    frame.set(payload, 3)
    this.ws!.send(frame)
  }

  // Register a pending reply for `session`; resolves when its FINAL chunk
  // arrives (reassembled in onBinaryChunk), rejects on REJECT/timeout/close.
  // `onMessage`, if given, receives each intermediate chunk parsed as JSON.
  private awaitReply<T>(
    session: number,
    timeoutMs = 10000,
    onMessage?: (msg: Record<string, unknown>) => void,
  ): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(session)
        reject(new Error("Request timeout"))
      }, timeoutMs)
      this.pending.set(session, {
        resolve: resolve as (data: unknown) => void,
        reject,
        timer,
        chunks: [],
        onMessage,
      })
    })
  }

  async send<T>(
    type: string,
    params: Record<string, unknown> = {},
  ): Promise<T> {
    return this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<T>(session)
      // Request = one FINAL session chunk: the command JSON + '\n' (the device
      // splits the header line from any body; these commands have no body).
      const body = new TextEncoder().encode(JSON.stringify({ type, ...params }) + "\n")
      this.sendChunk(session, FLAG_FINAL, body)
      return reply
    })
  }

  // Reassemble a reply from its session chunks. Each chunk is
  // [session:u16 LE | flags | payload]; FLAG_FINAL ends the reply, FLAG_REJECT
  // is a transport/framework refusal whose payload is the reason.
  private onBinaryChunk(data: ArrayBuffer) {
    const view = new Uint8Array(data)
    if (view.length < 3) return
    const session = view[0] | (view[1] << 8)
    const flags = view[2]

    // Session 0 is reserved for device-initiated broadcasts (log lines).
    if (session === 0) {
      try {
        this.broadcastHandlers.forEach((fn) => fn(JSON.parse(new TextDecoder().decode(view.subarray(3)))))
      } catch {
        /* malformed broadcast — ignore */
      }
      return
    }

    const req = this.pending.get(session)
    if (!req) {
      // No matching request — hand to legacy binary subscribers (unused here).
      this.binaryHandlers.forEach((fn) => fn(data))
      return
    }
    if (flags & FLAG_REJECT) {
      this.pending.delete(session)
      clearTimeout(req.timer)
      req.reject(new Error(new TextDecoder().decode(view.subarray(3)) || "rejected"))
      return
    }

    // Streaming reply: each chunk is one complete JSON message. Intermediate
    // chunks go to onMessage; the FINAL chunk resolves the request.
    if (req.onMessage) {
      const text = new TextDecoder().decode(view.subarray(3))
      if (flags & FLAG_FINAL) {
        this.pending.delete(session)
        clearTimeout(req.timer)
        try {
          req.resolve(text.length ? JSON.parse(text) : {})
        } catch (e) {
          req.reject(e instanceof Error ? e : new Error("bad reply"))
        }
      } else if (text.length) {
        try {
          req.onMessage(JSON.parse(text))
        } catch {
          /* ignore a malformed progress message */
        }
      }
      return
    }

    req.chunks.push(view.subarray(3))
    if (flags & FLAG_FINAL) {
      this.pending.delete(session)
      clearTimeout(req.timer)
      const total = req.chunks.reduce((a, c) => a + c.length, 0)
      const buf = new Uint8Array(total)
      let off = 0
      for (const c of req.chunks) {
        buf.set(c, off)
        off += c.length
      }
      const text = new TextDecoder().decode(buf)
      try {
        req.resolve(text.length ? JSON.parse(text) : {})
      } catch (e) {
        req.reject(e instanceof Error ? e : new Error("bad reply"))
      }
    }
  }

  subscribe(fn: BroadcastHandler): () => void {
    this.broadcastHandlers.add(fn)
    this.ensureConnected().catch(() => {})
    return () => {
      this.broadcastHandlers.delete(fn)
    }
  }

  subscribeBinary(fn: BinaryHandler): () => void {
    this.binaryHandlers.add(fn)
    this.ensureConnected().catch(() => {})
    return () => {
      this.binaryHandlers.delete(fn)
    }
  }

  // ── API methods ──────────────────────────────────────────────

  async getInfo(): Promise<DeviceInfo> {
    return this.send<DeviceInfo>("info")
  }

  async getLogs(): Promise<LogsResponse> {
    return this.send<LogsResponse>("getLogs")
  }

  async getUpdateStatus(): Promise<UpdateStatus> {
    return this.send<UpdateStatus>("updateStatus")
  }

  async getSettings(): Promise<SettingsResponse> {
    return this.send<SettingsResponse>("getSettings")
  }

  async setSetting(key: string, value: string): Promise<{ ok: boolean }> {
    return this.send("setSetting", { key, value })
  }

  async saveSettings(): Promise<{ ok: boolean }> {
    return this.send("saveSettings")
  }

  async wifiScan(): Promise<WifiScanResponse> {
    return this.send<WifiScanResponse>("wifiScan")
  }

  async getPartitions(): Promise<PartitionsResponse> {
    return this.send<PartitionsResponse>("partitions")
  }

  async reboot(): Promise<{ ok: boolean }> {
    return this.send("reboot")
  }

  private commandUrl(type: string): string {
    return this.apiUrl(`/api/command?type=${encodeURIComponent(type)}`)
  }

  /** Open endpoint: device name for the login page's brand slot. */
  async getLoginInfo(): Promise<{ name: string }> {
    const res = await fetch(this.apiUrl("/api/login"))
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
    return res.json()
  }

  /** Returns false on wrong password; throws on network failure. */
  async login(password: string): Promise<boolean> {
    const res = await fetch(this.apiUrl("/api/login"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password }),
    })
    if (res.status === 401) return false
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
    const { token } = (await res.json()) as { token: string }
    this.token = token
    sessionStorage.setItem(TOKEN_KEY, token)
    this.setAuthenticated(true)
    this.connect()
    return true
  }

  /** Upload a .bin as one streamed `writePartition` session: an envelope chunk
   *  ({"type":"writePartition","partition":...}\n) followed by body chunks, the
   *  last carrying FLAG_FINAL. The device drains it straight to flash and replies
   *  once, at end-of-stream. Runs through the open queue, so nothing else touches
   *  the socket mid-upload (the device would REJECT an interleaved session id). */
  async uploadPartition(
    partition: string,
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    return this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const total = file.size

      // Progress is DEVICE-driven: the handler streams {"p":<bytesWritten>}
      // messages as it flashes, and we map those to a percentage. Client-side
      // "bytes sent" can't see the device's write position (the OS buffers the
      // socket), so it would race to 100% while the write is still in flight.
      const reply = this.awaitReply<{ ok: boolean; size?: number; error?: string }>(
        session,
        120000,
        (msg) => {
          if (total && typeof msg.p === "number") {
            onProgress?.(Math.min(100, Math.round((msg.p / total) * 100)))
          }
        },
      )

      // Envelope chunk (not FINAL — the body follows on the same session id).
      const envelope = new TextEncoder().encode(JSON.stringify({ type: "writePartition", partition }) + "\n")
      this.sendChunk(session, 0, envelope)

      // Body chunks. CHUNK matches the device's inbound window (see WebSocketHandler).
      const CHUNK = 4096
      let sent = 0
      while (sent < total) {
        const end = Math.min(sent + CHUNK, total)
        const slice = new Uint8Array(await file.slice(sent, end).arrayBuffer())
        const isLast = end >= total
        await this.drainBuffer()
        this.sendChunk(session, isLast ? FLAG_FINAL : 0, slice)
        sent = end
      }
      // A zero-length file still needs a FINAL to close the request direction.
      if (total === 0) this.sendChunk(session, FLAG_FINAL, new Uint8Array(0))

      const res = await reply
      if (!res.ok) throw new Error(res.error ?? "writePartition failed")
      onProgress?.(100)
      return { ok: true, size: res.size ?? sent }
    })
  }

  // Backpressure: don't let the browser-side WS buffer outrun the socket.
  private async drainBuffer(limit = 64 * 1024) {
    while (this.ws && this.ws.bufferedAmount > limit) {
      await new Promise((r) => setTimeout(r, 20))
    }
  }

  /** Fetch a partition image through the command pipe and save it as <label>.bin.
   *  The response is chunked (no Content-Length), so progress is computed
   *  against expectedSize — the UI knows it from the partition table. */
  async downloadPartitionFile(
    label: string,
    expectedSize?: number,
    onProgress?: (percent: number) => void,
  ): Promise<void> {
    // Same single-threaded-server precaution as uploads: don't let our
    // heartbeat race a server that is busy streaming to us.
    this.stopHeartbeat()
    try {
      const res = await fetch(this.commandUrl("downloadPartition"), {
        method: "POST",
        headers: this.authHeaders(),
        body: JSON.stringify({ partition: label }),
      })
      if (res.status === 401) {
        this.clearAuth()
        throw new Error("Not authenticated")
      }
      if (!res.ok || !res.body) throw new Error(`${res.status} ${res.statusText}`)

      const reader = res.body.getReader()
      const chunks: BlobPart[] = []
      let received = 0
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        chunks.push(value)
        received += value.length
        if (expectedSize) onProgress?.(Math.min(100, Math.round((received / expectedSize) * 100)))
      }

      const blob = new Blob(chunks)
      const text = blob.size < 256 ? await blob.slice(0, 256).text() : ""
      if (text.startsWith('{"ok":false')) throw new Error(JSON.parse(text).error ?? "download failed")

      const url = URL.createObjectURL(blob)
      const a = document.createElement("a")
      a.href = url
      a.download = `${label}.bin`
      a.click()
      URL.revokeObjectURL(url)
    } finally {
      this.startHeartbeat()
    }
  }

}

const instance = new BackendService()
instance.connect()
export const backend = instance

// ── Types ────────────────────────────────────────────────────────

export interface DeviceInfo {
  name: string
  project: string
  firmware: string
  idf: string
  date: string
  time: string
  chip: string
  heapFree: number
  heapMin: number
  deviceTime: string
}

export interface UpdateStatus {
  firmware: string
  running: string
  nextSlot: string
}

export interface UploadResult {
  ok: boolean
  size: number
}

export type SettingType = "string" | "int32" | "uint32" | "float" | "bool"

export const NUMERIC_SETTING_TYPES: SettingType[] = ["int32", "uint32", "float"]

export interface SettingEntry {
  key: string
  label: string
  type: SettingType
  value: string | number | boolean
}

export interface SettingsResponse {
  settings: SettingEntry[]
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  channel: number
  secure: boolean
}

export interface WifiScanResponse {
  ok: boolean
  networks: WifiNetwork[]
}

export interface LogsResponse {
  lines: string[]
}

export interface Partition {
  label: string
  type: string
  subtype: string
  offset: number
  size: number
  running: boolean
  nextOta: boolean
  uploadable: boolean
  version: string
}

export interface PartitionsResponse {
  partitions: Partition[]
}

