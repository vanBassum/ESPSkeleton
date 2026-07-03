// Singleton backend service — all communication over a single WebSocket.

const DEV_HOST = "strux.local"

// ── Types ────────────────────────────────────────────────────────

type BroadcastHandler = (msg: Record<string, unknown>) => void
type BinaryHandler = (data: ArrayBuffer) => void

interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
}

export type ConnectionStatus = "connected" | "connecting" | "disconnected"
type StatusHandler = (status: ConnectionStatus) => void
type AuthHandler = (authenticated: boolean) => void

// ── Service ──────────────────────────────────────────────────────

let nextId = 1

class BackendService {
  private ws: WebSocket | null = null
  private pending = new Map<number, PendingRequest>()
  private broadcastHandlers = new Set<BroadcastHandler>()
  private binaryHandlers = new Set<BinaryHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  private connecting: Promise<void> | null = null
  private _status: ConnectionStatus = "disconnected"
  private token: string | null = sessionStorage.getItem("strux.token")
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
    sessionStorage.removeItem("strux.token")
    this.setAuthenticated(false)
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

    this.setStatus("connecting")

    const p = (async () => {
      // Validate the token over HTTP first: the browser WS API cannot
      // distinguish a refused upgrade (bad token) from a network
      // failure, and we must not clear a good token on a flaky link.
      try {
        const res = await fetch(this.commandUrl("ping"), {
          method: "POST",
          headers: this.authHeaders(),
          body: "{}",
        })
        if (res.status === 401) {
          this.clearAuth()
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
        const url = `${proto}//${host}/ws?token=${this.token}`
        console.log(`[BackendService] connecting to ${url} (DEV=${import.meta.env.DEV})`)
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
          // Binary frames are dispatched to binary subscribers.
          if (ev.data instanceof ArrayBuffer) {
            this.binaryHandlers.forEach((fn) => fn(ev.data))
            return
          }
          // Defensive: some browsers may deliver the first frame as a Blob if
          // binaryType wasn't applied in time. Convert and dispatch.
          if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
            ev.data.arrayBuffer().then((buf) => {
              this.binaryHandlers.forEach((fn) => fn(buf))
            })
            return
          }
          try {
            const msg = JSON.parse(ev.data)
            if (typeof msg.id === "number") {
              const req = this.pending.get(msg.id)
              if (req) {
                this.pending.delete(msg.id)
                clearTimeout(req.timer)
                if (msg.error) {
                  req.reject(new Error(msg.error))
                } else {
                  req.resolve(msg.payload)
                }
              }
            } else {
              this.broadcastHandlers.forEach((fn) => fn(msg))
            }
          } catch (e) {
            const sample = typeof ev.data === "string" ? ev.data.slice(-80) : "(non-string)"
            console.warn(
              `[BackendService] failed to parse WS frame (${typeof ev.data === "string" ? ev.data.length : "?"} bytes); tail: ${sample}`,
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

  async send<T>(
    type: string,
    params: Record<string, unknown> = {},
  ): Promise<T> {
    await this.ensureConnected()
    const id = nextId++
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error("Request timeout"))
      }, 10000)
      this.pending.set(id, {
        resolve: resolve as (data: unknown) => void,
        reject,
        timer,
      })
      this.ws!.send(JSON.stringify({ id, type, ...params }))
    })
  }

  subscribe(fn: BroadcastHandler): () => void {
    this.broadcastHandlers.add(fn)
    this.ensureConnected()
    return () => {
      this.broadcastHandlers.delete(fn)
    }
  }

  subscribeBinary(fn: BinaryHandler): () => void {
    this.binaryHandlers.add(fn)
    this.ensureConnected()
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
    sessionStorage.setItem("strux.token", token)
    this.setAuthenticated(true)
    this.connect()
    return true
  }

  /** Upload a .bin into an update session: begin (WS) → write (HTTP, streamed) → end (WS). */
  async uploadPartition(
    partition: string,
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    const begin = await this.send<{ ok: boolean; error?: string }>("updateBegin", { partition })
    if (!begin.ok) throw new Error(begin.error ?? "updateBegin failed")

    // The device's HTTP server is single-threaded: during one long
    // request it can serve NOBODY else, starved clients reconnect, and
    // the server's LRU purge then evicts the quietest socket — our own
    // WebSocket (verified on hardware; this is not just the heartbeat).
    // Chunking lets the server breathe between requests. The heartbeat
    // pause is a second belt so our watchdog can't misfire either.
    this.stopHeartbeat()
    try {
      const CHUNK = 256 * 1024
      let sent = 0
      let total = 0
      while (sent < file.size) {
        const slice = file.slice(sent, sent + CHUNK)
        const write = await this.postCommand("updateWrite", slice, (pct) => {
          onProgress?.(Math.round(((sent + (slice.size * pct) / 100) / file.size) * 100))
        })
        if (!write.ok) throw new Error(write.error ?? "updateWrite failed")
        total += write.size ?? slice.size
        sent += slice.size
        onProgress?.(Math.round((sent / file.size) * 100))
      }

      const end = await this.send<{ ok: boolean; error?: string }>("updateEnd")
      if (!end.ok) throw new Error(end.error ?? "updateEnd failed")

      return { ok: true, size: total }
    } catch (e) {
      // Best effort: close a dangling session so the next attempt isn't "busy".
      this.send("updateEnd").catch(() => {})
      throw e
    } finally {
      this.startHeartbeat()
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

  private postCommand(
    type: string,
    body: Blob,
    onProgress?: (percent: number) => void,
  ): Promise<{ ok: boolean; size?: number; error?: string }> {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest()
      xhr.open("POST", this.commandUrl(type))
      if (this.token) xhr.setRequestHeader("Authorization", `Bearer ${this.token}`)

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) {
          onProgress(Math.round((e.loaded / e.total) * 100))
        }
      }

      xhr.onload = () => {
        if (xhr.status === 401) {
          this.clearAuth()
          reject(new Error("Not authenticated"))
        } else if (xhr.status >= 200 && xhr.status < 300) {
          resolve(JSON.parse(xhr.responseText))
        } else {
          reject(new Error(xhr.responseText || `${xhr.status} ${xhr.statusText}`))
        }
      }

      xhr.onerror = () => reject(new Error("Upload failed"))
      xhr.ontimeout = () => reject(new Error("Upload timed out"))
      xhr.timeout = 120000

      xhr.send(body)
    })
  }
}

const instance = new BackendService()
instance.connect()
export const backend = instance

// ── Types ────────────────────────────────────────────────────────

export interface DeviceInfo {
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

